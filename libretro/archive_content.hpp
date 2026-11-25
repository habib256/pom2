// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>

namespace pom2::libretro {

bool isArchive(const std::filesystem::path& path);

// Unpack a .zip, a .gz, a ShrinkIt archive (.shk .sdk .bxy .sea) or a
// DiskCopy image (.dc .dc42 .image) or an A2R flux capture into `cacheRoot`, in a folder named after
// the archive's absolute path, so the same archive always lands at the same
// paths and its disks keep their private copies across sessions. Returns the
// content to load: the archive's own .m3u, a playlist written for several
// disk images (in name order), or its single disk image or program. A
// ShrinkIt archive of files becomes a ProDOS data volume (ExchangeMedia.h).
// A zip, a .gz, a ShrinkIt archive or a DiskCopy image unpacked into
// `cacheRoot`: the disk to load, or a playlist of them. Never throws: a
// failure leaves `error` set and returns an empty path (RetroArch's web build
// links with exception catching off, where a throw ends the page).
std::filesystem::path unpackArchive(const std::filesystem::path& archive,
                                    const std::filesystem::path& cacheRoot, std::string& error);

} // namespace pom2::libretro
