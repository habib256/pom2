// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace pom2::libretro {

struct DiskStorage {
    std::filesystem::path path;
    bool privateCopy = false;
};

enum class DiskStorageAccess { CreateIfMissing, ExistingOnly };

class DiskSaveLock;

// The lock follows the backing across atomic file replacements. A second
// instance can read this backing, and receives no lease to write to it.
std::shared_ptr<DiskSaveLock> lockDiskStorage(const std::filesystem::path& backing);

// Resolves a single disk side to its private writable backing,
// <saves>/pom2/disks/<content name>/<disk file name>, or to the original
// when no save directory is available or the source is notched. Never
// throws: a failure returns an empty path with `error` set (RetroArch's web
// build links with exception catching off, where a throw ends the page).
// A copy is found by the image and by where it sits in the content's folder
// (its .source record), so a playlist's two blank SAVE.DSK keep two copies,
// "SAVE.DSK" and "SAVE (2).DSK", whatever the playlist's order. `title`
// names the game's folder when it is not the content's (an archive's name
// for the disks unpacked from it).
DiskStorage openDiskStorage(const std::filesystem::path& saves,
                            const std::filesystem::path& content,
                            const std::filesystem::path& original,
                            std::string& error,
                            DiskStorageAccess access = DiskStorageAccess::CreateIfMissing,
                            const std::filesystem::path& title = {});

// Where a core before 2026-11-25 kept this disk's copy (hashes for names):
// what a save state of that time names its disk by. Empty when unknown.
std::filesystem::path legacyStoragePath(const std::filesystem::path& saves,
                                        const std::filesystem::path& content,
                                        const std::filesystem::path& original);

} // namespace pom2::libretro
