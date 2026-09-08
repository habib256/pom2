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

// Renderer-free host-mouse boundary. SlotBus owns the concrete cards; this
// coordinator resolves them only while holding the machine lock, publishes an
// immutable inspector snapshot and routes host input without retained aliases.

#ifndef POM2_MOUSE_COORDINATOR_H
#define POM2_MOUSE_COORDINATOR_H

#include <cstdint>

class EmulationController;

namespace pom2 {

class MouseCoordinator final
{
public:
    enum class Kind : std::uint8_t {
        None,
        Mame,
        AppleWin,
    };

    struct AppleWinSnapshot {
        int iX = 0, iY = 0;
        int nX = 0, nY = 0;
        int iMinX = 0, iMaxX = 0;
        int iMinY = 0, iMaxY = 0;
        bool bBtn0 = false, bBtn1 = false;
        bool bPrevBtn0 = false, bPrevBtn1 = false;
        std::uint8_t byMode = 0;
        std::uint8_t byState = 0;
        std::uint8_t by6821A = 0;
        std::uint8_t by6821B = 0;
        int buffPos = 0;
        int dataLen = 0;
        std::uint8_t lastCmd = 0;
        bool hostDrained = true;

        bool mouseOn() const noexcept { return (byMode & 0x01) != 0; }
    };

    struct ScreenHolesSnapshot {
        int xLo = 0, xHi = 0;
        int yLo = 0, yHi = 0;
        int mode = 0;
        int status = 0;

        int x() const noexcept { return (xHi << 8) | xLo; }
        int y() const noexcept { return (yHi << 8) | yLo; }
    };

    struct Snapshot {
        Kind kind = Kind::None;
        int slot = -1;
        bool mamePlugged = false;
        bool appleWinPlugged = false;
        AppleWinSnapshot appleWin;
        ScreenHolesSnapshot holes;

        bool plugged() const noexcept
        {
            return mamePlugged || appleWinPlugged;
        }
        bool appleWinActive() const noexcept { return kind == Kind::AppleWin; }
    };

    explicit MouseCoordinator(EmulationController& controller)
        : controller_(controller) {}

    /// AppleWin wins the primary inspector selection, matching the former UI
    /// alias precedence. Within a kind, the highest slot wins.
    Snapshot capture() const;

    /// Route to every live mouse card. Returns the number of cards updated.
    int routeHost(std::uint8_t rawX, std::uint8_t rawY, bool button);

private:
    EmulationController& controller_;
};

} // namespace pom2

#endif // POM2_MOUSE_COORDINATOR_H
