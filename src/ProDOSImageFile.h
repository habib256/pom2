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

// ProDOSImageFile — a disk-image FILE as the ProDOS volume inside it, and
// back (pom2_prodos's container layer).
//
//   .po / .hdv        block order, the bytes are the volume
//   .dsk / .do 140 KB DOS sector order, converted both ways
//   .2mg (2IMG)       any extension: the "2IMG" magic decides. [B] 2IMG
//                     specification (DiskImage_2MG_Info.txt): header length
//                     at $08, image format at $0C (0 = DOS order, 1 = ProDOS
//                     order, 2 = nibbles), flags at $10 (bit 31 locked —
//                     TwoImg.h), data offset at $18, data length at $1C
//                     (TwoImgHeader.h repairs Sweet-16's zero length).
//
// A 2IMG is rewritten with everything around its payload kept byte for
// byte — the header, the comment and creator chunks after the data — since
// an edit inside a volume never changes its size. A nibble 2IMG holds no
// blocks and is refused; a LOCKED one is read and refused for writing, the
// same rule as the host read-only bit (MediaNotch.h).

#ifndef POM2_PRODOS_IMAGE_FILE_H
#define POM2_PRODOS_IMAGE_FILE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2::prodosfs {

struct ImageFile {
    enum class Kind { Blocks, DosOrder, TwoImg };
    Kind kind = Kind::Blocks;
    bool twoImgDosOrder = false;   // a 2IMG whose payload is in DOS order
    bool locked = false;           // the 2IMG lock bit
    std::size_t dataOffset = 0;    // 2IMG: where the payload starts
    std::size_t dataLength = 0;
    std::vector<uint8_t> container;   // 2IMG: the whole file, payload included
};

/// The volume's blocks from a file's bytes. `extension` is lowercase with
/// its dot (".dsk"); a 2IMG is recognised by its magic whatever it says.
/// False with `error` for a damaged 2IMG or a nibble one.
bool readImageFile(const std::vector<uint8_t>& file, const std::string& extension,
                   ImageFile& layout, std::vector<uint8_t>& blocks, std::string& error);

/// The file's bytes for `blocks` (as `readImageFile` returned them, edited
/// in place), in the layout it came from. Empty when a 2IMG's volume no
/// longer has its payload's size (nothing in ProDOSDisk resizes one).
std::vector<uint8_t> writeImageFile(const ImageFile& layout, const std::vector<uint8_t>& blocks);

} // namespace pom2::prodosfs

#endif // POM2_PRODOS_IMAGE_FILE_H
