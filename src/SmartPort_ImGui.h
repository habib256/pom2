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

// SmartPort_ImGui — configuration panel for the slot-plugged SmartPortCard
// (Liron-class). One row per unit (kMaxUnits=2): type selector
// ([empty] / 3.5" 800K / ProDOS HDV), mount + eject buttons, write-back
// toggle, and an inline path field. The panel is pure data-in / actions-
// out — MainWindow owns the SmartPortCard pointer and applies the
// requested changes under the state mutex.
//
// Why a separate panel from `Disk35Controller_ImGui`: the existing
// 3.5" panel drives the //c+ on-board pair of `Disk35Image` instances
// owned by `EmulationController`. Slot-plugged Liron-class cards now
// own their units independently (different concerns: on-board hub vs
// slot-card chain), so they need their own UI surface.

#ifndef POM2_SMARTPORT_IMGUI_H
#define POM2_SMARTPORT_IMGUI_H

#include <array>
#include <cstdint>
#include <string>

namespace pom2 {

class SmartPort_ImGui
{
public:
    // ── Input snapshot the host (MainWindow) populates each frame ─────
    struct UnitSnapshot {
        // "" = empty slot. Non-empty = SmartPortUnit::kindKey of the
        // currently-installed unit ("35" / "hdv" / …).
        std::string kind;
        std::string kindLabel;
        std::string path;
        std::string lastError;
        uint32_t    blockCount       = 0;
        bool        loaded           = false;
        bool        writeProtected   = false;
        bool        writeBackEnabled = true;
    };

    struct CardSnapshot {
        int  slot     = 0;
        bool plugged  = false;
        std::array<UnitSnapshot, 2> units{};
    };

    // ── Output: actions the user requested this frame ──────────────────
    struct UnitAction {
        // Replace the unit's type. Non-empty string = kindKey of the
        // new unit (creates it via makeSmartPortUnit). Empty string +
        // `clearType=true` = remove the unit (set to empty slot).
        // `clearType=false` AND empty `setType` = no type change.
        std::string setType;
        bool        clearType        = false;
        std::string mountPath;       // non-empty = mount this image
        bool        eject            = false;
        bool        writeBackChanged = false;
        bool        writeBackOn      = false;
    };

    struct Result {
        std::array<UnitAction, 2> units{};
    };

    /// Render the panel. `open` is the host's bool toggle.
    /// Returns the actions the user requested this frame.
    Result render(const char* title, bool& open, const CardSnapshot& snap);

private:
    // Per-unit mount-dialog state. ImGui input box buffers can't live
    // on the stack across frames, so we keep them here.
    std::array<char, 256> pathBufs_[2] = {};
};

} // namespace pom2

#endif // POM2_SMARTPORT_IMGUI_H
