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

// SlotBusClock — converts elapsed CPU cycles into ticks of the SLOT BUS
// clock (phase 0), for a card whose chips hang off that line rather than
// off the CPU.
//
// Why it exists (bug hunt 2026-09-29, round three). The Mockingboard's and
// the Phasor's 6522s are clocked by the slot's phase-0 line, and so are
// their AYs' pin-22 CLOCK. An accelerator (TransWarp, the //c+'s on-board
// 4x) speeds the 6502 up; it does not speed phase 0 up — MAME keeps both
// chips on the fixed a2bus clock whatever the CPU does. POM2 handed those
// cards the ACCELERATED clock through `setCpuClock` and ticked the VIAs
// once per CPU cycle, so under a TransWarp every AY note came out 3.5x
// sharp and every VIA-T1-paced tune played 3.5x fast.
//
// The card keeps the CPU clock for what IS measured in CPU cycles (its
// emuCycles replay cursor) and asks this divider how many bus ticks a
// span of CPU cycles is worth. The bus clock comes from
// `SlotPeripheral::setStandardClock`; until that is called the divider
// follows the CPU clock (ratio 1), which is the pre-fix behaviour and what
// a hand-built card in a test expects.
//
// Not thread-safe: the owner guards it with its own card mutex.

#pragma once

#include "CpuClock.h"

#include <cstdint>

namespace pom2 {

class SlotBusClock
{
public:
    void setCpuClock(double hz)
    {
        if (hz > 0.0) { cpuHz_ = hz; recompute(); }
    }
    void setBusClock(double hz)
    {
        if (hz > 0.0) { busHz_ = hz; recompute(); }
    }

    /// Bus ticks elapsed over `cpuCycles` CPU cycles. The fractional
    /// remainder is carried, so a long run of short spans (one per MMIO
    /// access) sums to exactly what one long span would. Ratio 1 — every
    /// machine without an accelerator — returns its input untouched.
    uint64_t busTicks(uint64_t cpuCycles)
    {
        if (ratio_ == 1.0) return cpuCycles;
        const double t = static_cast<double>(cpuCycles) * ratio_ + frac_;
        const uint64_t n = static_cast<uint64_t>(t);
        frac_ = t - static_cast<double>(n);
        return n;
    }

    /// Bus ticks per CPU cycle (<= 1 under an accelerator).
    double ratio() const { return ratio_; }
    /// The bus clock in Hz (the CPU clock while no bus clock was given).
    double busHz() const { return busHz_ > 0.0 ? busHz_ : cpuHz_; }

private:
    void recompute()
    {
        ratio_ = (busHz_ > 0.0) ? busHz_ / cpuHz_ : 1.0;
        if (ratio_ == 1.0) frac_ = 0.0;
    }

    double cpuHz_ = static_cast<double>(POM2_CPU_CLOCK_HZ);
    double busHz_ = 0.0;     // 0 = not given: follow the CPU clock
    double ratio_ = 1.0;
    double frac_  = 0.0;
};

} // namespace pom2
