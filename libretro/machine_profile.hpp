// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pom2/core.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace pom2::libretro {

enum class MachineKind { IIeEnhanced, IIe, IIPlus, II, IIc, IIcPlus };

struct MachineRequest {
    std::string_view machine = "auto";    // pom2_machine
    std::string_view standard = "auto";   // pom2_video_standard
    std::string_view cpu = "auto";        // pom2_cpu
    std::string_view dix = "auto";        // pom2_dix_preset
    std::filesystem::path firmware;       // system/pom2
    std::filesystem::path firstDisk;      // empty without content
    bool firstDiskIs35 = false;
};

struct MachineChoice {
    MachineKind kind = MachineKind::IIeEnhanced;
    pom2::CoreConfig config;
    std::filesystem::path rom;
    std::filesystem::path characterRom;
    bool lowerBank = false;   // a //c 32 KiB dump: bank 0 at reset
    bool dix = false;
    std::string name;         // for the frontend's notification
};

// The machine to build, or false with `error` naming the firmware it needed.
bool chooseMachine(const MachineRequest& request, MachineChoice& choice, std::string& error);

// WOZ 2 INFO "compatible hardware" bits (pom2::woz::Hardware), 0 when the
// file is not a WOZ 2 or names none.
std::uint16_t wozCompatibleHardware(const std::filesystem::path& path);

bool isDixImage(const std::filesystem::path& path);

constexpr bool hasSlots(MachineKind kind)
{
    return kind != MachineKind::IIc && kind != MachineKind::IIcPlus;
}

} // namespace pom2::libretro
