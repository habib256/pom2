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

// WindowCapture — POM2 saving a picture of its OWN window, panels included.
//
// A development aid: the Apple screen already has a screenshot (F9, the AI
// server's /screen.ppm), but nothing showed the debugger or any other panel,
// and the operating system's capture needs a permission an automated session
// does not have (macOS Screen Recording). So the frame is read back from
// OpenGL, which needs none.
//
// Off unless $POM2_WINDOW_SHOT names a PNG file. Then, each frame, POM2
// looks for "<that file>.request": when it exists, the frame just drawn is
// written to the PNG (atomically) and the request file is removed — which is
// also how the asker knows the picture is ready. One stat() per frame while
// armed, nothing at all otherwise. Not built for the browser.

#ifndef POM2_WINDOW_CAPTURE_H
#define POM2_WINDOW_CAPTURE_H

namespace pom2 {

/// Call after the frame is drawn and BEFORE the buffers are swapped, with the
/// framebuffer's size in pixels.
void captureWindowIfRequested(int framebufferWidth, int framebufferHeight);

} // namespace pom2

#endif // POM2_WINDOW_CAPTURE_H
