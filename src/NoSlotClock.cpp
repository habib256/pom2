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

namespace {
std::tm addSeconds(std::tm tm, int64_t seconds)
{
    if (seconds == 0) return tm;
    tm.tm_isdst = -1;
    std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) return tm;
    t += static_cast<std::time_t>(seconds);
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}
int fromBcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
}  // namespace

// The 64th bit of a data transfer: a transfer that wrote anything sets the
// clock — kept as an offset from host time, as MAME's ds1215 `set_time`.
// Reserved bits read 0 (data sheet register map): seconds/minutes bit 7,
// hours bits 7-6 (24-hour mode), date bits 7-6, month bits 7-5.
void NoSlotClock::endTransfer()
{
    if (written_) {
        auto byte = [&](int n) { return static_cast<uint8_t>(writeShift_ >> (8 * n)); };
        std::tm set{};
        set.tm_sec  = fromBcd(byte(1) & 0x7F);
        set.tm_min  = fromBcd(byte(2) & 0x7F);
        set.tm_hour = fromBcd(byte(3) & 0x3F);
        set.tm_mday = fromBcd(byte(5) & 0x3F);
        set.tm_mon  = fromBcd(byte(6) & 0x1F) - 1;
        const std::tm now = timeFn_();
        set.tm_year = (now.tm_year / 100) * 100 + fromBcd(byte(7));   // same century
        set.tm_isdst = -1;
        std::tm host = now; host.tm_isdst = -1;
        const std::time_t a = std::mktime(&set), b = std::mktime(&host);
        if (a != static_cast<std::time_t>(-1) && b != static_cast<std::time_t>(-1))
            offsetSec_ = static_cast<int64_t>(a) - static_cast<int64_t>(b);
    }
    written_      = false;
    readingClock_ = false;
    bitsRead_     = 0;
    // AppleWin: exiting the transfer does NOT auto-restart pattern
    // matching; the host has to re-issue 64 key bits.
    writeEnabled_ = true;
    bitsMatched_  = 0;
}

void NoSlotClock::loadClockSnapshot()
{
    const std::tm tm = addSeconds(timeFn_(), offsetSec_);
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
    writeShift_ = r;
    written_    = false;
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
            endTransfer();
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
        // ...and it WRITES: A0 is the data bit, into the register bit this
        // cycle transfers (DS1216 data sheet; MAME ds1216e). AppleWin drops
        // it — so a SET-TIME utility's clock reverted to host time.
        const uint64_t mask = uint64_t(1) << bitsRead_;
        writeShift_ = (addr & 1) ? (writeShift_ | mask) : (writeShift_ & ~mask);
        written_ = true;
        clockShift_ >>= 1;
        ++bitsRead_;
        if (bitsRead_ >= 64) endTransfer();
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
constexpr uint16_t kNscSnapVersion = 2;   // v2: + the set-time offset and transfer
/// Highest version this reader understands. TOLERANT on purpose — see the
/// same constant in `Sony35Drive.cpp`: a strict equality test turns the next
/// format bump into "every .pom2snap the shipped build wrote is unloadable".
constexpr uint16_t kNscSnapVersionMax = kNscSnapVersion;
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
    byteio::putU64(out, writeShift_);                       // v2
    out.push_back(written_ ? 1 : 0);
    byteio::putU64(out, static_cast<uint64_t>(offsetSec_));
}

bool NoSlotClock::loadSnapshotState(const uint8_t* data, std::size_t len, bool commit)
{
    if (!data) return false;
    byteio::Reader r(data, len);
    if (!r.has(4 + 2 + 4 + 8)) return false;
    if (r.u32() != kNscSnapMagic) return false;
    const uint16_t version = r.u16();
    if (version == 0 || version > kNscSnapVersionMax) return false;
    // v2 carries the write path (shifter + written flag + offset): a v2 blob
    // without it is truncated, not a v1 one, and must be refused whole.
    if (!r.has(4 + 8 + (version >= 2 ? 8 + 1 + 8 : 0))) return false;
    if (!commit) return true;

    writeEnabled_ = r.u8() != 0;
    readingClock_ = r.u8() != 0;
    // Clamp, not mask: both cursors index a 64-bit shifter and are compared
    // with `>= 64` only AFTER an increment, so a crafted 200 would run the
    // matcher off the end of the key for good. `& 63` did bound them, but it
    // also REWROTE the one legal value it could not represent: `bitsMatched_`
    // rests at exactly 64 for the whole clock-readout phase (it is not zeroed
    // when the key completes), and masking sent that state back to 0 — a
    // rewind taken mid-readout came back with the matcher at the top of the
    // key. `min` keeps 64 and still rejects everything above it.
    // The two cursors have DIFFERENT resting ranges, which is why one mask
    // could not serve both: `bitsMatched_` legitimately sits at 64 for the
    // whole readout, `bitsRead_` is zeroed the instant it reaches 64 and so
    // never rests above 63.
    bitsMatched_  = static_cast<uint8_t>(std::min<int>(r.u8(), 64));
    bitsRead_     = static_cast<uint8_t>(std::min<int>(r.u8(), 63));
    // `kMagicKey >> bitsMatched_` is undefined at 64, and the matcher arm
    // only runs while `readingClock_` is false — a pairing every state this
    // chip can reach honours (64 matched ⇒ readout). A blob that says
    // otherwise is inconsistent, so restart the matcher rather than shift.
    if (!readingClock_ && bitsMatched_ >= 64) bitsMatched_ = 0;
    clockShift_   = r.u64();
    if (version >= 2) {
        writeShift_ = r.u64();
        written_    = r.u8() != 0;
        offsetSec_  = static_cast<int64_t>(r.u64());
    } else {
        writeShift_ = clockShift_; written_ = false; offsetSec_ = 0;
    }
    return true;
}

}  // namespace pom2
