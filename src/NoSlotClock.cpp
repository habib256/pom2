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

#include "NoSlotClock.h"

#include "ByteIO.h"

#include <algorithm>
#include <ctime>

namespace pom2 {

std::tm NoSlotClock::defaultTimeFn()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

NoSlotClock::NoSlotClock() : timeFn_(&NoSlotClock::defaultTimeFn) {}

NoSlotClock::NoSlotClock(TimeFn fn)
    : timeFn_(fn ? fn : &NoSlotClock::defaultTimeFn) {}

void NoSlotClock::onReset()
{
    // SmartWatch hardware reset (per AppleWin's CNoSlotClock::Reset): the
    // comparison register restarts at the top of the magic key, writes
    // are re-enabled, and any half-shifted clock readout is dropped.
    writeEnabled_ = true;
    readingClock_ = false;
    bitsMatched_  = 0;
    bitsRead_     = 0;
}

NoSlotClock::Phase NoSlotClock::phase() const
{
    if (!enabled_)        return Phase::Idle;
    if (readingClock_)    return Phase::ReadingClock;
    if (bitsMatched_ > 0) return Phase::MatchingKey;
    return Phase::Idle;
}

void NoSlotClock::loadClockSnapshot()
{
    const std::tm tm = timeFn_();
    auto packBcd = [](int lsb, int msb) -> uint8_t {
        // Clamped, not asserted: a leap-second `tm_sec == 60` is a legal
        // value the C library really does hand out.
        lsb = std::clamp(lsb, 0, 9);
        msb = std::clamp(msb, 0, 9);
        // The chip shifts out LSB-first per byte (D0 of byte 0 first),
        // so we pack the low BCD nibble in bits 0-3 and the high BCD
        // nibble in bits 4-7. Mirrors AppleWin's WriteNibble(low) then
        // WriteNibble(high) sequence per field.
        return static_cast<uint8_t>((msb << 4) | lsb);
    };

    // Field-by-field, per `CNoSlotClock::PopulateClockRegister`:
    //   byte 0 : centi-seconds   (BCD, 00-99)
    //   byte 1 : seconds         (BCD, 00-59)
    //   byte 2 : minutes         (BCD, 00-59)
    //   byte 3 : hours           (BCD, 00-23 in 24-hour mode)
    //   byte 4 : day-of-week     (1-7; AppleWin convention 1=Sunday)
    //   byte 5 : date            (BCD, 01-31)
    //   byte 6 : month           (BCD, 01-12)
    //   byte 7 : year            (BCD, 00-99)
    const int centisecond = 0;   // POSIX std::tm has no sub-second field
    const int sec = tm.tm_sec;
    const int min = tm.tm_min;
    const int hr  = tm.tm_hour;
    const int dow = tm.tm_wday + 1;            // 1 = Sunday (AppleWin)
    const int dom = tm.tm_mday;
    const int mon = tm.tm_mon + 1;             // 1-12
    const int yr  = tm.tm_year % 100;          // 00-99

    uint64_t r = 0;
    r |= static_cast<uint64_t>(packBcd(centisecond % 10, centisecond / 10)) <<  0;
    r |= static_cast<uint64_t>(packBcd(sec % 10, sec / 10))                 <<  8;
    r |= static_cast<uint64_t>(packBcd(min % 10, min / 10))                 << 16;
    r |= static_cast<uint64_t>(packBcd(hr  % 10, hr  / 10))                 << 24;
    r |= static_cast<uint64_t>(packBcd(dow % 10, dow / 10))                 << 32;
    r |= static_cast<uint64_t>(packBcd(dom % 10, dom / 10))                 << 40;
    r |= static_cast<uint64_t>(packBcd(mon % 10, mon / 10))                 << 48;
    r |= static_cast<uint64_t>(packBcd(yr  % 10, yr  / 10))                 << 56;
    clockShift_ = r;
    bitsRead_   = 0;
}

uint8_t NoSlotClock::interceptRead(uint16_t addr, uint8_t romByte)
{
    if (!enabled_) return romByte;

    // The DS1216E's behaviour pivots on A2 of the read address.
    // AppleWin's `CNoSlotClock::Read`:
    //   if (address & 0x04) → ClockRead   (D0 returns next clock bit)
    //   else                → ClockWrite  (A0 feeds next key bit)
    const bool isClockRead = (addr & 0x04) != 0;

    if (isClockRead) {
        // ── Read cycle (A2 = 1) ──────────────────────────────────────
        if (!readingClock_) {
            // Datasheet: an A2=1 read while the matcher is still in
            // pattern-recognition phase RESETS the matcher and re-
            // enables writes. (This is how a misfired sequence
            // recovers without a system reset.)
            writeEnabled_ = true;
            bitsMatched_  = 0;
            return romByte;
        }
        // Clock-readout phase: shift one bit onto D0; D1..D7 unchanged
        // (real chip tri-states D1..D7, ROM drives them).
        const uint8_t bit = static_cast<uint8_t>(clockShift_ & 1);
        clockShift_ >>= 1;
        ++bitsRead_;
        if (bitsRead_ >= 64) {
            readingClock_ = false;
            bitsRead_     = 0;
            // AppleWin behaviour: exiting readout does NOT auto-restart
            // pattern matching; the host has to re-issue 64 key bits.
            writeEnabled_ = true;
            bitsMatched_  = 0;
        }
        return static_cast<uint8_t>((romByte & 0xFE) | bit);
    }

    // ── Write cycle (A2 = 0): pattern-match phase ────────────────────
    // After a single bad bit the chip is "wedged" — writes are sticky-
    // disabled until an A2=1 read clears them. Pass-through still
    // returns the ROM byte verbatim; the chip is just no longer
    // listening to A0.
    if (!writeEnabled_) return romByte;

    if (readingClock_) {
        // AppleWin: during clock-readout, even A2=0 writes count
        // against the 64-bit shift (the host is expected to read
        // from A2=1 addresses, but any read in the range progresses
        // the shift register). Simulate that progression so a
        // misbehaving caller doesn't desync the chip.
        //
        //   void CNoSlotClock::ClockWrite(int address) {
        //       ...
        //       else if (m_ClockRegister.NextBit())
        //           m_bClockRegisterEnabled = false;
        //   }
        //
        // NextBit advances the SHARED ring position that ClockRead's
        // ReadBit reads from, so the bit is consumed, not merely
        // counted. Shifting the counter without shifting the register
        // (what this did) desynced the two: the next A2=1 read handed
        // back the bit this access had already burned, every following
        // bit came out one position late, and the last one was lost when
        // the counter hit 64 — a driver that interleaves a stray write
        // into its read loop got a garbled date rather than a shifted
        // one.
        clockShift_ >>= 1;
        ++bitsRead_;
        if (bitsRead_ >= 64) {
            readingClock_ = false;
            bitsRead_     = 0;
            writeEnabled_ = true;
            bitsMatched_  = 0;
        }
        return romByte;
    }

    const uint8_t got      = static_cast<uint8_t>(addr & 1);
    const uint8_t expected = static_cast<uint8_t>((kMagicKey >> bitsMatched_) & 1);
    if (got == expected) {
        ++bitsMatched_;
        if (bitsMatched_ >= 64) {
            // 64 key bits matched — latch a fresh time snapshot and
            // enter clock-readout phase.
            loadClockSnapshot();
            readingClock_ = true;
        }
    } else {
        // Sticky mismatch — disable further writes per Dallas spec.
        writeEnabled_ = false;
    }
    return romByte;
}

// ─── Snapshot / rewind ──────────────────────────────────────────────────
// See the header for why the bit cursors have to travel. Fixed 15-byte
// layout, self-identifying, tolerant of a foreign or truncated blob.

namespace {
constexpr uint32_t kNscSnapMagic   = 0x43534E32u;   // '2NSC'
constexpr uint16_t kNscSnapVersion = 1;
}  // namespace

void NoSlotClock::appendSnapshotState(std::vector<uint8_t>& out) const
{
    byteio::putU32(out, kNscSnapMagic);
    byteio::putU16(out, kNscSnapVersion);
    out.push_back(writeEnabled_ ? 1 : 0);
    out.push_back(readingClock_ ? 1 : 0);
    out.push_back(bitsMatched_);
    out.push_back(bitsRead_);
    byteio::putU64(out, clockShift_);
}

bool NoSlotClock::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    byteio::Reader r(data, len);
    if (!r.has(4 + 2 + 4 + 8)) return false;
    if (r.u32() != kNscSnapMagic)   return false;
    if (r.u16() != kNscSnapVersion) return false;

    writeEnabled_ = r.u8() != 0;
    readingClock_ = r.u8() != 0;
    // Clamp: both cursors index a 64-bit shifter and are compared with
    // `>= 64` only AFTER an increment, so a crafted 200 would run the
    // matcher off the end of the key for good.
    bitsMatched_  = static_cast<uint8_t>(r.u8() & 63);
    bitsRead_     = static_cast<uint8_t>(r.u8() & 63);
    clockShift_   = r.u64();
    return true;
}

}  // namespace pom2
