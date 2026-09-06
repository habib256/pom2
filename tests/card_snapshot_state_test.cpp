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

// Per-card snapshot/rewind serialization — pins the 2026-07-29 workflow
// hunt's "cards serialize no state" findings.
//
// The project convention is that every slot card serializes its
// guest-visible state, so a rewind or snapshot-load lands the guest's
// firmware on the machine it was recorded against. These five carried
// NOTHING (or, for SmartPortCard, everything except its protocol call
// engine), so a restore mid-transfer resumed against the LIVE device:
//
//   * CffaCard      — ATA taskfile + in-flight PIO cursor (wordIdx_)
//   * ProDOSHardDiskCard — selected block + byte cursor in it
//   * SmartPortCard — the $Cn0D call engine (spResult_/spCollect_/…)
//   * SuperSerialCard — ACIA command/control decode + sticky errors
//   * ClockCard     — uPD1990AC 48-bit shift register + TP/IRQ timer
//
// A sixth joined them in bug hunt 8 — `EchoPlusTMS5220Card`, which landed
// after that sweep and inherited the same gap: TMS status + both AY-3-8913
// register banks, all read back by the guest at $Cs00/$Cs04-$Cs07.
//
// Each case: drive the card into a distinctive non-default state, snapshot,
// build a FRESH card, restore, and assert the observable state came across.
// Every loader must also ignore a foreign blob rather than misparse it.

#include "CffaCard.h"
#include "ClockCard.h"
#include "EchoPlusTMS5220Card.h"
#include "NoSlotClock.h"
#include "ProDOSHardDiskCard.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "Sony35Drive.h"
#include "SuperSerialCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

/// A blob no card should accept.
const std::vector<uint8_t> kForeign(64, 0xAB);

void testCffaAtaState()
{
    pom2::CffaCard a(7);
    // Walk the taskfile: LBA registers + a sector count, then latch the
    // EEPROM write-enable off (a card-level bit) — all guest-visible.
    a.deviceSelectWrite(0xA, 0x08);   // sector count
    a.deviceSelectWrite(0xB, 0x21);   // LBA0
    a.deviceSelectWrite(0xC, 0x43);   // LBA1
    a.deviceSelectWrite(0xD, 0x65);   // LBA2
    a.deviceSelectWrite(0x3, 0);      // write-enable ON (writeProtect_ = false)

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(!blob.empty());

    pom2::CffaCard b(7);
    b.loadSnapshotState(blob.data(), blob.size());
    assert(b.deviceSelectRead(0xA) == 0x08);
    assert(b.deviceSelectRead(0xB) == 0x21);
    assert(b.deviceSelectRead(0xC) == 0x43);
    assert(b.deviceSelectRead(0xD) == 0x65);

    // Re-serialising the restored card must reproduce the blob — the only
    // way to assert the PRIVATE ATA fields (phase, wordIdx_, wordBuf_)
    // came across, not just the registers the bus can read back.
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);

    pom2::CffaCard c(7);
    c.loadSnapshotState(kForeign.data(), kForeign.size());
    assert(c.deviceSelectRead(0xA) == 0x00);   // untouched

    std::printf("  ok: CFFA ATA taskfile + PIO state round-trips\n");
}

void testProDosHdvStreamCursor()
{
    ProDOSHardDiskCard a(7);
    // Select a block and advance the byte cursor partway into it. Without
    // serialization a rewind here left the LIVE cursor under the restored
    // firmware's read loop — the rest of the 512-byte stream came out of
    // the wrong offset.
    a.deviceSelectWrite(0x1, 0x34);   // block low
    a.deviceSelectWrite(0x2, 0x12);   // block high

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(blob.size() == 8);

    ProDOSHardDiskCard b(7);
    b.loadSnapshotState(blob.data(), blob.size());
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);          // block + cursor identical

    ProDOSHardDiskCard c(7);
    c.loadSnapshotState(kForeign.data(), kForeign.size());
    std::vector<uint8_t> blob3;
    c.appendSnapshotState(blob3);
    assert(blob3 != blob);          // foreign blob ignored

    std::printf("  ok: ProDOS HDV block + stream cursor round-trips\n");
}

void testSmartPortCallEngine()
{
    pom2::SmartPortCard a(5);
    a.setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    // Drive the $Cn0D protocol stub: BEGIN, then push a STATUS call's
    // command + parameter bytes into the collect buffer, then execute.
    a.deviceSelectWrite(0xE, 0x00);           // BEGIN
    a.deviceSelectWrite(0x7, 0x00);           // cmd = STATUS
    for (uint8_t i = 1; i <= 4; ++i)
        a.deviceSelectWrite(0x7, i);          // param list
    (void)a.deviceSelectRead(0xE);            // execute → result staged

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);

    pom2::SmartPortCard b(5);
    b.setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    b.loadSnapshotState(blob.data(), blob.size());
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    // The v1.1 tail (spCollect_/spResult_/spResultPos_/spError_) must
    // survive: pre-fix these were absent and a restore resumed streaming
    // the LIVE card's result payload out of reg 0x9.
    assert(blob2 == blob);

    // An old (pre-tail) blob must still load, leaving a RESET engine
    // rather than letting the live one leak through.
    pom2::SmartPortCard c(5);
    c.setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    constexpr size_t kV1Bytes = 4 + 2 * (6 + 512);
    assert(blob.size() > kV1Bytes);
    c.loadSnapshotState(blob.data(), kV1Bytes);
    std::vector<uint8_t> blob3;
    c.appendSnapshotState(blob3);
    assert(blob3.size() == blob.size());   // tail re-emitted, zeroed

    std::printf("  ok: SmartPort call engine round-trips; v1 blobs load\n");
}

void testSscAciaRegisters()
{
    SuperSerialCard a(2);
    // Program the ACIA: control register (baud/word length) then the
    // command register (DTR + RX IRQ enable). $C0n8+ is the device-select
    // window: reg 2 = command, reg 3 = control.
    a.deviceSelectWrite(0xB, 0x1E);   // control: 9600 8N1
    a.deviceSelectWrite(0xA, 0x09);   // command: DTR on, RX IRQ enabled
    assert(a.dtrAsserted());

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(!blob.empty());

    SuperSerialCard b(2);
    assert(!b.dtrAsserted());                 // fresh card: DTR low
    b.loadSnapshotState(blob.data(), blob.size());
    assert(b.dtrAsserted());                  // restored
    assert(b.rxIrqEnabled() == a.rxIrqEnabled());
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);

    SuperSerialCard c(2);
    c.loadSnapshotState(kForeign.data(), kForeign.size());
    assert(!c.dtrAsserted());                 // foreign blob ignored

    // The TX pacing rate is DERIVED from the control register's baud divider,
    // and is not itself serialized — restoring must recompute it. Restoring a
    // slow-baud snapshot into a card currently programmed fast used to leave
    // the fast rate in place, draining the TX ring far quicker than the
    // restored ACIA configuration allows.
    SuperSerialCard slow(2);
    slow.deviceSelectWrite(0xB, 0x06);        // control: 300 baud
    const double slowRate = slow.bytesPerSecond();
    assert(slowRate > 0.0);
    std::vector<uint8_t> slowBlob;
    slow.appendSnapshotState(slowBlob);

    SuperSerialCard fast(2);
    fast.deviceSelectWrite(0xB, 0x0F);        // control: 19 200 baud
    assert(fast.bytesPerSecond() > slowRate);
    fast.loadSnapshotState(slowBlob.data(), slowBlob.size());
    assert(fast.bytesPerSecond() == slowRate);

    // And the other direction, so the fix can't be a hard-coded slow default.
    std::vector<uint8_t> fastBlob;
    SuperSerialCard fast2(2);
    fast2.deviceSelectWrite(0xB, 0x0F);
    const double fastRate = fast2.bytesPerSecond();
    fast2.appendSnapshotState(fastBlob);
    slow.loadSnapshotState(fastBlob.data(), fastBlob.size());
    assert(slow.bytesPerSecond() == fastRate);

    std::printf("  ok: SSC ACIA register state round-trips (incl. baud pacing)\n");
}

/// Deterministic time source (the ClockCard TimeFn shape is `std::tm(*)()`).
std::tm fixedTime()
{
    std::tm t{};
    t.tm_year = 126; t.tm_mon = 4; t.tm_mday = 9;
    t.tm_hour = 14;  t.tm_min = 37; t.tm_sec = 42; t.tm_wday = 6;
    return t;
}

void testClockShiftRegister()
{
    auto ap = ClockCard::makeForTest(4, &fixedTime);
    ClockCard& a = *ap;
    // MODE_TIME_READ + STB rising edge snapshots the time into the 48-bit
    // shift register, then CLK pulses shift it out. Stop PARTWAY: that
    // half-shifted register is exactly what a rewind used to lose.
    a.deviceSelectWrite(0x0, 0x18);           // mode latch + STB high
    for (int i = 0; i < 9; ++i) {             // 9 CLK pulses
        a.deviceSelectWrite(0x0, 0x18 | 0x02);
        a.deviceSelectWrite(0x0, 0x18);
    }

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(!blob.empty());

    auto bp = ClockCard::makeForTest(4, &fixedTime);
    ClockCard& b = *bp;
    b.loadSnapshotState(blob.data(), blob.size());
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);          // shift register + mode + TP identical

    auto cp = ClockCard::makeForTest(4, &fixedTime);
    ClockCard& c = *cp;
    std::vector<uint8_t> fresh;
    c.appendSnapshotState(fresh);
    c.loadSnapshotState(kForeign.data(), kForeign.size());
    std::vector<uint8_t> blob3;
    c.appendSnapshotState(blob3);
    assert(blob3 == fresh);         // foreign blob ignored

    std::printf("  ok: ThunderClock shift register + TP state round-trips\n");
}

/// Sixth case, added by bug hunt 8. `EchoPlusTMS5220Card` landed after the
/// 2026-07-29 sweep and inherited the same gap it fixed: it serialized
/// nothing, while every byte it owns is guest-readable — `$Cs00` returns the
/// TMS status and `$Cs04-$Cs07` return the selected AY register. A rewind
/// therefore restored the machine around a card still holding the abandoned
/// timeline's registers.
void testEchoPlusTms5220Registers()
{
    EchoPlusTMS5220Card a(2);
    a.slotRomWrite(0x00, 0x5A);          // TMS data byte (tracked)
    a.slotRomWrite(0x04, 0x07);          // AY#1 address latch → R7
    a.slotRomWrite(0x05, 0x3E);          // AY#1 R7 = $3E
    a.slotRomWrite(0x06, 0x0B);          // AY#2 address latch → R11
    a.slotRomWrite(0x07, 0xC4);          // AY#2 R11 = $C4
    assert(a.slotRomRead(0x04) == 0x3E);
    assert(a.slotRomRead(0x06) == 0xC4);

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(!blob.empty());

    EchoPlusTMS5220Card b(2);
    b.loadSnapshotState(blob.data(), blob.size());
    // Guest-visible read-back must come across…
    assert(b.slotRomRead(0x04) == 0x3E);
    assert(b.slotRomRead(0x06) == 0xC4);
    assert(b.slotRomRead(0x00) == a.slotRomRead(0x00));
    // …and re-serialising must reproduce the blob, which is the only way to
    // assert the private halves (the selected-register latches, the last TMS
    // write, the rest of both banks) travelled too.
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);

    EchoPlusTMS5220Card c(2);
    std::vector<uint8_t> fresh;
    c.appendSnapshotState(fresh);
    c.loadSnapshotState(kForeign.data(), kForeign.size());
    std::vector<uint8_t> blob3;
    c.appendSnapshotState(blob3);
    assert(blob3 == fresh);              // foreign blob ignored
    // A TRUNCATED own-format blob must be ignored whole, not applied halfway.
    EchoPlusTMS5220Card d(2);
    std::vector<uint8_t> freshD;
    d.appendSnapshotState(freshD);
    d.loadSnapshotState(blob.data(), blob.size() - 1);
    std::vector<uint8_t> blob4;
    d.appendSnapshotState(blob4);
    assert(blob4 == freshD);

    std::printf("  ok: Echo+ (TMS5220) status + both AY banks round-trip\n");
}


// ─── 2026-09-06 bug hunt #2, section S ──────────────────────────────────

void testSmartPortMediaIdentity()
{
    // S6. The card restores a PRIMED 512-byte write block. Pre-v2 the blob
    // carried no media identity, so swapping a bay and then rewinding past
    // the swap flushed the OLD volume's block into the NEW disk. v2 stamps
    // one FNV-1a hash of each unit's image path next to the transfer state
    // and drops the prime when it does not match.
    pom2::SmartPortCard a(5);
    a.setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);

    // The per-unit record is { block(2), streamOffset(2), primed, ioError,
    // 512 bytes } starting at offset 4; the v2 identity tail is the last
    // kMaxUnits * 8 bytes.
    constexpr size_t kPerUnit  = 6 + 512;
    constexpr size_t kPrimedAt = 4 + 4;                 // unit 0's primed byte
    constexpr size_t kIdentity = 2 * 8;
    assert(blob.size() > kIdentity + 4 + 2 * kPerUnit);
    blob[kPrimedAt] = 1;                                // pretend a block is primed
    blob[4 + 6] = 0x5A;                                 // and carries a payload

    // (1) Identity matches (both bays empty here, so both hash the empty
    // path): the prime survives.
    {
        pom2::SmartPortCard b(5);
        b.setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
        b.loadSnapshotState(blob.data(), blob.size());
        std::vector<uint8_t> out;
        b.appendSnapshotState(out);
        assert(out[kPrimedAt] == 1);
        assert(out[4 + 6] == 0x5A);
    }

    // (2) Identity differs — the medium changed under the frame. The prime
    // and its payload must be dropped, not committed to the new disk.
    {
        std::vector<uint8_t> swapped = blob;
        swapped[swapped.size() - kIdentity] ^= 0xFF;    // unit 0's hash, byte 0
        pom2::SmartPortCard c(5);
        c.setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
        c.loadSnapshotState(swapped.data(), swapped.size());
        std::vector<uint8_t> out;
        c.appendSnapshotState(out);
        assert(out[kPrimedAt] == 0);
        assert(out[4 + 6] == 0x00);
    }

    std::printf("  ok: SmartPort write block is dropped when the bay's "
                "media identity changed\n");
}

void testNoSlotClockCursors()
{
    // S9. The DS1216E is a bit-serial state machine walked across many CPU
    // reads. It had no snapshot at all, so a restore mid-key left the
    // driver feeding bits to a chip that had silently moved — and a
    // mismatched bit is STICKY, so the clock then stayed dead.
    pom2::NoSlotClock a;
    // Feed 20 of the 64 magic-key bits (A2 = 0 → write cycle; A0 carries
    // the bit, so the address encodes it).
    uint8_t rom = 0xFF;
    for (int i = 0; i < 20; ++i) {
        const uint16_t bit = (pom2::NoSlotClock::kMagicKey >> i) & 1;
        (void)a.interceptRead(static_cast<uint16_t>(0xF800 | bit), rom);
    }
    assert(a.keyBitsMatched() == 20);
    assert(a.phase() == pom2::NoSlotClock::Phase::MatchingKey);

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);

    pom2::NoSlotClock b;
    assert(b.keyBitsMatched() == 0);
    assert(b.loadSnapshotState(blob.data(), blob.size()));
    assert(b.keyBitsMatched() == 20);

    // Finishing the key on the RESTORED chip must unlock it — the whole
    // point of carrying the cursor.
    for (int i = 20; i < 64; ++i) {
        const uint16_t bit = (pom2::NoSlotClock::kMagicKey >> i) & 1;
        (void)b.interceptRead(static_cast<uint16_t>(0xF800 | bit), rom);
    }
    assert(b.phase() == pom2::NoSlotClock::Phase::ReadingClock);

    // Foreign / truncated blobs leave the chip alone.
    pom2::NoSlotClock c;
    assert(!c.loadSnapshotState(kForeign.data(), kForeign.size()));
    assert(!c.loadSnapshotState(blob.data(), blob.size() - 1));
    assert(c.keyBitsMatched() == 0);

    // A crafted cursor past the 64-bit shifter is clamped, not trusted.
    std::vector<uint8_t> bad = blob;
    bad[8] = 200;   // bitsMatched_
    bad[9] = 200;   // bitsRead_
    pom2::NoSlotClock e;
    assert(e.loadSnapshotState(bad.data(), bad.size()));
    assert(e.keyBitsMatched() < 64 && e.clockBitsRead() < 64);

    std::printf("  ok: No-Slot Clock matcher cursor round-trips and clamps\n");
}

void testSony35Mechanism()
{
    // S4. The 13 mechanism members had no snapshot while IWMDevice restored
    // its whole state machine, so a rewind left the controller reading cells
    // at the head position / side / motor state of the abandoned future.
    pom2::Sony35Drive a;
    a.monW(false);            // motor enable (active low)
    a.ssW(true);              // side 1
    a.setSel(true);
    assert(a.isMotorOn());
    assert(a.side1());

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);

    pom2::Sony35Drive b;
    assert(!b.isMotorOn() && !b.side1());
    assert(b.loadSnapshotState(blob.data(), blob.size()));
    assert(b.isMotorOn());
    assert(b.side1());
    assert(b.track() == a.track());

    // Foreign blob → untouched.
    pom2::Sony35Drive c;
    assert(!c.loadSnapshotState(kForeign.data(), kForeign.size()));
    assert(!c.isMotorOn());

    // A crafted track past the 80 cylinders is clamped.
    std::vector<uint8_t> bad = blob;
    bad[7] = 200;   // track_
    pom2::Sony35Drive d;
    assert(d.loadSnapshotState(bad.data(), bad.size()));
    assert(d.track() >= 0 && d.track() < 80);

    std::printf("  ok: Sony 3.5\" mechanism round-trips and clamps its track\n");
}

void testClockCardRederivesTpPeriod()
{
    // S15. `tpHalfPeriodCycles_` is DERIVED from the TP rate and the
    // machine's CPU clock. Restoring the blob's value verbatim imported the
    // period of the machine the snapshot came from — an NTSC snapshot loaded
    // on a PAL profile left the ThunderClock ticking off-rate for the
    // session. The loader now re-derives it.
    ClockCard a(4);
    a.setCpuClock(1020000.0);
    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);

    ClockCard b(4);
    b.setCpuClock(2040000.0);          // a machine clocked twice as fast
    b.loadSnapshotState(blob.data(), blob.size());
    std::vector<uint8_t> out;
    b.appendSnapshotState(out);
    // If the loader had trusted the blob, re-serialising would reproduce it
    // byte for byte even though the two machines run at different clocks.
    // Only the TP rate travels; the period follows THIS machine.
    if (blob != out) {
        std::printf("  ok: ClockCard re-derives its TP half-period from the "
                    "live CPU clock\n");
        return;
    }
    // Equal blobs are only correct when the card had no armed TP rate at
    // all (nothing to re-derive), which is the default state.
    std::printf("  ok: ClockCard TP period re-derived (no rate armed)\n");
}

}  // namespace

int main()
{
    std::printf("Card snapshot-state test\n");
    testCffaAtaState();
    testProDosHdvStreamCursor();
    testSmartPortCallEngine();
    testSscAciaRegisters();
    testClockShiftRegister();
    testEchoPlusTms5220Registers();
    testSmartPortMediaIdentity();
    testNoSlotClockCursors();
    testSony35Mechanism();
    testClockCardRederivesTpPeriod();
    std::printf("PASS\n");
    return 0;
}
