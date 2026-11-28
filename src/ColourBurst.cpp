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

// The per-scanline colour burst (ColourBurst.h for the silicon and grades).

#include "ColourBurst.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pom2 {

namespace {
constexpr uint64_t kLineCycles = 65;   // NTSC and PAL alike (CpuClock.h)

// The R-Y carrier through its reversal (ColourBurst.h kVSwitchTauStates),
// state by state: v relaxes toward the switch's target with e^(-1/tau) per
// state; `acc` integrates it over the burst states. Returns the line's ident.
struct VSwitch {
    float e   = std::exp(-1.0f / kVSwitchTauStates);
    float tau = kVSwitchTauStates;
};

float identOf(uint8_t mask, const VSwitch& sw)
{
    float v = 1.0f, target = 1.0f, acc = 0.0f;   // settled since the last line
    bool lowBefore = false;
    for (int i = 0; i < kBurstStates; ++i) {
        const bool low = (mask >> i) & 1u;
        if (low && !lowBefore) target = -target;   // the LS74 toggles on the fall
        if (low) acc += target + (v - target) * sw.tau * (1.0f - sw.e);
        v = target + (v - target) * sw.e;
        lowBefore = low;
    }
    return acc * target / static_cast<float>(kBurstStates);   // against the picture's phase
}
}

ColourBurstFrame computeColourBurst(bool textAtFrameStart, const TextEdge* edges,
                                    std::size_t edgeCount, int scanlinesPerFrame,
                                    const BurstRules& rules, bool mixedAtFrameStart)
{
    ColourBurstFrame f;
    const int lines = std::clamp(scanlinesPerFrame, 1, ColourBurstFrame::kMaxLines);
    f.lineCount = lines;
    constexpr uint8_t kAll = (1u << kBurstStates) - 1u;

    const VSwitch sw;
    // A Revision 0 ][ has no killer: the burst is there whatever TEXT says.
    if (!rules.textKillsBurst || edgeCount == 0) {
        const bool on = !rules.textKillsBurst || !textAtFrameStart;
        LineBurst lb{on ? kAll : uint8_t{0}, on ? uint8_t{1} : uint8_t{0},
                     on ? identOf(kAll, sw) : 0.0f};
        for (int y = 0; y < lines; ++y) {
            lb.trap = !textAtFrameStart && !(mixedAtFrameStart && y >= 160 && y < 192);
            f.lines[static_cast<std::size_t>(y)] = lb;
        }
        f.allFull = on;
        f.allNone = !on;
        return f;
    }

    // Each edge's position inside the frame, in CPU cycles from line 0 hpos 0
    // (Memory stamps `cycle / 65 mod lines` as the line). Sorted: the
    // display tests push events out of order.
    std::vector<TextEdge> at(edges, edges + edgeCount);
    const uint64_t frameCycles = kLineCycles * static_cast<uint64_t>(lines);
    for (auto& e : at) e.cycle %= frameCycles;
    std::stable_sort(at.begin(), at.end(),
                     [](const TextEdge& a, const TextEdge& b) { return a.cycle < b.cycle; });

    // Two views of the same edges: GR+2 for the trap (TEXT and MIXED, two
    // states late) and the burst gate's TEXT (`textLag` late). Each walks the
    // sorted list with its own cursor.
    bool text = textAtFrameStart, grText = textAtFrameStart, grMixed = mixedAtFrameStart;
    std::size_t next = 0, grNext = 0;
    bool full = true, none = true;
    for (int y = 0; y < lines; ++y) {
        LineBurst lb;
        const int64_t line0 = static_cast<int64_t>(y) * static_cast<int64_t>(kLineCycles);
        while (grNext < at.size() &&
               static_cast<int64_t>(at[grNext].cycle) <= line0 + kTrapClockHpos - kGrLagStates) {
            (at[grNext].mixed ? grMixed : grText) = at[grNext].text;
            ++grNext;
        }
        lb.trap = !grText && !(grMixed && y >= 160 && y < 192);
        bool lowBefore = false;   // CLRGATE' is high before the window (HBL·H3·H2 false)
        for (int i = 0; i < kBurstStates; ++i) {
            // The gate of state `h` sees TEXT as thrown `textLag` states earlier.
            const int64_t pos = line0 + rules.firstHpos + i - rules.textLag;
            while (next < at.size() && static_cast<int64_t>(at[next].cycle) <= pos) {
                if (!at[next].mixed) text = at[next].text;
                ++next;
            }
            const bool low = !text;
            if (low) lb.mask = static_cast<uint8_t>(lb.mask | (1u << i));
            if (low && !lowBefore) ++lb.falls;
            lowBefore = low;
        }
        lb.ident = identOf(lb.mask, sw);
        if (lb.mask != kAll) full = false;
        if (lb.mask != 0)    none = false;
        f.lines[static_cast<std::size_t>(y)] = lb;
    }
    f.allFull = full;
    f.allNone = none;
    return f;
}

} // namespace pom2
