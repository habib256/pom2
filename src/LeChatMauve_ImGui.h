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

// LeChatMauve_ImGui — the RGB card's live panel: variant, mode latch and its
// two lines, what the card is decoding right now, and on the Eve the sixteen
// switches with CPREG. Pure view: reads a Snapshot taken under the lock,
// returns the requests the frame made (DevicePanelCoordinator applies them).

#ifndef POM2_LE_CHAT_MAUVE_IMGUI_H
#define POM2_LE_CHAT_MAUVE_IMGUI_H

#include "LeChatMauveCard.h"

namespace pom2 {

class LeChatMauve_ImGui
{
public:
    struct Snapshot {
        bool                        plugged     = false;
        int                         slot        = 7;
        LeChatMauveCard::Variant    variant     = LeChatMauveCard::Variant::Feline;
        LeChatMauveCard::RenderMode mode        = LeChatMauveCard::RenderMode::COL140;
        LeChatMauveCard::DhgrMode   dhgrMode    = LeChatMauveCard::DhgrMode::COL140;
        uint8_t                     fifoBits    = 0b11;
        bool                        eightyCol   = false;   // current data line
        bool                        an3High     = false;   // current clock line
        bool                        invertBit7  = false;   // Dragon Wars compat
        uint8_t                     eveSwitches = 0;       // bit i = EveSwitch i
        uint8_t                     cpreg       = 0;
        LeChatMauveCard::RvbGraphState rvb;                // RVB Graph registers
        bool                        auxShadowText = false; // Memory hook armed?
        bool                        auxShadowHgr  = false;
    };

    struct FrameResult {
        bool                        requestOverride   = false;
        LeChatMauveCard::RenderMode overrideTo        = LeChatMauveCard::RenderMode::COL140;
        bool                        requestReset      = false;
        bool                        requestInvertBit7 = false;
        bool                        invertBit7To      = false;
        bool                        requestVariant    = false;
        LeChatMauveCard::Variant    variantTo         = LeChatMauveCard::Variant::Feline;
        // Poke one Eve switch as a `STA $C0Bx` would (Eve only).
        bool                        requestEveSwitch  = false;
        LeChatMauveCard::EveSwitch  eveSwitch         = LeChatMauveCard::ENHRCPREG;
        bool                        eveSwitchTo       = false;
    };

    FrameResult render(const char*     title,
                       bool&           open,
                       const Snapshot& snap);
};

} // namespace pom2

#endif // POM2_LE_CHAT_MAUVE_IMGUI_H
