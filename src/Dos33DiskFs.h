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

// Dos33DiskFs — the DOS 3.3 file system of a 140 KB 5.25" image, so a host
// can list, extract, add and delete files: the DOS 3.3 counterpart of
// CpmDiskFs (pom2_cpm), behind pom2_dos33. Beneath Apple DOS ch. 4:
//
//   VTOC          track 17 sector 0: first catalog sector (+$01/+$02),
//                 T/S pairs per list (+$27, 122), allocation direction
//                 (+$30/+$31), the free-sector bitmap from +$38, four bytes
//                 a track, bit set = free, sector 15 in bit 7 of the first.
//   catalog       a chain of sectors (+$01/+$02), seven 35-byte entries
//                 from +$0B: T/S list track/sector, type (bit 7 locked),
//                 30-byte name (high bit set, space padded), sector count.
//                 Track $FF = deleted (the old track moves to the name's
//                 last byte); track 0 = never used.
//   T/S list      a chain (+$01/+$02) of 122 data-sector pairs from +$0C;
//                 a 0/0 pair is a hole (random-access text files).
//
// File payloads, as `readFile`/`writeFile` see them: T = text up to the
// first $00 (bytes with the high bit set, $8D for a return); I and A = a
// two-byte length, then the program; B = address and length, then the data.

#ifndef POM2_DOS33_DISK_FS_H
#define POM2_DOS33_DISK_FS_H

#include <cstdint>
#include <string>
#include <vector>

namespace pom2::dos33fs {

enum class SectorOrder { Dos, Prodos };

inline constexpr std::size_t kImageBytes = 35u * 16u * 256u;   // 143 360

/// `.po` → ProDOS order; anything else (.dsk, .do) → DOS.
SectorOrder orderForPath(const std::string& path);

/// DOS file types: the catalog byte without its lock bit.
enum class FileType : uint8_t {
    Text = 0x00, Integer = 0x01, Applesoft = 0x02, Binary = 0x04,
    S = 0x08, Relocatable = 0x10, A2 = 0x20, B2 = 0x40,
};
/// "T", "I", "A", "B", "S", "R", "a", "b" — CATALOG's letters.
char typeLetter(uint8_t typeByte);

struct FileInfo {
    std::string name;      // trailing spaces trimmed
    uint8_t     type = 0;  // catalog byte, lock bit stripped
    bool        locked = false;
    int         sectors = 0;   // the catalog's count (T/S lists included)
};

class Dos33Disk
{
public:
    /// False (with `error`) unless the image is 143 360 bytes and its VTOC
    /// reads as a 35-track, 16-sector DOS 3.3 volume.
    bool open(std::vector<uint8_t> image, SectorOrder order, std::string& error);
    const std::vector<uint8_t>& image() const { return image_; }
    int volume() const;
    int freeSectors() const;

    std::vector<FileInfo> list() const;

    /// The file's data, sector by sector, holes as zeros, the T/S lists'
    /// own sectors excluded. `payload` trims it to what the type says is in
    /// it (see the header note); `loadAddress` gets a B file's address.
    bool readFile(const std::string& name, std::vector<uint8_t>& raw,
                  std::string& error) const;
    static std::vector<uint8_t> payload(uint8_t type, const std::vector<uint8_t>& raw,
                                        uint16_t* loadAddress = nullptr);

    /// Creates or replaces `name` (a locked file is refused). `data` is the
    /// PAYLOAD: the length / address header is built here. All-or-nothing:
    /// on failure the image is unchanged.
    bool writeFile(const std::string& name, FileType type,
                   const std::vector<uint8_t>& data, uint16_t loadAddress,
                   std::string& error);
    bool removeFile(const std::string& name, std::string& error);

    /// Host text → DOS text: '\n' (and "\r\n") → $8D, every byte | $80.
    static std::vector<uint8_t> textToDos(const std::vector<uint8_t>& host);
    /// DOS text → host text: high bit off, $0D → '\n'.
    static std::vector<uint8_t> textFromDos(const std::vector<uint8_t>& dos);

private:
    uint8_t* sector(int track, int sec);
    const uint8_t* sector(int track, int sec) const;
    struct Entry { int track, sec, offset; };   // a catalog slot
    bool findEntry(const std::string& name, Entry& at) const;
    bool isFree(int track, int sec) const;
    void setFree(int track, int sec, bool free);
    /// Data sectors and T/S list sectors of the file whose list starts at
    /// (t, s), in order. False on a broken chain.
    bool chain(int t, int s, std::vector<std::pair<int, int>>& lists,
               std::vector<std::pair<int, int>>& data) const;

    std::vector<uint8_t> image_;
    SectorOrder order_ = SectorOrder::Dos;
};

} // namespace pom2::dos33fs

#endif // POM2_DOS33_DISK_FS_H
