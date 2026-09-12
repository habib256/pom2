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

// WOZ1 / WOZ2 loader smoke test.
//
// Pins the verbatim port of MAME `src/lib/formats/woz_dsk.cpp` against
// synthetic minimum-viable WOZ files we build in-memory:
//
//   1. WOZ1 header parse + bit_count read at offset +6648.
//   2. WOZ2 header parse + 8-byte TRK header (starting_block, block_count,
//      bit_count u32) + block-aligned bit data.
//   3. MSB-first bit packing within each byte (the prologue D5 AA 96
//      decomposes into 24 specific bits in a known order).
//   4. WOZ disks always report write-protected (first-cut: read-only).
//   5. End-to-end: insert a synthetic .woz into DiskIICard, drive Q6L
//      reads of $C0EC, and confirm the LSS surfaces D5 AA 96. Crucially,
//      this works WITHOUT loading roms/diskii_p6.rom — the embedded
//      default P6 ROM is sufficient and `insertDisk` forces useBitLss
//      whenever any drive holds a WOZ image.
//
// Building a minimal WOZ file:
//
//   12-byte header: magic "WOZ1\xFF\n\r\n" or "WOZ2\xFF\n\r\n", then
//                  4-byte CRC32 (we leave it zero; the loader doesn't
//                  validate, mirroring MAME's policy).
//   INFO chunk: 60 bytes — info_version, disk_type=1, write_protected,
//              etc. We set version=1 / 2 to taste; only the first 5
//              bytes are inspected by POM2.
//   TMAP chunk: 160 bytes; entry 0 = TRK 0, rest = $FF.
//   TRKS chunk: WOZ1 → one 6656-byte slot; WOZ2 → 8-byte hdr at offset 0
//              of the chunk, plus aligned-to-block-3 (file offset 1536)
//              raw bit data.
//
// Bit data: D5 AA 96 EB packed MSB-first into the first 4 bytes, rest
// 0xFF. bit_count = 32 (just the 4 bytes — keeps the test fast: each
// LSS revolution is ~256 cycles).

#include "DiskIICard.h"
#include "DiskImage.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void putU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
void putU16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

// Build a minimal valid WOZ1 image with one populated track at TMAP[0].
// bit_count = bitCount; first `(bitCount+7)/8` bytes carry the bit data
// (`bitData`), padded to 6646 bytes.
std::vector<uint8_t> buildMinimalWoz1(const std::vector<uint8_t>& bitData,
                                      uint16_t bitCount)
{
    std::vector<uint8_t> woz;
    // 12-byte header.
    woz.insert(woz.end(),
        {'W', 'O', 'Z', '1', 0xFF, 0x0A, 0x0D, 0x0A});
    putU32LE(woz, 0);                       // CRC32 (unchecked)

    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };

    // INFO (60 bytes).
    std::vector<uint8_t> info(60, 0);
    info[0] = 1;       // info_version
    info[1] = 1;       // disk_type 5.25"
    info[2] = 0;       // write_protected (file flag — we'll still
                       // report WP true because wozFormat overrides)
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    addChunk("INFO", info);

    // TMAP (160 bytes; only QT 0 populated).
    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;
    addChunk("TMAP", tmap);

    // TRKS — one 6656-byte slot.
    std::vector<uint8_t> trks(6656, 0);
    const size_t copyBytes = std::min<size_t>(bitData.size(), 6646);
    std::memcpy(trks.data(), bitData.data(), copyBytes);
    // bytes_used (LE u16) at +6646.
    trks[6646] = static_cast<uint8_t>(copyBytes & 0xFF);
    trks[6647] = static_cast<uint8_t>((copyBytes >> 8) & 0xFF);
    // bit_count (LE u16) at +6648.
    trks[6648] = static_cast<uint8_t>(bitCount & 0xFF);
    trks[6649] = static_cast<uint8_t>((bitCount >> 8) & 0xFF);
    // splice_point at +6650 = 0xFFFF (none).
    trks[6650] = 0xFF; trks[6651] = 0xFF;
    addChunk("TRKS", trks);
    return woz;
}

// Build a minimal valid WOZ2 image. Track data lives in block 3 (file
// offset 1536) — we pad the TRKS chunk to ensure that alignment.
std::vector<uint8_t> buildMinimalWoz2(const std::vector<uint8_t>& bitData,
                                      uint32_t bitCount)
{
    std::vector<uint8_t> woz;
    woz.insert(woz.end(),
        {'W', 'O', 'Z', '2', 0xFF, 0x0A, 0x0D, 0x0A});
    putU32LE(woz, 0);

    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };

    // INFO (60 bytes; v2 fields zeroed except optimal_bit_timing).
    std::vector<uint8_t> info(60, 0);
    info[0] = 2;        // info_version 2 (WOZ2)
    info[1] = 1;        // disk_type 5.25"
    info[2] = 0;        // write_protected
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    info[37] = 1;       // disk_sides
    info[39] = 32;      // optimal_bit_timing (4µs cells in 125ns units)
    addChunk("INFO", info);

    // TMAP (160 bytes).
    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;
    addChunk("TMAP", tmap);

    // TRKS payload — 160 × 8-byte headers, then padding so the first
    // populated track lands at file offset 1536 (= block 3 × 512).
    std::vector<uint8_t> trks;
    trks.reserve(1280);
    // Header for trk 0: starting_block = 3, block_count = 1, bit_count = bitCount.
    // (We allocate exactly one 512-byte block of bit data, which holds
    // up to 4096 bits. The test only uses 32.)
    putU16LE(trks, 3);                      // starting_block
    putU16LE(trks, 1);                      // block_count
    putU32LE(trks, bitCount);               // bit_count
    // Headers for trks 1..159: zero (= unused).
    while (trks.size() < 160 * 8) trks.push_back(0);

    // Now pad TRKS payload so the chunk's data aligns: the WOZ2 spec
    // says track bit data lives at file-absolute block boundaries. With
    // the 12-byte header + 8-byte INFO chunk header + 60 bytes INFO
    // payload + 8-byte TMAP chunk header + 160 bytes TMAP payload + 8
    // bytes TRKS chunk header = 256 bytes consumed before TRKS payload.
    // We want the bit data at file offset 1536, so pad TRKS payload to
    // 1536 - 256 = 1280 bytes (just our header table — already exactly
    // 1280) and then append 512 bytes of bit data after a chunk-aware
    // boundary. Since the parser locates bit data via starting_block =
    // 3 (file offset 1536), we extend the TRKS chunk to include the
    // 512-byte block at offset 1536.
    while (trks.size() < (1536 - 12 - 8 - 60 - 8 - 160 - 8)) {
        trks.push_back(0);
    }
    // trks.size() now == 1280; the chunk payload after this block has
    // 512 bytes of bit data starting at chunk-payload offset 1280, which
    // maps to file offset 12 + 8 + 60 + 8 + 160 + 8 + 1280 = 1536. ✓
    std::vector<uint8_t> blockData(512, 0);
    const size_t copy = std::min<size_t>(bitData.size(), 512);
    std::memcpy(blockData.data(), bitData.data(), copy);
    trks.insert(trks.end(), blockData.begin(), blockData.end());
    addChunk("TRKS", trks);
    return woz;
}

std::string writeTempFile(const std::vector<uint8_t>& bytes,
                          const char* tag)
{
    const auto p = fs::temp_directory_path()
        / (std::string("pom2_woz_") + tag + ".woz");
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp WOZ for writing");
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.close();
    return p.string();
}

bool testWoz1Parse() {
    // Bit data: D5 AA 96 EB followed by 0xFF run.
    std::vector<uint8_t> bitData;
    bitData.push_back(0xD5);
    bitData.push_back(0xAA);
    bitData.push_back(0x96);
    bitData.push_back(0xEB);
    while (bitData.size() < 6646) bitData.push_back(0xFF);
    const uint16_t bitCount = 6646 * 8;     // full slot
    const auto woz = buildMinimalWoz1(bitData, bitCount);
    const std::string path = writeTempFile(woz, "v1");

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: WOZ1 load: %s\n", img.getLastError().c_str());
        return false;
    }
    if (!img.isWoz()) {
        std::printf("FAIL: isWoz() false after .woz load\n"); return false;
    }
    // A WOZ honours the user's write-protect (media are writable by default
    // since 2026-09-08, so protect it explicitly here). Buildable WOZ1
    // fixtures don't set INFO.write_protected, so flipping the gate back
    // must lift WP.
    img.setWriteBackEnabled(false);
    if (!img.isWriteProtected()) {
        std::printf("FAIL: WOZ should be WP when writeBackEnabled=false\n"); return false;
    }
    if (img.trackBitLength(0) != bitCount) {
        std::printf("FAIL: trackBitLength(0)=%d, expected %d\n",
                    img.trackBitLength(0), bitCount);
        return false;
    }
    // 0xD5 = 11010101 MSB-first → bits 0..7
    const uint8_t expected[24] = {
        1,1,0,1, 0,1,0,1,            // 0xD5
        1,0,1,0, 1,0,1,0,            // 0xAA
        1,0,0,1, 0,1,1,0             // 0x96
    };
    for (int i = 0; i < 24; ++i) {
        if (img.bitAt(0, i) != expected[i]) {
            std::printf("FAIL: bitAt(0, %d)=%d, expected %d\n",
                        i, img.bitAt(0, i), expected[i]);
            return false;
        }
    }
    // WOZ write-back: opt-in via setWriteBackEnabled(true) must lift
    // the WP gate as long as INFO.write_protected is clear. (The
    // buildMinimalWoz1 fixture leaves byte +2 of INFO at 0, so the
    // file-level WP is not set — flipping the toggle alone is enough.)
    img.setWriteBackEnabled(true);
    if (img.isWriteProtected()) {
        std::printf("FAIL: WOZ should be writable when writeBackEnabled=true "
                    "and INFO.write_protected=0\n");
        return false;
    }
    std::printf("[ OK ] WOZ1 parse + bit unpack + writeBack gate\n");
    return true;
}

bool testWoz2Parse() {
    std::vector<uint8_t> bitData;
    bitData.push_back(0xD5);
    bitData.push_back(0xAA);
    bitData.push_back(0x96);
    bitData.push_back(0xEB);
    while (bitData.size() < 512) bitData.push_back(0xFF);
    const uint32_t bitCount = 32;            // just the 4 bytes
    const auto woz = buildMinimalWoz2(bitData, bitCount);
    const std::string path = writeTempFile(woz, "v2");

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: WOZ2 load: %s\n", img.getLastError().c_str());
        return false;
    }
    if (!img.isWoz()) {
        std::printf("FAIL: isWoz() false after WOZ2 load\n"); return false;
    }
    if (img.trackBitLength(0) != static_cast<int>(bitCount)) {
        std::printf("FAIL: WOZ2 trackBitLength(0)=%d, expected %u\n",
                    img.trackBitLength(0), bitCount);
        return false;
    }
    if (img.bitAt(0, 0) != 1 || img.bitAt(0, 1) != 1 || img.bitAt(0, 2) != 0
        || img.bitAt(0, 7) != 1) {
        std::printf("FAIL: WOZ2 bit unpack wrong (D5 → 11010101 MSB-first)\n");
        return false;
    }
    std::printf("[ OK ] WOZ2 parse + bit unpack\n");
    return true;
}

// Drive the controller: motor on, Q7L+Q6L, then read $C0EC at 8-cycle
// intervals collecting bytes with bit-7 set. De-dups consecutive same
// values. Mirrors diskii_lss_smoke's spinAndCollect.
std::vector<uint8_t> spinAndCollect(DiskIICard& card,
                                    int cpuCycles, size_t maxBytes)
{
    std::vector<uint8_t> out;
    out.reserve(maxBytes);
    constexpr int kCyclesPerRead = 8;
    card.deviceSelectRead(0x9);
    card.deviceSelectRead(0xE);
    card.deviceSelectRead(0xC);
    int spent = 0;
    while (spent < cpuCycles && out.size() < maxBytes) {
        card.advanceCycles(kCyclesPerRead);
        spent += kCyclesPerRead;
        const uint8_t b = card.deviceSelectRead(0xC);
        if (b & 0x80) {
            if (out.empty() || out.back() != b) out.push_back(b);
        }
    }
    return out;
}

bool testWozLssEndToEnd() {
    // Build a WOZ1 with the prologue at bits 0..23, then 0xFF padding.
    // Use a full-slot bit_count so the LSS gets a long sync run after
    // the prologue (mirrors how a real Apple II disk's track loops).
    std::vector<uint8_t> bitData;
    bitData.push_back(0xD5);
    bitData.push_back(0xAA);
    bitData.push_back(0x96);
    bitData.push_back(0xEB);
    while (bitData.size() < 6646) bitData.push_back(0xFF);
    const auto woz = buildMinimalWoz1(bitData, 6646 * 8);
    const std::string path = writeTempFile(woz, "lss");

    DiskIICard card;
    // No loadLssRom() — the embedded default P6 PROM is in the
    // constructor's p6Rom array. We rely on insertDisk to flip
    // useBitLss=true for WOZ even though p6RomLoaded is false.
    if (!card.insertDisk(path)) {
        std::printf("FAIL: insertDisk(.woz): %s\n",
                    card.getLastError().c_str());
        return false;
    }
    const auto bytes = spinAndCollect(card, 1'000'000, 96);
    bool found = false;
    for (size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] == 0xD5 && bytes[i+1] == 0xAA && bytes[i+2] == 0x96) {
            found = true;
            std::printf("[ OK ] WOZ → LSS recovered D5 AA 96 at offset %zu"
                        " of %zu collected\n", i, bytes.size());
            return true;
        }
    }
    if (!found) {
        std::printf("FAIL: D5 AA 96 not in LSS-decoded WOZ stream:");
        for (size_t i = 0; i < bytes.size() && i < 32; ++i)
            std::printf(" %02X", bytes[i]);
        std::printf("\n");
    }
    return false;
}

bool testWozRejectsNonWoz() {
    // Build a buffer that has the magic prefix but garbage in the
    // sentinel bytes. Should fail cleanly.
    std::vector<uint8_t> bad;
    bad.insert(bad.end(),
        {'W', 'O', 'Z', '1', 0x00, 0x00, 0x00, 0x00});  // wrong sentinels
    putU32LE(bad, 0);
    while (bad.size() < 12) bad.push_back(0);
    const std::string path = writeTempFile(bad, "bad");
    DiskImage img;
    if (img.loadFile(path)) {
        std::printf("FAIL: malformed WOZ accepted\n"); return false;
    }
    std::printf("[ OK ] malformed WOZ rejected: %s\n",
                img.getLastError().c_str());
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= testWoz1Parse();
    ok &= testWoz2Parse();
    ok &= testWozLssEndToEnd();
    ok &= testWozRejectsNonWoz();
    if (ok) {
        std::printf("woz_load_smoke OK\n");
        return 0;
    }
    std::printf("woz_load_smoke FAILED\n");
    return 1;
}
