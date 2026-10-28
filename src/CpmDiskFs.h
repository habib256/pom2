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

// CpmDiskFs — the CP/M 2.2 file system of an Apple II 5.25" image, so a
// host can list, extract, add and delete files without cpmtools.
//
// Geometry: the Microsoft SoftCard format, which the ALS CP/M Card's 16-
// sector disks share (cpmtools `apple-do`):
//
//   35 tracks × 16 sectors × 256 bytes, 3 reserved (system) tracks,
//   1 KB blocks, 64 directory entries in blocks 0-1, 8-bit block numbers
//   (16 per extent, EXM = 0, 128 records per extent).
//
// CP/M's logical 256-byte sector n of a track lives in DOS 3.3 physical
// sector kSkew[n] (cpmtools `skewtab 0,6,12,3,9,15,14,5,11,2,8,7,13,4,10,1`;
// read back off cpm22.dsk, whose directory sits in DOS sectors 0/6/12 of
// track 3). A ProDOS-order image (.po) stores the same physical sectors in
// another order; `kDosToProdos` maps one to the other.
//
// The data area is 32 tracks = 128 blocks. The 56K/60K sysgens go further:
// cpm60k.dsk carries a user-31 "cp/m.sys" entry on blocks 128-139, whose
// track number wraps past 34 onto tracks 0-2 — the system tracks seen
// through the file system. Reads follow that wrap; writes never allocate
// past block 127, and every block any entry names (user 0-31) is in use.

#ifndef POM2_CPM_DISK_FS_H
#define POM2_CPM_DISK_FS_H

#include <cstdint>
#include <string>
#include <vector>

namespace pom2::cpmfs {

enum class SectorOrder { Dos, Prodos };

inline constexpr std::size_t kImageBytes   = 35u * 16u * 256u;   // 143 360
inline constexpr int         kReservedTracks = 3;
inline constexpr int         kBlockBytes   = 1024;
inline constexpr int         kDirEntries   = 64;
inline constexpr int         kDataBlocks   = 128;   // allocatable: 0..127
inline constexpr int         kMaxReadBlock = 139;   // 60K system wrap

/// DOS physical sector holding CP/M logical sector n of a track.
inline constexpr uint8_t kSkew[16] = {
    0, 6, 12, 3, 9, 15, 14, 5, 11, 2, 8, 7, 13, 4, 10, 1 };
/// Position in a ProDOS-order track of DOS physical sector d.
inline constexpr uint8_t kDosToProdos[16] = {
    0, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 15 };

/// `.po` → ProDOS order; anything else (.dsk, .do) → DOS order.
SectorOrder orderForPath(const std::string& path);

struct FileInfo {
    int         user = 0;
    std::string name;          // "NAME.EXT", upper case, no padding
    uint32_t    bytes = 0;     // records × 128 (CP/M 2.2 has no byte count)
    int         blocks = 0;
    bool        readOnly = false;
    bool        system = false;
};

class CpmDisk
{
public:
    /// Takes an image already in memory. False (with `error`) unless it is
    /// exactly 143 360 bytes and its directory looks like CP/M's.
    bool open(std::vector<uint8_t> image, SectorOrder order, std::string& error);

    const std::vector<uint8_t>& image() const { return image_; }

    /// Files of user areas 0-15, directory order, one entry per file.
    std::vector<FileInfo> list() const;
    int freeBlocks() const;

    /// `name` is "NAME.EXT" (case-insensitive). False when absent.
    bool readFile(int user, const std::string& name,
                  std::vector<uint8_t>& out, std::string& error) const;
    /// Creates or replaces. The data is padded with ^Z ($1A) to a whole
    /// 128-byte record, as CP/M itself does. All-or-nothing: on failure the
    /// image is unchanged.
    bool writeFile(int user, const std::string& name,
                   const std::vector<uint8_t>& data, std::string& error);
    bool removeFile(int user, const std::string& name, std::string& error);

    /// "bterm.com" → "BTERM   COM" (11 bytes). False for a name CP/M
    /// cannot hold: empty, > 8.3, or a character CCP treats as a delimiter.
    static bool toDirName(const std::string& name, std::string& out11);

private:
    uint8_t* sector(int track, int dosSector);
    const uint8_t* sector(int track, int dosSector) const;
    void readBlock(int block, uint8_t* out1k) const;
    void writeBlock(int block, const uint8_t* in1k);
    void loadDirectory();
    void storeDirectory();
    std::vector<bool> usedBlocks() const;
    std::vector<int> entriesOf(int user, const std::string& name11) const;

    std::vector<uint8_t> image_;
    SectorOrder order_ = SectorOrder::Dos;
    uint8_t dir_[kDirEntries][32] = {};
};

} // namespace pom2::cpmfs

#endif // POM2_CPM_DISK_FS_H
