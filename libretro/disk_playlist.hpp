// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pom2::libretro {

struct DiskEntry {
    std::filesystem::path path;
    std::string label;
    bool smartPort() const;
};

DiskEntry loadDiskImage(const std::filesystem::path& path);

// Returns an image or the entries of an M3U playlist in order. Paths are
// absolute, and an unreadable or unsupported entry fails the entire list.
std::vector<DiskEntry> loadDiskList(const std::filesystem::path& content);

} // namespace pom2::libretro
