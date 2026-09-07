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

#include "SlotCardFactory.h"

#include "CffaCard.h"
#include "CpuClock.h"
#include "DiskIICard.h"
#include "GrapplerCard.h"
#include "LironCard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "ProDOSHardDiskCard.h"
#include "ResourcePaths.h"
#include "WorkstationCard.h"
#include "SlotPeripheral.h"
#include "SmartPortCard.h"

#include <string>
#include <utility>

namespace pom2 {
namespace {

std::string productionResource(std::string_view resource)
{
    return findResource(std::string(resource));
}

} // namespace

SlotCardFactory::SlotCardFactory(ResourceLocator locator)
    : locate_(locator ? std::move(locator) : ResourceLocator(productionResource))
{
}

SlotCardFactory::Result SlotCardFactory::createMouse(
    const Request& request) const
{
    Result result;
    result.requestedKey = request.key;
    result.actualKey = "mouse";

    const std::string slotRom = locate_("roms/mouse_341-0270-c.bin");
    const std::string mcuRom = locate_("roms/mouse_341-0269.bin");
    if (slotRom.empty() || mcuRom.empty()) {
        result.status = "ROMs missing (need roms/mouse_341-0270-c.bin and "
                        "roms/mouse_341-0269.bin)";
        result.warningCategory = "Mouse";
        result.warning = "Mouse Card requested in slot " +
            std::to_string(request.slot) + " but " + result.status +
            " — leaving slot empty";
        return result;
    }

    auto card = std::make_unique<MouseCard>(request.slot);
    if (!card->loadRoms(slotRom, mcuRom)) {
        result.status = "ROM load failed (size mismatch?)";
        result.warningCategory = "Mouse";
        result.warning = "Mouse Card ROM load failed in slot " +
                         std::to_string(request.slot);
        return result;
    }

    result.resourcePath = slotRom;
    result.status = "loaded: " + slotRom + " + " + mcuRom;
    result.card = std::move(card);
    return result;
}

SlotCardFactory::Result SlotCardFactory::create(const Request& request) const
{
    Result result;
    result.requestedKey = request.key;
    result.actualKey = request.key;

    if (request.key == "diskii") {
        auto card = std::make_unique<DiskIICard>(request.slot);
        const std::string bootRom = locate_("roms/disk2.rom");
        if (!bootRom.empty() && card->loadBootRom(bootRom)) {
            result.resourcePath = bootRom;
            result.status = "loaded: " + bootRom;
        } else {
            result.status = "embedded 341-0027-A PROM (no user disk2.rom)";
        }

        const std::string lssRom = locate_("roms/diskii_p6.rom");
        if (!lssRom.empty()) (void)card->loadLssRom(lssRom);
        const std::string boot13 = locate_("roms/disk2_13.rom");
        if (!boot13.empty()) (void)card->loadBootRom13(boot13);
        const std::string lss13 = locate_("roms/diskii_p6_13.rom");
        if (!lss13.empty()) (void)card->loadLssRom13(lss13);
        // The IWM-only $C0nE/$C0nF hooks exist for the //c and //c+ (the
        // profiles with no physical slots); a real wozfdc on a II/II+/IIe has
        // neither register. Bug hunt #3 M10.
        card->setIwmHost(profileConfig(request.profile).noPhysicalSlots);
        result.card = std::move(card);
        return result;
    }

    if (request.key == "cffa") {
        const std::string preferred = request.cpuIsCmos
            ? "roms/cffa20eec02.bin" : "roms/cffa20ee02.bin";
        const std::string fallback = request.cpuIsCmos
            ? "roms/cffa20ee02.bin" : "roms/cffa20eec02.bin";
        std::string rom = locate_(preferred);
        if (rom.empty()) rom = locate_(fallback);
        if (rom.empty()) {
            result.warningCategory = "CFFA";
            result.warning = "Slot " + std::to_string(request.slot) +
                " requested CFFA but no firmware ROM "
                "(roms/cffa20ee02.bin) — leaving slot empty";
            return result;
        }
        auto card = std::make_unique<CffaCard>(request.slot);
        if (!card->loadRom(rom)) {
            result.warningCategory = "CFFA";
            result.warning = "CFFA firmware load failed in slot " +
                             std::to_string(request.slot);
            return result;
        }
        result.resourcePath = rom;
        result.status = "loaded: " + rom;
        result.card = std::move(card);
        return result;
    }

    if (request.key == "hdv") {
        result.card = std::make_unique<ProDOSHardDiskCard>(request.slot);
        return result;
    }

    if (request.key == "grappler") {
        auto card = std::make_unique<GrapplerCard>(request.slot);
        for (const std::string_view candidate : {
                 "roms/grappler_plus.bin", "roms/grappler+.bin",
                 "roms/grappler.bin"}) {
            const std::string rom = locate_(candidate);
            if (!rom.empty() && card->loadRom(rom)) {
                result.resourcePath = rom;
                break;
            }
        }
        if (!card->isRomLoaded()) {
            result.warningCategory = "Grappler";
            result.warning = "Grappler+ plugged in slot " +
                std::to_string(request.slot) +
                " without a 4 KB ROM dump (roms/grappler_plus.bin) — "
                "graphics dump commands unavailable, PR#n still works";
        }
        result.card = std::move(card);
        return result;
    }

    if (request.key == "workstation") {
        // Apple II Workstation Card. Hard ROM gate, unlike the Grappler: the
        // card is a coprocessor, so without its firmware there is nothing for
        // its 65C02 to run and a plugged card would be a dead $Cn00 page the
        // guest could still find. Better to refuse.
        auto card = std::make_unique<WorkstationCard>(request.slot);
        for (const std::string_view candidate : {
                 "roms/341-0358-A.bin", "roms/341-0358-a.bin"}) {
            const std::string rom = locate_(candidate);
            if (!rom.empty() && card->loadRom(rom)) {
                result.resourcePath = rom;
                break;
            }
        }
        if (!card->romLoaded()) {
            result.warningCategory = "Workstation";
            result.warning = "Apple II Workstation Card requested in slot " +
                std::to_string(request.slot) +
                " but roms/341-0358-A.bin (64 KiB) is missing — slot left empty";
            return result;   // no card: the gate is deliberate
        }
        result.card = std::move(card);
        return result;
    }

    if (request.key == "smartport35") {
        auto card = std::make_unique<SmartPortCard>(request.slot);
        if (!profileConfig(request.profile).noPhysicalSlots) {
            const std::string rom = locate_("roms/liron.rom");
            if (!rom.empty()) {
                card->loadLironRom(rom);
                result.resourcePath = rom;
            }
        }
        result.card = std::move(card);
        return result;
    }

    if (request.key == "liron") {
        auto card = std::make_unique<pom2::LironCard>(request.slot);
        if (!card->romLoaded()) {
            result.warningCategory = "rom";
            result.warning = "Liron card requested for slot " +
                std::to_string(request.slot) + " but " + card->lastError() +
                " — slot left empty";
            return result;   // no synthetic fallback: that card is smartport35
        }
        result.resourcePath = "roms/liron.rom";
        result.status = "loaded: roms/liron.rom";
        result.card = std::move(card);
        return result;
    }

    if (request.key == "mouse") return createMouse(request);

    if (request.key == "mouseaw") {
        const std::string slotRom = locate_("roms/mouse_341-0270-c.bin");
        if (!slotRom.empty()) {
            auto card = std::make_unique<MouseCardAppleWin>(request.slot);
            if (card->loadRom(slotRom)) {
                const auto& timing = pom2VideoTiming(
                    profileConfig(request.profile).videoStandard);
                card->setVblCycles(
                    timing.scanlinesPerFrame * timing.cyclesPerScanline);
                result.resourcePath = slotRom;
                result.status = "loaded: " + slotRom;
                result.card = std::move(card);
                return result;
            }
        }

        Request fallbackRequest = request;
        fallbackRequest.key = "mouse";
        result = createMouse(fallbackRequest);
        result.requestedKey = request.key;
        result.fallback = true;
        const std::string reason = slotRom.empty()
            ? "roms/mouse_341-0270-c.bin not found"
            : "AppleWin HLE ROM load failed";
        const std::string fallbackWarning =
            "Mouse (AppleWin HLE) requested in slot " +
            std::to_string(request.slot) + " but " + reason +
            " — falling back to MC68705 \"mouse\" card";
        if (result.warning.empty()) {
            result.warningCategory = "MouseAW";
            result.warning = fallbackWarning;
        } else {
            result.warning = fallbackWarning + "; " + result.warning;
            result.warningCategory = "MouseAW";
        }
        return result;
    }

    return result;
}

} // namespace pom2
