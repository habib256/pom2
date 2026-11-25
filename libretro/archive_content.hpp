// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <map>
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
struct ArchiveNotes {
    /// For each file written under another name than libretro v1.2.3 gave
    /// it: where v1.2.3 put it. Its disk saves are named from that path.
    std::map<std::filesystem::path, std::filesystem::path> legacy;
    /// For a member whose name another member shares but for its case: the
    /// first of them in the zip, the one file v1.2.3 had for them all on a
    /// case-blind volume (Windows, macOS).
    std::map<std::filesystem::path, std::filesystem::path> legacyTwin;
    /// Something left out that the player should know (a disk past the
    /// 64th, a member that could not be read), when the load succeeds.
    std::string notice;
};
std::filesystem::path unpackArchive(const std::filesystem::path& archive,
                                    const std::filesystem::path& cacheRoot, std::string& error,
                                    ArchiveNotes* notes = nullptr);

} // namespace pom2::libretro
