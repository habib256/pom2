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

// a2heaven's FASTChip //e — a 65C02 running from its own SRAM at up to
// 16.6 MHz, cabled into the 6502 socket from any slot. To the machine it is
// what a Zip Chip is, a CPU-socket accelerator (CpuSocketAccelerator.h);
// it differs in its registers, which sit at $C06A-$C06F.
//
// From the FASTChip IIe User's Guide v0.06 (a2heaven.com, 18 July 2017),
// "Programmer's Reference", pp. 21-24:
//
//   $C06A  W  $6A four times in a row unlocks; $A6 locks. "While unlocked,
//             any write other than $A6 or $6A will initiate an indefinite
//             synchronous sequence" — not modelled: it locks, as izapple2
//             does (cardFastChip.go).
//   $C06B  W  any byte enables the registers (once unlocked)
//          R  bit 7 = the FASTChip is there and enabled
//   $C06C  RW slot speeds, bit n = slot n fast (1) or normal (0)
//   $C06D  RW the speed: 0 = off (the motherboard's 6502), 1-40 = 0.20 to
//             16.6 MHz (the table on p. 22; 9 = 1.00 MHz)
//   $C06E  RW configuration register index; $C06F RW its data:
//             0 speed, 1 slot speeds, 2 speaker delay (NONE FAST NORMAL
//             MUSIC HIFI), 3 joystick delay (NONE SHORT LONG), 4-7 the
//             RamFactor / RamWorks emulations and the backlight (kept, inert)
//
// The usual sequence (the manual's ENABLE / SET_SPEED, 4cade's
// hw.accel.a): unlock, write $C06B, write $C06D, lock with $A6. The speed
// survives the lock; the registers answer only while enabled.
//
// Slow-downs: an access to a slot marked normal, or to the speaker / the
// paddles while their delay is not NONE, runs the next stretch at 1 MHz.
// The manual names the delay settings but gives no lengths, so POM2 uses
// the Zip Chip's (MAME apple2e.cpp: 52 ms after a slot access, 5 ms after a
// speaker or paddle one) for every setting other than NONE.

#ifndef POM2_FAST_CHIP_H
#define POM2_FAST_CHIP_H

#include "CpuClock.h"
#include "CpuSocketAccelerator.h"

#include <array>

namespace pom2 {

class FastChip final : public CpuSocketAccelerator
{
public:
    static constexpr uint8_t kSpeedOff   = 0;
    static constexpr uint8_t kSpeed1MHz  = 9;
    static constexpr uint8_t kSpeedMax   = 40;    // 16.6 MHz
    static constexpr int     kSlotDelayMs  = 52;  // the Zip Chip's (see above)
    static constexpr int     kShortDelayMs = 5;

    /// MHz for a speed register value (0 = off: the stock clock).
    static double speedMHz(uint8_t value);

    ReadResult ioRead(uint8_t low, uint64_t now, uint8_t stateByte) override;
    void   ioWrite(uint8_t low, uint8_t value, uint64_t now) override;
    double cpuSpeedMultiplier(uint64_t now) const override;
    uint64_t stockCyclesBetween(uint64_t from, uint64_t to) const override;
    void   onReset(uint64_t now) override;
    void   appendSnapshotState(std::vector<uint8_t>& out) const override;
    bool   validateSnapshotState(const uint8_t* data, std::size_t len) const override;
    bool   loadSnapshotState(const uint8_t* data, std::size_t len) override;

    void setStockClockHz(double hz) { if (hz > 0) stockHz_ = hz; }
    /// The speed saved in the chip's configuration — what it starts at.
    void setPowerOnSpeed(uint8_t v) { powerOnSpeed_ = v > kSpeedMax ? kSpeedMax : v; }
    uint8_t powerOnSpeed() const { return powerOnSpeed_; }

    // Diagnostics / tests.
    bool    unlocked() const { return unlocked_; }
    bool    enabled() const { return enabled_; }
    uint8_t speed() const { return config_[0]; }
    uint8_t slotMask() const { return config_[1]; }
    bool    delayActive(uint64_t now) const { return now < slowUntil_; }

private:
    void slowFor(int ms, uint64_t now);
    double multiplier() const;

    double   stockHz_ = static_cast<double>(POM2_CPU_CLOCK_HZ);
    uint8_t  powerOnSpeed_ = kSpeedMax;
    bool     unlocked_ = false;
    int      unlockCount_ = 0;
    bool     enabled_ = false;
    uint8_t  index_ = 0;                    // $C06E
    std::array<uint8_t, 8> config_{};       // [0] speed, [1] slot mask, [2] speaker, [3] joystick, …
    uint64_t slowUntil_ = 0;
};

} // namespace pom2

#endif // POM2_FAST_CHIP_H
