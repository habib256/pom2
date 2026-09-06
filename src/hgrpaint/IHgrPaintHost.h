// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Pom1 Apple 1 Emulator
// Copyright (C) 2000-2026 Verhille Arnaud
//
// HGR Paint portable module — the seam between the emulator-agnostic editor
// (HgrPaintEditor) and a host emulator (POM1 / POM2). The host owns the video
// RAM, the NTSC renderer and the file I/O; HgrPaintEditor owns the canvas,
// tools, undo and clipboard. Mirrors bench/IBenchHost.
//
// To port the HGR Paint editor to a new emulator: copy hgrpaint/ verbatim and
// implement one IHgrPaintHost (poke a byte, render an 8 KB page to RGBA, and the
// three file ops). No HgrPaintEditor change needed.
//
// External deps the host must provide (beyond ImGui + GL):
//   - IconsFontAwesome6.h on the include path (toolbar glyphs).
//   - stb_image.h + stb_image_write.h on the include path, and the STB_IMAGE*
//     _IMPLEMENTATION compiled once in the host (HgrImageDecode.cpp/the PNG
//     export use stb for the image-import + Save-PNG features).

#ifndef HGRPAINT_IHGRPAINT_HOST_H
#define HGRPAINT_IHGRPAINT_HOST_H

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "imgui.h"   // ImTextureID for textureToImTexture()

namespace hgrpaint {

// A ready-made sprite from the host's built-in library (POM1 ships the GEN2 HGR
// SCROLL-O-SPRITES catalogue under dev/lib/gen2/sprites/). `bytes` is row-major
// HGR (hRows rows × wBytes bytes/row, bit 0 = leftmost pixel) — exactly the
// layout hgrsprite::stamp expects, so the sprite editor drops it straight into
// its scratch page. Empty for a host with no such library (the default).
struct DevSprite {
    std::string name;
    int wBytes = 3, hRows = 16;
    std::vector<uint8_t> bytes;      // wBytes*hRows
    // Regime tag for the browser badge. ×1 = mono/artifact master (the bundled
    // catalogue); ×2 = doubled chosen-colour sprite (colour baked in the bytes).
    bool x2 = false;
    std::string colour;              // ×2 hue name (e.g. "Violet"); empty for ×1 / unknown
};
struct DevSpriteCategory {
    std::string name;
    std::vector<DevSprite> sprites;
};

class IHgrPaintHost {
public:
    virtual ~IHgrPaintHost() = default;

    // Poke one byte into the host's video RAM so strokes appear on the live
    // screen in real time. `addr` is an absolute CPU address (page base + offset).
    virtual void pokeByte(uint16_t addr, uint8_t value) = 0;

    // Optional bulk-write batching. Between beginBatch()/endBatch() the host MAY
    // coalesce the intervening pokeByte() writes into a single transaction (one
    // lock + one screen/snapshot update) instead of one per byte. Default = no-op,
    // so a host without batching still works unchanged. The editor brackets bulk
    // edits (fill, clear, paste, undo/redo, image apply) with these; interactive
    // freehand stays unbatched so it still appears live. Calls are not nested.
    virtual void beginBatch() {}
    virtual void endBatch() {}

    // Sync the host's LIVE display to the page/mode the editor is now editing:
    // grMode selects lo-res GR vs HIRES, page2 selects page 2 vs page 1. A POM1/
    // POM2 host flips its graphics soft switches (GRAPHICS + full screen + the
    // page + HIRES/lo-res) so the on-screen card follows the editor's HGR/HGR2/
    // GR/GR2 selector. Default no-op keeps the portable editor + headless/test
    // hosts unaffected (their canvas is rendered from the page bytes regardless).
    virtual void setDisplayMode(bool grMode, bool page2) { (void)grMode; (void)page2; }

    // Render an editor page into a kHiresWidth×kHiresHeight RGBA buffer through the
    // host's real video pipeline — the same renderer its screen uses, so the canvas
    // is pixel-identical to the emulator. `mono` selects a monochrome render for the
    // editor's mono preview; colour otherwise. `grMode=false` renders the bytes as an
    // 8 KB HIRES page ($2000-layout); `grMode=true` renders the first 1 KB as a
    // lo-res (GR) text page ($0400-layout, 40×48 16-colour blocks upscaled to the
    // same 280×192 canvas). `outRgba` holds at least kHiresWidth*kHiresHeight pixels
    // in GL_RGBA / GL_UNSIGNED_BYTE order.
    virtual void renderHgrPage(const uint8_t* page8k, uint32_t* outRgba, bool mono,
                               bool grMode = false) = 0;

    // Load a raw image dump from `path` into video RAM at absolute `baseAddr` (the
    // file's own length is loaded — an 8 KB HIRES page or a 1 KB lo-res page). Save
    // `sizeBytes` of video RAM at `baseAddr` to `path` (kHiresSize for HIRES, 0x400
    // for lo-res GR). Export the supplied RGBA image (w×h, top-down) to a PNG. Each
    // returns false + sets `err` on failure.
    virtual bool loadImage(const std::string& path, uint16_t baseAddr, std::string& err) = 0;
    virtual bool saveImage(const std::string& path, uint16_t baseAddr, int sizeBytes,
                           std::string& err) = 0;
    virtual bool savePng(const std::string& path, const uint32_t* rgba,
                         int w, int h, std::string& err) = 0;

    // Write an arbitrary byte blob the editors produce themselves (raw sprite
    // bytes, a generated .asm listing) through the host's own file-commit
    // policy. The default below is a portable sibling-temp + rename: the
    // previous contents of `path` survive a failed write, and the caller is
    // TOLD about it — a plain fopen("wb") truncated the user's file in place
    // and then reported "Saved" whatever fwrite/fclose returned (round-2 P2).
    // POM2 overrides it with its durable commit (fsync, then rename).
    virtual bool saveBytes(const std::string& path, const void* data,
                           std::size_t size, std::string& err)
    {
        namespace fs = std::filesystem;
        const fs::path target(path), tmp(path + ".hgrtmp");
        std::error_code ec;
        fs::remove(tmp, ec);            // our own name; clear any leftover
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { err = "cannot create " + tmp.string(); return false; }
            if (size) f.write(static_cast<const char*>(data),
                              static_cast<std::streamsize>(size));
            f.close();
            if (!f) {
                err = "write failed (disk full?): " + tmp.string();
                fs::remove(tmp, ec);
                return false;
            }
        }
        fs::rename(tmp, target, ec);
        if (ec) {
            err = "cannot replace " + path + ": " + ec.message();
            std::error_code rm;
            fs::remove(tmp, rm);
            return false;
        }
        return true;
    }

    // Native OS file picker. Returns true and writes the chosen absolute path
    // into `outPath` when the host can show a native dialog (a desktop POM1/POM2
    // host wired to its OS picker); false -> the editor falls back to its own
    // built-in ImGui file browser (the default below, also taken on WASM or a
    // Linux box without zenity/kdialog). `extCsv` is a comma-separated extension
    // list WITHOUT dots, e.g. "png,jpg,bmp" or "hgr"; empty matches everything.
    // The default keeps the portable editor self-contained — it never names a
    // native dialog itself.
    virtual bool pickFilePath(bool forSave,
                              const std::string& title,
                              const std::string& filterDesc,
                              const std::string& extCsv,
                              const std::string& defaultDir,
                              const std::string& defaultName,
                              std::string& outPath)
    {
        (void)forSave; (void)title; (void)filterDesc; (void)extCsv;
        (void)defaultDir; (void)defaultName; (void)outPath;
        return false;
    }

    // True when the host can pop an OS-native picker right now. Lets the editor
    // tell the two false-returns of pickFilePath apart: when this is true a false
    // return means the user CANCELLED, so the editor must NOT fall back to the
    // built-in ImGui browser (jarring to switch UIs mid-pick). When this is false
    // (default: WASM, or Linux without zenity/kdialog) pickFilePath is never
    // available and the ImGui browser is the only path.
    virtual bool nativeFilePickerAvailable() const { return false; }

    // Initial directory the file browser (native picker AND the ImGui fallback)
    // should open in on first use. Empty (the default) = the portable editor
    // falls back to the process CWD, keeping it standalone for POM2. POM1's host
    // returns the writable sdcard/HGR/ folder so painted pages round-trip with
    // the microSD SD CARD OS. Mirrors IBenchHost::browseDir().
    virtual std::string browseDir() const { return {}; }

    // The host's built-in HGR sprite library, grouped by category, or empty when
    // the host ships none (the default). POM1 parses dev/lib/gen2/sprites/*.asm;
    // the sprite editor shows a browser so you can drop a ready-made sprite into
    // the canvas. Called rarely (the editor caches the result), so a host MAY
    // parse files on demand here.
    virtual std::vector<DevSpriteCategory> devSprites() { return {}; }

    // Texture lifecycle — the HOST owns the graphics backend, so the portable
    // editor never names GL/GLFW/SDL/Metal. The editor hands over RGBA8 (w*h,
    // top-down) and draws the returned opaque handle via
    //   ImGui::Image(host->textureToImTexture(handle), ...);
    // so the host can translate the handle into whatever ImTextureID its
    // graphics backend expects (GLuint for OpenGL, id<MTLTexture> pointer for
    // Metal, etc. — see PomRenderer::asImTextureID).
    //
    // uploadTexture: pass tex==nullptr to create; reuse the returned handle
    // for follow-up uploads. The host MAY destroy + recreate internally when
    // the dimensions change (matches the historical glTexImage2D semantics).
    // `linear` selects bilinear filtering vs crisp nearest. Default no-op
    // impls let a headless/test host link without any graphics backend.
    virtual void* uploadTexture(void* tex, const void* rgba,
                                int w, int h, bool linear)
    { (void)tex; (void)rgba; (void)w; (void)h; (void)linear; return nullptr; }
    virtual void  destroyTexture(void* tex) { (void)tex; }

    // Translate an opaque texture handle into the ImTextureID accepted by
    // ImGui::Image / AddImage. Default: bit-cast the pointer through
    // uintptr_t (correct for backends whose ImTextureID *is* the texture
    // pointer, e.g. Metal's id<MTLTexture>); the GL implementation overrides
    // to return the underlying GLuint instead.
    virtual ImTextureID textureToImTexture(void* tex) const
    { return (ImTextureID)(uintptr_t)tex; }

    // ── Canvas colour pipeline — POM2 addition ──────────────────────────────
    // A host may offer several colour render pipelines for the canvas (POM2:
    // MAME-NTSC / composite-medium / 4-bit sharp / Le Chat Mauve RGB). The
    // editor shows a selector when more than one is offered; the mono preview
    // toggle is independent. Defaults keep single-pipeline hosts unchanged.
    virtual std::vector<std::string> canvasPipelines() const { return {"NTSC"}; }
    virtual void setCanvasPipeline(int idx) { (void)idx; }
    virtual int  canvasPipeline() const { return 0; }

    // ── DHGR extension (Apple IIe double hi-res) — POM2 addition ────────────
    // DHGR interleaves TWO 8 KB planes: the AUX byte supplies the left 7 dots
    // of each 14-dot cell, the MAIN byte the right 7 (560 dots/line, 140
    // colour pixels of 4 dots each). A host with an auxiliary bank returns
    // true from supportsDhgr() and implements the five methods below; the
    // defaults keep aux-less hosts (POM1's Apple-1 GEN2 card) unchanged — the
    // editor then hides its DHGR page entries.
    virtual bool supportsDhgr() const { return false; }

    // Poke one byte into the AUX plane of video RAM (absolute CPU address,
    // same $2000-$5FFF window as main). Coalesced by begin/endBatch exactly
    // like pokeByte.
    virtual void pokeAuxByte(uint16_t addr, uint8_t value)
    { (void)addr; (void)value; }

    // Sync the live display to DHGR on the given page (GRAPHICS + full screen
    // + HIRES + 80COL + AN3/DHIRES on). The non-DHGR setDisplayMode is
    // expected to switch those two extra latches back off.
    virtual void setDisplayModeDhgr(bool page2) { (void)page2; }

    // Render an aux+main page pair through the host's real DHGR pipeline into
    // a 560×192 RGBA buffer (2*kHiresWidth × kHiresHeight pixels).
    virtual void renderDhgrPage(const uint8_t* aux8k, const uint8_t* main8k,
                                uint32_t* outRgba, bool mono)
    { (void)aux8k; (void)main8k; (void)outRgba; (void)mono; }

    // Load / save a DHGR image dump at `baseAddr` (main-plane base, $2000 or
    // $4000) in A2FC order: AUX 8 KB first, then MAIN 8 KB (16 KB total).
    // Each returns false + sets `err` on failure.
    virtual bool loadDhgrImage(const std::string& path, uint16_t baseAddr,
                               std::string& err)
    { (void)path; (void)baseAddr; err = "DHGR is not supported by this host"; return false; }
    virtual bool saveDhgrImage(const std::string& path, uint16_t baseAddr,
                               std::string& err)
    { (void)path; (void)baseAddr; err = "DHGR is not supported by this host"; return false; }

    // ── DLGR extension (Apple IIe double lo-res) — POM2 addition ────────────
    // 80×48 blocks over an aux+main text-page pair (aux 1 KB first). Render
    // paints 560×192 RGBA; save dumps the 2 KB pair. Loading goes through
    // pokeByte/pokeAuxByte (the editor skips the screen holes itself).
    virtual void renderDlgrPage(const uint8_t* aux1k, const uint8_t* main1k,
                                uint32_t* outRgba, bool mono)
    { (void)aux1k; (void)main1k; (void)outRgba; (void)mono; }
    virtual void setDisplayModeDlgr(bool page2) { (void)page2; }
    virtual bool saveDlgrImage(const std::string& path, uint16_t baseAddr,
                               std::string& err)
    { (void)path; (void)baseAddr; err = "DLGR is not supported by this host"; return false; }
};

} // namespace hgrpaint

#endif // HGRPAINT_IHGRPAINT_HOST_H
