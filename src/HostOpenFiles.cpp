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

// The portable half of HostOpenFiles.h: the queue. The macOS hook that feeds
// it lives in HostOpenFiles_mac.mm; every other host gets the no-op install.

#include "HostOpenFiles.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pom2 {
namespace {

std::mutex&               queueMutex() { static std::mutex m; return m; }
std::vector<std::string>& queue()      { static std::vector<std::string> q; return q; }

} // namespace

void pushHostOpenFile(std::string path)
{
    if (path.empty()) return;
    std::lock_guard<std::mutex> lk(queueMutex());
    queue().push_back(std::move(path));
}

std::vector<std::string> takeHostOpenFiles()
{
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(queueMutex());
    out.swap(queue());
    return out;
}

#ifndef __APPLE__
void installHostOpenFilesHandler() {}
#endif

} // namespace pom2
