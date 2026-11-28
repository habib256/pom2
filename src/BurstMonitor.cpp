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

// Monitor response to the per-line burst (BurstMonitor.h for the grades).

#include "BurstMonitor.h"

#include <algorithm>
#include <cmath>

namespace pom2 {

bool NtscColourKiller::observe(const LineBurst& b)
{
    // Gated burst states: window positions kGateFirst.. (hpos 15-16).
    const int gated = ((b.mask >> kGateFirst) & 1) + ((b.mask >> (kGateFirst + 1)) & 1);
    // GSSquared's burst_cycles is the GATED count: a burst that falls wholly
    // before the gate (hpos 13-14 only) decays like no burst at all.
    if (gated > 0) level = std::min(level + gated * kPerCycle, kMaxLevel);
    else           level = std::max(level - kDecay, 0);
    if (!colour && level > kOnLevel)       colour = true;
    else if (colour && level < kOffLevel)  colour = false;
    return colour;
}

LineChroma PalDecoder::observe(const LineBurst& b)
{
    // The decoder's bistable steps every line, the Apple's once per CLRGATE'
    // fall: an even count leaves the two one step apart.
    if ((b.falls & 1u) == 0) relative = -relative;
    static const float a = std::exp(-1.0f / kIdentLines);
    ident = ident * a + (1.0f - a) * b.ident * static_cast<float>(relative);
    if (ident < -kFlip) {               // identification sets the bistable
        relative = -relative;
        ident    = -ident;
    }
    if (!colour && ident > kOn)        colour = true;
    else if (colour && ident < kOff)   colour = false;
    const int v = relative + prevRelative;   // the delay line: this line + the last
    prevRelative = relative;
    if (!colour) return LineChroma::Killed;
    if (v > 0) return LineChroma::Normal;
    if (v < 0) return LineChroma::VInverted;
    return LineChroma::VCancelled;
}

void monitorLineChroma(const ColourBurstFrame& frame, VideoStandard standard,
                       std::array<LineChroma, 192>& out,
                       std::array<int8_t, 192>* relative)
{
    const int lines = frame.lineCount;
    out.fill(LineChroma::Normal);
    if (relative) relative->fill(1);
    if (lines <= 0) return;
    if (frame.allFull) return;                       // steady state: colour on, in step
    if (frame.allNone) {
        out.fill(LineChroma::Killed);
        if (relative) relative->fill(0);
        return;
    }

    if (standard == VideoStandard::NTSC) {
        NtscColourKiller k;
        for (int pass = 0; pass < 3; ++pass)         // steady state (header)
            for (int y = 0; y < lines; ++y)
                k.observe(frame.lines[static_cast<std::size_t>(y)]);
        for (int y = 0; y < 192 && y < lines; ++y) {
            const bool c = k.observe(frame.lines[static_cast<std::size_t>(y)]);
            out[static_cast<std::size_t>(y)] = c ? LineChroma::Normal : LineChroma::Killed;
            if (relative) (*relative)[static_cast<std::size_t>(y)] = c ? 1 : 0;
        }
        return;
    }

    PalDecoder d;
    for (int pass = 0; pass < 3; ++pass)
        for (int y = 0; y < lines; ++y) d.observe(frame.lines[static_cast<std::size_t>(y)]);
    for (int y = 0; y < 192 && y < lines; ++y) {
        out[static_cast<std::size_t>(y)] = d.observe(frame.lines[static_cast<std::size_t>(y)]);
        if (relative)
            (*relative)[static_cast<std::size_t>(y)] =
                static_cast<int8_t>(d.colour ? d.relative : 0);
    }
}

} // namespace pom2
