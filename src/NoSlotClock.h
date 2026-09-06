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

#pragma once

// NoSlotClock — Dallas DS1216E "SmartWatch" emulation.
//
// The DS1216E is a 28-pin socket that physically sits under a ROM chip
// and intercepts reads to the underlying ROM. It is the canonical
// "clock without using a slot" solution for the Apple //c (which has
// no expansion slots) and for any //e/II+ owner short on slot count.
//
// Protocol (verified against AppleWin's `NoSlotClock.cpp`, which is
// itself derived from Nick Westgate's csa2 post + the Dallas DS1216
// datasheet):
//
//   The host CPU drives the chip via reads to the protected address
//   range ($F800-$FFFF when sat under the Monitor ROM). Address bit
//   A2 selects the operation:
//
//     A2 = 0  → "write" cycle: feed the next 1-bit of the magic key.
//               The bit value comes from A0 (so $F800 sends 0,
//               $F801 sends 1, $F802 sends 0, $F803 sends 1, etc.).
//               64 consecutive matches transition the chip into
//               clock-readout phase.
//     A2 = 1  → "read" cycle: emit the next clock-register bit on D0.
//               During pattern-matching this also RESETS the matcher
//               (host reverting to readout from a half-walked key).
//
//   Mismatch behaviour: a single wrong A0 bit DISABLES further
//   writes — the matcher stays "dead" until the host issues an A2=1
//   read (or until system reset clears state). This is the Dallas
//   datasheet pattern-recognition spec: a bad pattern is sticky.
//
// Where the chip is probed depends on the machine, because it hides
// under whichever ROM the era's drivers walk:
//
//   II / II+        $F800-$FFFF (Monitor ROM), only while the LC maps
//                   ROM — they have no internal slot-3/8 ROM to sit
//                   under. Matches AppleWin's `!SW_HIGHRAM &&
//                   !SW_WRITERAM` gate.
//   //e, //c-class  $C300-$C3FF and $C800-$C8FF, inside the
//                   INTCXROM / SLOTC3ROM branches — this is where
//                   ProDOS 8 ≥ 2.0.3 and GS/OS actually step the magic
//                   key (AppleWin `IsPotentialNoSlotClockAccess`).
//
// POM2 hooks this class into `Memory::memRead` / `memWrite` for those
// windows when `nsclock_enable` is true (default on; the pass-through
// path is a no-op for software that doesn't trigger the magic key).
// Detail → DEV.md § No-Slot Clock.

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <vector>

namespace pom2 {

class NoSlotClock
{
public:
    /// Time source. Defaults to `std::time + localtime`; tests swap a
    /// deterministic source via the second constructor.
    using TimeFn = std::tm (*)();

    NoSlotClock();
    explicit NoSlotClock(TimeFn fn);

    void setEnabled(bool on)        { enabled_ = on; }
    bool isEnabled() const          { return enabled_; }

    /// Called by `Memory::memRead` for every read in the watched range
    /// (`$F800-$FFFF` by default). `romByte` is what the ROM/RAM
    /// underneath would normally return. The intercept returns the
    /// byte the CPU actually sees: pass-through except during clock-
    /// readout phase, where D0 carries the next clock bit.
    uint8_t interceptRead(uint16_t addr, uint8_t romByte);

    /// WRITE cycles drive the SAME DS1216E state machine — the key bit
    /// rides on A0 of the ADDRESS, R/W is irrelevant to the matcher
    /// (AppleWin `CNoSlotClock::Write(address)` calls the identical
    /// ClockRead/ClockWrite pair). Some NSC drivers feed the 64-bit key
    /// with STA; with only the read hook they never unlocked the clock.
    void interceptWrite(uint16_t addr)
    {
        uint8_t discard = 0xFF;
        (void)interceptRead(addr, discard);
    }

    /// State accessor for UI / tests.
    enum class Phase { Idle, MatchingKey, ReadingClock };
    Phase   phase() const;
    int     keyBitsMatched() const  { return bitsMatched_; }
    int     clockBitsRead()  const  { return bitsRead_;    }

    /// Force a hardware-style reset: re-enables pattern matching and
    /// clears the matcher state. Not auto-called on CPU softReset
    /// because the DS1216E is battery-backed on real hardware and
    /// keeps state across resets.
    void    onReset();

    /// AppleWin-parity magic key — `CNoSlotClock::kClockInitSequence`
    /// in `AppleWin/source/NoSlotClock.h`. The 64 bits are LSB-first
    /// transmission of the Dallas DS1216E datasheet's 8-byte canonical
    /// sequence  C5 3A A3 5C C5 3A A3 5C  packed little-endian.
    static constexpr uint64_t kMagicKey = 0x5CA33AC55CA33AC5ULL;

    /// Snapshot / rewind. The chip is a bit-serial state machine walked
    /// across MANY CPU reads: a driver is typically 40-odd bits into the
    /// 64-bit magic key, or halfway through shifting a BCD date out, when
    /// a rewind frame is captured. Restoring RAM + PC without the matcher
    /// cursor resumed the driver mid-sequence against a chip that had
    /// silently moved (or reset) — the read-back date came out garbage and,
    /// because a mismatched bit is sticky, the clock then stayed dead for
    /// the session. `enabled_` is a USER setting and is deliberately not
    /// carried. Self-describing (magic + version); an unrecognised or
    /// truncated blob leaves the live chip untouched and returns false.
    void appendSnapshotState(std::vector<uint8_t>& out) const;
    bool loadSnapshotState(const uint8_t* data, std::size_t len);

private:
    void    loadClockSnapshot();
    static std::tm defaultTimeFn();

    TimeFn   timeFn_       = nullptr;
    bool     enabled_      = true;
    bool     writeEnabled_ = true;     // false after a mismatched key bit
                                       //   (sticky; cleared by A2=1 read)
    bool     readingClock_ = false;    // 64-bit clock-out phase active
    uint8_t  bitsMatched_  = 0;        // 0..63 (key-feed progress)
    uint8_t  bitsRead_     = 0;        // 0..63 (clock-out progress)
    uint64_t clockShift_   = 0;        // shifted out LSB-first to D0
};

}  // namespace pom2
