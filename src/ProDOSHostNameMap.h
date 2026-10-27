// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

// What a build of a ProDOS host folder hands the mount it was built for
// (ProDOSVolume.h): when its scan started, and which host file each ProDOS
// name came from, with a hash of the bytes the guest was served. Both are
// recorded per folder by `buildVolumeFromFolder` and taken once by
// `Block512Backing::loadFromBytes`. The write-back compares against them
// rather than against the folder as it stands at eject (STORAGE-014 #1, #3).

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pom2 {

struct HostNameMap {
    struct Entry {
        std::string   leaf, prodosName;
        bool          isDir = false, skipped = false, hashed = false;
        std::uint8_t  fileType = 0;
        std::uint16_t auxType  = 0;
        std::uint64_t hash     = 0;   // the bytes the guest was served
    };
    std::map<std::string, std::vector<Entry>> dirs;   // key: hostNameMapKey(host dir)
};

/// The key a host directory is filed under: lexically normal, no trailing '/'.
std::string hostNameMapKey(const std::filesystem::path& dir);
/// FNV-1a 64 over a served file's bytes.
std::uint64_t hostNameMapHash(const std::vector<std::uint8_t>& bytes);

void recordFolderScanStart(const std::string& hostFolder);
void recordFolderNameMap(const std::string& hostFolder, std::shared_ptr<const HostNameMap> map);
std::optional<std::filesystem::file_time_type> takeFolderScanStart(const std::string& hostFolder);
std::shared_ptr<const HostNameMap> takeFolderNameMap(const std::string& hostFolder);

}  // namespace pom2
