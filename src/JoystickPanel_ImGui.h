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

// JoystickPanel_ImGui — single-device joystick configuration panel.
// One combo box picks which host pad feeds the Apple II game port.
// Below: deadzone, invert X/Y, live axis bars + button LEDs, and an
// Apple II side mirror so the user can verify what BASIC will read
// from PADL(...) and the PB$Cs.

#ifndef POM2_JOYSTICK_PANEL_IMGUI_H
#define POM2_JOYSTICK_PANEL_IMGUI_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class JoystickPanel_ImGui
{
public:
    static constexpr int kAxes    = 4;
    static constexpr int kButtons = 3;
    static constexpr int kRawAxes = 8;

    /// One entry per detected GLFW host joystick. Used to populate the
    /// combo and the read-only "all detected devices" overflow list.
    struct HostDevice {
        int         index = -1;
        std::string name;
        std::array<float, kAxes>    axis{};
        std::array<float, 2>        trigger{ -1.0f, -1.0f };  // L2, R2
        std::array<float, kRawAxes> raw{};
        int                         rawCount = 0;
        std::array<bool,  kButtons> buttons{};
    };

    struct Snapshot {
        std::vector<HostDevice> hosts;

        // Active binding, mirrored from JoystickInput::Binding.
        int   hostIdx  = -1;
        float deadzone = 0.10f;
        std::array<bool, kAxes> invert{ false, false };
        bool  squareGate = true;
        /// Per paddle, a JoystickInput::Source code.
        std::array<int, kAxes> source{ 1, 2, 3, 4 };

        // Live Apple-II-side values for confirmation.
        std::array<uint8_t, 4> appleIIPaddle{ 128, 128, 128, 128 };
        std::array<bool,    3> appleIIButton{ false, false, false };
    };

    struct FrameResult {
        bool  changed  = false;
        int   hostIdx  = -1;
        float deadzone = 0.10f;
        std::array<bool, kAxes> invert{ false, false };
        bool  squareGate = true;
        std::array<int, kAxes> source{ 1, 2, 3, 4 };
    };

    FrameResult render(const char* title, bool& open, const Snapshot& snap);

private:
    // "Detect": the paddle whose source is being learned (-1 = none), and
    // the bound pad's raw axes when the button was clicked.
    int learning_ = -1;
    std::array<float, kRawAxes> learnBase_{};
};

} // namespace pom2

#endif // POM2_JOYSTICK_PANEL_IMGUI_H
