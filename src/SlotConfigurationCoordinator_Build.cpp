// POM2 — GPL-3.0-or-later
#include "SlotConfigurationCoordinator.h"
#include "EmulationController.h"
#include "Memory.h"
#include "PremiumSoftCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include <memory>
#include <utility>

namespace pom2 {
StorageCoordinator::RestoreSettingsResult SlotConfigurationCoordinator::buildResolvedPlan(
    const StateAccess& state, const Settings& settings, SystemProfile profile,
    const std::function<BuildClocks()>& clockSource, const BuildCard& buildCard,
    const std::function<void()>& runtimeOverlays) const
{
    auto& memory = state.memory();
    auto& bus = memory.slotBus();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot)
        if (!effectivePlan_[slot].empty()) buildCard(slot, effectivePlan_[slot]);

    if (settings.getBool("premium_softcard", false) && memory.isIIE() &&
        !profileConfig(profile).noPhysicalSlots) {
        auto card = std::make_unique<PremiumSoftCard>();
        card->setMemory(&memory);
        bus.plug(PremiumSoftCard::kAuxSlot, std::move(card));
    }
    if (runtimeOverlays) runtimeOverlays();

    // Legacy primary-drive settings require the finished topology. Restoring
    // per construction callback made a moved HDV/second Disk II drive choose
    // the wrong card. Rebuilds deliberately stop the worker for this I/O.
    auto restored = StorageCoordinator{}.restoreMediaFromSettings(bus, settings);
    const auto clocks = clockSource();
    // Include slot zero: the auxiliary Premium SoftCard has a Z80 clock.
    // Distinguish paced audio time, machine time, and the crystal clock.
    for (int slot = 0; slot < SlotBus::kSlotCount; ++slot)
        if (auto* card = bus.peripheral(slot)) {
            card->setCpuClock(clocks.cpu);
            card->setMachineCpuClock(clocks.machine);
            card->setStandardClock(clocks.standard);
        }
    return restored;
}
}
