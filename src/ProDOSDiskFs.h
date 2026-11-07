// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProDOSDiskFs — the ProDOS file system of a block-ordered disk image, read
// and modified IN PLACE: the ProDOS counterpart of Dos33DiskFs and CpmDiskFs,
// and the library behind `pom2_prodos` (ls / get / put / rm / mkdir / new).
//
// It is not ProDOSVolume. That one synthesises a whole volume from a host
// folder and decodes one back; this one edits an existing volume the way the
// ProDOS MLI would, leaving every block it does not need to touch alone, so
// it works on a disk ProDOS (or anything else) wrote.
//
// What it has to agree with is what ProDOS itself reads. The layout, from
// the ProDOS 8 Technical Reference Manual (Apple, 1985), Appendix B
// "File Organization" — read from the published text, not recalled:
//
//   [B] TRM § B.1   blocks 0-1 loader, block 2 the volume directory's key
//                   block, the directory (typically 4 blocks) then the bit
//                   map, one block per 4 096 blocks "or fraction thereof".
//   [B] TRM § B.2.1 every directory block opens with prev/next block
//                   pointers, low byte first; 0 ends the chain both ways.
//   [B] TRM § B.2.2 volume directory header at block offset $04 (Fig. B-3):
//                   storage $F + name length, name, 8 reserved bytes,
//                   creation, version, min_version, access, entry_length
//                   $27, entries_per_block $0D, file_count, bit_map_pointer,
//                   total_blocks. Bit map: 1 = free, block 0 = bit 7 of the
//                   first byte.
//   [B] TRM § B.2.3 subdirectory header (Fig. B-4): storage $E, the same
//                   fields up to file_count, then parent_pointer,
//                   parent_entry_number, parent_entry_length $27.
//   [B] TRM § B.2.4 file entry (Fig. B-5): storage + name length, name,
//                   file_type, key_pointer, blocks_used (index blocks
//                   included), 3-byte EOF, creation, version, min_version,
//                   access, aux_type, last_mod, header_pointer.
//   [B] TRM § B.3.2-B.3.4 seedling (EOF <= $200: the key block IS the data),
//                   sapling (EOF <= $20000: key = index block of up to 256
//                   data pointers), tree (key = master index of up to 128
//                   index blocks).
//   [B] TRM § B.3.7 an index block holds the LOW bytes of its pointers at
//                   offsets 0-255 and the high bytes at "this offset plus
//                   256". (prodos8.com's transcription of § B.3.3 says
//                   "bytes 0 - 127 / 128 - 255": that sentence is not in
//                   Apple's 1985 text and is wrong — 256 pointers need 512
//                   bytes.)
//   [B] TRM § B.3.6 sparse files: a zero pointer reads as 512 zero bytes.
//   [B] TRM § 2.1   names: up to 15 characters, A-Z 0-9 '.', first a letter,
//                   lower case converted to upper case.
//
// The image may be hostile: a user-supplied file or a volume a guest program
// scribbled on. Every block number read from it is range-checked, every
// chain walk is bounded and detects cycles, and every mutator first walks
// the WHOLE volume (`verify`) — a directory, index or data block claimed
// twice, outside the volume, or claimed by a file while the bit map marks it
// free makes the write refuse rather than risk overwriting someone's data.
// Mutators are all-or-nothing: on any failure the image is byte-identical
// to what it was.
//
// No exceptions, no I/O, no global state, no host clock: timestamps come
// from `setTimestamp` (default 0 = "no date", TRM § B.4.2.2 / Beneath Apple
// ProDOS p. 4-9 "zero if no date was assigned").

#ifndef POM2_PRODOS_DISK_FS_H
#define POM2_PRODOS_DISK_FS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2::prodosfs {

inline constexpr std::size_t kBlockBytes = 512;
/// A 5.25" disk: 35 tracks x 16 sectors x 256 bytes = 280 blocks.
inline constexpr std::size_t kDiskette140Bytes = 143360;

struct Entry {
    std::string name;            // as stored, upper case
    uint8_t  storageType = 0;    // 1 seedling, 2 sapling, 3 tree, $D directory (others reported, not followed)
    uint8_t  fileType    = 0;
    uint16_t auxType     = 0;
    uint32_t eof         = 0;
    uint16_t blocksUsed  = 0;
    uint8_t  access      = 0;
    bool     isDirectory = false;
    uint16_t keyPointer  = 0;
    uint16_t createDate  = 0, createTime = 0;   // TRM § B.4.2.2 packed form
    uint16_t modDate     = 0, modTime    = 0;
    /// Write-disabled per the access byte. [B] TRM § B.4.2.3: bit 1 is
    /// Write-Enable; "1 = enabled".
    bool     locked() const { return (access & 0x02) == 0; }
};

/// A legal ProDOS name ([B] TRM § 2.1). `folded` gets the upper-case form;
/// `why` a reason when it is refused.
bool isValidName(const std::string& name, std::string* folded = nullptr,
                 std::string* why = nullptr);

/// [B] TRM § B.4.2.2 Fig. B-13 date word (yyyyyyy mmmm ddddd) and time word
/// (000hhhhh 00mmmmmm). The 7-bit year is year % 100 for 1940-2039
/// ([B] ProDOS 8 Technical Note #28). 0 when out of range.
uint16_t packDate(int year, int month, int day);
uint16_t packTime(int hour, int minute);

class ProDOSDisk
{
public:
    /// A block-ordered image: size a multiple of 512, a valid volume
    /// directory header at block 2, total_blocks no larger than the image.
    /// Refuses (with a reason) anything else. A 32 MiB image (65 536 blocks)
    /// is accepted: total_blocks is 16-bit, so its last block is simply not
    /// part of the volume.
    bool open(std::vector<uint8_t> image, std::string& error);

    /// A fresh, empty volume: blocks 0-1 zero (no boot code — Apple's boot
    /// block is Apple's), a 4-block volume directory at 2-5, the bit map from
    /// block 6. `blocks` 280 for a 5.25" disk, 1600 for an 800 KB one, up to
    /// 65535. Empty vector + `error` on a bad name or size.
    static std::vector<uint8_t> format(uint32_t blocks, const std::string& volumeName,
                                       std::string& error,
                                       uint16_t date = 0, uint16_t time = 0);

    const std::vector<uint8_t>& image() const { return image_; }
    std::string volumeName() const;
    uint32_t    totalBlocks() const { return totalBlocks_; }
    uint32_t    freeBlocks() const;            // counted from the bit map

    /// Creation / modification stamp for what the mutators create or change
    /// (TRM § B.4.2.2 packed words). Default 0 = no date.
    void setTimestamp(uint16_t date, uint16_t time) { date_ = date; time_ = time; }

    /// `dirPath`: "" or "/" is the volume directory; otherwise "DIR" or
    /// "DIR/SUB" (a leading "/VOLNAME/" is accepted and checked). Every
    /// active entry of every block of the chain is listed, in on-disk order.
    /// Path components are matched case-insensitively. A name that is not
    /// legal ProDOS but is ON the disk (A2osX's `.PROFILE`) can be named in
    /// a path to read, replace or remove it; only a name being CREATED must
    /// pass `isValidName`.
    bool list(const std::string& dirPath, std::vector<Entry>& out, std::string& error) const;

    /// Sparse blocks read as zeros.
    bool readFile(const std::string& path, std::vector<uint8_t>& data, Entry* info,
                  std::string& error) const;

    /// Create, or replace an existing FILE of that name (never a directory;
    /// a write-disabled file is refused). Chooses seedling / sapling / tree
    /// by size and allocates the way a sequential ProDOS WRITE does (first
    /// free block first, data block 0 before the index block — TRM § B.3.1).
    /// Every block is allocated, zeros included (no holes are created).
    /// All-or-nothing: on failure (disk full, directory full and not
    /// extendable, bad name, inconsistent volume) the image is unchanged.
    bool writeFile(const std::string& path, uint8_t fileType, uint16_t auxType,
                   const std::vector<uint8_t>& data, std::string& error);

    /// A file, or an EMPTY directory. Frees its blocks in the bit map and
    /// clears its entry (storage type 0), updating file_count. A file whose
    /// access byte disables destroy is refused, and so is a Pascal area.
    bool removeFile(const std::string& path, std::string& error);

    bool makeDirectory(const std::string& path, std::string& error);

    /// Walk the whole volume and check what every mutator relies on: each
    /// directory / index / data block in range, claimed once, and marked
    /// used in the bit map; every entry's storage type one this code can
    /// follow (standard files, directories, and TN #25's $5 extended file
    /// and $4 Pascal area, whose blocks are claimed but never rewritten).
    /// False with the first problem found.
    bool verify(std::string& error) const;

private:
    struct Slot { uint32_t block = 0; unsigned index = 0; };   // index 0..12 in the block
    struct DirRef {
        uint32_t key = 2;       // key block of the directory
        bool     isRoot = true;
        Slot     entry;         // its entry in the parent (subdirectories only)
    };

    uint8_t*       blockPtr(uint32_t b)       { return image_.data() + std::size_t(b) * kBlockBytes; }
    const uint8_t* blockPtr(uint32_t b) const { return image_.data() + std::size_t(b) * kBlockBytes; }
    uint8_t*       entryPtr(const Slot& s);
    const uint8_t* entryPtr(const Slot& s) const;
    uint32_t bitmapBlocks() const;
    bool isFree(uint32_t b) const;
    void setFree(uint32_t b, bool free);

    bool splitPath(const std::string& path, std::vector<std::string>& comps,
                   std::string& error) const;
    bool dirChain(uint32_t key, bool isRoot, std::vector<uint32_t>& blocks,
                  std::string& error) const;
    bool descend(const std::vector<std::string>& comps, std::size_t count,
                 DirRef& dir, std::string& error) const;
    bool find(const DirRef& dir, const std::string& name, Slot& at, bool& found,
              std::string& error) const;
    bool resolveParent(const std::string& path, DirRef& dir, std::string& leaf,
                       std::string& error) const;
    bool fileBlocks(const uint8_t* entry, std::vector<uint32_t>& blocks,
                    std::string& error) const;

    bool allocate(uint32_t& block, std::string& error);
    bool freeSlot(const DirRef& dir, Slot& at, std::string& error);
    bool adjustFileCount(uint32_t dirKey, int delta, std::string& error);
    bool writeData(const std::vector<uint8_t>& data, uint8_t& storage, uint32_t& key,
                   uint32_t& blocksUsed, std::string& error);

    bool writeFileImpl(const std::string& path, uint8_t fileType, uint16_t auxType,
                       const std::vector<uint8_t>& data, std::string& error);
    bool removeFileImpl(const std::string& path, std::string& error);
    bool makeDirectoryImpl(const std::string& path, std::string& error);

    std::vector<uint8_t> image_;
    uint32_t totalBlocks_ = 0;
    uint32_t bitmapPtr_   = 0;
    uint32_t allocCursor_ = 0;
    uint16_t date_ = 0;
    uint16_t time_ = 0;
};

/// A 140 KB image whose bytes are in DOS 3.3 sector order (.dsk / .do) <->
/// ProDOS block order (.po). Size must be 143360; anything else returns an
/// empty vector.
std::vector<uint8_t> dosOrderToBlockOrder(const std::vector<uint8_t>& dosOrdered);
std::vector<uint8_t> blockOrderToDosOrder(const std::vector<uint8_t>& blockOrdered);

} // namespace pom2::prodosfs

#endif // POM2_PRODOS_DISK_FS_H
