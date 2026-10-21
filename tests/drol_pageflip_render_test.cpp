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

// Double-buffer page-flip rendering (DROL-class) — pins the one-direction
// PAGE2 heuristic in Apple2Display::forEachBeamSegment.
//
// DROL page-flips $C054/$C055 once per game-loop iteration, UNSYNCED to the
// beam (probe: tests/drol_probe.cpp — flips drift across the whole frame,
// 23/31 inside the visible band). Replaying such a flip at its raster
// position paints the band above it from the page the game has ALREADY
// started redrawing (we read RAM at render time, the real beam read it
// pristine) → half-erased sprites flicker. The fix: a frame whose PAGE2
// events all go one direction applies the FINAL page frame-wide. Frames
// that flip both directions (DIX MODPAGE beam racing) keep the exact
// replay — pinned separately by dix_modpage_split.
//
// That shortcut is now the fallback only: once the video write log
// (Memory.h § Video write log) is armed, the band above a flip is painted
// from the RAM the beam fetched, which is pinned at the end of main().

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t CLR_TEXT  = 0xC050;
constexpr uint16_t SET_HIRES = 0xC057;
constexpr uint16_t SET_PAGE1 = 0xC054;
constexpr uint16_t SET_PAGE2 = 0xC055;

constexpr int W = 280;
constexpr int H = 192;

void populate(Memory& mem)
{
    for (uint32_t a = 0x2000; a < 0x4000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(a & 0x7F));
    for (uint32_t a = 0x4000; a < 0x6000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(~(a * 3) & 0x7F));
}

void hgrMode(Memory& mem)
{
    mem.memRead(CLR_TEXT);
    mem.memRead(SET_HIRES);
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

}  // namespace

int main()
{
    // References: full-screen page 1 / page 2.
    Memory p1Ref; populate(p1Ref); hgrMode(p1Ref); p1Ref.memRead(SET_PAGE1);
    const auto fP1 = frameOf(p1Ref);
    Memory p2Ref; populate(p2Ref); hgrMode(p2Ref); p2Ref.memRead(SET_PAGE2);
    const auto fP2 = frameOf(p2Ref);
    assert(fP1 != fP2);

    // ── One mid-frame flip 1→2 (DROL pattern): the final page renders
    // frame-wide — NO split at line 100. ─────────────────────────────────
    {
        Memory mem; populate(mem); hgrMode(mem); mem.memRead(SET_PAGE1);
        mem.setCycleCounter(0);
        mem.beginVideoEventFrame();                      // frame-start = page 1
        mem.setCycleCounter(100 * 65 + 40);              // beam at line 100
        mem.memRead(SET_PAGE2);
        const auto f = frameOf(mem);
        assert(f == fP2 && "lone PAGE2 flip must apply frame-wide (no tear)");
    }

    // ── Same, opposite direction 2→1. ────────────────────────────────────
    {
        Memory mem; populate(mem); hgrMode(mem); mem.memRead(SET_PAGE2);
        mem.setCycleCounter(0);
        mem.beginVideoEventFrame();                      // frame-start = page 2
        mem.setCycleCounter(150 * 65 + 40);
        mem.memRead(SET_PAGE1);
        const auto f = frameOf(mem);
        assert(f == fP1 && "lone PAGE1 flip must apply frame-wide (no tear)");
    }

    // ── Both directions in one frame (beam racing) keeps the split: page 2
    // band between lines 96..144, page 1 elsewhere. ──────────────────────
    {
        Memory mem; populate(mem); hgrMode(mem); mem.memRead(SET_PAGE1);
        mem.setCycleCounter(0);
        mem.beginVideoEventFrame();
        mem.setCycleCounter(96 * 65 + 5);   mem.memRead(SET_PAGE2);  // HBL → col 0
        mem.setCycleCounter(144 * 65 + 5);  mem.memRead(SET_PAGE1);
        const auto f = frameOf(mem);
        auto row = [&](const std::vector<uint32_t>& img, int y) {
            return std::memcmp(f.data() + static_cast<size_t>(y) * W,
                               img.data() + static_cast<size_t>(y) * W,
                               W * sizeof(uint32_t)) == 0;
        };
        assert(row(fP1, 50)  && "above the band: page 1");
        assert(row(fP2, 120) && "inside the band: page 2");
        assert(row(fP1, 170) && "below the band: page 1");
    }

    // ── Beam-time RAM: the video write log (Memory.h § Video write log) ──
    // The real clock this time — advanceCycles publishes each 17030-cycle
    // frame. A frame with a soft-switch event arms the log; from the next
    // frame on the display paints each band from the RAM the beam fetched.
    auto advanceTo = [](Memory& mem, uint64_t cycle) {
        while (mem.getCycleCounter() < cycle) {
            const uint64_t left = cycle - mem.getCycleCounter();
            mem.advanceCycles(static_cast<int>(left < 1000 ? left : 1000));
        }
    };
    constexpr uint64_t kFrame = 17030;   // NTSC: 65 x 262
    auto row = [&](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int y) {
        return std::memcmp(a.data() + static_cast<size_t>(y) * W,
                           b.data() + static_cast<size_t>(y) * W,
                           W * sizeof(uint32_t)) == 0;
    };

    // DIRTBIKE (pom2games, 2026-10-21): flip 1 -> 2 at line 100, then start
    // redrawing page 1, now hidden. The band above the flip was fetched
    // from page 1 BEFORE the redraw: a real Apple II shows it intact (and
    // page 2 below — the tear), where POM2 painted the half-erased page.
    {
        Memory mem; populate(mem); hgrMode(mem); mem.memRead(SET_PAGE1);
        mem.setCycleCounter(0);
        advanceTo(mem, 50 * 65);
        mem.memRead(SET_PAGE2); mem.memRead(SET_PAGE1);   // arm: a frame with events
        advanceTo(mem, kFrame);                            // publish frame 0
        assert(mem.videoWriteLogArmed());
        advanceTo(mem, kFrame + 100 * 65 + 5);             // frame 1, line 100
        mem.memRead(SET_PAGE2);
        for (uint32_t a = 0x2000; a < 0x4000; ++a)         // erase the hidden page
            mem.memWrite(static_cast<uint16_t>(a), 0x00);
        advanceTo(mem, 2 * kFrame);                        // publish frame 1
        advanceTo(mem, 2 * kFrame + 3000);                 // the CPU runs on
        const uint8_t live = mem.memRead(0x2000);
        const auto f = frameOf(mem);
        assert(row(f, fP1, 50) && "above the flip: page 1 as the beam fetched it");
        assert(row(f, fP2, 150) && "below the flip: page 2");
        assert(mem.memRead(0x2000) == live && live == 0x00 &&
               "the paint put RAM back exactly");
    }

    // A write into the VISIBLE page while the beam is at line 100: line 50
    // was already fetched (old bytes), line 150 not yet (new bytes).
    {
        Memory mem; populate(mem); hgrMode(mem); mem.memRead(SET_PAGE1);
        mem.setCycleCounter(0);
        advanceTo(mem, 50 * 65);
        mem.memRead(SET_PAGE2); mem.memRead(SET_PAGE1);
        advanceTo(mem, kFrame);
        advanceTo(mem, kFrame + 10 * 65);
        mem.memRead(SET_PAGE1);                            // keep it armed
        advanceTo(mem, kFrame + 100 * 65 + 5);
        auto hgrRow = [](int y) {
            return static_cast<uint16_t>(0x2000 + (y & 7) * 0x400 +
                                         ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28);
        };
        for (int y : {50, 150})
            for (int c = 0; c < 40; ++c)
                mem.memWrite(static_cast<uint16_t>(hgrRow(y) + c), 0x00);
        advanceTo(mem, 2 * kFrame);
        const auto f = frameOf(mem);
        assert(row(f, fP1, 50) && "an already-scanned line keeps its old bytes");
        assert(!row(f, fP1, 150) && "a line still ahead of the beam shows the write");
    }

    std::printf("drol_pageflip_render OK\n");
    return 0;
}
