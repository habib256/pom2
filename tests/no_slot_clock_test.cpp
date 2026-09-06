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

// NoSlotClock — Dallas DS1216E SmartWatch smoke test.
//
// Drives `interceptRead` directly with the AppleWin-faithful protocol:
//   A2 = 0 read → "write" cycle, A0 carries the next magic-key bit
//   A2 = 1 read → "read" cycle, D0 carries the next clock-register bit
//
// Pins:
//   1. Pass-through is invisible when no host code touches the chip.
//   2. Walking the 64-bit magic key via A2=0 reads transitions to
//      clock-readout phase (pass-through D0 unaffected during the walk).
//   3. Reading from A2=1 addresses after a complete walk returns the
//      64 clock bits on D0, with D1..D7 untouched. The chip falls back
//      to pass-through after 64 readout bits.
//   4. A mismatched A0 bit during pattern matching makes the matcher
//      "sticky-dead" until an A2=1 read clears it (Dallas spec).
//   5. The packed BCD time round-trips through the readout to the
//      same fields the fake time source produced.
//   6. A stray A2=0 access DURING readout consumes exactly one clock bit
//      (AppleWin's ClockWrite advances the same ring position ClockRead
//      reads from), so an interleaved sequence still delivers the register
//      in order rather than stalling on one bit and losing the last.

#include "NoSlotClock.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {

constexpr uint64_t kMagicKey = pom2::NoSlotClock::kMagicKey;

// Send one magic-key bit to the chip via an A2=0 read.
uint8_t feedKeyBit(pom2::NoSlotClock& nsc, uint8_t bit, uint8_t romFill = 0xEA)
{
    const uint16_t addr = static_cast<uint16_t>(
        0xF800 | (bit ? 0x01 : 0x00));   // A2=0, A0=bit
    return nsc.interceptRead(addr, romFill);
}

// Read one clock-register bit from the chip via an A2=1 read.
uint8_t readClockBit(pom2::NoSlotClock& nsc, uint8_t romFill = 0xEA)
{
    return nsc.interceptRead(0xF804, romFill);   // A2=1
}

// Walk the entire 64-bit magic key, asserting every read is pass-through
// (D0 of the ROM byte must remain unchanged during pattern matching).
void walkMagicKey(pom2::NoSlotClock& nsc, uint8_t romFill = 0xEA)
{
    for (int i = 0; i < 64; ++i) {
        const uint8_t bit = static_cast<uint8_t>((kMagicKey >> i) & 1);
        const uint8_t out = feedKeyBit(nsc, bit, romFill);
        if (out != romFill) {
            std::fprintf(stderr,
                "walkMagicKey: bit %d returned 0x%02X (want 0x%02X)\n",
                i, out, romFill);
            std::abort();
        }
    }
}

// Shift the 64-bit clock register out via 64 A2=1 reads. Reconstructs
// the register LSB-first into a u64.
uint64_t readClockRegister(pom2::NoSlotClock& nsc, uint8_t romFill = 0xEA)
{
    uint64_t out = 0;
    for (int i = 0; i < 64; ++i) {
        const uint8_t r = readClockBit(nsc, romFill);
        if ((r & 0xFE) != (romFill & 0xFE)) {
            std::fprintf(stderr,
                "readClockRegister: bit %d preserved-bits mismatch "
                "(got 0x%02X, want top 7 bits of 0x%02X)\n",
                i, r, romFill);
            std::abort();
        }
        out |= static_cast<uint64_t>(r & 1) << i;
    }
    return out;
}

// ─── Test 1: pass-through is invisible ────────────────────────────────
void testPassThrough()
{
    pom2::NoSlotClock nsc;
    // 200 random reads — random mix of A2 values, both should leave
    // the ROM byte untouched in pass-through mode.
    for (int i = 0; i < 200; ++i) {
        const uint16_t addr = static_cast<uint16_t>(0xF800 + (i & 0x3F));
        const uint8_t r = nsc.interceptRead(addr, 0xA5);
        if (r != 0xA5) {
            std::fprintf(stderr,
                "passThrough: read %d (addr=0x%04X) = 0x%02X (want 0xA5)\n",
                i, addr, r);
            std::abort();
        }
    }
    std::printf("  ok: pass-through never modifies D0 outside readout\n");
}

// ─── Test 2: disabled chip is a literal pass-through ──────────────────
void testDisabled()
{
    pom2::NoSlotClock nsc;
    nsc.setEnabled(false);
    walkMagicKey(nsc, 0xBB);
    // Even a completed pattern walk on a disabled chip must not unlock
    // the clock — the next 64 A2=1 reads still return ROM verbatim.
    for (int i = 0; i < 64; ++i) {
        const uint8_t r = readClockBit(nsc, 0xBB);
        if (r != 0xBB) {
            std::fprintf(stderr,
                "disabled: read %d after key walk = 0x%02X (want 0xBB)\n",
                i, r);
            std::abort();
        }
    }
    std::printf("  ok: disabled chip ignores the magic key\n");
}

// ─── Test 3: full key walk → clock readout ────────────────────────────
// Fake time source: 2026-05-27 21:30:45, Wednesday (tm_wday=3 → dow=4).
std::tm fixedTime()
{
    std::tm tm{};
    tm.tm_sec  = 45;
    tm.tm_min  = 30;
    tm.tm_hour = 21;
    tm.tm_mday = 27;
    tm.tm_mon  = 4;     // May (0-based)
    tm.tm_year = 126;   // 2026 - 1900
    tm.tm_wday = 3;     // Wednesday (0=Sun) → dow=4 (1=Sun convention)
    return tm;
}

void testKeyWalkThenReadout()
{
    pom2::NoSlotClock nsc(&fixedTime);
    walkMagicKey(nsc);
    // Now in clock-readout phase. Pull 64 bits via A2=1 reads.
    const uint64_t reg = readClockRegister(nsc);

    auto bcd = [](int v) -> uint8_t {
        return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
    };

    const uint8_t hundredths = static_cast<uint8_t>((reg >>  0) & 0xFF);
    const uint8_t seconds    = static_cast<uint8_t>((reg >>  8) & 0xFF);
    const uint8_t minutes    = static_cast<uint8_t>((reg >> 16) & 0xFF);
    const uint8_t hours      = static_cast<uint8_t>((reg >> 24) & 0xFF);
    const uint8_t dow        = static_cast<uint8_t>((reg >> 32) & 0xFF);
    const uint8_t day        = static_cast<uint8_t>((reg >> 40) & 0xFF);
    const uint8_t month      = static_cast<uint8_t>((reg >> 48) & 0xFF);
    const uint8_t year       = static_cast<uint8_t>((reg >> 56) & 0xFF);

    auto check = [](const char* what, uint8_t got, uint8_t want) {
        if (got != want) {
            std::fprintf(stderr, "clock %s: got 0x%02X want 0x%02X\n",
                what, got, want);
            std::abort();
        }
    };
    check("0.01s", hundredths, 0x00);
    check("sec",   seconds,    bcd(45));
    check("min",   minutes,    bcd(30));
    check("hour",  hours,      bcd(21));
    check("dow",   dow,        bcd(4));   // tm_wday(3)+1 = 4
    check("day",   day,        bcd(27));
    check("month", month,      bcd(5));
    check("year",  year,       bcd(26));

    // After 64 readout bits the chip is back in pass-through; an
    // A2=1 read at this point returns the raw ROM byte.
    const uint8_t after = readClockBit(nsc, 0x99);
    if (after != 0x99) {
        std::fprintf(stderr, "post-readout: got 0x%02X want 0x99\n", after);
        std::abort();
    }
    std::printf("  ok: 64-bit RTC round-trips through D0 readout\n");
}

// ─── Test 4: mismatched bit wedges the matcher; A2=1 read clears ──────
void testStickyMismatch()
{
    pom2::NoSlotClock nsc(&fixedTime);
    // Walk 20 correct bits.
    for (int i = 0; i < 20; ++i) {
        const uint8_t b = static_cast<uint8_t>((kMagicKey >> i) & 1);
        feedKeyBit(nsc, b);
    }
    assert(nsc.keyBitsMatched() == 20);

    // Feed a wrong A0 — chip should sticky-disable writes.
    const uint8_t wrong = static_cast<uint8_t>(
        ((kMagicKey >> 20) & 1) ^ 1);
    feedKeyBit(nsc, wrong);

    // Subsequent A2=0 reads must be no-ops (write disabled). Walking
    // even a correct full sequence here does NOT unlock the clock.
    walkMagicKey(nsc);
    if (nsc.phase() == pom2::NoSlotClock::Phase::ReadingClock) {
        std::fprintf(stderr,
            "stickyMismatch: chip unlocked despite write-disabled state\n");
        std::abort();
    }

    // An A2=1 read clears the sticky disable. The next full walk
    // unlocks the clock as normal.
    (void)readClockBit(nsc);            // clears write-disable
    walkMagicKey(nsc);
    const uint64_t reg = readClockRegister(nsc);
    if ((reg & 0xFF) != 0x00 || ((reg >> 8) & 0xFF) != 0x45) {
        std::fprintf(stderr, "stickyMismatch: post-recovery readout "
                     "garbled (reg=0x%016llX)\n",
                     (unsigned long long)reg);
        std::abort();
    }
    std::printf("  ok: A2=1 read clears sticky-disable, recovery works\n");
}

// ─── Test 5: an A2=0 access during readout consumes a bit ─────────────
// AppleWin's ClockWrite, when the clock register is already enabled, is
//     else if (m_ClockRegister.NextBit()) m_bClockRegisterEnabled = false;
// and NextBit advances the SAME ring position ClockRead's ReadBit reads
// from — so a stray A2=0 access mid-readout consumes a bit, it does not
// merely count one. POM2 bumped its bit counter without shifting the
// register, which desynced the two: the next A2=1 read returned the bit the
// stray access had already burned, everything after came out one position
// late, and the 64th bit was lost when the counter ran out. Interleave one
// A2=0 access after every 8 bits and check the remaining bits are the
// clock's, still in order.
void testInterleavedWriteDuringReadout()
{
    pom2::NoSlotClock nsc(&fixedTime);
    walkMagicKey(nsc);

    auto bcd = [](int v) -> uint8_t {
        return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
    };
    // The register as the undisturbed readout would deliver it.
    const uint64_t want =
        (uint64_t)0x00           <<  0 |   // hundredths
        (uint64_t)bcd(45)        <<  8 |
        (uint64_t)bcd(30)        << 16 |
        (uint64_t)bcd(21)        << 24 |
        (uint64_t)bcd(4)         << 32 |
        (uint64_t)bcd(27)        << 40 |
        (uint64_t)bcd(5)         << 48 |
        (uint64_t)bcd(26)        << 56;

    int pos = 0;                       // ring position consumed so far
    while (pos < 64) {
        const uint8_t got = readClockBit(nsc);
        const uint8_t bit = static_cast<uint8_t>((want >> pos) & 1);
        if ((got & 1) != bit) {
            std::fprintf(stderr,
                "interleaved: bit %d = %u, want %u\n", pos, got & 1, bit);
            std::abort();
        }
        ++pos;
        // Every 8 bits, a stray A2=0 access: it must eat exactly one bit
        // of the register, so the NEXT A2=1 read continues at pos+1.
        if (pos % 8 == 0 && pos < 64) {
            (void)feedKeyBit(nsc, 0);
            ++pos;
        }
    }
    // 64 positions consumed → back to pass-through.
    if (nsc.phase() == pom2::NoSlotClock::Phase::ReadingClock) {
        std::fprintf(stderr, "interleaved: still in readout after 64 bits\n");
        std::abort();
    }
    const uint8_t after = readClockBit(nsc, 0x99);
    if (after != 0x99) {
        std::fprintf(stderr, "interleaved: post-readout got 0x%02X want 0x99\n",
                     after);
        std::abort();
    }
    std::printf("  ok: A2=0 access mid-readout consumes one clock bit\n");
}

}  // namespace

int main()
{
    std::printf("NoSlotClock smoke test\n");
    testPassThrough();
    testDisabled();
    testKeyWalkThenReadout();
    testStickyMismatch();
    testInterleavedWriteDuringReadout();
    std::printf("PASS\n");
    return 0;
}
