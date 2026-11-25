// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <memory>

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
// when no save directory is available or the source is notched.
DiskStorage openDiskStorage(const std::filesystem::path& saves,
                            const std::filesystem::path& content,
                            const std::filesystem::path& original,
                            DiskStorageAccess access = DiskStorageAccess::CreateIfMissing);

} // namespace pom2::libretro
