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

#include "ProDOSVolume.h"
#include "AtomicFileReplace.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace pom2 {

namespace {

constexpr std::size_t kBlockBytes        = 512;
constexpr std::size_t kVolDirEntriesK0   = 12;       // entries in block 2 (after vol header)
constexpr std::size_t kVolDirEntriesKN   = 13;       // entries in blocks 3, 4, 5
constexpr std::size_t kVolDirTotalSlots  = kVolDirEntriesK0 + 3 * kVolDirEntriesKN;  // 51
constexpr std::size_t kEntryLength       = 39;
constexpr std::size_t kSaplingMaxBytes   = 131072;   // 256 blocks × 512
constexpr std::size_t kBootBlocks        = 2;
constexpr std::size_t kVolDirBlocks      = 4;
constexpr std::size_t kBitmapBlock       = 6;
constexpr std::size_t kStructuralBeforeBitmap = kBootBlocks + kVolDirBlocks; // = 6
constexpr std::size_t kMinFirstDataBlock = kStructuralBeforeBitmap + 1;      // = 7
constexpr std::size_t kBlocksPerBitmap   = kBlockBytes * 8;                  // 4096
constexpr std::size_t kMaxVolumeBlocks   = 65535; // ProDOS total_blocks is 16-bit

constexpr std::uint8_t kStorageSeedling     = 0x1;
constexpr std::uint8_t kStorageSapling      = 0x2;
constexpr std::uint8_t kStorageSubdirEntry  = 0xD;
constexpr std::uint8_t kStorageSubdirHeader = 0xE;
constexpr std::uint8_t kStorageVolDir       = 0xF;

constexpr std::uint8_t kFileTypeDir         = 0x0F;

constexpr std::size_t  kMaxRecursionDepth   = 16;

// Bounds for the write-back walk (`decodeOneDir`). The volume image is
// guest-writable RAM, so the directory graph it describes is untrusted
// input: a subdir entry's key_pointer is only range-checked, nothing stops
// it pointing back at an ancestor — or at the very block that holds the
// entry. A depth cap alone bounds NOTHING there, because it does not bound
// the fan-out: 12 self-referential entries in one block explored to depth 16
// is 12^17 visits, each performing a real `fs::create_directories` under a
// fresh path. So the walk also carries a per-call set of already-expanded
// directory blocks (making the graph a forest — every block is walked at
// most once, which caps the visit count at the number of blocks in the
// image) and a hard budget on directories created.
constexpr std::size_t  kMaxDirBlockChain    = 256;    // blocks per dir chain
constexpr std::size_t  kMaxDecodeDirs       = 2048;   // host dirs per decode

struct PreparedFile {
    std::string                 prodosName;
    std::uint8_t                fileType   = 0;
    std::uint16_t               auxType    = 0;
    std::vector<std::uint8_t>   data;
    std::uint8_t                storageType = kStorageSeedling;
    std::size_t                 dataBlocks  = 0;
    std::size_t                 indexBlocks = 0;     // 0 for seedling, 1 for sapling
    // Filled at layout: keyPointer (= seedling data block, sapling index block).
    std::size_t                 firstBlock  = 0;
};

struct PreparedDir;

// Order of children within a directory. We mix files and subdirs into a
// single iteration order (alphabetical by ProDOS name). `index` is into
// either `files` or `subdirs` of the owning PreparedDir.
struct DirChild {
    bool        isDir = false;
    std::size_t index = 0;
};

struct PreparedDir {
    std::string                                  prodosName;     // empty for vol root
    std::vector<PreparedFile>                    files;
    std::vector<std::unique_ptr<PreparedDir>>    subdirs;
    std::vector<DirChild>                        order;
    // Layout fields (filled in pass 2):
    std::size_t   firstDirBlock = 0;     // volume dir = 2; subdirs allocated.
    std::size_t   numDirBlocks  = 1;     // volume dir = 4.
    std::size_t   parentDirBlock  = 0;
    std::uint8_t  parentEntrySlot = 0;   // 1-based, ProDOS convention; 0 = vol.
};

std::uint8_t fileTypeFromExtension(const std::string& ext)
{
    // Common ProDOS file types (cf. Apple ProDOS Tech Ref):
    //   $00 typeless / unknown
    //   $04 TXT
    //   $06 BIN
    //   $FA INT (Integer BASIC)
    //   $FC BAS (Applesoft)
    //   $FF SYS
    std::string e = ext;
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (e == ".bas") return 0xFC;
    if (e == ".bin") return 0x06;
    if (e == ".sys") return 0xFF;
    if (e == ".txt") return 0x04;
    if (e == ".int") return 0xFA;
    // Extensionless host files → typeless (0x00) so the decode adds NO
    // extension and the name round-trips verbatim ("GAME" → "GAME", not
    // "GAME.bin"). Unknown extensions keep their dot in the ProDOS name, so
    // their type is informational → plain BIN.
    if (e.empty() || e == ".") return 0x00;
    return 0x06;
}

std::string sanitiseProDOSName(const std::string& hostName)
{
    fs::path p(hostName);
    std::string raw = p.filename().string();

    // Strip well-known extensions so they don't eat into the 15-char ProDOS
    // name budget (e.g. "HELLO.BAS" → "HELLO" with file_type=BAS). Other
    // extensions stay; the dot is allowed in ProDOS names.
    static const char* kStripExts[] = {
        ".bas", ".bin", ".sys", ".txt", ".int",
        ".dsk", ".po",  ".do",  ".hdv", ".2mg"
    };
    for (const char* xe : kStripExts) {
        const std::size_t xn = std::strlen(xe);
        if (raw.size() <= xn) continue;
        std::string tail = raw.substr(raw.size() - xn);
        for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail == xe) {
            raw.resize(raw.size() - xn);
            break;
        }
    }

    // Restrict to A-Z 0-9 . — replace anything else with '.'.
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc >= 'a' && uc <= 'z')      out += static_cast<char>(uc - 'a' + 'A');
        else if ((uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') || uc == '.')
                                         out += static_cast<char>(uc);
        else                             out += '.';
    }
    // ProDOS names must start with a letter.
    if (out.empty() || !(out[0] >= 'A' && out[0] <= 'Z')) {
        out = "A" + out;
    }
    if (out.size() > 15) out.resize(15);
    return out;
}

std::string uniqueName(const std::string& base,
                       std::unordered_map<std::string, int>& used)
{
    if (used.find(base) == used.end()) {
        used[base] = 0;
        return base;
    }
    for (int i = 1; i < 1000; ++i) {
        const std::string suffix = "." + std::to_string(i);
        std::string cand = base;
        if (cand.size() + suffix.size() > 15) cand.resize(15 - suffix.size());
        cand += suffix;
        if (used.find(cand) == used.end()) {
            used[cand] = 0;
            return cand;
        }
    }
    return base;  // pathological — give up
}

inline void put16(std::uint8_t* p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}
inline void put24(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
}

void writeFileEntry(std::uint8_t* dst, const PreparedFile& f,
                    std::uint16_t keyPointer, std::uint16_t blocksUsed,
                    std::uint32_t eof, std::uint16_t headerPointer = 2)
{
    std::memset(dst, 0, kEntryLength);
    const std::uint8_t nameLen = static_cast<std::uint8_t>(f.prodosName.size());
    dst[0x00] = static_cast<std::uint8_t>((f.storageType << 4) | (nameLen & 0x0F));
    std::memcpy(dst + 1, f.prodosName.data(), nameLen);
    dst[0x10] = f.fileType;
    put16(dst + 0x11, keyPointer);
    put16(dst + 0x13, blocksUsed);
    put24(dst + 0x15, eof);
    // creation date_time = 0 (no metadata)
    dst[0x1C] = 0;     // version
    dst[0x1D] = 0;     // min_version
    dst[0x1E] = 0xE3;  // access: full
    put16(dst + 0x1F, f.auxType);
    // last_mod date_time = 0
    put16(dst + 0x25, headerPointer);
}

void writeSubdirEntryImpl(std::uint8_t* dst, const PreparedDir& sd,
                          std::uint16_t keyPointer, std::uint16_t blocksUsed,
                          std::uint16_t headerPointer)
{
    std::memset(dst, 0, kEntryLength);
    const std::uint8_t nameLen = static_cast<std::uint8_t>(sd.prodosName.size());
    dst[0x00] = static_cast<std::uint8_t>((kStorageSubdirEntry << 4) | (nameLen & 0x0F));
    std::memcpy(dst + 1, sd.prodosName.data(), nameLen);
    dst[0x10] = kFileTypeDir;
    put16(dst + 0x11, keyPointer);
    put16(dst + 0x13, blocksUsed);
    const std::uint32_t eof = static_cast<std::uint32_t>(blocksUsed) *
                              static_cast<std::uint32_t>(kBlockBytes);
    put24(dst + 0x15, eof);
    dst[0x1C] = 0;
    dst[0x1D] = 0;
    dst[0x1E] = 0xE3;
    put16(dst + 0x1F, 0);   // aux_type = 0 for subdirs
    put16(dst + 0x25, headerPointer);
}

void writeVolumeHeader(std::uint8_t* dst, const std::string& name,
                       std::uint16_t fileCount, std::uint16_t totalBlocks)
{
    std::memset(dst, 0, kEntryLength);
    const std::uint8_t nameLen = static_cast<std::uint8_t>(name.size());
    dst[0x00] = static_cast<std::uint8_t>((kStorageVolDir << 4) | (nameLen & 0x0F));
    std::memcpy(dst + 1, name.data(), nameLen);
    // Reserved bytes $10-$17 stay ZERO. A volume directory header carries no
    // marker — measured across every ProDOS image in the tree (10 of 10 have
    // $00 at entry offset $10; what varies after it is a modification date
    // some writers leave there). The marker below belongs to SUBDIRECTORY
    // headers and to nothing else.
    // creation date_time = 0
    dst[0x1C] = 0;
    dst[0x1D] = 0;
    dst[0x1E] = 0xC3;  // access: read/write/destroy/rename, no backup-needed
    dst[0x1F] = static_cast<std::uint8_t>(kEntryLength);
    dst[0x20] = static_cast<std::uint8_t>(kVolDirEntriesKN);
    put16(dst + 0x21, fileCount);
    put16(dst + 0x23, static_cast<std::uint16_t>(kBitmapBlock));
    put16(dst + 0x25, totalBlocks);
}

// Subdirectory header sits at offset 4 of the subdir's first block. Layout
// is parallel to writeVolumeHeader but with storage_type=$E + parent
// pointers and the ProDOS compatibility pattern in its reserved bytes.
void writeSubdirHeader(std::uint8_t* dst, const std::string& name,
                       std::uint16_t fileCount,
                       std::uint16_t parentBlock,
                       std::uint8_t  parentEntrySlot)
{
    std::memset(dst, 0, kEntryLength);
    const std::uint8_t nameLen = static_cast<std::uint8_t>(name.size());
    dst[0x00] = static_cast<std::uint8_t>((kStorageSubdirHeader << 4) | (nameLen & 0x0F));
    std::memcpy(dst + 1, name.data(), nameLen);
    // The subdirectory marker. ONE byte, at ENTRY offset $10, and the rest of
    // the reserved field stays zero.
    //
    // The "$14" in the ProDOS 8 TRM is BLOCK-relative — a directory block
    // opens with a 4-byte prev/next pair, so the header entry starts at block
    // offset 4 and the TRM's byte $14 is this entry's byte $10. That single
    // sentence is the whole of a confusion that produced two wrong versions
    // of this line: `dst[0x14] = 0x75` originally, then an eight-byte "magic
    // pattern" at $14. Neither is what ProDOS writes.
    //
    // Measured, not recalled, across every ProDOS image in the tree: 84 real
    // subdirectory headers carry $75 (61) or $76 (23) at entry offset $10,
    // and the seven bytes after it are zero in all but a handful — where they
    // hold a stale copy of the access/entry_length/entries_per_block trio
    // from a neighbouring field, which is where the folklore "pattern" came
    // from. $75 is the TRM's value and the commoner one.
    dst[0x10] = 0x75;
    dst[0x1C] = 0;
    dst[0x1D] = 0;
    dst[0x1E] = 0xC3;
    dst[0x1F] = static_cast<std::uint8_t>(kEntryLength);
    dst[0x20] = static_cast<std::uint8_t>(kVolDirEntriesKN);
    put16(dst + 0x21, fileCount);
    put16(dst + 0x23, parentBlock);
    dst[0x25] = parentEntrySlot;                            // 1-based
    dst[0x26] = static_cast<std::uint8_t>(kEntryLength);
}

// Clear (= used) the bits for blocks [first, lastEx) in the bitmap block.
void markBitmapUsed(std::vector<std::uint8_t>& image,
                    std::size_t first, std::size_t lastEx)
{
    std::uint8_t* bm = image.data() + kBitmapBlock * kBlockBytes;
    for (std::size_t b = first; b < lastEx; ++b) {
        const std::size_t byteIdx = b >> 3;
        const std::size_t bitIdx  = 7 - (b & 7);
        bm[byteIdx] &= static_cast<std::uint8_t>(~(1u << bitIdx));
    }
}

// Number of directory blocks needed to hold `entryCount` entries. Block 0 of
// any directory holds 12 entries (after the 39-byte header at offset 4) and
// every subsequent block holds 13. Always ≥ 1.
std::size_t numDirBlocksFor(std::size_t entryCount)
{
    if (entryCount <= kVolDirEntriesK0) return 1;
    return 1 + (entryCount - kVolDirEntriesK0 + kVolDirEntriesKN - 1)
                 / kVolDirEntriesKN;
}

// True iff `p` is `root` or lives under it. Component-wise, not a string
// prefix: "/srv/hostfolder2" starts with "/srv/hostfolder" and is not in it.
bool pathIsUnder(const fs::path& root, const fs::path& p)
{
    auto rIt = root.begin(), rEnd = root.end();
    auto pIt = p.begin(),    pEnd = p.end();
    for (; rIt != rEnd; ++rIt, ++pIt)
        if (pIt == pEnd || *pIt != *rIt) return false;
    return true;
}

// Recursively populate a PreparedDir from the given host folder. `usedNames`
// is per-directory (subdir name collisions don't conflict with parent dir
// names). `result` accumulates filesIncluded / filesSkipped counters.
//
// `root` + `visited` bound the walk. `directory_iterator` and `is_directory`
// both DEREFERENCE symlinks, so the previous version followed any link it
// met: `ln -s . loop` inside the folder recursed to the depth cap, and a link
// to `/` served the user's whole filesystem to the guest as a ProDOS volume
// (capped only by the 51 root slots and the 128 KB per-file limit). A link is
// followed only when its target is still inside the served folder, and the
// canonical path of every directory entered is remembered so an aliased or
// cyclic tree is walked once instead of exponentially.
void scanHostFolder(const fs::path& hostPath, PreparedDir& dir,
                    std::size_t depth, ProDOSBuildResult& result,
                    const fs::path& root,
                    std::unordered_set<std::string>& visited)
{
    if (depth > kMaxRecursionDepth) {
        pom2::log().warn("ProDOSVol",
            "skipping subtree, recursion depth exceeded: " + hostPath.string());
        return;
    }

    std::error_code ec;
    if (!fs::is_directory(hostPath, ec)) return;

    {
        std::error_code cec;
        const fs::path canon = fs::weakly_canonical(hostPath, cec);
        const std::string key = cec ? hostPath.string() : canon.string();
        if (!visited.insert(key).second) {
            pom2::log().warn("ProDOSVol",
                "skipping already-visited directory (link cycle): " +
                hostPath.string());
            return;
        }
    }

    std::vector<fs::path> children;
    for (const auto& entry : fs::directory_iterator(hostPath, ec)) {
        // Skip dotfiles (e.g. .DS_Store) — they pollute the synth volume
        // with platform metadata the guest can't make sense of.
        const std::string nm = entry.path().filename().string();
        if (!nm.empty() && nm.front() == '.') continue;
        std::error_code lec;
        if (entry.is_symlink(lec)) {
            std::error_code tec;
            const fs::path target = fs::weakly_canonical(entry.path(), tec);
            if (tec || !pathIsUnder(root, target)) {
                pom2::log().warn("ProDOSVol",
                    "skipping symlink out of the served folder: " + nm);
                ++result.filesSkipped;
                continue;
            }
        }
        if (entry.is_regular_file(ec) || entry.is_directory(ec)) {
            children.push_back(entry.path());
        }
    }
    std::sort(children.begin(), children.end());

    std::unordered_map<std::string, int> usedNames;
    for (const auto& path : children) {
        const std::size_t directChildCount = dir.order.size();
        const std::size_t budget =
            (depth == 0) ? kVolDirTotalSlots : (1u << 16);  // subdirs: large soft cap
        if (directChildCount >= budget) {
            ++result.filesSkipped;
            continue;
        }

        if (fs::is_directory(path, ec)) {
            auto sub = std::make_unique<PreparedDir>();
            sub->prodosName = uniqueName(sanitiseProDOSName(path.filename().string()),
                                         usedNames);
            scanHostFolder(path, *sub, depth + 1, result, root, visited);
            sub->numDirBlocks = numDirBlocksFor(sub->order.size());
            dir.order.push_back({true, dir.subdirs.size()});
            dir.subdirs.push_back(std::move(sub));
            continue;
        }

        // Regular file (existing logic, mostly).
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            pom2::log().warn("ProDOSVol",
                "skipping unreadable file: " + path.filename().string());
            ++result.filesSkipped;
            continue;
        }
        f.seekg(0, std::ios::end);
        const std::size_t fsize = static_cast<std::size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        if (fsize > kSaplingMaxBytes) {
            pom2::log().warn("ProDOSVol",
                "skipping oversized file (>128 KB): " + path.filename().string());
            ++result.filesSkipped;
            continue;
        }
        PreparedFile pf;
        pf.data.resize(fsize);
        if (fsize > 0) {
            f.read(reinterpret_cast<char*>(pf.data.data()),
                   static_cast<std::streamsize>(fsize));
            if (!f) {
                pom2::log().warn("ProDOSVol",
                    "short read, skipping: " + path.filename().string());
                ++result.filesSkipped;
                continue;
            }
        }
        // CiderPress / SD-CARD-OS metadata tag: "NAME#TTAAAA" carries the
        // ProDOS file type (TT) and aux type / load address (AAAA) in the host
        // filename — e.g. the HGR Paint editor saves pages as "PIC#062000"
        // (BIN at $2000), so a BLOAD needs no ,A override. The tag is stripped
        // from the ProDOS name; without one, the extension picks the type and
        // aux stays 0 (the historical behaviour).
        std::string hostName = path.filename().string();
        pf.fileType   = fileTypeFromExtension(path.extension().string());
        pf.auxType    = 0;
        {
            const std::size_t hash = hostName.rfind('#');
            if (hash != std::string::npos && hostName.size() - hash == 7) {
                bool hex = true;
                for (std::size_t i = hash + 1; i < hostName.size(); ++i)
                    if (!std::isxdigit(static_cast<unsigned char>(hostName[i])))
                        { hex = false; break; }
                if (hex) {
                    pf.fileType = static_cast<std::uint8_t>(
                        std::stoul(hostName.substr(hash + 1, 2), nullptr, 16));
                    pf.auxType = static_cast<std::uint16_t>(
                        std::stoul(hostName.substr(hash + 3, 4), nullptr, 16));
                    hostName.erase(hash);
                    if (hostName.empty()) hostName = "FILE";
                }
            }
        }
        pf.prodosName = uniqueName(sanitiseProDOSName(hostName), usedNames);
        if (fsize <= kBlockBytes) {
            pf.storageType = kStorageSeedling;
            pf.dataBlocks  = 1;
            pf.indexBlocks = 0;
        } else {
            pf.storageType = kStorageSapling;
            pf.dataBlocks  = (fsize + kBlockBytes - 1) / kBlockBytes;
            pf.indexBlocks = 1;
        }
        ++result.filesIncluded;
        dir.order.push_back({false, dir.files.size()});
        dir.files.push_back(std::move(pf));
    }
}

// Pointer to the entry slot at 0-based `slot` within `dirBlock`. For the
// volume directory's block 2, slot 0 is the volume header (caller must
// avoid it for file entries). For subdirs the same is true of slot 0 of
// the first dir block (subdir header).
std::uint8_t* dirSlotPtr(std::uint8_t* image, std::size_t dirBlock,
                         std::size_t slot)
{
    return image + dirBlock * kBlockBytes + 4 + slot * kEntryLength;
}

// Convert a 0-based "child index" within a directory to (dirBlock, slot)
// pair. The first dir block's slot 0 is reserved for the header, so child 0
// lands at (firstBlock, slot 1). Subsequent blocks use all 13 slots.
//
// Returns the 1-based ProDOS slot number (1..13) in `prodosSlot1Based`.
void childIndexToBlockSlot(std::size_t firstBlock, std::size_t childIndex,
                           std::size_t& outBlock, std::size_t& outSlotIn,
                           std::uint8_t& prodosSlot1Based)
{
    if (childIndex < kVolDirEntriesK0) {
        outBlock         = firstBlock;
        outSlotIn        = childIndex + 1;     // skip header
        prodosSlot1Based = static_cast<std::uint8_t>(outSlotIn + 1);  // ProDOS counts from 1
        return;
    }
    const std::size_t rest    = childIndex - kVolDirEntriesK0;
    const std::size_t blkOff  = rest / kVolDirEntriesKN + 1;          // +1 = next block
    const std::size_t slot    = rest % kVolDirEntriesKN;
    outBlock         = firstBlock + blkOff;
    outSlotIn        = slot;
    prodosSlot1Based = static_cast<std::uint8_t>(slot + 1);
}

// Lay down the data + (if sapling) index block for `f`. Updates `nextBlock`
// linearly, mirroring the original buildVolumeFromFolder allocation order
// so flat-volume tests stay byte-identical to the pre-subdir layout.
// Sets `f.firstBlock` (the seedling data block, or the sapling index block).
void writeFileData(std::vector<std::uint8_t>& image, PreparedFile& f,
                   std::size_t& nextBlock)
{
    auto blockPtr = [&](std::size_t b) { return image.data() + b * kBlockBytes; };
    if (f.storageType == kStorageSeedling) {
        const std::size_t db = nextBlock++;
        f.firstBlock = db;
        if (!f.data.empty()) {
            std::memcpy(blockPtr(db), f.data.data(),
                        std::min<std::size_t>(f.data.size(), kBlockBytes));
        }
        markBitmapUsed(image, db, db + 1);
    } else {
        const std::size_t idxBlk = nextBlock++;
        f.firstBlock = idxBlk;
        markBitmapUsed(image, idxBlk, idxBlk + 1);
        std::uint8_t* idx = blockPtr(idxBlk);
        for (std::size_t i = 0; i < f.dataBlocks; ++i) {
            const std::size_t db = nextBlock++;
            idx[i]       = static_cast<std::uint8_t>(db & 0xFF);
            idx[256 + i] = static_cast<std::uint8_t>((db >> 8) & 0xFF);
            const std::size_t off = i * kBlockBytes;
            const std::size_t len = std::min<std::size_t>(kBlockBytes, f.data.size() - off);
            std::memcpy(blockPtr(db), f.data.data() + off, len);
            markBitmapUsed(image, db, db + 1);
        }
    }
}

// Recursively emit `dir`: lay down its directory blocks (prev/next chain),
// header (volume or subdir), then for each child either write a file
// directory entry + its data, or recursively emit a child subdir then
// write the subdir entry.
//
// `nextBlock` is the next free data block. `dir.firstDirBlock` and
// `dir.numDirBlocks` must already be set by the caller (volume root: 2/4;
// subdirs: pre-allocated by emitDir's recursive caller).
void emitDir(std::vector<std::uint8_t>& image, PreparedDir& dir,
             std::size_t& nextBlock,
             std::size_t parentDirBlock, std::uint8_t parentEntrySlot,
             bool isVolumeRoot, const std::string& volumeName,
             std::uint16_t totalBlocks /* meaningful only for vol root */)
{
    auto blockPtr = [&](std::size_t b) { return image.data() + b * kBlockBytes; };

    // Linked list of dir blocks: prev=0, next=block+1, ..., last next=0.
    for (std::size_t i = 0; i < dir.numDirBlocks; ++i) {
        const std::size_t b      = dir.firstDirBlock + i;
        const std::size_t prev   = (i == 0) ? 0 : (b - 1);
        const std::size_t next   = (i + 1 < dir.numDirBlocks) ? (b + 1) : 0;
        put16(blockPtr(b) + 0, static_cast<std::uint16_t>(prev));
        put16(blockPtr(b) + 2, static_cast<std::uint16_t>(next));
    }

    // Mark the dir blocks as used in the bitmap.
    markBitmapUsed(image, dir.firstDirBlock,
                   dir.firstDirBlock + dir.numDirBlocks);

    // Header at offset 4 of the first dir block.
    const std::uint16_t childCount = static_cast<std::uint16_t>(dir.order.size());
    if (isVolumeRoot) {
        writeVolumeHeader(blockPtr(dir.firstDirBlock) + 4, volumeName,
                          childCount, totalBlocks);
    } else {
        writeSubdirHeader(blockPtr(dir.firstDirBlock) + 4, dir.prodosName,
                          childCount,
                          static_cast<std::uint16_t>(parentDirBlock),
                          parentEntrySlot);
    }

    // Walk children in the order they were inserted (alphabetical from the
    // scanner). For each child, either write a file entry + its data, or
    // recurse to emit the subdir before writing the subdir entry. We need
    // the subdir's keyPointer/blocksUsed BEFORE writing the parent entry,
    // so subdirs go first.
    const std::uint16_t headerPtr = static_cast<std::uint16_t>(dir.firstDirBlock);
    for (std::size_t childIdx = 0; childIdx < dir.order.size(); ++childIdx) {
        std::size_t  outBlock = 0;
        std::size_t  outSlot  = 0;
        std::uint8_t prodosSlot = 0;
        childIndexToBlockSlot(dir.firstDirBlock, childIdx,
                              outBlock, outSlot, prodosSlot);
        std::uint8_t* slot = dirSlotPtr(image.data(), outBlock, outSlot);

        const DirChild& dc = dir.order[childIdx];
        if (dc.isDir) {
            PreparedDir& sub = *dir.subdirs[dc.index];
            sub.firstDirBlock = nextBlock;
            nextBlock += sub.numDirBlocks;
            emitDir(image, sub, nextBlock,
                    /*parentDirBlock*/ outBlock,
                    /*parentEntrySlot*/ prodosSlot,
                    /*isVolumeRoot*/ false, volumeName, 0);
            writeSubdirEntryImpl(slot, sub,
                static_cast<std::uint16_t>(sub.firstDirBlock),
                static_cast<std::uint16_t>(sub.numDirBlocks),
                headerPtr);
        } else {
            PreparedFile& f = dir.files[dc.index];
            writeFileData(image, f, nextBlock);
            const std::uint16_t blocksUsed =
                static_cast<std::uint16_t>(f.dataBlocks + f.indexBlocks);
            writeFileEntry(slot, f,
                static_cast<std::uint16_t>(f.firstBlock),
                blocksUsed,
                static_cast<std::uint32_t>(f.data.size()),
                headerPtr);
        }
    }
}

} // namespace

// Recurse over the prepared tree and accumulate the total block count
// (dir blocks + file data + index blocks) for every node. Does NOT include
// structural blocks (0..6); the caller adds those.
namespace {
std::size_t totalBlocksForTree(const PreparedDir& dir, bool isVolumeRoot)
{
    std::size_t n = isVolumeRoot ? 0 : dir.numDirBlocks;     // vol dir is in 2..5
    for (const auto& f : dir.files) n += f.dataBlocks + f.indexBlocks;
    for (const auto& sd : dir.subdirs) n += totalBlocksForTree(*sd, false);
    return n;
}
}  // namespace

ProDOSBuildResult buildVolumeFromFolder(const std::string& hostFolder,
                                        const std::string& volumeName,
                                        std::vector<std::uint8_t>& outImage)
{
    ProDOSBuildResult result;

    // Sanitise the volume name once.
    std::string vname = sanitiseProDOSName(volumeName);
    if (vname.empty()) vname = "HOST";

    // Phase 1: walk the host folder tree (recursive).
    PreparedDir root;
    {
        std::error_code cec;
        fs::path served = fs::weakly_canonical(fs::path(hostFolder), cec);
        if (cec) served = fs::path(hostFolder);
        std::unordered_set<std::string> visited;
        scanHostFolder(hostFolder, root, /*depth=*/0, result, served, visited);
    }
    root.firstDirBlock = 2;
    root.numDirBlocks  = kVolDirBlocks;        // vol dir always 4 blocks (51 slots)

    // Phase 2: total block count.
    const std::size_t payloadBlocks = totalBlocksForTree(root, /*isVolumeRoot=*/true);

    // FREE SPACE. The volume used to be sized to fit its contents EXACTLY:
    // every block within total_blocks was marked used, so ProDOS reported
    // zero free blocks and the guest could not create, extend or re-SAVE a
    // single file. A folder mounted as a volume was read-only in practice
    // while presenting itself as writable — the guest got "DISK FULL" for a
    // 2-block file on an otherwise empty volume.
    //
    // New files ARE representable in the write-back: `decodeVolumeToFolder`
    // walks the directory graph and writes back every seedling/sapling entry
    // it finds, whether or not the build path put it there, and the volume
    // directory is always 4 blocks / 51 slots regardless of how many are
    // filled. So the slack is usable, not decorative.
    //
    // Bounded rather than generous: the image is a RAM allocation carried in
    // the snapshot payload, and the point is room to work, not a second hard
    // disk. 10 % of the content, at least 64 blocks (32 KB — an empty folder
    // still takes a few saves) and at most 4096 (2 MB).
    std::size_t slackBlocks = (kStructuralBeforeBitmap + payloadBlocks) / 10;
    if (slackBlocks < 64)   slackBlocks = 64;
    if (slackBlocks > 4096) slackBlocks = 4096;

    std::size_t bitmapBlocks = 1;
    std::size_t totalBlocks = 0;
    for (;;) {
        totalBlocks = kStructuralBeforeBitmap + bitmapBlocks + payloadBlocks +
                      slackBlocks;
        const std::size_t needed = (totalBlocks + kBlocksPerBitmap - 1) /
                                   kBlocksPerBitmap;
        if (needed == bitmapBlocks) break;
        bitmapBlocks = needed;
    }
    // The slack is a convenience, the content is not: give the slack back
    // before failing a volume that would otherwise have fitted.
    if (totalBlocks > kMaxVolumeBlocks && slackBlocks > 0) {
        const std::size_t over = totalBlocks - kMaxVolumeBlocks;
        slackBlocks = (over >= slackBlocks) ? 0 : (slackBlocks - over);
        totalBlocks = kStructuralBeforeBitmap + bitmapBlocks + payloadBlocks +
                      slackBlocks;
    }
    if (totalBlocks > kMaxVolumeBlocks) {
        result.error = "synthesised volume exceeds the 65535-block ProDOS limit";
        return result;
    }
    const std::size_t firstDataBlock = kStructuralBeforeBitmap + bitmapBlocks;
    if (totalBlocks < firstDataBlock) totalBlocks = firstDataBlock;

    outImage.assign(totalBlocks * kBlockBytes, 0);

    // Bitmap: initialise all bits within total_blocks as FREE, then mark
    // every structural block USED. emitDir clears bits as it lays down
    // dir + file blocks.
    {
        std::uint8_t* bm = outImage.data() + kBitmapBlock * kBlockBytes;
        for (std::size_t b = 0; b < totalBlocks; ++b) {
            const std::size_t byteIdx = b >> 3;
            const std::size_t bitIdx  = 7 - (b & 7);
            bm[byteIdx] |= static_cast<std::uint8_t>(1u << bitIdx);
        }
    }
    markBitmapUsed(outImage, 0, firstDataBlock);

    // Phase 3: emit. emitDir handles the linked-list prev/next chain for
    // the volume's 4 dir blocks AND the parallel chain for each subdir.
    std::size_t nextBlock = firstDataBlock;
    emitDir(outImage, root, nextBlock,
            /*parentDirBlock=*/0, /*parentEntrySlot=*/0,
            /*isVolumeRoot=*/true, vname,
            static_cast<std::uint16_t>(totalBlocks));

    result.ok          = true;
    result.totalBlocks = totalBlocks;
    return result;
}

namespace {

const char* extFromFileType(std::uint8_t t)
{
    // Inverse of fileTypeFromExtension. Default to .bin for anything we
    // didn't originally produce — keeps round-trip safe.
    switch (t) {
        case 0x00: return "";       // typeless → no extension (extensionless host file)
        case 0x04: return ".txt";
        case 0xFA: return ".int";
        case 0xFC: return ".bas";
        case 0xFF: return ".sys";
        case 0x06: default: return ".bin";
    }
}

inline std::uint16_t rd16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(p[1]) << 8;
}
inline std::uint32_t rd24(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) |
           static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16;
}

// Atomic overwrite: write to `dest`.tmp then rename — no torn file even on
// crash. Distinguishes a real write, an identical no-op, and an I/O failure so
// the caller never reports a failed guest-file export as successfully saved.
enum class FileWriteResult { Unchanged, Written, Error };

FileWriteResult writeFileAtomic(const fs::path& dest,
                                const std::vector<std::uint8_t>& bytes,
                                std::string& err)
{
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        std::ifstream in(dest, std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            const auto sz = static_cast<std::size_t>(in.tellg());
            if (sz == bytes.size()) {
                std::vector<std::uint8_t> have(sz);
                in.seekg(0, std::ios::beg);
                in.read(reinterpret_cast<char*>(have.data()),
                        static_cast<std::streamsize>(sz));
                if (in && have == bytes) return FileWriteResult::Unchanged;
            }
        }
    }
    // Unique per process + per call — a fixed `<dest>.tmp` is the name every
    // POM2 instance picks, and two flushing the same host folder truncated
    // each other's in-flight write. See tempSiblingPath().
    const fs::path tmp = tempSiblingPath(dest);
    std::error_code tmpEc;
    if (!prepareTempPath(tmp, tmpEc)) {
        err = "cannot prepare " + tmp.string() + ": " + tmpEc.message();
        return FileWriteResult::Error;
    }
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot open " + tmp.string() + " for write";
            fs::remove(tmp, ec);
            return FileWriteResult::Error;
        }
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        out.flush();
        out.close();
        if (!out) {
            fs::remove(tmp, ec);
            err = "write failed on " + tmp.string();
            return FileWriteResult::Error;
        }
    }
    if (!replaceFileAtomic(tmp, dest, ec)) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        err = "cannot replace " + dest.string() + ": " + ec.message();
        return FileWriteResult::Error;
    }
    return FileWriteResult::Written;
}

// Widen `newest` to cover `p`'s modification time. See
// ProDOSDecodeResult::completedAt for why the decode has to track this.
void noteWriteTime(const fs::path& p, fs::file_time_type& newest)
{
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (!ec && t > newest) newest = t;
}

}  // namespace

namespace {

// Per-decode walk state. `expanded` is GLOBAL to the walk, not per path:
// a directory block is expanded once and only once, so an aliased or
// self-referential key_pointer costs one skipped entry instead of an
// exponential subtree. `dirsLeft` is the second, independent bound — it
// survives even if some future entry type re-enters the walk.
struct DecodeWalk {
    const std::vector<std::uint8_t>&  image;
    std::size_t                       totalBlocks = 0;
    std::unordered_set<std::uint16_t> expanded;
    std::size_t                       dirsLeft    = kMaxDecodeDirs;
    ProDOSDecodeResult&               r;
    bool                              ioFailed    = false;
    /// Mount-time stamp: host files newer than this are preserved, not
    /// reverted (see decodeVolumeToFolder's doc). Null = legacy overwrite.
    const fs::file_time_type*         newerThan   = nullptr;
    /// Newest mtime this walk actually wrote — see completedAt.
    fs::file_time_type                newest{};
};

// Reserve `name` as a host filename inside one decoded directory, returning
// the name actually to use.
//
// TWO ProDOS entries can want ONE host name. The decode strips trailing dots
// before composing the host path (a name ending in '.' is legal in ProDOS and
// awkward-to-illegal on the host), so `README` and `README.` both come out as
// `README` — and the second write silently REPLACED the first, reporting both
// as written. The build path manufactures exactly that pair without trying:
// `sanitiseProDOSName` maps every character outside A-Z 0-9 '.' to '.', so a
// host folder holding `README` and `README!` becomes `README` and `README.`
// in the volume, `uniqueName` sees two distinct ProDOS names, and the
// write-back merges them back into one file. The guest can do it directly too
// — nothing stops it creating both names inside the volume.
//
// So: names are unique per decoded directory, and a clash gets a numeric
// suffix rather than overwriting. `used` covers THIS decode pass only, never
// what is already on disk, so re-decoding an unchanged volume still lands on
// the same names (writeFileAtomic's Unchanged path) instead of growing a new
// `.1` every time.
std::string reserveHostName(std::unordered_set<std::string>& used,
                            const std::string& name)
{
    if (used.insert(name).second) return name;
    for (int i = 1; i < 10000; ++i) {
        std::string cand = name + "." + std::to_string(i);
        if (used.insert(cand).second) return cand;
    }
    return name;                      // pathological — 10 000 clashing entries
}

// Walk one ProDOS directory (volume root or subdir) starting at `firstBlock`
// and recreate its contents under `hostFolder`. Recurses into subdir entries
// (storage_type $D) by creating a host subdirectory and calling itself.
// Termination rests on `w.expanded` (see DecodeWalk), not on `depth`.
void decodeOneDir(DecodeWalk& w,
                  std::uint16_t firstBlock,
                  const std::string& hostFolder,
                  std::size_t depth)
{
    ProDOSDecodeResult& r           = w.r;
    const std::size_t   totalBlocks = w.totalBlocks;
    if (w.ioFailed) return;

    if (depth > kMaxRecursionDepth) {
        pom2::log().warn("ProDOSVol",
            "decode: recursion depth exceeded under " + hostFolder);
        r.aborted = true;
        return;
    }

    auto blockPtr = [&](std::size_t b) -> const std::uint8_t* {
        return w.image.data() + b * kBlockBytes;
    };

    // Host names already emitted in THIS directory during THIS pass.
    std::unordered_set<std::string> usedHostNames;

    // What is already on disk here, indexed case-folded.
    //
    // The build path strips a known extension case-INSENSITIVELY, so the host
    // file `HELLO.BAS` becomes the ProDOS name `HELLO` with file_type $FC;
    // the decode composes the extension back from the type and it comes out
    // LOWER case. On a case-sensitive filesystem the write-back therefore
    // created `HELLO.bas` beside the user's `HELLO.BAS` — a second copy that
    // the next mount turned into two ProDOS entries, and so on every cycle.
    // Reusing the spelling that is already there keeps the round trip closed.
    std::unordered_map<std::string, std::string> existingHostNames;
    {
        std::error_code lec;
        for (const auto& de : fs::directory_iterator(hostFolder, lec)) {
            std::string actual = de.path().filename().string();
            std::string folded = actual;
            for (char& c : folded)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            existingHostNames.emplace(std::move(folded), std::move(actual));
        }
    }
    auto reuseExistingSpelling = [&](const std::string& wanted) {
        std::error_code xec;
        if (fs::exists(fs::path(hostFolder) / wanted, xec)) return wanted;
        std::string folded = wanted;
        for (char& c : folded)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto it = existingHostNames.find(folded);
        return (it == existingHostNames.end()) ? wanted : it->second;
    };

    std::uint16_t curBlock = firstBlock;
    std::size_t   guard    = 0;
    while (curBlock != 0 && guard++ < kMaxDirBlockChain) {
        if (curBlock >= totalBlocks) break;
        // Cycle / alias guard: a directory block reached a second time (via
        // a next-block pointer looping back, or a subdir key_pointer aliasing
        // a block already walked) would replay its entire subtree.
        if (!w.expanded.insert(curBlock).second) {
            pom2::log().warn("ProDOSVol",
                "decode: directory block " + std::to_string(curBlock) +
                " already walked (cyclic volume) — stopping chain under " +
                hostFolder);
            ++r.dirsSkipped;
            break;
        }
        const std::uint8_t* blk = blockPtr(curBlock);

        for (std::size_t slot = 0; slot < kVolDirEntriesKN; ++slot) {
            const std::size_t off = 4 + slot * kEntryLength;
            if (off + kEntryLength > kBlockBytes) break;
            const std::uint8_t* e = blk + off;
            const std::uint8_t storage = static_cast<std::uint8_t>(e[0] >> 4);
            const std::uint8_t nameLen = e[0] & 0x0F;
            if (storage == 0 || nameLen == 0)            continue;   // empty
            if (storage == kStorageVolDir ||
                storage == kStorageSubdirHeader)         continue;   // headers

            std::string name(reinterpret_cast<const char*>(e + 1), nameLen);
            const std::uint16_t keyPtr = rd16(e + 0x11);
            if (keyPtr == 0 || keyPtr >= totalBlocks) {
                ++r.filesSkipped;
                continue;
            }

            // Subdirectory entry → recurse.
            if (storage == kStorageSubdirEntry) {
                while (!name.empty() && name.back() == '.') name.pop_back();
                // The name is decoded from untrusted (guest-writable) image
                // bytes; reject anything that isn't a safe single component
                // before joining it to the host folder (path-traversal guard).
                if (!isHostSafeProDOSName(name)) {
                    pom2::log().warn("ProDOSVol",
                        "decode: skipping unsafe subdir name under " + hostFolder);
                    ++r.filesSkipped;
                    ++r.dirsSkipped;
                    continue;
                }
                // Refuse before touching the host filesystem: an entry whose
                // key_pointer names an already-walked block is a cycle, and
                // creating its directory would let a crafted volume mint host
                // directories for free.
                if (w.expanded.count(keyPtr) != 0) {
                    pom2::log().warn("ProDOSVol",
                        "decode: subdir '" + name + "' points at already-walked "
                        "block " + std::to_string(keyPtr) + " (cyclic volume) "
                        "— skipping");
                    ++r.dirsSkipped;
                    continue;
                }
                if (w.dirsLeft == 0) {
                    pom2::log().warn("ProDOSVol",
                        "decode: directory budget (" +
                        std::to_string(kMaxDecodeDirs) +
                        ") exhausted — host tree is partial");
                    ++r.dirsSkipped;
                    r.aborted = true;
                    return;
                }
                const fs::path subDest =
                    fs::path(hostFolder) / reserveHostName(usedHostNames, name);
                std::error_code ec;
                fs::create_directories(subDest, ec);
                if (ec) {
                    pom2::log().warn("ProDOSVol",
                        "cannot create subdir " + subDest.string() + ": " + ec.message());
                    ++r.filesSkipped;
                    ++r.dirsSkipped;
                    continue;
                }
                --w.dirsLeft;
                ++r.dirsCreated;
                decodeOneDir(w, keyPtr, subDest.string(), depth + 1);
                if (w.ioFailed) return;
                continue;
            }

            if (storage != kStorageSeedling && storage != kStorageSapling) {
                ++r.filesSkipped;                                    // tree / weird
                continue;
            }

            const std::uint8_t  fileType = e[0x10];
            const std::uint32_t eof      = rd24(e + 0x15);

            if (eof > kSaplingMaxBytes) {
                pom2::log().warn("ProDOSVol",
                    "skipping oversize file in decode: " + name);
                ++r.filesSkipped;
                continue;
            }

            std::vector<std::uint8_t> data;
            data.reserve(eof);

            if (storage == kStorageSeedling) {
                if (eof > kBlockBytes) {
                    // A seedling is defined as eof ≤ 512; a larger eof means
                    // an inconsistent entry (ProDOS promotes to sapling past
                    // 512). Warn rather than silently emit a 512-byte file.
                    pom2::log().warn("ProDOSVol",
                        "seedling entry claims eof>512, truncating: " + name);
                }
                const std::uint8_t* d = blockPtr(keyPtr);
                const std::size_t   take = std::min<std::size_t>(eof, kBlockBytes);
                data.insert(data.end(), d, d + take);
            } else {
                // Sapling: keyPtr → index block. Bytes 0..255 hold low bytes
                // of data block #s; bytes 256..511 hold the high bytes.
                const std::uint8_t* idx = blockPtr(keyPtr);
                std::size_t remaining = eof;
                for (std::size_t i = 0; i < 256 && remaining > 0; ++i) {
                    const std::uint16_t db =
                        static_cast<std::uint16_t>(idx[i]) |
                        static_cast<std::uint16_t>(idx[256 + i]) << 8;
                    const std::size_t take = std::min<std::size_t>(remaining, kBlockBytes);
                    if (db == 0) {
                        // Sparse hole: ProDOS reads an unallocated index entry
                        // back as a zero-filled block. Zero-fill and CONTINUE
                        // — the file's EOF terminates the read, not the hole.
                        data.insert(data.end(), take, 0u);
                    } else if (db >= totalBlocks) {
                        break;   // genuinely out-of-range pointer → stop
                    } else {
                        const std::uint8_t* d = blockPtr(db);
                        data.insert(data.end(), d, d + take);
                    }
                    remaining -= take;
                }
                if (data.size() < eof) {
                    pom2::log().warn("ProDOSVol",
                        "sapling file truncated on decode: " + name);
                }
            }

            // Compose host filename: ProDOS name + extension from file_type.
            // Strip any trailing dot the synth path may have left.
            while (!name.empty() && name.back() == '.') name.pop_back();
            // Reject names that aren't a safe single host component — the image
            // is guest-writable, so a crafted entry could carry '/' or '..'
            // and escape `hostFolder` (path-traversal guard).
            if (!isHostSafeProDOSName(name)) {
                pom2::log().warn("ProDOSVol",
                    "decode: skipping unsafe file name under " + hostFolder);
                ++r.filesSkipped;
                continue;
            }
            // Append a type-derived extension ONLY when the ProDOS name has
            // no extension of its own. Names that retain a dotted suffix
            // (sanitiseProDOSName keeps non-stripped extensions like ".DATA")
            // must NOT accrete a spurious ".bin" on every save cycle.
            const char* typeExt =
                (name.find('.') == std::string::npos) ? extFromFileType(fileType) : "";
            const fs::path dest =
                fs::path(hostFolder) /
                reserveHostName(usedHostNames,
                                reuseExistingSpelling(name + typeExt));
            // The volume is a snapshot taken at MOUNT time; a host file the
            // user edited since then is NEWER than that snapshot, and
            // rewriting it here would silently revert the user's edit to
            // the mount-time copy (reported as a successful save, no less).
            // Preserve it and say so — the guest's own writes leave the
            // host mtime alone, so they still land.
            if (w.newerThan) {
                std::error_code mec;
                const auto mtime = fs::last_write_time(dest, mec);
                if (!mec && mtime > *w.newerThan) {
                    pom2::log().warn("ProDOSVol",
                        "decode: preserving host-newer file " +
                        dest.filename().string() +
                        " (edited on the host after the volume was mounted;"
                        " the volume's stale copy was NOT written)");
                    ++r.filesSkipped;
                    continue;
                }
            }
            std::string writeErr;
            const FileWriteResult wr = writeFileAtomic(dest, data, writeErr);
            if (wr == FileWriteResult::Error) {
                r.error = writeErr;
                w.ioFailed = true;
                pom2::log().warn("ProDOSVol", "decode: " + r.error);
                return;
            }
            if (wr == FileWriteResult::Written) {
                ++r.filesWritten;
                noteWriteTime(dest, w.newest);
            }
        }
        // Next directory block pointer is at offset 2 of every dir block.
        curBlock = rd16(blk + 2);
    }
}

}  // namespace

bool isHostSafeProDOSName(const std::string& name)
{
    // A decoded entry name becomes a single host path component, so it must
    // not be empty, "." / "..", over-length, or contain anything outside the
    // ProDOS-legal set (which excludes '/', '\\', NUL → blocks traversal).
    if (name.empty() || name.size() > 15) return false;
    if (name == "." || name == "..")     return false;
    for (unsigned char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.';
        if (!ok) return false;
    }
    // Windows DOS-device names. `CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`,
    // `LPT1`-`LPT9` are reserved in EVERY directory and with ANY extension:
    // `AUX.txt` opens the serial port, not a file. They are also perfectly
    // legal ProDOS names — `AUX` is one a guest would plausibly write — so a
    // write-back that met one on Windows opened a device and either hung on
    // it or wrote the volume's bytes to a port. Refused for all platforms so
    // a host folder decoded on Linux stays decodable on Windows (and vice
    // versa): a name that is safe on one machine and a device on another is
    // not a portable store.
    std::string stem = name.substr(0, name.find('.'));
    for (char& c : stem)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* kDosDevices[] = { "CON", "PRN", "AUX", "NUL" };
    for (const char* d : kDosDevices)
        if (stem == d) return false;
    if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9' &&
        (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0))
        return false;
    return true;
}

ProDOSDecodeResult decodeVolumeToFolder(
    const std::vector<std::uint8_t>& image,
    const std::string& hostFolder,
    const std::filesystem::file_time_type* preserveNewerThan)
{
    ProDOSDecodeResult r;

    if (image.size() < (kMinFirstDataBlock * kBlockBytes)) {
        r.error = "volume image too small (need ≥ "
                  + std::to_string(kMinFirstDataBlock * kBlockBytes) + " bytes)";
        return r;
    }
    if ((image.size() % kBlockBytes) != 0) {
        r.error = "volume image is not a whole number of 512-byte blocks";
        return r;
    }
    const std::size_t totalBlocks = image.size() / kBlockBytes;

    std::error_code ec;
    fs::create_directories(hostFolder, ec);
    if (ec) {
        r.error = "cannot create host folder '" + hostFolder + "': " + ec.message();
        return r;
    }

    DecodeWalk walk{ image, totalBlocks, {}, kMaxDecodeDirs, r, false,
                     preserveNewerThan, {} };
    decodeOneDir(walk, /*firstBlock=*/2, hostFolder, /*depth=*/0);

    // The stamp the caller must adopt: no earlier than now, and no earlier
    // than anything this decode wrote.
    r.completedAt = fs::file_time_type::clock::now();
    if (walk.newest > r.completedAt) r.completedAt = walk.newest;

    if (walk.ioFailed) return r;

    if (r.aborted) {
        r.error = "volume directory graph exceeded the decode bounds; "
                  "host folder holds a partial tree";
        pom2::log().warn("ProDOSVol", "decode: " + r.error);
    }
    r.ok = true;
    return r;
}

} // namespace pom2
