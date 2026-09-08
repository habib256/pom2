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

// The closed-loop cursor sync must never overshoot the host pointer.
//
// A model of the loop: the UI sees the card's position through a snapshot,
// pushes a correction, and the card applies it later, on its own thread —
// here, every SECOND pointer event. With the correction computed against a
// stale position (the old rule) the second event stacked a second copy of
// the same move and the cursor jumped past the target (A2FILECMD, 2026-09-08).
// With MouseSync's rule — push only once the previous push is drained — the
// position walks monotonically to the target and stops there.

#include "MouseSync.h"

#include <cassert>
#include <cstdio>

int main()
{
    using namespace pom2::mousesync;
    // The card: a 0..1023 window, cursor at 0, one push in flight at most.
    CardState s;
    s.iMinX = 0; s.iMaxX = 1023; s.iMinY = 0; s.iMaxY = 1023;
    s.mouseOn = true;
    int pendingX = 0, pendingY = 0;
    bool inFlight = false;

    // Host pointer parked at 40 % of the widget: target (409, 409).
    const double frac = 0.4;
    const int target = 0 + static_cast<int>(frac * 1023 + 0.5);
    int maxX = 0, pushes = 0, skips = 0;
    for (int event = 0; event < 40; ++event) {
        s.hostDrained = !inFlight;
        Delta d;
        const Decision dec = absoluteDelta(frac, frac, s, d);
        if (dec == Decision::Push) {
            ++pushes;
            pendingX = d.dx; pendingY = d.dy; inFlight = true;
        } else if (dec == Decision::Skip) {
            ++skips;
        }
        // The CPU thread drains every other event.
        if (event % 2 == 1 && inFlight) {
            s.iX += pendingX; s.iY += pendingY;
            inFlight = false;
        }
        if (s.iX > maxX) maxX = s.iX;
        assert(s.iX <= target && "the cursor must never overshoot the pointer");
    }
    assert(s.iX == target && s.iY == target && "…and must reach it");
    assert(skips > 0 && "events while a push is in flight are skipped, not stacked");
    assert(pushes >= 4 && "a 409-unit gap converges over several ±127 pushes");

    // Mouse off, or no clamp window: not this policy's business.
    Delta d;
    CardState off = s; off.mouseOn = false;
    assert(absoluteDelta(0.5, 0.5, off, d) == Decision::NotApplicable);
    CardState flat = s; flat.iMaxX = flat.iMinX;
    assert(absoluteDelta(0.5, 0.5, flat, d) == Decision::NotApplicable);
    // Outside the widget saturates to the matching edge.
    CardState edge = s; edge.iX = 1000; edge.hostDrained = true;
    assert(absoluteDelta(2.0, -1.0, edge, d) == Decision::Push);
    assert(d.dx == 23 && d.dy == -127);

    std::printf("mouse_sync_policy: %d pushes, %d skips, max %d, target %d: OK\n",
                pushes, skips, maxX, target);
    return 0;
}
