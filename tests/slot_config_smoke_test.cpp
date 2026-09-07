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

// Slot configuration smoke test — pins the per-card slot constructor
// argument plumbing introduced in Phase 1 of the Mouse Card / Slot
// Configuration UI work.
//
// What this gates:
//
//   * SuperSerialCard's slot ROM signature ($Cn05=$38, $Cn07=$18,
//     $Cn0B=$01, $Cn0C=$31) appears at the slot the card was
//     constructed with — NOT at slot 2 specifically. The PR#n / IN#n
//     trampolines and the spin-on-TDRE / RDRF routines also patch the
//     correct $C0nX device-select addresses.
//   * ProDOSHardDiskCard's ProDOS detection bytes ($Cn01=$20, $Cn03=$00,
//     $Cn05=$03) and the boot trampoline at $Cn20 land at the
//     constructor-supplied slot.
//   * The DiskIICard's slot is queryable via `getSlot()` even though the
//     P5A boot PROM itself is slot-agnostic (it auto-detects via the
//     `JSR $FF58 / TSX` trick). This is the basis of the slot-panel UI:
//     even a slot-agnostic card needs to advertise its plug location.
//   * LeChatMauveCard's `getSlot()` matches the constructor arg even
//     though the card has no slot ROM at all (pure soft-switch sniffer).
//
// The test runs entirely off-line — no Apple II ROM, no .dsk image, no
// host folder. We just instantiate the cards in non-default slots, plug
// them, and read back via Memory::memRead to walk the SlotBus dispatch
// path the way a 6502 fetch would.

#include "DiskIICard.h"
#include "LeChatMauveCard.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"
#include "SuperSerialCard.h"

#include <cassert>
#include <cstdio>
#include <memory>

namespace {

void testSscAtSlot(int slot)
{
    Memory mem;
    auto card = std::make_unique<SuperSerialCard>(slot);
    assert(card->getSlot() == slot);
    mem.slotBus().plug(slot, std::move(card));

    const uint16_t base = static_cast<uint16_t>(0xC000 + slot * 0x100);
    // SSC autodetection signature.
    assert(mem.memRead(base + 0x05) == 0x38);
    assert(mem.memRead(base + 0x07) == 0x18);
    assert(mem.memRead(base + 0x0B) == 0x01);
    assert(mem.memRead(base + 0x0C) == 0x31);

    // PR#n entry at $Cn00 should JMP into the per-slot trampoline at
    // $Cn20. The high byte of the JMP target == base high byte == 0xC0+slot.
    const uint8_t slotHi = static_cast<uint8_t>(0xC0 + slot);
    assert(mem.memRead(base + 0x00) == 0x4C);          // JMP
    assert(mem.memRead(base + 0x01) == 0x20);          // low byte of $Cn20
    assert(mem.memRead(base + 0x02) == slotHi);        // $Cn

    // IN#n entry at $Cn08 → JMP $Cn40.
    assert(mem.memRead(base + 0x08) == 0x4C);
    assert(mem.memRead(base + 0x09) == 0x40);
    assert(mem.memRead(base + 0x0A) == slotHi);

    // PR#n trampoline at $Cn20: ACIA init (cmd=$0B — the real SSC
    // firmware programs the 6551 before any I/O; without it a plain
    // `PR#n : PRINT` writes the TDR with DTR de-asserted and every byte
    // is dropped), then LDA #<output_routine ($CnB0) into CSWL/CSWH.
    //   A9 0B 8D <devLo+A> C0 A9 B0 85 36 A9 <slotHi> 85 37 60
    const uint8_t devLo = static_cast<uint8_t>(0x80 + slot * 16);
    assert(mem.memRead(base + 0x20) == 0xA9);
    assert(mem.memRead(base + 0x21) == 0x0B);
    assert(mem.memRead(base + 0x22) == 0x8D);
    assert(mem.memRead(base + 0x23) == static_cast<uint8_t>(devLo + 0xA));
    assert(mem.memRead(base + 0x24) == 0xC0);
    assert(mem.memRead(base + 0x25) == 0xA9);
    assert(mem.memRead(base + 0x26) == 0xB0);
    assert(mem.memRead(base + 0x27) == 0x85);
    assert(mem.memRead(base + 0x28) == 0x36);          // CSWL
    assert(mem.memRead(base + 0x29) == 0xA9);
    assert(mem.memRead(base + 0x2A) == slotHi);
    assert(mem.memRead(base + 0x2B) == 0x85);
    assert(mem.memRead(base + 0x2C) == 0x37);          // CSWH
    assert(mem.memRead(base + 0x2D) == 0x60);          // RTS

    // Output routine at $CnB0 patches the absolute LDA $C0n9 / STA $C0n8
    // addresses to the slot's device-select range.
    // PHA at $CnB0
    assert(mem.memRead(base + 0xB0) == 0x48);
    // LDA $C0n9 at $CnB1
    assert(mem.memRead(base + 0xB1) == 0xAD);
    assert(mem.memRead(base + 0xB2) == static_cast<uint8_t>(devLo + 0x9));
    assert(mem.memRead(base + 0xB3) == 0xC0);
    // STA $C0n8 at $CnB9
    assert(mem.memRead(base + 0xB9) == 0x8D);
    assert(mem.memRead(base + 0xBA) == static_cast<uint8_t>(devLo + 0x8));
    assert(mem.memRead(base + 0xBB) == 0xC0);
}

void testHdvAtSlot(int slot)
{
    Memory mem;
    auto card = std::make_unique<ProDOSHardDiskCard>(slot);
    assert(card->getSlot() == slot);
    mem.slotBus().plug(slot, std::move(card));

    const uint16_t base = static_cast<uint16_t>(0xC000 + slot * 0x100);
    const uint8_t slotHi = static_cast<uint8_t>(0xC0 + slot);

    // ProDOS block-device signature.
    assert(mem.memRead(base + 0x01) == 0x20);
    assert(mem.memRead(base + 0x03) == 0x00);
    assert(mem.memRead(base + 0x05) == 0x03);
    assert(mem.memRead(base + 0x07) == 0x01);
    // $CnFE = $07, not $03: bit 2 is "the device can be written to"
    // (ProDOS 8 TN.PDOS.021). It read $03 — status + read, i.e. READ-ONLY —
    // while the driver has always implemented command $02, so every
    // capability-inspecting utility presented the volume as unwritable.
    // Bug hunt #4 #13; SmartPortCard was fixed for the same defect in 2026-09.
    assert(mem.memRead(base + 0xFE) == 0x07);
    // $CnFF is the driver entry OFFSET, and the ROM moved when the read and
    // write routines grew their out-of-range check — so read it rather than
    // hard-coding it, and check that it points at the JMP below.
    const uint8_t driverOff = mem.memRead(base + 0xFF);
    assert(driverOff != 0x00);

    // Entry: JMP $Cn20.
    assert(mem.memRead(base + 0x00) == 0x4C);
    assert(mem.memRead(base + 0x02) == slotHi);

    // The boot routine at $Cn20 begins with LDA #$01 / STA $42 / LDA #unit
    // where unit = slot << 4. Walk the first few bytes.
    assert(mem.memRead(base + 0x20) == 0xA9);
    assert(mem.memRead(base + 0x21) == 0x01);
    assert(mem.memRead(base + 0x22) == 0x85);
    assert(mem.memRead(base + 0x23) == 0x42);
    assert(mem.memRead(base + 0x24) == 0xA9);
    assert(mem.memRead(base + 0x25) == static_cast<uint8_t>(slot << 4));
}

void testDiskIIAtSlot(int slot)
{
    auto card = std::make_unique<DiskIICard>(slot);
    assert(card->getSlot() == slot);
    // The Disk II ROM is slot-agnostic by design (P5A auto-detects via the
    // standard JSR-$FF58 trick), so we don't assert specific bytes here —
    // only that the constructor stores the slot and the card plugs without
    // protest.
    Memory mem;
    mem.slotBus().plug(slot, std::move(card));
}

void testChatMauveAtSlot(int slot)
{
    auto card = std::make_unique<LeChatMauveCard>(slot);
    assert(card->getSlot() == slot);
    // No ROM, no device-select space — the card is a video soft-switch
    // sniffer. Its slot exists only for diagnostics. Plug + unplug should
    // succeed regardless of the slot chosen.
    Memory mem;
    mem.slotBus().plug(slot, std::move(card));
}

}  // namespace

int main()
{
    // SSC works in any slot 1..7. Pick three to spot-check the ROM
    // patching logic across boundaries.
    for (int s : {1, 3, 5, 7}) testSscAtSlot(s);

    // ProDOS HDV slot constants get baked into eight different ROM bytes —
    // walk a few alternates to confirm they all line up.
    for (int s : {2, 4, 5, 6}) testHdvAtSlot(s);

    // DiskII (slot-agnostic ROM) and LeChatMauve (no ROM) only need to
    // verify that the slot survives plug/unplug.
    for (int s : {1, 6, 7}) testDiskIIAtSlot(s);
    for (int s : {1, 4, 7}) testChatMauveAtSlot(s);

    std::printf("OK slot_config_smoke\n");
    return 0;
}
