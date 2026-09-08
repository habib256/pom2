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

#include "SlotProvisioningCoordinator.h"

#include "CffaCard.h"
#include "EmulationController.h"
#include "FloppySoundDevice.h"
#include "Logger.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SlotCardFactory.h"
#include "SmartPortCard.h"
#include "StorageCoordinator.h"
#include "SystemProfile.h"

#include <utility>

namespace pom2 {
namespace {

int findFreeSlot(const SlotBus& bus, int preferred, bool iieClass)
{
    if (preferred >= 1 && preferred < SlotBus::kSlotCount &&
        !bus.isPlugged(preferred)) {
        return preferred;
    }
    for (int slot = SlotBus::kSlotCount - 1; slot >= 1; --slot) {
        // Slot 3 is not a free slot on a //e-class machine: with SLOTC3ROM
        // off (the reset default) the motherboard owns $C300-$C3FF and the
        // card has no $Cn00 page the guest can reach, so bootFromSlot(N)
        // fails its Appx-C signature check. Slot Config already warns about
        // this for a hand-plugged card (MainWindow_Slots.cpp) — and on the
        // shipped default map slot 3 is the ONLY free one, so every
        // `POM2 game.hdv` on a fresh install auto-plugged into a dead slot
        // and then blamed the image. Refusing is the honest answer.
        if (slot == 3 && iieClass) continue;
        if (!bus.isPlugged(slot)) return slot;
    }
    return -1;
}

} // namespace

SlotProvisioningCoordinator::SlotProvisioningCoordinator(
    SlotCardFactory& factory, StorageCoordinator& storage)
    : factory_(factory), storage_(storage)
{
}

SlotProvisioningCoordinator::Result
SlotProvisioningCoordinator::ensureHdvBootTarget(
    EmulationController& controller, const Settings& settings,
    SystemProfile profile)
{
    Result result;
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    const auto cards = storage_.topology(bus);

    // //c-class firmware can boot only the built-in SmartPort. Physical HDV
    // and CFFA cards are masked by forced INTCXROM on those profiles.
    const bool smartPortOnly =
        profile == SystemProfile::AppleIIc ||
        profile == SystemProfile::AppleIIcPlus ||
        profile == SystemProfile::AppleIIcPAL;
    if (smartPortOnly) {
        if (cards.primarySmartPort) {
            result.slot = cards.primarySmartPort->getSlot();
            return result;
        }
        result.error = "this profile has no bootable SmartPort target";
        return result;
    }

    if (cards.primaryCffa) {
        result.slot = cards.primaryCffa->getSlot();
        return result;
    }
    if (cards.primaryHdv) {
        result.slot = cards.primaryHdv->getSlot();
        return result;
    }
    if (cards.primarySmartPort) {
        result.slot = cards.primarySmartPort->getSlot();
        return result;
    }

    const int slot = findFreeSlot(bus, 7, profileConfig(profile).iieMode);
    if (slot < 0) {
        result.error = "no free slot for an HDV card";
        return result;
    }

    SlotCardFactory::Request request;
    request.key = "hdv";
    request.slot = slot;
    request.profile = profile;
    auto made = factory_.create(request);
    auto* card = dynamic_cast<ProDOSHardDiskCard*>(made.card.get());
    if (!card) {
        result.error = made.warning.empty()
            ? "HDV card construction failed" : std::move(made.warning);
        return result;
    }
    card->setWriteBackEnabled(settings.getBool("hdv_writeback", false));
    bus.plug(slot, std::move(made.card));
    storage_.markAutoProvisionedHdv(slot);

    result.slot = slot;
    result.added = true;
    log().info("Slots",
        "auto-plugged ProDOS HDV card in slot " + std::to_string(slot) +
        " (saved slot config unchanged)");
    return result;
}

SlotProvisioningCoordinator::Result
SlotProvisioningCoordinator::ensureSmartPortBootTarget(
    EmulationController& controller, SystemProfile profile)
{
    Result result;
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    const auto cards = storage_.topology(bus);
    if (cards.primarySmartPort) {
        result.slot = cards.primarySmartPort->getSlot();
        return result;
    }
    if (profileConfig(profile).noPhysicalSlots) {
        result.error = "this profile has no physical slot for SmartPort";
        return result;
    }

    const int slot = findFreeSlot(bus, 5, profileConfig(profile).iieMode);
    if (slot < 0) {
        result.error = "no free slot for a SmartPort card";
        return result;
    }

    SlotCardFactory::Request request;
    request.key = "smartport35";
    request.slot = slot;
    request.profile = profile;
    auto made = factory_.create(request);
    auto* card = dynamic_cast<SmartPortCard*>(made.card.get());
    if (!card) {
        result.error = made.warning.empty()
            ? "SmartPort card construction failed" : std::move(made.warning);
        return result;
    }
    card->setFloppySound(&controller.floppySound35());
    bus.plug(slot, std::move(made.card));
    storage_.markAutoProvisionedSmartPort(slot);

    result.slot = slot;
    result.added = true;
    log().info("Slots",
        "auto-plugged SmartPort card in slot " + std::to_string(slot) +
        " (saved slot config unchanged)");
    return result;
}

bool SlotProvisioningCoordinator::isSessionOnlySlot(int slot) const noexcept
{
    return slot == storage_.autoProvisionedHdvSlot() ||
           slot == storage_.autoProvisionedSmartPortSlot();
}

void SlotProvisioningCoordinator::resetSessionTracking() noexcept
{
    storage_.clearAutoProvisioned();
}

} // namespace pom2
