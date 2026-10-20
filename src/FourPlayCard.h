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

// FourPlayCard — the **4play** joystick card, a port of MAME's
// `src/devices/bus/a2bus/4play.cpp`.
//
// Four DIGITAL joysticks on an Apple II. That is the whole point: the
// machine's own game port is *analogue* and carries two paddles, so two
// players with sticks is the ceiling and the software has to time an RC
// discharge to read them. This card is four bytes at `$C0nX` — read one,
// get one player's directions and buttons, no timing, no calibration.
//
// It is **modern homebrew**, not period hardware: Lukazi designed it in
// 2016 (MAME's own header links the blog). So the software that uses it is
// the current Apple II homebrew scene rather than a 1980s catalogue — which
// is the honest reason to want it here, and the reason it is a small card
// rather than a big one.
//
// THE WHOLE DEVICE, from MAME `4play.cpp:95-140`: `read_c0nx` returns
// player 1-4 for offsets 0-3 and `0xFF` for everything else. There is no
// write side, no ROM, no state — MAME's `device_start()` is empty. What
// POM2 adds is the only thing it must: a place for the host's gamepads to
// deposit their state, which is why the four bytes are atomics.
//
// Threading follows `PaddleInputs`: the UI thread writes, the CPU worker
// reads, and neither takes `stateMutex` for it. Joystick position is host
// input, not emulated state — so it is deliberately absent from the
// snapshot, exactly as the game-port paddles are. A rewind must not put
// somebody's thumb back where it was.

#ifndef POM2_FOUR_PLAY_CARD_H
#define POM2_FOUR_PLAY_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace pom2 {

class FourPlayCard : public SlotPeripheral
{
public:
    static constexpr int kPlayers = 4;

    /// One player's byte. MAME `4play.cpp:41-48`: everything is active
    /// HIGH except bit 5, which is `IP_ACTIVE_LOW IPT_UNKNOWN` — nothing
    /// drives it, so it reads back as 1.
    enum Bit : uint8_t {
        kUp      = 0x01,
        kDown    = 0x02,
        kLeft    = 0x04,
        kRight   = 0x08,
        kButton3 = 0x10,
        kUnused  = 0x20,
        kButton2 = 0x40,
        kButton1 = 0x80,
    };

    /// What an untouched player reads as: bit 5 alone.
    static constexpr uint8_t kIdle = kUnused;
    /// What an offset outside 0-3 reads as (MAME `4play.cpp:136`).
    static constexpr uint8_t kUnmapped = 0xFF;

    struct Player {
        bool up = false, down = false, left = false, right = false;
        bool button1 = false, button2 = false, button3 = false;
    };

    explicit FourPlayCard(int slot);

    std::string_view name() const override { return "4play Joystick Card (rev. B)"; }
    int getSlot() const { return slot_; }

    uint8_t deviceSelectRead(uint8_t low4) override;
    void    onReset() override;

    /// Publish one player's stick, from the UI thread. `player` is 0-3.
    void setPlayer(int player, const Player& p);
    /// Release every player — used when input focus leaves the machine, so
    /// a held direction does not stick while the user is in a menu.
    void releaseAll();

    /// The bit packing, exposed so a test can check it without a card and
    /// so the layout lives in exactly one place.
    static uint8_t pack(const Player& p);

private:
    int slot_;
    // Written by the UI thread, read by the CPU worker — the same
    // arrangement `PaddleInputs` uses, and for the same reason.
    std::array<std::atomic<uint8_t>, kPlayers> state_;
};

} // namespace pom2

#endif // POM2_FOUR_PLAY_CARD_H
