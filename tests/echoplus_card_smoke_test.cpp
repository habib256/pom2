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

// EchoPlusCard smoke test — verifies the SlotPeripheral surface that
// glues the SSI263 chip model to the slot bus:
//   (1) slotRomRead/Write at $Cs00..$Cs04 reach the 5 SSI263 registers;
//   (2) $Cs05..$CsFF read as open bus ($FF);
//   (3) advanceCycles ticks the chip and asserts the slot IRQ when
//       A/!R goes high;
//   (4) ack via a write to $00..$02 releases the slot IRQ.
//
// Chip-level state-machine semantics are pinned by `ssi263_smoke` —
// this test focuses on the bus glue.

#include "EchoPlusCard.h"
#include "Ssi263.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void testRegisterDecode()
{
    EchoPlusCard card(4);

    // Write all 5 registers via slotRomWrite.
    card.slotRomWrite(0x0, 0x55);
    card.slotRomWrite(0x1, 0xAA);
    card.slotRomWrite(0x2, 0xF0);   // rate=15
    card.slotRomWrite(0x3, 0x0F);   // CTL=0, amp=15 (exit power-down)
    card.slotRomWrite(0x4, 0x80);

    auto s = card.snapshotChip();
    // DURPHON write triggers a phoneme + bumps the counter (since
    // CTL=0 was set in the same sequence by $03 — order matters here
    // for the v1 chip-write semantics, but writing $00 once with CTL
    // already cleared bumps the count).
    assert(s.regs[0] == 0x55);
    assert(s.regs[1] == 0xAA);
    assert(s.regs[2] == 0xF0);
    assert(s.regs[3] == 0x0F);
    assert(s.regs[4] == 0x80);
    assert(!s.powerDown);
    assert(s.irqEnabled);

    // Out-of-range reads = open bus.
    assert(card.slotRomRead(0x05) == 0xFF);
    assert(card.slotRomRead(0x80) == 0xFF);
    assert(card.slotRomRead(0xFF) == 0xFF);

    // Out-of-range writes are no-ops (no chip state change).
    const auto regsBefore = s.regs;
    card.slotRomWrite(0x05, 0x42);
    card.slotRomWrite(0xC0, 0x42);
    auto s2 = card.snapshotChip();
    for (int r = 0; r < 5; ++r) assert(s2.regs[r] == regsBefore[r]);

    std::printf("  ok: $Cs00-$Cs04 routes to SSI263; rest = open bus\n");
}

void testIrqWiring()
{
    EchoPlusCard card(4);
    // Configure a fast phoneme with IRQ enabled (mode 11).
    card.slotRomWrite(0x3, 0x0F);     // exit power-down
    card.slotRomWrite(0x2, 0xF0);     // rate=15 (fast)
    card.slotRomWrite(0x0, 0xC1);     // mode=11, phoneme=1

    // Slot IRQ is low until the phoneme finishes.
    assert(!card.isIrqAsserted());
    auto s = card.snapshotChip();
    assert(s.phonemeRemainingCycles > 0);

    // Tick past the duration → A/!R should be set + slot IRQ asserted.
    card.advanceCycles(s.phonemeRemainingCycles + 100);
    assert(card.isIrqAsserted());
    s = card.snapshotChip();
    assert(s.aRequest);

    // Slot IRQ stays high until the host CPU acks (writes $00-$02).
    card.advanceCycles(10000);
    assert(card.isIrqAsserted());

    // Ack via $00 (DURPHON) write: A/!R clears, slot IRQ releases,
    // new phoneme starts.
    card.slotRomWrite(0x0, 0xC2);
    assert(!card.isIrqAsserted());
    s = card.snapshotChip();
    assert(!s.aRequest);
    assert(s.currentPhoneme == 2);

    std::printf("  ok: advanceCycles past phoneme → slot IRQ assert; "
                "$00 ack → release\n");
}

void testReadAcknowledgement()
{
    EchoPlusCard card(4);
    // Drive a phoneme to completion.
    card.slotRomWrite(0x3, 0x0F);
    card.slotRomWrite(0x2, 0xF0);
    card.slotRomWrite(0x0, 0xC5);
    auto s = card.snapshotChip();
    card.advanceCycles(s.phonemeRemainingCycles + 100);
    assert(card.isIrqAsserted());

    // Reads drive A/!R on bit 7 and leave bits 0-6 to the floating bus
    // (AppleWin SSI263::Read; PhasorCard merges the same way) — $FF here,
    // the open bus of a standalone card. They used to be the chip byte raw,
    // a constant $80. Reads do NOT clear A/!R — only writes to $00-$02 do.
    assert(card.slotRomRead(0x0) == 0xFF);
    assert(card.slotRomRead(0x3) == 0xFF);
    assert(card.isIrqAsserted());

    // Reset state: chip in power-down, IRQ released.
    card.onReset();
    assert(!card.isIrqAsserted());
    s = card.snapshotChip();
    assert(s.powerDown);
    assert(!s.aRequest);

    std::printf("  ok: reads see A/!R bit but don't ack; onReset clears\n");
}

void testSnapshotRoundTrip()
{
    // The SSI263 carries rewindable state; the card was missing from the
    // snapshot roster (Mockingboard SoundII captured its identical chip
    // all along). Pin: registers + mode survive an append/reset/load.
    EchoPlusCard card(4);
    card.slotRomWrite(0x03, 0x0F);   // CTTRAMP: CTL=0, amp=15
    card.slotRomWrite(0x02, 0xF0);   // RATEINF
    card.slotRomWrite(0x00, 0xC1);   // DURPHON: mode bits + phoneme 1
    const auto before = card.snapshotChip();
    std::vector<uint8_t> blob;
    card.appendSnapshotState(blob);

    EchoPlusCard fresh(4);
    fresh.loadSnapshotState(blob.data(), blob.size());
    const auto after = fresh.snapshotChip();
    for (int r = 0; r < 5; ++r) assert(after.regs[r] == before.regs[r]);
    assert(after.currentPhoneme == before.currentPhoneme);
    assert(after.mode           == before.mode);
    assert(after.powerDown      == before.powerDown);

    // Garbage / truncated blobs are rejected (state untouched).
    EchoPlusCard untouched(4);
    const auto base = untouched.snapshotChip();
    uint8_t junk[4] = { 'X', 'X', 9, 9 };
    untouched.loadSnapshotState(junk, sizeof(junk));
    const auto still = untouched.snapshotChip();
    assert(still.regs[0] == base.regs[0] && still.mode == base.mode);

    std::printf("  ok: SSI263 state snapshot round-trip\n");
}

void testLatchedIrqAndRepeat()
{
    EchoPlusCard card;
    card.slotRomWrite(0, 0x85);
    card.slotRomWrite(3, 0x0F);       // latch function 2, enable IRQ
    card.slotRomWrite(0, 0x05);      // mode does not change until CTL edge
    auto snap = card.snapshotChip();
    assert(snap.mode == 2 && snap.irqEnabled);
    card.advanceCycles(snap.phonemeRemainingCycles);
    assert(card.isIrqAsserted());
    card.slotRomWrite(1, 0x00);      // acknowledge through INFLECT
    assert(!card.isIrqAsserted());
    snap = card.snapshotChip();
    card.advanceCycles(snap.phonemeRemainingCycles - 1);
    assert(!card.isIrqAsserted());
    card.advanceCycles(1);
    assert(card.isIrqAsserted());

    card.slotRomWrite(3, 0x8F);      // power down drops slot IRQ
    assert(!card.isIrqAsserted());
    card.slotRomWrite(3, 0x0F);      // latch DUR=00: disable IRQ, retain function
    snap = card.snapshotChip();
    assert(snap.mode == 2 && !snap.irqEnabled);
    card.advanceCycles(snap.phonemeRemainingCycles);
    assert(card.slotRomRead(0) & 0x80);  // status remains pollable
    assert(!card.isIrqAsserted());
    std::puts("  ok: latched IRQ mode and repeated requests reach the slot line");
}

} // namespace

int main()
{
    std::printf("EchoPlusCard smoke test\n");
    testRegisterDecode();
    testIrqWiring();
    testReadAcknowledgement();
    testSnapshotRoundTrip();
    testLatchedIrqAndRepeat();
    std::printf("PASS\n");
    return 0;
}
