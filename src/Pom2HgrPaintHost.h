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

// Pom2HgrPaintHost — POM2's implementation of the portable hgrpaint::
// IHgrPaintHost seam (see src/hgrpaint/, shared verbatim with POM1). Pokes
// route into main RAM under EmulationController::stateMutex(), the canvas
// render goes through a private scratch Memory + Apple2Display pair (the
// exact same NTSC pipeline as the live screen, so the editor canvas is
// pixel-identical to the emulator), and files through plain fstream +
// stb_image_write.
//
// The scratch Memory is never clocked, so its video-event log never
// publishes a frame — Apple2Display::render() on it always takes the fast
// single-state path, and the editor page bytes staged at $2000/$0400 are
// what gets painted.

#ifndef POM2_HGRPAINT_HOST_H
#define POM2_HGRPAINT_HOST_H

#include "IHgrPaintHost.h"      // hgrpaint/ on the include path

#include "PaintCardBatcher.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Apple2Display;
class EmulationController;
class LeChatMauveCard;
class Memory;

class Pom2HgrPaintHost : public hgrpaint::IHgrPaintHost
{
public:
    explicit Pom2HgrPaintHost(EmulationController* emu);
    ~Pom2HgrPaintHost() override;

    void pokeByte(uint16_t addr, uint8_t value) override;
    void beginBatch() override;
    void endBatch() override;
    void setDisplayMode(bool grMode, bool page2) override;
    void renderHgrPage(const uint8_t* page8k, uint32_t* outRgba, bool mono,
                       bool grMode = false) override;
    bool loadImage(const std::string& path, uint16_t baseAddr, std::string& err) override;
    bool saveImage(const std::string& path, uint16_t baseAddr, int sizeBytes,
                   std::string& err) override;
    bool savePng(const std::string& path, const uint32_t* rgba,
                 int w, int h, std::string& err) override;
    // The sprite editor's raw/ASM exports, committed the way every other POM2
    // write-back commits (durable temp + rename) instead of the portable
    // default's plain rename.
    bool saveBytes(const std::string& path, const void* data, std::size_t size,
                   std::string& err) override;
    void* uploadTexture(void* tex, const void* rgba,
                        int w, int h, bool linear) override;
    void  destroyTexture(void* tex) override;
    ImTextureID textureToImTexture(void* tex) const override;

    // Canvas colour pipeline: MAME-NTSC / composite-medium / 4-bit / Chat Mauve.
    std::vector<std::string> canvasPipelines() const override;
    void setCanvasPipeline(int idx) override;
    int  canvasPipeline() const override { return canvasPipe_; }

    // File browser home: the ProDOS host folder (prodos_folder/) when present,
    // so a tagged save ("PIC#062000") lands straight in the synthesised ProDOS
    // volume — BLOAD-able by name at $2000 after a (re)mount.
    std::string browseDir() const override;

    // ── DHGR extension (POM2-only, see IHgrPaintHost.h) ──────────────────────
    bool supportsDhgr() const override;
    void pokeAuxByte(uint16_t addr, uint8_t value) override;
    void setDisplayModeDhgr(bool page2) override;
    void renderDhgrPage(const uint8_t* aux8k, const uint8_t* main8k,
                        uint32_t* outRgba, bool mono) override;
    bool loadDhgrImage(const std::string& path, uint16_t baseAddr,
                       std::string& err) override;
    bool saveDhgrImage(const std::string& path, uint16_t baseAddr,
                       std::string& err) override;

    // ── DLGR extension ───────────────────────────────────────────────────────
    void renderDlgrPage(const uint8_t* aux1k, const uint8_t* main1k,
                        uint32_t* outRgba, bool mono) override;
    void setDisplayModeDlgr(bool page2) override;
    bool saveDlgrImage(const std::string& path, uint16_t baseAddr,
                       std::string& err) override;

private:
    // Stage the scratch soft switches for one of the three editor regimes and
    // render it. `page8k`/`aux8k` are page-relative editor bytes.
    enum class ScratchMode { Hgr, Gr, Dhgr, Dlgr };
    void renderScratch(ScratchMode m, const uint8_t* main8k, const uint8_t* aux8k,
                       uint32_t* outRgba, bool mono);
    void ensureScratch();

    EmulationController* emu_;
    PaintCardBatcher writer_;              // begin/end/poke → one lock per batch
    PaintCardBatcher auxWriter_;           // same, for the DHGR aux plane

    // Offscreen render rig (lazy — built on first canvas render). IIe mode so
    // the same pair serves HGR, lo-res GR and DHGR.
    std::unique_ptr<Memory> scratch_;
    std::unique_ptr<Apple2Display> gfx_;
    // Standalone Chat Mauve card attached to the scratch display so the
    // "Le Chat Mauve RGB" canvas pipeline can decode (default COL140 mode);
    // inert for every other pipeline (the ChatMauve branches gate on the
    // HiResMode, not on the card's presence).
    std::unique_ptr<LeChatMauveCard> chatMauve_;
    ScratchMode scratchMode_ = ScratchMode::Hgr;
    bool scratchStaged_ = false;           // soft switches staged at least once
    int canvasPipe_ = 0;                   // index into canvasPipelines()
};

#endif // POM2_HGRPAINT_HOST_H
