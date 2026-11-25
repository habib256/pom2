// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
// A host path as the POM2 SDK takes it. The SDK opens files through narrow
// std::string paths (std::ifstream, std::fopen); on Windows those are read
// in the ANSI code page, so a path outside it (a Japanese name on a Western
// Windows, any accent under MinGW, whose path::string() is UTF-8) cannot be
// opened — and MSVC's path::string() throws on one (bug hunt 2026-11-25).
#pragma once

#include <filesystem>
#include <string>

namespace pom2::libretro {

/// The path as the SDK's narrow file APIs open it: the ANSI code page on
/// Windows, else the 8.3 short name when the volume keeps one; UTF-8
/// elsewhere. Empty when Windows cannot name the file in narrow characters.
std::string sdkPath(const std::filesystem::path& path);

} // namespace pom2::libretro
