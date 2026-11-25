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

// ScreenInspector_ImGui — the screen as it is really displayed, to be read
// rather than watched (French Touch's request, 2026-11-25).
//
// A program that changes mode or page during the frame has no page in RAM
// that looks like its screen, so a page editor cannot answer "what is at this
// pixel". This panel shows the display's own picture, zoomed, and says for
// the pixel under the mouse: its coordinates, the line's video mode and page,
// the byte and bit, the address that byte was fetched from and its value.
// With the beam view on (Memory.h) the picture is the tube at this cycle and
// the beam is drawn on it. The state is PER BYTE, not per line: French Touch's
// demos flip TEXT, PAGE2 and HIRES inside scanlines and mix text, lo-res,
// hi-res and the double modes in one frame, so a gutter on the left shows
// each line's mode and page and marks the lines split inside. Two saves: the
// picture as a PNG, and the video pages "as fetched"
// (`Memory::captureFetchedPages`: hi-res, text / lo-res, and their aux
// halves), which the Paint editor and any Apple II tool can open.
//
// Locking: one short `lockState()` per frame for the per-line states and the
// byte under the mouse; the picture is the display's framebuffer, copied on
// the UI thread like the main screen's.

#ifndef POM2_SCREEN_INSPECTOR_IMGUI_H
#define POM2_SCREEN_INSPECTOR_IMGUI_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Apple2Display;
class EmulationController;

namespace pom2 {

class ScreenInspector_ImGui
{
public:
    ~ScreenInspector_ImGui();
    /// `savePng` is the application's screenshot action.
    /// `refreshDisplay` renders the display for this UI frame when nobody has
    /// yet (the Apple II Screen window may be hidden behind this panel's tab).
    void render(EmulationController& ctrl, Apple2Display& display, bool* open,
                const std::function<void()>& savePng,
                const std::function<void()>& refreshDisplay);


private:
    unsigned texture_ = 0;
    int      texW_ = 0, texH_ = 0;
    std::vector<uint32_t> pixels_;
    float    zoom_ = 2.5f;
    bool     fit_  = true;      ///< size the picture to the panel      ///< screen pixels per Apple line
    std::string status_;
    /// The line the band on top reads byte by byte: the last one under the
    /// mouse.
    int      bandLine_ = 0;
    bool     bandChosen_ = false;
};

} // namespace pom2

#endif // POM2_SCREEN_INSPECTOR_IMGUI_H
