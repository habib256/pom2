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
#include "ApplicardCard.h"
#include "Ccs7710Card.h"
#include "Cffa3000Card.h"
#include "CpuClock.h"
#include "DiskIICard.h"
#include "AppleParallelCard.h"
#include "GrapplerCard.h"
#include "GrapplerClassicCard.h"
#include "LironCard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "IIcMouse.h"
#include "ProDOSHardDiskCard.h"
#include "ResourcePaths.h"
#include "WorkstationCard.h"
#include "SlotPeripheral.h"
#include "SmartPortCard.h"
#include "SlinkyCard.h"
#include "SuperSerialCard.h"
#include "VidexUltratermCard.h"
#include "VidexVideotermCard.h"
#include "VidHDCard.h"
#include "SnesMaxCard.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

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
            result.status = "POM2 boot PROM (no roms/disk2.rom)";
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

    if (request.key == "cffa3") {
        result.card = std::make_unique<Cffa3000Card>(request.slot);
        result.status = "SmartPort block service (8 units)";
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

    if (request.key == "grappler1" || request.key == "pic") {
        // ROM-gated like the CFFA: without its firmware each is a dead $Cn
        // page a detection routine would still find, so the slot stays empty.
        const bool pic = request.key == "pic";
        const std::string rom = locate_(pic ? "roms/341-0057.bin"
                                            : "roms/grappler_eps-1.bin");
        std::vector<uint8_t> bytes;
        if (!rom.empty()) {
            std::ifstream f(rom, std::ios::binary);
            bytes.resize(pic ? AppleParallelCard::kPromBytes + 1
                             : GrapplerClassicCard::kRomBytes + 1);
            f.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<std::size_t>(f.gcount()));
        }
        std::unique_ptr<SlotPeripheral> card;
        if (pic) {
            auto c = std::make_unique<AppleParallelCard>(request.slot);
            if (c->loadProm(bytes)) card = std::move(c);
        } else {
            auto c = std::make_unique<GrapplerClassicCard>(request.slot);
            if (c->loadRom(bytes)) card = std::move(c);
        }
        if (!card) {
            result.warningCategory = pic ? "PIC" : "Grappler";
            result.warning = std::string(pic ? "Apple Parallel Interface"
                                             : "Grappler (1981)") +
                " requested in slot " + std::to_string(request.slot) +
                " but its ROM (" + (pic ? "roms/341-0057.bin, 512 B"
                                        : "roms/grappler_eps-1.bin, 2 KB") +
                ") is missing or the wrong size — slot left empty";
            return result;
        }
        result.resourcePath = rom;
        result.status = "loaded: " + rom;
        card->onReset();
        result.card = std::move(card);
        return result;
    }

    if (request.key == "videoterm") {
        // Videx Videoterm. ROM-gated like the Grappler and the PIC: the
        // firmware and the normal character set ARE the card — without them
        // PR#3 jumps into $FF and the 80 columns have no glyphs. The 2 KB
        // clone dump (MAME BIOS 0, 60 Hz CRTC table) is preferred, the 1 KB
        // Videx v2.4 (BIOS 1, 50 Hz table) is the fallback. The alternate
        // (inverse) set is optional: its absence is exactly ~normal.
        const auto readUpTo = [](const std::string& path, std::size_t max) {
            std::vector<uint8_t> b;
            if (path.empty()) return b;
            std::ifstream f(path, std::ios::binary);
            b.resize(max + 1);   // one byte over: an oversized file is refused
            f.read(reinterpret_cast<char*>(b.data()),
                   static_cast<std::streamsize>(b.size()));
            b.resize(static_cast<std::size_t>(f.gcount()));
            return b;
        };
        auto card = std::make_unique<VidexVideotermCard>(request.slot);
        std::string fwPath;
        for (const std::string_view candidate : {
                 "roms/videx_videoterm_v24_60hz.bin",
                 "roms/videx_videoterm_v24_50hz.bin"}) {
            const std::string p = locate_(candidate);
            if (!p.empty() &&
                card->loadFirmware(readUpTo(p, VidexVideotermCard::kRomBytes))) {
                fwPath = p;
                break;
            }
        }
        const std::string normal = locate_("roms/videx_videoterm_char_normal.bin");
        const std::string alt    = locate_("roms/videx_videoterm_char_inverse.bin");
        std::vector<uint8_t> altBytes =
            readUpTo(alt, VidexVideotermCard::kCharSetBytes);
        if (altBytes.size() != VidexVideotermCard::kCharSetBytes) altBytes.clear();
        const bool charsOk = card->loadCharRoms(
            readUpTo(normal, VidexVideotermCard::kCharSetBytes), altBytes);
        if (fwPath.empty() || !charsOk) {
            result.warningCategory = "Videoterm";
            result.warning = "Videx Videoterm requested in slot " +
                std::to_string(request.slot) + " but " +
                (fwPath.empty()
                     ? "its firmware (roms/videx_videoterm_v24_60hz.bin, 2 KB, "
                       "or roms/videx_videoterm_v24_50hz.bin, 1 KB)"
                     : "its character ROM (roms/videx_videoterm_char_normal.bin, 2 KB)") +
                " is missing or the wrong size — slot left empty";
            return result;
        }
        result.resourcePath = fwPath;
        result.status = "loaded: " + fwPath + " + " + normal +
            (card->alternateIsDerived() ? std::string(" (inverse set derived)")
                                        : " + " + alt);
        card->onReset();
        result.card = std::move(card);
        return result;
    }

    if (request.key == "ccs7710") {
        // CCS 7710: never refused — without CCS's PROMs it serves POM2's
        // own firmware page (Ccs7710Card.h).
        auto card = std::make_unique<Ccs7710Card>(request.slot);
        auto read = [&](const char* rel) {
            std::vector<uint8_t> b;
            const std::string p = locate_(rel);
            if (!p.empty()) {
                std::ifstream f(p, std::ios::binary);
                b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            return b;
        };
        if (card->loadProms(read(Ccs7710Card::kPromHiPath), read(Ccs7710Card::kPromLoPath)))
            result.status = std::string("loaded: ") + Ccs7710Card::kPromHiPath + " + " + Ccs7710Card::kPromLoPath;
        else
            result.status = "POM2's own firmware page";
        result.card = std::move(card);
        return result;
    }

    if (request.key == "applicard") {
        // PCPI Appli-Card. ROM-gated: the Z80 boots from its 2 KB ROM and has
        // nothing else to run (ApplicardCard.h).
        const std::string rom = locate_(ApplicardCard::kRomPath);
        std::vector<uint8_t> bytes;
        if (!rom.empty()) {
            std::ifstream f(rom, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        auto card = std::make_unique<ApplicardCard>();
        if (!card->loadRom(bytes)) {
            result.warningCategory = "Applicard";
            result.warning = "PCPI Applicard requested in slot " +
                std::to_string(request.slot) + " but " + ApplicardCard::kRomPath +
                " (2 KB) is missing or the wrong size — slot left empty";
            return result;
        }
        result.resourcePath = rom;
        result.status = "loaded: " + rom;
        result.card = std::move(card);
        return result;
    }

    if (request.key == "ultraterm") {
        // Videx UltraTerm. ROM-gated like the Videoterm. A //e-class machine
        // takes the enhanced-//e set (MouseText glyphs) when it is present,
        // MAME's a2ultrme; everything else the original a2ulttrm.
        const auto readFile = [](const std::string& path) {
            std::vector<uint8_t> b;
            if (path.empty()) return b;
            std::ifstream f(path, std::ios::binary);
            b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return b;
        };
        auto card = std::make_unique<VidexUltratermCard>(request.slot);
        std::string fw, chars;
        const bool iie = profileConfig(request.profile).iieMode;
        const std::pair<const char*, const char*> sets[] = {
            { VidexUltratermCard::kRomPathEnh, VidexUltratermCard::kCharPathEnh },
            { VidexUltratermCard::kRomPath,    VidexUltratermCard::kCharPath },
        };
        for (std::size_t i = iie ? 0 : 1; i < 2; ++i) {
            const std::string f = locate_(sets[i].first), c = locate_(sets[i].second);
            if (!f.empty() && !c.empty() && card->loadRoms(readFile(f), readFile(c))) {
                fw = f;
                chars = c;
                break;
            }
        }
        if (fw.empty()) {
            result.warningCategory = "UltraTerm";
            result.warning = "Videx UltraTerm requested in slot " +
                std::to_string(request.slot) + " but its firmware and character ROM (" +
                VidexUltratermCard::kRomPath + ", " + VidexUltratermCard::kCharPath +
                ", 4 KB each) are missing or the wrong size — slot left empty";
            return result;
        }
        result.resourcePath = fw;
        result.status = "loaded: " + fw + " + " + chars;
        card->onReset();
        result.card = std::move(card);
        return result;
    }

    if (request.key == "snesmax") {
        result.card = std::make_unique<SnesMaxCard>(request.slot);
        return result;
    }
    if (request.key == "vidhd") {
        // VidHD: no ROM to load (the detection bytes are hard-wired,
        // VidHDCard.h). Its picture reaches the monitor through the
        // display's CardVideoSource seam, wired by the caller.
        auto card = std::make_unique<VidHDCard>(request.slot);
        card->onReset();
        result.status = "no ROM needed";
        result.card = std::move(card);
        return result;
    }

    if (request.key == "slinky") {
        // Apple Memory Expansion Card. ROM-gated like the CFFA: the 341-0344A
        // firmware IS the RAM-disk driver, and a card without it would be a
        // $Cn page ProDOS finds and cannot use (SlinkyCard.h).
        const std::string rom = locate_(SlinkyCard::kRomPath);
        std::vector<uint8_t> bytes;
        if (!rom.empty()) {
            std::ifstream f(rom, std::ios::binary);
            bytes.resize(SlinkyCard::kRomBytes + 1);
            f.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<std::size_t>(f.gcount()));
        }
        auto card = std::make_unique<SlinkyCard>(request.slot);
        if (!card->loadRom(bytes)) {
            result.warningCategory = "Slinky";
            result.warning = "Memory Expansion card requested in slot " +
                std::to_string(request.slot) + " but " +
                SlinkyCard::kRomPath + " (4 KB) is missing or the wrong size"
                " — slot left empty";
            return result;
        }
        result.resourcePath = rom;
        result.status = "loaded: " + rom;
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

    if (request.key == "smartport35" || request.key == "superdrive") {
        if (request.key == "superdrive" &&
            request.profile != SystemProfile::AppleIIe &&
            request.profile != SystemProfile::AppleIIePAL) {
            result.warningCategory = "SuperDrive";
            result.warning = "Apple IIe SuperDrive requires an enhanced Apple IIe profile";
            return result;
        }
        auto card = std::make_unique<SmartPortCard>(request.slot);
        card->setSuperDriveMode(request.key == "superdrive");
        if (request.key == "smartport35" &&
            !profileConfig(request.profile).noPhysicalSlots) {
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

    if (request.key == "iicmouse") {
        if (!profileConfig(request.profile).noPhysicalSlots || request.slot != 4) return result;
        result.card = std::make_unique<IIcMouse>(
            request.profile == SystemProfile::AppleIIcPlus ? 7 : 4);
        result.status = "native //c IOU mouse (system ROM)";
        return result;
    }

    if (request.key == "mouseaw") {
        const std::string slotRom = locate_("roms/mouse_341-0270-c.bin");
        if (!slotRom.empty()) {
            auto card = std::make_unique<MouseCardAppleWin>(request.slot);
            if (card->loadRom(slotRom)) {
                card->setIicHost(profileConfig(request.profile).noPhysicalSlots);
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

std::string SlotCardFactory::loadSuperSerialFirmware(
    SuperSerialCard& card, SystemProfile profile) const
{
    if (profileConfig(profile).noPhysicalSlots) return {};
    for (const std::string_view candidate : {
             "roms/ssc_341-0065-a.bin", "roms/341-0065-a.bin",
             "roms/SSC.rom"}) {
        const std::string rom = locate_(candidate);
        if (rom.empty()) continue;
        // One byte past the size, so a larger file is refused, not truncated.
        std::ifstream f(rom, std::ios::binary);
        std::vector<uint8_t> bytes(SuperSerialCard::kFirmwareBytes + 1);
        f.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(f.gcount()));
        if (card.loadFirmware(bytes)) return rom;
    }
    return {};
}

} // namespace pom2
