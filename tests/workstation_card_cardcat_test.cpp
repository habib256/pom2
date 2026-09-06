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

// The Workstation Card, identified by REAL GUEST SOFTWARE.
//
// Every other check on this card is POM2 asking POM2. This one boots Henry
// Lowe's **CardCat** off `disks_3.5/CardCat 1.94.po` on an enhanced //e and
// reads the answer off the text screen: CardCat walks $C100-$C7FF looking at
// each slot's firmware signature bytes and names the card it finds. If the
// $Cn00 window served the wrong page — or served ROM where the card serves
// RAM — CardCat would say "Unknown" or "No Firmware Card Detected".
//
// The negative control is the point: the same boot with the slot empty must
// NOT print the card's name, so the string is coming from what POM2 puts on
// the bus and not from CardCat's own text.
//
// Gated on the //e ROM, the card ROM and the disk; SKIPs cleanly without any
// of them. `disks_3.5` is a repo-only directory (packaging/bundle.manifest
// denies it), so this test never runs from a package.

#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "WorkstationCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

namespace {

// Text page row bases, in the Woz interleave.
const uint16_t kRow[24] = {
    0x400, 0x480, 0x500, 0x580, 0x600, 0x680, 0x700, 0x780,
    0x428, 0x4A8, 0x528, 0x5A8, 0x628, 0x6A8, 0x728, 0x7A8,
    0x450, 0x4D0, 0x550, 0x5D0, 0x650, 0x6D0, 0x750, 0x7D0,
};

bool exists(const char* p) { std::ifstream f(p, std::ios::binary); return f.good(); }

/// //e alternate character set: $00-$1F inverse caps, $20-$3F inverse
/// special, $40-$5F MouseText, $60-$7F inverse lower, $80-$9F NORMAL CAPS,
/// $A0-$FF normal. Getting this wrong is what makes a screen dump look
/// empty exactly where the capitals are.
char screenChar(uint8_t b)
{
    uint8_t a;
    if      (b < 0x20) a = static_cast<uint8_t>(b | 0x40);
    else if (b < 0x40) a = b;
    else if (b < 0x60) a = static_cast<uint8_t>(b | 0x40);
    else if (b < 0x80) a = b;
    else if (b < 0xA0) a = static_cast<uint8_t>((b & 0x1F) | 0x40);
    else               a = static_cast<uint8_t>(b & 0x7F);
    return (a >= 0x20 && a < 0x7F) ? static_cast<char>(a) : ' ';
}

/// Boot CardCat with the card plugged (or not) and return the text screen.
std::string bootAndReadScreen(const char* cardRom, bool plugCard, bool dump)
{
    Memory mem;
    M6502  cpu(&mem);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom("roms/apple2e.rom", true)) return {};

    if (plugCard) {
        auto ws = std::make_unique<pom2::WorkstationCard>(4);
        if (!ws->loadRom(cardRom)) return {};
        mem.slotBus().plug(4, std::move(ws));
    }

    auto sp = std::make_unique<pom2::SmartPortCard>(5);
    sp->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    std::string err;
    if (!sp->mountBay(0, "disks_3.5/CardCat 1.94.po", err)) return {};
    mem.slotBus().plug(5, std::move(sp));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    cpu.setProgramCounter(0xC500);      // boot slot 5, as bootFromSlot does

    for (long i = 0; i < 40'000'000L; ++i) cpu.step();

    std::string screen;
    for (int r = 0; r < 24; ++r) {
        std::string line;
        for (int c = 0; c < 40; ++c)
            line += screenChar(mem.peekMainRam(static_cast<uint16_t>(kRow[r] + c)));
        if (dump) std::printf("    |%s|\n", line.c_str());
        screen += line;
        screen += '\n';
    }
    return screen;
}

} // namespace

int main()
{
    const char* cardRom = exists("roms/341-0358-A.bin") ? "roms/341-0358-A.bin"
                        : exists("roms/341-0358-a.bin") ? "roms/341-0358-a.bin"
                                                        : nullptr;
    if (!cardRom || !exists("roms/apple2e.rom") ||
        !exists("disks_3.5/CardCat 1.94.po")) {
        std::printf("workstation_card_cardcat: SKIP "
                    "(needs roms/apple2e.rom, roms/341-0358-A.bin and "
                    "\"disks_3.5/CardCat 1.94.po\")\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    const std::string withCard = bootAndReadScreen(cardRom, true, /*dump=*/true);
    assert(!withCard.empty() && "CardCat did not boot");

    // CardCat got far enough to draw its slot table at all.
    assert(withCard.find("Card Cat") != std::string::npos &&
           "CardCat's banner is missing — it did not finish booting");
    assert(withCard.find("Slot") != std::string::npos);

    // And it named the card.
    assert(withCard.find("Workstation Card") != std::string::npos &&
           "CardCat did not recognise the Workstation Card in slot 4");
    std::printf("  ok: CardCat identifies slot 4 as the Workstation Card\n");

    // Negative control: with the slot empty the name must be absent, so the
    // string above came off POM2's bus and not out of CardCat's text.
    const std::string without = bootAndReadScreen(cardRom, false, /*dump=*/false);
    assert(!without.empty());
    assert(without.find("Card Cat") != std::string::npos);
    assert(without.find("Workstation Card") == std::string::npos &&
           "the name appears with the slot empty — the test proves nothing");
    std::printf("  ok: with slot 4 empty, CardCat does not name it\n");

    std::printf("OK workstation_card_cardcat\n");
    return 0;
}
