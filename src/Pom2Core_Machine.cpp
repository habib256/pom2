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

// The SDK's machine options beside the motherboard: a socket accelerator and
// the beam-paced run that goes with it, the No-Slot Clock, and a RamWorks
// III. Split from Pom2Core.cpp (file-size rule R0, docs/decisions/
// project-scope.md); same Core::Impl (Pom2CoreImpl.h).

#include "Pom2CoreImpl.h"

#include "FloppySoundDevice.h"
#include "NoSlotClock.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <memory>
#include <utility>

namespace pom2 {

bool Core::attachNoSlotClock(std::int64_t startEpoch)
{
    if (impl_->noSlotClock) {
        impl_->error = "A No-Slot Clock is already attached";
        return false;
    }
    auto* impl = impl_.get();
    // The start lives in the machine, not in the callable: a restore sets it
    // from the snapshot, and a start < 0 is the host's clock.
    pom2::NoSlotClock::TimeFn source = [impl]() {
        std::time_t when = std::time(nullptr);
        if (impl->clockStartEpoch >= 0) {
            // Beam time: an accelerator speeds the CPU, not the clock chip.
            const auto elapsed = static_cast<std::int64_t>(
                static_cast<double>(impl->memory.videoNow()) / impl->cpuClockHz);
            when = static_cast<std::time_t>(impl->clockStartEpoch + elapsed);
        }
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &when);
#else
        localtime_r(&when, &tm);
#endif
        return tm;
    };
    impl_->clockStartEpoch = startEpoch;
    impl_->noSlotClock = std::make_unique<pom2::NoSlotClock>(std::move(source));
    impl_->memory.setNoSlotClock(impl_->noSlotClock.get());
    impl_->error.clear();
    return true;
}

bool Core::noSlotClockAttached() const
{
    return impl_->noSlotClock != nullptr;
}

bool Core::setRamWorksBanks(int banks)
{
    if (!impl_->iieMemory || impl_->iicClass() || banks < 1 || banks > 128) {
        impl_->error = "RamWorks III needs a //e and 1 to 128 banks";
        return false;
    }
    impl_->memory.setRamWorksBanks(static_cast<std::uint32_t>(banks));
    impl_->error.clear();
    return true;
}

int Core::ramWorksBanks() const
{
    return static_cast<int>(impl_->memory.ramWorksBanks());
}

void Core::Impl::applySpeed(double multiplier)
{
    if (multiplier == appliedSpeed) return;
    appliedSpeed = multiplier;
    const double stockHz = cpuClockHz;
    const double hz = stockHz * multiplier;
    // As EmulationController::applyAcceleratorClock: the beam keeps the
    // crystal's time, the speaker stamps stock time from this cycle on, and
    // the cards are told the CPU's clock (their AYs keep the bus clock).
    memory.setVideoClockRatio(1.0 / multiplier);
    speaker.setCpuClockAt(memory.getCycleCounter(), hz, stockHz);
    cassette.setCpuClock(hz);
    if (driveSound525) driveSound525->setCpuClock(stockHz);   // the Disk II stamps stock time
    if (driveSound35) driveSound35->setCpuClock(hz);
    for (int slot = 0; slot < SlotBus::kSlotCount; ++slot)
        if (SlotPeripheral* card = memory.slotBus().peripheral(slot)) {
            card->setCpuClock(hz);
            card->setMachineCpuClock(hz);
        }
}

bool Core::setAccelerator(Accelerator model)
{
    const bool plusMachine = impl_->machine == Machine::AppleIIcPlus;
    if ((impl_->machine == Machine::AppleIIc && model != Accelerator::None) ||
        (plusMachine && model == Accelerator::FastChip)) {
        impl_->error = "This machine cannot take that accelerator";
        return false;
    }
    pom2::CpuSocketAccelerator* part = nullptr;
    const double stock = impl_->cpuClockHz;
    if (model == Accelerator::ZipChip) {
        impl_->zipChip = pom2::ZipChip{};
        impl_->zipChip.setStockClockHz(stock);
        // [D] MAME apple2e.cpp accel_reset: the //c+ comes up fast.
        impl_->zipChip.setBootFast(true);
        impl_->zipChip.setSoldered(plusMachine);
        impl_->zipChip.onReset(impl_->memory.getCycleCounter());
        part = &impl_->zipChip;
    } else if (model == Accelerator::FastChip) {
        impl_->fastChip = pom2::FastChip{};
        impl_->fastChip.setStockClockHz(stock);
        impl_->fastChip.onReset(impl_->memory.getCycleCounter());
        part = &impl_->fastChip;
    }
    impl_->memory.setCpuSocketAccelerator(part);
    impl_->acceleratorModel = model;
    impl_->applySpeed(std::max(1e-3, impl_->memory.cpuSpeedMultiplier()));
    impl_->error.clear();
    return true;
}

Accelerator Core::accelerator() const
{
    return impl_->acceleratorModel;
}

double Core::cpuSpeedMultiplier() const
{
    return impl_->memory.cpuSpeedMultiplier();
}

int Core::runVideo(int videoCycles)
{
    if (videoCycles <= 0) return 0;
    if (impl_->acceleratorModel == Accelerator::None && impl_->appliedSpeed == 1.0)
        return run(videoCycles);
    // The application's chunk loop (EmulationController::runFor): each
    // slice is sized from the speed the guest has selected now.
    constexpr int kSlice = 4096;
    double done = 0.0;
    while (done < videoCycles) {
        const double speed = std::max(1e-3, impl_->memory.cpuSpeedMultiplier());
        impl_->applySpeed(speed);
        const double remaining = std::ceil((videoCycles - done) * speed);
        const int slice = static_cast<int>(std::min<double>(kSlice, std::max(1.0, remaining)));
        const int ran = run(slice);
        done += static_cast<double>(ran > 0 ? ran : slice) / speed;
    }
    return static_cast<int>(std::lround(done));
}

}  // namespace pom2
