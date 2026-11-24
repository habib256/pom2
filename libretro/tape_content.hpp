// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "machine_profile.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace pom2::libretro {

bool isTapeFile(const std::filesystem::path& path);

// What a program tape holds, read before the machine is built
// (pom2::Core::readCassetteRecords), and how to load it.
struct TapePlan {
    enum class Kind { IntegerBasic, Applesoft, Binary } kind = Kind::Binary;
    std::size_t length = 0;              // the program or dump, in bytes
    std::vector<MachineKind> prefer;     // machines whose ROM loads it, best first
};

// Throws std::runtime_error with the reason: no tape, no record on it.
TapePlan planTape(const std::filesystem::path& path);

// The keys that load and start it at the machine's power-on prompt, or an
// empty string when it cannot be done for the user (a memory dump: the
// tape does not say where it goes). `note` explains either way.
std::string tapeCommand(const TapePlan& plan, MachineKind machine, std::string& note);

} // namespace pom2::libretro
