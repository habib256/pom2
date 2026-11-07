// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// prodos_disk_fs — the in-place ProDOS file system (ProDOSDiskFs) against the
// ProDOS 8 Technical Reference Manual, Appendix B, and against a SECOND,
// independent implementation: POM2's own ProDOSVolume (the host-folder
// volume builder / decoder). Every volume this test builds is decoded by
// `decodeVolumeToFolder` and compared file by file, and a volume built by
// `buildVolumeFromFolder` is read, modified in place and decoded again.
//
// Sections:
//   1. format: geometry, header fields, bit map (own blocks used, blocks
//      past total_blocks never free).
//   2. write / read at the storage-type boundaries; TRM § B.3.1's worked
//      example reproduced block for block; replace returns blocks exactly.
//   3. directories: nesting, parent pointers, growth past 12 / 25 entries,
//      removal rules, free count back to the start.
//   4. limits and refusals, each leaving the image byte-identical.
//   5. the second opinion (ProDOSVolume).
//   6. hostile images: crafted cycles and pointers, random and mutated
//      volumes — errors or bounded results, never a crash or a hang.
//   7. DOS 3.3 order <-> ProDOS block order, against Beneath Apple ProDOS
//      Table 3.1 and against DiskImage.cpp's own skew tables.

#include "ProDOSDiskFs.h"
#include "ProDOSVolume.h"
#include "HostFileTag.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace pom2::prodosfs;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, msg)                                                     \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s (%s)\n", __FILE__, __LINE__,    \
                        #cond, std::string(msg).c_str());                        \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

uint32_t rd16(const std::vector<uint8_t>& img, std::size_t off)
{
    return uint32_t(img[off]) | (uint32_t(img[off + 1]) << 8);
}
uint32_t rd24(const std::vector<uint8_t>& img, std::size_t off)
{
    return rd16(img, off) | (uint32_t(img[off + 2]) << 16);
}
void wr16(std::vector<uint8_t>& img, std::size_t off, uint32_t v)
{
    img[off]     = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
uint32_t indexAt(const std::vector<uint8_t>& img, uint32_t block, unsigned i)
{
    return uint32_t(img[block * 512u + i]) | (uint32_t(img[block * 512u + 256 + i]) << 8);
}
bool bitFree(const std::vector<uint8_t>& img, uint32_t bitmapBlock, uint32_t b)
{
    return (img[bitmapBlock * 512u + (b >> 3)] & (0x80u >> (b & 7))) != 0;
}

/// Byte offset of the entry `name` in the directory whose key block is
/// `dirKey`, or -1. Test-side, for well-formed images only.
long rawEntry(const std::vector<uint8_t>& img, uint32_t dirKey, const std::string& name)
{
    uint32_t b = dirKey;
    for (int guard = 0; b != 0 && guard < 2000; ++guard) {
        for (unsigned i = (b == dirKey ? 1u : 0u); i < 13; ++i) {
            const std::size_t off = b * 512u + 4 + i * 39u;
            if (img[off] != 0 && (img[off] & 0x0F) == name.size() &&
                std::memcmp(&img[off + 1], name.data(), name.size()) == 0)
                return static_cast<long>(off);
        }
        b = rd16(img, b * 512u + 2);
    }
    return -1;
}

std::vector<uint8_t> pattern(std::size_t n, uint32_t seed)
{
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 12345u;
    for (auto& c : v) {
        x = x * 1103515245u + 12345u;
        c = static_cast<uint8_t>(x >> 16);
    }
    return v;
}

std::vector<uint8_t> fresh(uint32_t blocks, const char* name = "TEST")
{
    std::string err;
    auto img = ProDOSDisk::format(blocks, name, err);
    CHECK_MSG(!img.empty(), err);
    return img;
}

bool openOk(ProDOSDisk& d, std::vector<uint8_t> img)
{
    std::string err;
    const bool ok = d.open(std::move(img), err);
    CHECK_MSG(ok, err);
    return ok;
}

std::vector<uint8_t> readHost(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void writeHost(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// ----------------------------------------------------------- second opinion

fs::path g_tmp;
int g_decodeSerial = 0;

/// Every file of `d` (recursively) must come out of POM2's ProDOSVolume
/// decoder with the same bytes, under the host name its own convention
/// gives the (name, type, aux) triple, and nothing else may come out.
void compareWithDecoder(const ProDOSDisk& d, const std::string& label)
{
    const fs::path dest = g_tmp / ("decode_" + std::to_string(++g_decodeSerial));
    const auto r = pom2::decodeVolumeToFolder(d.image(), dest.string());
    CHECK_MSG(r.ok, label + ": decode failed: " + r.error);
    std::size_t expectedFiles = 0;
    std::vector<std::pair<std::string, fs::path>> pending{{"", dest}};
    while (!pending.empty()) {
        const auto [dirPath, hostDir] = pending.back();
        pending.pop_back();
        std::vector<Entry> entries;
        std::string err;
        CHECK_MSG(d.list(dirPath, entries, err), label + ": " + err);
        for (const Entry& e : entries) {
            const std::string path = dirPath.empty() ? e.name : dirPath + "/" + e.name;
            if (e.isDirectory) {
                CHECK_MSG(fs::is_directory(hostDir / e.name), label + ": no folder " + path);
                pending.push_back({path, hostDir / e.name});
                continue;
            }
            ++expectedFiles;
            const fs::path host = hostDir / pom2::hostNameForType(e.name, e.fileType, e.auxType);
            std::vector<uint8_t> mine;
            CHECK_MSG(d.readFile(path, mine, nullptr, err), label + ": " + err);
            CHECK_MSG(fs::is_regular_file(host), label + ": decoder wrote no " + host.string());
            CHECK_MSG(readHost(host) == mine, label + ": decoder disagrees on " + path);
        }
    }
    std::size_t hostFiles = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dest, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) ++hostFiles;
    CHECK_MSG(hostFiles == expectedFiles,
              label + ": decoder wrote " + std::to_string(hostFiles) + " files, expected " +
                  std::to_string(expectedFiles));
}

// ------------------------------------------------------------------ 1. format

void testFormat()
{
    for (uint32_t blocks : {280u, 1600u, 65535u}) {
        std::string err;
        auto img = ProDOSDisk::format(blocks, "Test.Vol", err);
        CHECK_MSG(img.size() == blocks * 512u, err);
        ProDOSDisk d;
        if (!openOk(d, img)) continue;
        const uint32_t bm = (blocks + 4095) / 4096;
        CHECK(d.volumeName() == "TEST.VOL");
        CHECK(d.totalBlocks() == blocks);
        CHECK(d.freeBlocks() == blocks - (2 + 4 + bm));
        // Header fields, TRM Fig. B-3 (block offsets).
        const std::size_t h = 2 * 512;
        CHECK(img[h + 4] == 0xF8);                // storage $F, name length 8
        CHECK(std::memcmp(&img[h + 5], "TEST.VOL", 8) == 0);
        CHECK(img[h + 0x23] == 0x27);
        CHECK(img[h + 0x24] == 0x0D);
        CHECK(rd16(img, h + 0x25) == 0);          // file_count
        CHECK(rd16(img, h + 0x27) == 6);          // bit_map_pointer
        CHECK(rd16(img, h + 0x29) == blocks);     // total_blocks
        // Directory chain 2 <-> 3 <-> 4 <-> 5 (TRM § B.2.1).
        for (uint32_t b = 2; b <= 5; ++b) {
            CHECK(rd16(img, b * 512) == (b == 2 ? 0 : b - 1));
            CHECK(rd16(img, b * 512 + 2) == (b == 5 ? 0 : b + 1));
        }
        // Boot, directory and bit map blocks used; everything else free;
        // nothing past total_blocks free in the last bit map block.
        for (uint32_t b = 0; b < bm * 4096; ++b) {
            const bool shouldBeFree = b >= 6 + bm && b < blocks;
            if (bitFree(img, 6, b) != shouldBeFree) {
                CHECK_MSG(false, "bit map wrong at block " + std::to_string(b));
                break;
            }
        }
        std::vector<Entry> entries;
        CHECK(d.list("", entries, err) && entries.empty());
        CHECK(d.list("/test.vol/", entries, err));
        CHECK_MSG(d.verify(err), err);
        if (blocks == 280) compareWithDecoder(d, "empty 280");
    }
    std::string err;
    CHECK(ProDOSDisk::format(6, "X", err).empty());
    CHECK(ProDOSDisk::format(65536, "X", err).empty());
    CHECK(ProDOSDisk::format(280, "1BAD", err).empty());
    CHECK(ProDOSDisk::format(280, "", err).empty());
    CHECK(!ProDOSDisk::format(7, "TINY", err).empty());
}

// -------------------------------------------------------- 2. write and read

void testWriteRead()
{
    ProDOSDisk d;
    if (!openOk(d, fresh(1600))) return;
    const uint16_t date = packDate(2026, 11, 5), time = packTime(12, 34);
    CHECK(date == ((26u << 9) | (11u << 5) | 5u));
    CHECK(time == ((12u << 8) | 34u));
    CHECK(packDate(1939, 1, 1) == 0 && packDate(2040, 1, 1) == 0);
    d.setTimestamp(date, time);
    struct Case { uint32_t size; uint8_t storage; uint16_t blocks; uint8_t type; uint16_t aux; };
    const Case cases[] = {
        {0, 1, 1, 0x06, 0x2000},       {1, 1, 1, 0x04, 0},
        {512, 1, 1, 0x00, 0},          {513, 2, 3, 0xFC, 0x0801},
        {131072, 2, 257, 0x06, 0x4000},
        {131073, 3, 260, 0x06, 0},     // 257 data + 2 index + master
        {307200, 3, 604, 0x06, 0},     // 600 data + 3 index + master
    };
    std::string err;
    for (const Case& c : cases) {
        const std::string name = "F" + std::to_string(c.size);
        const auto data = pattern(c.size, c.size);
        const uint32_t before = d.freeBlocks();
        CHECK_MSG(d.writeFile(name, c.type, c.aux, data, err), name + ": " + err);
        CHECK_MSG(d.freeBlocks() == before - c.blocks, name);
        std::vector<uint8_t> back;
        Entry e;
        CHECK_MSG(d.readFile(name, back, &e, err), err);
        CHECK_MSG(back == data, name);
        CHECK_MSG(e.storageType == c.storage, name);
        CHECK_MSG(e.blocksUsed == c.blocks, name);
        CHECK(e.eof == c.size && e.fileType == c.type && e.auxType == c.aux);
        CHECK(e.access == 0xE3 && !e.locked());
        CHECK(e.createDate == date && e.createTime == time && e.modDate == date);
        // header_pointer, version, min_version (TRM Fig. B-5).
        const long off = rawEntry(d.image(), 2, name);
        CHECK(off > 0);
        if (off > 0) {
            CHECK(rd16(d.image(), off + 0x25) == 2);
            CHECK(d.image()[off + 0x1C] == 0 && d.image()[off + 0x1D] == 0);
        }
    }
    CHECK_MSG(d.verify(err), err);
    compareWithDecoder(d, "write/read 1600");

    // TRM § B.3.1, "Growing a Tree File", on a 280-block disk: data block 0
    // at 7, index block 0 at 8, data blocks 1-255 at 9-263, the master index
    // at 264, index block 1 at 265, data block 256 at 266, 267-279 unused.
    {
        ProDOSDisk t;
        if (openOk(t, fresh(280))) {
            CHECK_MSG(t.writeFile("TREE", 0x06, 0, pattern(131073, 7), err), err);
            const auto& img = t.image();
            const long off = rawEntry(img, 2, "TREE");
            CHECK(off > 0);
            if (off > 0) {
                CHECK(img[off] >> 4 == 3);
                CHECK(rd16(img, off + 0x11) == 264);
                CHECK(rd16(img, off + 0x13) == 260);
                CHECK(rd24(img, off + 0x15) == 131073);
            }
            CHECK(indexAt(img, 264, 0) == 8);
            CHECK(indexAt(img, 264, 1) == 265);
            CHECK(indexAt(img, 264, 2) == 0);
            CHECK(indexAt(img, 8, 0) == 7);
            CHECK(indexAt(img, 8, 1) == 9);
            CHECK(indexAt(img, 8, 255) == 263);
            CHECK(indexAt(img, 265, 0) == 266);
            CHECK(indexAt(img, 265, 1) == 0);
            CHECK(t.freeBlocks() == 13);            // blocks 267-279
            for (uint32_t b = 267; b < 280; ++b) CHECK(bitFree(img, 6, b));
        }
    }

    // Replace: shorter, longer, and the free count comes back exactly.
    {
        ProDOSDisk r;
        if (!openOk(r, fresh(1600))) return;
        const uint32_t f0 = r.freeBlocks();
        r.setTimestamp(packDate(1999, 1, 2), packTime(3, 4));
        CHECK_MSG(r.writeFile("R", 0x06, 0x300, pattern(2000, 1), err), err);
        CHECK(r.freeBlocks() == f0 - 5);
        r.setTimestamp(packDate(2001, 5, 6), packTime(7, 8));
        CHECK_MSG(r.writeFile("r", 0x04, 0, pattern(100, 2), err), err);   // folded
        CHECK(r.freeBlocks() == f0 - 1);
        CHECK_MSG(r.writeFile("R", 0x06, 0x1234, pattern(70000, 3), err), err);
        CHECK(r.freeBlocks() == f0 - (137 + 1));
        std::vector<uint8_t> back;
        Entry e;
        CHECK(r.readFile("R", back, &e, err) && back == pattern(70000, 3));
        CHECK(e.auxType == 0x1234 && e.fileType == 0x06);
        CHECK(e.createDate == packDate(1999, 1, 2));   // creation kept
        CHECK(e.modDate == packDate(2001, 5, 6));
        std::vector<Entry> entries;
        CHECK(r.list("", entries, err) && entries.size() == 1);
        CHECK(rd16(r.image(), 2 * 512 + 0x25) == 1);   // file_count
        CHECK_MSG(r.removeFile("R", err), err);
        CHECK(r.freeBlocks() == f0);
        CHECK(rd16(r.image(), 2 * 512 + 0x25) == 0);
        CHECK(r.list("", entries, err) && entries.empty());
    }
}

// ------------------------------------------------------------ 3. directories

void testDirectories()
{
    ProDOSDisk d;
    if (!openOk(d, fresh(1600, "DIRS"))) return;
    std::string err;
    const uint32_t f0 = d.freeBlocks();
    CHECK_MSG(d.makeDirectory("A", err), err);
    CHECK_MSG(d.makeDirectory("a/B", err), err);
    CHECK(!d.makeDirectory("A/B", err));                     // exists
    CHECK_MSG(d.writeFile("A/B/F1", 0x06, 0x0800, pattern(3000, 9), err), err);
    std::vector<uint8_t> back;
    CHECK(d.readFile("/DIRS/A/B/F1", back, nullptr, err) && back == pattern(3000, 9));
    CHECK(d.readFile("/dirs/a/b/f1", back, nullptr, err));
    CHECK(!d.readFile("/OTHER/A/B/F1", back, nullptr, err));

    std::vector<Entry> root, inA;
    CHECK(d.list("", root, err) && root.size() == 1);
    CHECK(d.list("A", inA, err) && inA.size() == 1);
    if (root.size() != 1 || inA.size() != 1) return;
    const uint32_t keyA = root[0].keyPointer, keyB = inA[0].keyPointer;
    CHECK(root[0].isDirectory && root[0].fileType == 0x0F && root[0].blocksUsed == 1);
    CHECK(root[0].eof == 512);
    const auto& img = d.image();
    // Subdirectory header of B, TRM Fig. B-4 (block offsets).
    const std::size_t hb = keyB * 512u;
    CHECK(rd16(img, hb) == 0 && rd16(img, hb + 2) == 0);
    CHECK(img[hb + 4] == 0xE1 && img[hb + 5] == 'B');
    CHECK(img[hb + 0x14] == 0x75);                           // BAP p. 4-28
    CHECK(img[hb + 0x23] == 0x27 && img[hb + 0x24] == 0x0D);
    CHECK(rd16(img, hb + 0x25) == 1);                        // file_count: F1
    CHECK(rd16(img, hb + 0x27) == keyA);                     // parent_pointer
    CHECK(img[hb + 0x29] == 2);                              // first entry after the header
    CHECK(img[hb + 0x2A] == 0x27);
    CHECK(rawEntry(img, keyA, "B") == long(keyA * 512u + 4 + 39));
    CHECK(rd16(img, keyA * 512u + 4 + 39 + 0x25) == keyA);   // B's header_pointer

    // Growth: A holds B; G01..G11 fill its key block (12 entries).
    auto gname = [](int i) { char b[8]; std::snprintf(b, sizeof b, "G%02d", i); return std::string(b); };
    for (int i = 1; i <= 11; ++i)
        CHECK_MSG(d.writeFile("A/" + gname(i), 0x06, 0, pattern(10, i), err), err);
    CHECK(d.list("", root, err) && root.size() == 1 && root[0].blocksUsed == 1);
    const uint32_t beforeGrow = d.freeBlocks();
    CHECK_MSG(d.writeFile("A/" + gname(12), 0x06, 0, pattern(10, 12), err), err);   // 13th
    CHECK(d.freeBlocks() == beforeGrow - 2);                 // one dir block, one data
    CHECK(d.list("", root, err) && root.size() == 1);
    CHECK(root[0].blocksUsed == 2 && root[0].eof == 1024);
    const uint32_t second = rd16(d.image(), keyA * 512u + 2);
    CHECK(second != 0 && rd16(d.image(), second * 512u) == keyA);
    CHECK(rd16(d.image(), second * 512u + 2) == 0);
    CHECK(rawEntry(d.image(), keyA, gname(12)) == long(second * 512u + 4));
    CHECK(rd16(d.image(), second * 512u + 4 + 0x25) == keyA);   // header_pointer
    // A directory made in the second block: parent entry number 2 there.
    CHECK_MSG(d.makeDirectory("A/C", err), err);
    {
        std::vector<Entry> la;
        CHECK(d.list("A", la, err) && la.size() == 14);
        uint32_t keyC = 0;
        for (const Entry& e : la) if (e.name == "C") keyC = e.keyPointer;
        CHECK(keyC != 0);
        if (keyC) {
            CHECK(rd16(d.image(), keyC * 512u + 0x27) == second);
            CHECK(d.image()[keyC * 512u + 0x29] == 2);
        }
    }
    CHECK(rd16(d.image(), keyA * 512u + 4 + 0x21) == 14);    // A's file_count
    for (int i = 13; i <= 23; ++i)                            // 25 entries: still 2 blocks
        CHECK_MSG(d.writeFile("A/" + gname(i), 0x06, 0, pattern(10, i), err), err);
    CHECK(d.list("", root, err) && root[0].blocksUsed == 2);
    CHECK_MSG(d.writeFile("A/" + gname(24), 0x06, 0, pattern(10, 24), err), err);   // 26th
    CHECK(d.list("", root, err) && root[0].blocksUsed == 3 && root[0].eof == 1536);
    CHECK(d.list("A", inA, err) && inA.size() == 26);
    CHECK_MSG(d.verify(err), err);
    compareWithDecoder(d, "directories");

    // Removal rules.
    CHECK(!d.removeFile("A", err));                           // not empty
    CHECK(!d.removeFile("A/B", err));                         // holds F1
    CHECK_MSG(d.removeFile("A/B/F1", err), err);
    CHECK_MSG(d.removeFile("A/B", err), err);
    CHECK_MSG(d.removeFile("A/C", err), err);
    for (int i = 1; i <= 24; ++i) CHECK_MSG(d.removeFile("A/" + gname(i), err), err);
    CHECK(rd16(d.image(), keyA * 512u + 4 + 0x21) == 0);
    CHECK(d.list("A", inA, err) && inA.empty());
    CHECK_MSG(d.removeFile("A", err), err);
    CHECK(d.freeBlocks() == f0);
    CHECK(d.list("", root, err) && root.empty());
    CHECK_MSG(d.verify(err), err);
}

// -------------------------------------------------- 4. limits and refusals

void testRefusals()
{
    std::string err;
    {   // The volume directory: 12 + 3 x 13 = 51 entries, no more.
        ProDOSDisk d;
        if (!openOk(d, fresh(280))) return;
        for (int i = 1; i <= 51; ++i)
            CHECK_MSG(d.writeFile("N" + std::to_string(i), 0x06, 0, pattern(5, i), err), err);
        const auto before = d.image();
        CHECK(!d.writeFile("N52", 0x06, 0, pattern(5, 52), err));
        CHECK(err.find("full") != std::string::npos);
        CHECK(!d.makeDirectory("D52", err));
        CHECK(d.image() == before);
        CHECK(d.writeFile("N51", 0x06, 0, pattern(9, 1), err));   // replacing still works
        compareWithDecoder(d, "51 entries");
    }
    {   // Disk full: nothing changes.
        ProDOSDisk d;
        if (!openOk(d, fresh(280))) return;
        CHECK(d.writeFile("SMALL", 0x06, 0, pattern(600, 1), err));
        const auto before = d.image();
        CHECK(!d.writeFile("HUGE", 0x06, 0, pattern(140000, 2), err));
        CHECK(d.image() == before);
        // Replacing with something too big keeps the old file intact.
        CHECK(!d.writeFile("SMALL", 0x06, 0, pattern(140000, 2), err));
        CHECK(d.image() == before);
        // The directory grew, then the data did not fit: the growth is undone.
        CHECK(d.makeDirectory("D", err));
        for (int i = 0; i < 12; ++i)
            CHECK(d.writeFile("D/X" + std::to_string(i), 0x06, 0, pattern(1, i), err));
        const auto full = d.image();
        const uint32_t free = d.freeBlocks();
        CHECK(!d.writeFile("D/BIG", 0x06, 0, pattern(free * 512, 3), err));
        CHECK(d.image() == full);
        // free - 2 data blocks (a sapling, 255 of them) + index + the new
        // directory block = exactly what is left.
        CHECK(free - 2 <= 256);
        CHECK(d.writeFile("D/FITS", 0x06, 0, pattern((free - 2) * 512, 3), err));
        CHECK(d.freeBlocks() == 0);
        CHECK_MSG(d.verify(err), err);
    }
    {   // Names, paths, types, locks.
        ProDOSDisk d;
        if (!openOk(d, fresh(280, "NAMES"))) return;
        const auto before = d.image();
        for (const char* bad : {"", "1ABC", "A B", "ABCDEFGHIJKLMNOP", "A_B", "/", "/OTHER/X",
                                "A-B", "\xC1", ".A"})
            CHECK_MSG(!d.writeFile(bad, 0x06, 0, pattern(3, 1), err), bad);
        CHECK(d.image() == before);
        CHECK(isValidName("ABCDEFGHIJKLMNO"));
        CHECK_MSG(d.writeFile("hello.txt", 0x04, 0, pattern(3, 1), err), err);
        std::vector<Entry> entries;
        CHECK(d.list("", entries, err) && entries.size() == 1 && entries[0].name == "HELLO.TXT");
        CHECK(d.writeFile("/names/Z9.Q", 0x06, 0, pattern(3, 1), err));
        CHECK(d.makeDirectory("DIR", err));
        auto snap = d.image();
        CHECK(!d.writeFile("DIR", 0x06, 0, pattern(3, 1), err));          // over a directory
        CHECK(!d.writeFile("NEWDIR", 0x0F, 0, pattern(3, 1), err));       // type $0F
        CHECK(!d.writeFile("HELLO.TXT/X", 0x06, 0, pattern(3, 1), err));  // through a file
        CHECK(!d.makeDirectory("HELLO.TXT/X", err));
        CHECK(!d.list("HELLO.TXT", entries, err));
        CHECK(!d.makeDirectory("HELLO.TXT", err));                        // exists
        CHECK(!d.removeFile("NOPE", err));
        CHECK(!d.writeFile("NOPE/X", 0x06, 0, pattern(3, 1), err));
        std::vector<uint8_t> scratch;
        CHECK(!d.readFile("DIR", scratch, nullptr, err));                 // a directory
        CHECK(d.image() == snap);
        // A name ProDOS 8 would not create but that is on a disk (A2osX
        // writes `.PROFILE`): reachable to read and remove, never created.
        {
            ProDOSDisk o;
            if (openOk(o, d.image())) {
                CHECK(o.writeFile("XPROFILE", 0x04, 0, pattern(40, 8), err));
                auto odd = o.image();
                const long at = rawEntry(odd, 2, "XPROFILE");
                CHECK(at > 0);
                if (at > 0) {
                    odd[at + 1] = '.';
                    ProDOSDisk p;
                    if (openOk(p, odd)) {
                        std::vector<uint8_t> v;
                        CHECK(p.readFile(".PROFILE", v, nullptr, err) && v == pattern(40, 8));
                        CHECK(!p.writeFile(".OTHER", 0x04, 0, pattern(4, 1), err));
                        CHECK(!p.makeDirectory(".DIR", err));
                        CHECK_MSG(p.removeFile(".PROFILE", err), err);
                    }
                }
            }
        }
        // Locked: write and destroy disabled.
        auto img = d.image();
        const long off = rawEntry(img, 2, "HELLO.TXT");
        CHECK(off > 0);
        if (off > 0) {
            img[off + 0x1E] = 0x21;   // read + backup only
            ProDOSDisk l;
            if (openOk(l, img)) {
                std::vector<Entry> le;
                CHECK(l.list("", le, err) && !le.empty() && le[0].locked());
                CHECK(!l.writeFile("HELLO.TXT", 0x04, 0, pattern(3, 2), err));
                CHECK(!l.removeFile("HELLO.TXT", err));
                CHECK(l.image() == img);
                std::vector<uint8_t> back;
                CHECK(l.readFile("HELLO.TXT", back, nullptr, err));   // reading is fine
            }
        }
    }
}

// TN #25's storage types: a $5 extended file (an IIgs System disk is full
// of them) and a $4 Pascal area. `verify` refused both, so one such entry
// blocked every put / rm / mkdir on the volume (bug hunt 2026-11-07). Their
// blocks are now claimed: writes go round them, never over them.
void setBit(std::vector<uint8_t>& img, uint32_t b, bool free)
{
    uint8_t& byte = img[6 * 512u + (b >> 3)];
    const uint8_t bit = static_cast<uint8_t>(0x80u >> (b & 7));
    byte = free ? static_cast<uint8_t>(byte | bit) : static_cast<uint8_t>(byte & ~bit);
}

void testNonStandardStorage()
{
    std::string err;
    {   // $5: the key block's two mini-entries, data fork at +$000 and
        // resource fork at +$100, each a standard fork.
        ProDOSDisk d;
        if (!openOk(d, fresh(280))) return;
        CHECK(d.writeFile("XFILE", 0x06, 0, pattern(512, 1), err));   // its key becomes the extended key
        CHECK(d.writeFile("DFORK", 0x06, 0, pattern(600, 2), err));   // sapling: index + 2
        CHECK(d.writeFile("RFORK", 0x06, 0, pattern(100, 3), err));   // seedling
        auto img = d.image();
        const long x = rawEntry(img, 2, "XFILE"), df = rawEntry(img, 2, "DFORK"),
                   rf = rawEntry(img, 2, "RFORK");
        CHECK(x > 0 && df > 0 && rf > 0);
        if (x <= 0 || df <= 0 || rf <= 0) return;
        const uint32_t xkey = rd16(img, std::size_t(x) + 0x11);
        uint8_t* xk = &img[xkey * 512u];
        std::fill(xk, xk + 512, uint8_t(0));
        xk[0x000] = 0x2; wr16(img, xkey * 512u + 0x001, rd16(img, std::size_t(df) + 0x11));
        wr16(img, xkey * 512u + 0x003, 3); wr16(img, xkey * 512u + 0x005, 600);
        xk[0x100] = 0x1; wr16(img, xkey * 512u + 0x101, rd16(img, std::size_t(rf) + 0x11));
        wr16(img, xkey * 512u + 0x103, 1); wr16(img, xkey * 512u + 0x105, 100);
        img[std::size_t(x)] = static_cast<uint8_t>(0x50 | (img[std::size_t(x)] & 0x0F));
        wr16(img, std::size_t(x) + 0x13, 5);
        img[std::size_t(df)] = 0;                         // the forks are no files of their own
        img[std::size_t(rf)] = 0;
        wr16(img, 2 * 512 + 0x25, 1);                     // file_count
        ProDOSDisk g;
        if (!openOk(g, img)) return;
        CHECK_MSG(g.verify(err), err);
        const uint32_t free0 = g.freeBlocks();
        CHECK_MSG(g.writeFile("NEW", 0x06, 0, pattern(3000, 4), err), err);
        CHECK_MSG(g.verify(err), err);                    // NEW took none of the forks' blocks
        CHECK(g.makeDirectory("SUB", err));
        std::vector<uint8_t> v;
        CHECK(!g.readFile("XFILE", v, nullptr, err));     // two forks: not one file's bytes
        const auto before = g.image();
        CHECK(!g.writeFile("XFILE", 0x06, 0, pattern(5, 5), err));   // replacing would drop a fork
        CHECK(g.image() == before);
        const uint32_t free1 = g.freeBlocks();
        CHECK_MSG(g.removeFile("XFILE", err), err);       // key + index + 2 data + resource
        CHECK(g.freeBlocks() == free1 + 5);
        CHECK_MSG(g.verify(err), err);
        CHECK(free1 < free0);
        // A mini-entry that is no standard fork is still refused.
        img[xkey * 512u + 0x100] = 0x7;
        ProDOSDisk h;
        if (openOk(h, img)) {
            CHECK(!h.verify(err) && err.find("resource fork") != std::string::npos);
            CHECK(!h.writeFile("NEW", 0x06, 0, pattern(5, 5), err));
        }
    }
    {   // $4: key_pointer = TOTAL_BLOCKS - 2, blocks_used = 2.
        ProDOSDisk d;
        if (!openOk(d, fresh(280))) return;
        CHECK(d.writeFile("PASCAL.AREA", 0xEF, 0, pattern(10, 1), err));
        auto img = d.image();
        const long a = rawEntry(img, 2, "PASCAL.AREA");
        CHECK(a > 0);
        if (a <= 0) return;
        setBit(img, rd16(img, std::size_t(a) + 0x11), true);   // the seedling's block back
        img[std::size_t(a)] = static_cast<uint8_t>(0x40 | (img[std::size_t(a)] & 0x0F));
        wr16(img, std::size_t(a) + 0x11, 278);
        wr16(img, std::size_t(a) + 0x13, 2);
        setBit(img, 278, false);
        setBit(img, 279, false);
        ProDOSDisk g;
        if (!openOk(g, img)) return;
        CHECK_MSG(g.verify(err), err);
        CHECK_MSG(g.writeFile("NEW", 0x06, 0, pattern(1000, 2), err), err);
        CHECK_MSG(g.verify(err), err);
        const auto before = g.image();
        CHECK(!g.removeFile("PASCAL.AREA", err));         // PPM's, not ours
        CHECK(g.image() == before);
        // An area the bit map does not hold is inconsistent: refused.
        setBit(img, 279, true);
        ProDOSDisk h;
        if (openOk(h, img)) CHECK(!h.verify(err));
    }
}

// --------------------------------------------------- 5. ProDOSVolume volumes

void testBuiltVolume()
{
    const fs::path src = g_tmp / "host_src";
    std::error_code ec;
    fs::create_directories(src / "sub", ec);
    std::string helloText = "HELLO WORLD\r";
    const std::vector<uint8_t> hello(helloText.begin(), helloText.end());
    const auto prog = pattern(1000, 11);
    auto big = pattern(140000, 12);
    std::fill(big.begin() + 1024, big.begin() + 2048, 0);   // a hole the builder skips
    const auto pic = pattern(8192, 13);
    const auto inner = pattern(700, 14);
    writeHost(src / "hello.txt", hello);
    writeHost(src / "prog.bin", prog);
    writeHost(src / "big.bin", big);
    writeHost(src / "PIC#062000", pic);
    writeHost(src / "sub" / "inner.txt", inner);

    std::vector<uint8_t> img;
    const auto built = pom2::buildVolumeFromFolder(src.string(), "HOST", img);
    CHECK_MSG(built.ok, built.error);
    if (!built.ok) return;
    ProDOSDisk d;
    if (!openOk(d, img)) return;
    std::string err;
    CHECK(d.volumeName() == "HOST");
    CHECK_MSG(d.verify(err), err);
    std::vector<Entry> root;
    CHECK(d.list("", root, err) && root.size() == 5);
    auto get = [&](const std::string& path, Entry* e = nullptr) {
        std::vector<uint8_t> v;
        std::string why;
        CHECK_MSG(d.readFile(path, v, e, why), path + ": " + why);
        return v;
    };
    Entry e;
    CHECK(get("HELLO", &e) == hello);
    CHECK(e.fileType == 0x04);
    CHECK(get("PROG", &e) == prog);
    CHECK(e.fileType == 0x06);
    CHECK(get("BIG", &e) == big);
    CHECK(e.storageType == 3);
    CHECK(e.blocksUsed == 275);   // 274 chunks, 2 sparse = 272 data; 2 index + master
    CHECK(get("PIC", &e) == pic);
    CHECK(e.fileType == 0x06 && e.auxType == 0x2000);
    CHECK(get("SUB/INNER") == inner);
    compareWithDecoder(d, "built volume");

    // Modify in place, decode again.
    CHECK_MSG(d.writeFile("SUB/NEW.FILE", 0x06, 0x0300, pattern(5000, 15), err), err);
    CHECK_MSG(d.removeFile("PROG", err), err);
    CHECK_MSG(d.makeDirectory("NEWDIR", err), err);
    CHECK_MSG(d.writeFile("NEWDIR/X", 0x06, 0, pattern(3000, 16), err), err);
    CHECK_MSG(d.writeFile("HELLO", 0x04, 0, pattern(900, 17), err), err);
    CHECK_MSG(d.verify(err), err);
    CHECK(get("SUB/NEW.FILE") == pattern(5000, 15));
    CHECK(get("BIG") == big);
    compareWithDecoder(d, "built volume, modified");
}

// --------------------------------------------------------- 6. hostile images

/// Throws everything at `d`; every refused mutation must leave the image as
/// it was. Returns nothing: the point is that it returns at all.
void exercise(ProDOSDisk& d)
{
    std::string err;
    std::vector<Entry> entries;
    std::vector<uint8_t> data;
    if (d.list("", entries, err)) {
        for (const Entry& e : entries) {
            std::string folded;
            if (!isValidName(e.name, &folded)) continue;
            std::vector<Entry> sub;
            if (e.isDirectory) d.list(folded, sub, err);
            d.readFile(folded, data, nullptr, err);
        }
    }
    // A refused mutation changes nothing; an accepted one on a volume that
    // verified leaves it verifiable.
    auto mutate = [&](bool ok, const std::vector<uint8_t>& before, bool wasConsistent) {
        if (!ok) CHECK(d.image() == before);
        else if (wasConsistent) CHECK_MSG(d.verify(err), err);
    };
    std::vector<uint8_t> before = d.image();
    bool consistent = d.verify(err);
    mutate(d.writeFile("NEWF", 0x06, 0, pattern(1500, 1), err), before, consistent);
    before = d.image();
    consistent = d.verify(err);
    mutate(d.makeDirectory("NEWD", err), before, consistent);
    if (!entries.empty()) {
        std::string folded;
        if (isValidName(entries[0].name, &folded)) {
            before = d.image();
            consistent = d.verify(err);
            mutate(d.removeFile(folded, err), before, consistent);
        }
    }
    d.freeBlocks();
}

std::vector<uint8_t> populated()
{
    ProDOSDisk d;
    std::string err;
    std::vector<uint8_t> none;
    if (!d.open(fresh(800, "HOSTILE"), err)) return none;
    d.writeFile("BIGTREE", 0x06, 0, pattern(140000, 5), err);
    d.writeFile("SEED", 0x06, 0, pattern(100, 1), err);
    d.writeFile("SAP", 0x06, 0, pattern(3000, 2), err);
    d.writeFile("MID", 0x06, 0, pattern(30000, 3), err);
    d.makeDirectory("DIR", err);
    d.writeFile("DIR/IN", 0x06, 0, pattern(700, 4), err);
    return d.image();
}

void testHostile()
{
    std::string err;
    std::vector<uint8_t> data;
    std::vector<Entry> entries;

    {   // A directory whose next pointer points at itself.
        auto img = fresh(280);
        wr16(img, 2 * 512 + 2, 2);
        ProDOSDisk d;
        if (openOk(d, img)) {
            CHECK(!d.list("", entries, err));
            CHECK(err.find("loops") != std::string::npos);
            CHECK(!d.readFile("X", data, nullptr, err));
            CHECK(!d.writeFile("X", 0x06, 0, pattern(4, 1), err));
            CHECK(d.image() == img);
            CHECK(!d.verify(err));
        }
    }
    {   // A sapling whose index points past the end of the volume.
        ProDOSDisk d;
        if (openOk(d, fresh(280))) {
            CHECK(d.writeFile("S", 0x06, 0, pattern(2000, 1), err));
            auto img = d.image();
            const long off = rawEntry(img, 2, "S");
            const uint32_t idx = rd16(img, off + 0x11);
            img[idx * 512 + 1] = 0xFF;
            img[idx * 512 + 256 + 1] = 0xFF;          // pointer 1 = $FFFF
            ProDOSDisk h;
            if (openOk(h, img)) {
                CHECK(!h.readFile("S", data, nullptr, err));
                CHECK(data.empty());
                CHECK(!h.verify(err));
                CHECK(!h.writeFile("OTHER", 0x06, 0, pattern(4, 1), err));
                CHECK(!h.removeFile("S", err));
                CHECK(h.image() == img);
            }
        }
    }
    {   // total_blocks larger than the image; impossible bit map; bad header.
        auto img = fresh(280);
        wr16(img, 2 * 512 + 0x29, 281);
        ProDOSDisk d;
        CHECK(!d.open(img, err));
        wr16(img, 2 * 512 + 0x29, 0xFFFF);
        CHECK(!d.open(img, err));
        img = fresh(280);
        wr16(img, 2 * 512 + 0x27, 280);               // bit map past the end
        CHECK(!d.open(img, err));
        wr16(img, 2 * 512 + 0x27, 2);                 // bit map over the directory
        CHECK(!d.open(img, err));
        wr16(img, 2 * 512 + 0x27, 279);               // odd but legal: last block
        CHECK(d.open(img, err));
        img = fresh(280);
        img[2 * 512 + 0x23] = 0x28;
        CHECK(!d.open(img, err));
        img = fresh(280);
        img[2 * 512 + 4] = 0xE4;
        CHECK(!d.open(img, err));
        CHECK(!d.open(std::vector<uint8_t>(1000), err));
        CHECK(!d.open(std::vector<uint8_t>(1024), err));
        CHECK(!d.open({}, err));
        CHECK(!d.open(std::vector<uint8_t>(65537u * 512u), err));
    }
    {   // A file whose key pointer is block 0.
        ProDOSDisk d;
        if (openOk(d, fresh(280))) {
            CHECK(d.writeFile("K", 0x06, 0, pattern(100, 1), err));
            auto img = d.image();
            const long off = rawEntry(img, 2, "K");
            wr16(img, off + 0x11, 0);
            ProDOSDisk h;
            if (openOk(h, img)) {
                CHECK(!h.readFile("K", data, nullptr, err));
                CHECK(!h.removeFile("K", err));
                CHECK(!h.writeFile("K", 0x06, 0, pattern(4, 1), err));
                CHECK(h.image() == img);
            }
        }
    }
    {   // A block a file needs is marked free in the bit map: no writes.
        ProDOSDisk d;
        if (openOk(d, fresh(280))) {
            CHECK(d.writeFile("F", 0x06, 0, pattern(100, 1), err));
            auto img = d.image();
            const uint32_t key = rd16(img, rawEntry(img, 2, "F") + 0x11);
            img[6 * 512 + (key >> 3)] |= static_cast<uint8_t>(0x80u >> (key & 7));
            ProDOSDisk h;
            if (openOk(h, img)) {
                CHECK(h.readFile("F", data, nullptr, err) && data == pattern(100, 1));
                CHECK(!h.writeFile("G", 0x06, 0, pattern(4, 1), err));
                CHECK(err.find("bit map") != std::string::npos);
                CHECK(!h.removeFile("F", err));
                CHECK(!h.makeDirectory("D", err));
                CHECK(h.image() == img);
            }
        }
    }
    {   // Two files sharing a block; a subdirectory pointing at the root;
        // a subdirectory pointing at its own parent's block.
        ProDOSDisk d;
        if (openOk(d, fresh(280))) {
            CHECK(d.writeFile("A", 0x06, 0, pattern(100, 1), err));
            CHECK(d.writeFile("B", 0x06, 0, pattern(100, 2), err));
            CHECK(d.makeDirectory("S", err));
            auto img = d.image();
            const long a = rawEntry(img, 2, "A"), b = rawEntry(img, 2, "B");
            wr16(img, b + 0x11, rd16(img, a + 0x11));
            ProDOSDisk h;
            if (openOk(h, img)) {
                CHECK(!h.verify(err));
                CHECK(!h.writeFile("C", 0x06, 0, pattern(4, 1), err));
            }
            img = d.image();
            wr16(img, rawEntry(img, 2, "S") + 0x11, 2);
            if (openOk(h, img)) {
                CHECK(!h.list("S", entries, err));
                CHECK(!h.verify(err));
            }
            // S's key block made a member of its own chain via next.
            img = d.image();
            const uint32_t keyS = rd16(img, rawEntry(img, 2, "S") + 0x11);
            wr16(img, keyS * 512 + 2, keyS);
            if (openOk(h, img)) {
                CHECK(!h.list("S", entries, err));
                CHECK(!h.writeFile("S/X", 0x06, 0, pattern(4, 1), err));
                CHECK(h.image() == img);
            }
        }
    }
    {   // A tree whose master index points past the end.
        ProDOSDisk d;
        if (openOk(d, fresh(1600))) {
            CHECK(d.writeFile("T", 0x06, 0, pattern(200000, 1), err));
            auto img = d.image();
            const uint32_t master = rd16(img, rawEntry(img, 2, "T") + 0x11);
            img[master * 512 + 1] = 0x40;
            img[master * 512 + 256 + 1] = 0x06;      // index block 1 = $0640 = 1600
            ProDOSDisk h;
            if (openOk(h, img)) {
                CHECK(!h.readFile("T", data, nullptr, err));
                CHECK(!h.verify(err));
            }
        }
    }

    // Random images of a valid size: half of them with a plausible header,
    // so the walks get past open().
    std::mt19937 rng(20261105u);
    int opened = 0, stillConsistent = 0;
    for (int iter = 0; iter < 300; ++iter) {
        std::vector<uint8_t> img(280 * 512);
        for (auto& c : img) c = static_cast<uint8_t>(rng());
        if (iter % 2 == 0) {
            uint8_t* h = img.data() + 2 * 512 + 4;
            h[0] = 0xF4;
            std::memcpy(h + 1, "RAND", 4);
            h[0x1F] = 0x27;
            h[0x20] = 0x0D;
            h[0x23] = 6;  h[0x24] = 0;
            h[0x25] = 280 & 0xFF; h[0x26] = 280 >> 8;
            if (iter % 4 == 0) {   // and a one-block root of random entries
                wr16(img, 2 * 512, 0);
                wr16(img, 2 * 512 + 2, 0);
            }
        }
        ProDOSDisk d;
        if (d.open(img, err)) { exercise(d); ++opened; }
    }
    // Mutated real volumes: a few random bytes anywhere past the boot blocks.
    const auto base = populated();
    CHECK(!base.empty());
    if (!base.empty()) {
        for (int iter = 0; iter < 1500; ++iter) {
            auto img = base;
            const int flips = 1 + int(rng() % 8);
            for (int k = 0; k < flips; ++k) {
                // Bias towards the metadata: directory, bit map, and the
                // first blocks where the files' index blocks are.
                const std::size_t region = (rng() % 3 == 0) ? img.size() : 40 * 512;
                const std::size_t at = 1024 + rng() % (region - 1024);
                img[at] = static_cast<uint8_t>(rng());
            }
            ProDOSDisk d;
            if (d.open(img, err)) {
                exercise(d);
                ++opened;
                if (d.verify(err)) ++stillConsistent;
            }
        }
    }
    std::printf("prodos_disk_fs: hostile: %d images opened and exercised, "
                "%d mutated volumes still consistent afterwards\n", opened, stillConsistent);
}

// ------------------------------------------------- 7. 5.25" sector ordering

bool parseTable(const std::string& text, const std::string& name, int out[16])
{
    const auto at = text.find(name + "[16] = {");
    if (at == std::string::npos) return false;
    std::istringstream in(text.substr(text.find('{', at) + 1));
    for (int i = 0; i < 16; ++i) {
        if (!(in >> out[i])) return false;
        char comma = 0;
        if (i < 15 && !(in >> comma)) return false;
    }
    return true;
}

void testSectorOrder()
{
    // Beneath Apple ProDOS Table 3.1 (p. 3-48), DOS 3.3 row: the sectors
    // behind blocks 0-7 of a track.
    const int bap[8][2] = {{0x0, 0xE}, {0xD, 0xC}, {0xB, 0xA}, {0x9, 0x8},
                           {0x7, 0x6}, {0x5, 0x4}, {0x3, 0x2}, {0x1, 0xF}};
    std::vector<uint8_t> dos(kDiskette140Bytes);
    for (std::size_t t = 0; t < 35; ++t)
        for (std::size_t s = 0; s < 16; ++s)
            for (std::size_t i = 0; i < 256; ++i)
                dos[t * 4096 + s * 256 + i] = static_cast<uint8_t>(i == 0 ? t : i == 1 ? s : i);
    const auto blk = dosOrderToBlockOrder(dos);
    CHECK(blk.size() == kDiskette140Bytes);
    if (blk.size() == kDiskette140Bytes) {
        CHECK(blk[0] == 0 && blk[1] == 0);            // block 0, first half: T0 S0
        CHECK(blk[256] == 0 && blk[257] == 14);       // block 0, second half: T0 S14
        for (std::size_t b = 0; b < 280; ++b)
            for (int half = 0; half < 2; ++half) {
                const std::size_t off = b * 512 + std::size_t(half) * 256;
                CHECK(blk[off] == b / 8);
                CHECK(blk[off + 1] == bap[b % 8][half]);
            }
        CHECK(blockOrderToDosOrder(blk) == dos);
    }
    auto noise = pattern(kDiskette140Bytes, 77);
    CHECK(blockOrderToDosOrder(dosOrderToBlockOrder(noise)) == noise);
    CHECK(dosOrderToBlockOrder(blockOrderToDosOrder(noise)) == noise);
    CHECK(dosOrderToBlockOrder(std::vector<uint8_t>(1000)).empty());
    CHECK(blockOrderToDosOrder(std::vector<uint8_t>(143361)).empty());
    // A formatted 140 KB volume survives the round trip through .dsk order.
    {
        ProDOSDisk d;
        if (openOk(d, fresh(280))) {
            std::string err;
            CHECK(d.writeFile("X", 0x06, 0, pattern(5000, 5), err));
            const auto asDsk = blockOrderToDosOrder(d.image());
            ProDOSDisk back;
            CHECK(openOk(back, dosOrderToBlockOrder(asDsk)));
            std::vector<uint8_t> data;
            CHECK(back.readFile("X", data, nullptr, err) && data == pattern(5000, 5));
        }
    }

    // POM2's own skew tables (DiskImage.cpp) must say the same thing: for
    // each block half, the physical sector holding ProDOS half 2k+h holds
    // DOS 3.3 sector bap[k][h].
    std::string text;
    for (const fs::path& p : {fs::path(__FILE__).parent_path() / ".." / "src" / "DiskImage.cpp",
                              fs::path("src") / "DiskImage.cpp",
                              fs::path("..") / "src" / "DiskImage.cpp"}) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            const auto bytes = readHost(p);
            text.assign(bytes.begin(), bytes.end());
            break;
        }
    }
    if (text.empty()) {
        std::printf("prodos_disk_fs: SKIP DiskImage.cpp table check (source not found)\n");
        return;
    }
    int dosLogical[16], prodosLogical[16];
    CHECK(parseTable(text, "kDos33LogicalForPhysical", dosLogical));
    CHECK(parseTable(text, "kProDosLogicalForPhysical", prodosLogical));
    for (int k = 0; k < 8; ++k)
        for (int h = 0; h < 2; ++h) {
            int physical = -1;
            for (int p = 0; p < 16; ++p)
                if (prodosLogical[p] == 2 * k + h) physical = p;
            CHECK(physical >= 0);
            if (physical >= 0) CHECK(dosLogical[physical] == bap[k][h]);
        }
}

} // namespace

int main()
{
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    g_tmp = fs::temp_directory_path() /
            ("pom2_prodos_disk_fs_" + std::to_string(stamp) + "_" + std::to_string(rd()));
    std::error_code ec;
    fs::create_directories(g_tmp, ec);
    if (ec) {
        std::printf("prodos_disk_fs: cannot create %s\n", g_tmp.string().c_str());
        return 2;
    }

    testFormat();
    testWriteRead();
    testDirectories();
    testRefusals();
    testNonStandardStorage();
    testBuiltVolume();
    testHostile();
    testSectorOrder();

    fs::remove_all(g_tmp, ec);
    if (g_failures) {
        std::printf("prodos_disk_fs: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("prodos_disk_fs: OK\n");
    return 0;
}
