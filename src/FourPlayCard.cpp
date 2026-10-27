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

// FourPlayCard — see the header. Port of MAME `4play.cpp`.

#include "FourPlayCard.h"

namespace pom2 {

FourPlayCard::FourPlayCard(int slot)
    : slot_(slot)
{
    releaseAll();
}

uint8_t FourPlayCard::pack(const Player& p)
{
    // MAME `4play.cpp:41-48`. Bit 5 is always set: it is the one
    // `IP_ACTIVE_LOW` line and nothing on the card drives it.
    uint8_t v = kUnused;
    if (p.up)      v |= kUp;
    if (p.down)    v |= kDown;
    if (p.left)    v |= kLeft;
    if (p.right)   v |= kRight;
    if (p.button1) v |= kButton1;
    if (p.button2) v |= kButton2;
    if (p.button3) v |= kButton3;
    return v;
}

void FourPlayCard::setPlayer(int player, const Player& p)
{
    if (player < 0 || player >= kPlayers) return;
    state_[static_cast<std::size_t>(player)].store(pack(p), std::memory_order_relaxed);
}

void FourPlayCard::releaseAll()
{
    for (auto& s : state_) s.store(kIdle, std::memory_order_relaxed);
}

void FourPlayCard::onReset()
{
    // A reset does not unplug anybody's joystick — the card has no state of
    // its own to clear (MAME's `device_start` is empty and there is no
    // `device_reset`). Left explicit so the absence reads as a decision.
}

// Offsets 0-3 are the four players (MAME `4play.cpp:123-140`).
uint8_t FourPlayCard::deviceSelectRead(uint8_t low4)
{
    if (low4 < kPlayers)
        return state_[low4].load(std::memory_order_relaxed);
    // $C0n4-$C0nF are not driven: the floating bus (AppleWin FourPlay.cpp,
    // written by the card's designer; the RevB documentation lists only the
    // four joystick bytes). MAME returns $FF.
    return openBus();
}

} // namespace pom2
