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
// the beam is drawn on it — a CROSS over the whole picture in two tones (a
// dark edge under a colour of the user's choice), because any single colour
// is one the Apple II can put on screen (French Touch, 2026-11-27); in
// blanking, an arrow on the edge the beam is behind. A right click on the
// picture runs the machine until the beam reaches that point, and the frame's
// soft-switch throws are listed with the V and H each was made at. The state is PER BYTE, not per line: French Touch's
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
    /// `showInListing` brings the Debugger's listing to an address: the
    /// instruction that threw a soft switch, from the switch list.
    void render(EmulationController& ctrl, Apple2Display& display, bool* open,
                const std::function<void()>& savePng,
                const std::function<void()>& refreshDisplay,
                const std::function<void(uint16_t)>& showInListing);

    /// The beam marker's colour, packed as ImGui packs it (IM_COL32); kept
    /// in the settings by MainWindow (`screen_inspector_beam_colour`).
    uint32_t beamColour() const { return beamColour_; }
    void setBeamColour(uint32_t colour) { beamColour_ = colour | 0xFF000000u; }

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
    uint32_t beamColour_ = 0xFFFF00FFu;     ///< magenta, opaque
    /// The point of the picture a right click chose to run the beam to.
    int      runLine_ = 0, runColumn_ = 0;
    bool     showSwitches_ = false;         ///< the frame's soft-switch list
    /// 64 KiB the disassembler reads from: only the few bytes of each
    /// throwing instruction are filled, each frame the list is shown.
    std::vector<uint8_t> codeScratch_;
    /// The throw a right click in that list chose (the list is rebuilt
    /// every frame: a menu bound to a ROW would change target under it).
    int      menuPc_ = -1, menuLine_ = 0, menuH_ = 0;
};

} // namespace pom2

#endif // POM2_SCREEN_INSPECTOR_IMGUI_H
