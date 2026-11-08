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

// Per-kind mid-line soft-switch column offset.
//
// A mid-line soft switch's visible column is NOT the same for every switch
// kind. Raw `frameCycleToPos` maps a cycle to `byteCol = hpos - 24` (the
// 40-byte window opens at horizontal cycle 25). `beamColForEvent` then
// places each kind (2026-11-08):
//   - PAGE2 / 80STORE / HiRes ($C054/$C055, $C056/$C057): hpos - 24. They
//     change the ADDRESS the scanner fetches next.
//   - DHIRES / AN3 ($C05E/$C05F): hpos - 24 (re-interprets the byte now).
//   - TEXT / MIXED ($C050-$C053): hpos - 23, ONE cycle after a PAGE2 flip
//     on the same cycle. [A] tomcw, AppleWin GH#656, real //e PAL photos.
//   - 80COL ($C00C/$C00D): hpos - 24, with PAGE2 (2026-11-08). [A] logic
//     analyser on a real PAL //e rev B (Accurapple ls166.csv, see main()).
//
// [A] The -25 for PAGE2 and HiRes comes from French Touch's DIX, the release
// built for real PAL machines, measured with `grouik_sync_probe`:
//   - Crazy Cycles II part 1's 8-cell grid is whole only at -25.
//   - Mad Effect's silhouette edges admit an offset of exactly 25.
//   - OLDSKOOL FORT ET VERT's HiRes splits sat 2 columns off at -24.
// The user then checked OLDSKOOL, Mad Effect, Mad Effect 2 and CCII by eye in
// POM2: a visual check of the emulator, not a hardware measurement.
// [A] Since 2026-11-08 the HiRes/TEXT gap is also measured: a logic analyser
// on a real PAL //e rev B (S. Champailler, Accurapple logic_ana/ls166.csv @
// 69f45f3) shows mid-line LORES/HIRES reaching the LS166 on the 3rd LDPS after
// the access cycle and TEXT on the 4th — one byte apart, as pinned here.
//
// History:
//   - 2026-07-31: PAGE2 got -24 from a standalone MADEF.dsk of unknown
//     build.
//   - 2026-09-02: HiRes was pulled back to -24 against that same chain.
//
// This test pins the RELATIONSHIPS: HiRes splits on the same column as PAGE2,
// TEXT splits one column right of both, and all of them follow the base. A
// retune of the base therefore moves them together.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint16_t CLR_TEXT   = 0xC050;   // graphics
constexpr uint16_t SET_TEXT   = 0xC051;   // text    (one cycle late)
constexpr uint16_t SET_HIRES  = 0xC057;   // hi-res  (display-side)
constexpr uint16_t CLR_HIRES  = 0xC056;   // lo-res  (display-side)
constexpr uint16_t CLR_PAGE2  = 0xC054;   // page 1  (fetch-side)
constexpr uint16_t SET_PAGE2  = 0xC055;   // page 2  (fetch-side)

constexpr int W = 280;
constexpr int H = 192;
constexpr int kBandTop = 96;              // 12*8, row-aligned band start
constexpr int kMidHpos = 45;             // hpos 45 → raw col 21 (hpos-24)

uint16_t hgrAddr(int y, int page)
{
    return static_cast<uint16_t>((page == 2 ? 0x4000 : 0x2000)
        + 0x400 * (y & 7) + 0x80 * ((y >> 3) & 7) + 0x28 * (y >> 6));
}
uint16_t loresAddr(int row, int page)
{
    return static_cast<uint16_t>((page == 2 ? 0x0800 : 0x0400)
        + 0x80 * (row & 7) + 0x28 * (row >> 3));
}

// Content chosen for CLEAN, per-row-identical splits: the "left" state is
// solid BLACK and every "right" state is solid non-black, so the split column
// is an unambiguous black->colour transition on every scanline, unaffected by
// data patterns. HGR page 1 = 0x00 (black); HGR page 2 = 0x2A and lo-res
// $0400 = 0xAA are both non-black. The reference frame is all-left (all
// black), so `splitCol` finds the first non-black column = the split.
// The TEXT case fills text page 1 with $20 (inverse space, a solid cell on
// every scanline) instead of the lo-res $AA: '*' has blank top and bottom
// glyph rows, which is where the probed scanlines fall.
bool gSolidText = false;

void populate(Memory& m)
{
    for (int y = 0; y < 192; ++y)
        for (int col = 0; col < 40; ++col) {
            m.memWrite(hgrAddr(y, 1) + col, 0x00);                     // HIRES left: black
            m.memWrite(hgrAddr(y, 2) + col, static_cast<uint8_t>(0x2A)); // PAGE2 right: non-black HGR
        }
    for (int row = 0; row < 24; ++row)
        for (int col = 0; col < 40; ++col) {
            m.memWrite(loresAddr(row, 1) + col,                         // LORES right: non-black
                       static_cast<uint8_t>(gSolidText ? 0x20 : 0xAA));         // (TEXT: solid inverse)
            m.memWrite(loresAddr(row, 2) + col, static_cast<uint8_t>(0xAA));
        }
}

std::vector<uint32_t> frameOf(Memory& m)
{
    Apple2Display d;
    d.setAuxMemory(m.auxData());
    d.render(m);
    assert(d.width() == W && d.height() == H);
    const uint32_t* p = d.pixels();
    return std::vector<uint32_t>(p, p + static_cast<size_t>(W) * H);
}

// memcmp a horizontal pixel span [x0,x1) of scanline y between two frames.
bool spanEqual(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
               int y, int x0, int x1)
{
    for (int x = x0; x < x1; ++x)
        if (a[static_cast<size_t>(y) * W + x] != b[static_cast<size_t>(y) * W + x])
            return false;
    return true;
}

// Build a beam frame that starts graphics/`startA`/`startB` and, on every line
// of the lower band, re-asserts the left state then throws `midSwitch` at
// hpos 45 — a per-scanline mid-line split.
std::vector<uint32_t> beamSplit(uint16_t midSwitch, uint16_t startA, uint16_t startB)
{
    Memory m;
    populate(m);
    m.memRead(CLR_TEXT);
    m.memRead(startA);
    m.memRead(startB);
    m.setCycleCounter(0);
    m.beginVideoEventFrame();
    for (int y = kBandTop; y < H; ++y) {
        m.setCycleCounter(static_cast<uint64_t>(y) * 65 + 5);     // HBL
        m.memRead(startA);                                        // restore the FULL
        m.memRead(startB);                                        // left state (midSwitch
                                                                  // undid one of them)
        m.setCycleCounter(static_cast<uint64_t>(y) * 65 + kMidHpos);
        m.memRead(midSwitch);                                     // the split
    }
    return frameOf(m);
}

// A full-width frame held in one fixed state.
std::vector<uint32_t> fullFrame(uint16_t a, uint16_t b)
{
    Memory m; populate(m);
    m.memRead(CLR_TEXT); m.memRead(a); m.memRead(b);
    return frameOf(m);
}

} // namespace

// Assert `beam` is `leftRef` in the window [0, splitCol) and `rightRef` from
// [splitCol, 40) on every probed scanline — i.e. the mid-line switch splits
// EXACTLY at `splitCol`. Column-clean because the beam renderer paints each
// [col,col) segment independently (no cross-column composite fringe).
void assertSplitAt(const std::vector<uint32_t>& beam,
                   const std::vector<uint32_t>& leftRef,
                   const std::vector<uint32_t>& rightRef,
                   int splitCol, const char* what)
{
    const int px = splitCol * 7;
    for (int y : {kBandTop, 120, 150, 191}) {
        assert(spanEqual(beam, leftRef,  y, 0,  px) && what);
        assert(spanEqual(beam, rightRef, y, px, W) && what);
        // And it is a genuine split at THIS column, not one over: the column
        // just left of the seam is still the left state, the one just right is
        // the right state (fails if the split is off by a column either way).
        assert(!spanEqual(beam, rightRef, y, (splitCol - 1) * 7, px) &&
               "split is one column too far right");
        assert(!spanEqual(beam, leftRef,  y, px, px + 7) &&
               "split is one column too far left");
    }
}

int main()
{
    // Base mapping is unchanged: raw frameCycleToPos still says hpos 45 → 21.
    assert(Apple2Display::frameCycleToPos(kMidHpos).byteCol == 21);

    // References: solid-black HIRES page 1, non-black HIRES page 2, non-black
    // LORES — each a full-width frame in one fixed state.
    const auto refHiresBlack = fullFrame(SET_HIRES, CLR_PAGE2);   // black
    const auto refPage2      = fullFrame(SET_HIRES, SET_PAGE2);   // non-black HGR
    const auto refLores      = fullFrame(CLR_HIRES, CLR_PAGE2);   // non-black lo-res

    // RE-ANCHORED 2026-11-08 (evening): every kind one column right of its
    // earlier value, with the $C019 / //c VBLINT one-cycle lead ([A]
    // arekkusu42's BEAMPOS; Apple2Display_Beam.cpp beamColForEvent). For a
    // $C019-synced program the two cancel: DIX's corpus frames reappear pixel
    // for pixel, only at a different instant. The relationships pinned here
    // do not move.
    // ── PAGE2: hpos 45 - 24 = column 21. ─────────────────────────────────
    // Left = HIRES page 1 (black), right = HIRES page 2 (non-black).
    const auto beamPage2 = beamSplit(SET_PAGE2, SET_HIRES, CLR_PAGE2);
    assertSplitAt(beamPage2, refHiresBlack, refPage2, 21,
                  "PAGE2 mid-line split must land on hpos-24 (col 21)");
    std::printf("  PAGE2 splits at col 21\n");

    // ── HI-RES: the SAME column as PAGE2 (21). ────────────────────────────
    // Left = HIRES (black), right = LORES (non-black), same scanline.
    // $C056/$C057 changes the fetched address like PAGE2. OLDSKOOL FORT ET
    // VERT's only mid-line switches are HiRes at beam cycles 25/26, and it
    // lines up with its TV-set art only here.
    const auto beamHires = beamSplit(CLR_HIRES, SET_HIRES, CLR_PAGE2);
    assertSplitAt(beamHires, refHiresBlack, refLores, 21,
                  "HIRES mid-line split must land on PAGE2's column (21)");
    std::printf("  HIRES splits at col 21 (= PAGE2)\n");

    // ── TEXT: one column RIGHT of PAGE2 (22). ─────────────────────────────
    // Left = HIRES page 1 (black), right = TEXT page 1 ($AA cells). The
    // per-line restore re-asserts graphics, then TEXT is thrown at hpos 45.
    gSolidText = true;
    const auto beamText = beamSplit(SET_TEXT, CLR_TEXT, SET_HIRES);
    std::vector<uint32_t> refText;
    {
        Memory m; populate(m);
        m.memRead(SET_HIRES); m.memRead(CLR_PAGE2); m.memRead(SET_TEXT);
        refText = frameOf(m);
    }
    gSolidText = false;
    assertSplitAt(beamText, refHiresBlack, refText, 22,
                  "TEXT mid-line split must land one column right of PAGE2 "
                  "(hpos-23, col 22)");
    std::printf("  TEXT splits at col 22 (= PAGE2 + 1)\n");

    // DHIRES/AN3 also map to hpos-24. That mapping is validated against the
    // live demo rather than pinned here, because a clean AN3/DHGR mid-line
    // split needs a full double-res frame.

    // ── 80COL: the SAME column as PAGE2 (21). ─────────────────────────────
    // [A] Logic analyser on a real PAL //e rev B (S. Champailler, Accurapple
    //     `composite/tests/logic_ana/ls166.csv` @ 69f45f3, numbers read off
    //     2026-11-08): after a mid-line `STA $C00D` the first AUX load (the
    //     extra LDPS of PHASE 0) comes +1.73 us after the write cycle starts,
    //     and its MAIN partner is the 3rd LDPS — the byte a PAGE2 flip on that
    //     cycle reaches. `STA $C00C` drops the aux load at the same place.
    //     Before, 80COL took the default rule, one column late.
    // Rendered: main text page = normal spaces (black), aux text page =
    // inverse spaces (lit). 40 columns show main only and stay black; 80
    // columns light the aux half-cells, so the first lit pixel of a line is
    // the column the switch took effect.
    {
        Memory m;
        m.setIIEMode(true);
        m.resetSoftSwitches();
        uint8_t* aux = m.auxDataMutable();
        for (int row = 0; row < 24; ++row)
            for (int col = 0; col < 40; ++col) {
                m.memWrite(loresAddr(row, 1) + col, 0xA0);
                aux[loresAddr(row, 1) + col] = 0x20;
            }
        m.memRead(SET_TEXT);
        m.memWrite(0xC00C, 0);                                   // 80COL off
        m.setCycleCounter(0);
        m.beginVideoEventFrame();
        for (int y = kBandTop; y < H; ++y) {
            m.setCycleCounter(static_cast<uint64_t>(y) * 65 + 5);
            m.memWrite(0xC00C, 0);                               // HBL: 40 columns
            m.setCycleCounter(static_cast<uint64_t>(y) * 65 + kMidHpos);
            m.memWrite(0xC00D, 0);                               // the split
        }
        Apple2Display d;
        d.setAuxMemory(m.auxData());
        d.render(m);
        const int w = d.width();
        const uint32_t* px = d.pixels();
        const int perCol = w / 40;
        for (int y : {kBandTop, 120, 150, 191}) {
            int first = -1;
            for (int x = 0; x < w && first < 0; ++x)
                if ((px[static_cast<size_t>(y) * w + x] & 0x00FFFFFFu) != 0) first = x;
            if (first < 0 || first / perCol != 21) {
                std::printf("FAIL: 80COL mid-line switch lit from x=%d (col %d) on line %d; "
                            "the silicon says col 21 (= PAGE2)\n",
                            first, first < 0 ? -1 : first / perCol, y);
                return 1;
            }
        }
        std::printf("  80COL splits at col 21 (= PAGE2)\n");
    }

    // ── AN3 with 80COL OFF: one column LEFT of PAGE2 (20). ────────────────
    // In plain HGR, $C05E (AN3 off, DHIRES on) only masks bit 7's half-dot
    // delay, and that reaches the byte fetched in the access cycle itself.
    // [A] Logic analyser on a real PAL //e rev B (S. Champailler, Accurapple
    //     test_p_line88.lac @ 69f45f3, multi_tests.s test_P): the first byte
    //     delayed again after AN3ON is the 2nd LDPS from the access.
    // Every byte is $AA (bit 7 set): delayed on the left, not on the right.
    {
        auto shot = [](bool split, bool an3OffAll) {
            Memory m;
            m.setIIEMode(true);
            m.resetSoftSwitches();
            for (int y = 0; y < 192; ++y)
                for (int col = 0; col < 40; ++col)
                    m.memWrite(static_cast<uint16_t>(hgrAddr(y, 1) + col), 0xAA);
            m.memRead(CLR_TEXT); m.memRead(SET_HIRES); m.memRead(CLR_PAGE2);
            m.memRead(an3OffAll ? 0xC05E : 0xC05F);
            m.setCycleCounter(0);
            m.beginVideoEventFrame();
            if (split)
                for (int y = kBandTop; y < H; ++y) {
                    m.setCycleCounter(static_cast<uint64_t>(y) * 65 + 5);
                    m.memRead(0xC05F);                       // HBL: AN3 on
                    m.setCycleCounter(static_cast<uint64_t>(y) * 65 + kMidHpos);
                    m.memRead(0xC05E);                       // the split
                }
            return frameOf(m);
        };
        const auto delayed = shot(false, false), masked = shot(false, true);
        const auto beamAn3 = shot(true, false);
        if (spanEqual(delayed, masked, 120, 0, W)) {
            std::printf("FAIL: AN3 does not change an HGR line of bit-7 bytes (fixture)\n");
            return 1;
        }
        assertSplitAt(beamAn3, delayed, masked, 20,
                      "AN3 (80COL off) must split one column left of PAGE2 (col 20)");
        std::printf("  AN3 with 80COL off splits at col 20 (= PAGE2 - 1)\n");
    }

    std::printf("raster_switch_kind_offset: OK (HiRes = PAGE2 = 80COL = hpos-24; TEXT = PAGE2+1; AN3 w/o 80COL = PAGE2-1)\n");
    return 0;
}
