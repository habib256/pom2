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

// Beam-racing the framebuffer HORIZONTALLY — pins that renderBeamRacing()
// honours a mid-scanline (per-byte-column) soft-switch split, not just the
// scanline-quantized vertical bands. A frame whose lower half re-flips $C050/
// $C051 every scanline — graphics from byte column 0, text from byte column
// 20 — must paint, ON THE SAME LINE, HGR in the left 140 px and TEXT in the
// right 140 px. The pre-horizontal code (full-width bands) could only ever
// make a whole scanline one mode or the other.
//
// Plan: TODO.md [Display] "Split horizontal mid-scanline" step 4.

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

constexpr int W       = 280;          // legacy 280-wide framebuffer
constexpr int H       = 192;
// Byte column where text takes over: hpos 45 → col 22. TEXT lands one column right of the `hpos - 24` page mapping — AppleWin GH#656 / MAME PR #15247 photos
// (Apple2Display_Beam.cpp beamColForEvent). A PAGE2 flip at hpos 45 would land at 21.
constexpr int kSplitCol  = 22;
constexpr int kSplitPx   = kSplitCol * 7;   // 154
constexpr int kBandTop   = 96;        // row-aligned (12 * 8): split band start

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

// Identical, deterministic text + HGR contents in every Memory we build, so
// the only difference between frames is the *mode* (and where it changes),
// never the data.
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

// Render `mem` through a fresh display in the default ColorNTSC mode (legacy
// 280-wide path) and copy out the RGBA framebuffer.
std::vector<uint32_t> frameOf(Memory& mem)
{
    Apple2Display d;
    d.setAuxMemory(mem.auxData());
    d.render(mem);
    assert(d.width() == W && d.height() == H && "expected the 280-wide path");
    const uint32_t* p = d.pixels();
    return std::vector<uint32_t>(p, p + static_cast<size_t>(W) * H);
}

// memcmp a horizontal pixel span [x0, x1) of scanline `y` between two frames.
bool spanEqual(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
               int y, int x0, int x1)
{
    return std::memcmp(a.data() + static_cast<size_t>(y) * W + x0,
                       b.data() + static_cast<size_t>(y) * W + x0,
                       static_cast<size_t>(x1 - x0) * sizeof(uint32_t)) == 0;
}

} // namespace

int main()
{
    // ── frameCycleToPos unit check (plan step 1) ──────────────────────────
    // 65 cycles/scanline; 25-cycle HBL, then 40 visible bytes — but the
    // switch→column mapping is `hpos - 24`, one cycle earlier than the raw
    // window offset: the scanner latches column c's byte in phi1 of the
    // cycle whose phi2 the CPU uses, so a switch there first shows at c+1.
    // Measured against French Touch MAD EFFECT (2026-07-31, see CHANGELOG).
    assert(Apple2Display::frameCycleToPos(0).byteCol == 0);          // HBL → col 0
    assert(Apple2Display::frameCycleToPos(23).byteCol == 0);         // still HBL
    assert(Apple2Display::frameCycleToPos(24).byteCol == 0);         // last HBL cyc
    assert(Apple2Display::frameCycleToPos(25).byteCol == 1);         // first visible → c+1
    assert(Apple2Display::frameCycleToPos(45).byteCol == 21);        // mid-line
    assert(Apple2Display::frameCycleToPos(64).byteCol == 40);        // clamp at window end
    assert(Apple2Display::frameCycleToPos(96 * 65 + 45).scanline == 96);
    assert(Apple2Display::frameCycleToPos(96 * 65 + 45).byteCol  == 21);

    // ── Reference 1: a pure graphics+HIRES frame. ────────────────────────
    Memory hgrRef;
    populate(hgrRef);
    hgrRef.memRead(CLR_TEXT);
    hgrRef.memRead(SET_HIRES);
    hgrRef.memRead(SET_PAGE1);
    const auto fHgr = frameOf(hgrRef);

    // ── Reference 2: a pure TEXT frame. ──────────────────────────────────
    Memory textRef;
    populate(textRef);
    textRef.memRead(SET_TEXT);
    textRef.memRead(SET_PAGE1);
    const auto fText = frameOf(textRef);

    // Sanity: HGR and TEXT pixels differ in BOTH the left and right windows on
    // the rows we probe — else the test can't tell the two modes apart.
    for (int y : {100, 150}) {
        assert(!spanEqual(fHgr, fText, y, 0, kSplitPx));
        assert(!spanEqual(fHgr, fText, y, kSplitPx, W));
    }

    // ── Beam-raced frame: top half HGR, lower half a per-scanline strip ──
    // (graphics in [0,20), text in [20,40) on every line from kBandTop down).
    Memory beam;
    populate(beam);
    beam.memRead(CLR_TEXT);     // frame-start state = graphics + HIRES
    beam.memRead(SET_HIRES);
    beam.memRead(SET_PAGE1);
    beam.setCycleCounter(0);
    beam.beginVideoEventFrame();             // captures HGR as frame-start state
    for (int y = kBandTop; y < H; ++y) {
        beam.setCycleCounter(static_cast<uint64_t>(y) * 65 + 5);   // HBL → byteCol 0
        beam.memRead(CLR_TEXT);              // graphics from column 0
        beam.setCycleCounter(static_cast<uint64_t>(y) * 65 + 45);  // hpos 45 → TEXT from col 22 (`hpos - 23`)
        beam.memRead(SET_TEXT);              // text from column 20
    }
    const auto fBeam = frameOf(beam);

    // Top band (above the split): full-width HGR, unchanged.
    for (int y : {8, 40, 88}) {
        assert(spanEqual(fBeam, fHgr, y, 0, W)
               && "top band must be full-width HGR");
    }

    // Split band: LEFT 140 px is HGR (frame-start state), RIGHT 140 px is TEXT
    // — on the SAME scanline. This is the horizontal mid-scanline split.
    for (int y : {kBandTop, 104, 150, 191}) {
        assert(spanEqual(fBeam, fHgr, y, 0, kSplitPx)
               && "split line: left window must match the HGR reference");
        assert(spanEqual(fBeam, fText, y, kSplitPx, W)
               && "split line: right window must match the TEXT reference");
        // And it is a genuine split: left is NOT text, right is NOT graphics.
        assert(!spanEqual(fBeam, fText, y, 0, kSplitPx)
               && "split line: left window must NOT be TEXT");
        assert(!spanEqual(fBeam, fHgr, y, kSplitPx, W)
               && "split line: right window must NOT be HGR");
    }

    std::printf("horizontal_split_smoke OK\n");
    return 0;
}
