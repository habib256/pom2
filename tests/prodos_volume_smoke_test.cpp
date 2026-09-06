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

// ProDOS host-folder volume synthesizer smoke test.
//
// Pins:
//   * Volume header layout (block 2 offset 4..42).
//   * File entry layout (storage_type, name, file_type, key_pointer,
//     blocks_used, eof) for both seedling and sapling files.
//   * Sapling index block format (split low/high pointer halves).
//   * Round-trip data integrity: first block of a sapling file matches
//     the first 512 bytes of the source host file.
//   * Empty-folder special case (no files, valid empty volume).

#include "ProDOSVolume.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

constexpr std::size_t kBlockBytes = 512;

static void writeFile(const fs::path& path, const std::vector<std::uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    assert(f.good());
    if (!data.empty()) {
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    assert(f.good());
}

static fs::path makeTempDir(const std::string& tag)
{
    fs::path dir = fs::temp_directory_path() / ("pom2_prodos_smoke_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

static std::uint16_t rd16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
static std::uint32_t rd24(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16);
}

static void testEmptyFolder()
{
    fs::path dir = makeTempDir("empty");

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(dir.string(), "HOST", img);
    assert(br.ok);
    assert(br.filesIncluded == 0);
    assert(br.filesSkipped  == 0);
    // 2 boot + 4 vol dir + 1 bitmap + the free-block slack. The volume used
    // to be sized to fit its contents EXACTLY, which left ProDOS reporting
    // zero free blocks: the guest could not create or re-SAVE a single file
    // on a folder it was told was writable. The slack is 10 % of the content
    // with a 64-block floor, so an empty folder gets the floor.
    // (Bug hunt 2026-09-06 #H12.)
    assert(br.totalBlocks   == 7 + 64);
    assert(img.size() == (7 + 64) * kBlockBytes);

    // Volume header at offset 4 of block 2.
    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;
    assert(rd16(b2 + 0) == 0);       // prev pointer
    assert(rd16(b2 + 2) == 3);       // next pointer
    assert(b2[4] == 0xF4);           // storage_type = 0xF, name_length = 4 ("HOST")
    assert(std::memcmp(b2 + 5, "HOST", 4) == 0);
    assert(b2[4 + 0x1F] == 39);      // entry_length
    assert(b2[4 + 0x20] == 13);      // entries_per_block
    assert(rd16(b2 + 4 + 0x21) == 0);// file_count
    assert(rd16(b2 + 4 + 0x23) == 6);// bit_map_pointer
    assert(rd16(b2 + 4 + 0x25) == 7 + 64);// total_blocks

    // The slack is FREE in the bitmap — otherwise it would be 64 blocks the
    // guest still cannot use. Structure (0..6) stays used.
    const std::uint8_t* bm = img.data() + 6 * kBlockBytes;
    auto blockFree = [bm](std::size_t b) {
        return (bm[b >> 3] & (1u << (7 - (b & 7)))) != 0;
    };
    for (std::size_t b = 0; b < 7; ++b)  assert(!blockFree(b));
    for (std::size_t b = 7; b < 71; ++b) assert(blockFree(b));
}

static void testSeedlingAndSapling()
{
    fs::path dir = makeTempDir("populated");

    // 3 files: short BAS (200 B → seedling), short TXT (300 B → seedling),
    // larger BIN (5000 B → sapling, ceil(5000/512)=10 data blocks).
    std::vector<std::uint8_t> bas(200);
    for (std::size_t i = 0; i < bas.size(); ++i) bas[i] = static_cast<std::uint8_t>(i & 0xFF);
    std::vector<std::uint8_t> txt(300);
    for (std::size_t i = 0; i < txt.size(); ++i) txt[i] = static_cast<std::uint8_t>('A' + (i % 26));
    std::vector<std::uint8_t> bin(5000);
    for (std::size_t i = 0; i < bin.size(); ++i) bin[i] = static_cast<std::uint8_t>((i * 7u + 3u) & 0xFF);

    writeFile(dir / "program.bas", bas);
    writeFile(dir / "hello.txt",   txt);
    writeFile(dir / "data.bin",    bin);

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(dir.string(), "HOST", img);
    assert(br.ok);
    assert(br.filesIncluded == 3);
    assert(br.filesSkipped  == 0);

    // Layout: 7 structural + 1 (bas seedling) + 1 (txt seedling) +
    // 1 (bin sapling index) + 10 (bin data blocks) = 20 blocks, plus the
    // 64-block free-space floor (see testEmptyFolder).
    assert(br.totalBlocks == 20 + 64);
    assert(img.size() == (20 + 64) * kBlockBytes);

    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;

    // file_count
    assert(rd16(b2 + 4 + 0x21) == 3);
    assert(rd16(b2 + 4 + 0x25) == 20 + 64);

    // First file entry starts at block 2 offset 4 + 39 = 43.
    // Files are inserted in alphabetical order:
    //   data.bin   → entry 0 (sapling, file_type 0x06, eof 5000)
    //   hello.txt  → entry 1 (seedling, file_type 0x04, eof 300)
    //   program.bas → entry 2 (seedling, file_type 0xFC, eof 200)

    auto entryAt = [&](std::size_t idx) -> const std::uint8_t* {
        const std::size_t kEntryLen = 39;
        if (idx < 12) return b2 + 4 + kEntryLen + idx * kEntryLen;
        idx -= 12;
        const std::size_t blk = 3 + (idx / 13);
        const std::size_t slot = idx % 13;
        return img.data() + blk * kBlockBytes + 4 + slot * kEntryLen;
    };

    // ── Entry 0: DATA (.bin → BIN, sapling, 5000 bytes)
    const std::uint8_t* e0 = entryAt(0);
    const std::uint8_t st0 = e0[0] >> 4;
    const std::uint8_t nl0 = e0[0] & 0x0F;
    assert(st0 == 0x2);                       // sapling
    assert(nl0 == 4);                         // "DATA"
    assert(std::memcmp(e0 + 1, "DATA", 4) == 0);
    assert(e0[0x10] == 0x06);                 // BIN
    const std::uint16_t key0 = rd16(e0 + 0x11);
    const std::uint16_t bu0  = rd16(e0 + 0x13);
    const std::uint32_t eof0 = rd24(e0 + 0x15);
    assert(bu0 == 11);                        // 1 index + 10 data
    assert(eof0 == 5000);
    assert(rd16(e0 + 0x25) == 2);             // header_pointer = vol dir key block

    // Sapling index block: 256 LE pointers split as low half | high half.
    const std::uint8_t* idxBlk = img.data() + key0 * kBlockBytes;
    const std::uint16_t firstDataBlk = idxBlk[0] | (idxBlk[256] << 8);
    assert(firstDataBlk != 0);
    // First 512 bytes of bin should match the first data block.
    const std::uint8_t* firstData = img.data() + firstDataBlk * kBlockBytes;
    assert(std::memcmp(firstData, bin.data(), kBlockBytes) == 0);

    // 10th (last) data block: pointer at index 9.
    const std::uint16_t lastDataBlk = idxBlk[9] | (idxBlk[256 + 9] << 8);
    assert(lastDataBlk != 0);
    // bin[5000 - 1] is at index 9 * 512 + 5000 - 4608 = 392 within last block.
    const std::uint8_t* lastBlock = img.data() + lastDataBlk * kBlockBytes;
    const std::size_t tailLen = 5000 - 9 * kBlockBytes;
    assert(std::memcmp(lastBlock, bin.data() + 9 * kBlockBytes, tailLen) == 0);

    // ── Entry 1: HELLO (.txt → TXT, seedling, 300 bytes)
    const std::uint8_t* e1 = entryAt(1);
    const std::uint8_t st1 = e1[0] >> 4;
    const std::uint8_t nl1 = e1[0] & 0x0F;
    assert(st1 == 0x1);
    assert(nl1 == 5);
    assert(std::memcmp(e1 + 1, "HELLO", 5) == 0);
    assert(e1[0x10] == 0x04);                 // TXT
    const std::uint16_t key1 = rd16(e1 + 0x11);
    assert(rd16(e1 + 0x13) == 1);             // blocks_used
    assert(rd24(e1 + 0x15) == 300);           // eof
    const std::uint8_t* d1 = img.data() + key1 * kBlockBytes;
    assert(std::memcmp(d1, txt.data(), txt.size()) == 0);

    // ── Entry 2: PROGRAM (.bas → BAS, seedling, 200 bytes)
    const std::uint8_t* e2 = entryAt(2);
    const std::uint8_t st2 = e2[0] >> 4;
    const std::uint8_t nl2 = e2[0] & 0x0F;
    assert(st2 == 0x1);
    assert(nl2 == 7);
    assert(std::memcmp(e2 + 1, "PROGRAM", 7) == 0);
    assert(e2[0x10] == 0xFC);                 // BAS
    assert(rd24(e2 + 0x15) == 200);

    // Bitmap sanity: structural blocks 0..6 are USED (bits cleared); the
    // first data block (block 7) is also USED; last allocated block (19)
    // is USED; block 20+ is "out of range" — must be USED too (since we
    // initialised only bits 0..total_blocks-1 to free, and only freed within
    // total_blocks, the byte covering block 20+ stays as we set it). We
    // only assert the structural + first data + a clearly free expectation
    // depending on layout. With totalBlocks=20, all blocks 0..19 are used
    // (every block is part of either structure or file data).
    const std::uint8_t* bm = img.data() + 6 * kBlockBytes;
    auto blockFree = [bm](std::size_t b) {
        const std::size_t byteIdx = b >> 3;
        const std::size_t bitIdx  = 7 - (b & 7);
        return (bm[byteIdx] & (1u << bitIdx)) != 0;
    };
    for (std::size_t b = 0; b < 20; ++b) {
        assert(!blockFree(b));
    }

    std::printf("prodos_volume_smoke: empty + populated OK\n");
}

static void testNameSanitisationAndCollisions()
{
    fs::path dir = makeTempDir("names");

    // "HELLO WORLD.TXT" → "HELLOWORLD" (letters/digits only, ≤15 chars).
    // "1ROOT.bin" must be prefixed → "A1ROOT".
    // Two files producing the same sanitised name must get .1 / .2 suffixes.
    writeFile(dir / "hello world.txt", { 'a' });
    writeFile(dir / "1root.bin",       { 'b' });
    writeFile(dir / "Foo!.bin",        { 'c' });
    writeFile(dir / "Foo?.bin",        { 'd' });   // collision after sanitise

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(dir.string(), "HOST", img);
    assert(br.ok);
    assert(br.filesIncluded == 4);

    // Spot-check: at least one entry sanitises to "A1ROOT" (1root.bin).
    bool found_a1root = false, found_foo = false, found_foo1 = false;
    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;
    for (std::size_t i = 0; i < 12; ++i) {
        const std::uint8_t* e = b2 + 4 + 39 + i * 39;
        if ((e[0] >> 4) == 0) continue;  // empty slot
        const std::uint8_t nl = e[0] & 0x0F;
        std::string name(reinterpret_cast<const char*>(e + 1), nl);
        if (name == "A1ROOT") found_a1root = true;
        if (name == "FOO.")   found_foo    = true;     // first collision wins base name
        if (name == "FOO..1") found_foo1   = true;     // second gets .1 suffix
    }
    assert(found_a1root);
    assert(found_foo);
    assert(found_foo1);
    std::printf("prodos_volume_smoke: name sanitisation + collisions OK\n");
}

static void testRoundTripFolderToVolumeToFolder()
{
    // Source folder: 3 files with distinct types and a sapling.
    fs::path src = makeTempDir("round_src");
    std::vector<std::uint8_t> bas(120);
    for (std::size_t i = 0; i < bas.size(); ++i) bas[i] = static_cast<std::uint8_t>('a' + (i % 26));
    std::vector<std::uint8_t> txt(80);
    for (std::size_t i = 0; i < txt.size(); ++i) txt[i] = static_cast<std::uint8_t>('1' + (i % 9));
    std::vector<std::uint8_t> bin(2500);             // sapling, 5 data blocks
    for (std::size_t i = 0; i < bin.size(); ++i) bin[i] = static_cast<std::uint8_t>((i * 11u + 5u) & 0xFF);

    writeFile(src / "hello.bas",  bas);
    writeFile(src / "readme.txt", txt);
    writeFile(src / "game.bin",   bin);

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(src.string(), "HOST", img);
    assert(br.ok && br.filesIncluded == 3);

    // Simulate a guest write: flip one byte in HELLO's data block. Find
    // HELLO's seedling key_pointer via the directory; HELLO is 5 chars,
    // so we scan for storage=$1 and name "HELLO".
    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;
    std::uint16_t helloKey = 0;
    for (std::size_t i = 0; i < 12; ++i) {
        const std::uint8_t* e = b2 + 4 + 39 + i * 39;
        if ((e[0] >> 4) != 0x1) continue;
        const std::uint8_t nl = e[0] & 0x0F;
        if (nl == 5 && std::memcmp(e + 1, "HELLO", 5) == 0) {
            helloKey = rd16(e + 0x11);
            break;
        }
    }
    assert(helloKey != 0);
    img[helloKey * kBlockBytes + 7] = 0xEE;       // mutate one byte

    // Decode into a fresh folder.
    fs::path dst = makeTempDir("round_dst");
    auto dr = pom2::decodeVolumeToFolder(img, dst.string());
    assert(dr.ok);
    assert(dr.filesWritten == 3);
    assert(dr.filesSkipped == 0);

    // Check the 3 files are present with the right names and extensions.
    auto readBack = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return std::vector<std::uint8_t>{};
        f.seekg(0, std::ios::end);
        const auto sz = static_cast<std::size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> out(sz);
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(sz));
        return out;
    };

    auto helloOut  = readBack(dst / "HELLO.bas");
    auto readmeOut = readBack(dst / "README.txt");
    auto gameOut   = readBack(dst / "GAME.bin");
    assert(!helloOut.empty());
    assert(!readmeOut.empty());
    assert(!gameOut.empty());

    // HELLO must reflect the guest mutation at offset 7.
    assert(helloOut.size() == bas.size());
    assert(helloOut[7] == 0xEE);
    for (std::size_t i = 0; i < bas.size(); ++i) {
        if (i == 7) continue;
        assert(helloOut[i] == bas[i]);
    }

    // README + GAME must round-trip exactly.
    assert(readmeOut == txt);
    assert(gameOut   == bin);

    // No-delete safety: a file that exists in the destination but isn't
    // in the volume must NOT be removed.
    writeFile(dst / "do_not_delete.txt", { 'X', 'Y' });
    auto dr2 = pom2::decodeVolumeToFolder(img, dst.string());
    assert(dr2.ok);
    assert(fs::exists(dst / "do_not_delete.txt"));

    // A failed host write must fail the decode; it must not be counted as a
    // successfully persisted guest change. Directories at the DESTINATION
    // names make the commit's rename fail deterministically, without
    // permission tricks and without assuming what the temp file is called
    // (it is unique per process + per call now; see pom2::tempSiblingPath).
    fs::path blocked = makeTempDir("round_blocked");
    fs::create_directory(blocked / "HELLO.bas");
    fs::create_directory(blocked / "README.txt");
    fs::create_directory(blocked / "GAME.bin");
    auto dr3 = pom2::decodeVolumeToFolder(img, blocked.string());
    assert(!dr3.ok);
    assert(!dr3.error.empty());

    std::printf("prodos_volume_smoke: folder→volume→folder round-trip OK\n");
}

static void testSubdirsBuildAndDecode()
{
    // Source folder with one nested subdir:
    //   src/INTRO.txt           ("hi")
    //   src/GAMES/PACMAN.bin    256 bytes
    //   src/GAMES/SCORES.txt    ("100")
    fs::path src = makeTempDir("subdirs_src");
    fs::create_directories(src / "GAMES");
    writeFile(src / "intro.txt", { 'h', 'i' });
    std::vector<std::uint8_t> pacman(256);
    for (std::size_t i = 0; i < pacman.size(); ++i) pacman[i] = static_cast<std::uint8_t>(i);
    writeFile(src / "GAMES" / "pacman.bin", pacman);
    writeFile(src / "GAMES" / "scores.txt", { '1', '0', '0' });

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(src.string(), "HOST", img);
    assert(br.ok);
    assert(br.filesIncluded == 3);                  // 3 regular files (subdir not counted)

    // Volume directory must have a child entry with storage_type=$D for
    // GAMES. Find it in block 2.
    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;
    bool foundDir = false, foundIntro = false;
    std::uint16_t gamesKey = 0;
    std::uint8_t  gamesParentSlot = 0;
    for (std::size_t i = 0; i < 12; ++i) {
        const std::uint8_t* e = b2 + 4 + 39 + i * 39;
        const std::uint8_t st = e[0] >> 4;
        if (st == 0) continue;
        const std::uint8_t nl = e[0] & 0x0F;
        std::string nm(reinterpret_cast<const char*>(e + 1), nl);
        if (st == 0xD && nm == "GAMES") {
            foundDir = true;
            gamesKey = rd16(e + 0x11);
            gamesParentSlot = static_cast<std::uint8_t>(i + 2);  // 1-based, +1 for vol header
            assert(e[0x10] == 0x0F);                              // file_type DIR
        }
        if (st == 0x1 && nm == "INTRO") {
            foundIntro = true;
            assert(e[0x10] == 0x04);                              // TXT
        }
    }
    assert(foundDir && foundIntro);
    assert(gamesKey != 0);

    // GAMES subdir header at its first block, offset 4. Validate fields.
    const std::uint8_t* gamesBlock = img.data() + gamesKey * kBlockBytes;
    const std::uint8_t* hdr = gamesBlock + 4;
    assert((hdr[0] >> 4) == 0xE);                                 // subdir header
    assert((hdr[0] & 0x0F) == 5);                                 // "GAMES"
    assert(std::memcmp(hdr + 1, "GAMES", 5) == 0);
    // The subdirectory marker, at ENTRY offset $10. The ProDOS 8 TRM calls it
    // byte $14, counting from the start of the BLOCK — a directory block
    // opens with a 4-byte prev/next pair, so the header entry begins at block
    // offset 4. This test asserted $14 entry-relative for as long as the
    // encoder wrote it there, and both were wrong: 84 real subdirectory
    // headers across the images in this tree carry $75 or $76 at entry $10,
    // and $00 at entry $14.
    assert(hdr[0x10] == 0x75);
    assert(hdr[0x14] == 0x00);   // and nothing here, which is what ProDOS does
    assert(rd16(hdr + 0x21) == 2);                                // 2 children
    assert(rd16(hdr + 0x23) == 2);                                // parent block = vol dir block 2
    assert(hdr[0x25] == gamesParentSlot);                         // 1-based parent slot

    // Two file entries in the GAMES dir block (slots 1 and 2 — slot 0 = header).
    bool foundPac = false, foundScores = false;
    for (std::size_t s = 1; s < 13; ++s) {
        const std::uint8_t* e = gamesBlock + 4 + s * 39;
        const std::uint8_t st = e[0] >> 4;
        if (st == 0) continue;
        const std::uint8_t nl = e[0] & 0x0F;
        std::string nm(reinterpret_cast<const char*>(e + 1), nl);
        if (nm == "PACMAN" && st == 0x1) {
            foundPac = true;
            assert(e[0x10] == 0x06);                              // BIN
            const std::uint16_t key = rd16(e + 0x11);
            const std::uint32_t eof = rd24(e + 0x15);
            assert(eof == 256);
            const std::uint8_t* d = img.data() + key * kBlockBytes;
            assert(std::memcmp(d, pacman.data(), 256) == 0);
            // header_pointer = first block of GAMES subdir
            assert(rd16(e + 0x25) == gamesKey);
        }
        if (nm == "SCORES" && st == 0x1) {
            foundScores = true;
            assert(e[0x10] == 0x04);                              // TXT
            assert(rd24(e + 0x15) == 3);
        }
    }
    assert(foundPac && foundScores);

    // Now decode and verify round-trip into a fresh folder.
    fs::path dst = makeTempDir("subdirs_dst");
    auto dr = pom2::decodeVolumeToFolder(img, dst.string());
    assert(dr.ok);
    assert(dr.filesWritten == 3);

    assert(fs::exists(dst / "INTRO.txt"));
    assert(fs::is_directory(dst / "GAMES"));
    assert(fs::exists(dst / "GAMES" / "PACMAN.bin"));
    assert(fs::exists(dst / "GAMES" / "SCORES.txt"));

    std::ifstream pacOut(dst / "GAMES" / "PACMAN.bin", std::ios::binary);
    std::vector<std::uint8_t> pacOutBytes((std::istreambuf_iterator<char>(pacOut)),
                                           std::istreambuf_iterator<char>());
    assert(pacOutBytes == pacman);

    std::printf("prodos_volume_smoke: subdirs build+decode OK\n");
}

// "NAME#TTAAAA" metadata tags (CiderPress convention, 2026-07-12): the tag
// sets file_type + aux_type and is stripped from the ProDOS name — the HGR
// Paint editor's "PIC#062000" saves become BIN files loading at $2000.
static void testMetadataTagParsing()
{
    fs::path dir = makeTempDir("tags");
    writeFile(dir / "PIC#062000", std::vector<std::uint8_t>(256, 0xAB));
    writeFile(dir / "PLAIN.bin",  std::vector<std::uint8_t>(16, 0x01));
    writeFile(dir / "NOTAG#12",   std::vector<std::uint8_t>(16, 0x02));  // short → not a tag

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(dir.string(), "HOST", img);
    assert(br.ok);
    assert(br.filesIncluded == 3);

    // Scan the volume directory (blocks 2..5) for the three entries.
    bool sawPic = false, sawPlain = false, sawNoTag = false;
    for (int blk = 2; blk <= 5; ++blk) {
        const std::uint8_t* b = img.data() + blk * kBlockBytes;
        for (int e = 0; e < 13; ++e) {
            const std::uint8_t* ent = b + 4 + e * 39;
            const int nameLen = ent[0] & 0x0F;
            if (nameLen == 0) continue;
            const std::string name(reinterpret_cast<const char*>(ent + 1), nameLen);
            const std::uint8_t  type = ent[0x10];
            const std::uint16_t aux  = static_cast<std::uint16_t>(ent[0x1F] | (ent[0x20] << 8));
            if (name == "PIC")   { sawPic = true;   assert(type == 0x06 && aux == 0x2000); }
            if (name == "PLAIN") { sawPlain = true; assert(type == 0x06 && aux == 0x0000); }
            if (name.rfind("NOTAG", 0) == 0) { sawNoTag = true; assert(aux == 0x0000); }
        }
    }
    assert(sawPic && sawPlain && sawNoTag);
    std::printf("prodos_volume_smoke: metadata tags OK\n");
}

// ── Two ProDOS entries must never land on ONE host file ──────────────────
//
// Found by fuzzing the build/decode round trip (bug hunt 8 round 3). The
// decode strips trailing dots before composing a host name — legal in ProDOS,
// awkward-to-illegal on the host — so `README` and `README.` both came out as
// `README`, and the SECOND write silently replaced the first while both
// halves reported success (build: 2 included, 0 skipped; decode: 2 written).
// The user's file was simply gone after a write-back.
//
// The build path manufactures that pair without anyone trying:
// sanitiseProDOSName maps every character outside A-Z 0-9 '.' to '.', so a
// host folder holding `README` and `README!` becomes `README` + `README.` in
// the volume. The guest can also create both names inside the volume itself,
// so the decode has to be safe on its own.
static void testDecodeNeverMergesTwoEntriesOntoOneFile()
{
    const fs::path src = makeTempDir("merge_src");
    const fs::path dst = makeTempDir("merge_dst");

    const std::vector<std::uint8_t> a = { 'A','A','A','A','A','A','A','A' };
    const std::vector<std::uint8_t> b = { 'B','B','B','B','B','B','B','B','B' };
    writeFile(src / "README",  a);
    writeFile(src / "README!", b);          // sanitises to "README."

    std::vector<std::uint8_t> image;
    const pom2::ProDOSBuildResult br =
        pom2::buildVolumeFromFolder(src.string(), "TEST", image);
    assert(br.ok);
    assert(br.filesIncluded == 2 && br.filesSkipped == 0);

    const pom2::ProDOSDecodeResult dr =
        pom2::decodeVolumeToFolder(image, dst.string());
    assert(dr.ok);

    // BOTH bodies must exist on the host — the count AND the contents, since
    // the failure mode was one file holding the other's bytes.
    int files = 0;
    bool sawA = false, sawB = false;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dst, ec)) {
        if (!e.is_regular_file(ec)) continue;
        ++files;
        std::ifstream in(e.path(), std::ios::binary);
        const std::vector<std::uint8_t> got(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        if (got == a) sawA = true;
        if (got == b) sawB = true;
    }
    assert(files == 2 && "two volume entries must not collapse to one file");
    assert(sawA && sawB && "neither file's contents may be overwritten");

    // …and a second write-back must be STABLE, not grow a fresh suffix every
    // time: the disambiguation tracks names used in this pass, never what is
    // already on disk.
    const pom2::ProDOSDecodeResult again =
        pom2::decodeVolumeToFolder(image, dst.string());
    assert(again.ok);
    assert(again.filesWritten == 0 && "an unchanged volume rewrites nothing");
    int filesAgain = 0;
    for (const auto& e : fs::directory_iterator(dst, ec))
        if (e.is_regular_file(ec)) ++filesAgain;
    assert(filesAgain == 2 && "repeated write-back must not accrete files");

    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
    std::printf("prodos_volume_smoke: colliding host names stay distinct OK\n");
}

// A SCOSWAMP-sized host tree exceeds the old single-bitmap 4096-block cap.
// Pin two properties: synthesis succeeds beyond 2 MiB, and allocation starts
// after every contiguous bitmap block rather than overwriting bitmap #2.
static void testMultipleBitmapBlocks()
{
    const fs::path src = makeTempDir("multi_bitmap_src");
    const fs::path big = src / "BIG";
    fs::create_directories(big);
    for (int n = 0; n < 40; ++n) {
        std::vector<std::uint8_t> body(65536,
            static_cast<std::uint8_t>(n + 1));
        char name[16];
        std::snprintf(name, sizeof(name), "F%02d.bin", n);
        writeFile(big / name, body);
    }

    std::vector<std::uint8_t> image;
    const auto br = pom2::buildVolumeFromFolder(src.string(), "HOST", image);
    assert(br.ok);
    assert(br.filesIncluded == 40 && br.filesSkipped == 0);
    assert(br.totalBlocks > 4096);
    assert(image.size() == br.totalBlocks * kBlockBytes);

    // Root's first child is BIG. With two bitmap blocks (6 and 7), its
    // directory key block must start at block 8 or later.
    const std::uint8_t* entry = image.data() + 2 * kBlockBytes + 4 + 39;
    assert((entry[0] >> 4) == 0xD); // subdirectory storage type
    assert(rd16(entry + 0x11) >= 8);

    const fs::path dst = makeTempDir("multi_bitmap_dst");
    const auto dr = pom2::decodeVolumeToFolder(image, dst.string());
    assert(dr.ok && dr.filesWritten == 40);
    assert(fs::file_size(dst / "BIG" / "F39.bin") == 65536);

    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
    std::printf("prodos_volume_smoke: multiple bitmap blocks OK\n");
}

static std::string slurp(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// A host file edited AFTER the mount snapshot must survive the write-back:
// the volume holds the mount-time copy, and rewriting it silently reverted
// the user's edit while reporting a successful save. The legacy nullptr
// call keeps the overwrite-everything behaviour.
static void testHostNewerFilePreserved()
{
    const fs::path dir = makeTempDir("hostnewer");
    writeFile(dir / "NOTES.txt", {'o', 'l', 'd'});

    std::vector<std::uint8_t> img;
    assert(pom2::buildVolumeFromFolder(dir.string(), "HOST", img).ok);

    // Guard the round-trip name EXPLICITLY (the reason this test used to fail
    // only on case-sensitive Linux): decode the volume into a fresh, empty
    // directory and require the file it writes back to be exactly "NOTES.txt".
    // A case-sensitive FS is what makes a mismatch fatal, so name it directly
    // rather than trust the case-insensitive host to hide it.
    {
        const fs::path fresh = makeTempDir("hostnewer_rt");
        auto rr = pom2::decodeVolumeToFolder(img, fresh.string(), nullptr);
        assert(rr.ok);
        assert(fs::exists(fresh / "NOTES.txt") &&
               "ProDOS name round-trip must reproduce the exact host filename");
        std::error_code ec;
        fs::remove_all(fresh, ec);
    }

    // The user edits the file on the host AFTER the mount snapshot.
    writeFile(dir / "NOTES.txt", {'n', 'e', 'w', 'e', 'r'});

    // The filename must round-trip through the ProDOS name normalisation
    // UNCHANGED, or the skip cannot match the on-disk file. sanitiseProDOSName
    // uppercases the base and strips a known extension into the file_type;
    // decode re-adds it via extFromFileType, which is LOWERCASE (".txt"). So
    // "NOTES.TXT" comes back as "NOTES.txt" — a no-op on a case-insensitive FS
    // (macOS), but on a case-sensitive one (Linux CI) `last_write_time(dest)`
    // then misses the file, sets its error_code, and the newer-file skip never
    // runs (filesSkipped stays 0). "NOTES.txt" is already the round-trip form,
    // so dest == the host file on every FS. (This, not any clock/mtime issue,
    // is why the test failed only on Linux.)
    //
    // Derive the mount stamp from the edited file's OWN recorded mtime backed
    // off 24 h — no `now()` clock mixing (libc++ aliases the file clock,
    // libstdc++ does not) and no `last_write_time` setter — so both sides of
    // the `dest_mtime > newerThan` compare read the same on-disk timestamp.
    const auto newerThan =
        fs::last_write_time(dir / "NOTES.txt") - std::chrono::hours(24);

    auto r = pom2::decodeVolumeToFolder(img, dir.string(), &newerThan);
    assert(r.ok);
    assert(r.filesSkipped >= 1);
    assert(slurp(dir / "NOTES.txt") == "newer" &&
           "write-back reverted a host-newer file to the mount snapshot");

    // Legacy callers (no stamp) keep the overwrite-everything behaviour.
    r = pom2::decodeVolumeToFolder(img, dir.string(), nullptr);
    assert(r.ok);
    assert(slurp(dir / "NOTES.txt") == "old");

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("prodos_volume_smoke: host-newer preservation OK\n");
}

// A host folder is not a closed tree: `directory_iterator` and `is_directory`
// both DEREFERENCE symlinks, so the scanner followed any link it met. `ln -s
// . loop` recursed to the depth cap, and a link to a directory outside the
// folder served that directory to the guest as part of the volume.
// (Bug hunt 2026-09-06 #H11.)
static void testSymlinkLoopsAndEscapesAreRefused()
{
    const fs::path root    = makeTempDir("symlink_root");
    const fs::path outside = makeTempDir("symlink_outside");
    writeFile(root / "INSIDE.txt",     {'i', 'n'});
    writeFile(outside / "SECRET.txt",  {'s', 'e', 'c'});

    std::error_code ec;
    fs::create_directory_symlink(".",      root / "LOOP", ec);
    if (ec) {                       // no symlink support on this filesystem
        fs::remove_all(root, ec);
        fs::remove_all(outside, ec);
        std::printf("prodos_volume_smoke: symlink guard SKIPPED (no symlinks)\n");
        return;
    }
    fs::create_directory_symlink(outside, root / "ESCAPE", ec);
    assert(!ec);

    std::vector<std::uint8_t> img;
    const auto br = pom2::buildVolumeFromFolder(root.string(), "HOST", img);
    assert(br.ok);

    // Decode into a fresh folder and look at what actually came through.
    const fs::path dst = makeTempDir("symlink_dst");
    const auto dr = pom2::decodeVolumeToFolder(img, dst.string());
    assert(dr.ok);
    assert(fs::exists(dst / "INSIDE.txt"));
    assert(!fs::exists(dst / "SECRET.txt"));
    for (const auto& e : fs::recursive_directory_iterator(dst, ec))
        assert(e.path().filename().string() != "SECRET.txt" &&
               "a symlink out of the served folder reached the guest");

    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
    fs::remove_all(dst, ec);
    std::printf("prodos_volume_smoke: symlink loop + escape refused OK\n");
}

// `HELLO.BAS` → ProDOS `HELLO` (+type $FC) → decode `HELLO` + ".bas". On a
// case-sensitive filesystem that is a SECOND file beside the user's, which
// the next mount turns into two ProDOS entries, and so on every cycle.
// (Bug hunt 2026-09-06 #H24.)
static void testExtensionCaseRoundTripDoesNotDuplicate()
{
    const fs::path dir = makeTempDir("extcase");
    writeFile(dir / "HELLO.BAS", {'h', 'i'});

    std::vector<std::uint8_t> img;
    assert(pom2::buildVolumeFromFolder(dir.string(), "HOST", img).ok);
    const auto dr = pom2::decodeVolumeToFolder(img, dir.string());
    assert(dr.ok);

    std::error_code ec;
    int files = 0;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) ++files;
    assert(files == 1 && "write-back duplicated the file under a new case");
    assert(fs::exists(dir / "HELLO.BAS"));

    fs::remove_all(dir, ec);
    std::printf("prodos_volume_smoke: extension case round-trip OK\n");
}

// `AUX`, `CON`, `COM1`… are legal ProDOS names AND Windows DOS devices in
// every directory, with any extension. Writing one back opened a port.
// (Bug hunt 2026-09-06 #H24.)
static void testWindowsReservedNamesRefused()
{
    assert(!pom2::isHostSafeProDOSName("AUX"));
    assert(!pom2::isHostSafeProDOSName("aux"));
    assert(!pom2::isHostSafeProDOSName("CON.TXT"));
    assert(!pom2::isHostSafeProDOSName("NUL"));
    assert(!pom2::isHostSafeProDOSName("PRN"));
    assert(!pom2::isHostSafeProDOSName("COM1"));
    assert(!pom2::isHostSafeProDOSName("LPT9.DAT"));
    // ...and names that merely start the same way stay legal.
    assert(pom2::isHostSafeProDOSName("AUXILIARY"));
    assert(pom2::isHostSafeProDOSName("COM10"));
    assert(pom2::isHostSafeProDOSName("CONFIG"));
    std::printf("prodos_volume_smoke: Windows device names refused OK\n");
}

int main()
{
    testEmptyFolder();
    testSeedlingAndSapling();
    testNameSanitisationAndCollisions();
    testRoundTripFolderToVolumeToFolder();
    testSubdirsBuildAndDecode();
    testMetadataTagParsing();
    testDecodeNeverMergesTwoEntriesOntoOneFile();
    testMultipleBitmapBlocks();
    testHostNewerFilePreserved();
    testSymlinkLoopsAndEscapesAreRefused();
    testExtensionCaseRoundTripDoesNotDuplicate();
    testWindowsReservedNamesRefused();
    std::printf("prodos_volume_smoke: PASS\n");
    return 0;
}
