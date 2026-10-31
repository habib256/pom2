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

// EmulationController — the CPU socket: which accelerator sits in it (the
// //c Plus's own, the user's Zip Chip or FASTChip //e, or none).

#include "EmulationController.h"

void EmulationController::setZipChipInstalled(bool on)
{
    std::lock_guard<std::mutex> lk(stateMtx);
    zipInstalled_.store(on);
    zipChip_.onReset(0);   // a chip just seated comes up as after a reset
    if (on) fastInstalled_.store(false);   // one socket
    installSocketAccelerator();
}

void EmulationController::setFastChipInstalled(bool on)
{
    std::lock_guard<std::mutex> lk(stateMtx);
    fastInstalled_.store(on);
    fastChip_.onReset(0);
    if (on) zipInstalled_.store(false);
    installSocketAccelerator();
}

void EmulationController::installSocketAccelerator()
{
    // The //c Plus has its accelerator soldered on, so the user's Zip Chip
    // option (a //e socket part) never applies there.
    if (mem.isIIcPlus()) {
        if (mem.cpuSocketAccelerator() != &iicPlusZip_) {
            iicPlusZip_.setBootFast(true);   // MAME accel_reset: IIc+ → fast
            iicPlusZip_.setSoldered(true);
            iicPlusZip_.onReset(0);
        }
        mem.setCpuSocketAccelerator(&iicPlusZip_);
        return;
    }
    mem.setCpuSocketAccelerator(zipInstalled_.load()  ? static_cast<pom2::CpuSocketAccelerator*>(&zipChip_)
                              : fastInstalled_.load() ? static_cast<pom2::CpuSocketAccelerator*>(&fastChip_)
                              : nullptr);
}

