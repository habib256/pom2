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

// Pom2HgrPaintHost — see Pom2HgrPaintHost.h.

#include "Pom2HgrPaintHost.h"
#include "Pom2Build.h"

#include "Apple2Display.h"
#include "AtomicFileReplace.h"
#include "EmulationController.h"
#include "HgrPaintModel.h"        // hgrpaint:: geometry constants
#include "LeChatMauveCard.h"
#include "Logger.h"
#include "Memory.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "Pom2GL.h"

// The portable hgrpaint/ module only *declares* the stb entry points
// (HgrImageDecode.cpp calls stbi_load, savePng below calls stbi_write_png);
// the host app owns the single non-static implementation. MainWindow.cpp's
// copy is STB_IMAGE_STATIC (TU-internal, About-photo only), so this is the
// one that actually links against hgrpaint/.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace {

bool publishBytes(const std::string& path, const uint8_t* data, size_t size,
                  std::string& err)
{
    namespace fs = std::filesystem;
    const fs::path target(path), tmp(path + ".pom2tmp");
    std::error_code permEc;
    const auto perms = fs::status(target, permEc).permissions();
    const bool havePerms = !permEc;
    // The temp name is ours by construction; anything sitting on it is our own
    // debris or a plant that would redirect this write somewhere else (see
    // AtomicFileReplace.h). ofstream(trunc) follows a symlink like any open.
    std::error_code tmpEc;
    if (!pom2::prepareTempPath(tmp, tmpEc)) {
        err = "cannot use temp file " + tmp.string() + ": " + tmpEc.message();
        return false;
    }
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot create " + tmp.string(); return false; }
    out.write(reinterpret_cast<const char*>(data),
              static_cast<std::streamsize>(size));
    out.flush();
    out.close();
    if (!out) {
        err = "write failed (disk full?): " + tmp.string();
        fs::remove(tmp, permEc);
        return false;
    }
    std::error_code ec;
    if (havePerms) { fs::permissions(tmp, perms, ec); ec.clear(); }
    if (!pom2::replaceFileAtomic(tmp, target, ec)) {
        err = "cannot replace " + path + ": " + ec.message();
        fs::remove(tmp, permEc);
        return false;
    }
    return true;
}

// Opaque handle behind IHgrPaintHost's void* texture API.
struct GlTex {
    GLuint id = 0;
    int w = 0, h = 0;
    bool linear = false;
};

} // namespace

Pom2HgrPaintHost::Pom2HgrPaintHost(EmulationController* emu)
    : emu_(emu),
      writer_([this](const PaintCardBatcher::Writes& w) {
          if (!emu_) return;
          auto st = emu_->lockState();
          Memory& mem = st.memory();
          for (const auto& [addr, val] : w)
              if (addr < 0xC000) mem.writeRamUnchecked(addr, val);
      }),
      auxWriter_([this](const PaintCardBatcher::Writes& w) {
          if (!emu_) return;
          auto st = emu_->lockState();
          uint8_t* aux = st.memory().auxDataMutable();
          for (const auto& [addr, val] : w)
              if (addr < 0xC000) aux[addr] = val;
      })
{
}

Pom2HgrPaintHost::~Pom2HgrPaintHost() = default;

// Pokes bypass the IIe paging switches on purpose (writeRamUnchecked / the
// raw aux bank): the editor edits a specific plane of a specific page, and
// must keep doing so even while the live machine has 80STORE/RAMWRT active.
void Pom2HgrPaintHost::pokeByte(uint16_t addr, uint8_t value) { writer_.poke(addr, value); }
void Pom2HgrPaintHost::pokeAuxByte(uint16_t addr, uint8_t value) { auxWriter_.poke(addr, value); }
void Pom2HgrPaintHost::beginBatch() { writer_.begin(); auxWriter_.begin(); }
void Pom2HgrPaintHost::endBatch()   { auxWriter_.end(); writer_.end(); }

// Flip the live machine's video soft switches so the on-screen picture
// follows the page the editor is editing — the same $C05x writes a program
// would perform. IIe-only latches (80COL, AN3/DHIRES) are switched off so a
// previous DHGR selection doesn't linger; Memory ignores them on a II+.
void Pom2HgrPaintHost::setDisplayMode(bool grMode, bool page2)
{
    if (!emu_) return;
    auto st = emu_->lockState();
    Memory& mem = st.memory();
    mem.memWrite(0xC050, 0);                       // GRAPHICS
    mem.memWrite(0xC052, 0);                       // full screen (MIXED off)
    mem.memWrite(page2 ? 0xC055 : 0xC054, 0);      // page select
    mem.memWrite(grMode ? 0xC056 : 0xC057, 0);     // LORES / HIRES
    if (mem.isIIE()) {
        mem.memWrite(0xC00C, 0);                   // 80COL off
        mem.memWrite(0xC05F, 0);                   // AN3 / DHIRES off
    }
}

void Pom2HgrPaintHost::setDisplayModeDhgr(bool page2)
{
    if (!emu_) return;
    auto st = emu_->lockState();
    Memory& mem = st.memory();
    if (!mem.isIIE()) return;
    mem.memWrite(0xC050, 0);                       // GRAPHICS
    mem.memWrite(0xC052, 0);                       // full screen
    mem.memWrite(page2 ? 0xC055 : 0xC054, 0);      // page select
    mem.memWrite(0xC057, 0);                       // HIRES
    mem.memWrite(0xC00D, 0);                       // 80COL on
    mem.memWrite(0xC05E, 0);                       // AN3 / DHIRES on
}

void Pom2HgrPaintHost::setDisplayModeDlgr(bool page2)
{
    if (!emu_) return;
    auto st = emu_->lockState();
    Memory& mem = st.memory();
    if (!mem.isIIE()) return;
    mem.memWrite(0xC050, 0);                       // GRAPHICS
    mem.memWrite(0xC052, 0);                       // full screen
    mem.memWrite(page2 ? 0xC055 : 0xC054, 0);      // page select
    mem.memWrite(0xC056, 0);                       // LORES
    mem.memWrite(0xC00D, 0);                       // 80COL on
    mem.memWrite(0xC05E, 0);                       // AN3 / double-res on
}

bool Pom2HgrPaintHost::supportsDhgr() const
{
    // Read under the state lock like every other machine-state query here.
    // `isIIE()` is a profile-switch-time flag rather than live emulation
    // state, so the old unlocked read never misbehaved in practice — but it
    // was still a read racing `applyProfile`'s write, and the editors call
    // this from their per-frame paths (HgrPaintEditor.cpp:180, :833), which
    // is exactly where an unsynchronised read is least visible. The cost is
    // one uncontended acquire: the worker only holds the lock for a
    // 4096-cycle slice at a time.
    //
    // Safe from the editors: MainWindow closes its own lock scope before
    // calling `render()` (MainWindow.cpp:10079), so this never re-enters.
    if (!emu_) return false;
    auto st = emu_->lockState();
    return st.memory().isIIE();
}

// The ProDOS host-folder convention (`prodos_folder/`, same probe ladder as
// MainWindow's disk library): pictures saved there with their "#TTAAAA" tag
// appear in the synthesised ProDOS volume with the right type + load address
// on the next mount/boot of the host-folder HDV.
std::string Pom2HgrPaintHost::browseDir() const
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* c : {"prodos_folder", "../prodos_folder", "../../prodos_folder"}) {
        if (!fs::is_directory(c, ec)) continue;
        fs::path canon = fs::canonical(c, ec);
        return ec ? std::string(c) : canon.string();
    }
    return {};
}

// ── Offscreen canvas render ──────────────────────────────────────────────────
// A private, never-clocked Memory (IIe mode, so one rig serves HGR, lo-res GR
// and DHGR) + Apple2Display pair. Its cycle counter never advances, so its
// video-event log never publishes a frame: render() always takes the fast
// single-state renderInternal path over exactly the bytes staged below.

void Pom2HgrPaintHost::ensureScratch()
{
    if (scratch_) return;
    scratch_ = std::make_unique<Memory>();
    scratch_->setIIEMode(true);
    gfx_ = std::make_unique<Apple2Display>();
    gfx_->setAuxMemory(scratch_->auxData());
    chatMauve_ = std::make_unique<LeChatMauveCard>();
    gfx_->setChatMauveCard(chatMauve_.get());
    // AppleWin canvas pipeline: Monitor sub-mode — Tv's 50% previous-frame
    // blend would smear while painting (and depends on frame pacing the
    // never-clocked scratch doesn't have).
    gfx_->setAppleWinSubMode(Apple2Display::AppleWinSubMode::Monitor);
}

std::vector<std::string> Pom2HgrPaintHost::canvasPipelines() const
{
    // The two composite pipelines demodulate the real 14.318 MHz waveform —
    // they are what shows the DHGR NTSC-8-px import's extended colours.
    return { "NTSC (MAME)", "Composite medium", "4-bit sharp",
             "Le Chat Mauve RGB", "AppleWin NTSC (composite)",
             "OE composite (CPU)" };
}

void Pom2HgrPaintHost::setCanvasPipeline(int idx)
{
    canvasPipe_ = std::clamp(idx, 0, 5);
}

void Pom2HgrPaintHost::renderScratch(ScratchMode m, const uint8_t* main8k,
                                     const uint8_t* aux8k, uint32_t* outRgba,
                                     bool mono)
{
    ensureScratch();

    if (!scratchStaged_ || scratchMode_ != m) {
        const bool dbl   = (m == ScratchMode::Dhgr || m == ScratchMode::Dlgr);
        const bool hires = (m == ScratchMode::Hgr || m == ScratchMode::Dhgr);
        scratch_->memWrite(0xC050, 0);             // GRAPHICS
        scratch_->memWrite(0xC052, 0);             // full screen
        scratch_->memWrite(0xC054, 0);             // page 1 (pages share layout)
        scratch_->memWrite(hires ? 0xC057 : 0xC056, 0);
        scratch_->memWrite(dbl ? 0xC00D : 0xC00C, 0);   // 80COL
        scratch_->memWrite(dbl ? 0xC05E : 0xC05F, 0);   // AN3 / double-res
        scratchMode_   = m;
        scratchStaged_ = true;
    }

    switch (m) {
    case ScratchMode::Hgr:
        for (int i = 0; i < hgrpaint::kHiresSize; ++i)
            scratch_->writeRamUnchecked(static_cast<uint16_t>(0x2000 + i), main8k[i]);
        break;
    case ScratchMode::Gr:
        // Lo-res: the first 1 KB of the editor page is the text/lo-res page.
        for (int i = 0; i < 0x400; ++i)
            scratch_->writeRamUnchecked(static_cast<uint16_t>(0x0400 + i), main8k[i]);
        break;
    case ScratchMode::Dhgr:
        for (int i = 0; i < hgrpaint::kHiresSize; ++i)
            scratch_->writeRamUnchecked(static_cast<uint16_t>(0x2000 + i), main8k[i]);
        std::memcpy(scratch_->auxDataMutable() + 0x2000, aux8k, hgrpaint::kHiresSize);
        break;
    case ScratchMode::Dlgr:
        for (int i = 0; i < 0x400; ++i)
            scratch_->writeRamUnchecked(static_cast<uint16_t>(0x0400 + i), main8k[i]);
        std::memcpy(scratch_->auxDataMutable() + 0x0400, aux8k, 0x400);
        break;
    }

    // Canvas look: the selected colour pipeline / white-phosphor mono (decay
    // 0 — no afterglow ghosting on erase). Deliberately independent of the
    // user's on-screen HiResMode so the canvas stays deterministic.
    Apple2Display::HiResMode colorMode = Apple2Display::HiResMode::ColorNTSC;
    switch (canvasPipe_) {
    case 1: colorMode = Apple2Display::HiResMode::ColorCompMedium;     break;
    case 2: colorMode = Apple2Display::HiResMode::ColorComp4Bit;       break;
    case 3: colorMode = Apple2Display::HiResMode::ChatMauveRGB;        break;
    case 4: colorMode = Apple2Display::HiResMode::ColorAppleWin;       break;
    case 5: colorMode = Apple2Display::HiResMode::ColorCompositeOECpu; break;
    default: break;
    }
    gfx_->setHiResMode(mono ? Apple2Display::HiResMode::MonoWhite : colorMode);
    gfx_->render(*scratch_);

    // Width adaptation: the renderHgrPage contract is a 280-wide buffer, but
    // the Chat Mauve HGR path paints at its native 560 dots (frame80) —
    // average each dot pair down. DHGR is 560-wide by contract.
    const int wantW = (m == ScratchMode::Dhgr || m == ScratchMode::Dlgr)
                          ? 2 * hgrpaint::kHiresWidth
                          : hgrpaint::kHiresWidth;
    const uint32_t* src = gfx_->pixels();
    if (gfx_->width() == wantW) {
        std::copy(src, src + static_cast<size_t>(wantW) * gfx_->height(), outRgba);
    } else if (gfx_->width() == 2 * wantW) {
        for (int y = 0; y < gfx_->height(); ++y) {
            const uint32_t* in = src + static_cast<size_t>(y) * 2 * wantW;
            uint32_t* out = outRgba + static_cast<size_t>(y) * wantW;
            for (int x = 0; x < wantW; ++x) {
                const uint32_t a = in[2 * x], b = in[2 * x + 1];
                const uint32_t r  = (((a      ) & 0xFF) + ((b      ) & 0xFF)) >> 1;
                const uint32_t g  = (((a >>  8) & 0xFF) + ((b >>  8) & 0xFF)) >> 1;
                const uint32_t bl = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) >> 1;
                out[x] = 0xFF000000u | (bl << 16) | (g << 8) | r;
            }
        }
    } else {
        // Neither the contract width nor the 2× Chat Mauve one: the caller's
        // buffer is sized for `wantW` and there is no safe way to fill it, so
        // leave it untouched (the editor keeps the previous frame) and say so
        // once per width rather than silently painting nothing forever.
        static int warnedWidth = 0;
        if (warnedWidth != gfx_->width()) {
            warnedWidth = gfx_->width();
            pom2::log().warn("HgrPaint",
                "renderScratch: unexpected renderer width " +
                std::to_string(gfx_->width()) + " (expected " +
                std::to_string(wantW) + " or " + std::to_string(2 * wantW) +
                ") — canvas not updated");
        }
    }
}

void Pom2HgrPaintHost::renderHgrPage(const uint8_t* page8k, uint32_t* outRgba,
                                     bool mono, bool grMode)
{
    renderScratch(grMode ? ScratchMode::Gr : ScratchMode::Hgr,
                  page8k, nullptr, outRgba, mono);
}

void Pom2HgrPaintHost::renderDhgrPage(const uint8_t* aux8k, const uint8_t* main8k,
                                      uint32_t* outRgba, bool mono)
{
    renderScratch(ScratchMode::Dhgr, main8k, aux8k, outRgba, mono);
}

void Pom2HgrPaintHost::renderDlgrPage(const uint8_t* aux1k, const uint8_t* main1k,
                                      uint32_t* outRgba, bool mono)
{
    renderScratch(ScratchMode::Dlgr, main1k, aux1k, outRgba, mono);
}

bool Pom2HgrPaintHost::saveDlgrImage(const std::string& path, uint16_t baseAddr,
                                     std::string& err)
{
    if (!emu_) { err = "no emulator"; return false; }
    if (baseAddr > 0xC000 - 0x400) { err = "save address is not RAM"; return false; }
    std::vector<uint8_t> bytes(0x800);
    {
        auto st = emu_->lockState();
        const Memory& mem = st.memory();
        std::memcpy(bytes.data(),         mem.auxData() + baseAddr, 0x400);
        std::memcpy(bytes.data() + 0x400, mem.data()    + baseAddr, 0x400);
    }
    return publishBytes(path, bytes.data(), bytes.size(), err);
}

// ── File I/O ─────────────────────────────────────────────────────────────────

bool Pom2HgrPaintHost::loadImage(const std::string& path, uint16_t baseAddr,
                                 std::string& err)
{
    if (!emu_) { err = "no emulator"; return false; }
    // `0xC000 - baseAddr` is unsigned: a base at or above the I/O page wraps to
    // a ~4 GB allocation instead of a rejection. No video page lives there.
    if (baseAddr >= 0xC000) { err = "load address is not RAM"; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    const size_t maxBytes = static_cast<size_t>(0xC000 - baseAddr);
    std::vector<char> bytes(maxBytes);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(in.gcount()));
    if (bytes.empty()) { err = "empty file"; return false; }
    const size_t n = bytes.size();
    auto st = emu_->lockState();
    Memory& mem = st.memory();
    for (size_t i = 0; i < n; ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(baseAddr + i),
                              static_cast<uint8_t>(bytes[i]));
    return true;
}

bool Pom2HgrPaintHost::saveImage(const std::string& path, uint16_t baseAddr,
                                 int sizeBytes, std::string& err)
{
    if (!emu_) { err = "no emulator"; return false; }
    if (baseAddr >= 0xC000) { err = "save address is not RAM"; return false; }
    if (sizeBytes <= 0) sizeBytes = hgrpaint::kHiresSize;
    sizeBytes = std::min<int>(sizeBytes, 0xC000 - baseAddr);
    std::vector<uint8_t> bytes(static_cast<size_t>(sizeBytes));
    {
        auto st = emu_->lockState();
        std::memcpy(bytes.data(), st.memory().data() + baseAddr, bytes.size());
    }
    return publishBytes(path, bytes.data(), bytes.size(), err);
}

bool Pom2HgrPaintHost::loadDhgrImage(const std::string& path, uint16_t baseAddr,
                                     std::string& err)
{
    if (!emu_) { err = "no emulator"; return false; }
    if (baseAddr > 0xC000 - hgrpaint::kHiresSize) {
        err = "load address is not RAM"; return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    const size_t required = 2 * static_cast<size_t>(hgrpaint::kHiresSize);
    std::vector<char> bytes(required);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<size_t>(in.gcount()) < required) {
        err = "not a 16 KB DHGR (A2FC) dump: " + path;
        return false;
    }
    auto st = emu_->lockState();
    Memory& mem = st.memory();
    std::memcpy(mem.auxDataMutable() + baseAddr, bytes.data(), hgrpaint::kHiresSize);
    for (int i = 0; i < hgrpaint::kHiresSize; ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(baseAddr + i),
                              static_cast<uint8_t>(bytes[hgrpaint::kHiresSize + i]));
    return true;
}

bool Pom2HgrPaintHost::saveDhgrImage(const std::string& path, uint16_t baseAddr,
                                     std::string& err)
{
    if (!emu_) { err = "no emulator"; return false; }
    if (baseAddr > 0xC000 - hgrpaint::kHiresSize) {
        err = "save address is not RAM"; return false;
    }
    std::vector<uint8_t> bytes(2 * static_cast<size_t>(hgrpaint::kHiresSize));
    {
        auto st = emu_->lockState();
        const Memory& mem = st.memory();
        std::memcpy(bytes.data(), mem.auxData() + baseAddr, hgrpaint::kHiresSize);
        std::memcpy(bytes.data() + hgrpaint::kHiresSize, mem.data() + baseAddr,
                    hgrpaint::kHiresSize);
    }
    return publishBytes(path, bytes.data(), bytes.size(), err);
}

bool Pom2HgrPaintHost::savePng(const std::string& path, const uint32_t* rgba,
                               int w, int h, std::string& err)
{
    // rgba is top-down RGBA8, exactly what stbi_write_png expects with
    // stride = w*4.
    namespace fs = std::filesystem;
    const fs::path tmp(path + ".pom2tmp");
    // Same rule as publishBytes: clear the temp name before writing through it.
    std::error_code tmpEc;
    if (!pom2::prepareTempPath(tmp, tmpEc)) {
        err = "cannot use temp file " + tmp.string() + ": " + tmpEc.message();
        return false;
    }
    if (stbi_write_png(tmp.string().c_str(), w, h, 4, rgba, w * 4) == 0) {
        err = "stbi_write_png failed (directory writable?)";
        std::error_code removeEc;
        fs::remove(tmp, removeEc);
        return false;
    }
    std::error_code ec;
    std::error_code permEc;
    const auto perms = fs::status(fs::path(path), permEc).permissions();
    if (!permEc) fs::permissions(tmp, perms, ec);
    ec.clear();
    if (!pom2::replaceFileAtomic(tmp, fs::path(path), ec)) {
        err = "cannot replace " + path + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool Pom2HgrPaintHost::saveBytes(const std::string& path, const void* data,
                                 std::size_t size, std::string& err)
{
    std::error_code ec;
    if (!pom2::writeFileAtomic(std::filesystem::path(path), data, size, ec)) {
        err = ec.message().empty() ? std::string("write failed") : ec.message();
        return false;
    }
    return true;
}

// ── GL texture plumbing ──────────────────────────────────────────────────────
// Same-size repeat uploads (the steady state at ~60 Hz while painting)
// sub-update the existing texture; only a dimension/filter change
// destroys-and-recreates.

void* Pom2HgrPaintHost::uploadTexture(void* tex, const void* rgba,
                                      int w, int h, bool linear)
{
    auto* t = static_cast<GlTex*>(tex);
    if (t && t->w == w && t->h == h && t->linear == linear) {
        glBindTexture(GL_TEXTURE_2D, t->id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        return t;
    }
    if (t) destroyTexture(t);
    t = new GlTex{0, w, h, linear};
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    const GLint filt = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

void Pom2HgrPaintHost::destroyTexture(void* tex)
{
    auto* t = static_cast<GlTex*>(tex);
    if (!t) return;
    if (t->id) glDeleteTextures(1, &t->id);
    delete t;
}

ImTextureID Pom2HgrPaintHost::textureToImTexture(void* tex) const
{
    auto* t = static_cast<GlTex*>(tex);
    return t ? (ImTextureID)(uintptr_t)t->id : (ImTextureID)0;
}
