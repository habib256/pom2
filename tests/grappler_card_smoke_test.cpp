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

// GrapplerCard smoke test — pins:
//
//   1. Fallback stub ROM (no Grappler+ dump): exposes the synthetic PR#n
//      trampoline + Pascal autodetect bytes so `PR#1` still works without
//      the 4 KB Grappler dump.
//   2. MAME-faithful $C0nX decode (a2bus_grapplerplus, grappler.cpp):
//      data latch on !(offset & 3) ($0/$4/$8/$C → spool), A0 = high ROM
//      bank at $C800, A1/A2 = IRQ disable/enable; status = IRQ|BUSY|PE|
//      SELECT|ACK with the host printer on-line ($53 idle: the S1
//      printer-type DIP defaults to Apple Dot Matrix, bits 6-4 = 101).
//      (The previous decode spooled offset 1 — the real card's BANK
//      SELECT — so genuine firmware printed into the void and read $FF
//      status = busy + paper out.)
//   3. ROM gate — `isRomLoaded()` is false until `loadRom()` succeeds; a
//      wrong-size dump is rejected and the stub stays in place; $CnXX
//      reads AND writes reset the expansion bank (MAME read_cnxx
//      grappler.cpp:578-583 / write_cnxx :586-591) while bus reset does
//      NOT (reset_from_bus :536-539 leaves the U2D bank flip-flop alone).

#include "GrapplerCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "TestTempPath.h"

namespace {

void testStubRom()
{
    GrapplerCard card(1);
    assert(!card.isRomLoaded());

    // The stub is the only hand-assembled part of this card (a real roms/
    // dump is copied verbatim), so it is the only part that can overrun a
    // region — see SlotRomAsm.h.
    assert(!card.romLayoutError());

    // PR#n entry at $Cn00 — JMP $Cn20.
    assert(card.slotRomRead(0x00) == 0x4C);
    assert(card.slotRomRead(0x01) == 0x20);
    assert(card.slotRomRead(0x02) == 0xC1);     // slotHi for slot 1
    // Pascal 1.1 autodetect.
    assert(card.slotRomRead(0x05) == 0x38);
    assert(card.slotRomRead(0x07) == 0x18);
    assert(card.slotRomRead(0x0B) == 0x01);
    assert(card.slotRomRead(0x0C) == 0x00);
    // CSWL/CSWH installer.
    assert(card.slotRomRead(0x20) == 0xA9);
    assert(card.slotRomRead(0x21) == 0x31);
    assert(card.slotRomRead(0x25) == 0xC1);
    // Output handler — data port is $C0(8+s)0, the MAME data-latch offset.
    assert(card.slotRomRead(0x31) == 0x8D);
    assert(card.slotRomRead(0x32) == 0x90);     // $80 + 1*16 + 0
    assert(card.slotRomRead(0x33) == 0xC0);
    assert(card.slotRomRead(0x34) == 0x60);

    // Slot 3 rebakes slot-dependent bytes.
    GrapplerCard card3(3);
    assert(card3.slotRomRead(0x02) == 0xC3);
    assert(card3.slotRomRead(0x25) == 0xC3);
    assert(card3.slotRomRead(0x32) == 0xB0);    // $80 + 3*16 + 0

    // Expansion ROM is open bus while the stub is active.
    assert(card.expansionRomRead(0x000) == 0xFF);
    assert(card.expansionRomRead(0x100) == 0xFF);

    std::printf("  ok: stub ROM fingerprint\n");
}

void testDataPortSpool()
{
    GrapplerCard card(1);

    // Idle status on every offset: no IRQ (bit 7), DIP = 101 in bits 6-4
    // (Apple Dot Matrix — POM2's printer is an ImageWriter, see the
    // header), BUSY=0 (3), PE=0 (2), SELECT=1 (1), ACK set (0) → $53.
    for (uint8_t i = 0; i < 16; ++i)
        assert(card.deviceSelectRead(i) == 0x53);

    // Data latch decodes !(offset & 3): $0/$4/$8/$C all spool.
    card.deviceSelectWrite(0x0, 0xC8);
    card.deviceSelectWrite(0x4, 0xC9);
    card.deviceSelectWrite(0x8, 0x8D);
    assert(card.bytesWritten() == 3);
    const auto raw = card.spoolBytes();
    assert(raw[0] == 0xC8 && raw[1] == 0xC9 && raw[2] == 0x8D);
    assert(card.spoolText() == "HI\n");
    card.deviceSelectWrite(0xC, 0x21);
    assert(card.bytesWritten() == 4);

    // Non-data offsets do NOT spool: 1 = bank select, 2 = IRQ disable,
    // 3 = bank + disable, 5 = bank + enable…
    card.deviceSelectWrite(0x1, 0xFF);
    card.deviceSelectWrite(0x2, 0xFF);
    card.deviceSelectWrite(0x3, 0xFF);
    assert(card.bytesWritten() == 4);

    // IRQ enable (A2): with the instant-ACK latch set, the pending bit
    // appears in bit 7; A1 disables and releases it. (No bus attached —
    // assertIrq is a documented no-op when unplugged.)
    card.deviceSelectWrite(0x4 | 0x0, 0x00);       // data + keep enabled state
    card.deviceSelectWrite(0x5, 0x00);             // A0 bank + A2 enable
    assert((card.deviceSelectRead(0) & 0x80) != 0);
    card.deviceSelectWrite(0x2, 0x00);             // A1 disable
    assert((card.deviceSelectRead(0) & 0x80) == 0);

    // Reset: IRQ disabled, ACK latch back to set, idle status again.
    card.deviceSelectWrite(0x5, 0x00);
    card.onReset();
    assert(card.deviceSelectRead(0) == 0x53);

    card.clearSpool();
    assert(card.bytesWritten() == 0);

    std::printf("  ok: MAME c0nx decode + spool semantics\n");
}

void testRomLoadGate()
{
    GrapplerCard card(1);

    // Missing file is rejected, stub stays in place.
    assert(!card.loadRom("/this/path/does/not/exist.bin"));
    assert(!card.isRomLoaded());

    // Wrong-size payload is also rejected so a truncated dump doesn't
    // silently break software detection.
    const std::string tmp = pom2test::tempPath("pom2_grappler_bad.bin");
    {
        std::ofstream f(tmp, std::ios::binary);
        // Anything that isn't exactly 4096 bytes.
        for (int i = 0; i < 1024; ++i) f.put(static_cast<char>(i & 0xFF));
    }
    assert(!card.loadRom(tmp));
    assert(!card.isRomLoaded());
    std::remove(tmp.c_str());

    // A 4 KB blob loads cleanly. Fill = high byte of the address so the
    // two 2 KB expansion banks are distinguishable ($00-$07 low bank,
    // $08-$0F high bank).
    const std::string good = pom2test::tempPath("pom2_grappler_good.bin");
    {
        std::ofstream f(good, std::ios::binary);
        for (int i = 0; i < 4096; ++i) f.put(static_cast<char>((i >> 8) & 0xFF));
    }
    assert(card.loadRom(good));
    assert(card.isRomLoaded());
    // Slot ROM now mirrors the file bytes (page 0 of the dump).
    assert(card.slotRomRead(0x00) == 0x00);
    assert(card.slotRomRead(0x10) == 0x00);
    // Expansion ROM starts on the LOW 2 KB bank.
    assert(card.expansionRomRead(0x000) == 0x00);
    assert(card.expansionRomRead(0x100) == 0x01);
    // A0 device-select write flips to the HIGH bank (MAME set_rom_bank).
    card.deviceSelectWrite(0x1, 0x00);
    assert(card.expansionRomRead(0x000) == 0x08);
    assert(card.expansionRomRead(0x700) == 0x0F);
    // Any $CnXX read resets the bank to 0 (MAME read_cnxx side effect).
    (void)card.slotRomRead(0x00);
    assert(card.expansionRomRead(0x000) == 0x00);

    // $CnXX writes (a bus conflict on real hardware) still clock the bank
    // flip-flop low — MAME `write_cnxx` (grappler.cpp:586-591).
    card.deviceSelectWrite(0x1, 0x00);          // bank high again
    assert(card.expansionRomRead(0x000) == 0x08);
    card.slotRomWrite(0x00, 0xFF);
    assert(card.expansionRomRead(0x000) == 0x00);

    // Reset does NOT reset the ROM bank: the U2D flip-flop is not wired
    // to bus RESET — MAME `reset_from_bus` (grappler.cpp:536-539) and
    // `device_reset` (grappler.cpp:777-787) touch only the ACK latch and
    // the IRQ flip-flop. (An earlier POM2 revision cleared the bank here.)
    card.deviceSelectWrite(0x1, 0x00);          // bank high
    card.onReset();
    assert(card.expansionRomRead(0x000) == 0x08);   // still high bank
    (void)card.slotRomRead(0x00);                    // next $CnXX fetch drops it
    assert(card.expansionRomRead(0x000) == 0x00);

    // Snapshot round-trip: bank / ACK latch / IRQ-enable are guest-visible
    // state and must survive a rewind (they were absent from the snapshot
    // when the card first gained them).
    card.deviceSelectWrite(0x5, 0x00);          // bank high + IRQ enable
    assert((card.deviceSelectRead(0) & 0x80) != 0);
    std::vector<uint8_t> blob;
    card.appendSnapshotState(blob);
    (void)card.slotRomRead(0x00);               // drop the bank…
    card.onReset();                             // …and the IRQ enable
    assert(card.expansionRomRead(0x000) == 0x00);
    assert((card.deviceSelectRead(0) & 0x80) == 0);
    card.loadSnapshotState(blob.data(), blob.size());
    assert(card.expansionRomRead(0x000) == 0x08);          // bank restored
    assert((card.deviceSelectRead(0) & 0x80) != 0);        // IRQ restored
    std::remove(good.c_str());

    std::printf("  ok: ROM-load size gate + $C800 banking + snapshot\n");
}

void testPrinterTypeDip()
{
    // S1 printer type reads back at status bits 6-4 (MAME read_c0nx:
    // `(m_s1->read() & 0x07) << 4`). The firmware branches on it to pick
    // which printer dialect it speaks — Epson makes it emit `ESC K n1 n2`
    // binary graphics, which an ImageWriter renders as noise, so POM2
    // defaults to Apple Dot Matrix instead of MAME's Epson.
    GrapplerCard card(1);
    assert(card.printerType() == GrapplerCard::PrinterType::AppleDotMatrix);
    assert(((card.deviceSelectRead(0) >> 4) & 0x07) == 5);

    card.setPrinterType(GrapplerCard::PrinterType::Epson);
    assert(((card.deviceSelectRead(0) >> 4) & 0x07) == 0);
    card.setPrinterType(GrapplerCard::PrinterType::CItoh8510);
    assert(((card.deviceSelectRead(0) >> 4) & 0x07) == 1);
    // The DIP must not leak into the other status bits.
    assert((card.deviceSelectRead(0) & 0x0F) == 0x03);

    // S1:1 "Most Significant Bit" — open drops bit 7 at the latch
    // (MAME data_latched: `data & (BIT(s1,3) ? 0xff : 0x7f)`).
    assert(card.msbSoftwareControl());
    card.deviceSelectWrite(0x0, 0xC1);
    assert(card.spoolBytes().back() == 0xC1);
    card.setMsbSoftwareControl(false);
    card.deviceSelectWrite(0x0, 0xC1);
    assert(card.spoolBytes().back() == 0x41);

    std::printf("  ok: S1 printer-type DIP + MSB switch\n");
}

void testPrinterBusyHandshake()
{
    // The host-side ImageWriter reports a full input buffer; the card has
    // to make that visible the way the firmware senses it. The genuine
    // Grappler+ output routine spins on bit 0 (ACK), not bit 3 (BUSY):
    //     $CD89 JSR $CDE1 / AND #$02 (SELECT) / AND #$01 (ACK) / BEQ $CD89
    // so a busy printer must read back as "not acknowledged" or the guest
    // never waits for the paper.
    GrapplerCard card(1);
    assert(card.deviceSelectRead(0) == 0x53);      // idle: DIP + SELECT + ACK

    card.setPrinterBusy(true);
    assert(card.printerBusy());
    const uint8_t busyStatus = card.deviceSelectRead(0);
    assert((busyStatus & 0x01) == 0x00);           // ACK held clear
    assert((busyStatus & 0x08) == 0x08);           // BUSY asserted
    assert((busyStatus & 0x02) == 0x02);           // still on-line

    // Writing while busy still latches the byte (the card has a latch,
    // one byte deep) — it is the ACK the firmware waits on.
    card.deviceSelectWrite(0x0, 'X');
    assert(card.bytesWritten() == 1);
    assert((card.deviceSelectRead(0) & 0x01) == 0x00);

    // MAME `read_cnxx`: while the ACK latch is clear, address bit 6 is
    // forced low for $Cn80-$CnFF fetches. That sense path follows the
    // effective latch too.
    card.setPrinterBusy(false);
    assert(card.deviceSelectRead(0) == 0x53);      // acknowledged again

    // A busy printer can't raise the ACK interrupt either.
    card.deviceSelectWrite(0x5, 0x00);             // A2 = enable IRQ
    assert((card.deviceSelectRead(0) & 0x80) != 0);
    card.setPrinterBusy(true);
    assert((card.deviceSelectRead(0) & 0x80) == 0);
    card.setPrinterBusy(false);
    assert((card.deviceSelectRead(0) & 0x80) != 0);
    card.deviceSelectWrite(0x2, 0x00);             // A1 = disable IRQ

    std::printf("  ok: printer BUSY → ACK handshake\n");
}

void testSpoolIsBounded()
{
    GrapplerCard card(1);
    for (size_t i = 0; i < GrapplerCard::kMaxSpoolBytes + 9; ++i)
        card.deviceSelectWrite(0, static_cast<uint8_t>(i));
    assert(card.bytesWritten() == GrapplerCard::kMaxSpoolBytes + 9);
    assert(card.spoolBytes().size() == GrapplerCard::kMaxSpoolBytes);
    std::vector<uint8_t> fresh;
    assert(card.drainSpoolFrom(GrapplerCard::kMaxSpoolBytes + 4, fresh) ==
           GrapplerCard::kMaxSpoolBytes + 9);
    assert(fresh.size() == 5);
    std::printf("  ok: Grappler spool is bounded\n");
}

} // namespace

int main()
{
    std::printf("GrapplerCard smoke test\n");
    testStubRom();
    testDataPortSpool();
    testRomLoadGate();
    testPrinterTypeDip();
    testPrinterBusyHandshake();
    testSpoolIsBounded();
    std::printf("PASS\n");
    return 0;
}
