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

// SlotCardFactory ROM policy and deterministic resource-adapter contract.

#include "CffaCard.h"
#include "DiskIICard.h"
#include "GrapplerCard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "ProDOSHardDiskCard.h"
#include "SlotCardFactory.h"
#include "SmartPortCard.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void writeSizedFile(const std::filesystem::path& path, std::size_t size)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::vector<std::uint8_t> bytes(size, 0);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    assert(out.good());
}

pom2::SlotCardFactory::Request request(std::string key, int slot = 4)
{
    pom2::SlotCardFactory::Request out;
    out.key = std::move(key);
    out.slot = slot;
    out.profile = pom2::SystemProfile::AppleIIe;
    return out;
}

} // namespace

int main()
{
    pom2::SlotCardFactory missing(
        [](std::string_view) { return std::string(); });

    const auto embeddedDisk = missing.create(request("diskii", 6));
    assert(dynamic_cast<DiskIICard*>(embeddedDisk.card.get()));
    assert(embeddedDisk.status.find("embedded") != std::string::npos);

    const auto missingCffa = missing.create(request("cffa"));
    // The CFFA ships two firmware builds and the factory picks by the CPU it
    // is told about. applyProfile used to ask AFTER re-plugging, i.e. the
    // outgoing machine's core — a //e → ][+ switch built the 65C02 firmware
    // (INC A / LDA (zp) / BRA = KIL on NMOS) onto a 6502. The pin here is
    // the choice itself; the ordering lives in applyProfile step 6b.
    {
        auto nmos = request("cffa", 7); nmos.cpuIsCmos = false;
        auto cmos = request("cffa", 7); cmos.cpuIsCmos = true;
        std::vector<std::string> probed;
        pom2::SlotCardFactory recorder([&](std::string_view resource) {
            probed.emplace_back(resource);
            return std::string();
        });
        (void)recorder.create(nmos);
        (void)recorder.create(cmos);
        const auto has = [&](const char* leaf) {
            for (const auto& p : probed) if (p.find(leaf) != std::string::npos) return true;
            return false;
        };
        assert(has("cffa20ee02.bin") && has("cffa20eec02.bin") &&
               "the CPU flag must select between the two CFFA firmware builds");
    }
    assert(!missingCffa);
    assert(missingCffa.warningCategory == "CFFA");

    const auto hdv = missing.create(request("hdv", 7));
    assert(dynamic_cast<ProDOSHardDiskCard*>(hdv.card.get()));

    const auto stubGrappler = missing.create(request("grappler", 1));
    assert(dynamic_cast<GrapplerCard*>(stubGrappler.card.get()));
    assert(!stubGrappler.warning.empty());

    const auto missingMouse = missing.create(request("mouse"));
    assert(!missingMouse);
    assert(missingMouse.status.find("ROMs missing") != std::string::npos);

    const auto unknown = missing.create(request("not-a-card"));
    assert(!unknown && unknown.warning.empty());

    const auto token = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const auto temp = std::filesystem::temp_directory_path() /
        ("pom2-slot-card-factory-" + std::to_string(token));
    std::filesystem::create_directories(temp);

    const auto diskRom = temp / "disk2.rom";
    const auto cffaRom = temp / "cffa20eec02.bin";
    const auto grapplerRom = temp / "grappler_plus.bin";
    const auto mouseSlotRom = temp / "mouse_slot.bin";
    const auto mouseMcuRom = temp / "mouse_mcu.bin";
    writeSizedFile(diskRom, 256);
    writeSizedFile(cffaRom, 4096);
    writeSizedFile(grapplerRom, 4096);
    writeSizedFile(mouseSlotRom, 2048);
    writeSizedFile(mouseMcuRom, 2048);

    const std::unordered_map<std::string, std::string> resources{
        {"roms/disk2.rom", diskRom.string()},
        {"roms/cffa20eec02.bin", cffaRom.string()},
        {"roms/grappler_plus.bin", grapplerRom.string()},
        {"roms/mouse_341-0270-c.bin", mouseSlotRom.string()},
        {"roms/mouse_341-0269.bin", mouseMcuRom.string()},
    };
    pom2::SlotCardFactory factory([&](std::string_view name) {
        const auto found = resources.find(std::string(name));
        return found == resources.end() ? std::string() : found->second;
    });

    const auto disk = factory.create(request("diskii", 6));
    assert(dynamic_cast<DiskIICard*>(disk.card.get()));
    assert(disk.resourcePath == diskRom.string());

    auto cffaRequest = request("cffa");
    cffaRequest.cpuIsCmos = true;
    const auto cffa = factory.create(cffaRequest);
    assert(dynamic_cast<pom2::CffaCard*>(cffa.card.get()));
    assert(cffa.resourcePath == cffaRom.string());

    const auto grappler = factory.create(request("grappler", 1));
    assert(dynamic_cast<GrapplerCard*>(grappler.card.get()));
    assert(grappler.warning.empty());

    const auto mouse = factory.create(request("mouse"));
    assert(dynamic_cast<MouseCard*>(mouse.card.get()));
    assert(mouse.actualKey == "mouse" && !mouse.fallback);

    const auto mouseAw = factory.create(request("mouseaw"));
    assert(dynamic_cast<MouseCardAppleWin*>(mouseAw.card.get()));
    assert(mouseAw.actualKey == "mouseaw" && !mouseAw.fallback);

    const auto smartPort = factory.create(request("smartport35", 5));
    assert(dynamic_cast<pom2::SmartPortCard*>(smartPort.card.get()));

    // A stateful fake makes the AppleWin probe fail once, then exposes the
    // same slot ROM to the faithful implementation. This deterministically
    // pins the fallback result without depending on host files.
    int slotRomLookups = 0;
    pom2::SlotCardFactory fallbackFactory([&](std::string_view name) {
        if (name == "roms/mouse_341-0270-c.bin") {
            ++slotRomLookups;
            return slotRomLookups == 1 ? std::string() : mouseSlotRom.string();
        }
        if (name == "roms/mouse_341-0269.bin") return mouseMcuRom.string();
        return std::string();
    });
    const auto fallbackMouse = fallbackFactory.create(request("mouseaw"));
    assert(dynamic_cast<MouseCard*>(fallbackMouse.card.get()));
    assert(fallbackMouse.requestedKey == "mouseaw");
    assert(fallbackMouse.actualKey == "mouse");
    assert(fallbackMouse.fallback);
    assert(!fallbackMouse.warning.empty());

    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    assert(!ec);

    std::cout << "slot card factory: OK\n";
    return 0;
}
