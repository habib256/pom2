// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

namespace pom2::libretro {

bool isArchive(const std::filesystem::path& path);

// Unpack a .zip or .gz into `cacheRoot`, in a folder named after the
// archive's absolute path, so the same archive always lands at the same
// paths and its disks keep their private copies across sessions. Returns the
// content to load: the archive's own .m3u, a playlist written for several
// disk images (in name order), or its single disk image or program.
// Throws std::runtime_error with the reason.
std::filesystem::path unpackArchive(const std::filesystem::path& archive,
                                    const std::filesystem::path& cacheRoot);

} // namespace pom2::libretro
