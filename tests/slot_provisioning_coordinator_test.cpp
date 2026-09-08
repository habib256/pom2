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

// Session-only boot-storage provisioning policy.

#include "CffaCard.h"
#include "EmulationController.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SlotCardFactory.h"
#include "SlotPeripheral.h"
#include "SlotProvisioningCoordinator.h"
#include "SmartPortCard.h"
#include "StorageCoordinator.h"

#include <cassert>
#include <memory>
#include <string>
#include <string_view>

namespace {

class OccupiedCard final : public SlotPeripheral
{
public:
    std::string_view name() const override { return "occupied"; }
};

} // namespace

int main()
{
    int resourceLookups = 0;
    pom2::SlotCardFactory factory([&](std::string_view resource) {
        if (resource == "roms/liron.rom") ++resourceLookups;
        return std::string();
    });

    // Explicit HDV boot intent provisions slot 7 and inherits write-back
    // without persisting either the card or its empty medium.
    pom2::StorageCoordinator storage;
    pom2::SlotProvisioningCoordinator provision(factory, storage);
    pom2::Settings settings;
    settings.setBool("hdv_writeback", true);
    EmulationController hdvController;
    auto target = provision.ensureHdvBootTarget(
        hdvController, settings, pom2::SystemProfile::AppleIIe);
    assert(target && target.added && target.slot == 7);
    assert(provision.isSessionOnlySlot(7));
    {
        auto state = hdvController.lockState();
        const auto* card = dynamic_cast<const ProDOSHardDiskCard*>(
            state.memory().slotBus().peripheral(7));
        assert(card && card->isWriteBackEnabled());
    }
    target = provision.ensureHdvBootTarget(
        hdvController, settings, pom2::SystemProfile::AppleIIe);
    assert(target && !target.added && target.slot == 7);
    provision.resetSessionTracking();
    assert(!provision.isSessionOnlySlot(7));

    // Existing dedicated devices win in the historical CFFA -> HDV ->
    // SmartPort order; no new topology is created.
    pom2::StorageCoordinator existingStorage;
    pom2::SlotProvisioningCoordinator existingProvision(
        factory, existingStorage);
    EmulationController existingController;
    {
        auto state = existingController.lockState();
        state.memory().slotBus().plug(
            4, std::make_unique<pom2::CffaCard>(4));
    }
    target = existingProvision.ensureHdvBootTarget(
        existingController, settings, pom2::SystemProfile::AppleIIe);
    assert(target && !target.added && target.slot == 4);

    // //c-class HDV must use its ROM-visible built-in SmartPort rather than
    // an expansion block card masked by INTCXROM.
    pom2::StorageCoordinator iicStorage;
    pom2::SlotProvisioningCoordinator iicProvision(factory, iicStorage);
    EmulationController iicController;
    {
        auto state = iicController.lockState();
        state.memory().slotBus().plug(
            4, std::make_unique<pom2::CffaCard>(4));
        state.memory().slotBus().plug(
            5, std::make_unique<pom2::SmartPortCard>(5));
    }
    target = iicProvision.ensureHdvBootTarget(
        iicController, settings, pom2::SystemProfile::AppleIIc);
    assert(target && !target.added && target.slot == 5);
    target = iicProvision.ensureSmartPortBootTarget(
        iicController, pom2::SystemProfile::AppleIIc);
    assert(target && !target.added && target.slot == 5);

    // A no-physical-slot profile cannot be silently given an expansion card.
    pom2::StorageCoordinator closedStorage;
    pom2::SlotProvisioningCoordinator closedProvision(factory, closedStorage);
    EmulationController closedController;
    target = closedProvision.ensureSmartPortBootTarget(
        closedController, pom2::SystemProfile::AppleIIc);
    assert(!target && !target.error.empty());
    target = closedProvision.ensureHdvBootTarget(
        closedController, settings, pom2::SystemProfile::AppleIIc);
    assert(!target && !target.error.empty());

    // SmartPort uses the injected factory/resource adapter, prefers slot 5,
    // and is idempotent on repeated explicit boot requests.
    pom2::StorageCoordinator smartStorage;
    pom2::SlotProvisioningCoordinator smartProvision(factory, smartStorage);
    EmulationController smartController;
    target = smartProvision.ensureSmartPortBootTarget(
        smartController, pom2::SystemProfile::AppleIIe);
    assert(target && target.added && target.slot == 5);
    assert(resourceLookups == 1);
    assert(smartProvision.isSessionOnlySlot(5));
    const auto typeCommand = smartStorage.setMediaBayType(
        smartController, settings, 5, 0, "35");
    assert(typeCommand.ok);
    assert(settings.getString(
        "smartport_slot5_unit0_type", "not-persisted") ==
        "not-persisted");
    target = smartProvision.ensureSmartPortBootTarget(
        smartController, pom2::SystemProfile::AppleIIe);
    assert(target && !target.added && target.slot == 5);
    assert(resourceLookups == 1);

    // With every slot occupied, provisioning reports a value error and does
    // not replace an existing card.
    pom2::StorageCoordinator fullStorage;
    pom2::SlotProvisioningCoordinator fullProvision(factory, fullStorage);
    EmulationController fullController;
    {
        auto state = fullController.lockState();
        for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
            state.memory().slotBus().plug(
                slot, std::make_unique<OccupiedCard>());
        }
    }
    target = fullProvision.ensureSmartPortBootTarget(
        fullController, pom2::SystemProfile::AppleIIe);
    assert(!target && !target.error.empty());

    // The shipped default map fills 1,2,4,5,6,7 and leaves ONLY slot 3 free.
    // On a //e-class machine slot 3 is not a slot the guest can boot from —
    // $C300 is the internal 80-column firmware with SLOTC3ROM off — so the
    // auto-plug used to land there, fail the boot signature and blame the
    // image. A II+ has a real slot 3 and may still use it.
    {
        pom2::StorageCoordinator s3Storage;
        pom2::SlotProvisioningCoordinator s3Provision(factory, s3Storage);
        EmulationController s3Controller;
        {
            auto state = s3Controller.lockState();
            for (int slot : {1, 2, 4, 5, 6, 7})
                state.memory().slotBus().plug(slot, std::make_unique<OccupiedCard>());
        }
        auto t = s3Provision.ensureHdvBootTarget(
            s3Controller, settings, pom2::SystemProfile::AppleIIePAL);
        assert(!t && !t.error.empty() && "a //e must not auto-plug into slot 3");
        t = s3Provision.ensureSmartPortBootTarget(
            s3Controller, pom2::SystemProfile::AppleIIePAL);
        assert(!t && !t.error.empty());
        {
            auto state = s3Controller.lockState();
            assert(!state.memory().slotBus().isPlugged(3) && "nothing was plugged");
        }
        t = s3Provision.ensureHdvBootTarget(
            s3Controller, settings, pom2::SystemProfile::AppleIIPlus);
        assert(t && t.added && t.slot == 3 && "a II+ has a real slot 3");
    }

    return 0;
}
