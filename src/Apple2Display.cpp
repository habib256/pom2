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

#include "Apple2Display.h"
#include "Apple2Display_Internal.h"
#include "Apple2VideoDecode.h"
#include "AppleWinNtsc.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

// NTSC artifact-decode primitives (bit doubler, artifact LUT, rotl4b, word /
// bit-stream builders, avgRgb) live in Apple2VideoDecode.h — shared by the
// HGR, DHGR and composite-signal paths. Bring them into scope so the
// per-scanline call sites below stay unqualified.
using namespace pom2::a2v;
using namespace pom2::a2disp;   // Apple2Display_Internal.h helpers

Apple2Display::Apple2Display()
    : frame(kWidth * kHeight, 0xFF000000)
    , frame80(kWidth80 * kHeight, 0xFF000000)
    , appleWinPrev  (kWidth   * kHeight, 0xFF000000)
    , appleWinPrev80(kWidth80 * kHeight, 0xFF000000)
    , persistenceL  (kWidth   * kHeight, 0)
    , persistenceL80(kWidth80 * kHeight, 0)
    , signalBuf    (kSignalWidth * kSignalHeight, 0)
{
}

void Apple2Display::setHiResMode(HiResMode m)
{
    if (m == hiResMode) return;
    hiResMode = m;
    // Clear both phosphor histories so a residual amber afterglow doesn't
    // tint a freshly-selected green or colour mode for a few frames.
    std::fill(persistenceL.begin(),   persistenceL.end(),   0);
    std::fill(persistenceL80.begin(), persistenceL80.end(), 0);
    // Clear the composite signal buffer too: if we just left the
    // OpenEmulator mode the leftover bytes would be irrelevant; if we
    // just entered it, the first frame's render() will repopulate.
    std::fill(signalBuf.begin(), signalBuf.end(), 0);
    signalProducedFlag = false;
    // Also clear the AppleWin Tv sub-mode's "previous frame" buffer so
    // we don't blend leftover content from another mode — and say so, or
    // the first frame blends against the black we just wrote.
    std::fill(appleWinPrev  .begin(), appleWinPrev  .end(), 0xFF000000u);
    std::fill(appleWinPrev80.begin(), appleWinPrev80.end(), 0xFF000000u);
    appleWinPrevValid_ = false;
    appleWinPainted_   = false;
}

void Apple2Display::setAppleWinSubMode(AppleWinSubMode m)
{
    if (m == appleWinSubMode) return;
    appleWinSubMode = m;
    // Tv blur references the previous frame's buffer — reset it on
    // sub-mode switch so Monitor → Tv doesn't ghost the last Monitor
    // frame in.
    std::fill(appleWinPrev  .begin(), appleWinPrev  .end(), 0xFF000000u);
    std::fill(appleWinPrev80.begin(), appleWinPrev80.end(), 0xFF000000u);
    appleWinPrevValid_ = false;
    appleWinPainted_   = false;
}

uint16_t Apple2Display::textRowAddress(int y, bool page2)
{
    // Apple II text/lo-res row interleave: addr = base + 0x80*(y%8) +
    // 0x28*(y/8). The 8 KB DRAM dance the Apple I never had — Woz reused
    // the row counter to refresh dynamic RAM.
    const uint16_t base = page2 ? 0x0800 : 0x0400;
    return static_cast<uint16_t>(base + 0x80 * (y & 7) + 0x28 * (y >> 3));
}

uint16_t Apple2Display::hgrRowAddress(int y, bool page2)
{
    // HGR formula: addr = base + 0x400*(y%8) + 0x80*((y/8)%8) + 0x28*(y/64).
    // Same trick as text mode but the inner counter goes through 8 banks
    // of 8 sub-pages of 40 bytes (3 sub-pages × 64 lines = 192).
    const uint16_t base = page2 ? 0x4000 : 0x2000;
    return static_cast<uint16_t>(base
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

void Apple2Display::applyVideoEvent(Memory::DisplayState& state,
                                    Memory::VideoEventKind kind, bool value)
{
    switch (kind) {
        case Memory::VideoEventKind::TextMode:  state.textMode  = value; break;
        case Memory::VideoEventKind::MixedMode: state.mixedMode = value; break;
        case Memory::VideoEventKind::Page2:     state.page2     = value; break;
        case Memory::VideoEventKind::HiRes:     state.hiRes     = value; break;
        case Memory::VideoEventKind::EightyCol: state.eightyCol = value; break;
        case Memory::VideoEventKind::Dhgr:      state.dhgr      = value; break;
        case Memory::VideoEventKind::An3:       state.an3       = value; break;
        case Memory::VideoEventKind::EightyStore: state.eightyStore = value; break;
        case Memory::VideoEventKind::AltChar:   state.altChar   = value; break;
    }
}

// See the TextFrameKey comment in Apple2Display.h for why this is limited to
// full-screen text and never consulted on a beam-raced frame.
bool Apple2Display::staticTextFrameUnchanged(Memory& mem,
                                             const Memory::DisplayState& state)
{
    // MIXED counts as graphics: its bottom four text rows sit under a graphics
    // band whose painter does write phosphor persistence. Leaving the candidate
    // key invalid is all it takes to bar the next frame from skipping — render()
    // commits whatever this function leaves behind.
    if (!state.textMode || state.mixedMode) return false;

    // $0400-$0BFF covers text/lo-res pages 1 AND 2, taken from BOTH main and
    // aux. Copying the union rather than resolving which page is live keeps
    // this independent of the page/80STORE/80COL routing rules — if any byte
    // the text painters could possibly read moves, the compare fails and we
    // repaint. Over-covering costs a bigger memcmp; under-covering would show
    // a stale screen.
    constexpr size_t kBase = 0x0400, kLen = 0x0800;
    const uint8_t* mainRam = mem.data();
    const uint8_t* auxBytes = mem.auxData();

    TextFrameKey k;
    k.valid       = true;
    k.state       = state;
    k.iie         = mem.isIIE();
    k.flashPhase  = ((frameCounter / kFlashHalfPeriodFrames) & 1u) != 0;
    k.textSharp   = oeDemod_.textSharp;
    k.hiResModeId = static_cast<int>(hiResMode);
    // The char ROM goes in by VALUE, not by pointer: reloading a different
    // character set can reuse the same heap block, so a pointer+size compare
    // could report "unchanged" across an actual glyph change and freeze the
    // old font on screen. A few KB more memcmp is not worth that risk.
    // The ACTIVE 4 KB set (an 8 KB dual-bank ROM's live half, chosen by AN2 —
    // charRomActiveData). The pointer differs between banks, so keying on it
    // makes an AN2 font switch invalidate the cached text frame.
    const uint8_t* cromData = mem.charRomActiveData();
    const std::size_t cromSize = mem.charRomActiveSize();
    k.charRom     = cromData;
    k.charRomSize = cromSize;
    // Le Chat Mauve state the guest can move without touching DisplayState and
    // without emitting a video event ($C0B8-$C0BB, see the header). currentMode
    // also rides along: it is driven by the $C05E/$C05F FIFO, which DOES emit
    // events today, but keying on it costs one int and removes the dependence
    // on that staying true.
    k.chatMauve = chatMauve;
    k.chatMauveState =
        chatMauve ? static_cast<int>(chatMauve->renderStateKey()) : -1;
    k.vram.resize(kLen * 2 + cromSize);
    std::memcpy(k.vram.data(),            mainRam + kBase, kLen);
    std::memcpy(k.vram.data() + kLen,     auxBytes + kBase, kLen);
    if (cromData && cromSize)
        std::memcpy(k.vram.data() + kLen * 2, cromData, cromSize);

    const TextFrameKey& p = textFrameKey_;
    const bool same =
        p.valid &&
        p.iie == k.iie && p.flashPhase == k.flashPhase &&
        p.textSharp == k.textSharp &&
        p.hiResModeId == k.hiResModeId &&
        p.charRom == k.charRom && p.charRomSize == k.charRomSize &&
        p.chatMauve == k.chatMauve && p.chatMauveState == k.chatMauveState &&
        std::memcmp(&p.state, &k.state, sizeof(Memory::DisplayState)) == 0 &&
        p.vram.size() == k.vram.size() &&
        std::memcmp(p.vram.data(), k.vram.data(), k.vram.size()) == 0;

    nextTextFrameKey_ = std::move(k);
    return same;
}

void Apple2Display::renderInternal(Memory& mem,
                                   const Memory::DisplayState& state)
{
    // `state` comes from render() — the PUBLISHED frame's, never
    // `mem.getDisplayState()`. Re-reading the live one here made the full-
    // frame repaint disagree with the frame-skip key (which is built from the
    // published state): a switch thrown just after the frame boundary left
    // the key saying "nothing changed, skip" while this painted the new mode.
    renderInternalBand(mem, state, 0, kHeight);
}

void Apple2Display::renderInternalBand(Memory& mem, const Memory::DisplayState& state,
                                       int scanY0, int scanY1)
{
    if (scanY0 >= scanY1) return;
    renderInternalBandImpl(mem, state, scanY0, scanY1);
    // Eve TXTGREEN is a post-pass over whatever painted the text rows: the
    // decision tree below has a dozen exits and the text painters are shared
    // with every other pipeline, so the tint is applied once, here.
    if (chatMauveActive() &&
        chatMauve->textMode(state.eightyCol, /*an3On=*/!state.dhgr)
            == LeChatMauveCard::TextMode::Green)
        tintTextGreen(state, scanY0, scanY1);
}

void Apple2Display::renderInternalBandImpl(Memory& mem, const Memory::DisplayState& state,
                                           int scanY0, int scanY1)
{
    const bool iie80 = mem.isIIE() && state.eightyCol;
    const int  hiResEnd = state.mixedMode ? 160 : 192;
    int gLo = 0, gHi = 0, tLo = 0, tHi = 0;

    // ── Le Chat Mauve / Video-7 RGB card — single HGR (native 560 dots) ──
    // Every single-HGR state under the card comes here: AN3 on (ordinary
    // HGR), and AN3 off with 80COL off — where the Féline goes monochrome
    // (`POKE -16290,0`), the Video-7 shows its foreground/background mode
    // (MAME hgr_update: rgb_monitor && m_dhires && !m_80col) and the Eve
    // keeps decoding. AN3 off WITH 80COL on is DHGR: it takes the IIe path
    // below and renderDhgr asks the card again (LeChatMauveCard::dhgrMode).
    // The card decides (hgrMode); this function only routes.
    const bool cm = chatMauveActive();
    const bool chatMauveHGR =
        cm && state.hiRes && !state.textMode && !(iie80 && state.dhgr);
    if (chatMauveHGR) {
        if (bandScanlines(scanY0, scanY1, 0, hiResEnd, &gLo, &gHi)) {
            const auto hm = chatMauve->hgrMode(/*an3On=*/!state.dhgr);
            if ((hm == LeChatMauveCard::HgrMode::FgBg ||
                 hm == LeChatMauveCard::HgrMode::Cp280) && auxRam != nullptr)
                renderHgrDuochrome(mem, state, gLo, gHi,
                                   chatMauve->auxHiNibbleIsForeground());
            else
                renderHiResChatMauve80(mem, state, gLo, gHi, hm);
        }
        if (state.mixedMode && bandScanlines(scanY0, scanY1, 160, 192, &gLo, &gHi)) {
            if (iie80) {
                if (bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
                    renderText80(mem, state, tLo, tHi, scanY0, scanY1);
            } else {
                if (bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
                    renderText(mem, state, tLo, tHi, 0, 40, scanY0, scanY1);
                upscaleFrameToFrame80(gLo, gHi);
            }
        }
        setUseFrame80(true);
        return;
    }

    // ── Le Chat Mauve / Video-7 RGB card — colour TEXT ───────────────────
    // Video-7 F/B (AN3 on, 80COL off, hi nibble = fg) and the Eve's TXT16
    // ($C0B9, 80COL off, hi nibble = bg). Plain and Green fall through to
    // the motherboard's text painters (Green is tinted afterwards).
    if (cm && mem.isIIE() && state.textMode && auxRam != nullptr &&
        chatMauve->textMode(state.eightyCol, /*an3On=*/!state.dhgr)
            == LeChatMauveCard::TextMode::Color) {
        if (bandRows(scanY0, scanY1, 0, 24, &tLo, &tHi))
            renderTextChatMauveFgBg(mem, state, tLo, tHi, scanY0, scanY1,
                                    chatMauve->auxHiNibbleIsForeground());
        setUseFrame80(true);
        return;
    }

    // ── IIe 80-column native paths (560-wide frame80) ───────────────────
    if (iie80) {
        if (state.textMode) {
            if (bandRows(scanY0, scanY1, 0, 24, &tLo, &tHi))
                renderText80(mem, state, tLo, tHi, scanY0, scanY1);
            setUseFrame80(true);
            return;
        }
        if (state.hiRes && state.dhgr) {
            if (bandScanlines(scanY0, scanY1, 0, hiResEnd, &gLo, &gHi))
                renderDhgr(mem, state, gLo, gHi);
            if (state.mixedMode && bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
                renderText80(mem, state, tLo, tHi, scanY0, scanY1);
            setUseFrame80(true);
            return;
        }
        if (!state.hiRes && state.dhgr) {
            const int blockEnd = state.mixedMode ? 40 : 48;
            if (bandScanlines(scanY0, scanY1, 0, blockEnd * 4, &gLo, &gHi))
                renderLoResDouble(mem, state, gLo / 4, (gHi + 3) / 4, gLo, gHi);
            if (state.mixedMode && bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
                renderText80(mem, state, tLo, tHi, scanY0, scanY1);
            setUseFrame80(true);
            return;
        }
        if (state.mixedMode) {
            if (state.hiRes && bandScanlines(scanY0, scanY1, 0, 160, &gLo, &gHi))
                renderHiRes(mem, state, gLo, gHi);
            else if (!state.hiRes && bandScanlines(scanY0, scanY1, 0, 160, &gLo, &gHi))
                renderLoRes(mem, state, gLo / 4, (gHi + 3) / 4, 0, 40, gLo, gHi);
            if (gLo < gHi)
                upscaleFrameToFrame80(gLo, gHi);
            if (bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
                renderText80(mem, state, tLo, tHi, scanY0, scanY1);
            setUseFrame80(true);
            return;
        }
    }

    // ── Legacy 280-wide path (frame) ────────────────────────────────────
    setUseFrame80(false);
    if (state.textMode) {
        if (bandRows(scanY0, scanY1, 0, 24, &tLo, &tHi))
            renderText(mem, state, tLo, tHi, 0, 40, scanY0, scanY1);
    } else if (state.hiRes) {
        if (bandScanlines(scanY0, scanY1, 0, 192, &gLo, &gHi))
            renderHiRes(mem, state, gLo, gHi);
        if (state.mixedMode && bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
            renderText(mem, state, tLo, tHi, 0, 40, scanY0, scanY1);
    } else {
        if (bandScanlines(scanY0, scanY1, 0, 48 * 4, &gLo, &gHi))
            renderLoRes(mem, state, gLo / 4, (gHi + 3) / 4, 0, 40, gLo, gHi);
        if (state.mixedMode && bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
            renderText(mem, state, tLo, tHi, 0, 40, scanY0, scanY1);
    }

    // Beam-raced replay under the Chat Mauve (bandLatch_ >= 0 only inside
    // renderBeamRacing's paint): bridge this 280-wide band into frame80 so
    // it shares a buffer with the card's 560-wide segments — DIX rasters
    // mix TEXT/GR bands with Féline HGR inside one frame. Static frames
    // keep the native 280 render (goldens untouched).
    if (cm && bandLatch_ >= 0) {
        upscaleFrameToFrame80(scanY0, scanY1);
        setUseFrame80(true);
    }
}

Apple2Display::RasterPos Apple2Display::frameCycleToPos(uint64_t emuCycle,
                                                       VideoStandard std)
{
    // Mirror Memory::pushVideoEventLocked's scanline derivation so this maps
    // a VideoEvent's emuCycle back to the exact (scanline, byteCol) the
    // recorder stamped — only the horizontal component was discarded before.
    // PAL = 312 lines/frame, NTSC = 262; the 65-cycle line is the same.
    const VideoTiming& t = pom2VideoTiming(std);
    const uint64_t kCyclesPerScanline = static_cast<uint64_t>(t.cyclesPerScanline);
    const uint64_t kScanlinesPerFrame = static_cast<uint64_t>(t.scanlinesPerFrame);
    const uint64_t rawLine = (emuCycle / kCyclesPerScanline) % kScanlinesPerFrame;
    // VBL lines collapse to kHeight (192) — the SAME "frame end, not
    // visible" stamp Memory::pushVideoEventLocked records, so this really
    // is the recorder's inverse. (It briefly clamped to 191 instead; any
    // consumer re-deriving an event's line from emuCycle would have
    // resurrected the spurious line-191 split the 192 stamp eliminated.)
    // The visible replay consumes only byteCol from this function.
    const int scanline = rawLine < static_cast<uint64_t>(kHeight)
                             ? static_cast<int>(rawLine)
                             : kHeight;
    // The 40-byte visible window opens at horizontal cycle 25 (the first 25
    // cycles of each scanline are horizontal blanking). A switch thrown in
    // HBL (hpos < 25) lands at byteCol 0 → it governs the whole upcoming line;
    // a switch inside the window splits the line at that byte boundary. The
    // exact transition cycle within a character clock is a later refinement;
    // v1 is "visually correct at the column boundary".
    const int hpos    = static_cast<int>(emuCycle % kCyclesPerScanline);
    // -24, not -25. The visible window does open at hpos 25, but a soft
    // switch performed AT hpos 25+c is too late to affect column c: the
    // video scanner latches that byte during phi1 of the cycle whose phi2
    // the CPU is using for its access, so the switch first shows at column
    // c+1. Hence the effective mapping is one cycle earlier than the raw
    // window offset.
    //
    // Measured, not assumed. Replaying French Touch's MAD EFFECT (GPLv3
    // sources in disks_5.4/demo/madef/) and sweeping all 65 candidate
    // phases, the demo's 192 per-scanline lit-run starts ($C055 — their
    // column IS the silhouette it draws) land wholly inside the 40-column
    // window only for offsets **21..24**, and nowhere else. 25 sat just
    // outside that band, which is exactly why the leftmost scanlines of the
    // picture spilled one cycle into HBL and clamped to column 0 while the
    // rest drew correctly. 24 is the edge of the measured band and the one
    // value with a mechanism behind it.
    //
    // (Sweeping ALL switches instead has no solution: the `$C054` that
    // CLOSES the lit run is legitimately thrown in HBL — a switch in
    // blanking governs the whole upcoming line. Only the opening switch
    // must be inside the window.)
    const int byteCol = std::clamp(hpos - 24, 0, 40);
    return {scanline, byteCol};
}

void Apple2Display::patchMixedTextBand(Memory& mem,
                                       const Memory::DisplayState& state)
{
    // `state` is the PUBLISHED frame's state, handed down by render() — the
    // same one that chose this path. Polling `mem.getDisplayState()` here
    // instead asked the LIVE recording frame, which the CPU worker has
    // already advanced past: a guest that flipped to TEXT after the frame
    // closed skipped the band entirely (the demod's graphics rows stayed
    // under the text), and a page flip drew it from the wrong page.
    if (!state.mixedMode || state.textMode) return;

    if (mem.isIIE() && state.eightyCol)
        renderText80(mem, state, 20, 24);
    else {
        renderText(mem, state, 20, 24);
        upscaleFrameToFrame80(kMixedTextFirstScanline, kHeight);
    }
}

void Apple2Display::render(Memory& mem)
{
    // The static-text skip key is published when render() returns, on every
    // path — see commitTextFrameKey() and the two-slot rationale in the header.
    // Doing it here rather than at the bottom of the function means the frame
    // this render() paints is the one the key describes even if a future edit
    // grows an early return.
    struct KeyPublisher {
        Apple2Display* self;
        ~KeyPublisher() { self->commitTextFrameKey(); }
    } keyPublisher{this};
    // A demod armed for the PREVIOUS frame's signal must not fire onto the
    // pixels this frame is about to paint; whoever wants it has already had
    // its chance (pixels() / finishPendingCpuDemod() run it lazily), and the
    // paths below re-arm it for this frame when the mode still calls for one.
    pendingCpuDemodRows_ = 0;

    // Frame counter drives the FLASH animation, phosphor persistence and
    // the AppleWin Tv blur. Derive it from the EMULATED frame index
    // (cycleCounter / cycles-per-video-frame, standard-aware) instead of
    // ++ per render call: the UI renders at the HOST monitor's refresh
    // (vsync — 120/144 Hz panels exist), which made FLASH blink 2-2.4×
    // too fast there, and PAL machines flashed at the NTSC rate. MAME's
    // flash is frame_number() & 0x10 at the machine's own 50/60 Hz;
    // frameCounter = emulated frame index reproduces exactly that.
    {
        const auto& vt = pom2VideoTiming(mem.videoStandard());
        const uint64_t emuFrame = mem.getCycleCounter() /
            (65ull * static_cast<uint64_t>(vt.scanlinesPerFrame));
        // Delta feeds decay/blur pacing; clamp stalls, and treat a
        // backwards jump (rewind / snapshot load) as "no time passed".
        emuFrameDelta_ = (emuFrame > lastEmuFrame_)
            ? static_cast<uint32_t>(std::min<uint64_t>(emuFrame - lastEmuFrame_, 8))
            : 0;
        lastEmuFrame_ = emuFrame;
        frameCounter  = static_cast<uint32_t>(emuFrame);
    }
    mixedCompositeUsesFb_ = false;

    // Routing state must describe the PUBLISHED frame — the one whose
    // pixels we are about to paint — not the live recording frame that is
    // already running ahead of it. `mem.getDisplayState()` is the live
    // one: with beam-raced content, a mid-frame switch that happened
    // AFTER the published frame closed (say the guest flipping back to
    // TEXT) chose the wrong demod/mixed path for pixels that were still
    // graphics, and `lastRenderState_` — documented as "the published
    // frame's snapshot" and used by the present path — carried the same
    // error. Fold the published events onto the published frame-start
    // state instead; with no events the two are identical, so the
    // non-beam-raced path is unchanged.
    //
    // A frame with NO events used to fall through to the live state, which
    // is the same error one frame later: nothing happened DURING the
    // published frame, but the guest may well have thrown a switch right
    // after it closed (that switch sits in Memory's recording log, not in
    // `events`), and painting the published frame in the new mode flipped it
    // a frame early. `frameCounter > 0` is the machine having completed a
    // video frame, i.e. Memory having published one: before that the
    // published snapshot is still the power-on default and the live state is
    // the only description of the screen there is — which is also how the
    // display tests drive this class, poking soft switches into Memory with
    // no clock running at all.
    auto events = mem.takeVideoEvents();
    Memory::DisplayState state = mem.getDisplayState();
    if (!events.empty() || frameCounter > 0) {
        state = mem.getDisplayStateAtFrameStart();
        for (const auto& e : events) applyVideoEvent(state, e.kind, e.value);
    }
    lastRenderState_ = state;   // published-frame snapshot for present-path decisions
    // Any graphics band anywhere in the frame keeps the composite/demod
    // path alive, even when the frame ENDS in text (mixed-mode splits).
    bool mixedGfx = state.mixedMode && !state.textMode;
    if (!events.empty()) {
        Memory::DisplayState walk = mem.getDisplayStateAtFrameStart();
        if (walk.mixedMode && !walk.textMode) mixedGfx = true;
        for (const auto& e : events) {
            applyVideoEvent(walk, e.kind, e.value);
            if (walk.mixedMode && !walk.textMode) mixedGfx = true;
        }
    }

    // Both ColorCompositeOE and ColorAppleWin consume the same 14.318 MHz
    // composite bitstream. ColorCompositeOE hands it to MainWindow's GLSL
    // demod pass (signalProduced() = true is the gate) AND keeps a framebuffer
    // (the NTSC-LUT render, below) as the fallback shown when the shader is
    // unavailable. ColorAppleWin demodulates the signal CPU-side here and
    // overwrites the entire 560-wide frame80 — so renderInternal's framebuffer
    // colorization would be 100 % discarded for AppleWin. Skip it: that is the
    // "double render" Phase 4 removes. (Falls through to renderInternal only if
    // the signal somehow can't be produced, so we never present stale pixels.)
    const bool appleWin   = (hiResMode == HiResMode::ColorAppleWin);
    const bool oeCpu      = (hiResMode == HiResMode::ColorCompositeOECpu);
    // CPU demods (AppleWin, OE-CPU) overwrite frame80 from the composite
    // signal for graphics; full-screen TEXT uses renderInternal (crisp
    // mono) — same as MAME/LUT paths and the GPU textSharp bypass; demod
    // would falsely colour the glyph edges. OE-CPU mirrors the GPU's
    // textSharp toggle exactly: when the user unchecks "Sharp text" the
    // GPU shader demodulates full-screen TEXT, so the CPU twin must too
    // (it used to ignore the toggle and always render text crisp).
    const bool cpuDemod     = appleWin || oeCpu;
    const bool oeDemodsText = oeCpu && !oeDemod_.textSharp;
    const bool cpuDemodGfx  = cpuDemod && (!state.textMode || oeDemodsText);
    const bool needSignal = (hiResMode == HiResMode::ColorCompositeOE) || cpuDemod;

    // Fetch the last PUBLISHED video frame's soft-switch log ONCE (a copy —
    // Memory republishes at each video-frame boundary, so re-rendering the
    // same frame at 60 Hz over 50 Hz PAL content is safe). Both consumers
    // need it: the RGBA beam-racing path (the fallback framebuffer) AND the
    // composite signal builder, so mid-scanline mode switches (text↔graphics
    // splits, page flips, DHGR toggles) show up in the OE/AppleWin composite
    // picture too — not just the LUT modes. `events` survives the
    // renderBeamRacing copy and is handed to fillCompositeSignal below.
    if (!cpuDemodGfx) {
        if (events.empty()) {
            // Repaint unless this is the very same full-screen text frame that
            // is already in the framebuffer. Beam-raced frames take the else
            // branch and are never skipped.
            if (!staticTextFrameUnchanged(mem, state)) renderInternal(mem, state);
        } else {
            // A beam-raced frame publishes no key: the framebuffer no longer
            // corresponds to any single whole-frame text state.
            renderBeamRacing(mem, events);
        }
    }
    // The remaining case — a CPU demod about to overwrite frame80 from the
    // composite signal — also publishes no key, and needs no statement to say
    // so: nextTextFrameKey_ stays invalid.

    signalProducedFlag = needSignal ? fillCompositeSignal(mem, events) : false;

    if (cpuDemodGfx && !signalProducedFlag) {
        // Defensive fallback: no signal → render the normal framebuffer so
        // the screen isn't left showing the previous frame's demod output.
        renderInternal(mem, state);
    }

    // The 17-tap FP demod (~1-2 ms) is DEFERRED to finishPendingCpuDemod():
    // it consumes only signalBuf (filled above) and writes frame80 — both
    // display-owned, UI-thread-only — so it doesn't need the caller's
    // stateMutex, which used to stall the CPU worker every UI frame. The
    // mixed text band is patched HERE (it reads guest RAM → needs the
    // lock); the deferred demod is per-row and only rewrites the graphics
    // rows [0, 160) in mixed mode, so the patch survives it.
    if (oeCpu && signalProducedFlag && (!state.textMode || oeDemodsText)) {
        if (mixedGfx) {
            patchMixedTextBand(mem, state);
            scheduleCpuDemodInto80(kMixedTextFirstScanline);
        } else {
            scheduleCpuDemodInto80(kSignalHeight);
        }
    }

    if ((mixedGfx && hiResMode == HiResMode::ColorCompositeOE)
        && signalProducedFlag) {
        patchMixedTextBand(mem, state);
        scheduleCpuDemodInto80(kMixedTextFirstScanline);
        mixedCompositeUsesFb_ = true;
    }

    if (appleWin && signalProducedFlag && !state.textMode) {
        // Map our public sub-mode enum onto the pom2::AppleWinNtsc::SubMode
        // values 1-for-1 — they're declared as separate types only so
        // the public Apple2Display API doesn't drag AppleWinNtsc.h into
        // every TU that includes Apple2Display.h.
        pom2::AppleWinNtsc::SubMode sub = pom2::AppleWinNtsc::SubMode::Monitor;
        switch (appleWinSubMode) {
            case AppleWinSubMode::Monitor:   sub = pom2::AppleWinNtsc::SubMode::Monitor;   break;
            case AppleWinSubMode::Tv:        sub = pom2::AppleWinNtsc::SubMode::Tv;        break;
            case AppleWinSubMode::Idealized: sub = pom2::AppleWinNtsc::SubMode::Idealized; break;
        }
        const int w = kSignalWidth;   // 560
        const int h = kSignalHeight;  // 192
        // Tv blur references the previous EMULATED frame. Refresh the stash
        // only when the machine actually advanced a frame — stashing every
        // render call made the blur reference the previous *UI* frame, so on
        // a 144 Hz monitor the 50 % blend collapsed toward nothing (and
        // repeated renders of one paused frame blended it into itself).
        // frame80 still holds the previous frame's final output here — but
        // only if THIS path is what put it there. On the first frame after a
        // boot or a mode / sub-mode switch it holds another mode's picture,
        // or nothing at all, and blending against it dimmed the whole frame
        // by half (the black clear the switch does) or ghosted the outgoing
        // mode into it. So the stash is taken only once an AppleWin frame
        // exists to stash, and the blend is skipped — prevFrame = nullptr,
        // the "very first frame" case the API documents — until it does.
        if (emuFrameDelta_ > 0 && appleWinPainted_) {
            std::memcpy(appleWinPrev80.data(), frame80.data(),
                        static_cast<size_t>(w) * h * sizeof(uint32_t));
            appleWinPrevValid_ = true;
        }
        pom2::AppleWinNtsc::renderFrame(signalBuf.data(),
                                  frame80.data(),
                                  w, h,
                                  sub,
                                  appleWinPrevValid_ ? appleWinPrev80.data()
                                                     : nullptr,
                                  signalPhaseOffset_);
        appleWinPainted_ = true;
        // The output IS native 560-wide regardless of the Apple II's
        // soft-switch state, so route the UI to frame80.
        setUseFrame80(true);
        if (mixedGfx)
            patchMixedTextBand(mem, state);
    }
}

// CPU port of the OpenEmulator demod shader (NtscPostProcessor's demod-only
// fragment shader). Same Y/U/V recovery, YUV→RGB matrix, subcarrier phase
// AND user knobs (hue rotation, chroma-bandwidth sharpness blend, PAL
// line-phase alternation via setOeDemodParams) as the GPU path, run on the
// CPU into frame80 — any edit here must be mirrored in the GLSL
// (NtscPostProcessor.cpp kFragmentShader) and re-pinned by
// oe_demod_gpu_cpu_parity. CRT glass (scanlines / mask / barrel /
// persistence) is layered on afterward by CrtEffectStack when enabled.
//
// Optimised: the tap weights depend only on the tap offset i, and the
// subcarrier sin/cos depend only on (x+i) mod 4 — both hoisted out of the
// per-pixel loop, so the inner loop is mul/add only (no trig per pixel).
void Apple2Display::finishPendingCpuDemod()
{
    if (pendingCpuDemodRows_ <= 0) return;
    renderCompositeOeCpu(pendingCpuDemodRows_);
    pendingCpuDemodRows_ = 0;
    // This runs after render() returned (pixels(), or MainWindow once it has
    // dropped stateMutex) and rewrites frame80 — the closing half of the
    // contract scheduleCpuDemodInto80() opened. The frames that arm it never
    // publish a key, so this is normally a no-op; stating it here keeps the
    // rule "the code that writes the framebuffer is the code that invalidates"
    // true of the deferred write as well as of the scheduling.
    framebufferMutated();
}

void Apple2Display::demodCompositeForCapture()
{
    if (hiResMode != HiResMode::ColorCompositeOE) return;
    if (!signalProducedFlag) return;
    // Frames where the GPU path itself presents the framebuffer already
    // have correct pixels(): mixed graphics+text (CPU demod band + patched
    // text rows, pendingCpuDemodRows_ set by render()) and sharp TEXT.
    if (mixedCompositeUsesFb_) return;
    if (lastRenderState_.textMode && oeDemod_.textSharp) return;
    // From here the capture owns the framebuffer: frame80 is about to hold a
    // demodulated image the on-screen (GPU-demodulated) frame never had, and
    // width() jumps to 560. scheduleCpuDemodInto80 drops the static-text key
    // with it, so the next render() rebuilds the display's own image instead
    // of skipping and leaving the capture's pixels up — a /screen taken on a
    // motionless TEXT screen used to soften it until the guest touched a byte.
    scheduleCpuDemodInto80(kSignalHeight);
}

void Apple2Display::renderCompositeOeCpu(int rows)
{
    constexpr float kPi = 3.14159265358979f;
    constexpr int   N = 8;
    const int   sw = kSignalWidth;                       // 560
    const int   sh = std::min(rows, kSignalHeight);     // ≤ 192 (per-row FIR)
    const uint8_t* sig = signalBuf.data();

    // OpenEmulator-exact 17-tap FIR kernels (Dolph-Chebyshev(50 dB) × sinc,
    // realIDFT recipe — see NtscPostProcessor.cpp for the GPU twin and the
    // libemulation provenance). lumaK: 2.0 MHz, sum 1, notches fs/4
    // (|H(0.25)| ≈ 0.002). chromaSoft: OE-faithful 0.6 MHz, sum 2 (the ×2
    // demod gain) — the neutral kernel at Sharpness 0.5. chromaSharp: the
    // 2.0 MHz kernel × 2, blended in over the upper half of the Sharpness
    // slider exactly like the GPU shader's mix(). Symmetric, [0]=centre.
    static const float lumaK[N + 1] = {
        0.27941f, 0.23593f, 0.13462f, 0.03665f, -0.01538f,
        -0.02210f, -0.00999f, -0.00072f, 0.00130f
    };
    static const float chromaSoft[N + 1] = {
        0.26030f, 0.24788f, 0.21373f, 0.16602f, 0.11509f,
        0.07008f, 0.03648f, 0.01543f, 0.00515f
    };
    static const float chromaSharp[N + 1] = {
        0.55882f, 0.47185f, 0.26923f, 0.07331f, -0.03077f,
        -0.04421f, -0.01999f, -0.00144f, 0.00259f
    };
    // Chroma-bandwidth sharpness: 0.5 = neutral (pure soft kernel); only the
    // upper half of the slider blends toward the sharper 2.0 MHz kernel —
    // same clamp + mix as the GPU shader.
    const float sharp =
        std::clamp((oeDemod_.sharpness - 0.5f) * 2.0f, 0.0f, 1.0f);
    float chromaK[N + 1];
    for (int a = 0; a <= N; ++a)
        chromaK[a] = chromaSoft[a] + (chromaSharp[a] - chromaSoft[a]) * sharp;
    // OpenEmulator demod: chroma = composite·(sin φ, cos φ) → U,V; YUV→RGB
    // matrix below. The subcarrier table is built at the four raw phases
    // π/2·k; signalPhaseOffset_ enters ONCE, at the index `k = (xi+po)&3`
    // below — matching MAME's rotl4(absX + offset) (apple2video.cpp DHGR
    // is_80_column → absX+1) and the AppleWin LUT path
    // (AppleWinNtsc.cpp renderLine: `lut[(x + phase) & 3]`). Baking the
    // offset into the table AND the index applied it twice (2·po) and
    // rotated every DHGR hue by 90° (N=1 demodulated green, not the MAME
    // dark blue). Probe-verified against the MAME-LUT render for the four
    // aligned nibble patterns (see dhgr_phase_signal_test).
    float sinP[4], cosP[4];
    for (int k = 0; k < 4; ++k) {
        const float ph = kPi * 0.5f * static_cast<float>(k);
        sinP[k] = std::sin(ph);
        cosP[k] = std::cos(ph);
    }
    // Optional hue rotation in the U/V plane (user knob) — same convention
    // as the GPU shader: h = uHue·π, applied AFTER the PAL V sign.
    const float hueCs = std::cos(oeDemod_.hue * kPi);
    const float hueSn = std::sin(oeDemod_.hue * kPi);

    for (int y = 0; y < sh; ++y) {
        const uint8_t* row = sig + static_cast<size_t>(y) * sw;
        uint32_t* outRow = frame80.data() + static_cast<size_t>(y) * kWidth80;
        // PAL line-phase alternation: the GPU shader multiplies every V tap
        // by ±1 per line; the sign is constant across a row, so applying it
        // to the accumulated V below is identical.
        const float vSign = (oeDemod_.palMode && (y & 1)) ? -1.0f : 1.0f;
        for (int x = 0; x < sw; ++x) {
            float Y = 0.0f, U = 0.0f, V = 0.0f;
            for (int i = -N; i <= N; ++i) {
                const int xi = x + i;
                if (xi < 0 || xi >= sw) continue;
                const float s = row[xi] ? 1.0f : 0.0f;
                const int   k = (xi + signalPhaseOffset_) & 3;
                const int   a = i < 0 ? -i : i;
                Y += s * lumaK[a];                // FIR luma (sum=1, notches fs/4)
                U += s * sinP[k] * chromaK[a];    // FIR chroma (sum=2 → ×2 gain)
                V += s * cosP[k] * chromaK[a];
            }
            V *= vSign;
            const float Ur = U * hueCs - V * hueSn;
            const float Vr = U * hueSn + V * hueCs;
            // YUV → RGB (OpenEmulator libemulation OpenGLCanvas.cpp).
            float r = Y                  + 1.139883f * Vr;
            float g = Y - 0.394642f * Ur - 0.580622f * Vr;
            float b = Y + 2.032062f * Ur;
            auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
            const uint32_t R = static_cast<uint32_t>(cl(r) * 255.0f + 0.5f);
            const uint32_t G = static_cast<uint32_t>(cl(g) * 255.0f + 0.5f);
            const uint32_t B = static_cast<uint32_t>(cl(b) * 255.0f + 0.5f);
            outRow[x] = (static_cast<uint32_t>(0xFF) << 24) | (B << 16) | (G << 8) | R;
        }
    }
}

// ─── Text mode ────────────────────────────────────────────────────────────

// Built-in 5×7 monospaced ASCII font, packed as 8 bytes per glyph (top→bottom).
// Bits 0-4 = pixel pattern, MSB-left: bit 4 is the leftmost dot, bit 0 the
// rightmost (glyphRows7's fallback reads `(row8 >> (5 - gx)) & 1`, and the
// glyph data is authored that way — e.g. '/' has 0x01 on its TOP row, the
// top-right dot). Only the printable
// range ($20-$7F) is fleshed out — control codes render as a checker pattern.
// Characters not commonly used by Apple II text output (lowercase) inherit
// from their uppercase counterparts; original Apple II only had uppercase
// anyway so this matches the visual.
static const uint8_t kAscii5x7[96 * 8] = {
    // 0x20 ' '
    0,0,0,0,0,0,0,0,
    // 0x21 '!'
    0x04,0x04,0x04,0x04,0x04,0x00,0x04,0x00,
    // 0x22 '"'
    0x0A,0x0A,0x0A,0,0,0,0,0,
    // 0x23 '#'
    0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A,0,
    // 0x24 '$'
    0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04,0,
    // 0x25 '%'
    0x19,0x19,0x02,0x04,0x08,0x13,0x13,0,
    // 0x26 '&'
    0x08,0x14,0x14,0x08,0x15,0x12,0x0D,0,
    // 0x27 '\''
    0x04,0x04,0x08,0,0,0,0,0,
    // 0x28 '('
    0x02,0x04,0x08,0x08,0x08,0x04,0x02,0,
    // 0x29 ')'
    0x08,0x04,0x02,0x02,0x02,0x04,0x08,0,
    // 0x2A '*'
    0x00,0x04,0x15,0x0E,0x15,0x04,0x00,0,
    // 0x2B '+'
    0x00,0x04,0x04,0x1F,0x04,0x04,0x00,0,
    // 0x2C ','
    0,0,0,0,0,0x04,0x04,0x08,
    // 0x2D '-'
    0x00,0x00,0x00,0x1F,0x00,0x00,0x00,0,
    // 0x2E '.'
    0,0,0,0,0,0x0C,0x0C,0,
    // 0x2F '/'
    0x01,0x01,0x02,0x04,0x08,0x10,0x10,0,
    // 0x30 '0'
    0x0E,0x11,0x13,0x15,0x19,0x11,0x0E,0,
    // 0x31 '1'
    0x04,0x0C,0x04,0x04,0x04,0x04,0x0E,0,
    // 0x32 '2'
    0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,0,
    // 0x33 '3'
    0x0E,0x11,0x01,0x06,0x01,0x11,0x0E,0,
    // 0x34 '4'
    0x02,0x06,0x0A,0x12,0x1F,0x02,0x02,0,
    // 0x35 '5'
    0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,0,
    // 0x36 '6'
    0x06,0x08,0x10,0x1E,0x11,0x11,0x0E,0,
    // 0x37 '7'
    0x1F,0x01,0x02,0x04,0x08,0x08,0x08,0,
    // 0x38 '8'
    0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,0,
    // 0x39 '9'
    0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C,0,
    // 0x3A ':'
    0,0,0x0C,0x0C,0,0x0C,0x0C,0,
    // 0x3B ';'
    0,0,0x0C,0x0C,0,0x0C,0x04,0x08,
    // 0x3C '<'
    0x02,0x04,0x08,0x10,0x08,0x04,0x02,0,
    // 0x3D '='
    0,0,0x1F,0,0x1F,0,0,0,
    // 0x3E '>'
    0x08,0x04,0x02,0x01,0x02,0x04,0x08,0,
    // 0x3F '?'
    0x0E,0x11,0x01,0x02,0x04,0x00,0x04,0,
    // 0x40 '@'
    0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E,0,
    // 0x41 'A'
    0x0E,0x11,0x11,0x11,0x1F,0x11,0x11,0,
    // 0x42 'B'
    0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E,0,
    // 0x43 'C'
    0x0E,0x11,0x10,0x10,0x10,0x11,0x0E,0,
    // 0x44 'D'
    0x1C,0x12,0x11,0x11,0x11,0x12,0x1C,0,
    // 0x45 'E'
    0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F,0,
    // 0x46 'F'
    0x1F,0x10,0x10,0x1E,0x10,0x10,0x10,0,
    // 0x47 'G'
    0x0E,0x11,0x10,0x17,0x11,0x11,0x0F,0,
    // 0x48 'H'
    0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0,
    // 0x49 'I'
    0x0E,0x04,0x04,0x04,0x04,0x04,0x0E,0,
    // 0x4A 'J'
    0x07,0x02,0x02,0x02,0x02,0x12,0x0C,0,
    // 0x4B 'K'
    0x11,0x12,0x14,0x18,0x14,0x12,0x11,0,
    // 0x4C 'L'
    0x10,0x10,0x10,0x10,0x10,0x10,0x1F,0,
    // 0x4D 'M'
    0x11,0x1B,0x15,0x15,0x11,0x11,0x11,0,
    // 0x4E 'N'
    0x11,0x11,0x19,0x15,0x13,0x11,0x11,0,
    // 0x4F 'O'
    0x0E,0x11,0x11,0x11,0x11,0x11,0x0E,0,
    // 0x50 'P'
    0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,0,
    // 0x51 'Q'
    0x0E,0x11,0x11,0x11,0x15,0x12,0x0D,0,
    // 0x52 'R'
    0x1E,0x11,0x11,0x1E,0x14,0x12,0x11,0,
    // 0x53 'S'
    0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E,0,
    // 0x54 'T'
    0x1F,0x04,0x04,0x04,0x04,0x04,0x04,0,
    // 0x55 'U'
    0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0,
    // 0x56 'V'
    0x11,0x11,0x11,0x11,0x11,0x0A,0x04,0,
    // 0x57 'W'
    0x11,0x11,0x11,0x15,0x15,0x15,0x0A,0,
    // 0x58 'X'
    0x11,0x11,0x0A,0x04,0x0A,0x11,0x11,0,
    // 0x59 'Y'
    0x11,0x11,0x11,0x0A,0x04,0x04,0x04,0,
    // 0x5A 'Z'
    0x1F,0x01,0x02,0x04,0x08,0x10,0x1F,0,
    // 0x5B '['
    0x0E,0x08,0x08,0x08,0x08,0x08,0x0E,0,
    // 0x5C '\\'
    0x10,0x10,0x08,0x04,0x02,0x01,0x01,0,
    // 0x5D ']'
    0x0E,0x02,0x02,0x02,0x02,0x02,0x0E,0,
    // 0x5E '^'
    0x04,0x0A,0x11,0,0,0,0,0,
    // 0x5F '_'
    0,0,0,0,0,0,0x1F,0,
    // 0x60 '`'
    0x08,0x04,0x02,0,0,0,0,0,
    // The lowercase range $61-$7A isn't backed by per-glyph patterns yet;
    // resolveGlyph() falls back to the uppercase glyph when a //e program
    // writes a lowercase byte. Drop a real //e character ROM into
    // roms/apple2_char.rom for accurate lowercase rendering.
};

// Map a screen byte to a glyph row pattern + video attributes.
//   $00-$3F  inverse   ─ low 6 bits = char index (always inverse)
//   $40-$7F  flashing  ─ low 6 bits = char index (alternates inverse/normal
//                        at ~2 Hz — drives the Monitor cursor blink and
//                        any inverse-blinking spaces left behind by
//                        Applesoft when it moves to a new line). On IIe
//                        with ALTCHAR=on, this range becomes mousetext
//                        (non-flashing, glyph from second 2 KB ROM bank).
//   $80-$FF  normal    ─ low 7 bits = ASCII (//e exposes lowercase here)
static void resolveGlyph(uint8_t screenByte, uint8_t out[8],
                         bool& invert, bool& flash)
{
    uint8_t ascii;
    flash = false;
    if (screenByte & 0x80) {
        invert = false;
        ascii  = screenByte & 0x7F;
    } else {
        invert = true;
        flash  = (screenByte & 0x40) != 0;   // bit 6 set → FLASH attribute
        const uint8_t idx6 = screenByte & 0x3F;
        ascii = (idx6 < 0x20) ? static_cast<uint8_t>(0x40 + idx6) : idx6;
    }

    // Lowercase fallback to uppercase while no character ROM is loaded.
    if (ascii >= 0x61 && ascii <= 0x7A) ascii = static_cast<uint8_t>(ascii - 0x20);

    // kAscii5x7 spans 0x20-0x7F (96 entries). Entries 0x61-0x7F are
    // zero-filled today (lowercase + a few punct glyphs not authored
    // yet); rendering them as blank cells is still preferable to the
    // box placeholder for chars that real Apple II text would draw.
    if (ascii >= 0x20 && ascii <= 0x7F) {
        std::memcpy(out, &kAscii5x7[(ascii - 0x20) * 8], 8);
    } else {
        const uint8_t box[8] = { 0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F, 0 };
        std::memcpy(out, box, 8);
    }
}

// Char-ROM-backed glyph resolver. The ROM has been pre-processed at
// load time (`Memory::loadCharRom`) into AppleWin-style csbits format:
//
//   * Each byte = one displayed row of 7 pixels.
//   * Bit 0 = leftmost pixel, bit 6 = rightmost; 1 = lit pixel.
//   * Codes $00-$3F (inverse range) are pre-flipped to look like inverse
//     video already (white BG with dark glyph). Codes $80-$FF (normal)
//     are stored as normal video (dark BG with white glyph). Codes
//     $40-$7F (flashing) hold the normal-looking pattern; the renderer
//     XORs with 0x7F when the flash phase is on.
//   * IIe ALTCHAR additions (4 KB ROM only): codes $40-$5F look up the
//     second 2 KB bank (mousetext glyphs); codes $60-$7F display the
//     lowercase glyph from $E0-$FF as inverse video.
//   * 2 KB ROMs (II/II+) have no lowercase: codes $61-$7A and $E1-$FA
//     are remapped to their uppercase equivalents (clear bit 5).
//
// The renderer reads each row's 7 pixels by `(row >> i) & 1` for i=0..6
// after the optional flash XOR.
struct GlyphLookup {
    uint8_t bytes[8];
    bool    flash = false;
};

static GlyphLookup lookupCsbitsGlyph(uint8_t screenByte,
                                     const uint8_t* charRom,
                                     std::size_t charRomSize,
                                     bool charRomLower,
                                     bool altCharSet)
{
    GlyphLookup g{};
    if (charRomSize < 2048) { return g; }

    uint8_t mapped = screenByte;

    // Lowercase fallback: map a-z to A-Z by clearing bit 5, as the IIe
    // firmware does. Gated on what the ROM CONTAINS, not its size — the
    // Videx LOWER CASE CHIP is 2 KB and HAS lowercase (see CharRomDump.h).
    if (!charRomLower) {
        const uint8_t ascii = mapped & 0x7F;
        if (ascii >= 0x61 && ascii <= 0x7A) {
            mapped = static_cast<uint8_t>((mapped & 0x80) | (ascii - 0x20));
        }
    }

    // Code-range routing (mirrors MAME's IIe `get_text_character`):
    //
    //   ALTCHAR off (II/II+ behaviour, IIe boot default):
    //     $00-$3F  inverse (always)
    //     $40-$7F  flashing — remap to $00-$3F and toggle invert at the
    //              flash phase (renderer does the XOR per row).
    //     $80-$FF  normal
    //
    //   ALTCHAR on (IIe-only, requires 4 KB ROM):
    //     $00-$3F  inverse (same as above)
    //     $40-$5F  mousetext — keep code as-is so the lookup hits the
    //              4 KB ROM's mousetext slot at offsets $200-$2FF.
    //              Non-flashing inverse-style display.
    //     $60-$7F  lowercase inverse — remap to $E0-$FF (= lowercase
    //              normal range) and force display-time invert so the
    //              user sees lowercase on a bright background.
    //     $80-$FF  normal (lowercase included on a 4 KB ROM)
    std::size_t code = mapped;
    bool extraInvert = false;

    if (altCharSet && charRomSize >= 4096) {
        if (mapped >= 0x40 && mapped <= 0x5F) {
            // Mousetext: csbits hold the closed-apple / heart / etc.
            // glyphs at this offset (the 4 KB ROM repurposes the
            // flashing slot for mousetext).
            // Leave `code = mapped` (i.e. $40-$5F).
        } else if (mapped >= 0x60 && mapped <= 0x7F) {
            code = static_cast<std::size_t>(mapped | 0x80);
            extraInvert = true;
        }
    } else if (mapped >= 0x40 && mapped <= 0x7F) {
        // Flashing — remap to inverse range so the LOOKUP hits the
        // inverse glyph; flash flag drives the per-row XOR at render.
        code = static_cast<std::size_t>(mapped & 0x3F);
        g.flash = true;
    }

    const std::size_t off = code * 8;
    for (int i = 0; i < 8; ++i) {
        g.bytes[i] = (off + i < charRomSize) ? charRom[off + i] & 0x7Fu : 0;
    }
    if (extraInvert) {
        for (int i = 0; i < 8; ++i) g.bytes[i] ^= 0x7Fu;
    }
    return g;
}

// Unified glyph-row resolver shared by every text painter (40/80-col
// framebuffer, Chat Mauve fg/bg colour text, and the composite-signal
// generator). Returns the cell's 8 rows as 7-bit lit masks: bit `gx`
// (0 = leftmost) set ⇔ pixel gx is lit, with inverse + flash already
// applied. Both the char-ROM (csbits) path and the built-in 5×7 fallback
// collapse onto this one representation, so each call site is reduced to a
// plain `(rows[gy] >> gx) & 1` read instead of re-deriving the glyph bits.
static std::array<uint8_t, 8> glyphRows7(uint8_t screenByte,
                                         const uint8_t* charRom,
                                         std::size_t charRomSize,
                                         bool charRomLower,
                                         bool useCharRom, bool altCharSet,
                                         bool flashPhase)
{
    std::array<uint8_t, 8> rows{};
    if (useCharRom) {
        const auto g = lookupCsbitsGlyph(screenByte, charRom, charRomSize, charRomLower, altCharSet);
        for (int gy = 0; gy < 8; ++gy) {
            uint8_t bits = g.bytes[gy];
            if (g.flash && flashPhase) bits ^= 0x7Fu;
            rows[gy] = bits & 0x7Fu;
        }
    } else {
        uint8_t glyph[8];
        bool invert = false, flash = false;
        resolveGlyph(screenByte, glyph, invert, flash);
        if (flash && flashPhase) invert = !invert;
        for (int gy = 0; gy < 8; ++gy) {
            const uint8_t row8 = glyph[gy];
            uint8_t bits = 0;
            for (int gx = 0; gx < 7; ++gx) {
                bool lit = (gx >= 1 && gx <= 5) && ((row8 >> (5 - gx)) & 1);
                if (invert) lit = !lit;
                if (lit) bits |= static_cast<uint8_t>(1u << gx);
            }
            rows[gy] = bits;
        }
    }
    return rows;
}

void Apple2Display::renderText(Memory& mem, const Memory::DisplayState& state,
                               int firstRow, int lastRow, int col0, int col1,
                               int clipY0, int clipY1)
{
    col0 = std::max(0, col0);
    col1 = std::min(40, col1);
    if (col0 >= col1) return;
    // IIe scanner routing for text/lo-res page 1 ($0400-$07FF): when
    // 40-column text always displays MAIN page 1; the //e video scanner only
    // multiplexes aux RAM in 80-column mode (renderText80) — NOT here. The
    // page-1/page-2 base is already chosen by videoTextPage2() = page2 &&
    // !80store (MAME use_page_2()); reading that base from aux when
    // 80STORE+PAGE2 was a bug that showed aux garbage for 40-col programs
    // page-flipping via 80STORE (MAME apple2video.cpp text_update reads only
    // m_ram_ptr in 40-col).
    const uint8_t* ram = mem.data();

    // Flash phase: 0 = invert as-stored, 1 = flip back to normal. Toggles
    // every kFlashHalfPeriodFrames (16) frames → 32-frame cycle ≈ 1.875 Hz,
    // matching MAME IIe's `frame_number() & 0x10` and AppleWin's
    // `(++counter & 0xF)==0`.
    const bool flashPhase = (frameCounter / kFlashHalfPeriodFrames) & 1u;

    // Char ROM path: when a real character ROM is loaded, render each
    // cell as 7 actual pixels from the ROM (bit 0 = leftmost). 2 KB ROM
    // = II/II+ standard (no mousetext); 4 KB+ = IIe (second bank holds
    // mousetext glyphs, used when ALTCHAR=on).
    const bool useCharRom  = mem.charRomActiveSize() >= 2048;
    const bool altCharSet  = state.altChar;

    for (int row = firstRow; row < lastRow; ++row) {
        const uint16_t rowAddr = textRowAddress(row, videoTextPage2(state));
        for (int col = col0; col < col1; ++col) {
            const uint8_t src = ram[rowAddr + col];
            const int cellX = col * 7;
            const int cellY = row * 8;

            const auto rows = glyphRows7(src, mem.charRomActiveData(), mem.charRomActiveSize(), mem.charRomHasLowercase(),
                                         useCharRom, altCharSet, flashPhase);
            for (int gy = 0; gy < 8; ++gy) {
                const int y = cellY + gy;
                if (y < clipY0 || y >= clipY1) continue;   // beam-split clip
                for (int gx = 0; gx < 7; ++gx)
                    frame[y * kWidth + (cellX + gx)] =
                        ((rows[gy] >> gx) & 1u) ? 0xFFFFFFFFu : 0xFF000000u;
            }
        }
    }
}

// Le Chat Mauve / Video-7 "foreground-background" colored TEXT mode.
// Mirror of renderText's glyph generation, but each cell is painted at
// 560-dot density into `frame80` with per-cell colours pulled from aux RAM.
// MAME `apple2video.cpp` text_update :788-791 selects this path on
//   (IIE||PRAVETZ_8C) && rgb_monitor() && m_dhires && !m_80col
// and render_line_color_array :571-583 does the colouring: the 7-bit glyph
// is doubled to 14 dots; a set dot picks the aux byte's high nibble
// (foreground), a clear dot the low nibble (background) — both lo-res
// palette indices.
void Apple2Display::renderTextChatMauveFgBg(Memory& mem,
                                            const Memory::DisplayState& state,
                                            int firstRow, int lastRow,
                                            int clipY0, int clipY1,
                                            bool auxHiIsForeground)
{
    const uint8_t* ram = mem.data();
    const uint8_t* aux = auxRam ? auxRam : ram;
    const bool flashPhase = (frameCounter / kFlashHalfPeriodFrames) & 1u;

    const bool  useCharRom = mem.charRomActiveSize() >= 2048;
    const bool  altCharSet = state.altChar;

    for (int row = firstRow; row < lastRow; ++row) {
        const uint16_t rowAddr = textRowAddress(row, videoTextPage2(state));
        const int cellY = row * 8;
        for (int col = 0; col < 40; ++col) {
            // Char code from main RAM; fg/bg attribute from aux at the same
            // text address (MAME: char = m_ram_ptr[address], colours =
            // aux_page[aux_address]).
            const uint8_t src    = ram[rowAddr + col];
            const uint8_t attr   = aux[rowAddr + col];
            const uint32_t hi = kChatMauveLoResPalette[(attr >> 4) & 0x0Fu];
            const uint32_t lo = kChatMauveLoResPalette[attr & 0x0Fu];
            const uint32_t fg = auxHiIsForeground ? hi : lo;   // Video-7 : Eve TXT16
            const uint32_t bg = auxHiIsForeground ? lo : hi;

            // Resolve the glyph into a uniform 7-bit row (bit i = pixel i,
            // bit 0 = leftmost, 1 = lit) with invert/flash already applied,
            // so the 14-dot widening below is shared by both font paths.
            const auto glyphRows = glyphRows7(src, mem.charRomActiveData(), mem.charRomActiveSize(), mem.charRomHasLowercase(),
                                              useCharRom, altCharSet, flashPhase);

            for (int gy = 0; gy < 8; ++gy) {
                const int y = cellY + gy;
                if (y < clipY0 || y >= clipY1) continue;   // beam-split clip
                const uint8_t bits = glyphRows[gy];
                uint32_t* outRow = frame80.data()
                    + static_cast<size_t>(y) * kWidth80;
                // Double each glyph pixel to two dots (MAME double_7_bits).
                for (int d = 0; d < 14; ++d)
                    outRow[col * 14 + d] = ((bits >> (d >> 1)) & 1u) ? fg : bg;
            }
        }
    }
}

// ─── Lo-res mode ──────────────────────────────────────────────────────────

// Palette verbatim from MAME `apple2video.cpp::apple2_palette[]` — the
// reference sRGB values calibrated against real Apple II hardware. Same
// 16 indices drive both lo-res blocks and the artefact LUT in
// renderHiRes() (the LUT looks up these very entries).
const uint32_t Apple2Display::kLoResPalette[16] = {
    0xFF000000, //  0 Black
    0xFF400BA7, //  1 Dark Red       rgb(0xa7, 0x0b, 0x40)
    0xFFF71C40, //  2 Dark Blue      rgb(0x40, 0x1c, 0xf7)
    0xFFFF28E6, //  3 Purple         rgb(0xe6, 0x28, 0xff)
    0xFF407400, //  4 Dark Green     rgb(0x00, 0x74, 0x40)
    0xFF808080, //  5 Dark Gray      rgb(0x80, 0x80, 0x80)
    0xFFFF9019, //  6 Medium Blue    rgb(0x19, 0x90, 0xff)
    0xFFFF9CBF, //  7 Light Blue     rgb(0xbf, 0x9c, 0xff)
    0xFF006340, //  8 Brown          rgb(0x40, 0x63, 0x00)
    0xFF006FE6, //  9 Orange         rgb(0xe6, 0x6f, 0x00)
    0xFF808080, // 10 Light Gray     rgb(0x80, 0x80, 0x80)  ← same as 5 in NTSC
    0xFFBF8BFF, // 11 Pink           rgb(0xff, 0x8b, 0xbf)
    0xFF00D719, // 12 Light Green    rgb(0x19, 0xd7, 0x00)
    0xFF08E3BF, // 13 Yellow         rgb(0xbf, 0xe3, 0x08)
    0xFFBFF458, // 14 Aquamarine     rgb(0x58, 0xf4, 0xbf)
    0xFFFFFFFF, // 15 White
};

// Le Chat Mauve / Video-7 lo-res palette. Lo-res is where the
// "two distinct grays" Chat Mauve trademark actually shows up on
// standard Apple II — because lo-res indexes its 16 colours directly
// from a 4-bit nibble in screen RAM, no chroma decoding involved. NTSC
// composite collapses indices 5 and 10 onto the same grey because
// their phase signatures cancel through the chroma filter, but the
// Chat Mauve digital RGB decoder produces two visibly distinct tinted
// grays (5 = olive-ish, 10 = mauve-ish) — *that* is the trademark.
//
// Values verbatim from AppleWin `RGBMonitor.cpp::PaletteRGB_Feline`
// (commit ec3b03c, source-of-truth for Apple II RGB decoder emulation;
// upstream tag "Feline" = the Le Chat Mauve "Feline" board, the most
// commonly emulated variant). Per AppleWin's own comment block on the
// table: "extracted from a white-balanced RGB video capture" of a real
// card — so these are empirical pixel values, not a synthetic palette
// choice. MAME has no separate Chat Mauve palette (its Video-7 RGB
// mode reuses the standard `apple2_palette[]`, which collapses the two
// grays); we follow AppleWin instead because the whole point of
// modelling Le Chat Mauve is the two-grays trademark.
//
// Stored as ABGR-in-uint32 (R = lowest byte) to match `kLoResPalette`.
const uint32_t Apple2Display::kChatMauveLoResPalette[16] = {
    0xFF000000, //  0 Black
    0xFF4C12AC, //  1 Deep Red       rgb(0xac, 0x12, 0x4c)
    0xFF830700, //  2 Dark Blue      rgb(0x00, 0x07, 0x83)
    0xFFD11AAA, //  3 Magenta        rgb(0xaa, 0x1a, 0xd1)
    0xFF2F8300, //  4 Dark Green     rgb(0x00, 0x83, 0x2f)
    0xFF7E979F, //  5 Dark Gray      rgb(0x9f, 0x97, 0x7e) ← Feline gray #1 (olive tint)
    0xFFB58A00, //  6 Medium Blue    rgb(0x00, 0x8a, 0xb5)
    0xFFFF9E9F, //  7 Light Blue     rgb(0x9f, 0x9e, 0xff)
    0xFF005F7A, //  8 Brown          rgb(0x7a, 0x5f, 0x00)
    0xFF4772FF, //  9 Orange         rgb(0xff, 0x72, 0x47)
    0xFF7F6878, // 10 Light Gray     rgb(0x78, 0x68, 0x7f) ← Feline gray #2 (mauve tint)
    0xFFCF7AFF, // 11 Pink           rgb(0xff, 0x7a, 0xcf)
    0xFF2CE66F, // 12 Light Green    rgb(0x6f, 0xe6, 0x2c)
    0xFF7BF6FF, // 13 Yellow         rgb(0xff, 0xf6, 0x7b)
    0xFFB2EE6C, // 14 Aquamarine     rgb(0x6c, 0xee, 0xb2)
    0xFFFFFFFF, // 15 White
};

namespace {

// Phosphor for the monochrome modes. RGB is the fully-lit colour
// (luminance 1.0); decay is the per-frame multiplier on the history buffer
// (0.0 = no afterglow, 1.0 = freeze). Selected by an explicit switch on the
// mode (phosphorFor), NOT by indexing a table with the HiResMode enum's
// integer value — the old table forced every new enumerator to be appended
// at the end to stay aligned (a fragile, silent coupling). Non-mono modes
// return the white reference tint as a harmless default; only the mono
// paths actually consult it. This is the seed of the Phase-2 "tint" effect
// layer: phosphor becomes an axis independent of the colour decoder.
// (Lives above the lo-res painters since 2026-07-12 — their mono branches
// consult it too.)
struct Phosphor { uint8_t r, g, b; float decay; };
inline constexpr Phosphor kPhosphorWhite = { 0xFF, 0xFF, 0xFF, 0.00f };
inline constexpr Phosphor kPhosphorGreen = { 0x33, 0xFF, 0x33, 0.85f }; // P31 (CIE x=0.280, y=0.595)
inline constexpr Phosphor kPhosphorAmber = { 0xFF, 0xB0, 0x00, 0.96f }; // long persistence

inline Phosphor phosphorFor(Apple2Display::HiResMode m)
{
    switch (m) {
        case Apple2Display::HiResMode::MonoGreen: return kPhosphorGreen;
        case Apple2Display::HiResMode::MonoAmber: return kPhosphorAmber;
        default:                                  return kPhosphorWhite;
    }
}

} // namespace

void Apple2Display::renderLoRes(Memory& mem, const Memory::DisplayState& state,
                                int firstRow, int lastRow, int col0, int col1,
                                int clipY0, int clipY1)
{
    col0 = std::max(0, col0);
    col1 = std::min(40, col1);
    if (col0 >= col1) return;
    // Lo-res draws 40 columns × 48 rows of 7×4 colour blocks. Each text
    // byte stores TWO blocks: low nibble is the upper block, high nibble
    // the lower one.
    // Lo-res always displays MAIN page 1 — the scanner only reads aux in
    // 80-column/double modes (see renderText). page2 base via
    // videoTextPage2(). (Reading aux under 80STORE+PAGE2 was a bug.)
    const uint8_t* ram = mem.data();

    // Palette selection. ChatMauveRGB swaps in the 16-colour Péritel
    // table — same indices, but indices 5 and 10 are now visibly distinct
    // grays (where the NTSC //gs-corrected default merges them onto a
    // single neutral). The "Chat Mauve trademark" actually shows up here,
    // not in HGR.
    const bool useChatMauve = (hiResMode == HiResMode::ChatMauveRGB) && (chatMauve != nullptr);
    const uint32_t* palette = useChatMauve ? kChatMauveLoResPalette : kLoResPalette;

    // Monochrome: a lo-res block on a mono monitor is NOT a grey — the colour
    // nibble keeps cycling on the 14.318 MHz dot clock, so it displays as its
    // repeating 4-bit pattern (vertical stripes; nibble 0 black, 15 white,
    // greys 5/10 fine 50% stripes). Same serialisation as the composite
    // signal path (fillCompositeSignal paintLoRes40: bit = nibble >>
    // (absSample & 3)); each 280-wide pixel covers two 14 MHz samples, so its
    // luminance is the average of its sample pair, then the standard
    // max(target, prev × decay) phosphor rule (persistenceL, like renderHiRes).
    const bool monochrome = (hiResMode == HiResMode::MonoWhite ||
                             hiResMode == HiResMode::MonoGreen ||
                             hiResMode == HiResMode::MonoAmber);
    const Phosphor phos = phosphorFor(hiResMode);

    // Each lo-res row corresponds to half a text row (4 scanlines).
    for (int blockRow = firstRow; blockRow < lastRow; ++blockRow) {
        const int textRow = blockRow / 2;
        const bool upperHalf = (blockRow % 2 == 0);
        const uint16_t rowAddr = textRowAddress(textRow, videoTextPage2(state));
        for (int col = col0; col < col1; ++col) {
            const uint8_t b = ram[rowAddr + col];
            const uint8_t nibble = upperHalf ? (b & 0x0F) : (b >> 4);
            const uint32_t rgb = palette[nibble];
            const int x0 = col * 7;
            const int y0 = blockRow * 4;
            for (int dy = 0; dy < 4; ++dy) {
                const int y = y0 + dy;
                if (y < clipY0 || y >= clipY1) continue;   // beam-split clip
                if (monochrome) {
                    uint8_t* histRow = persistenceL.data()
                                     + static_cast<size_t>(y) * kWidth;
                    for (int dx = 0; dx < 7; ++dx) {
                        const int px = x0 + dx;
                        const int s0 = (nibble >> ((px * 2)     & 3)) & 1;
                        const int s1 = (nibble >> ((px * 2 + 1) & 3)) & 1;
                        const int target = ((s0 + s1) * 255) / 2;   // 0 / 127 / 255
                        const int prev = static_cast<int>(
                            static_cast<float>(histRow[px]) * phos.decay);
                        const int merged = std::max(target, prev);
                        histRow[px] = static_cast<uint8_t>(merged);
                        const uint32_t r = (static_cast<uint32_t>(phos.r) * merged + 127) / 255;
                        const uint32_t g = (static_cast<uint32_t>(phos.g) * merged + 127) / 255;
                        const uint32_t bl = (static_cast<uint32_t>(phos.b) * merged + 127) / 255;
                        frame[y * kWidth + px] =
                            (uint32_t(0xFF) << 24) | (bl << 16) | (g << 8) | r;
                    }
                    continue;
                }
                for (int dx = 0; dx < 7; ++dx)
                    frame[y * kWidth + (x0 + dx)] = rgb;
            }
        }
    }
}

// ─── Double lo-res (DLGR) ─────────────────────────────────────────────────
//
// 80-column lo-res: aux RAM supplies the EVEN 7-dot half of each column (its
// nibble rotated left 1) and main RAM the ODD half — MAME apple2video.cpp
// `lores_update<Double>` (`rotl4(NIBBLE(aux),1)` then `NIBBLE(main)`). Output
// is the 560-wide frame80. Marginal //e mode (rare demos/utilities); without
// this the //e fell back to a plausible 40-col lo-res from main RAM only.
void Apple2Display::renderLoResDouble(Memory& mem,
                                      const Memory::DisplayState& state,
                                      int firstRow, int lastRow,
                                      int clipY0, int clipY1)
{
    const uint8_t* ram = mem.data();
    const uint8_t* aux = auxRam ? auxRam : ram;   // fall back to main if no aux
    const bool useChatMauve = (hiResMode == HiResMode::ChatMauveRGB) && (chatMauve != nullptr);
    const uint32_t* palette = useChatMauve ? kChatMauveLoResPalette : kLoResPalette;
    auto rotl4 = [](uint8_t n) -> uint8_t {
        return static_cast<uint8_t>(((n << 1) | (n >> 3)) & 0x0F);
    };

    // Monochrome: like single lo-res, each nibble displays as its repeating
    // 4-bit pattern at the 14.318 MHz dot clock — one 560-wide dot per sample,
    // pattern indexed at the ABSOLUTE sample position (same rule as
    // fillCompositeSignal's paintLoResDouble; the pattern generator is locked
    // to the subcarrier, not restarted per 7-dot half-cell). Aux pattern is
    // the rotl4'd nibble. Phosphor history in persistenceL80 (560-wide).
    const bool monochrome = (hiResMode == HiResMode::MonoWhite ||
                             hiResMode == HiResMode::MonoGreen ||
                             hiResMode == HiResMode::MonoAmber);
    const Phosphor phos = phosphorFor(hiResMode);

    for (int blockRow = firstRow; blockRow < lastRow; ++blockRow) {
        const int  textRow   = blockRow / 2;
        const bool upperHalf = (blockRow % 2 == 0);
        const uint16_t rowAddr = textRowAddress(textRow, videoTextPage2(state));
        for (int col = 0; col < 40; ++col) {
            const uint8_t mb = ram[rowAddr + col];
            const uint8_t ab = aux[rowAddr + col];
            const uint8_t mNib = upperHalf ? (mb & 0x0F) : (mb >> 4);
            const uint8_t aNib = upperHalf ? (ab & 0x0F) : (ab >> 4);
            const uint32_t auxRgb  = palette[rotl4(aNib)];
            const uint32_t mainRgb = palette[mNib];
            const int x0 = col * 14;
            const int y0 = blockRow * 4;
            for (int dy = 0; dy < 4; ++dy) {
                const int y = y0 + dy;
                if (y < clipY0 || y >= clipY1) continue;   // beam-split clip
                uint32_t* row = frame80.data()
                              + static_cast<size_t>(y) * kWidth80 + x0;
                if (monochrome) {
                    const uint8_t auxPat  = rotl4(aNib);
                    uint8_t* histRow = persistenceL80.data()
                                     + static_cast<size_t>(y) * kWidth80 + x0;
                    for (int dx = 0; dx < 14; ++dx) {
                        const uint8_t pat = (dx < 7) ? auxPat : mNib;
                        const int bit = (pat >> ((x0 + dx) & 3)) & 1;
                        const int target = bit ? 255 : 0;
                        const int prev = static_cast<int>(
                            static_cast<float>(histRow[dx]) * phos.decay);
                        const int merged = std::max(target, prev);
                        histRow[dx] = static_cast<uint8_t>(merged);
                        const uint32_t r = (static_cast<uint32_t>(phos.r) * merged + 127) / 255;
                        const uint32_t g = (static_cast<uint32_t>(phos.g) * merged + 127) / 255;
                        const uint32_t bl = (static_cast<uint32_t>(phos.b) * merged + 127) / 255;
                        row[dx] = (uint32_t(0xFF) << 24) | (bl << 16) | (g << 8) | r;
                    }
                    continue;
                }
                for (int dx = 0; dx < 7; ++dx) row[dx]     = auxRgb;
                for (int dx = 0; dx < 7; ++dx) row[7 + dx] = mainRgb;
            }
        }
    }
}

// ─── Hi-res mode ──────────────────────────────────────────────────────────
//
// Colour decode follows MAME's `apple2video.cpp` (PR #10773 by benrg) —
// the gold-standard algorithm calibrated against real Apple II hardware.
// Three building blocks:
//
//   1. **Bit doubler.** Each of the 7 visible HGR bits is duplicated to
//      give a 14-bit word per byte (40 bytes × 14 = 560 sub-pixels per
//      scanline at the master 14.32 MHz cadence).
//   2. **Half-dot delay (MSB).** When a byte's bit 7 is set, the entire
//      14-bit word is shifted left by 1 sub-pixel, with the *top* bit
//      of the previous byte's word feeding bit 0. That single-cell
//      shift is the 74LS74 flip-flop delay (~70 ns / 90° chroma phase)
//      that real silicon implements. Because the delay lives in the
//      stream, fringing at MSB-toggle byte boundaries falls out for
//      free.
//   3. **7-bit sliding window + 4-phase rotation.** A 7-bit window walks
//      the 14-bit-per-byte stream with 3 bits of left context. For each
//      sub-pixel position the window indexes a 128-entry static LUT
//      (verbatim from MAME); the LUT entry is a byte that packs four
//      4-bit "lo-res palette index" candidates, one per NTSC phase.
//      `rotl4b(byte, x)` extracts the candidate matching the current
//      absolute sub-pixel x mod 4. The 4-bit result is the lo-res
//      palette index — the artefact colour drops out of the same
//      16-colour table that drives `renderLoRes()`.
//
// Output is at 560 sub-pixels per scanline; we average pairs into 280
// framebuffer pixels (the chroma-bandwidth-limited downsample real
// CRTs perform optically).
//
// Monochrome paths reuse the doubled bit stream but skip the LUT and
// rotation — luminance only, multiplied by a phosphor tint. Persistence
// for amber rides on a per-pixel history × decay buffer.
//
// Convention: bit 0 of an HGR byte is the LEFTMOST pixel, bit 6 the
// RIGHTMOST, bit 7 the per-byte half-dot delay flag.

namespace {

// NTSC artifact-decode primitives (kStreamLen, kBitDoubler,
// kArtifactColorLut, rotl4b, buildHgrWordRow, buildBitStream, avgRgb) now
// live in Apple2VideoDecode.h (namespace pom2::a2v) — shared verbatim with
// the DHGR and composite-signal paths. They're in scope here via the
// file-level `using namespace pom2::a2v;` near the top of this file.

// (Phosphor + phosphorFor moved above renderLoRes — the lo-res mono paths
// need them too since 2026-07-12.)

} // namespace

void Apple2Display::renderHiRes(Memory& mem, const Memory::DisplayState& state,
                                int firstScanline, int lastScanline,
                                int writeCol0, int writeCol1)
{
    // Column window in 280-wide framebuffer pixels (each byte = 7 px). The
    // scanline is always decoded in full so the NTSC artifact sliding window
    // keeps its neighbour-byte context across the split; only the write-back
    // (and the mono persistence history) is clipped to [px0, px1). Default
    // (0, 40) → px0=0, px1=280, byte-identical to the pre-split behaviour.
    const int px0 = std::clamp(writeCol0, 0, 40) * 7;
    const int px1 = std::clamp(writeCol1, 0, 40) * 7;
    if (px0 >= px1) return;
    // Single hi-res always displays MAIN page 1. Aux HGR ($2000-$3FFF) is
    // only shown via DHGR (80COL+DHIRES, renderDhgr) — with 80COL off the
    // scanner never reads aux, so reading it under 80STORE+HIRES+PAGE2 was a
    // bug (showed aux garbage for single-HGR 80STORE page-flipping). page2
    // base via videoHgrPage2(). MAME hgr_update reads only m_ram_ptr.
    const uint8_t* ram = mem.data();

    // IIe DHIRES annunciator on + 80COL off = rev-0 emulation: mask
    // bit 7 of every HGR byte so no half-dot delay / no orange-blue
    // palette. MAME `apple2video.cpp:747`: `bit7_mask = m_dhires ? 0 :
    // 0x80`. POM2's DHGR path is gated on `eightyCol`, so when this
    // function runs with `state.dhgr` true we are necessarily in
    // standard-HGR rev-0 territory (II+ always has `state.dhgr=false`).
    const uint8_t bit7Mask = state.dhgr ? uint8_t{0x7F} : uint8_t{0xFF};

    std::array<uint32_t, kWidth> raw;

    // Effective mode: ChatMauveRGB without a plugged card silently falls
    // back to NTSC (matches a real Apple II that's been pulled out of its
    // RGB adapter — the composite signal is still on the wire).
    // ColorCompositeOE also renders the NTSC LUT into `frame` as a
    // fallback — the real OE output comes from the shader in MainWindow
    // which consumes signalBuf, but if for any reason the shader isn't
    // available (lo-res, no GL context yet) the visible framebuffer is
    // still a sensible composite-coloured image.
    HiResMode effMode = hiResMode;
    // With a card plugged, every single-HGR state is routed to the 560-dot
    // Chat Mauve painters before this function is reached
    // (renderInternalBandImpl); without one, ChatMauveRGB is composite on
    // the wire. Either way there is nothing for this 280-wide path to do
    // for the card — the pair-aligned decode it used to carry here was a
    // second, byte-level copy of the LCM rule and is gone.
    if (effMode == HiResMode::ChatMauveRGB) effMode = HiResMode::ColorNTSC;
    if (effMode == HiResMode::ColorCompositeOE)           effMode = HiResMode::ColorNTSC;
    // ColorAppleWin normally never reaches renderHiRes at all: render()
    // skips renderInternal for it and demodulates the composite signal
    // CPU-side into frame80 (Phase 4 — no more discarded LUT pass). This
    // branch only fires on the defensive fallback path where the signal
    // couldn't be produced, so a sensible NTSC framebuffer is still drawn.
    if (effMode == HiResMode::ColorAppleWin)              effMode = HiResMode::ColorNTSC;

    if (effMode == HiResMode::ColorNTSC
        || effMode == HiResMode::ColorCompMedium
        || effMode == HiResMode::ColorComp4Bit) {
        // MAME-style 7-bit sliding-window decode. ContextBits = 3 leaves
        // the centre sub-pixel at bit 3 of the window, with 3 bits of
        // left context (the tail of the previous byte) and 3 bits of
        // right context (the head of the next byte) on either side.
        // Composite modes 0 / 1 use the artifact LUT (row 0 = canonical,
        // row 1 = medium-color-biased per MAME `apple2video.cpp:479-485`).
        // Mode 2 is a 4-bit square filter: each 4-dot nibble in the
        // bit stream maps DIRECTLY to a palette index, no artifact
        // window (MAME `:486-493` — `rotl4(w & 0x0f, x + is_80_column - 1)`).
        constexpr int kContextBits = 3;
        const int lutRow = (effMode == HiResMode::ColorCompMedium) ? 1 : 0;
        const bool squareFilter = (effMode == HiResMode::ColorComp4Bit);
        const uint8_t cacheKey = static_cast<uint8_t>(lutRow | (squareFilter ? 2 : 0));

        // Decode tables, built once. They fold the three per-sub-pixel
        // steps of the original loop — artifact LUT, `rotl4b` phase select,
        // lo-res palette — and the per-pixel `avgRgb` pair average into two
        // lookups, and are computed WITH those same functions so the output
        // is bit-identical (pom2_bench framebuffer hash is the check):
        //   phaseIdx[row][w & 0x7F][absX & 3] = rotl4b(LUT[row][w], absX)
        //   squareIdx[nibble][(absX - 1) & 3] = rotl4b(nibble | nibble << 4, absX - 1)
        //   pairAvg[a][b]                     = avgRgb(palette[a], palette[b])
        // rotl4b only looks at `count & 3`, which is why the phase dimension
        // is 4 — and why absX - 1 at absX = 0 (wraps to 3) needs no special
        // case: (unsigned)(-1) & 3 == 3 in both the table and the original.
        struct Tables {
            uint8_t  phaseIdx[2][128][4];
            uint8_t  squareIdx[16][4];
            uint32_t pairAvg[16][16];
            Tables() {
                for (int r = 0; r < 2; ++r)
                    for (unsigned w = 0; w < 128; ++w)
                        for (unsigned ph = 0; ph < 4; ++ph)
                            phaseIdx[r][w][ph] = static_cast<uint8_t>(
                                rotl4b(kArtifactColorLut[r][w], ph));
                for (unsigned n = 0; n < 16; ++n)
                    for (unsigned ph = 0; ph < 4; ++ph)
                        squareIdx[n][ph] = static_cast<uint8_t>(
                            rotl4b(static_cast<uint8_t>(n | (n << 4)), ph));
                for (unsigned a = 0; a < 16; ++a)
                    for (unsigned b = 0; b < 16; ++b)
                        pairAvg[a][b] = avgRgb(kLoResPalette[a], kLoResPalette[b]);
            }
        };
        static const Tables T;

        uint16_t words[40];
        uint8_t  idx[kStreamLen];   // lo-res palette index per sub-pixel

        for (int y = firstScanline; y < lastScanline; ++y) {
            const uint16_t rowAddr = hgrRowAddress(y, videoHgrPage2(state));
            buildHgrWordRow(ram, rowAddr, words, bit7Mask);

            uint32_t* outRow = frame.data() + static_cast<size_t>(y) * kWidth;
            HgrRowCache& rc = hgrRowCache_[static_cast<size_t>(y)];
            if (rc.valid && rc.key == cacheKey
                && std::memcmp(rc.words, words, sizeof(words)) == 0) {
                std::memcpy(outRow + px0, rc.out.data() + px0,
                            static_cast<size_t>(px1 - px0) * sizeof(uint32_t));
                continue;
            }

            // Scanline's 560 sub-pixels via incremental window. `w`
            // accumulates up to (3 + 14 + 14) = 31 bits — fits in a
            // uint32_t. Each iteration consumes one bit (`>>= 1`).
            uint32_t w = static_cast<uint32_t>(words[0]) << kContextBits;
            if (squareFilter) {
                // 4-bit square filter — literal port of MAME
                // composite_color_mode 2: rotl4(w & 0x0f,
                // x + is_80_column - 1) (apple2video.cpp:487-494).
                // is_80_column = 0 for HGR, so the rotation is
                // absX - 1. POM2's window carries kContextBits of LEFT
                // context; MAME's current 4 dots sit one bit lower than
                // the LUT window, so the nibble is (w >> kContextBits-1)
                // — NOT (w >> kContextBits). The two corrections must go
                // together: matched against a MAME oracle this is
                // bit-exact (0/2.2M dots), whereas (>>kContextBits,absX)
                // diverged on ~50% of interior dots.
                for (int col = 0; col < 40; ++col) {
                    if (col + 1 < 40)
                        w |= static_cast<uint32_t>(words[col + 1]) << (14 + kContextBits);
                    for (int b = 0; b < 14; ++b) {
                        const int absX = col * 14 + b;
                        const unsigned nibble = (w >> (kContextBits - 1)) & 0x0Fu;
                        idx[absX] = T.squareIdx[nibble][static_cast<unsigned>(absX - 1) & 3u];
                        w >>= 1;
                    }
                }
            } else {
                const auto& phase = T.phaseIdx[lutRow];
                for (int col = 0; col < 40; ++col) {
                    if (col + 1 < 40)
                        w |= static_cast<uint32_t>(words[col + 1]) << (14 + kContextBits);
                    for (int b = 0; b < 14; ++b) {
                        const int absX = col * 14 + b;
                        idx[absX] = phase[w & 0x7Fu][static_cast<unsigned>(absX) & 3u];
                        w >>= 1;
                    }
                }
            }

            // Downsample 560 sub-pixels → 280 framebuffer pixels by
            // pair averaging. This is the optical chroma-bandwidth-limit
            // a real CRT applies — without it, the 14 MHz bit pattern
            // would alias against the 7 MHz pixel grid. The full row is
            // decoded (the cache holds whole rows); only the write-back
            // below is clipped to [px0, px1).
            for (int x = 0; x < kWidth; ++x)
                rc.out[static_cast<size_t>(x)] = T.pairAvg[idx[2 * x]][idx[2 * x + 1]];
            std::memcpy(rc.words, words, sizeof(words));
            rc.key   = cacheKey;
            rc.valid = true;

            std::memcpy(outRow + px0, rc.out.data() + px0,
                        static_cast<size_t>(px1 - px0) * sizeof(uint32_t));
        }
        return;
    }

    // (See bottom of file for IIe 80-col text helpers.)

    // Monochrome path. The bit stream is sampled at twice the visible
    // pixel rate — averaging adjacent sub-pixels gives the soft
    // horizontal anti-aliased luminance a real CRT's chroma-bandwidth
    // limit produces. Persistence is per-pixel max(target, prev × decay):
    // mimics the additive re-excitation + passive fade of phosphor
    // chemistry. Switching modes clears the buffer (see setHiResMode).
    uint8_t stream[kStreamLen];
    const Phosphor phos = phosphorFor(effMode);
    // Decay is per EMULATED frame; the UI may render the same frame several
    // times on a >60 Hz monitor. Raise the per-frame factor to the
    // elapsed-emu-frames power so afterglow speed doesn't track the host
    // refresh; delta 0 (same frame re-rendered, or paused) must not decay.
    const float effDecay =
        emuFrameDelta_ == 0 ? 1.0f :
        emuFrameDelta_ == 1 ? phos.decay :
        std::pow(phos.decay, static_cast<float>(emuFrameDelta_));
    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint16_t rowAddr = hgrRowAddress(y, videoHgrPage2(state));
        buildBitStream(ram, rowAddr, stream, bit7Mask);

        uint8_t* histRow = persistenceL.data() + static_cast<size_t>(y) * kWidth;
        for (int x = px0; x < px1; ++x) {
            const int sub = x * 2;
            const int lit = stream[sub] + stream[sub + 1];   // 0..2
            const int target = (lit * 255) / 2;
            const int prev   = static_cast<int>(static_cast<float>(histRow[x]) * effDecay);
            const int merged = std::max(target, prev);
            histRow[x] = static_cast<uint8_t>(merged);

            const uint32_t r = (static_cast<uint32_t>(phos.r) * merged + 127) / 255;
            const uint32_t g = (static_cast<uint32_t>(phos.g) * merged + 127) / 255;
            const uint32_t b = (static_cast<uint32_t>(phos.b) * merged + 127) / 255;
            raw[x] = (uint32_t(0xFF) << 24) | (b << 16) | (g << 8) | r;
        }

        uint32_t* outRow = frame.data() + static_cast<size_t>(y) * kWidth;
        std::memcpy(outRow + px0, raw.data() + px0,
                    static_cast<size_t>(px1 - px0) * sizeof(uint32_t));
    }
}

// ─── IIe 80-column text ──────────────────────────────────────────────────
//
// On a IIe with 80COL on, the screen is 560 native horizontal pixels:
// 80 character cells × 7 px each. Aux RAM holds the EVEN columns (0,2,…)
// and main RAM holds the ODD columns (1,3,…). The display reads aux byte
// at the same logical address as the main byte — there's only one text
// page, just split across two banks. PAGE2 still selects between page 1
// and page 2 unless 80STORE is on (in which case writes to text page 1
// route to aux per the memory dispatcher; the display keeps reading from
// page 1 because only 80STORE+PAGE2 swaps banks at the memory layer, and
// the read here uses page 1 either way).
//
// The 4 KB IIe character ROM doubles as the alternate-character source:
// ALTCHAR=on selects the second 2 KB bank where flashing inverse becomes
// mousetext + non-flashing inverse. When the user has not loaded a real
// charset ROM (`roms/apple2_char.rom`), the built-in 5×7 fallback covers
// the printable range; ALTCHAR is then a no-op.

void Apple2Display::renderText80(Memory& mem, const Memory::DisplayState& state,
                                 int firstRow, int lastRow,
                                 int clipY0, int clipY1)
{
    const bool altCharSet = state.altChar;
    const uint8_t* main_ = mem.data();
    const uint8_t* aux_  = auxRam ? auxRam : mem.data();
    const bool flashPhase = (frameCounter / kFlashHalfPeriodFrames) & 1u;

    const bool useCharRom = mem.charRomActiveSize() >= 2048;

    for (int row = firstRow; row < lastRow; ++row) {
        // 80STORE + PAGE2 already routes writes to aux at the memory
        // layer, so reading from page 1 is the right thing for both halves.
        const uint16_t rowAddr = textRowAddress(row, videoTextPage2(state));
        for (int col = 0; col < 40; ++col) {
            // For each 40-byte text row, AUX byte renders the EVEN
            // 80-col cell (chars 0,2,4,…) at xCell0, MAIN byte the
            // ODD cell (chars 1,3,5,…) at xCell1.
            for (int half = 0; half < 2; ++half) {
                const uint8_t src   = (half == 0) ? aux_[rowAddr + col]
                                                  : main_[rowAddr + col];
                const int     cellX = col * 14 + half * 7;
                const int     cellY = row * 8;

                const auto rows = glyphRows7(src, mem.charRomActiveData(), mem.charRomActiveSize(), mem.charRomHasLowercase(),
                                             useCharRom, altCharSet, flashPhase);
                for (int gy = 0; gy < 8; ++gy) {
                    const int y = cellY + gy;
                    if (y < clipY0 || y >= clipY1) continue;   // beam-split clip
                    for (int gx = 0; gx < 7; ++gx)
                        frame80[y * kWidth80 + (cellX + gx)] =
                            ((rows[gy] >> gx) & 1u) ? 0xFFFFFFFFu : 0xFF000000u;
                }
            }
        }
    }
}

void Apple2Display::upscaleFrameToFrame80(int firstScanline, int lastScanline)
{
    // Pixel-double frame[] horizontally into frame80[] for the requested
    // scanline range. Each native 280-wide pixel becomes two 560-wide
    // pixels of identical colour. Used to bridge HGR (always rendered at
    // 280 wide) into the 560-wide frame80 buffer when mixed-mode 80-col
    // text is on at the bottom.
    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint32_t* in  = frame.data()    + static_cast<size_t>(y) * kWidth;
        uint32_t*       out = frame80.data()  + static_cast<size_t>(y) * kWidth80;
        for (int x = 0; x < kWidth; ++x) {
            const uint32_t p = in[x];
            out[x * 2 + 0] = p;
            out[x * 2 + 1] = p;
        }
    }
}

// ─── IIe Double Hi-Res (DHGR) ────────────────────────────────────────────
//
// DHGR doubles HGR's horizontal resolution by interleaving aux RAM with
// main RAM at the byte level. Per scanline (HGR address formula already
// resolves the base of the row):
//
//   for c in 0..39:
//     aux_byte  = aux  [base + c]   → 7 dots at columns [c*14 .. c*14+6]
//     main_byte = main [base + c]   → 7 dots at columns [c*14+7 .. c*14+13]
//     bit 0 of each byte is the leftmost dot in its half.
//
// Total: 40 byte-pairs × 14 dots = 560 dots per scanline. MAME masks each
// byte with `& 0x7f` (the high bit is unused in DHGR); POM2 reads only
// bits 0..6 explicitly, so the masking is implicit.
//
// Three color paths, picked by `hiResMode`:
//
//   ColorNTSC   — composite artifact decode. 7-bit sliding window over
//                 the raw 560-dot stream, indexed into MAME's
//                 `kArtifactColorLut[128]`, then `rotl4b(value, absX+1)`
//                 selects the 4-bit lo-res palette index. Per-pixel
//                 (560 lookups/scanline) → produces the inter-cell
//                 fringing real composite Apple IIe monitors show. The
//                 `+1` matches MAME's `is_80_column = 1` for DHGR in
//                 `apple2video.cpp::render_line_artifact_color`.
//
//   ChatMauveRGB — clean RGB-card 4-dot block decode. Each 4 consecutive
//                  dots form a nibble (bit 0 = leftmost), the nibble is
//                  rotated left by 1 (matches MAME's Video-7 rgbmode==3
//                  path in `dhgr_update`), and the result indexes
//                  `kChatMauveLoResPalette` — the Péritel palette where
//                  indices 5 and 10 are *distinct* grays (the famous
//                  "two distinct grays" Le Chat Mauve trademark).
//
//   Mono*       — each dot = luminance bit × phosphor tint. No artifact
//                 decoding. Uses the dedicated `persistenceL80` history
//                 buffer (560×192) so amber afterglow persists in DHGR
//                 just like it does in HGR via `persistenceL`.

void Apple2Display::renderDhgr(Memory& mem, const Memory::DisplayState& state,
                               int firstScanline, int lastScanline)
{
    const uint8_t* main_ = mem.data();
    const uint8_t* aux_  = auxRam ? auxRam : main_;

    const HiResMode m = hiResMode;
    const bool monochrome = (m == HiResMode::MonoWhite ||
                             m == HiResMode::MonoGreen ||
                             m == HiResMode::MonoAmber);
    // ChatMauveRGB without a plugged card silently falls back to NTSC
    // (matches a real IIe pulled out of its RGB adapter).
    const bool useChatMauve = (m == HiResMode::ChatMauveRGB) && (chatMauve != nullptr);
    const bool useComposite = !monochrome && !useChatMauve;
    const uint32_t* rgbCardPalette = useChatMauve
        ? kChatMauveLoResPalette : kLoResPalette;

    // The card decides what DHGR means (LeChatMauveCard::dhgrMode: the
    // patent latch, the variant's fallbacks, the Eve's table IX-1). Three
    // of the Eve's answers are not decodes of the 560-dot stream at all.
    using DhgrMode = LeChatMauveCard::DhgrMode;
    // Inside a beam-raced replay the band carries the latch of its own
    // moment; a whole-frame render asks the card's current value.
    const DhgrMode dm = !useChatMauve ? DhgrMode::COL140
        : (bandLatch_ >= 0
               ? chatMauve->dhgrModeFor(
                     static_cast<LeChatMauveCard::RenderMode>(bandLatch_ & 0b11))
               : chatMauve->dhgrMode());
    if (useChatMauve) {
        if (dm == DhgrMode::CP280 && auxRam != nullptr) {
            renderHgrDuochrome(mem, state, firstScanline, lastScanline,
                               chatMauve->auxHiNibbleIsForeground());
            return;
        }
        if (dm == DhgrMode::COL280A || dm == DhgrMode::COL280B) {
            renderDhgrCol280(mem, state, firstScanline, lastScanline,
                             dm == DhgrMode::COL280B);
            return;
        }
        if (dm == DhgrMode::Blank) {
            // Table IX-1 "écran noir": HR1+HR2 without HR3. CPREG keeps
            // working underneath (Memory's aux shadow), the picture is black.
            for (int y = firstScanline; y < lastScanline; ++y)
                std::fill_n(frame80.data() + static_cast<size_t>(y) * kWidth80,
                            kWidth80, 0xFF000000u);
            return;
        }
    }

    // Phosphor tint + decay. Mirrors the HGR mono path so DHGR mono now
    // shares the same amber afterglow / green persistence characteristics
    // — only the buffer geometry differs (560×192 vs 280×192).
    const Phosphor phos = monochrome ? phosphorFor(m) : kPhosphorWhite;
    const struct { uint8_t r, g, b; } tint = { phos.r, phos.g, phos.b };

    constexpr int kContextBits = 3;

    uint8_t  dots [kWidth80];   // raw 560-dot stream (mono + RGB-card paths)
    uint16_t pairs[40];         // aux+main packed words (composite path)

    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint16_t rowAddr = hgrRowAddress(y, videoHgrPage2(state));
        uint32_t* outRow = frame80.data() + static_cast<size_t>(y) * kWidth80;

        if (useComposite) {
            // Composite color mode selection: NTSC = LUT row 0, Medium =
            // LUT row 1, 4Bit = square filter. Matches MAME
            // `apple2video.cpp:479-498` for DHGR.
            const int lutRow = (m == HiResMode::ColorCompMedium) ? 1 : 0;
            const bool squareFilter = (m == HiResMode::ColorComp4Bit);
            // Pack the aux+main pair as 14 bits: aux's bits 0..6 in the low
            // 7 (leftmost 7 dots of the cell pair), main's bits 0..6 in
            // bits 7..13 (rightmost 7 dots). Mirrors MAME's
            //   words[col] = (vaux & 0x7f) | ((vram & 0x7f) << 7);
            for (int c = 0; c < 40; ++c) {
                const uint8_t auxB  = aux_ [rowAddr + c] & 0x7Fu;
                const uint8_t mainB = main_[rowAddr + c] & 0x7Fu;
                pairs[c] = static_cast<uint16_t>(auxB | (mainB << 7));
            }

            // Incremental 7-bit window (3 left context + current + 3 right
            // context). At each step the lookup uses bits 0..6 of `w`,
            // then `w >>= 1` shifts the whole stream by one dot.
            uint32_t w = static_cast<uint32_t>(pairs[0]) << kContextBits;
            for (int col = 0; col < 40; ++col) {
                if (col + 1 < 40) {
                    w |= static_cast<uint32_t>(pairs[col + 1])
                         << (14 + kContextBits);
                }
                for (int b = 0; b < 14; ++b) {
                    const int absX = col * 14 + b;
                    unsigned loresIdx;
                    if (squareFilter) {
                        // Mode 2 (square) — literal MAME rotl4(w & 0x0f,
                        // x + is_80_column - 1) with is_80_column = 1 for DHGR,
                        // so the rotation is absX (= absX + 1 - 1). The nibble
                        // origin is (w >> kContextBits-1), one bit below the LUT
                        // window — same coupled correction as the HGR path.
                        // MAME-oracle bit-exact (0/2.2M dots).
                        const unsigned nibble = (w >> (kContextBits - 1)) & 0x0Fu;
                        loresIdx = rotl4b(
                            static_cast<uint8_t>(nibble | (nibble << 4)),
                            static_cast<unsigned>(absX));
                    } else {
                        const uint8_t lutEntry =
                            kArtifactColorLut[lutRow][w & 0x7Fu];
                        // is_80_column = 1 for DHGR → rotation = absX + 1.
                        loresIdx = rotl4b(
                            lutEntry, static_cast<unsigned>(absX + 1));
                    }
                    outRow[absX] = kLoResPalette[loresIdx];
                    w >>= 1;
                }
            }
            continue;
        }

        // Build the 560-dot stream (mono + RGB-card both walk it).
        for (int c = 0; c < 40; ++c) {
            const uint8_t auxB  = aux_ [rowAddr + c];
            const uint8_t mainB = main_[rowAddr + c];
            const int base = c * 14;
            for (int i = 0; i < 7; ++i) {
                dots[base + i]     = static_cast<uint8_t>((auxB  >> i) & 1u);
                dots[base + 7 + i] = static_cast<uint8_t>((mainB >> i) & 1u);
            }
        }

        if (monochrome) {
            // Same `max(target, prev × decay)` rule as the HGR mono path
            // (renderHiRes monochrome branch). MonoWhite/MonoGreen have
            // decay=0 — the multiplication collapses to plain target
            // every frame — so they look identical to the no-history
            // version. MonoAmber's decay=0.96 is what makes the bytes
            // glow for ~25 frames after being cleared on a real CRT.
            uint8_t* histRow = persistenceL80.data()
                             + static_cast<size_t>(y) * kWidth80;
            for (int x = 0; x < kWidth80; ++x) {
                const int target = dots[x] ? 255 : 0;
                const int prev   = static_cast<int>(
                    static_cast<float>(histRow[x]) * phos.decay);
                const int merged = std::max(target, prev);
                histRow[x] = static_cast<uint8_t>(merged);
                const uint32_t r = (static_cast<uint32_t>(tint.r) * merged + 127) / 255;
                const uint32_t g = (static_cast<uint32_t>(tint.g) * merged + 127) / 255;
                const uint32_t b = (static_cast<uint32_t>(tint.b) * merged + 127) / 255;
                outRow[x] = (uint32_t(0xFF) << 24) | (b << 16) | (g << 8) | r;
            }
        } else {
            // Le Chat Mauve / Video-7 RGB card, on the 560-dot stream.
            constexpr uint32_t kWhite = 0xFFFFFFFFu;
            constexpr uint32_t kBlack = 0xFF000000u;

            if (dm == DhgrMode::Chunky160) {
                // Video-7 "160-wide" chunky mode — MAME dhgr_update
                // rgbmode==2 (:906-930). Each column = aux + (main<<8) →
                // four 4-bit pixels of three dots each (480 wide), centred
                // in 560 with 40 black margins. Video-7 variant only; the
                // Chat Mauve boards fold this latch value into COL140.
                int x = 0;
                for (int b = 0; b < 40; ++b) outRow[x++] = kBlack;
                for (int c = 0; c < 40; ++c) {
                    unsigned v = aux_[rowAddr + c]
                               + (static_cast<unsigned>(main_[rowAddr + c]) << 8);
                    for (int i = 0; i < 4; ++i) {
                        const uint32_t col = rgbCardPalette[v & 0x0Fu];
                        outRow[x++] = col; outRow[x++] = col; outRow[x++] = col;
                        v >>= 4;
                    }
                }
                for (int b = 0; b < 40; ++b) outRow[x++] = kBlack;
            } else if (dm == DhgrMode::BW560) {
                // 560 dots black and white — the clean version of what a
                // mono monitor shows (MAME rgbmode 0 forces the mono
                // renderer, dhgr_update :896,941-944).
                for (int x = 0; x < kWidth80; ++x)
                    outRow[x] = dots[x] ? kWhite : kBlack;
            } else if (dm == DhgrMode::COL140) {
                // The patent's 140×192 mode: each 4-dot cell of the line's
                // fixed grid is one colour, the nibble (bit 0 = leftmost dot)
                // rotated right by one into lo-res numbering (AppleWin
                // `UpdateDHiResCellRGB`, MAME rotl4(n,1)). No fringing.
                for (int cell = 0; cell < kWidth80 / 4; ++cell) {
                    const int d = cell * 4;
                    const unsigned nib = dots[d] | (dots[d + 1] << 1)
                                       | (dots[d + 2] << 2) | (dots[d + 3] << 3);
                    const uint32_t col = rgbCardPalette[((nib << 1) | (nib >> 3)) & 0x0Fu];
                    outRow[d] = outRow[d + 1] = outRow[d + 2] = outRow[d + 3] = col;
                }
            } else {
                // Mixed COL140 + BW560 — docs/chatmauve_plan.md § 3.3. The
                // hardware is a per-BYTE mux (the byte's bit 7 selects the
                // 560 path or the 140 path for its seven dots — patent
                // FIG. 1) over the 140 path's 4-dot cell latch, which runs
                // free on the line's grid whatever the mux does. The two
                // boundary rules fenarinarsa measured on the //c adapter
                // (AppleWin PR #837, `UpdateDHiResCellRGB`) fall out of it:
                //   * a colour cell that runs into a BW byte is CUT there
                //     (the mux switched; the BW byte paints its own bits);
                //   * a BW byte that runs into a colour byte has its LAST
                //     BW dot repeated until the next cell boundary (the
                //     latch was not reloaded at a cell start, so the 140
                //     path is still holding the level the 560 path left).
                // The cell's colour is the nibble of the raw stream on the
                // grid, wherever its four bits come from — as AppleWin.
                // The manual's "il est fortement conseillé que les 4 bits 7
                // des octets d'une cellule soient dans un même état" is
                // advice to stay clear of exactly these two cases.
                // MAME's byte-level rule (colour dots of a partial cell
                // painted from the mixed nibble) is the approximation this
                // replaces. Dragon Wars encodes bit 7 the other way round:
                // `invertBit7` is that compatibility switch.
                const uint8_t bit7Xor = chatMauve->invertBit7() ? uint8_t{0x80} : uint8_t{0};
                bool    lastColor = true;   // stream start: no partial cell to repeat into
                uint8_t lastBit   = 0;
                for (int b = 0; b < 80; ++b) {
                    const uint8_t byte = ((b & 1) ? main_[rowAddr + (b >> 1)]
                                                  : aux_ [rowAddr + (b >> 1)]) ^ bit7Xor;
                    const bool isColor = (byte & 0x80u) != 0;
                    const int  d0 = b * 7;
                    // First dot of this byte that starts a fresh cell.
                    const int  firstFullCell = (d0 + 3) & ~3;
                    for (int d = d0; d < d0 + 7; ++d) {
                        if (isColor) {
                            if (!lastColor && d < firstFullCell) {
                                outRow[d] = lastBit ? kWhite : kBlack;   // repeat rule
                            } else {
                                const int c = d & ~3;
                                const unsigned nib = dots[c] | (dots[c + 1] << 1)
                                                   | (dots[c + 2] << 2) | (dots[c + 3] << 3);
                                outRow[d] = rgbCardPalette[((nib << 1) | (nib >> 3)) & 0x0Fu];
                            }
                        } else {
                            lastBit   = dots[d];
                            outRow[d] = lastBit ? kWhite : kBlack;
                        }
                    }
                    lastColor = isColor;
                }
            }
        }
    }
}

// ─── Composite-signal generator for the OpenEmulator shader path ─────────
//
// Produces a 14.318 MHz 1-bit luminance waveform (560 samples × 192 lines)
// that the GLSL shader in MainWindow demodulates into NTSC Y/I/Q. Each
// scanline of the Apple II video output is exactly 560 samples wide at
// the 4×-subcarrier rate — the same width DHGR already uses natively —
// so HGR (with its half-dot delay), DHGR, and text all naturally fit.
//
// We only generate the signal for HGR, DHGR, DLGR, and 40/80-column text;
// lo-res GR uses paintLoRes40, DLGR uses paintLoResDouble (aux+main).
bool Apple2Display::fillCompositeSignal(Memory& mem,
                                        const std::vector<Memory::VideoEvent>& events)
{
    // Beam-racing: when the frame logged mid-scanline display soft-switch
    // edges we recompose the signal band-by-band, starting from the
    // frame-start state and applying each event at its scanline boundary —
    // mirroring renderBeamRacing()'s replay, but writing the 14.318 MHz
    // waveform instead of the RGBA framebuffer. `state` is mutable so the
    // paint helpers (capturing it by reference) see the per-band switches.
    // Same rule as render()'s own `state` (see there): the frame-start
    // snapshot describes the PUBLISHED frame, and the live one has already
    // moved into the recording frame — an event-free frame is not an excuse
    // to paint the signal from a state that belongs to the next one. Only
    // before the machine's first video frame (frameCounter 0 — no publication
    // has happened, and the display tests drive Memory with no clock at all)
    // is the live state the better description.
    const bool beamRace = !events.empty();
    Memory::DisplayState state = (beamRace || frameCounter > 0)
                                     ? mem.getDisplayStateAtFrameStart()
                                     : mem.getDisplayState();
    signalPhaseOffset_ = 0;
    // Zero first so bands a given mode leaves unpainted (mixed-mode text band,
    // a text→graphics split's empty rows) read as black instead of stale.
    std::fill(signalBuf.begin(), signalBuf.end(), 0);
    const uint8_t* ram = mem.data();
    const uint8_t* aux = auxRam ? auxRam : ram;
    const bool flashPhase = (frameCounter / kFlashHalfPeriodFrames) & 1u;
    const bool  useCharRom = mem.charRomActiveSize() >= 2048;
    // NOTE: ALTCHAR is read as `state.altChar` inside each paint helper, not
    // hoisted here — `state` is reassigned per beam-race segment below, so a
    // mid-frame $C00E/$C00F flip must reach the glyph lookup (the RGBA path
    // already passes per-band state.altChar through renderText/renderText80).

    // Helper: paint one text row (40 cols × 8 scanlines = 7×8 dots per cell,
    // each dot doubled to 2 signal samples → 14 samples per cell, 560/line).
    // clipY0/clipY1 bound the written scanlines for non-row-aligned beam
    // splits (bandRows hands a straddled row to both bands — see bandRows).
    auto paintText40 = [&](int firstRow, int lastRow, int col0, int col1,
                           int clipY0, int clipY1) {
        for (int row = firstRow; row < lastRow; ++row) {
            const uint16_t rowAddr = textRowAddress(row, videoTextPage2(state));
            for (int col = col0; col < col1; ++col) {
                const uint8_t src = ram[rowAddr + col];
                const auto bytes = glyphRows7(src, mem.charRomActiveData(), mem.charRomActiveSize(), mem.charRomHasLowercase(),
                                              useCharRom, state.altChar, flashPhase);
                for (int gy = 0; gy < 8; ++gy) {
                    const int y = row * 8 + gy;
                    if (y < clipY0 || y >= clipY1) continue;
                    uint8_t* dst = signalBuf.data()
                                 + static_cast<size_t>(y) * kSignalWidth
                                 + col * 14;
                    for (int gx = 0; gx < 7; ++gx) {
                        const uint8_t lit = ((bytes[gy] >> gx) & 1u) ? 0xFFu : 0x00u;
                        dst[gx * 2 + 0] = lit;
                        dst[gx * 2 + 1] = lit;
                    }
                }
            }
        }
    };

    // Helper: paint one 80-col text row (80 cols × 8 dots × 7 pixels = 560).
    // col0/col1 are byte columns (0..40); each maps to two 80-col cells.
    auto paintText80 = [&](int firstRow, int lastRow, int col0, int col1,
                           int clipY0, int clipY1) {
        for (int row = firstRow; row < lastRow; ++row) {
            const uint16_t rowAddr = textRowAddress(row, videoTextPage2(state));
            for (int col = col0 * 2; col < col1 * 2; ++col) {
                // Aux RAM holds even columns, main RAM odd columns
                // (AppleWin scanner convention).
                const uint8_t src = (col & 1) ? ram[rowAddr + (col >> 1)]
                                              : aux[rowAddr + (col >> 1)];
                const auto bytes = glyphRows7(src, mem.charRomActiveData(), mem.charRomActiveSize(), mem.charRomHasLowercase(),
                                              useCharRom, state.altChar, flashPhase);
                for (int gy = 0; gy < 8; ++gy) {
                    const int y = row * 8 + gy;
                    if (y < clipY0 || y >= clipY1) continue;
                    uint8_t* dst = signalBuf.data()
                                 + static_cast<size_t>(y) * kSignalWidth
                                 + col * 7;
                    for (int gx = 0; gx < 7; ++gx) {
                        dst[gx] = ((bytes[gy] >> gx) & 1u) ? 0xFFu : 0x00u;
                    }
                }
            }
        }
    };

    // Helper: paint a band of HGR scanlines [first, last) at 280 dots ×
    // 2 = 560 signal samples per line. Uses the existing 560-sub-pixel
    // bit stream builder (which already applies the per-byte half-dot
    // delay from bit 7).
    auto paintHgr = [&](int first, int last, int col0, int col1) {
        // IIe rev-0 DHIRES quirk, same as renderHiRes: AN3 on suppresses the
        // half-dot delay in plain HGR (MAME apple2video.cpp `bit7_mask =
        // m_dhires ? 0 : 0x80`). Was hardcoded 0xFF here, so the composite
        // paths disagreed with the LUT framebuffer on the same frame.
        const uint8_t bit7Mask = state.dhgr ? uint8_t{0x7F} : uint8_t{0xFF};
        uint8_t stream[kStreamLen];
        // Each byte column is 14 signal samples (40 × 14 = 560). The stream is
        // built for the WHOLE scanline (so the half-dot delay / byte-boundary
        // fringing keeps its neighbour context) and only [col0,col1) written.
        const int s0 = col0 * 14;
        const int s1 = col1 * 14;
        for (int y = first; y < last; ++y) {
            const uint16_t rowAddr = hgrRowAddress(y, videoHgrPage2(state));
            buildBitStream(ram, rowAddr, stream, bit7Mask);
            uint8_t* dst = signalBuf.data()
                         + static_cast<size_t>(y) * kSignalWidth;
            for (int x = s0; x < s1; ++x) {
                dst[x] = stream[x] ? 0xFFu : 0x00u;
            }
        }
    };

    // Helper: paint a band of DHGR scanlines [first, last). DHGR
    // interleaves aux+main HGR memory at 7 bits each — 14 dots per byte
    // pair, 40 byte pairs per line = 560 dots = 560 signal samples.
    auto paintDhgr = [&](int first, int last, int col0, int col1) {
        for (int y = first; y < last; ++y) {
            const uint16_t rowAddr = hgrRowAddress(y, videoHgrPage2(state));
            uint8_t* dst = signalBuf.data()
                         + static_cast<size_t>(y) * kSignalWidth;
            for (int c = col0; c < col1; ++c) {
                const uint8_t auxB  = aux[rowAddr + c] & 0x7Fu;
                const uint8_t mainB = ram[rowAddr + c] & 0x7Fu;
                const int base = c * 14;
                for (int i = 0; i < 7; ++i) {
                    dst[base + i    ] = ((auxB  >> i) & 1u) ? 0xFFu : 0x00u;
                    dst[base + 7 + i] = ((mainB >> i) & 1u) ? 0xFFu : 0x00u;
                }
            }
        }
    };

    // Helper: paint a band of lo-res block-rows [first, last). Each
    // block-row is 4 signal scanlines of one half (low nibble = upper,
    // high nibble = lower) of one 40-byte text-row. A real Apple II in
    // lo-res emits the 4-bit colour nibble as a repeating bit pattern at
    // the 4×-subcarrier rate — exactly one of the 16 NTSC artifact
    // patterns. We just emit `(nibble >> (x mod 4)) & 1` at every
    // sample; the shader's Y/I/Q demodulator then recovers the colour
    // from the pattern's spectral content, same path HGR uses.
    auto paintLoRes40 = [&](int firstBlockRow, int lastBlockRow, int col0, int col1,
                            int clipY0, int clipY1) {
        for (int blockRow = firstBlockRow; blockRow < lastBlockRow; ++blockRow) {
            const int textRow = blockRow / 2;
            const bool upperHalf = (blockRow % 2 == 0);
            const uint16_t rowAddr = textRowAddress(textRow, videoTextPage2(state));
            for (int col = col0; col < col1; ++col) {
                const uint8_t b = ram[rowAddr + col];
                const uint8_t nibble = upperHalf
                    ? static_cast<uint8_t>(b & 0x0Fu)
                    : static_cast<uint8_t>((b >> 4) & 0x0Fu);
                for (int dy = 0; dy < 4; ++dy) {
                    const int y = blockRow * 4 + dy;
                    if (y < clipY0 || y >= clipY1) continue;
                    uint8_t* dst = signalBuf.data()
                                 + static_cast<size_t>(y) * kSignalWidth
                                 + col * 14;
                    for (int dx = 0; dx < 14; ++dx) {
                        const int absX = col * 14 + dx;
                        const uint8_t bit = (nibble >> (absX & 3)) & 1u;
                        dst[dx] = bit ? 0xFFu : 0x00u;
                    }
                }
            }
        }
    };

    // DLGR: aux nibble (rotl4) on even 7-dot half, main on odd — MAME
    // lores_update<Double>; mirrors renderLoResDouble().
    auto paintLoResDouble = [&](int firstBlockRow, int lastBlockRow, int col0, int col1,
                                int clipY0, int clipY1) {
        auto rotl4 = [](uint8_t n) -> uint8_t {
            return static_cast<uint8_t>(((n << 1) | (n >> 3)) & 0x0F);
        };
        for (int blockRow = firstBlockRow; blockRow < lastBlockRow; ++blockRow) {
            const int  textRow   = blockRow / 2;
            const bool upperHalf = (blockRow % 2 == 0);
            const uint16_t rowAddr = textRowAddress(textRow, videoTextPage2(state));
            for (int col = col0; col < col1; ++col) {
                const uint8_t mb = ram[rowAddr + col];
                const uint8_t ab = aux[rowAddr + col];
                const uint8_t mNib = upperHalf
                    ? static_cast<uint8_t>(mb & 0x0Fu)
                    : static_cast<uint8_t>((mb >> 4) & 0x0Fu);
                const uint8_t aNib = upperHalf
                    ? static_cast<uint8_t>(ab & 0x0Fu)
                    : static_cast<uint8_t>((ab >> 4) & 0x0Fu);
                const uint8_t auxPat  = rotl4(aNib);
                const uint8_t mainPat = mNib;
                for (int dy = 0; dy < 4; ++dy) {
                    const int y = blockRow * 4 + dy;
                    if (y < clipY0 || y >= clipY1) continue;
                    uint8_t* dst = signalBuf.data()
                                 + static_cast<size_t>(y) * kSignalWidth
                                 + col * 14;
                    // The nibble pattern is emitted at the ABSOLUTE
                    // 14.318 MHz sample index (col*14+dx), same as
                    // paintLoRes40's `(nibble >> (absX & 3))` — the real
                    // hardware's pattern generator is locked to the colour
                    // subcarrier, not restarted per 7-dot half-cell.
                    // Indexing by `dx & 3` rotated the hue by (2·col) mod 4
                    // per column (14 ≡ 2 mod 4), making a uniform DLGR fill
                    // demodulate to alternating colours.
                    for (int dx = 0; dx < 7; ++dx) {
                        const uint8_t bit = (auxPat >> ((col * 14 + dx) & 3)) & 1u;
                        dst[dx] = bit ? 0xFFu : 0x00u;
                    }
                    for (int dx = 0; dx < 7; ++dx) {
                        const uint8_t bit = (mainPat >> ((col * 14 + 7 + dx) & 3)) & 1u;
                        dst[7 + dx] = bit ? 0xFFu : 0x00u;
                    }
                }
            }
        }
    };

    // Paint one band × column segment [scanY0, scanY1) × byte columns
    // [col0, col1) according to the CURRENT `state` (set per segment by the
    // beam-race replay below). Mirrors renderInternalBand()'s decision tree —
    // same bandRows/bandScanlines clipping — but writes into signalBuf instead
    // of frame / frame80. Every painter (280-wide AND the 560-wide IIe modes)
    // honours [col0, col1) — the signal builders are simple per-column bit
    // emitters (no NTSC artifact window: the shader demodulates downstream), so
    // the horizontal mid-scanline split lands in the OE/AppleWin demod picture
    // too, not just the RGBA framebuffer.
    auto paintSignalBand = [&](int scanY0, int scanY1, int col0, int col1) {
        int lo = 0, hi = 0;
        if (state.textMode) {
            if (bandRows(scanY0, scanY1, 0, 24, &lo, &hi)) {
                if (mem.isIIE() && state.eightyCol)
                    paintText80(lo, hi, col0, col1, scanY0, scanY1);
                else
                    paintText40(lo, hi, col0, col1, scanY0, scanY1);
            }
            return;
        }
        if (!state.hiRes) {
            // Lo-res / DLGR: each block-row is 4 signal scanlines. Block
            // rows are rounded outward and the painters clip to [lo, hi),
            // so a block straddling a beam split is painted by both bands,
            // each within its own scanlines (same policy as bandRows).
            const bool isDlgr   = mem.isIIE() && state.eightyCol && state.dhgr;
            const int  blockEnd = state.mixedMode ? 40 : 48;  // block-rows
            if (bandScanlines(scanY0, scanY1, 0, blockEnd * 4, &lo, &hi)) {
                const int brLo = lo / 4;
                const int brHi = (hi + 3) / 4;
                if (isDlgr) paintLoResDouble(brLo, brHi, col0, col1, lo, hi);
                else        paintLoRes40(brLo, brHi, col0, col1, lo, hi);
            }
            // Mixed-mode text band stays black — crisp mono text is composited
            // after demod (patchMixedTextBand), same as HGR mixed.
            return;
        }
        // Hi-res — DHGR variant when on IIe with 80COL + DHIRES, else HGR.
        // signalPhaseOffset_ is a single per-frame demod constant; the last
        // graphics band painted wins (matches the uniform-frame fast path —
        // a mid-frame HGR↔DHGR phase split is a documented approximation).
        const bool isDhgr = mem.isIIE() && state.eightyCol && state.dhgr && state.hiRes;
        signalPhaseOffset_ = isDhgr ? 1 : 0;
        const int hiResEnd = state.mixedMode ? 160 : 192;
        if (bandScanlines(scanY0, scanY1, 0, hiResEnd, &lo, &hi)) {
            if (isDhgr) paintDhgr(lo, hi, col0, col1); else paintHgr(lo, hi, col0, col1);
        }
        // Mixed-mode text band: see lo-res note above — left black here.
    };

    if (!beamRace) {
        paintSignalBand(0, kSignalHeight, 0, 40);
        return true;
    }

    // Beam-racing: replay the event log through the SAME band × column-segment
    // decomposition the RGBA path uses (forEachBeamSegment), painting each
    // segment into signalBuf. `state` is the mutable local the paint helpers
    // capture by reference, so set it per segment before painting.
    forEachBeamSegment(mem.getDisplayStateAtFrameStart(), events,
        mem.videoStandard(), 0b11,
        [&](const Memory::DisplayState& st, int y0, int y1, int col0, int col1,
            uint8_t) {
            state = st;
            paintSignalBand(y0, y1, col0, col1);
        });
    return true;
}
