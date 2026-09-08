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

// MouseSync — the closed-loop "absolute" cursor drive for the AppleWin HLE
// mouse card, as a policy with no GLFW and no card in it (same shape and
// same reason as MouseGrab.h / KeyChord.h: a headless test can link it).
//
// The loop: the host pointer's position inside the screen widget is
// projected onto the firmware's clamp window `[iMinX..iMaxX] × [iMinY..iMaxY]`
// and the UI pushes the delta that drives the card's `iX/iY` toward that
// target. The card applies the delta on the CPU thread, when it next drains
// its host shadow (`MouseCardAppleWin::pollHostInput`, once per generation).
//
// THE JUMP (2026-09-08, A2FILECMD). The UI computed every delta against the
// `iX/iY` of the snapshot it had just taken — which is the position BEFORE
// the previous push has been drained whenever host events arrive faster
// than the CPU thread consumes them (two pointer events inside one
// 4096-cycle chunk is routine on a 120 Hz panel). Event 1 pushed
// `T1 - iX0`; event 2, seeing the same stale `iX0`, pushed `T2 - iX0` on top
// of it: the card then moved by `T1 + T2 - 2·iX0`, overshooting by
// `T1 - iX0`, and the next event pulled it back. On screen: a cursor that
// follows the host pointer and keeps jumping past it. The rule here closes
// that window — a correction is pushed only once the previous one has been
// consumed; until then the event is skipped (the target is absolute, the
// next event catches up).

#ifndef POM2_MOUSE_SYNC_H
#define POM2_MOUSE_SYNC_H

#include <algorithm>

namespace pom2 {
namespace mousesync {

/// What the UI knows about the card at the moment of a pointer event.
struct CardState {
    int  iX = 0, iY = 0;            ///< firmware cursor, clamp-window units
    int  iMinX = 0, iMaxX = 0;      ///< firmware clamp window
    int  iMinY = 0, iMaxY = 0;
    bool mouseOn = false;           ///< MODE_MOUSE_ON latched by the firmware
    bool hostDrained = true;        ///< the previous push has been consumed
};

struct Delta {
    int dx = 0;
    int dy = 0;
};

enum class Decision {
    NotApplicable,   ///< mouse off or no clamp window: use the relative drive
    Skip,            ///< a push is still in flight: send nothing this event
    Push,            ///< `out` carries the (±127-bounded) correction
};

/// `fracX/fracY` are the pointer's position inside the widget, 0..1
/// (callers saturate outside it so leaving the widget pins the Apple cursor
/// at the matching edge). Each push is bounded to ±127 — the MCU's signed
/// 8-bit wrap range — so a large gap converges over several drained events.
inline Decision absoluteDelta(double fracX, double fracY, const CardState& s,
                              Delta& out)
{
    const int rangeX = s.iMaxX - s.iMinX;
    const int rangeY = s.iMaxY - s.iMinY;
    if (!s.mouseOn || rangeX <= 0 || rangeY <= 0) return Decision::NotApplicable;
    if (!s.hostDrained) return Decision::Skip;
    const double fx = std::clamp(fracX, 0.0, 1.0);
    const double fy = std::clamp(fracY, 0.0, 1.0);
    const int targetX = s.iMinX + static_cast<int>(fx * rangeX + 0.5);
    const int targetY = s.iMinY + static_cast<int>(fy * rangeY + 0.5);
    out.dx = std::clamp(targetX - s.iX, -127, 127);
    out.dy = std::clamp(targetY - s.iY, -127, 127);
    return Decision::Push;
}

} // namespace mousesync
} // namespace pom2

#endif // POM2_MOUSE_SYNC_H
