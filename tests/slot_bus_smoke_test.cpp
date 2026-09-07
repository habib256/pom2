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

// SlotBus dispatch smoke test — pins:
//   * device-select decode (slot N at $C080+N*16, low4 = addr & 0xF)
//   * slot-ROM decode (slot N at $CN00, low8 = addr & 0xFF)
//   * $C800 ownership: FIRST-one-wins, only a POPULATED slot claims, and
//     the claim is released only by $CFFF (MAME `apple2e.cpp:2970-2987`
//     `read_slot_rom`, `:2989-3025` `write_slot_rom`, `:3137-3155`
//     `c800_r`). POM2 used to latch last-one-wins, empty slots included.
//   * $CFFF deactivates the expansion ROM (read or write)
//   * an unclaimed read returns the FLOATING BUS, not a hard $FF — every
//     one of those upstream handlers, plus `c080_r` (`:2883-2918`), ends
//     in `return read_floatingbus();`. SlotBus takes the source from
//     `setFloatingBusSource`; with none installed it answers $FF.
//   * unplug() of the active slot clears the latch

#include "SlotBus.h"

#include <cassert>
#include <cstdio>
#include <memory>

namespace {

class FakeCard : public SlotPeripheral
{
public:
    explicit FakeCard(uint8_t signature) : sig(signature) {}
    std::string_view name() const override { return "FakeCard"; }

    uint8_t deviceSelectRead(uint8_t low4) override {
        ++deviceReads;
        return static_cast<uint8_t>(low4 | sig);
    }
    void deviceSelectWrite(uint8_t low4, uint8_t v) override {
        ++deviceWrites;
        lastWriteLow4 = low4;
        lastWriteValue = v;
    }
    uint8_t slotRomRead(uint8_t low8) override {
        ++slotRomReads;
        return static_cast<uint8_t>(low8 ^ sig);
    }
    uint8_t expansionRomRead(uint16_t offset) override {
        ++expansionReads;
        return static_cast<uint8_t>(offset & 0xFF);
    }
    void expansionRomWrite(uint16_t offset, uint8_t v) override {
        ++expansionWrites;
        lastExpOffset = offset;
        lastExpValue  = v;
    }
    void onPlug()   override { ++plugCount; }
    void onUnplug() override { ++unplugCount; }
    void onReset()  override { ++resetCount; }

    int deviceReads     = 0;
    int deviceWrites    = 0;
    int slotRomReads    = 0;
    int expansionReads  = 0;
    int expansionWrites = 0;
    int plugCount       = 0;
    int unplugCount     = 0;
    int resetCount      = 0;
    uint8_t  sig;
    uint8_t  lastWriteLow4   = 0;
    uint8_t  lastWriteValue  = 0;
    uint16_t lastExpOffset   = 0;
    uint8_t  lastExpValue    = 0;
};

} // namespace

int main()
{
    SlotBus bus;

    // Plug card in slot 6 (typical Disk II location).
    auto cardPtr = std::make_unique<FakeCard>(0xA0);
    FakeCard* card = cardPtr.get();
    bus.plug(6, std::move(cardPtr));
    assert(bus.isPlugged(6));
    assert(card->plugCount == 1);

    // Device select: slot 6 = $C0E0-$C0EF.
    assert(bus.deviceSelectRead(0xC0E0) == (0x00 | 0xA0));
    assert(bus.deviceSelectRead(0xC0EF) == (0x0F | 0xA0));
    bus.deviceSelectWrite(0xC0E5, 0x42);
    assert(card->lastWriteLow4 == 0x05);
    assert(card->lastWriteValue == 0x42);

    // Slot ROM read: slot 6 ROM at $C600-$C6FF.
    const uint8_t got = bus.slotRomRead(0xC600);
    assert(got == (0x00 ^ 0xA0));
    // A populated slot's $CnXX access claims the unclaimed $C800 window.
    assert(bus.getActiveExpansionSlot() == 6);

    // An EMPTY slot reads open bus and claims NOTHING — `read_slot_rom`
    // only reaches the claim inside `if (m_slotdevice[slotnum] != nullptr)`.
    assert(bus.slotRomRead(0xC400) == 0xFF);
    assert(bus.getActiveExpansionSlot() == 6);
    // ...and a SECOND populated slot cannot steal it either: upstream's
    // comment is literally "a bus fight here is resolved as
    // first-one-wins", gated on `m_cnxx_slot == CNXX_UNCLAIMED`.
    bus.plug(2, std::make_unique<FakeCard>(0x50));
    assert(bus.slotRomRead(0xC200) == (0x00 ^ 0x50));
    assert(bus.getActiveExpansionSlot() == 6);
    (void)bus.unplug(2);
    assert(bus.getActiveExpansionSlot() == 6);   // unplugging a NON-holder

    // Writes into $CnXX claim on the same rule (`write_slot_rom`).
    (void)bus.expansionRomRead(0xCFFF);          // release
    assert(bus.getActiveExpansionSlot() == -1);
    bus.slotRomWrite(0xC400, 0x11);              // empty slot: no claim
    assert(bus.getActiveExpansionSlot() == -1);
    bus.slotRomWrite(0xC600, 0x11);              // populated: claims
    assert(bus.getActiveExpansionSlot() == 6);

    const uint8_t expByte = bus.expansionRomRead(0xC842);
    assert(expByte == 0x42);  // (0xC842 - 0xC800) & 0xFF == 0x42
    assert(card->expansionReads == 1);

    // $CFFF disables expansion ROM.
    (void)bus.expansionRomRead(0xCFFF);
    assert(bus.getActiveExpansionSlot() == -1);
    // Read after disable should NOT route to the card.
    const int prevExpansionReads = card->expansionReads;
    assert(bus.expansionRomRead(0xC900) == 0xFF);
    assert(card->expansionReads == prevExpansionReads);

    // An unclaimed read takes the floating-bus source when one is
    // installed (Memory installs `Memory::floatingBus()`), and $FF only
    // as the no-source fallback.
    bus.setFloatingBusSource([]() -> uint8_t { return 0x3C; });
    assert(bus.getActiveExpansionSlot() == -1);
    assert(bus.slotRomRead(0xC400) == 0x3C);     // empty slot ROM
    assert(bus.deviceSelectRead(0xC0B0) == 0x3C);// empty device select
    assert(bus.expansionRomRead(0xC900) == 0x3C);// unclaimed $C800 window
    assert(bus.expansionRomRead(0xCFFF) == 0x3C);// the deselect read itself
    bus.setFloatingBusSource(nullptr);
    assert(bus.slotRomRead(0xC400) == 0xFF);

    // Re-arm and write to expansion ROM.
    (void)bus.slotRomRead(0xC600);
    bus.expansionRomWrite(0xC820, 0x99);
    assert(card->lastExpOffset == 0x0020);
    assert(card->lastExpValue  == 0x99);

    // Reset hits all plugged cards.
    bus.reset();
    assert(card->resetCount == 1);
    // Reset does NOT clear the expansion latch (intentional invariant).
    assert(bus.getActiveExpansionSlot() == 6);

    // Unplug clears latch when active slot is removed.
    auto removed = bus.unplug(6);
    assert(removed != nullptr);
    assert(card->unplugCount == 1);
    assert(!bus.isPlugged(6));
    assert(bus.getActiveExpansionSlot() == -1);

    std::printf("SlotBus smoke: OK\n");
    return 0;
}
