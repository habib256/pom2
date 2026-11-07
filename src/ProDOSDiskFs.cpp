// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProDOS file system on a block-ordered image, edited in place — see
// ProDOSDiskFs.h for the layout and its sources. This file adds the rules
// for WRITING: which block is taken first, what a new entry holds, and how a
// directory grows. Each carries its grade.

#include "ProDOSDiskFs.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pom2::prodosfs {

namespace {

// [B] TRM § B.1 / § B.2.2: the volume directory's key block is block 2.
constexpr uint32_t kVolDirKey = 2;
// [B] TRM § B.2.2: entry_length = $27, entries_per_block = $0D — the header
// counts as one of the 13, so a key block holds 12 file entries and every
// other block 13. Volumes with other values are refused, not guessed at.
constexpr std::size_t kEntryLength     = 0x27;
constexpr unsigned    kEntriesPerBlock = 0x0D;

// [B] TRM § B.4.2.1 storage types.
constexpr uint8_t kSeedling     = 0x1;
constexpr uint8_t kSapling      = 0x2;
constexpr uint8_t kTree         = 0x3;
// [B] ProDOS 8 Technical Note #25 "Non-Standard Storage Types" (rev. Dec.
// 1991, prodos8.com/docs/technote/25): $4 Pascal area, $5 GS/OS extended
// file (two forks).
constexpr uint8_t kPascalArea   = 0x4;
constexpr uint8_t kExtended     = 0x5;
constexpr uint8_t kSubdirEntry  = 0xD;
constexpr uint8_t kSubdirHeader = 0xE;
constexpr uint8_t kVolumeHeader = 0xF;

// [B] TRM Table B-1: $0F = directory file.
constexpr uint8_t kDirFileType = 0x0F;

// [B] TRM § B.3.2 / B.3.3 / B.4.2.1: seedling EOF <= $200, sapling
// <= $20000, EOF is three bytes (<= $FFFFFF).
constexpr uint32_t kSeedlingMax = 0x200;
constexpr uint32_t kSaplingMax  = 0x20000;
constexpr uint32_t kMaxEof      = 0xFFFFFF;

// [B] TRM § B.1 / § B.2.2: one bit map block per 4 096 blocks; total_blocks
// is a 2-byte field.
constexpr uint32_t kBlocksPerBitmapBlock = 4096;
constexpr uint32_t kMaxVolumeBlocks      = 65535;
// [F] A 32 MiB image (65 536 blocks) is the common HDV size in the wild;
// ProDOS cannot address its last block, which this code never touches.
constexpr std::size_t kMaxImageBlocks = 65536;

// [B] TRM § 4.4.1 CREATE (p. 36-37): "Access, in most cases, should be set
// to $E3 (full access permitted)" and "The backup bit (B) is always set by
// this call". So a file this code creates carries $E3, as ProDOS's own
// CREATE would leave it; § B.4.2.3 adds that the backup bit is set again
// "after a ... CLOSE after WRITE", which is why a replaced file gets it too.
constexpr uint8_t kAccessNewFile = 0xE3;
constexpr uint8_t kAccessBackup  = 0x20;
constexpr uint8_t kAccessWrite   = 0x02;
constexpr uint8_t kAccessDestroy = 0x80;
// [F] Access byte of a directory HEADER (volume or subdirectory). The TRM
// defines the field (§ B.2.2 / B.2.3) but not the value a formatter or
// CREATE writes; $C3 (destroy, rename, write, read — no backup) is what
// POM2's ProDOSVolume.cpp writes and what Beneath Apple ProDOS p. 4-9 lists
// as the bit meanings. Settled by: the header of a disk freshly formatted
// by Apple's System Utilities, and of a directory made by ProDOS's CREATE.
constexpr uint8_t kAccessHeader = 0xC3;
// [B] Beneath Apple ProDOS ch. 4 "Subdirectory Header" (p. 4-28): "$14 must
// contain $75" — block offset $14, i.e. entry offset $10, the first reserved
// byte. Its MLI error list (p. 6-61, error $4A) adds that the byte must
// carry "5 and only 5 one bits (it is usually $75)". Apple's TRM § B.2.3
// calls the 8 bytes only "reserved". The VOLUME header's reserved bytes stay
// zero: Beneath Apple ProDOS p. 4-9 "Usually zeroes".
constexpr uint8_t kSubdirMarker = 0x75;

// [B] TRM Fig. B-5: file entry offsets (entry-relative).
constexpr std::size_t eStorage   = 0x00;
constexpr std::size_t eName      = 0x01;
constexpr std::size_t eType      = 0x10;
constexpr std::size_t eKey       = 0x11;
constexpr std::size_t eBlocks    = 0x13;
constexpr std::size_t eEof       = 0x15;
constexpr std::size_t eCreated   = 0x18;
constexpr std::size_t eVersion   = 0x1C;
constexpr std::size_t eMinVer    = 0x1D;
constexpr std::size_t eAccess    = 0x1E;
constexpr std::size_t eAux       = 0x1F;
constexpr std::size_t eModified  = 0x21;
constexpr std::size_t eHeaderPtr = 0x25;
// [B] TRM Fig. B-3 / B-4: header offsets, entry-relative (block offset - 4).
constexpr std::size_t hReserved        = 0x10;
constexpr std::size_t hCreated         = 0x18;
constexpr std::size_t hVersion         = 0x1C;
constexpr std::size_t hMinVer          = 0x1D;
constexpr std::size_t hAccess          = 0x1E;
constexpr std::size_t hEntryLength     = 0x1F;
constexpr std::size_t hEntriesPerBlock = 0x20;
constexpr std::size_t hFileCount       = 0x21;
constexpr std::size_t hBitmapPtr       = 0x23;   // volume header
constexpr std::size_t hTotalBlocks     = 0x25;   // volume header
constexpr std::size_t hParentPtr       = 0x23;   // subdirectory header
constexpr std::size_t hParentEntry     = 0x25;   // subdirectory header
constexpr std::size_t hParentEntryLen  = 0x26;   // subdirectory header

// [B] Beneath Apple ProDOS Table 3.1 (p. 3-48), bottom rows: the DOS 3.3
// sectors behind each block of a track — 0&E, D&C, B&A, 9&8, 7&6, 5&4, 3&2,
// 1&F. Index = 256-byte half within the track in block order (block k of
// the track = halves 2k, 2k+1); value = DOS 3.3 sector. The mapping is its
// own inverse.
constexpr uint8_t kHalfToDosSector[16] = {
    0x0, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0xF };

uint16_t rd16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t rd24(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
}
void wr16(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void wr24(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
}

char upperAscii(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool sameName(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (upperAscii(a[i]) != upperAscii(b[i])) return false;
    return true;
}

std::string hex(uint32_t v)
{
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    do { s.insert(s.begin(), digits[v & 0xF]); v >>= 4; } while (v);
    return "$" + s;
}

std::string entryName(const uint8_t* e)
{
    const std::size_t len = e[eStorage] & 0x0F;
    return std::string(reinterpret_cast<const char*>(e + eName), len);
}

/// Index entry `i` of an index block: [B] TRM § B.3.7 — low byte at i,
/// high byte at i + 256.
uint32_t indexEntry(const uint8_t* idx, unsigned i)
{
    return uint32_t(idx[i]) | (uint32_t(idx[256 + i]) << 8);
}
void setIndexEntry(uint8_t* idx, unsigned i, uint32_t block)
{
    idx[i]       = static_cast<uint8_t>(block & 0xFF);
    idx[256 + i] = static_cast<uint8_t>((block >> 8) & 0xFF);
}

/// The 4-byte date/time field: [B] TRM Fig. B-13 / Beneath Apple ProDOS
/// p. 4-9 — date word (bytes 0-1) then time word (bytes 2-3), low byte first.
void writeStamp(uint8_t* p, uint16_t date, uint16_t time)
{
    wr16(p, date);
    wr16(p + 2, time);
}

/// Every field of a file entry ([B] TRM Fig. B-5). version and min_version
/// are 0: [B] TRM § B.2.4 "In ProDOS 1.0, version = 0", "min_version = 0".
void writeEntry(uint8_t* e, const std::string& name, uint8_t storage, uint8_t fileType,
                uint32_t key, uint32_t blocksUsed, uint32_t eof,
                uint16_t cDate, uint16_t cTime, uint8_t access, uint16_t aux,
                uint16_t mDate, uint16_t mTime, uint32_t headerPtr)
{
    std::memset(e, 0, kEntryLength);
    e[eStorage] = static_cast<uint8_t>((storage << 4) | (name.size() & 0x0F));
    std::memcpy(e + eName, name.data(), name.size());
    e[eType] = fileType;
    wr16(e + eKey, key);
    wr16(e + eBlocks, blocksUsed);
    wr24(e + eEof, eof);
    writeStamp(e + eCreated, cDate, cTime);
    e[eVersion] = 0;
    e[eMinVer]  = 0;
    e[eAccess]  = access;
    wr16(e + eAux, aux);
    writeStamp(e + eModified, mDate, mTime);
    wr16(e + eHeaderPtr, headerPtr);
}

Entry decodeEntry(const uint8_t* e)
{
    Entry out;
    out.name        = entryName(e);
    out.storageType = static_cast<uint8_t>(e[eStorage] >> 4);
    out.fileType    = e[eType];
    out.auxType     = rd16(e + eAux);
    out.eof         = rd24(e + eEof);
    out.blocksUsed  = rd16(e + eBlocks);
    out.access      = e[eAccess];
    out.isDirectory = out.storageType == kSubdirEntry;
    out.keyPointer  = rd16(e + eKey);
    out.createDate  = rd16(e + eCreated);
    out.createTime  = rd16(e + eCreated + 2);
    out.modDate     = rd16(e + eModified);
    out.modTime     = rd16(e + eModified + 2);
    return out;
}

/// [B] TRM § B.2.4 / B.2.5: an entry is active iff its first byte is non-zero.
bool active(const uint8_t* e) { return e[eStorage] != 0; }

} // namespace

// ------------------------------------------------------------------ names

bool isValidName(const std::string& name, std::string* folded, std::string* why)
{
    auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    // [B] TRM § 2.1: "up to 15 characters long. It may contain capital
    // letters (A-Z), digits (0-9), and periods (.), and it must begin with a
    // letter. Lowercase letters are automatically converted to uppercase."
    if (name.empty()) return fail("a name cannot be empty");
    if (name.size() > 15) return fail("'" + name + "' is longer than 15 characters");
    std::string up;
    up.reserve(name.size());
    for (char c : name) up.push_back(upperAscii(c));
    if (!(up[0] >= 'A' && up[0] <= 'Z'))
        return fail("'" + name + "' does not begin with a letter");
    for (char c : up) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.';
        if (!ok) return fail("'" + name + "' may hold only letters, digits and '.'");
    }
    if (folded) *folded = up;
    return true;
}

uint16_t packDate(int year, int month, int day)
{
    // [B] ProDOS 8 Technical Note #28: year values 40-99 are 1940-1999,
    // 0-39 are 2000-2039; 100-127 are invalid.
    if (year < 1940 || year > 2039 || month < 1 || month > 12 || day < 1 || day > 31)
        return 0;
    return static_cast<uint16_t>(((year % 100) << 9) | (month << 5) | day);
}

uint16_t packTime(int hour, int minute)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return 0;
    return static_cast<uint16_t>((hour << 8) | minute);
}

// ------------------------------------------------------------ open / format

bool ProDOSDisk::open(std::vector<uint8_t> image, std::string& error)
{
    if (image.size() % kBlockBytes != 0) {
        error = "image size is not a multiple of 512 bytes";
        return false;
    }
    const std::size_t blocks = image.size() / kBlockBytes;
    if (blocks <= kVolDirKey || blocks > kMaxImageBlocks) {
        error = "image is " + std::to_string(blocks) +
                " blocks; a ProDOS volume needs 3 to 65535";
        return false;
    }
    const uint8_t* h = image.data() + kVolDirKey * kBlockBytes + 4;
    if ((h[0] >> 4) != kVolumeHeader || (h[0] & 0x0F) == 0) {
        error = "no ProDOS volume directory header at block 2";
        return false;
    }
    if (h[hEntryLength] != kEntryLength || h[hEntriesPerBlock] != kEntriesPerBlock) {
        error = "volume directory has entry_length " + hex(h[hEntryLength]) +
                " / entries_per_block " + hex(h[hEntriesPerBlock]) +
                " (ProDOS uses $27 / $0D)";
        return false;
    }
    const uint32_t total  = rd16(h + hTotalBlocks);
    const uint32_t bitmap = rd16(h + hBitmapPtr);
    if (total > blocks) {
        error = "volume claims " + std::to_string(total) + " blocks but the image holds " +
                std::to_string(blocks);
        return false;
    }
    const uint32_t bmBlocks = (total + kBlocksPerBitmapBlock - 1) / kBlocksPerBitmapBlock;
    if (total <= kVolDirKey || bitmap <= kVolDirKey || bmBlocks == 0 ||
        bitmap + bmBlocks > total) {
        error = "volume bit map (block " + std::to_string(bitmap) + ", " +
                std::to_string(bmBlocks) + " block(s)) does not fit in " +
                std::to_string(total) + " blocks";
        return false;
    }
    image_       = std::move(image);
    totalBlocks_ = total;
    bitmapPtr_   = bitmap;
    allocCursor_ = 0;
    return true;
}

std::vector<uint8_t> ProDOSDisk::format(uint32_t blocks, const std::string& volumeName,
                                        std::string& error, uint16_t date, uint16_t time)
{
    std::string name, why;
    if (!isValidName(volumeName, &name, &why)) {
        error = "bad volume name: " + why;
        return {};
    }
    // [B] TRM § B.3.1: "Blocks 0-1 - Loader Blocks 2-5 - Volume directory
    // Block 6 - Volume bit map" on a formatted 280-block disk; § B.1: one
    // bit map block per 4 096 blocks. 2 + 4 + 1 = 7 is the smallest volume.
    constexpr uint32_t kDirBlocks = 4;
    constexpr uint32_t kBitmapAt  = kVolDirKey + kDirBlocks;   // 6
    const uint32_t bmBlocks = (blocks + kBlocksPerBitmapBlock - 1) / kBlocksPerBitmapBlock;
    if (blocks > kMaxVolumeBlocks || blocks < kBitmapAt + bmBlocks || blocks < 7) {
        error = "a volume holds 7 to 65535 blocks, not " + std::to_string(blocks);
        return {};
    }
    std::vector<uint8_t> img(std::size_t(blocks) * kBlockBytes, 0);
    auto blk = [&](uint32_t b) { return img.data() + std::size_t(b) * kBlockBytes; };
    // [B] TRM § B.2.1: prev/next pointers, 0 at both ends.
    for (uint32_t b = kVolDirKey; b < kBitmapAt; ++b) {
        wr16(blk(b), b == kVolDirKey ? 0 : b - 1);
        wr16(blk(b) + 2, b + 1 == kBitmapAt ? 0 : b + 1);
    }
    uint8_t* h = blk(kVolDirKey) + 4;
    h[0] = static_cast<uint8_t>((kVolumeHeader << 4) | name.size());
    std::memcpy(h + 1, name.data(), name.size());
    // Reserved $10-$17 stay zero (see kSubdirMarker).
    writeStamp(h + hCreated, date, time);
    h[hVersion]         = 0;
    h[hMinVer]          = 0;
    h[hAccess]          = kAccessHeader;
    h[hEntryLength]     = kEntryLength;
    h[hEntriesPerBlock] = kEntriesPerBlock;
    wr16(h + hFileCount, 0);
    wr16(h + hBitmapPtr, kBitmapAt);
    wr16(h + hTotalBlocks, blocks);
    // [B] TRM § B.2.2: "a value of 1 means the block is free ... the first
    // block is represented by the high bit in the first byte". Everything
    // before the first free block is the volume's own; bits past
    // total_blocks stay 0 so nothing can ever allocate a block that is not
    // there.
    uint8_t* bm = blk(kBitmapAt);
    for (uint32_t b = kBitmapAt + bmBlocks; b < blocks; ++b)
        bm[b >> 3] |= static_cast<uint8_t>(0x80u >> (b & 7));
    return img;
}

std::string ProDOSDisk::volumeName() const
{
    if (image_.empty()) return {};
    return entryName(blockPtr(kVolDirKey) + 4);
}

// ------------------------------------------------------------------ bit map

uint32_t ProDOSDisk::bitmapBlocks() const
{
    return (totalBlocks_ + kBlocksPerBitmapBlock - 1) / kBlocksPerBitmapBlock;
}

bool ProDOSDisk::isFree(uint32_t b) const
{
    const uint8_t* bm = blockPtr(bitmapPtr_);
    return (bm[b >> 3] & (0x80u >> (b & 7))) != 0;
}

void ProDOSDisk::setFree(uint32_t b, bool free)
{
    uint8_t& byte = blockPtr(bitmapPtr_)[b >> 3];
    const uint8_t bit = static_cast<uint8_t>(0x80u >> (b & 7));
    byte = free ? static_cast<uint8_t>(byte | bit) : static_cast<uint8_t>(byte & ~bit);
}

uint32_t ProDOSDisk::freeBlocks() const
{
    uint32_t n = 0;
    for (uint32_t b = 0; b < totalBlocks_; ++b)
        if (isFree(b)) ++n;
    return n;
}

uint8_t* ProDOSDisk::entryPtr(const Slot& s)
{
    return blockPtr(s.block) + 4 + std::size_t(s.index) * kEntryLength;
}
const uint8_t* ProDOSDisk::entryPtr(const Slot& s) const
{
    return blockPtr(s.block) + 4 + std::size_t(s.index) * kEntryLength;
}

// ------------------------------------------------------------------- walks

bool ProDOSDisk::splitPath(const std::string& path, std::vector<std::string>& comps,
                           std::string& error) const
{
    comps.clear();
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (!path.empty() && path[0] == '/' && !parts.empty()) {
        // [B] TRM § 2.1.1: a full pathname is "a series of filenames, each
        // preceded by a slash", the first of them the volume name.
        if (!sameName(parts[0], volumeName())) {
            error = "path names volume /" + parts[0] + " but this volume is /" + volumeName();
            return false;
        }
        parts.erase(parts.begin());
    }
    for (const std::string& p : parts) {
        std::string folded, why;
        if (isValidName(p, &folded, &why)) {
            comps.push_back(folded);
            continue;
        }
        // A name ProDOS 8 would not create can still be ON the disk — A2osX
        // writes `.PROFILE` — and must stay reachable to read or remove. Any
        // 1-15 printable ASCII characters are accepted for LOOKUP; creating
        // an entry re-checks the leaf strictly (`requireCreatable`).
        const bool printable = !p.empty() && p.size() <= 15 &&
            std::all_of(p.begin(), p.end(), [](char c) { return c > ' ' && c < 0x7F; });
        if (!printable) {
            error = "bad name: " + why;
            return false;
        }
        comps.push_back(p);
    }
    return true;
}

namespace {
bool requireCreatable(const std::string& leaf, std::string& error)
{
    std::string why;
    if (isValidName(leaf, nullptr, &why)) return true;
    error = "bad name: " + why;
    return false;
}
} // namespace

bool ProDOSDisk::dirChain(uint32_t key, bool isRoot, std::vector<uint32_t>& blocks,
                          std::string& error) const
{
    blocks.clear();
    if (key <= kVolDirKey - 1 || key >= totalBlocks_ || (!isRoot && key == kVolDirKey)) {
        error = "directory key block " + std::to_string(key) + " is outside the volume";
        return false;
    }
    const uint8_t* h = blockPtr(key) + 4;
    const uint8_t want = isRoot ? kVolumeHeader : kSubdirHeader;
    if ((h[0] >> 4) != want) {
        error = "block " + std::to_string(key) + " holds no " +
                (isRoot ? "volume directory" : "subdirectory") + " header";
        return false;
    }
    if (h[hEntryLength] != kEntryLength || h[hEntriesPerBlock] != kEntriesPerBlock) {
        error = "directory at block " + std::to_string(key) +
                " does not use $27-byte entries, 13 per block";
        return false;
    }
    // [B] TRM § B.2.1: follow next pointers to 0. Bounded by the volume:
    // a block seen twice is a cycle.
    std::vector<bool> seen(totalBlocks_, false);
    uint32_t b = key;
    while (b != 0) {
        if (b < kVolDirKey || b >= totalBlocks_) {
            error = "directory chain points at block " + std::to_string(b) +
                    ", outside the volume";
            return false;
        }
        if (seen[b]) {
            error = "directory chain loops back to block " + std::to_string(b);
            return false;
        }
        seen[b] = true;
        blocks.push_back(b);
        b = rd16(blockPtr(b) + 2);
    }
    return true;
}

bool ProDOSDisk::find(const DirRef& dir, const std::string& name, Slot& at, bool& found,
                      std::string& error) const
{
    found = false;
    std::vector<uint32_t> chain;
    if (!dirChain(dir.key, dir.isRoot, chain, error)) return false;
    for (uint32_t b : chain) {
        for (unsigned i = (b == dir.key ? 1u : 0u); i < kEntriesPerBlock; ++i) {
            const Slot s{b, i};
            const uint8_t* e = entryPtr(s);
            if (!active(e)) continue;
            if (sameName(entryName(e), name)) {
                at = s;
                found = true;
                return true;
            }
        }
    }
    return true;
}

bool ProDOSDisk::descend(const std::vector<std::string>& comps, std::size_t count,
                         DirRef& dir, std::string& error) const
{
    dir = DirRef{};
    for (std::size_t i = 0; i < count; ++i) {
        Slot at;
        bool found = false;
        if (!find(dir, comps[i], at, found, error)) return false;
        if (!found) {
            error = comps[i] + ": no such directory";
            return false;
        }
        const uint8_t* e = entryPtr(at);
        if ((e[eStorage] >> 4) != kSubdirEntry) {
            error = comps[i] + " is not a directory";
            return false;
        }
        dir.key    = rd16(e + eKey);
        dir.isRoot = false;
        dir.entry  = at;
    }
    return true;
}

bool ProDOSDisk::resolveParent(const std::string& path, DirRef& dir, std::string& leaf,
                               std::string& error) const
{
    std::vector<std::string> comps;
    if (!splitPath(path, comps, error)) return false;
    if (comps.empty()) {
        error = "no file name in '" + path + "'";
        return false;
    }
    if (!descend(comps, comps.size() - 1, dir, error)) return false;
    leaf = comps.back();
    return true;
}

bool ProDOSDisk::fileBlocks(const uint8_t* e, std::vector<uint32_t>& blocks,
                            std::string& error) const
{
    blocks.clear();
    const uint8_t storage = static_cast<uint8_t>(e[eStorage] >> 4);
    const uint32_t key = rd16(e + eKey);
    const std::string name = entryName(e);
    auto inRange = [&](uint32_t b, const char* what) {
        if (b == 0 || b >= totalBlocks_) {
            error = name + ": " + what + " " + std::to_string(b) + " is outside the volume";
            return false;
        }
        return true;
    };
    if (!inRange(key, "key block")) return false;
    // An index block's 256 pointers ([B] TRM § B.3.3 / B.3.7). A tree's
    // master index uses at most 128 of them (§ B.3.4), but every non-zero
    // one is a block the file holds as far as the bit map is concerned.
    auto walkIndex = [&](uint32_t idxBlock, bool isMaster) {
        const uint8_t* idx = blockPtr(idxBlock);
        for (unsigned i = 0; i < 256; ++i) {
            const uint32_t p = indexEntry(idx, i);
            if (p == 0) continue;
            if (!inRange(p, isMaster ? "index block" : "data block")) return false;
            blocks.push_back(p);
        }
        return true;
    };
    // One fork: a seedling, sapling or tree rooted at `root`.
    auto fork = [&](uint8_t type, uint32_t root) {
        if (!inRange(root, "key block")) return false;
        blocks.push_back(root);
        if (type == kSeedling) return true;
        if (type == kSapling) return walkIndex(root, false);
        if (type == kTree) {
            const uint8_t* master = blockPtr(root);
            for (unsigned g = 0; g < 256; ++g) {
                const uint32_t sub = indexEntry(master, g);
                if (sub == 0) continue;
                if (!inRange(sub, "index block")) return false;
                blocks.push_back(sub);
                if (!walkIndex(sub, false)) return false;
            }
            return true;
        }
        error = name + ": storage type " + hex(type) + " is not a standard file";
        return false;
    };
    if (storage == kSeedling || storage == kSapling || storage == kTree)
        return fork(storage, key);
    if (storage == kExtended) {
        // [B] TN #25: the extended key block holds two mini-entries, the
        // data fork's at +$000 and the resource fork's at +$100, each
        // storage_type (byte), key_block (word), blocks_used (word), EOF
        // (3 bytes) — every fork a standard seedling / sapling / tree.
        blocks.push_back(key);
        const uint8_t* xk = blockPtr(key);
        for (const unsigned off : {0x000u, 0x100u})
            if (!fork(xk[off], rd16(xk + off + 1))) {
                error += off ? " (resource fork)" : " (data fork)";
                return false;
            }
        return true;
    }
    if (storage == kPascalArea) {
        // [B] TN #25: key_pointer is the first block of the area (TOTAL_BLOCKS
        // - 2 as created), blocks_used is 2, and "when PPM expands or
        // contracts the Pascal area, blocks_used and key_pointer are updated
        // accordingly". [F] That the area is the CONTIGUOUS run key ..
        // key + blocks_used - 1 is read off that sentence, not stated; a
        // volume where it is not would fail `verify` (a block marked free or
        // used twice) and be refused, never written over.
        const uint32_t used = rd16(e + eBlocks);
        if (used == 0 || key + used > totalBlocks_) {
            error = name + ": Pascal area of " + std::to_string(used) + " blocks from " +
                    std::to_string(key) + " is outside the volume";
            return false;
        }
        for (uint32_t b = key; b < key + used; ++b) blocks.push_back(b);
        return true;
    }
    error = name + ": storage type " + hex(storage) + " is not a standard file";
    return false;
}

bool ProDOSDisk::verify(std::string& error) const
{
    if (image_.empty()) {
        error = "no volume open";
        return false;
    }
    std::vector<uint8_t> claimed(totalBlocks_, 0);
    auto claim = [&](uint32_t b, const std::string& owner) {
        if (b >= totalBlocks_) {
            error = owner + " uses block " + std::to_string(b) + ", outside the volume";
            return false;
        }
        if (claimed[b]) {
            error = "block " + std::to_string(b) + " is used twice (again by " + owner + ")";
            return false;
        }
        claimed[b] = 1;
        if (isFree(b)) {
            error = "block " + std::to_string(b) + " is used by " + owner +
                    " but the volume bit map marks it free";
            return false;
        }
        return true;
    };
    // [B] TRM § B.1: blocks 0-1 (loader) and the bit map are the volume's own.
    if (!claim(0, "the boot loader") || !claim(1, "the boot loader")) return false;
    for (uint32_t i = 0; i < bitmapBlocks(); ++i)
        if (!claim(bitmapPtr_ + i, "the volume bit map")) return false;

    // Explicit stack, not recursion: a crafted volume can nest thousands of
    // directories. Each directory block is claimed once, so a key pointer
    // aliasing an ancestor ends the walk with an error, never a loop.
    struct Pending { uint32_t key; bool isRoot; std::string path; };
    std::vector<Pending> stack{{kVolDirKey, true, "/" + volumeName()}};
    std::vector<uint32_t> chain, blocks;
    while (!stack.empty()) {
        const Pending dir = stack.back();
        stack.pop_back();
        if (!dirChain(dir.key, dir.isRoot, chain, error)) {
            error = dir.path + ": " + error;
            return false;
        }
        for (uint32_t b : chain)
            if (!claim(b, "directory " + dir.path)) return false;
        for (uint32_t b : chain) {
            for (unsigned i = (b == dir.key ? 1u : 0u); i < kEntriesPerBlock; ++i) {
                const uint8_t* e = entryPtr(Slot{b, i});
                if (!active(e)) continue;
                const uint8_t storage = static_cast<uint8_t>(e[eStorage] >> 4);
                const std::string path = dir.path + "/" + entryName(e);
                if (storage == kSubdirEntry) {
                    stack.push_back({rd16(e + eKey), false, path});
                } else if (storage == kSeedling || storage == kSapling || storage == kTree ||
                           storage == kExtended || storage == kPascalArea) {
                    // $5 and $4 are TN #25's (fileBlocks): an IIgs System
                    // disk holds extended files, and refusing them blocked
                    // every write to it (bug hunt 2026-11-07).
                    if (!fileBlocks(e, blocks, error)) return false;
                    for (uint32_t fb : blocks)
                        if (!claim(fb, path)) return false;
                } else {
                    // Any other storage type: which blocks it owns is in no
                    // document, so nothing may be allocated while it is there.
                    error = path + " has storage type " + hex(storage) +
                            ", which this tool does not follow";
                    return false;
                }
            }
        }
    }
    return true;
}

// -------------------------------------------------------------------- reads

bool ProDOSDisk::list(const std::string& dirPath, std::vector<Entry>& out,
                      std::string& error) const
{
    out.clear();
    if (image_.empty()) { error = "no volume open"; return false; }
    std::vector<std::string> comps;
    if (!splitPath(dirPath, comps, error)) return false;
    DirRef dir;
    if (!descend(comps, comps.size(), dir, error)) return false;
    std::vector<uint32_t> chain;
    if (!dirChain(dir.key, dir.isRoot, chain, error)) return false;
    for (uint32_t b : chain)
        for (unsigned i = (b == dir.key ? 1u : 0u); i < kEntriesPerBlock; ++i) {
            const uint8_t* e = entryPtr(Slot{b, i});
            if (active(e)) out.push_back(decodeEntry(e));
        }
    return true;
}

bool ProDOSDisk::readFile(const std::string& path, std::vector<uint8_t>& data, Entry* info,
                          std::string& error) const
{
    data.clear();
    if (image_.empty()) { error = "no volume open"; return false; }
    DirRef dir;
    std::string leaf;
    if (!resolveParent(path, dir, leaf, error)) return false;
    Slot at;
    bool found = false;
    if (!find(dir, leaf, at, found, error)) return false;
    if (!found) { error = leaf + ": no such file"; return false; }
    const uint8_t* e = entryPtr(at);
    const Entry ent = decodeEntry(e);
    if (ent.isDirectory) { error = leaf + " is a directory"; return false; }
    if (ent.storageType != kSeedling && ent.storageType != kSapling &&
        ent.storageType != kTree) {
        error = leaf + ": storage type " + hex(ent.storageType) + " is not supported";
        return false;
    }
    const uint32_t key = ent.keyPointer;
    if (key == 0 || key >= totalBlocks_) {
        error = leaf + ": key block " + std::to_string(key) + " is outside the volume";
        return false;
    }
    // EOF is 24-bit, so this allocation is bounded at 16 MiB whatever the
    // image says.
    const uint32_t eof = ent.eof;
    const uint32_t nBlocks = (eof + kBlockBytes - 1) / kBlockBytes;
    data.assign(eof, 0);
    auto copyBlock = [&](uint32_t fileBlock, uint32_t diskBlock) {
        const std::size_t off = std::size_t(fileBlock) * kBlockBytes;
        const std::size_t len = std::min<std::size_t>(kBlockBytes, data.size() - off);
        std::memcpy(data.data() + off, blockPtr(diskBlock), len);
    };
    auto badPointer = [&](const char* what, uint32_t from, uint32_t to) {
        error = leaf + ": " + what + " " + std::to_string(from) + " points at block " +
                std::to_string(to) + ", outside the volume";
        data.clear();
        return false;
    };
    if (ent.storageType == kSeedling) {
        // A seedling with EOF past 512 is sparse beyond its one block.
        if (eof > 0) copyBlock(0, key);
    } else if (ent.storageType == kSapling) {
        if (nBlocks > 256) {
            error = leaf + ": a sapling cannot hold an EOF of " + std::to_string(eof);
            data.clear();
            return false;
        }
        const uint8_t* idx = blockPtr(key);
        for (uint32_t i = 0; i < nBlocks; ++i) {
            const uint32_t p = indexEntry(idx, i);
            if (p == 0) continue;                       // [B] TRM § B.3.6 hole
            if (p >= totalBlocks_) return badPointer("index block", key, p);
            copyBlock(i, p);
        }
    } else {
        const uint8_t* master = blockPtr(key);
        for (uint32_t i = 0; i < nBlocks; ++i) {
            const uint32_t sub = indexEntry(master, i / 256);   // i/256 <= 127
            if (sub == 0) { i |= 255; continue; }       // a whole hole of 256 blocks
            if (sub >= totalBlocks_) return badPointer("master index block", key, sub);
            const uint32_t p = indexEntry(blockPtr(sub), i % 256);
            if (p == 0) continue;
            if (p >= totalBlocks_) return badPointer("index block", sub, p);
            copyBlock(i, p);
        }
    }
    if (info) *info = ent;
    return true;
}

// ---------------------------------------------------------------- mutation

bool ProDOSDisk::allocate(uint32_t& block, std::string& error)
{
    // [B] TRM § B.3.1: "when a new block is needed ProDOS always allocates
    // the first unused block in the volume bit map". The cursor only skips
    // what this same operation already took (frees happen before allocation).
    const uint32_t ownEnd = bitmapPtr_ + bitmapBlocks();
    for (uint32_t b = allocCursor_; b < totalBlocks_; ++b) {
        if (!isFree(b)) continue;
        if (b < ownEnd) {
            error = "the volume bit map marks block " + std::to_string(b) +
                    " free, but it belongs to the volume's own structure";
            return false;
        }
        setFree(b, false);
        std::memset(blockPtr(b), 0, kBlockBytes);
        allocCursor_ = b + 1;
        block = b;
        return true;
    }
    error = "disk full";
    return false;
}

bool ProDOSDisk::adjustFileCount(uint32_t dirKey, int delta, std::string& error)
{
    uint8_t* h = blockPtr(dirKey) + 4;
    const uint32_t count = rd16(h + hFileCount);
    if (delta < 0 && count == 0) {
        error = "directory file_count is already 0; the directory is inconsistent";
        return false;
    }
    if (delta > 0 && count == 0xFFFF) {
        error = "directory file_count would overflow";
        return false;
    }
    wr16(h + hFileCount, delta < 0 ? count - 1 : count + 1);
    return true;
}

bool ProDOSDisk::freeSlot(const DirRef& dir, Slot& at, std::string& error)
{
    std::vector<uint32_t> chain;
    if (!dirChain(dir.key, dir.isRoot, chain, error)) return false;
    for (uint32_t b : chain)
        for (unsigned i = (b == dir.key ? 1u : 0u); i < kEntriesPerBlock; ++i)
            if (!active(entryPtr(Slot{b, i}))) {
                at = Slot{b, i};
                return true;
            }
    if (dir.isRoot) {
        // [B] Beneath Apple ProDOS p. 6-61, error $49: "Unlike subdirectories,
        // the Volume Directory is of a fixed size (51 entries) and cannot be
        // extended."
        error = "the volume directory is full (" +
                std::to_string(chain.size() * kEntriesPerBlock - 1) +
                " entries) and cannot grow";
        return false;
    }
    // A subdirectory grows by one block, linked at the end of its chain
    // ([B] TRM § B.2.1), and its entry in the parent counts it: blocks_used
    // includes "the blocks containing subdirectory information" (§ B.2.4),
    // and the directory file's EOF is its length in bytes. ProDOS finds that
    // entry through the header's parent_pointer / parent_entry_number
    // (§ B.2.3); if they do not name the entry the path went through, the
    // directory is inconsistent and is not touched.
    const uint8_t* h = blockPtr(dir.key) + 4;
    if (rd16(h + hParentPtr) != dir.entry.block ||
        h[hParentEntry] != dir.entry.index + 1 || h[hParentEntryLen] != kEntryLength) {
        error = "subdirectory header at block " + std::to_string(dir.key) +
                " does not point back at its entry; not extending it";
        return false;
    }
    uint32_t nb = 0;
    if (!allocate(nb, error)) return false;
    const uint32_t last = chain.back();
    wr16(blockPtr(last) + 2, nb);
    wr16(blockPtr(nb), last);
    wr16(blockPtr(nb) + 2, 0);
    uint8_t* pe = entryPtr(dir.entry);
    const uint32_t used = uint32_t(chain.size()) + 1;
    wr16(pe + eBlocks, used);
    wr24(pe + eEof, used * kBlockBytes);
    at = Slot{nb, 0};
    return true;
}

bool ProDOSDisk::writeData(const std::vector<uint8_t>& data, uint8_t& storage, uint32_t& key,
                           uint32_t& blocksUsed, std::string& error)
{
    const uint32_t size = static_cast<uint32_t>(data.size());
    // [B] TRM § B.3.6: "If you create a file with an EOF of $0, ProDOS
    // allocates only the key block (a data block)" — so even an empty file
    // has one data block. Every 512-byte chunk is allocated, zeros included:
    // that is what a sequential WRITE does (§ B.3.6 allocates "those data
    // blocks that have had data written to them"); holes only appear when a
    // program moves the MARK past unwritten blocks.
    const uint32_t nData = std::max<uint32_t>(1, (size + kBlockBytes - 1) / kBlockBytes);
    std::vector<uint32_t> dataBlocks(nData, 0);
    auto copyChunk = [&](uint32_t i) {
        const std::size_t off = std::size_t(i) * kBlockBytes;
        if (off >= data.size()) return;
        const std::size_t len = std::min<std::size_t>(kBlockBytes, data.size() - off);
        std::memcpy(blockPtr(dataBlocks[i]), data.data() + off, len);
    };
    // Allocation order follows the growth TRM § B.3.1 describes for a file
    // written from the start: data block 0 (the seedling's key), then the
    // index block and data blocks 1-255, then the master index, index block
    // 1 and data block 256, and a new index block each 256 data blocks.
    if (!allocate(dataBlocks[0], error)) return false;
    copyChunk(0);
    if (size <= kSeedlingMax) {
        storage = kSeedling;
        key = dataBlocks[0];
        blocksUsed = 1;
        return true;
    }
    uint32_t index0 = 0;
    if (!allocate(index0, error)) return false;
    setIndexEntry(blockPtr(index0), 0, dataBlocks[0]);
    const uint32_t firstGroupEnd = std::min<uint32_t>(nData, 256);
    for (uint32_t i = 1; i < firstGroupEnd; ++i) {
        if (!allocate(dataBlocks[i], error)) return false;
        setIndexEntry(blockPtr(index0), i, dataBlocks[i]);
        copyChunk(i);
    }
    if (size <= kSaplingMax) {
        storage = kSapling;
        key = index0;
        blocksUsed = nData + 1;
        return true;
    }
    uint32_t master = 0;
    if (!allocate(master, error)) return false;
    setIndexEntry(blockPtr(master), 0, index0);
    uint32_t indexBlocks = 1;
    uint32_t current = index0;
    for (uint32_t i = 256; i < nData; ++i) {
        if (i % 256 == 0) {
            if (!allocate(current, error)) return false;
            setIndexEntry(blockPtr(master), i / 256, current);
            ++indexBlocks;
        }
        if (!allocate(dataBlocks[i], error)) return false;
        setIndexEntry(blockPtr(current), i % 256, dataBlocks[i]);
        copyChunk(i);
    }
    storage = kTree;
    key = master;
    blocksUsed = nData + indexBlocks + 1;
    return true;
}

bool ProDOSDisk::writeFile(const std::string& path, uint8_t fileType, uint16_t auxType,
                           const std::vector<uint8_t>& data, std::string& error)
{
    if (image_.empty()) { error = "no volume open"; return false; }
    std::vector<uint8_t> before = image_;
    allocCursor_ = 0;
    if (writeFileImpl(path, fileType, auxType, data, error)) return true;
    image_.swap(before);
    return false;
}

bool ProDOSDisk::writeFileImpl(const std::string& path, uint8_t fileType, uint16_t auxType,
                               const std::vector<uint8_t>& data, std::string& error)
{
    if (data.size() > kMaxEof) {
        error = "a ProDOS file holds at most 16 777 215 bytes";
        return false;
    }
    if (fileType == kDirFileType) {
        error = "file type $0F is a directory; use makeDirectory";
        return false;
    }
    DirRef dir;
    std::string leaf;
    if (!resolveParent(path, dir, leaf, error)) return false;
    std::string why;
    if (!verify(why)) {
        error = "volume is inconsistent, not writing: " + why;
        return false;
    }
    Slot at;
    bool found = false;
    if (!find(dir, leaf, at, found, error)) return false;
    uint16_t cDate = date_, cTime = time_;
    uint8_t access = kAccessNewFile;
    if (found) {
        uint8_t* e = entryPtr(at);
        const uint8_t storage = static_cast<uint8_t>(e[eStorage] >> 4);
        if (storage == kSubdirEntry) {
            error = leaf + " is a directory";
            return false;
        }
        if (storage != kSeedling && storage != kSapling && storage != kTree) {
            error = leaf + ": storage type " + hex(storage) + " cannot be replaced";
            return false;
        }
        if ((e[eAccess] & kAccessWrite) == 0) {
            error = leaf + " is locked";
            return false;
        }
        // Replacing keeps the entry, its creation stamp and its access
        // byte, plus the backup bit ([B] TRM § B.4.2.3).
        std::vector<uint32_t> old;
        if (!fileBlocks(e, old, error)) return false;
        for (uint32_t b : old) setFree(b, true);
        cDate  = rd16(e + eCreated);
        cTime  = rd16(e + eCreated + 2);
        access = static_cast<uint8_t>(e[eAccess] | kAccessBackup);
    } else {
        if (!requireCreatable(leaf, error)) return false;
        if (!freeSlot(dir, at, error)) return false;
        if (!adjustFileCount(dir.key, +1, error)) return false;
    }
    uint8_t storage = 0;
    uint32_t key = 0, blocksUsed = 0;
    if (!writeData(data, storage, key, blocksUsed, error)) return false;
    writeEntry(entryPtr(at), leaf, storage, fileType, key, blocksUsed,
               static_cast<uint32_t>(data.size()), cDate, cTime, access, auxType,
               date_, time_, dir.key);
    return true;
}

bool ProDOSDisk::removeFile(const std::string& path, std::string& error)
{
    if (image_.empty()) { error = "no volume open"; return false; }
    std::vector<uint8_t> before = image_;
    allocCursor_ = 0;
    if (removeFileImpl(path, error)) return true;
    image_.swap(before);
    return false;
}

bool ProDOSDisk::removeFileImpl(const std::string& path, std::string& error)
{
    DirRef dir;
    std::string leaf;
    if (!resolveParent(path, dir, leaf, error)) return false;
    std::string why;
    if (!verify(why)) {
        error = "volume is inconsistent, not removing: " + why;
        return false;
    }
    Slot at;
    bool found = false;
    if (!find(dir, leaf, at, found, error)) return false;
    if (!found) { error = leaf + ": no such file"; return false; }
    uint8_t* e = entryPtr(at);
    if ((e[eAccess] & kAccessDestroy) == 0) {
        // [B] TRM § B.4.2.3: bit 7 is Destroy-Enable.
        error = leaf + " is locked (destroy disabled)";
        return false;
    }
    const uint8_t storage = static_cast<uint8_t>(e[eStorage] >> 4);
    std::vector<uint32_t> blocks;
    if (storage == kPascalArea) {
        // Its blocks are a whole Pascal volume set PPM manages; freeing them
        // from here would leave PPM's header describing space ProDOS reuses.
        error = leaf + " is a Pascal area: remove it with the Pascal ProFile Manager";
        return false;
    }
    if (storage == kSubdirEntry) {
        // [B] TRM § B.2.4: "You cannot delete a subdirectory that contains
        // any files."
        const DirRef sub{rd16(e + eKey), false, at};
        if (!dirChain(sub.key, false, blocks, error)) return false;
        for (uint32_t b : blocks)
            for (unsigned i = (b == sub.key ? 1u : 0u); i < kEntriesPerBlock; ++i)
                if (active(entryPtr(Slot{b, i}))) {
                    error = leaf + " is not empty";
                    return false;
                }
    } else if (!fileBlocks(e, blocks, error)) {
        return false;
    }
    for (uint32_t b : blocks) setFree(b, true);
    // [B] TRM § B.2.4: an inactive entry has a first byte of zero. Only that
    // byte is cleared — the rest is left as it was, which is what lets an
    // undelete utility find the file again.
    e[eStorage] = 0;
    return adjustFileCount(dir.key, -1, error);
}

bool ProDOSDisk::makeDirectory(const std::string& path, std::string& error)
{
    if (image_.empty()) { error = "no volume open"; return false; }
    std::vector<uint8_t> before = image_;
    allocCursor_ = 0;
    if (makeDirectoryImpl(path, error)) return true;
    image_.swap(before);
    return false;
}

bool ProDOSDisk::makeDirectoryImpl(const std::string& path, std::string& error)
{
    DirRef dir;
    std::string leaf;
    if (!resolveParent(path, dir, leaf, error)) return false;
    std::string why;
    if (!verify(why)) {
        error = "volume is inconsistent, not writing: " + why;
        return false;
    }
    Slot at;
    bool found = false;
    if (!find(dir, leaf, at, found, error)) return false;
    if (found) { error = leaf + " already exists"; return false; }
    if (!requireCreatable(leaf, error)) return false;
    if (!freeSlot(dir, at, error)) return false;
    if (!adjustFileCount(dir.key, +1, error)) return false;
    // [B] TRM § 4.4.1 CREATE: "One block (512 bytes) of disk space is
    // allocated, and the entry's key_pointer field is set to indicate that
    // block" — for a directory (storage $D) too.
    uint32_t key = 0;
    if (!allocate(key, error)) return false;
    uint8_t* blk = blockPtr(key);
    wr16(blk, 0);       // [B] TRM § B.2.1: key block has no predecessor
    wr16(blk + 2, 0);   // ... and, alone, no successor
    uint8_t* h = blk + 4;
    h[0] = static_cast<uint8_t>((kSubdirHeader << 4) | leaf.size());
    std::memcpy(h + 1, leaf.data(), leaf.size());
    h[hReserved] = kSubdirMarker;
    writeStamp(h + hCreated, date_, time_);
    h[hVersion]         = 0;
    h[hMinVer]          = 0;
    h[hAccess]          = kAccessHeader;
    h[hEntryLength]     = kEntryLength;
    h[hEntriesPerBlock] = kEntriesPerBlock;
    wr16(h + hFileCount, 0);
    // [B] TRM § B.2.3: parent_pointer = the block holding this directory's
    // entry; parent_entry_number = its number in that block, counted from 1
    // with the key block's header as entry 1 (§ B.2.5's reading loop starts
    // a key block at "entry two" and any other block at entry 1).
    wr16(h + hParentPtr, at.block);
    h[hParentEntry]    = static_cast<uint8_t>(at.index + 1);
    h[hParentEntryLen] = kEntryLength;
    // The entry: file type $0F, blocks_used 1, EOF = the directory file's
    // length (one block), access as CREATE leaves it ($E3, see above).
    writeEntry(entryPtr(at), leaf, kSubdirEntry, kDirFileType, key, 1,
               static_cast<uint32_t>(kBlockBytes), date_, time_, kAccessNewFile, 0,
               date_, time_, dir.key);
    return true;
}

// --------------------------------------------------- 5.25" sector ordering

std::vector<uint8_t> dosOrderToBlockOrder(const std::vector<uint8_t>& dosOrdered)
{
    if (dosOrdered.size() != kDiskette140Bytes) return {};
    std::vector<uint8_t> out(kDiskette140Bytes);
    for (std::size_t t = 0; t < 35; ++t)
        for (std::size_t half = 0; half < 16; ++half)
            std::memcpy(out.data() + t * 4096 + half * 256,
                        dosOrdered.data() + t * 4096 + std::size_t(kHalfToDosSector[half]) * 256,
                        256);
    return out;
}

std::vector<uint8_t> blockOrderToDosOrder(const std::vector<uint8_t>& blockOrdered)
{
    if (blockOrdered.size() != kDiskette140Bytes) return {};
    std::vector<uint8_t> out(kDiskette140Bytes);
    for (std::size_t t = 0; t < 35; ++t)
        for (std::size_t half = 0; half < 16; ++half)
            std::memcpy(out.data() + t * 4096 + std::size_t(kHalfToDosSector[half]) * 256,
                        blockOrdered.data() + t * 4096 + half * 256, 256);
    return out;
}

} // namespace pom2::prodosfs
