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

#include "ProDOSHostNameMap.h"

#include <mutex>

namespace fs = std::filesystem;

namespace pom2 {

namespace {
std::mutex registryMutex;
std::map<std::string, fs::file_time_type> scanStarts;                  // folder -> last build's start
std::map<std::string, std::shared_ptr<const HostNameMap>> nameMaps;   // folder -> last build's names
}  // namespace

std::string hostNameMapKey(const fs::path& dir)
{
    std::string k = dir.lexically_normal().generic_string();
    while (k.size() > 1 && k.back() == '/') k.pop_back();
    return k;
}

std::uint64_t hostNameMapHash(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::uint8_t c : bytes) { h ^= c; h *= 0x100000001B3ull; }
    return h;
}

void recordFolderScanStart(const std::string& hostFolder)
{
    std::lock_guard<std::mutex> lk(registryMutex);
    scanStarts[hostFolder] = fs::file_time_type::clock::now();
}

void recordFolderNameMap(const std::string& hostFolder, std::shared_ptr<const HostNameMap> map)
{
    std::lock_guard<std::mutex> lk(registryMutex);
    nameMaps[hostFolder] = std::move(map);
}

std::optional<fs::file_time_type> takeFolderScanStart(const std::string& hostFolder)
{
    std::lock_guard<std::mutex> lk(registryMutex);
    const auto it = scanStarts.find(hostFolder);
    if (it == scanStarts.end()) return std::nullopt;
    const auto t = it->second;
    scanStarts.erase(it);
    return t;
}

std::shared_ptr<const HostNameMap> takeFolderNameMap(const std::string& hostFolder)
{
    std::lock_guard<std::mutex> lk(registryMutex);
    const auto it = nameMaps.find(hostFolder);
    if (it == nameMaps.end()) return nullptr;
    auto m = std::move(it->second);
    nameMaps.erase(it);
    return m;
}

}  // namespace pom2
