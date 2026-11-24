// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// exchange_media — ShrinkIt archives and DiskCopy 4.2 images turned into
// mountable disks (NuFxArchive.h, DiskCopy42.h, ExchangeMedia.h).
//
// The oracle for NuFX is NuLib2 3.1 (Andy McFadden, BSD): the fixtures in
// tests/fixtures/nufx/ were written by it from POM2's own content
// (make_fixtures.sh), and what each holds is rebuilt here from the same
// recipe, so a decode is checked against the bytes that went IN, not against
// what this decoder made of them. Beyond the fixtures, the reader was run
// against NuLib2's own extraction of the 82 ShrinkIt archives of the local
// corpus on 2026-11-24 (724 LZW/1, 168 LZW/2 and 356 stored threads, the
// "bad Mac" ones and resource forks included): identical, byte for byte
// (CHANGELOG 2026-11-24).
//
// A2R: synthetic captures laid out from [B] the A2R 2 and 3 references; the
// track that comes out must be exactly one revolution of the flux that went
// in. The real-capture check (Wilderness side B, A2R 2: its converted WOZ
// boots to a screen identical to the WOZ Applesauce made of the same
// capture) needs a disk POM2 may not ship and is in the CHANGELOG.
//
// DiskCopy's checksum is checked against a value worked by hand from [B]
// File Type Note $E0/$0005; the real Apple image the converter was first run
// on (a 1991 developer CD disk) is not redistributable and stays out.

#include "A2rFlux.h"
#include "DiskBuilder.h"
#include "DiskCopy42.h"
#include "ExchangeMedia.h"
#include "NuFxArchive.h"
#include "ProDOSDiskFs.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

using Bytes = std::vector<uint8_t>;

Bytes readFixture(const char* name)
{
    std::ifstream f(std::string("tests/fixtures/nufx/") + name, std::ios::binary);
    return Bytes{std::istreambuf_iterator<char>(f), {}};
}

// What make_fixtures.sh put in HELLO.TXT.
Bytes helloText()
{
    std::string s;
    for (int i = 0; i < 400; ++i)
        s += "POM2 NUFX FIXTURE LINE " + std::to_string(i) + ": THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG\n";
    return Bytes(s.begin(), s.end());
}

const pom2::nufx::Record* find(const std::vector<pom2::nufx::Record>& recs, const std::string& path)
{
    for (const auto& r : recs)
        if (r.path == path) return &r;
    return nullptr;
}

void testFilesArchive(const char* fixture)
{
    const Bytes archive = readFixture(fixture);
    CHECK(!archive.empty());
    CHECK(pom2::nufx::looksLikeNuFx(archive));
    std::vector<pom2::nufx::Record> recs;
    std::string error;
    CHECK(pom2::nufx::readArchive(archive, recs, error));
    CHECK(recs.size() == 3);
    const auto* hello = find(recs, "HELLO.TXT");
    const auto* data = find(recs, "DATA");
    const auto* note = find(recs, "SUB/NOTE.TXT");   // NuLib2 stores ':' as the separator
    CHECK(hello && data && note);
    if (!hello || !data || !note) return;
    CHECK(hello->data == helloText());   // 28 690 bytes: seven 4 KiB chunks
    CHECK(!hello->isDisk);
    // DATA#062000: BIN at $2000, a seeded random block (Python's MT19937 is
    // not std::mt19937's seeding, so the size and type are what is checked
    // here; the bytes are covered by the archive's v3 thread CRC).
    CHECK(data->data.size() == 5000);
    CHECK(data->fileType == 0x06 && data->auxType == 0x2000);
    const std::string noteText = "A NOTE IN A SUBDIRECTORY\r";
    CHECK(note->data == Bytes(noteText.begin(), noteText.end()));
}

void testLzw1Thread()
{
    // HELLO.TXT's first chunk of codes, framed as LZW/1 (make_fixtures.sh).
    const Bytes thread = readFixture("lzw1_thread.bin");
    CHECK(!thread.empty());
    Bytes out;
    std::string error;
    CHECK(pom2::nufx::expandLzw(2, thread.data(), thread.size(), 4096, out, error));
    const Bytes text = helloText();
    CHECK(out == Bytes(text.begin(), text.begin() + 4096));
    // A flipped CRC byte must be caught.
    Bytes bad = thread;
    bad[0] ^= 0x01;
    CHECK(!pom2::nufx::expandLzw(2, bad.data(), bad.size(), 4096, out, error));
    CHECK(error.find("CRC") != std::string::npos);
}

void testDiskArchive()
{
    const Bytes archive = readFixture("disk.sdk");
    std::vector<pom2::exchange::Image> images;
    std::string note, error;
    CHECK(pom2::exchange::convert("disk.sdk", archive, images, note, error));
    CHECK(images.size() == 1);
    if (images.size() != 1) return;
    CHECK(images[0].name == "vol.po");
    CHECK(images[0].bytes.size() == 143360);
    pom2::prodosfs::ProDOSDisk disk;
    CHECK(disk.open(images[0].bytes, error));
    CHECK(disk.volumeName() == "FIXTURE");
    Bytes hello;
    CHECK(disk.readFile("HELLO.TXT", hello, nullptr, error));
    // pom2_prodos put stores a text file with the Apple II's CR line ends.
    Bytes expected = helloText();
    for (auto& c : expected) if (c == '\n') c = '\r';
    CHECK(hello == expected);
}

void testFilesBecomeAVolume()
{
    const Bytes archive = readFixture("files.shk");
    std::vector<pom2::exchange::Image> images;
    std::string note, error;
    CHECK(pom2::exchange::convert("My Files.shk", archive, images, note, error));
    CHECK(images.size() == 1);
    if (images.size() != 1) return;
    CHECK(images[0].name == "My Files.po");
    CHECK(images[0].bytes.size() == 143360);   // fits on a 5.25" volume
    CHECK(note.find("3 files") != std::string::npos);
    pom2::prodosfs::ProDOSDisk disk;
    CHECK(disk.open(images[0].bytes, error));
    CHECK(disk.volumeName() == "MY.FILES");
    CHECK(disk.verify(error));
    Bytes hello, sub;
    pom2::prodosfs::Entry info;
    CHECK(disk.readFile("HELLO.TXT", hello, nullptr, error) && hello == helloText());
    CHECK(disk.readFile("DATA", sub, &info, error));
    CHECK(info.fileType == 0x06 && info.auxType == 0x2000 && sub.size() == 5000);
    CHECK(disk.readFile("SUB/NOTE.TXT", sub, nullptr, error));
    // Booted by mistake, the 5.25" volume says so: POM2's own sector.
    const Bytes sector = pom2::diskbuild::notBootableSector();
    CHECK(std::equal(sector.begin(), sector.end(), images[0].bytes.begin()));
}

void put16(Bytes& v, uint32_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32le(Bytes& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }

// A NuFX archive of stored files, laid out from [B] File Type Note
// $E0/$8002: version-0 records, the name in the record header, one
// uncompressed data thread each.
Bytes makeArchive(const std::vector<std::pair<std::string, Bytes>>& files)
{
    Bytes a = {0x4E, 0xF5, 0x46, 0xE9, 0x6C, 0xE5, 0, 0};   // "NuFile", master CRC (unchecked)
    put32le(a, uint32_t(files.size()));
    a.resize(48, 0);
    for (const auto& [name, data] : files) {
        Bytes h = {0x4E, 0xF5, 0x46, 0xD8, 0, 0};            // "NuFX", header CRC below
        put16(h, 58);                                        // attribCount: 56 + the name length word
        put16(h, 0);                                         // record version 0
        put32le(h, 1);                                       // one thread
        put16(h, 1);                                         // ProDOS
        put16(h, '/');                                       // separator
        put32le(h, 0xE3);                                    // access
        put32le(h, 0x04);                                    // TXT
        put32le(h, 0);                                       // aux
        put16(h, 1);                                         // seedling
        h.resize(h.size() + 24, 0);                          // three dates
        put16(h, uint32_t(name.size()));
        h.insert(h.end(), name.begin(), name.end());
        put16(h, 2); put16(h, 0); put16(h, 0); put16(h, 0);  // data fork, stored, no CRC
        put32le(h, uint32_t(data.size()));
        put32le(h, uint32_t(data.size()));
        const uint16_t crc = pom2::nufx::crc16(0, h.data() + 6, h.size() - 6);
        h[4] = uint8_t(crc);
        h[5] = uint8_t(crc >> 8);
        a.insert(a.end(), h.begin(), h.end());
        a.insert(a.end(), data.begin(), data.end());
    }
    return a;
}

void testProdosNames()
{
    // [B] ProDOS TRM § 2.1: a letter, then letters, digits, periods; 15 at most.
    CHECK(pom2::exchange::prodosName("a file name!") == "A.FILE.NAME");
    CHECK(pom2::exchange::prodosName("2nd") == "A2ND");
    CHECK(pom2::exchange::prodosName("averyveryverylongname") == "AVERYVERYVERYLO");
    CHECK(pom2::exchange::prodosName("...") == "A");
}

void testManyTopLevelNamesGoInAFolder()
{
    // [B] ProDOS TRM § B.2.2: the volume directory holds 51 entries and
    //     cannot grow. 60 files at the top of an archive go into one folder.
    std::vector<std::pair<std::string, Bytes>> files;
    for (int i = 0; i < 60; ++i) {
        const std::string text = "FILE " + std::to_string(i) + "\r";
        files.push_back({"F" + std::to_string(i), Bytes(text.begin(), text.end())});
    }
    // Two names that fold to the same ProDOS name keep both files.
    files.push_back({"same name", Bytes{1}});
    files.push_back({"SAME NAME", Bytes{2}});
    const Bytes archive = makeArchive(files);
    std::vector<pom2::exchange::Image> images;
    std::string note, error;
    CHECK(pom2::exchange::convert("Many.shk", archive, images, note, error));
    CHECK(images.size() == 1);
    if (images.size() != 1) return;
    pom2::prodosfs::ProDOSDisk disk;
    CHECK(disk.open(images[0].bytes, error));
    CHECK(disk.verify(error));
    std::vector<pom2::prodosfs::Entry> root, inside;
    CHECK(disk.list("", root, error));
    CHECK(root.size() == 1 && root[0].isDirectory && root[0].name == "MANY");
    CHECK(disk.list("MANY", inside, error));
    CHECK(inside.size() == 62);
    Bytes data;
    CHECK(disk.readFile("MANY/F59", data, nullptr, error));
    CHECK(std::string(data.begin(), data.end()) == "FILE 59\r");
    Bytes a, b;
    CHECK(disk.readFile("MANY/SAME.NAME", a, nullptr, error) && a == Bytes{1});
    CHECK(disk.readFile("MANY/SAME.NAME.2", b, nullptr, error) && b == Bytes{2});
}

void testBigArchiveGrowsTheVolume()
{
    // 300 KB of files do not fit 140 KB: the next size up, 800 KB, is used.
    std::vector<std::pair<std::string, Bytes>> files;
    for (int i = 0; i < 3; ++i) files.push_back({"BIG" + std::to_string(i), Bytes(100000, uint8_t(i))});
    std::vector<pom2::exchange::Image> images;
    std::string note, error;
    CHECK(pom2::exchange::convert("big.shk", makeArchive(files), images, note, error));
    CHECK(images.size() == 1 && images[0].bytes.size() == 819200 && images[0].name == "big.po");
}

void putBe32(Bytes& v, std::size_t at, uint32_t x)
{
    v[at] = uint8_t(x >> 24); v[at + 1] = uint8_t(x >> 16); v[at + 2] = uint8_t(x >> 8); v[at + 3] = uint8_t(x);
}

void testDiskCopy()
{
    // [B] Worked by hand: $1234 -> ror -> $0000091A; + $5678 = $00005F92 -> ror -> $00002FC9.
    const uint8_t words[4] = {0x12, 0x34, 0x56, 0x78};
    CHECK(pom2::diskcopy42::checksum(words, 4) == 0x00002FC9u);

    // An 800 KB image: a header, the blocks, 12 tag bytes a block.
    Bytes blocks(819200);
    std::mt19937 rng(42);
    for (auto& b : blocks) b = uint8_t(rng());
    Bytes tags(1600 * 12);
    for (auto& b : tags) b = uint8_t(rng());
    Bytes file(pom2::diskcopy42::kHeaderBytes, 0);
    const std::string name = "Test Disk";
    file[0] = uint8_t(name.size());
    std::copy(name.begin(), name.end(), file.begin() + 1);
    putBe32(file, 64, 819200);
    putBe32(file, 68, uint32_t(tags.size()));
    putBe32(file, 72, pom2::diskcopy42::checksum(blocks.data(), blocks.size()));
    putBe32(file, 76, pom2::diskcopy42::checksum(tags.data() + 12, tags.size() - 12));
    file[80] = 1;      // 800 KB
    file[81] = 0x24;   // Apple II GCR format byte
    file[82] = 0x01;
    file[83] = 0x00;
    file.insert(file.end(), blocks.begin(), blocks.end());
    file.insert(file.end(), tags.begin(), tags.end());

    CHECK(pom2::diskcopy42::looksLikeDiskCopy42(file));
    CHECK(pom2::diskcopy42::diskName(file) == "Test Disk");
    CHECK(pom2::exchange::isExchangeFile("disk.image", file));
    CHECK(!pom2::exchange::isExchangeFile("disk.image", blocks));   // a raw .image is not converted
    std::vector<pom2::exchange::Image> images;
    std::string note, error;
    CHECK(pom2::exchange::convert("Disk.dc42", file, images, note, error));
    CHECK(images.size() == 1 && images[0].name == "Disk.po" && images[0].bytes == blocks);
    CHECK(note.find("Test Disk") != std::string::npos);

    // The first block's 12 tag bytes are outside the tag checksum.
    Bytes firstTag = file;
    firstTag[pom2::diskcopy42::kHeaderBytes + 819200] ^= 0xFF;
    CHECK(pom2::exchange::convert("Disk.dc42", firstTag, images, note, error));
    // Any other changed byte is caught.
    Bytes badData = file;
    badData[pom2::diskcopy42::kHeaderBytes + 1000] ^= 0x01;
    CHECK(!pom2::exchange::convert("Disk.dc42", badData, images, note, error));
    Bytes badTag = file;
    badTag[pom2::diskcopy42::kHeaderBytes + 819200 + 100] ^= 0x01;
    CHECK(!pom2::exchange::convert("Disk.dc42", badTag, images, note, error));
    Bytes truncated(file.begin(), file.end() - 1);
    CHECK(!pom2::diskcopy42::looksLikeDiskCopy42(truncated));
}

std::vector<uint32_t> randomRevolution(uint32_t seed, uint32_t ticksPerCell)
{
    // ~200 ms of 4, 8 and 12 µs intervals (1, 2 and 3 cells), in ticks.
    std::mt19937 rng(seed);
    std::vector<uint32_t> rev;
    uint64_t total = 0;
    const uint64_t target = 200000ull * ticksPerCell / 4;   // 200 ms: 4 µs is ticksPerCell ticks
    while (total < target) {
        const uint32_t d = ticksPerCell * (1 + rng() % 3);
        rev.push_back(d);
        total += d;
    }
    return rev;
}

// 1.25 turns of `rev`, starting `offset` transitions in, as A2R flux bytes.
Bytes captureBytes(const std::vector<uint32_t>& rev, std::size_t offset)
{
    Bytes out;
    const std::size_t count = rev.size() + rev.size() / 4;
    for (std::size_t i = 0; i < count; ++i) {
        uint32_t v = rev[(offset + i) % rev.size()];
        while (v >= 255) { out.push_back(255); v -= 255; }
        out.push_back(uint8_t(v));
    }
    return out;
}

void chunk(Bytes& f, const char* id, const Bytes& data)
{
    f.insert(f.end(), id, id + 4);
    put32le(f, uint32_t(data.size()));
    f.insert(f.end(), data.begin(), data.end());
}

// The WOZ flux track that quarter track `qt` maps to, as intervals in ticks.
bool wozFluxTrack(const Bytes& woz, int qt, std::vector<uint32_t>& deltas, int& index)
{
    auto rd16 = [&](std::size_t o) { return uint32_t(woz[o] | (woz[o + 1] << 8)); };
    auto rd32 = [&](std::size_t o) { return rd16(o) | (rd16(o + 2) << 16); };
    const std::size_t flux = std::size_t(rd16(66)) * 512;
    if (flux + 168 > woz.size() || std::memcmp(woz.data() + flux, "FLUX", 4) != 0) return false;
    index = woz[flux + 8 + qt];
    if (index == 0xFF) return true;
    const std::size_t at = std::size_t(rd16(256 + index * 8)) * 512, bytes = rd32(256 + index * 8 + 4);
    deltas.clear();
    uint32_t v = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        v += woz[at + i];
        if (woz[at + i] != 255) { deltas.push_back(v); v = 0; }
    }
    return true;
}

// `got` is `want` turned: the same intervals from some starting point.
bool isRotation(const std::vector<uint32_t>& got, const std::vector<uint32_t>& want)
{
    if (got.size() != want.size() || got.empty()) return false;
    for (std::size_t start = 0; start < want.size(); ++start) {
        bool same = true;
        for (std::size_t i = 0; i < got.size() && same; ++i) same = got[i] == want[(start + i) % want.size()];
        if (same) return true;
    }
    return false;
}

void testA2r2()
{
    // [B] A2R 2: "A2R2" FF 0A 0D 0A; INFO (36 bytes, disk type at +33);
    //     STRM: location, type, u32 length, u32 loop point, data; $FF ends.
    const auto rev0 = randomRevolution(7, 32), rev8 = randomRevolution(8, 32);
    Bytes f = {'A', '2', 'R', '2', 0xFF, 0x0A, 0x0D, 0x0A};
    Bytes info(36, 0);
    info[0] = 1;
    info[33] = 1;      // 5.25"
    info[34] = 1;      // write-protected
    chunk(f, "INFO", info);
    Bytes strm;
    for (auto [qt, rev, offset] : {std::tuple<int, const std::vector<uint32_t>*, std::size_t>{0, &rev0, 1234},
                                   {8, &rev8, 40000}}) {
        const Bytes data = captureBytes(*rev, offset);
        strm.push_back(uint8_t(qt));
        strm.push_back(1);                  // timing
        put32le(strm, uint32_t(data.size()));
        put32le(strm, 0);                   // no loop point: the nominal turn, refined
        strm.insert(strm.end(), data.begin(), data.end());
    }
    strm.push_back(0xFF);
    chunk(f, "STRM", strm);

    CHECK(pom2::exchange::isExchangeFile("Game.A2R", f));
    std::vector<pom2::exchange::Image> images;
    std::string note, error;
    CHECK(pom2::exchange::convert("Game.a2r", f, images, note, error));
    CHECK(images.size() == 1);
    if (images.size() != 1) return;
    const Bytes& woz = images[0].bytes;
    CHECK(images[0].name == "Game.woz");
    CHECK(std::memcmp(woz.data(), "WOZ2", 4) == 0);
    CHECK(woz[20] == 3 && woz[21] == 1 && woz[22] == 1);   // INFO v3, 5.25", the capture's notch
    CHECK(note.find("2 tracks") != std::string::npos && note.find("repeats") != std::string::npos);
    std::vector<uint32_t> d;
    int index = -1;
    CHECK(wozFluxTrack(woz, 0, d, index) && index == 0 && isRotation(d, rev0));
    CHECK(wozFluxTrack(woz, 8, d, index) && index == 1 && isRotation(d, rev8));
    // A whole track also answers the quarter tracks beside it; 2 is between none.
    CHECK(wozFluxTrack(woz, 1, d, index) && index == 0);
    CHECK(wozFluxTrack(woz, 7, d, index) && index == 1);
    CHECK(wozFluxTrack(woz, 9, d, index) && index == 1);
    CHECK(wozFluxTrack(woz, 2, d, index) && index == 0xFF);
    CHECK(wozFluxTrack(woz, 20, d, index) && index == 0xFF);
    // TMAP sends every quarter track to FLUX.
    CHECK(std::all_of(woz.begin() + 88, woz.begin() + 248, [](uint8_t v) { return v == 0xFF; }));

    // A 3.5" capture is refused by name.
    Bytes disk35 = f;
    disk35[8 + 8 + 33] = 2;
    CHECK(!pom2::exchange::convert("Game.a2r", disk35, images, note, error));
    CHECK(error.find("3.5") != std::string::npos);
}

void testA2r3()
{
    // [B] A2R 3: RWCP with a resolution in ps (62.5 ns here, Applesauce's
    //     default) and index signals; SLVD holds one solved revolution.
    const auto rev = randomRevolution(9, 64);   // 4 µs = 64 ticks of 62.5 ns
    const auto solved = randomRevolution(10, 64);
    Bytes f = {'A', '2', 'R', '3', 0xFF, 0x0A, 0x0D, 0x0A};
    Bytes info(37, 0);
    info[0] = 1;
    info[33] = 1;      // 5.25" SS at 0.25 step
    chunk(f, "INFO", info);
    Bytes rwcp = {1};
    put32le(rwcp, 62500);
    rwcp.resize(16, 0);
    {
        const Bytes data = captureBytes(rev, 500);
        uint64_t turn = 0;
        for (uint32_t v : rev) turn += v;
        rwcp.push_back('C');
        rwcp.push_back(3);                 // xtiming
        put16(rwcp, 4);                    // track 1
        rwcp.push_back(1);                 // one index signal: the end of the first turn
        put32le(rwcp, uint32_t(turn));
        put32le(rwcp, uint32_t(data.size()));
        rwcp.insert(rwcp.end(), data.begin(), data.end());
    }
    rwcp.push_back('X');
    chunk(f, "RWCP", rwcp);
    Bytes slvd = {2};
    put32le(slvd, 62500);
    slvd.resize(16, 0);
    {
        Bytes data;
        for (uint32_t v : solved) {
            while (v >= 255) { data.push_back(255); v -= 255; }
            data.push_back(uint8_t(v));
        }
        slvd.push_back('T');
        put16(slvd, 12);                   // track 3
        slvd.insert(slvd.end(), 8, 0);     // mirror distances, reserved
        slvd.push_back(0);                 // no index signal
        put32le(slvd, uint32_t(data.size()));
        slvd.insert(slvd.end(), data.begin(), data.end());
    }
    slvd.push_back('X');
    chunk(f, "SLVD", slvd);

    std::vector<pom2::exchange::Image> images;
    std::string note, error;
    CHECK(pom2::exchange::convert("Disk.a2r", f, images, note, error));
    if (images.size() != 1) { CHECK(false); return; }
    CHECK(note.find("1 solved") != std::string::npos);
    // 62.5 ns ticks become 125 ns ones: every interval halves.
    auto halved = [](std::vector<uint32_t> v) { for (auto& x : v) x /= 2; return v; };
    std::vector<uint32_t> d;
    int index = -1;
    CHECK(wozFluxTrack(images[0].bytes, 4, d, index) && index != 0xFF && isRotation(d, halved(rev)));
    CHECK(wozFluxTrack(images[0].bytes, 12, d, index) && index != 0xFF && d == halved(solved));
    // A PC drive's capture is not an Apple 5.25" one.
    Bytes pc = f;
    pc[8 + 8 + 33] = 3;
    CHECK(!pom2::exchange::convert("Disk.a2r", pc, images, note, error));
}

void testHostileInput()
{
    // A damaged archive is refused with a reason, never read past its end.
    Bytes archive = readFixture("files.shk");
    for (std::size_t cut : {std::size_t(10), std::size_t(60), std::size_t(200), archive.size() / 2, archive.size() - 3}) {
        Bytes part(archive.begin(), archive.begin() + std::min(cut, archive.size()));
        std::vector<pom2::nufx::Record> recs;
        std::string error;
        CHECK(!pom2::nufx::readArchive(part, recs, error));
        CHECK(!error.empty());
    }
    Bytes flipped = archive;
    flipped[48 + 20] ^= 0x40;   // inside the first record header: its CRC fails
    std::vector<pom2::nufx::Record> recs;
    std::string error;
    CHECK(!pom2::nufx::readArchive(flipped, recs, error));
    CHECK(error.find("CRC") != std::string::npos);
    std::vector<pom2::exchange::Image> images;
    std::string note;
    CHECK(!pom2::exchange::convert("x.shk", Bytes(100, 0), images, note, error));
}

} // namespace

int main()
{
    testFilesArchive("files.shk");
    testFilesArchive("stored.shk");
    testLzw1Thread();
    testDiskArchive();
    testFilesBecomeAVolume();
    testProdosNames();
    testManyTopLevelNamesGoInAFolder();
    testBigArchiveGrowsTheVolume();
    testDiskCopy();
    testA2r2();
    testA2r3();
    testHostileInput();
    if (g_fail) {
        std::fprintf(stderr, "exchange_media: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("exchange_media: all checks passed");
    return 0;
}
