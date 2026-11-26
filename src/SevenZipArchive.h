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

// SevenZipArchive — read the members of a .7z archive held in memory.
//
// The desktop (MediaSource.h) and the libretro core (archive_content.hpp)
// open a .7z as they open a .zip: the disks inside become local images. The
// decoder is Igor Pavlov's LZMA SDK (src/third_party/lzma, public domain):
// LZMA, LZMA2, PPMd, the branch filters, and "copy". An encrypted archive,
// or one packed with Deflate / Deflate64 / BZip2, is refused as unsupported.
//
// Nothing here trusts the archive. A 7z decodes a whole solid block at once,
// and its header says how large that block is, so every block is checked
// against `maxFolderBytes` before anything is decoded, and the SDK allocates
// through an allocator with a ceiling (a PPMd header names its own model
// size, up to 4 GiB; POM2 allows 256 MiB, above 7-Zip's largest preset).
// Nothing throws on bad content: the libretro core runs in RetroArch's web
// build, where a throw ends the page.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pom2 {

/// The bytes start with the 7z signature ('7' 'z' BC AF 27 1C).
bool isSevenZip(const std::vector<std::uint8_t>& head);

class SevenZipArchive {
public:
    struct Entry {
        std::string   name;             ///< UTF-8, '/' between folders
        std::uint64_t size = 0;
        bool          directory = false;
    };

    SevenZipArchive();
    ~SevenZipArchive();
    SevenZipArchive(const SevenZipArchive&) = delete;
    SevenZipArchive& operator=(const SevenZipArchive&) = delete;

    /// Read the archive's directory. `bytes` is kept by reference and must
    /// outlive this reader. False with `err` set when the bytes are not a
    /// 7z, are damaged, hold more than `maxEntries` names, or hold a block
    /// that unpacks to more than `maxFolderBytes`.
    bool open(const std::vector<std::uint8_t>& bytes, std::string& err,
              std::size_t maxFolderBytes, std::size_t maxEntries = 65536);

    /// Every entry, in archive order (directories included).
    const std::vector<Entry>& entries() const { return entries_; }

    /// Entry `index` into `out`. False with `err` set when it is larger than
    /// `maxBytes`, fails its CRC, or is packed with a method this decoder
    /// does not have (`unsupported` set then, as `readZipMember` does). The
    /// last block decoded is kept, so reading the members of a solid block in
    /// order decodes it once.
    bool extract(std::size_t index, std::vector<std::uint8_t>& out, std::string& err,
                 std::size_t maxBytes, bool* unsupported = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<Entry> entries_;
};

}  // namespace pom2
