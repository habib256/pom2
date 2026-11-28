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

#pragma once

// Two-finger pinch zoom — the ImGui side (policy: PinchZoom.h).
//
// Two calls. The frame loop calls `pinchZoomNewFrame()` right after
// `ImGui::NewFrame()`: a Ctrl+wheel becomes a pinch (and stops being a wheel,
// so nothing also scrolls), the queued gesture becomes this frame's, and the
// window under its focal point is resolved with ImGui's own z-order-aware
// hit-test. A zoomable view calls `pinchZoomTake(min, max, g)` once a frame,
// from inside the window it draws in, with its zoomable rect; it gets the
// gesture when that rect is the one under the fingers.

#include "PinchZoom.h"

#include "imgui.h"

namespace pom2 {

/// After ImGui::NewFrame(), once per frame, main thread.
void pinchZoomNewFrame();

/// This frame's pinch, if it is aimed at [min, max) of the CURRENT window
/// (or a child window drawn over that rect). At most one caller a frame
/// gets it. Zoom about `g.focal` by `g.factor()`; `g.fit` = back to fit.
bool pinchZoomTake(const ImVec2& min, const ImVec2& max, pinch::Gesture& g);

} // namespace pom2
