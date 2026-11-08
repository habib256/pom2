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

// $C019 (RDVBLBAR) intra-line phase — the edge is NOT on a line boundary.
//
// Why this test exists
// --------------------
// French Touch's "MAD EFFECT" (Apple IIe PAL 128K + Mockingboard, 2019)
// derives its whole frame sync from the VBL'→DISPLAY edge of $C019, then
// counts a fixed number of cycles to land on cycle 0 of the first display
// line. Its source (`Sources/main.a`, shipped GPLv3 alongside the disk in
// `disks_5.4/demo/madef/`) states the hardware contract verbatim:
//
//     ; WARNING: DISPLAY detected (VERTBLANK <0) from cycle #52 of last
//     ; line (#311) of VBL
//     ; so BMI not taken (LDA VERTBLANK occurs at cycle #51 of line 311)
//     ...                                        ; line 311 / cycle 54
//     NOP : NOP : NOP : NOP  : LDA $EA           ; +11
//                                                ; = 65
//     ; line 0 (display) / cycle 0
//
// i.e. the flag goes "display" 13 cycles BEFORE the first display line
// starts, and the demo spends exactly those 13 cycles getting to cycle 0.
// An emulator that flips the flag on the 65-cycle line boundary hands the
// demo a beam position 13 cycles off, and every beam-raced split lands in
// the wrong column for the rest of the frame.
//
// ANCHOR: an alternative reading — the demo's "cycle 0" being the first
// VISIBLE byte, which would put the edge at hpos 12 of line 0 — was tried
// and rejected. It did not move the demo's page-flips to the expected
// column, and it breaks pal_timing/vbl_smoke, which require line 192 to
// read VBL from its very first cycle.
//
// POM2 used to derive the flag from the scanline number alone
// (`scanline = now / 65; active = scanline < 192`), which puts the edge at
// hpos 0 with no intra-line phase at all. This test pins the phase.
//
// The same horizontal offset governs BOTH edges (VBL'→display and
// display→VBL'): it is a property of when the vertical counter advances
// within a scanline, so it does not depend on the video standard.
//
// Reference frame: POM2's own beam convention, documented in
// `Apple2Display::frameCycleToPos` — "the 40-byte visible window opens at
// horizontal cycle 25; the first 25 cycles of each scanline are horizontal
// blanking". hpos 52 is therefore 27 bytes into the visible window.

#include "CpuClock.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Edge {
    uint64_t cycle;
    int      line;
    int      hpos;
    bool     toDisplay;   // true = VBL'→DISPLAY (bit7 0→1)
};

bool displayFlag(Memory& mem, uint64_t cycle)
{
    mem.setCycleCounter(cycle);
    return (mem.memRead(0xC019) & 0x80) != 0;
}

// Walk one whole frame cycle by cycle and record every bit-7 transition.
std::vector<Edge> edgesOf(VideoStandard std)
{
    const VideoTiming& t = pom2VideoTiming(std);
    const uint64_t lineCycles  = static_cast<uint64_t>(t.cyclesPerScanline);
    const uint64_t frameCycles = lineCycles *
                                 static_cast<uint64_t>(t.scanlinesPerFrame);

    Memory mem;
    mem.setIIEMode(true);
    mem.setVideoStandard(std);

    std::vector<Edge> edges;
    // Seed from the LAST cycle of the frame so an edge sitting exactly on
    // the frame wrap (cycle 0) is still seen — that is precisely where the
    // un-phased implementation puts it, and missing it would report one
    // edge instead of two.
    bool prev = displayFlag(mem, frameCycles - 1);
    for (uint64_t c = 0; c < frameCycles; ++c) {
        const bool cur = displayFlag(mem, c);
        if (cur != prev) {
            edges.push_back({c,
                             static_cast<int>(c / lineCycles),
                             static_cast<int>(c % lineCycles),
                             cur});
        }
        prev = cur;
    }
    return edges;
}

void check(VideoStandard std, const char* name, int lastVblLine)
{

    const auto edges = edgesOf(std);
    std::printf("%s: %zu edge(s)\n", name, edges.size());
    for (const auto& e : edges)
        std::printf("  cycle %6llu  line %3d  hpos %2d  -> %s\n",
                    static_cast<unsigned long long>(e.cycle),
                    e.line, e.hpos, e.toDisplay ? "DISPLAY" : "VBL");

    // Exactly two transitions per frame: into VBL' and back out.
    assert(edges.size() == 2 && "expected exactly one VBL entry + one exit");

    const Edge* toVbl     = nullptr;
    const Edge* toDisplay = nullptr;
    for (const auto& e : edges) (e.toDisplay ? toDisplay : toVbl) = &e;
    assert(toVbl && toDisplay);

    // A READ of $C019 sees each edge ONE cycle before the line boundary
    // (2026-11-08): the CPU reads in PHASE 0, after the scanner's counters
    // advanced, while the floating bus of the same cycle was fetched before.
    // [A] arekkusu42's BEAMPOS (VidModes disk, 2026-04-21) checks exactly
    // this against the floating bus and failed by one cycle without it, on
    // every //e and on the //c. That one cycle is the whole lead: the larger
    // intra-line phases tried against MAD EFFECT (note at the top) stay
    // rejected, and the display's switch columns moved with it so MAD
    // EFFECT's placements do not change. In beam coordinates the reads below
    // change on the last cycle of the line BEFORE the boundary.
    assert(toDisplay->line == lastVblLine &&
           "VBL'→DISPLAY must read on the last cycle of the last VBL line");
    assert(toDisplay->hpos == 64 &&
           "one cycle ahead of the line boundary, no more");

    // Entering VBL': the last cycle of line 191.
    assert(toVbl->line == 191 &&
           "DISPLAY→VBL' must read on the last cycle of line 191");
    assert(toVbl->hpos == 64 &&
           "both edges share the same intra-line phase");
}

}  // namespace

int main()
{
    // PAL: 312 lines, last VBL line = 311 (the line MAD EFFECT names).
    check(VideoStandard::PAL, "PAL ", 311);
    // NTSC: 262 lines, same horizontal phase on its own last VBL line.
    check(VideoStandard::NTSC, "NTSC", 261);

    std::printf("vbl_edge_phase OK\n");
    return 0;
}
