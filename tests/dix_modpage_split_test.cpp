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

// DIX "MODPAGE" mid-scanline technique — integration probe.
//
// The French Touch DIX anthology (Fr3nchT0uch/DIX, GPLv3) drives its beam-raced
// effects with a cycle-exact inner loop that, every 65-cycle scanline, does
//     MODPAGE  LDA $C054,X        ; PAGE1/PAGE2 select, mid-line
//     MODLINE  LDA $C056 (×11)    ; HIRES, mid-line
// so the LEFT and RIGHT halves of a single scanline display *different HGR
// pages*. This test reproduces that exact pattern against POM2's beam-racing
// (the horizontal mid-scanline split): a frame whose lower band re-flips
// $C054/$C055 every scanline — PAGE1 from byte column 0, PAGE2 from byte
// column 20 — must show page-1 HGR in the left 140 px and page-2 HGR in the
// right 140 px, ON THE SAME LINE.
//
// This validates the *rendering* half of DIX validation. The *timing* half
// (PAL 50 Hz, Mockingboard-T2 frame sync) is a separate, documented machine
// gap — see docs/test_corpus.md (DIX entry) and TODO.md.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t CLR_TEXT   = 0xC050;
constexpr uint16_t SET_HIRES  = 0xC057;
constexpr uint16_t SET_PAGE1  = 0xC054;
constexpr uint16_t SET_PAGE2  = 0xC055;

constexpr int W        = 280;
constexpr int H        = 192;
constexpr int kSplitPx = 20 * 7;       // 140 — hpos 45 → col 20 (`hpos - 25`, 2026-11-08)
constexpr int kBandTop = 96;

// Fill HGR page 1 ($2000-$3FFF) and page 2 ($4000-$5FFF) with DISTINCT
// deterministic data; filling every byte makes the exact HGR interleave the
// renderer uses irrelevant.
void populate(Memory& mem)
{
    for (uint32_t a = 0x2000; a < 0x4000; ++a)            // page 1
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(a & 0x7F));
    for (uint32_t a = 0x4000; a < 0x6000; ++a)            // page 2 (distinct)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(~(a * 3) & 0x7F));
}

void hgrMode(Memory& mem)
{
    mem.memRead(CLR_TEXT);    // graphics
    mem.memRead(SET_HIRES);   // hi-res
}

std::vector<uint32_t> frameOf(Memory& mem)
{
    Apple2Display d;
    d.setAuxMemory(mem.auxData());
    d.render(mem);
    assert(d.width() == W && d.height() == H);
    const uint32_t* p = d.pixels();
    return std::vector<uint32_t>(p, p + static_cast<size_t>(W) * H);
}

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
    // ── Reference: full-screen HGR page 1, and full-screen HGR page 2. ────
    Memory p1Ref; populate(p1Ref); hgrMode(p1Ref); p1Ref.memRead(SET_PAGE1);
    const auto fP1 = frameOf(p1Ref);

    Memory p2Ref; populate(p2Ref); hgrMode(p2Ref); p2Ref.memRead(SET_PAGE2);
    const auto fP2 = frameOf(p2Ref);

    // Sanity: the two pages differ in both windows on the probed rows.
    for (int y : {100, 150}) {
        assert(!spanEqual(fP1, fP2, y, 0, kSplitPx));
        assert(!spanEqual(fP1, fP2, y, kSplitPx, W));
    }

    // ── Beam-raced "MODPAGE": top band page 1, lower band a per-scanline
    // PAGE1|PAGE2 strip (page 1 in [0,20), page 2 in [20,40) on every line). ─
    Memory beam; populate(beam); hgrMode(beam); beam.memRead(SET_PAGE1);
    beam.setCycleCounter(0);
    beam.beginVideoEventFrame();             // frame-start = HGR page 1
    for (int y = kBandTop; y < H; ++y) {
        beam.setCycleCounter(static_cast<uint64_t>(y) * 65 + 5);   // HBL → col 0
        beam.memRead(SET_PAGE1);             // page 1 from column 0
        beam.setCycleCounter(static_cast<uint64_t>(y) * 65 + 45);  // hpos 45 → col 20 (PAGE2 maps to `hpos - 25`)
        beam.memRead(SET_PAGE2);             // page 2 from column 20
    }
    const auto fBeam = frameOf(beam);

    // Top band: full-width page 1.
    for (int y : {8, 40, 88})
        assert(spanEqual(fBeam, fP1, y, 0, W) && "top band must be full-width page 1");

    // Split band: LEFT 140 px page 1, RIGHT 140 px page 2 — same scanline.
    for (int y : {kBandTop, 104, 150, 191}) {
        assert(spanEqual(fBeam, fP1, y, 0, kSplitPx)
               && "MODPAGE line: left window must be HGR page 1");
        assert(spanEqual(fBeam, fP2, y, kSplitPx, W)
               && "MODPAGE line: right window must be HGR page 2");
        assert(!spanEqual(fBeam, fP2, y, 0, kSplitPx)
               && "MODPAGE line: left window must NOT be page 2");
        assert(!spanEqual(fBeam, fP1, y, kSplitPx, W)
               && "MODPAGE line: right window must NOT be page 1");
    }

    std::printf("dix_modpage_split OK\n");
    return 0;
}
