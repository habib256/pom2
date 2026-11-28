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

// Two-finger pinch zoom — the HOST sources (policy: PinchZoom.h).
//
// GLFW forwards no pinch on any platform, so each host is tapped where its
// gesture actually arrives, and every one feeds `pinch::router()` on the main
// thread:
//
//   macOS    PinchZoomHost_mac.mm — a local NSEvent monitor for
//            NSEventTypeMagnify (trackpad pinch) and NSEventTypeSmartMagnify
//            (two-finger double tap → fit). Two-finger SCROLL is a different
//            event type and keeps reaching GLFW untouched.
//   Windows  PinchZoomHost.cpp — a window procedure chained in front of
//            GLFW's (and ImGui's): WM_MOUSEWHEEL with MK_CONTROL (what a
//            precision touchpad sends for a pinch, injected Ctrl key or not)
//            and WM_GESTURE GID_ZOOM (a touch screen).
//   Browser  wasm/pinch_zoom.js — Safari's gesture* events and two touches
//            call `pom2_pinch_zoom` (exported from PinchZoomHost.cpp); a
//            `wheel` with ctrlKey (Chrome/Firefox/Edge trackpad pinch) is
//            diverted by main.cpp's wheel callback.
//   Linux    nothing to install: X11 has no pinch event and GLFW exposes no
//            Wayland pointer-gestures. A driver that turns a pinch into
//            Ctrl+wheel reaches pinchZoomNewFrame()'s Ctrl+wheel path, which
//            every platform shares.

struct GLFWwindow;

namespace pom2 {

/// Once, after ImGui_ImplGlfw_InitForOpenGL (on Windows the hook must chain
/// in front of the one ImGui's backend installs). No-op on Linux and in the
/// browser, where the sources need no window handle.
void installPinchZoomSources(GLFWwindow* window);

} // namespace pom2
