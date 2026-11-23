// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pom2::libretro {

bool isProgramFile(const std::filesystem::path& path);

// A program loaded straight into RAM at the BASIC prompt, then started by
// typing `command` (RUN, or CALL to the load address).
struct ProgramImage {
    std::uint16_t address = 0;
    std::vector<std::uint8_t> bytes;
    bool applesoft = false;   // set the Applesoft program pointers
    std::string command;
};

// A .bas is an Applesoft listing, tokenized as the ROM's PARSE would. A .bin
// loads where its name says (CiderPress's NAME#06AAAA), where its AppleSingle
// or DOS 3.3 header says, or at $0803 (cc65's start address). Throws
// std::runtime_error with the reason.
ProgramImage loadProgram(const std::filesystem::path& path);

} // namespace pom2::libretro
