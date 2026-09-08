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

// ClockCard smoke test — pins the ProDOS clock-detection signature and
// the uPD1990AC bit-bang protocol the card emulates so ProDOS's hard-
// coded ThunderClock+ driver can talk to it. The driver lives inside
// ProDOS itself; here we drive the chip from C++ exactly as a host
// would and assert it serialises out the right BCD bytes.

#include "ClockCard.h"

#include <cassert>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>

namespace {

constexpr uint8_t kBitDataIn = 0x01;
constexpr uint8_t kBitClk    = 0x02;
constexpr uint8_t kBitStb    = 0x04;
// Mode bits live in bits 3..5. MODE_TIME_READ = 0b011 → 0x18 raw.
constexpr uint8_t kModeTimeReadShifted = 0x03 << 3;     // 0x18

std::tm fixedTime_2026_05_09_14_37_42()
{
    std::tm t{};
    t.tm_year  = 126;     // 2026 - 1900
    t.tm_mon   = 4;       // May (0-indexed)
    t.tm_mday  = 9;
    t.tm_hour  = 14;
    t.tm_min   = 37;
    t.tm_sec   = 42;
    t.tm_wday  = 6;       // Saturday
    return t;
}

void testSignature()
{
    ClockCard card(4);
    // ProDOS scans for these exact bytes at the listed offsets. This is the
    // real contract and holds whichever ROM the card ended up with — the
    // ctor loads `roms/thunderclock_u9_v1.3.bin` when the user has it, and
    // `tryLoadDump` rejects any dump that lacks this signature.
    assert(card.slotRomRead(0x00) == 0x08);     // PHP
    assert(card.slotRomRead(0x02) == 0x28);     // PLP
    assert(card.slotRomRead(0x04) == 0x58);     // CLI
    assert(card.slotRomRead(0x06) == 0x70);     // BVS

    if (card.romFromDump()) {
        // Everything below describes POM2's SYNTHETIC ROM layout, which a
        // real Thunderware EPROM has no reason to share: it puts its own
        // firmware in those bytes. Asserting them unconditionally made this
        // test fail the moment a user dropped the genuine dump into roms/ —
        // a green suite that turns red because the emulator got *more*
        // faithful is a broken test, not a broken card.
        std::printf("clock_card_smoke: real dump present (%s) — synthetic "
                    "filler bytes not asserted\n", card.romSource().c_str());
        return;
    }
    // BVS operand must be 0 so the no-overflow path also lands at $Cs08.
    assert(card.slotRomRead(0x07) == 0x00);
    // Filler bytes between signature ops are 1-byte instructions so the
    // CPU can walk straight through if anything ever JMPs to $Cs00.
    assert(card.slotRomRead(0x01) == 0xD8);     // CLD
    assert(card.slotRomRead(0x03) == 0xD8);     // CLD
    assert(card.slotRomRead(0x05) == 0x78);     // SEI (re-disable IRQ)
    // $Cs08 is RTS — a stray JMP $Cs00 falls through and returns cleanly.
    assert(card.slotRomRead(0x08) == 0x60);
}

// Helper: read one DATA_OUT bit and advance the chip by pulsing CLK.
// The chip's DATA_OUT is "live" (always the current LSB of the shift
// register), so the host samples BEFORE the CLK rising edge. The
// rising edge shifts the register one position right so the *next*
// call sees the next bit. This matches the loop in ProDOS's hardcoded
// ThunderClock driver: `LDA $C080,X ; ASL A ; ROR buf ; <pulse CLK>`.
uint8_t readBitThenAdvance(ClockCard& card, uint8_t baseline)
{
    const uint8_t bit = (card.deviceSelectRead(0) & 0x80) ? 1 : 0;
    card.deviceSelectWrite(0, baseline & ~kBitClk);    // CLK low (idempotent)
    card.deviceSelectWrite(0, baseline |  kBitClk);    // CLK rising → shift
    card.deviceSelectWrite(0, baseline & ~kBitClk);    // CLK back low
    return bit;
}

// Helper: read 8 bits LSB-first from the chip into one BCD byte.
uint8_t readByteLsbFirst(ClockCard& card, uint8_t baseline)
{
    uint8_t out = 0;
    for (int i = 0; i < 8; ++i) {
        const uint8_t bit = readBitThenAdvance(card, baseline);
        out = static_cast<uint8_t>(out | (bit << i));
    }
    return out;
}

// End-to-end of the read path: latch host time via STB+MODE_TIME_READ,
// shift out 6 bytes, and check each one against the fixed timestamp.
void testTimeReadProtocol()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    // Step 1: arm MODE_TIME_READ on C0/C1/C2 with STB still low.
    //   bits 3..5 = 011 → 0x18; STB = 0; CLK = 0.
    card->deviceSelectWrite(0, kModeTimeReadShifted);

    // Step 2: STB rising edge with mode=TIME_READ snapshots host time
    // into the 40-bit shift register.
    card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);

    // Step 3: drop STB so subsequent writes only pulse CLK. Mode bits
    // can be left at 0 — the chip latched on STB rise. Keep them at
    // TIME_READ to match what real driver code does.
    const uint8_t baseline = kModeTimeReadShifted;     // STB low, CLK low
    card->deviceSelectWrite(0, baseline);

    // Step 4: clock out the 5 bytes the chip holds, LSB-first. 40-bit
    // register — no year byte (the shipped ThunderClock firmware reads
    // exactly 10 nibbles = 40 CLK pulses; see ClockCard's 40-bit note).
    const uint8_t sec    = readByteLsbFirst(*card, baseline);
    const uint8_t min    = readByteLsbFirst(*card, baseline);
    const uint8_t hour   = readByteLsbFirst(*card, baseline);
    const uint8_t day    = readByteLsbFirst(*card, baseline);
    const uint8_t mthDow = readByteLsbFirst(*card, baseline);

    // 2026-05-09 14:37:42 Saturday → BCD bytes:
    //   sec=$42, min=$37, hour=$14, day=$09.
    //   shiftReg[4] packs month (4-bit binary, NOT BCD) in the high
    //   nibble and day-of-week in the low nibble (MAME upd1990a.cpp:95).
    //   May=5, Saturday=6 → (5 << 4) | 6 = $56.
    assert(sec    == 0x42);
    assert(min    == 0x37);
    assert(hour   == 0x14);
    assert(day    == 0x09);
    assert(mthDow == 0x56);
}

// After all bits have been shifted out, further CLK pulses should keep
// reading zeros (the shift register has been emptied) and never hang or
// spin. This pins the safety of an over-long driver loop.
void testShiftDrainsToZero()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);
    card->deviceSelectWrite(0, kModeTimeReadShifted);
    card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);
    const uint8_t baseline = kModeTimeReadShifted;
    card->deviceSelectWrite(0, baseline);

    // Shift out all 48 bits.
    for (int i = 0; i < 48; ++i) (void)readBitThenAdvance(*card, baseline);

    // Next 16 bits should all read zero — the register has drained.
    for (int i = 0; i < 16; ++i) {
        assert(readBitThenAdvance(*card, baseline) == 0);
    }
}

// Multiple STB pulses re-snapshot host time. The test injector returns
// the same time each call, so the bytes after a re-snapshot should
// match the bytes from the first snapshot.
void testStbRelatchesTime()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    auto readSeconds = [&]() -> uint8_t {
        card->deviceSelectWrite(0, kModeTimeReadShifted);
        card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);
        const uint8_t baseline = kModeTimeReadShifted;
        card->deviceSelectWrite(0, baseline);
        return readByteLsbFirst(*card, baseline);
    };

    assert(readSeconds() == 0x42);
    // Drop STB, re-arm — should still read the same fixed timestamp.
    assert(readSeconds() == 0x42);
}

// Reads across $C0n0-$C0nF all mirror DATA_OUT (MAME
// `a2thunderclock.cpp:112-115` ignores `offset` on read). After
// `onReset` the shift register is zero, so bit 7 = 0 on every offset.
void testDataOutMirrorOnAllDeviceSelectOffsets()
{
    ClockCard card(4);
    assert(card.deviceSelectRead(0)  == 0x00);
    assert(card.deviceSelectRead(1)  == 0x00);
    assert(card.deviceSelectRead(2)  == 0x00);
    assert(card.deviceSelectRead(15) == 0x00);
}

// MODE_TIME_SET round-trip: load 48 bits via DATA_IN + CLK in MODE_SHIFT,
// commit via STB rising edge with mode=TIME_SET, then read back through
// the normal TIME_READ path and confirm the bytes match. Pins both the
// DATA_IN → MSB-of-shiftReg[4] injection on CLK and the shiftReg →
// time-base commit on STB-in-TIME_SET (MAME `upd1990a.cpp:194-225`).
void testTimeSetRoundTrip()
{
    // The injector returns a fixed "host" time; the chip should layer
    // the user-set offset on top, so subsequent reads return the SET
    // bytes regardless of what the injector says.
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    // Desired set time: Dec-31 23:58:59 Sunday. BCD bytes shifted out
    // LSB-first: sec=$59, min=$58, hour=$23, day=$31,
    // mthDow = (12<<4) | 0 = $C0 (Sunday = day-of-week 0). NO year byte:
    // the uPD1990A(C) register is 40 bits (MAME upd1990a.cpp `max_shift`
    // is 5 for a non-4990A part), and the shipped ThunderClock firmware
    // clocks exactly 40 pulses — 10 nibbles over sec/min/hour/day/
    // month+dow. Driving 48 used to slide every field by one byte.
    //
    // The shift register's serial-out layout is byte0 → byte1 → ... →
    // byte5 (each byte LSB-first). The corresponding serial-in order
    // — what we need to drive on DATA_IN as we pulse CLK 48 times —
    // is the *reverse*: send byte4's MSB first, ... down to byte0's
    // LSB last. (Each CLK pulse injects DATA_IN into shiftReg[4]'s MSB
    // and shifts everything one bit right.)

    const uint8_t target[5] = {
        0x59, 0x58, 0x23, 0x31, 0xC0
    };
    constexpr uint8_t kModeShiftShifted   = 0x01 << 3;     // C0/C1/C2 = 001
    constexpr uint8_t kModeTimeSetShifted = 0x02 << 3;     // C0/C1/C2 = 010

    // Arm MODE_SHIFT (no STB pulse yet — just hold the mode bits).
    card->deviceSelectWrite(0, kModeShiftShifted);

    // Clock 40 bits in. For position k in [0, 40), the bit value is
    // bit (k % 8) of target[(k/8)] — i.e. byte 0's LSB first, then
    // byte 0's bit 1, …, byte 5's MSB last. As each CLK pulse injects
    // DATA_IN at shiftReg[4]'s MSB and shifts right by one bit, the
    // bit that lands at shiftReg[0]'s LSB after 40 cycles is the one
    // that was clocked in FIRST. So clock byte 0 bit 0 first, byte 0
    // bit 1 next, …, byte 4 bit 7 last.
    for (int k = 0; k < 40; ++k) {
        const int byteIdx = k / 8;
        const int bitIdx  = k % 8;
        const uint8_t bit = (target[byteIdx] >> bitIdx) & 1;
        const uint8_t baseline =
            static_cast<uint8_t>(kModeShiftShifted | (bit ? kBitDataIn : 0));
        card->deviceSelectWrite(0, baseline);                 // CLK low
        card->deviceSelectWrite(0, baseline | kBitClk);       // CLK rising → shift
        card->deviceSelectWrite(0, baseline);                 // CLK back low
    }

    // Switch to MODE_TIME_SET and pulse STB to commit the shift register
    // to the chip's time base (MAME `upd1990a.cpp:194-225`).
    card->deviceSelectWrite(0, kModeTimeSetShifted);
    card->deviceSelectWrite(0, kModeTimeSetShifted | kBitStb);
    card->deviceSelectWrite(0, kModeTimeSetShifted);

    // Now read back via the standard TIME_READ path. The bytes should
    // match the target (the injector returns the same fixed timestamp
    // each call so the effective time stays pinned at our set value).
    card->deviceSelectWrite(0, kModeTimeReadShifted);
    card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);
    const uint8_t baseline = kModeTimeReadShifted;
    card->deviceSelectWrite(0, baseline);

    const uint8_t got[5] = {
        readByteLsbFirst(*card, baseline),
        readByteLsbFirst(*card, baseline),
        readByteLsbFirst(*card, baseline),
        readByteLsbFirst(*card, baseline),
        readByteLsbFirst(*card, baseline),
    };

    // mktime normalises the input — DST and locale boundaries can
    // shift seconds by up to one hour. Assert the secondary fields
    // exactly (they don't move under normalisation), and let hour /
    // dow flex by one hour / one day.
    assert(got[0] == 0x59);              // seconds
    assert(got[1] == 0x58);              // minutes
    // hour might shift ±1 across DST, but for Dec 31 / May 9 the
    // injector pair sits comfortably outside any DST transition; we
    // still allow a window of ±1 to keep the test portable.
    assert(got[2] == 0x23 || got[2] == 0x22 || got[2] == 0x00);
    assert(got[3] == 0x31);              // day
    // mthDow: month (high nibble) must be 12. dow may be normalised by
    // mktime against the real Dec 31 1995 (Sunday → tm_wday=0). Either
    // value 0 or 0xC0 is acceptable.
    assert((got[4] & 0xF0) == 0xC0);
    // No year assertion: the chip has no year counter (40-bit register).
    // commitTimeSetFromShiftReg takes the year from the host clock, which
    // is what a real ThunderClock+ does — ProDOS reads only month/day/
    // time from the card and supplies the year itself.
}

// MODE_SHIFT is *not* strictly gated in POM2 (deliberate divergence
// from MAME `upd1990a.cpp:312-327` — see ClockCard.cpp commentary).
// This test pins the divergence: clocking CLK while in MODE_TIME_READ
// also shifts the register. ProDOS's ThunderClock driver depends on
// this, and the testTimeReadProtocol() case above already exercises it
// implicitly — here we just spell out that the mode bits don't matter
// for the shift action.
void testShiftLaxAcrossModes()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    // Pre-load shift register via TIME_READ.
    card->deviceSelectWrite(0, kModeTimeReadShifted);
    card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);

    // Read 4 bits while keeping mode = REGISTER_HOLD (mode bits 000),
    // which in MAME would block any shift. POM2's lax behaviour shifts
    // anyway — the first 4 bits of $42 ($42 = 0100_0010 LSB-first: 0,1,0,0,0,0,1,0).
    const uint8_t baseline = 0x00;       // mode = HOLD, STB=0, CLK=0
    card->deviceSelectWrite(0, baseline);

    assert(readBitThenAdvance(*card, baseline) == 0);
    assert(readBitThenAdvance(*card, baseline) == 1);
    assert(readBitThenAdvance(*card, baseline) == 0);
    assert(readBitThenAdvance(*card, baseline) == 0);
}

// ─── TP (Timing-Pulse) interrupt path ────────────────────────────────────
//
// The uPD1990AC's TP output toggles at a software-selectable rate; the
// ThunderClock+ gates it to the slot IRQ line via the $C0n0 bit-6 enable
// latch. These cases pin the rates (MAME `upd1990a.cpp:248-257`) and the
// card-level IRQ wiring (ThunderClock Plus manual ch. V).

constexpr uint8_t kBitIrqEnable = 0x40;

// One emulated second of CPU cycles (matches POM2_CPU_CLOCK_HZ).
constexpr int kCpuHz = 1022727;

// Latch a TP rate via STB and enable interrupts. `tpMode` is the raw
// C0/C1/C2 mode code (4=64Hz, 5=256Hz, 6=2048Hz; 7 is MODE_TEST on this
// part, not a rate — see testModeSevenIsTestNotTp4096 below).
void armTpInterrupts(ClockCard& card, uint8_t tpMode)
{
    const uint8_t modeShifted = static_cast<uint8_t>(tpMode << 3);
    card.deviceSelectWrite(0, modeShifted);              // set C0/C1/C2, STB low
    card.deviceSelectWrite(0, modeShifted | kBitStb);    // STB rising → program TP
    card.deviceSelectWrite(0, modeShifted);              // STB low
    card.deviceSelectWrite(0, kBitIrqEnable);            // $40 → enable IRQs
}

// Count IRQ assertions over `totalCycles`, clearing after each so the next
// TP edge re-asserts. Steps in small increments so each edge is observed.
int countIrqPulses(ClockCard& card, int totalCycles)
{
    constexpr int kStep = 64;     // < the shortest TP period (~250 cyc @ 4096Hz)
    int count = 0;
    for (int c = 0; c < totalCycles; c += kStep) {
        card.advanceCycles(kStep);
        if (card.slotIrqAsserted()) {
            ++count;
            card.deviceSelectRead(8);     // clear via device-select read
        }
    }
    return count;
}

// Each TP rate produces ~rate IRQ pulses per emulated second.
void testTpRatesProduceExpectedPulseCounts()
{
    struct { uint8_t mode; int rateHz; } cases[] = {
        { 0x04,   64 }, { 0x05,  256 }, { 0x06, 2048 },
    };
    for (const auto& c : cases) {
        auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);
        assert(card->tpRateHz() == 0);            // TP off until a rate latched
        armTpInterrupts(*card, c.mode);
        assert(card->tpRateHz() == c.rateHz);
        assert(card->interruptsEnabled());

        const int pulses = countIrqPulses(*card, kCpuHz);
        // Allow a small tolerance for cycle-budget rounding + step phasing.
        const int lo = c.rateHz - c.rateHz / 32 - 1;
        const int hi = c.rateHz + c.rateHz / 32 + 1;
        assert(pulses >= lo && pulses <= hi);
    }
}

// Mode code 7 on the PARALLEL C0/C1/C2 field is MODE_TEST, not TP 4096 Hz.
//
// MAME `upd1990a.cpp:198-205` `stb_w`: `if (is_serial_mode()) m_c =
// m_shift_reg[6]; else { m_c = m_c_unlatched; if (m_c == 7) m_c =
// MODE_TEST; }`, and `is_serial_mode()` (`:63-67`) needs a TYPE_4990A —
// `a2thunderclock.cpp:94` fits a plain `UPD1990A(…, 32.768_kHz_XTAL)`.
// MODE_TP_4096HZ is enum 7 but MODE_TEST is enum 15 (`upd1990a.h:76-91`),
// so the ThunderClock+ can never select 4096 Hz. POM2 read code 7 as that
// rate and handed a guest 4096 IRQs/s the real card cannot produce.
//
// MODE_TEST itself leaves TP alone (`:344-359` never touches `m_timer_tp`)
// and makes a following SHIFT / TIME_READ latch put TP at XTAL/1024 = 32 Hz
// (`:240-242`, `:307-309`).
void testModeSevenIsTestNotTp4096()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    // Start from a real rate so "leaves TP alone" is observable.
    armTpInterrupts(*card, 0x06);
    assert(card->tpRateHz() == 2048);
    assert(!card->testModeActive());

    // Latch code 7: test mode on, TP untouched — NOT 4096 Hz.
    const uint8_t seven = static_cast<uint8_t>(0x07 << 3);
    card->deviceSelectWrite(0, seven);
    card->deviceSelectWrite(0, seven | kBitStb);
    card->deviceSelectWrite(0, seven);
    assert(card->testModeActive() && "code 7 must latch MODE_TEST");
    assert(card->tpRateHz() == 2048 &&
           "MODE_TEST does not reprogram TP, and 4096 Hz is unreachable here");

    // A TIME_READ latch while in test mode drops TP to 32 Hz.
    const uint8_t timeRead = static_cast<uint8_t>(0x03 << 3);
    card->deviceSelectWrite(0, timeRead);
    card->deviceSelectWrite(0, timeRead | kBitStb);
    card->deviceSelectWrite(0, timeRead);
    assert(card->tpRateHz() == 32 && "32 Hz time pulse in testmode");
    assert(card->testModeActive() &&
           "TIME_READ does not clear test mode (MAME :210-219)");

    // ...and a TP-rate latch leaves test mode and restores a normal rate.
    const uint8_t tp256 = static_cast<uint8_t>(0x05 << 3);
    card->deviceSelectWrite(0, tp256);
    card->deviceSelectWrite(0, tp256 | kBitStb);
    card->deviceSelectWrite(0, tp256);
    assert(!card->testModeActive());
    assert(card->tpRateHz() == 256);

    // A reset clears it too.
    card->deviceSelectWrite(0, seven);
    card->deviceSelectWrite(0, seven | kBitStb);
    assert(card->testModeActive());
    card->onReset();
    assert(!card->testModeActive());
}

// With interrupts disabled, TP still toggles internally but never pulls IRQ.
void testNoIrqWhenDisabled()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);
    // Latch 2048 Hz but do NOT write the $40 enable.
    const uint8_t modeShifted = 0x06 << 3;
    card->deviceSelectWrite(0, modeShifted);
    card->deviceSelectWrite(0, modeShifted | kBitStb);
    card->deviceSelectWrite(0, modeShifted);
    assert(card->tpRateHz() == 2048);
    assert(!card->interruptsEnabled());

    card->advanceCycles(kCpuHz);
    assert(!card->slotIrqAsserted());
    assert(!card->interruptPending());
}

// Writing bit-6-clear to $C0n0 (as the clock-read driver does) disables
// interrupts; a bare $40 write re-arms without disturbing the TP rate.
void testEnableLatchTracksBit6()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);
    armTpInterrupts(*card, 0x06);                 // 2048 Hz, enabled
    assert(card->interruptsEnabled());

    card->deviceSelectWrite(0, 0x00);             // bit 6 clear → disable
    assert(!card->interruptsEnabled());
    card->advanceCycles(kCpuHz);
    assert(!card->slotIrqAsserted());

    card->deviceSelectWrite(0, kBitIrqEnable);    // $40 → re-enable, rate intact
    assert(card->interruptsEnabled());
    assert(card->tpRateHz() == 2048);
    assert(countIrqPulses(*card, kCpuHz) > 1900);
}

// The "interrupt asserted" flag (read $C0n0 bit 5) reflects a pending
// request and is cleared by the read (manual 5-3 polling sequence).
void testInterruptAssertedFlagBit5()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);
    armTpInterrupts(*card, 0x06);                 // 2048 Hz, enabled

    // Advance one full TP period (~500 cyc) so a pulse latches.
    card->advanceCycles(kCpuHz / 2048 + 4);
    assert(card->slotIrqAsserted());
    assert(card->interruptPending());

    // Reading $C0n0 returns bit 5 set, then clears the request.
    const uint8_t reg = card->deviceSelectRead(0);
    assert((reg & 0x20) != 0);                    // bit 5 = interrupt asserted
    assert(!card->slotIrqAsserted());             // cleared by the read
    assert(!card->interruptPending());

    // A second read now sees bit 5 clear.
    assert((card->deviceSelectRead(0) & 0x20) == 0);
}

// RESET must NOT lose a user-set time: the uPD1990AC on the ThunderClock+
// is battery-backed (on-board NiCd; the chip's time counter keeps ticking
// across power-off — uPD1990AC datasheet V_BATT operation, and MAME
// `upd1990a.cpp` device_reset never touches the time counter). Only the
// interrupt hardware and TP timer react to a bus RESET (manual 5-2).
// Regression: onReset used to zero userOffsetActive/userOffsetSeconds, so
// every Ctrl-Reset forgot a TIME_SET.
void testResetPreservesSetTime()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    // Clock in a set time of 1995-12-31 23:58:59 (same target byte layout
    // as testTimeSetRoundTrip) and commit via STB in MODE_TIME_SET.
    // 40-bit register: 5 bytes, no year (see testTimeSetRoundTrip).
    const uint8_t target[5] = { 0x59, 0x58, 0x23, 0x31, 0xC0 };
    constexpr uint8_t kModeShiftShifted   = 0x01 << 3;
    constexpr uint8_t kModeTimeSetShifted = 0x02 << 3;
    card->deviceSelectWrite(0, kModeShiftShifted);
    for (int k = 0; k < 40; ++k) {
        const uint8_t bit = (target[k / 8] >> (k % 8)) & 1;
        const uint8_t baseline =
            static_cast<uint8_t>(kModeShiftShifted | (bit ? kBitDataIn : 0));
        card->deviceSelectWrite(0, baseline);
        card->deviceSelectWrite(0, baseline | kBitClk);
        card->deviceSelectWrite(0, baseline);
    }
    card->deviceSelectWrite(0, kModeTimeSetShifted);
    card->deviceSelectWrite(0, kModeTimeSetShifted | kBitStb);
    card->deviceSelectWrite(0, kModeTimeSetShifted);

    // Bus RESET (Ctrl-Reset / profile switch).
    card->onReset();

    // Read back through the normal TIME_READ path — the set time survives.
    card->deviceSelectWrite(0, kModeTimeReadShifted);
    card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);
    const uint8_t baseline = kModeTimeReadShifted;
    card->deviceSelectWrite(0, baseline);
    const uint8_t sec = readByteLsbFirst(*card, baseline);
    const uint8_t min = readByteLsbFirst(*card, baseline);
    assert(sec == 0x59);
    assert(min == 0x58);
    // Skip hour (DST-flexible), then pin day + month — the set time
    // survived the bus RESET. No year byte on this chip (40-bit).
    (void)readByteLsbFirst(*card, baseline);    // hour
    assert(readByteLsbFirst(*card, baseline) == 0x31);          // day
    assert((readByteLsbFirst(*card, baseline) & 0xF0) == 0xC0); // month=12
}

// RESET disables interrupts and stops the TP timer (manual 5-2 point 2).
void testResetStopsTpAndIrq()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);
    armTpInterrupts(*card, 0x06);
    card->advanceCycles(kCpuHz / 2048 + 4);
    assert(card->slotIrqAsserted());

    card->onReset();
    assert(!card->interruptsEnabled());
    assert(!card->slotIrqAsserted());
    assert(card->tpRateHz() == 0);
    card->advanceCycles(kCpuHz);
    assert(!card->slotIrqAsserted());             // TP stopped → no new pulses
}

}  // namespace

// ── /IOSTB is claimed only with the 2 KB EPROM ────────────────────────────
// (bug hunt #9.) `takesC800()` answered true unconditionally, but the
// synthetic ROM and the 256-byte slot-ROM-only dump both leave the
// expansion window empty ($FF), so a dump-less ThunderClock in a lower slot
// latched $C800-$CFFF first-one-wins and starved the card that really
// serves it. The window claim must follow the EPROM.
void testC800ClaimFollowsTheEprom()
{
    // In the tree, with the shipped dump: the claim matches what the window
    // actually serves.
    {
        auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);
        bool serves = false;
        for (int i = 0; i < 0x800 && !serves; ++i)
            serves = card->expansionRomRead(static_cast<uint16_t>(i)) != 0xFF;
        assert(card->takesC800() == serves &&
               "the /IOSTB claim must match whether the window serves bytes");
    }
    // Without a dump: synthetic ROM, empty window, no claim.
    {
        auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42,
                                           /*probeDump=*/false);
        assert(!card->romFromDump());
        assert(card->expansionRomRead(0) == 0xFF);
        assert(!card->takesC800() &&
               "a ThunderClock with no EPROM must not latch $C800");
    }
    std::printf("  ok: the $C800 claim follows the EPROM\n");
}

int main()
{
    testC800ClaimFollowsTheEprom();
    testSignature();
    std::printf("ProDOS detection signature: OK\n");

    testTimeReadProtocol();
    std::printf("uPD1990AC time-read protocol round-trip: OK\n");

    testShiftDrainsToZero();
    std::printf("Shift register drains to zero: OK\n");

    testStbRelatchesTime();
    std::printf("STB rising-edge relatches time: OK\n");

    testDataOutMirrorOnAllDeviceSelectOffsets();
    std::printf("DATA_OUT mirrored across $C0n0-$C0nF: OK\n");

    testTimeSetRoundTrip();
    std::printf("MODE_TIME_SET round-trip (DATA_IN → shift → STB+SET → read): OK\n");

    testShiftLaxAcrossModes();
    std::printf("MODE_SHIFT lax: CLK shifts in any mode (ProDOS compat): OK\n");

    testTpRatesProduceExpectedPulseCounts();
    std::printf("TP rates 64/256/2048 Hz → IRQ pulse counts: OK\n");

    testModeSevenIsTestNotTp4096();
    std::printf("Mode code 7 is MODE_TEST on the parallel part: OK\n");

    testNoIrqWhenDisabled();
    std::printf("TP toggles but no IRQ while disabled: OK\n");

    testEnableLatchTracksBit6();
    std::printf("$C0n0 bit-6 enable latch (disable/re-arm): OK\n");

    testInterruptAssertedFlagBit5();
    std::printf("Read $C0n0 bit 5 interrupt-asserted flag + clear: OK\n");

    testResetPreservesSetTime();
    std::printf("RESET preserves battery-backed set time: OK\n");

    testResetStopsTpAndIrq();
    std::printf("RESET stops TP timer + disables interrupts: OK\n");

    std::printf("clock_card_smoke OK\n");
    return 0;
}
