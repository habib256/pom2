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

// Horizontal (mid-scanline, per-byte-column) beam-racing in the COMPOSITE
// signal path. The RGBA split is pinned by horizontal_split_smoke; this pins
// the same split in fillCompositeSignal() — the 14.318 MHz waveform the OE /
// AppleWin demod modes consume. A frame whose lower band re-flips $C050/$C051
// every scanline (graphics from byte column 0, text from byte column 20) must
// produce, ON THE SAME LINE, the HGR waveform in the left 280 samples and the
// TEXT waveform in the right 280 samples of signalBuf.
//
// Plan: TODO.md [Display] "Split horizontal mid-scanline", composite increment.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t SET_TEXT  = 0xC051;
constexpr uint16_t CLR_TEXT  = 0xC050;
constexpr uint16_t SET_HIRES = 0xC057;
constexpr uint16_t SET_PAGE1 = 0xC054;

constexpr int W            = 560;          // composite signal width
constexpr int kSplitCol    = 21;           // byte column where text takes over (hpos 45 → col 21)
constexpr int kSplitSample = kSplitCol * 14;   // 280
constexpr int kBandTop     = 96;           // row-aligned (12 * 8)

uint16_t textRowAddr(int row)
{
    return static_cast<uint16_t>(0x0400 + 0x80 * (row & 7) + 0x28 * (row >> 3));
}

uint16_t hgrAddr(int y)
{
    return static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

void populate(Memory& mem)
{
    for (int row = 0; row < 24; ++row) {
        const uint16_t a = textRowAddr(row);
        for (int col = 0; col < 40; ++col)
            mem.memWrite(a + col, static_cast<uint8_t>(0xC1 + ((row * 5 + col) & 0x1F)));
    }
    for (int y = 0; y < 192; ++y) {
        const uint16_t a = hgrAddr(y);
        for (int col = 0; col < 40; ++col)
            mem.memWrite(a + col, static_cast<uint8_t>(0x55 ^ ((y + col * 3) & 0x7F)));
    }
}

// Capture the 560×192 composite signal for `mem` in ColorCompositeOECpu mode
// (needSignal → fillCompositeSignal runs).
std::vector<uint8_t> signalOf(Memory& mem)
{
    Apple2Display d;
    d.setAuxMemory(mem.auxData());
    d.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
    d.render(mem);
    assert(d.signalProduced());
    const uint8_t* s = d.signal();
    return std::vector<uint8_t>(s, s + static_cast<size_t>(d.signalWidth()) * d.signalHeight());
}

// One render: the composite signal AND the framebuffer it presents. The
// framebuffer is what the mixed-mode text band lands in, so the published-
// frame check below needs both.
struct Shot {
    std::vector<uint8_t>  sig;
    std::vector<uint32_t> pix;
    int                   w = 0, h = 0;
};

Shot renderOf(Memory& mem)
{
    Apple2Display d;
    d.setAuxMemory(mem.auxData());
    d.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
    d.render(mem);
    assert(d.signalProduced());
    Shot s;
    const uint8_t* g = d.signal();
    s.sig.assign(g, g + static_cast<size_t>(d.signalWidth()) * d.signalHeight());
    s.w = d.width();
    s.h = d.height();
    const uint32_t* p = d.pixels();       // runs the deferred CPU demod
    s.pix.assign(p, p + static_cast<size_t>(s.w) * s.h);
    return s;
}

// memcmp a horizontal sample span [x0, x1) of scanline `y` between two signals.
bool spanEqual(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
               int y, int x0, int x1)
{
    return std::memcmp(a.data() + static_cast<size_t>(y) * W + x0,
                       b.data() + static_cast<size_t>(y) * W + x0,
                       static_cast<size_t>(x1 - x0)) == 0;
}

} // namespace

int main()
{
    // ── Reference 1: pure graphics+HIRES. ────────────────────────────────
    Memory hgrRef;
    populate(hgrRef);
    hgrRef.memRead(CLR_TEXT);
    hgrRef.memRead(SET_HIRES);
    hgrRef.memRead(SET_PAGE1);
    const auto sHgr = signalOf(hgrRef);

    // ── Reference 2: pure TEXT. ──────────────────────────────────────────
    Memory textRef;
    populate(textRef);
    textRef.memRead(SET_TEXT);
    textRef.memRead(SET_PAGE1);
    const auto sText = signalOf(textRef);

    // Sanity: the two waveforms differ in both windows on the probed rows.
    for (int y : {100, 150}) {
        assert(!spanEqual(sHgr, sText, y, 0, kSplitSample));
        assert(!spanEqual(sHgr, sText, y, kSplitSample, W));
    }

    // ── Beam-raced frame: top half HGR, lower band a per-scanline strip ──
    Memory beam;
    populate(beam);
    beam.memRead(CLR_TEXT);     // frame-start = graphics + HIRES
    beam.memRead(SET_HIRES);
    beam.memRead(SET_PAGE1);
    beam.setCycleCounter(0);
    beam.beginVideoEventFrame();
    for (int y = kBandTop; y < 192; ++y) {
        beam.setCycleCounter(static_cast<uint64_t>(y) * 65 + 5);   // HBL → col 0
        beam.memRead(CLR_TEXT);              // graphics from column 0
        beam.setCycleCounter(static_cast<uint64_t>(y) * 65 + 45);  // hpos 45 → col 21 (mapping is `hpos - 24`)
        beam.memRead(SET_TEXT);              // text from column 20
    }
    const auto sBeam = signalOf(beam);

    // Top band: full-width HGR waveform.
    for (int y : {8, 40, 88})
        assert(spanEqual(sBeam, sHgr, y, 0, W) && "top band must be HGR");

    // Split band: LEFT 280 samples HGR, RIGHT 280 samples TEXT — same line.
    for (int y : {kBandTop, 104, 150, 191}) {
        assert(spanEqual(sBeam, sHgr, y, 0, kSplitSample)
               && "split line: left window must match the HGR signal");
        assert(spanEqual(sBeam, sText, y, kSplitSample, W)
               && "split line: right window must match the TEXT signal");
        assert(!spanEqual(sBeam, sText, y, 0, kSplitSample)
               && "split line: left window must NOT be TEXT");
        assert(!spanEqual(sBeam, sHgr, y, kSplitSample, W)
               && "split line: right window must NOT be HGR");
    }

    // ── The PUBLISHED frame, not the live one ────────────────────────────
    //
    // render() paints the frame Memory published. A soft switch thrown AFTER
    // that frame closed sits in Memory's recording log — not in the published
    // events — while the live DisplayState already carries it. Two places
    // read the live state anyway: render() itself fell back to it whenever
    // the published frame had no events (so a mode flip showed up a frame
    // early, on pixels that still belonged to the old mode), and
    // patchMixedTextBand polled it directly (so the bottom text band of a
    // mixed frame was skipped for that frame, leaving demodulated graphics
    // where the text should be).
    //
    // Two machines, identical up to and including a fully published mixed
    // frame; one of them then flips to TEXT the way a guest does at the top
    // of the next frame. The frame on screen must be the same for both.
    {
        constexpr uint16_t SET_MIXED = 0xC053;
        auto build = [](Memory& m) {
            populate(m);
            m.memRead(CLR_TEXT);
            m.memRead(SET_HIRES);
            m.memRead(SET_PAGE1);
            m.memRead(SET_MIXED);
            // Two whole NTSC video frames: frame 0 publishes the switches
            // above, frame 1 publishes with NO events at all — which is the
            // case that used to fall through to the live state.
            for (int i = 0; i < 2 * 262; ++i) m.advanceCycles(65);
        };
        Memory quiet;  build(quiet);
        Memory moved;  build(moved);
        moved.memRead(SET_TEXT);      // …the guest flips after the frame closed

        const Shot a = renderOf(quiet);
        const Shot b = renderOf(moved);
        assert(a.sig == b.sig
               && "the composite signal must describe the published frame");
        assert(a.w == b.w && a.h == b.h && a.pix == b.pix
               && "the framebuffer must too, mixed-mode text band included");

        // …and the comparison means something: the band really was painted.
        // Rows 160..191 are the text band over the demodulated graphics, and
        // a skipped patch leaves them black.
        bool bandInk = false;
        for (int y = 160; y < a.h && !bandInk; ++y)
            for (int x = 0; x < a.w; ++x)
                if ((a.pix[static_cast<size_t>(y) * a.w + x] & 0x00FFFFFFu) != 0) {
                    bandInk = true;
                    break;
                }
        assert(bandInk && "the mixed text band was never painted");
    }

    std::printf("horizontal_split_composite OK\n");
    return 0;
}
