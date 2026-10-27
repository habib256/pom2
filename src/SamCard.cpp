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

#include "SamCard.h"

#include "M6502.h"

namespace pom2 {

SamCard::SamCard(int) {}

// MAME a2sam.cpp:44 — every offset of $C0n0-$C0nF is the DAC latch.
void SamCard::deviceSelectWrite(uint8_t /*low4*/, uint8_t v)
{
    const uint64_t* bus = busCycleCounter();
    const uint64_t now = (bus ? *bus : 0) +
        (cpu_ ? static_cast<uint64_t>(cpu_->getCurrentInstructionCycles()) : 0);
    dac_.write(static_cast<uint64_t>(toStock(now) + 0.5), v);
}

double SamCard::toStock(uint64_t cycle) const
{
    const double s = static_cast<double>(cycle) * stockRatio_ + stockOffset_;
    return s > 0.0 ? s : 0.0;
}

// Same split as SpeakerDevice::setCpuClockAt: the stock position of the
// current cycle is kept, only the slope changes from here on.
void SamCard::rebase(double cpuHz, double stockHz)
{
    const double newHz = stockHz > 0.0 ? stockHz : cpuHz;
    const double ratio = newHz / cpuHz;
    if (ratio != stockRatio_) {
        const uint64_t* bus = busCycleCounter();
        const uint64_t now = bus ? *bus : 0;
        const double at = toStock(now);
        stockRatio_  = ratio;
        stockOffset_ = at - static_cast<double>(now) * ratio;
    }
    dac_.setCpuClock(newHz);
}

void SamCard::setCpuClock(double hz)
{
    if (!(hz > 0.0)) return;
    cpuHz_ = hz;
    rebase(cpuHz_, stockHz_);
}

void SamCard::setStandardClock(double hz)
{
    if (!(hz > 0.0)) return;
    stockHz_ = hz;
    rebase(cpuHz_ > 0.0 ? cpuHz_ : hz, stockHz_);
}

} // namespace pom2
