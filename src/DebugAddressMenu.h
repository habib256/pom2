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

// DebugAddressMenu — the menu items one ADDRESS offers the debugger: break
// when it is read or written, watch it, show it in the listing.
//
// Two panels offer them, the Debugger's listing (on an instruction's operand)
// and the Memory viewer (on a byte). The viewer draws under the machine's
// state lock, and arming a breakpoint takes that non-recursive lock: so the
// menu only REPORTS what was picked, and the caller applies it once the lock
// is gone (`Debugger_ImGui::applyAddressAction`).

#ifndef POM2_DEBUG_ADDRESS_MENU_H
#define POM2_DEBUG_ADDRESS_MENU_H

#include "imgui.h"

#include <cstdint>
#include <cstdio>

namespace pom2 {

enum class DebugAddressAction : uint8_t {
    None, BreakAtPc, BreakOnRead, BreakOnWrite, BreakOnAccess, Watch, WatchWord, ShowInListing,
};

/// Call inside an open popup. Returns the item picked this frame.
inline DebugAddressAction debugAddressMenuItems(uint16_t addr)
{
    DebugAddressAction picked = DebugAddressAction::None;
    char item[64];
    const auto offer = [&](const char* format, DebugAddressAction action) {
        std::snprintf(item, sizeof item, format, addr);
        if (ImGui::MenuItem(item)) picked = action;
    };
    // The three kinds first, in AppleWin's names — French Touch's request:
    // "right-click an address and choose the breakpoint: BP, BPMR, BPMW".
    offer("Break at $%04X  (BP)",                 DebugAddressAction::BreakAtPc);
    offer("Break when $%04X is read  (BPMR)",     DebugAddressAction::BreakOnRead);
    offer("Break when $%04X is written  (BPMW)",  DebugAddressAction::BreakOnWrite);
    offer("Break when $%04X is read or written",  DebugAddressAction::BreakOnAccess);
    ImGui::Separator();
    offer("Watch $%04X",                          DebugAddressAction::Watch);
    offer("Watch $%04X as a 16-bit word",         DebugAddressAction::WatchWord);
    offer("Show $%04X in the Debugger's listing", DebugAddressAction::ShowInListing);
    return picked;
}

} // namespace pom2

#endif // POM2_DEBUG_ADDRESS_MENU_H
