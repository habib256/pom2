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

// SlowMotion — the speeds below 1x, in ONE table shared by the toolbar's
// speed menu and the command palette (French Touch's request, 2026-11-25:
// watch a graphics effect being drawn).
//
// A speed is a cycle budget per worker tick (EmulationController::
// setCyclesPerFrame); 1x is one video frame of cycles. A slow speed is that
// budget scaled down, never below one cycle. POM2 advances by whole
// instructions, each with its exact cycle count: at 0.1 % a tick's budget is
// about 20 cycles, so the machine moves a few instructions per tick, not one
// cycle at a time. Pair it with the beam view (Memory.h) to see the frame in
// progress rather than the last completed one.

#ifndef POM2_SLOW_MOTION_H
#define POM2_SLOW_MOTION_H

#include <cstddef>

namespace pom2 {

struct SlowSpeed {
    const char* id;       ///< command-palette id suffix ("speed.<id>")
    const char* label;    ///< menu label
    double      factor;   ///< of 1x
};

inline constexpr SlowSpeed kSlowSpeeds[] = {
    {"50",   "50 %",    0.5},
    {"25",   "25 %",    0.25},
    {"10",   "10 %",    0.1},
    {"1",    "1 %",     0.01},
    {"025",  "0.25 %",  0.0025},
    {"01",   "0.1 %",   0.001},
};
inline constexpr std::size_t kSlowSpeedCount = sizeof(kSlowSpeeds) / sizeof(kSlowSpeeds[0]);

/// The tick budget of a slow speed for a machine whose 1x budget is
/// `cycles1x`: rounded, and at least one cycle.
inline constexpr int slowSpeedBudget(int cycles1x, double factor)
{
    const int n = static_cast<int>(static_cast<double>(cycles1x) * factor + 0.5);
    return n < 1 ? 1 : n;
}

} // namespace pom2

#endif // POM2_SLOW_MOTION_H
