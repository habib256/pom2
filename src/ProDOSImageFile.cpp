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

// ProDOSImageFile — see the header.

#include "ProDOSImageFile.h"

#include "ProDOSDiskFs.h"
#include "TwoImg.h"
#include "TwoImgHeader.h"

#include <algorithm>
#include <cstring>

namespace pom2::prodosfs {

namespace {

constexpr std::size_t kTwoImgMinHeader = 64;

uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

} // namespace

bool readImageFile(const std::vector<uint8_t>& file, const std::string& extension,
                   ImageFile& layout, std::vector<uint8_t>& blocks, std::string& error)
{
    layout = {};
    if (file.size() >= 4 && std::memcmp(file.data(), "2IMG", 4) == 0) {
        if (file.size() < kTwoImgMinHeader) { error = "2IMG header cut short"; return false; }
        const uint32_t format = rd32(&file[0x0C]);
        const uint32_t flags  = rd32(&file[0x10]);
        const uint32_t offset = rd32(&file[0x18]);
        const uint32_t length = twoImgDataLength(file.data());
        if (format == 2) { error = "a nibble 2IMG holds no ProDOS blocks"; return false; }
        if (format > 2)  { error = "2IMG image format " + std::to_string(format) + " is not one the spec defines"; return false; }
        if (offset < kTwoImgMinHeader || offset > file.size() || length > file.size() - offset) {
            error = "2IMG data offset/length point outside the file";
            return false;
        }
        layout.kind = ImageFile::Kind::TwoImg;
        layout.twoImgDosOrder = format == 0;
        layout.locked = twoImgWriteProtected(flags);
        layout.dataOffset = offset;
        layout.dataLength = length;
        layout.container = file;
        blocks.assign(file.begin() + offset, file.begin() + offset + length);
        if (layout.twoImgDosOrder) {
            if (blocks.size() != kDiskette140Bytes) {
                error = "a DOS-order 2IMG must hold a 140 KB disk";
                return false;
            }
            blocks = dosOrderToBlockOrder(blocks);
        }
        return true;
    }
    if ((extension == ".dsk" || extension == ".do") && file.size() == kDiskette140Bytes) {
        layout.kind = ImageFile::Kind::DosOrder;
        blocks = dosOrderToBlockOrder(file);
        return true;
    }
    layout.kind = ImageFile::Kind::Blocks;
    blocks = file;
    return true;
}

std::vector<uint8_t> writeImageFile(const ImageFile& layout, const std::vector<uint8_t>& blocks)
{
    switch (layout.kind) {
        case ImageFile::Kind::Blocks:   return blocks;
        case ImageFile::Kind::DosOrder: return blockOrderToDosOrder(blocks);
        case ImageFile::Kind::TwoImg: {
            std::vector<uint8_t> out = layout.container;
            const std::vector<uint8_t> payload =
                layout.twoImgDosOrder ? blockOrderToDosOrder(blocks) : blocks;
            // An in-place edit keeps the volume's size; anything else would
            // move the chunks after the payload: refused (empty result).
            if (payload.size() != layout.dataLength) return {};
            std::copy(payload.begin(), payload.end(),
                      out.begin() + static_cast<std::ptrdiff_t>(layout.dataOffset));
            return out;
        }
    }
    return blocks;
}

} // namespace pom2::prodosfs
