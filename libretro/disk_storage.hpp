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
// `variant` > 0 names the copy "<disk> (<variant + 1>)": a playlist's second
// disk of the same file name keeps a copy of its own.
DiskStorage openDiskStorage(const std::filesystem::path& saves,
                            const std::filesystem::path& content,
                            const std::filesystem::path& original,
                            std::string& error,
                            DiskStorageAccess access = DiskStorageAccess::CreateIfMissing,
                            unsigned variant = 0);

// Where a core before 2026-11-25 kept this disk's copy (hashes for names):
// what a save state of that time names its disk by. Empty when unknown.
std::filesystem::path legacyStoragePath(const std::filesystem::path& saves,
                                        const std::filesystem::path& content,
                                        const std::filesystem::path& original);

} // namespace pom2::libretro
