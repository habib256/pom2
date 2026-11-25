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
// `busy`: set when the lock exists and another holder has it (worth a wait),
// as opposed to a lock that cannot be made.
std::shared_ptr<DiskSaveLock> lockDiskStorage(const std::filesystem::path& backing, bool* busy = nullptr);

// Resolves a single disk side to its private writable backing,
// <saves>/pom2/disks/<content name>/<disk file name>, or to the original
// when no save directory is available or the source is notched. Never
// throws: a failure returns an empty path with `error` set (RetroArch's web
// build links with exception catching off, where a throw ends the page).
// What names a disk's copies besides its own path. `title`: the game's
// folder when it is not the content's name (an archive's, for the disks
// unpacked from it). `legacyContent` / `legacyOriginal`: where libretro
// v1.2.3 had unpacked the content and this disk, when under other names
// than today's, so its saves are found.
struct StorageNames {
    std::filesystem::path title, legacyContent, legacyOriginal;
    /// A second place v1.2.3 may have had the disk (a case-only twin).
    std::filesystem::path legacyOriginalAlt;
};

// A copy is found by the image and by where it sits in the content's folder
// (its .source record), so a playlist's two blank SAVE.DSK keep two copies,
// "SAVE.DSK" and "SAVE (2).DSK", whatever the playlist's order.
DiskStorage openDiskStorage(const std::filesystem::path& saves,
                            const std::filesystem::path& content,
                            const std::filesystem::path& original,
                            std::string& error,
                            DiskStorageAccess access = DiskStorageAccess::CreateIfMissing,
                            const StorageNames& names = {});

// Where a core before 2026-11-25 kept this disk's copy (hashes for names):
// what a save state of that time names its disk by. Empty when unknown.
std::filesystem::path legacyStoragePath(const std::filesystem::path& saves,
                                        const std::filesystem::path& content,
                                        const std::filesystem::path& original,
                                        const StorageNames& names = {});

} // namespace pom2::libretro
