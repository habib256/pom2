// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "machine_profile.hpp"

#include "WozCompat.h"

#include <array>
#include <string>
#include <utility>
#include <vector>
#include <cstring>
#include <fstream>
#include <initializer_list>

namespace pom2::libretro {
namespace {

struct Firmware {
    MachineKind kind;
    const char* option;
    const char* name;
    std::uint16_t wozBit;
    std::array<const char*, 3> roms;            // unused entries are null
    std::array<const char*, 3> characterRoms;
};

// The application's probe lists (SystemProfile.cpp), less the fallbacks that
// would load another machine's firmware: an Enhanced ROM on the NMOS //e
// meets 65C02 opcodes and stops.
const Firmware kFirmware[] = {
    {MachineKind::IIeEnhanced, "iie-enhanced", "Apple //e Enhanced", woz::kAppleIIeEnhanced,
     {"apple2e.rom"}, {"apple2e_char.rom", "apple2_char.rom"}},
    {MachineKind::IIe, "iie", "Apple //e", woz::kAppleIIe,
     {"apple2e_unenh.rom", "342-0135-b.64.rom"},
     {"apple2e_char_2k.rom", "342-0133-a.chr.rom", "apple2_char.rom"}},
    {MachineKind::IIPlus, "iiplus", "Apple ][+", woz::kAppleIIPlus,
     {"apple2p.rom", "apple2.rom"}, {"apple2_char.rom"}},
    {MachineKind::II, "ii", "Apple ][", woz::kAppleII,
     {"apple2o.rom"}, {"apple2_char.rom"}},
    {MachineKind::IIc, "iic", "Apple //c", woz::kAppleIIc,
     {"apple2c-32Kv0.rom", "apple2c-16K.rom", "3420033a.256"},
     {"apple2e_char.rom", "apple2_char.rom"}},
    {MachineKind::IIcPlus, "iic-plus", "Apple //c Plus", woz::kAppleIIcPlus,
     {"apple2cp.rom", "apple2c-plus.rom"}, {"apple2e_char.rom", "apple2_char.rom"}},
};

const Firmware& firmwareFor(MachineKind kind)
{
    for (const auto& entry : kFirmware)
        if (entry.kind == kind) return entry;
    return kFirmware[0];
}

std::filesystem::path firstPresent(const std::filesystem::path& directory,
                                   const std::array<const char*, 3>& names)
{
    std::error_code error;
    for (const char* name : names) {
        if (!name) continue;
        const auto path = directory / name;
        if (std::filesystem::is_regular_file(path, error)) return path;
    }
    return {};
}

bool available(const std::filesystem::path& directory, const Firmware& entry)
{
    return !firstPresent(directory, entry.roms).empty()
        && !firstPresent(directory, entry.characterRoms).empty();
}

std::string missingMessage(const Firmware& entry)
{
    std::string text = entry.name;
    text += " needs ";
    text += entry.roms[0];
    text += " and ";
    text += entry.characterRoms[0];
    text += " in the pom2 folder of the frontend's system directory.";
    return text;
}

Machine namedMachine(MachineKind kind)
{
    switch (kind) {
    case MachineKind::IIeEnhanced: return Machine::AppleIIeEnhanced;
    case MachineKind::IIe:         return Machine::AppleIIe;
    case MachineKind::IIPlus:      return Machine::AppleIIPlus;
    case MachineKind::II:          return Machine::AppleIIPlus;   // same board, older ROM
    case MachineKind::IIc:         return Machine::AppleIIc;
    case MachineKind::IIcPlus:     return Machine::AppleIIcPlus;
    }
    return Machine::AppleIIeEnhanced;
}

} // namespace

namespace {

// [B] WOZ 2.1 reference, META chunk: tab-separated key / value lines, and
// requires_machine lists 2|2+|2e|2c|2e+|2gs|2c+|3|3+.
std::uint16_t metaRequiredMachines(std::ifstream& file)
{
    static const std::pair<const char*, std::uint16_t> kNames[] = {
        {"2", woz::kAppleII},         {"2+", woz::kAppleIIPlus},  {"2e", woz::kAppleIIe},
        {"2c", woz::kAppleIIc},       {"2e+", woz::kAppleIIeEnhanced}, {"2gs", woz::kAppleIIgs},
        {"2c+", woz::kAppleIIcPlus},  {"3", woz::kAppleIII},      {"3+", woz::kAppleIIIPlus}};
    file.clear();
    std::streamoff at = 12;
    for (int chunks = 0; chunks < 64; ++chunks) {
        unsigned char head[8];
        file.seekg(at);
        if (!file.read(reinterpret_cast<char*>(head), 8)) return 0;
        const std::uint32_t size = head[4] | (head[5] << 8) | (head[6] << 16) | (std::uint32_t(head[7]) << 24);
        if (std::memcmp(head, "META", 4) == 0) {
            if (size > (1u << 20)) return 0;
            std::string meta(size, '\0');
            if (!file.read(meta.data(), size)) return 0;
            const auto key = meta.find("requires_machine\t");
            if (key == std::string::npos) return 0;
            const auto from = key + 17;
            const auto end = meta.find('\n', from);
            const std::string list = meta.substr(from, end == std::string::npos ? std::string::npos : end - from);
            std::uint16_t bits = 0;
            std::size_t start = 0;
            while (start <= list.size()) {
                auto bar = list.find('|', start);
                if (bar == std::string::npos) bar = list.size();
                std::string name = list.substr(start, bar - start);
                while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
                for (const auto& [text, bit] : kNames)
                    if (name == text) bits |= bit;
                start = bar + 1;
            }
            return bits;
        }
        at += 8 + static_cast<std::streamoff>(size);
    }
    return 0;
}

}  // namespace

std::uint16_t wozCompatibleHardware(const std::filesystem::path& path)
{
    // [B] WOZ 2.1 specification, applesaucefdc.com/woz/reference2/: the INFO
    // chunk sits at offset 12; its data's version is byte 0 and the
    // compatible-hardware word (little-endian) bytes 40-41 from version 2.
    // A WOZ written with INFO version 1 (or a zero word) may still name its
    // machines in META: Genetic Drift and Pest Patrol say ][ and ][+ only
    // there, and failed on the //e Enhanced Automatic otherwise picked.
    std::array<unsigned char, 64> head{};
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(head.data()), head.size())) return 0;
    const bool woz2 = std::memcmp(head.data(), "WOZ2", 4) == 0;
    if ((!woz2 && std::memcmp(head.data(), "WOZ1", 4) != 0) || std::memcmp(head.data() + 12, "INFO", 4) != 0)
        return 0;
    // WOZ 1 has the same META chunk and no hardware word (Genetic Drift).
    const unsigned char* info = head.data() + 20;
    const std::uint16_t bits = woz2 && info[0] >= 2
        ? static_cast<std::uint16_t>((info[40] | (info[41] << 8)) & woz::kKnownHardware) : 0;
    return bits ? bits : static_cast<std::uint16_t>(metaRequiredMachines(file) & woz::kKnownHardware);
}

bool wozBoots13Sector(const std::filesystem::path& path)
{
    // [B] WOZ 2.1 specification: INFO byte 38 is the boot sector format,
    // 2 = 13-sector (DOS 3.1/3.2), 3 = both. Absent before INFO version 2.
    std::array<unsigned char, 64> head{};
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(head.data()), head.size())) return false;
    if (std::memcmp(head.data(), "WOZ2", 4) != 0 || std::memcmp(head.data() + 12, "INFO", 4) != 0)
        return false;
    const unsigned char* info = head.data() + 20;
    return info[0] >= 2 && (info[38] == 2 || info[38] == 3);
}

bool isDixImage(const std::filesystem::path& path)
{
    // The bundled DIX-fix and the reference corpus image have distinct hashes.
    const auto hash = pom2::Core::sha256File(path.string());
    return hash == "e28da6f871e9564fca0665f69d4806b258d0bbaef73ba1f9679a53c56eac9dc4"
        || hash == "e375db9d722454ec1c75dff32386c5d92f15847f8390552acd081a69991e18df";
}

bool chooseMachine(const MachineRequest& request, MachineChoice& choice, std::string& error)
{
    choice = {};
    choice.dix = request.dix == "enabled"
        || (request.dix == "auto" && request.firstDiskIs35 && isDixImage(request.firstDisk));

    const Firmware* picked = nullptr;
    if (choice.dix) {
        picked = &firmwareFor(MachineKind::IIeEnhanced);
    } else if (request.machine != "auto") {
        for (const auto& entry : kFirmware)
            if (request.machine == entry.option) picked = &entry;
        if (!picked) picked = &kFirmware[0];
    } else {
        for (MachineKind kind : request.prefer) {
            if (available(request.firmware, firmwareFor(kind))) {
                picked = &firmwareFor(kind);
                break;
            }
        }
        // A WOZ that names its machines picks the first of them we can run.
        const auto bits = request.firstDisk.empty() ? 0 : wozCompatibleHardware(request.firstDisk);
        // A 13-sector disk that runs on the original ][ asks for it first:
        // the DOS 3.2 era wrote its programs in Integer BASIC, which a ][+
        // only has on a language card ("LANGUAGE NOT AVAILABLE" otherwise;
        // 9 of 19 such woz-a-day titles, 2026-11-24).
        const bool original13 = (bits & woz::kAppleII) && wozBoots13Sector(request.firstDisk);
        std::vector<MachineKind> order{MachineKind::IIeEnhanced, MachineKind::IIe, MachineKind::IIPlus,
                                       MachineKind::II, MachineKind::IIc, MachineKind::IIcPlus};
        if (original13) order.insert(order.begin(), MachineKind::II);
        for (MachineKind kind : order) {
            const auto& entry = firmwareFor(kind);
            if ((bits & entry.wozBit) && available(request.firmware, entry)) {
                picked = &entry;
                break;
            }
        }
        for (MachineKind kind : {MachineKind::IIeEnhanced, MachineKind::IIe, MachineKind::IIc,
                                 MachineKind::IIPlus, MachineKind::IIcPlus, MachineKind::II}) {
            if (picked) break;
            if (request.needsCassette && !hasCassette(kind)) continue;
            if (available(request.firmware, firmwareFor(kind))) picked = &firmwareFor(kind);
        }
        if (!picked) {
            error = "No Apple II firmware found. " + missingMessage(kFirmware[0]);
            return false;
        }
    }
    if (request.needsCassette && !hasCassette(picked->kind)) {
        // [B] The //c and //c Plus have no cassette port.
        error = picked->name + std::string(" has no cassette port: choose another machine to play a tape");
        return false;
    }
    if (!available(request.firmware, *picked)) {
        error = missingMessage(*picked);
        return false;
    }

    choice.kind = picked->kind;
    choice.name = picked->name;
    choice.rom = firstPresent(request.firmware, picked->roms);
    choice.characterRom = firstPresent(request.firmware, picked->characterRoms);
    choice.lowerBank = picked->kind == MachineKind::IIc;

    const bool pal = choice.dix || request.standard == "pal";
    choice.config.timing = pal ? VideoTiming::PAL : VideoTiming::NTSC;
    choice.config.machine = namedMachine(picked->kind);
    if (hasSlots(picked->kind) && !choice.dix && (request.cpu == "nmos" || request.cpu == "cmos")) {
        choice.config.machine = Machine::Custom;
        choice.config.cpu = request.cpu == "cmos" ? CpuModel::CMOS65C02 : CpuModel::NMOS6502;
        choice.config.iieMemory = picked->kind == MachineKind::IIeEnhanced || picked->kind == MachineKind::IIe;
    }
    if (pal) choice.name += " PAL";
    if (choice.dix) choice.name += " (DIX)";
    return true;
}

} // namespace pom2::libretro
