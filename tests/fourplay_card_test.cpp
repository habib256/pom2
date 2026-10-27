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

// FourPlayCard — the port of MAME's `4play.cpp`.
//
// The card is four bytes, so the test is about the two things that are easy
// to get wrong in four bytes: the exact bit assignment (MAME
// `4play.cpp:41-48`, everything active HIGH *except* bit 5, which is the one
// active-LOW line and therefore reads back set), and what an offset outside
// 0-3 answers ($FF, `4play.cpp:136`). Then the same through a real slot bus,
// because a card that is right on its own and wrong on the bus is still
// wrong.

#include "FourPlayCard.h"
#include "Memory.h"
#include "SlotBus.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>

using pom2::FourPlayCard;

void testBitLayout()
{
    FourPlayCard::Player p;
    // An untouched stick is not zero: bit 5 is active-LOW and undriven.
    assert(FourPlayCard::pack(p) == 0x20);
    assert(FourPlayCard::kIdle == 0x20);

    p = {}; p.up      = true; assert(FourPlayCard::pack(p) == 0x21);
    p = {}; p.down    = true; assert(FourPlayCard::pack(p) == 0x22);
    p = {}; p.left    = true; assert(FourPlayCard::pack(p) == 0x24);
    p = {}; p.right   = true; assert(FourPlayCard::pack(p) == 0x28);
    p = {}; p.button3 = true; assert(FourPlayCard::pack(p) == 0x30);
    p = {}; p.button2 = true; assert(FourPlayCard::pack(p) == 0x60);
    p = {}; p.button1 = true; assert(FourPlayCard::pack(p) == 0xA0);

    // Everything at once, which is also the only value that proves no two
    // controls share a bit.
    p = { true, true, true, true, true, true, true };
    assert(FourPlayCard::pack(p) == 0xFF);
    std::printf("  ok: bit layout matches MAME 4play.cpp:41-48\n");
}

void testFourIndependentPlayers()
{
    FourPlayCard card(4);
    for (int i = 0; i < FourPlayCard::kPlayers; ++i)
        assert(card.deviceSelectRead(static_cast<uint8_t>(i)) == FourPlayCard::kIdle);

    FourPlayCard::Player p2; p2.left = true; p2.button1 = true;
    card.setPlayer(1, p2);
    assert(card.deviceSelectRead(0) == 0x20);   // untouched
    assert(card.deviceSelectRead(1) == 0xA4);   // LEFT | BUTTON1 | unused
    assert(card.deviceSelectRead(2) == 0x20);
    assert(card.deviceSelectRead(3) == 0x20);

    FourPlayCard::Player p4; p4.down = true;
    card.setPlayer(3, p4);
    assert(card.deviceSelectRead(3) == 0x22);
    assert(card.deviceSelectRead(1) == 0xA4 && "player 2 must not have moved");

    // Out-of-range players are ignored rather than smearing into a neighbour.
    card.setPlayer(-1, p4);
    card.setPlayer(4, p4);
    card.setPlayer(99, p4);
    assert(card.deviceSelectRead(0) == 0x20);
    assert(card.deviceSelectRead(3) == 0x22);

    card.releaseAll();
    for (int i = 0; i < FourPlayCard::kPlayers; ++i)
        assert(card.deviceSelectRead(static_cast<uint8_t>(i)) == FourPlayCard::kIdle);
    std::printf("  ok: four independent players, and releaseAll clears them\n");
}

void testUnmappedOffsets()
{
    FourPlayCard card(4);
    for (int off = 4; off < 16; ++off)
        assert(card.deviceSelectRead(static_cast<uint8_t>(off)) == 0xFF);
    std::printf("  ok: offsets 4-15 read $FF (MAME 4play.cpp:136)\n");
}

void testThroughTheSlotBus()
{
    // $C0nX for slot 4 is $C0C0-$C0CF. Read through Memory, which is the
    // only path a guest has.
    Memory mem;
    auto card = std::make_unique<FourPlayCard>(4);
    auto* live = card.get();
    mem.slotBus().plug(4, std::move(card));

    for (int i = 0; i < 4; ++i)
        assert(mem.memRead(static_cast<uint16_t>(0xC0C0 + i)) == FourPlayCard::kIdle);

    FourPlayCard::Player p; p.right = true; p.button2 = true;
    live->setPlayer(2, p);
    assert(mem.memRead(0xC0C2) == 0x68);     // RIGHT | BUTTON2 | unused
    assert(mem.memRead(0xC0C0) == 0x20);
    // $C0n4-$C0nF are undriven: the floating bus (AppleWin FourPlay.cpp, by
    // the card's designer), not MAME's $FF.
    assert(mem.memRead(0xC0CF) == mem.peekFloatingBus());

    // A reset leaves the sticks alone: they are host input, not emulated
    // state, so nothing about the machine restarting moves somebody's thumb.
    mem.slotBus().reset();
    assert(mem.memRead(0xC0C2) == 0x68);
    std::printf("  ok: reads through Memory at $C0nX, and reset leaves it alone\n");
}

int main()
{
    testBitLayout();
    testFourIndependentPlayers();
    testUnmappedOffsets();
    testThroughTheSlotBus();
    std::printf("OK fourplay_card\n");
    return 0;
}
