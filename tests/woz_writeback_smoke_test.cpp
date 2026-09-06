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

// WOZ write-back smoke test.
//
// Pins the WOZ write-back path added in §2 Disques: writeFlux now
// splices into bitStream[qt] for WOZ images, saveDirty() repacks the
// dirty quarter-tracks back into the original file bytes (zeroing the
// CRC32 header per Applesauce WOZ 2.1 spec), and isWriteProtected()
// honours the user's writeBackEnabled opt-in.
//
// What this gates:
//
//   1. After load + setWriteBackEnabled(true), writeFlux on a known
//      cell range updates bitStream[qt] visibly via bitAt().
//   2. saveDirty() succeeds and produces a file that:
//        - has the same magic + structure as the input
//        - has CRC32 header bytes zeroed (sentinel for "skip CRC check")
//        - reads back through loadFile() with the modified bit pattern
//          intact at the same quarter-track / bit offset.
//   3. INFO.write_protected = 1 keeps the image WP regardless of the
//      writeBackEnabled toggle (the per-file WP byte wins).
//
// Both WOZ1 (160 fixed 6656-byte slots; bit_count at slot+6648) and
// WOZ2 (160 × 8-byte TRK headers + block-aligned bit data) are
// exercised. The synthetic WOZ builders live here too rather than
// being shared with `woz_load_smoke_test.cpp` — keeping the tests
// self-contained avoids a header-only dependency for a few utility
// functions.

#include "DiskImage.h"

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

std::vector<uint8_t> buildMinimalWoz1(const std::vector<uint8_t>& bitData,
                                      uint16_t bitCount,
                                      bool fileWp = false)
{
    std::vector<uint8_t> woz;
    woz.insert(woz.end(),
        {'W', 'O', 'Z', '1', 0xFF, 0x0A, 0x0D, 0x0A});
    putU32LE(woz, 0);   // CRC32 — left at zero so the loader skips checks.

    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };

    std::vector<uint8_t> info(60, 0);
    info[0] = 1; info[1] = 1; info[2] = fileWp ? 1 : 0;
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    addChunk("INFO", info);

    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;
    addChunk("TMAP", tmap);

    std::vector<uint8_t> trks(6656, 0);
    const size_t copyBytes = std::min<size_t>(bitData.size(), 6646);
    std::memcpy(trks.data(), bitData.data(), copyBytes);
    trks[6646] = static_cast<uint8_t>(copyBytes & 0xFF);
    trks[6647] = static_cast<uint8_t>((copyBytes >> 8) & 0xFF);
    trks[6648] = static_cast<uint8_t>(bitCount & 0xFF);
    trks[6649] = static_cast<uint8_t>((bitCount >> 8) & 0xFF);
    trks[6650] = 0xFF; trks[6651] = 0xFF;
    addChunk("TRKS", trks);
    return woz;
}

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

    std::vector<uint8_t> info(60, 0);
    info[0] = 2; info[1] = 1; info[2] = 0;
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    info[37] = 1;        // disk_sides
    info[39] = 32;       // optimal_bit_timing
    addChunk("INFO", info);

    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;
    addChunk("TMAP", tmap);

    // trk 0 header: starting_block=3, block_count=1, bit_count=bitCount.
    std::vector<uint8_t> trks;
    putU16LE(trks, 3);
    putU16LE(trks, 1);
    putU32LE(trks, bitCount);
    while (trks.size() < 160 * 8) trks.push_back(0);
    while (trks.size() < (1536 - 12 - 8 - 60 - 8 - 160 - 8)) {
        trks.push_back(0);
    }
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
        / (std::string("pom2_wozwb_") + tag + ".woz");
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp WOZ for writing");
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.close();
    return p.string();
}

std::vector<uint8_t> readWholeFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> out(sz);
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(sz));
    return out;
}

// Drive writeFlux to overwrite an 8-cell window starting at bit
// `cellOffset` of QT 0 with the cell pattern in `cellBits` (length 8,
// values 0 or 1). Each "1" in `cellBits` becomes a flux transition at
// the cell centre (cell*8 + 4 LSS cycles); each "0" leaves the cell
// quiet. The cell window is [cellOffset*8, (cellOffset+8)*8) LSS cycles.
void writeCellWindow(DiskImage& img, int cellOffset,
                     const std::array<uint8_t, 8>& cellBits)
{
    std::vector<int64_t> transitions;
    transitions.reserve(8);
    for (int b = 0; b < 8; ++b) {
        if (cellBits[b]) {
            transitions.push_back(
                static_cast<int64_t>(cellOffset + b) * 8 + 4);
        }
    }
    const int64_t start = static_cast<int64_t>(cellOffset) * 8;
    const int64_t end   = static_cast<int64_t>(cellOffset + 8) * 8;
    img.writeFlux(0, start, end,
                  static_cast<int>(transitions.size()),
                  transitions.data());
}

bool testWoz1WriteBackRoundTrip()
{
    // Initial bit data: 32 bits = D5 AA 96 EB MSB-first. Untouched
    // tail stays at 0xFF (the WOZ build pads).
    std::vector<uint8_t> bitData = {0xD5, 0xAA, 0x96, 0xEB};
    while (bitData.size() < 6646) bitData.push_back(0xFF);
    const uint16_t bitCount = 6646 * 8;
    const std::string path =
        writeTempFile(buildMinimalWoz1(bitData, bitCount), "v1_wb");

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: load WOZ1: %s\n", img.getLastError().c_str());
        return false;
    }
    img.setWriteBackEnabled(true);
    if (img.isWriteProtected()) {
        std::printf("FAIL: WOZ1 still WP after setWriteBackEnabled(true)\n");
        return false;
    }

    // Overwrite cells 0..7 with $A5 (10100101 MSB-first). cells {0,2,5,7}
    // → flux events; {1,3,4,6} → quiet.
    const std::array<uint8_t, 8> a5 = {1,0,1,0,0,1,0,1};
    writeCellWindow(img, 0, a5);

    // Visible immediately via bitAt.
    for (int i = 0; i < 8; ++i) {
        if (img.bitAt(0, i) != a5[i]) {
            std::printf("FAIL: in-memory bitAt(0,%d)=%d, expected %d\n",
                        i, img.bitAt(0, i), a5[i]);
            return false;
        }
    }

    if (!img.hasUnsavedChanges()) {
        std::printf("FAIL: hasUnsavedChanges() false after writeFlux\n");
        return false;
    }
    if (!img.saveDirty()) {
        std::printf("FAIL: saveDirty(): %s\n", img.getLastError().c_str());
        return false;
    }
    if (img.hasUnsavedChanges()) {
        std::printf("FAIL: hasUnsavedChanges() still true after saveDirty\n");
        return false;
    }

    // File on disk must have the CRC32 zeroed (sentinel for "not
    // computed") and the first byte of the TRKS bit data updated to
    // $A5.
    const auto bytes = readWholeFile(path);
    if (bytes.size() < 12) {
        std::printf("FAIL: saved file truncated\n"); return false;
    }
    for (int i = 8; i < 12; ++i) {
        if (bytes[i] != 0) {
            std::printf("FAIL: saved CRC32 byte %d = $%02X, expected 0\n",
                        i, bytes[i]);
            return false;
        }
    }

    // Locate TRKS chunk → first byte of bit data.
    // Layout: 12-byte hdr + INFO chunk hdr (8) + 60 INFO + TMAP chunk
    // hdr (8) + 160 TMAP + TRKS chunk hdr (8) = offset 256.
    const size_t bitDataOff = 12 + 8 + 60 + 8 + 160 + 8;
    if (bytes[bitDataOff] != 0xA5) {
        std::printf("FAIL: saved bit data byte 0 = $%02X, expected $A5\n",
                    bytes[bitDataOff]);
        return false;
    }
    // Bytes 1..3 must still match the original (we only wrote cell 0..7).
    if (bytes[bitDataOff + 1] != 0xAA
        || bytes[bitDataOff + 2] != 0x96
        || bytes[bitDataOff + 3] != 0xEB) {
        std::printf("FAIL: bytes 1..3 mutated: %02X %02X %02X\n",
                    bytes[bitDataOff + 1], bytes[bitDataOff + 2],
                    bytes[bitDataOff + 3]);
        return false;
    }

    // Re-load and verify bitAt round-trip.
    DiskImage img2;
    if (!img2.loadFile(path)) {
        std::printf("FAIL: re-load saved WOZ1: %s\n",
                    img2.getLastError().c_str());
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (img2.bitAt(0, i) != a5[i]) {
            std::printf("FAIL: re-loaded bitAt(0,%d)=%d, expected %d\n",
                        i, img2.bitAt(0, i), a5[i]);
            return false;
        }
    }
    std::printf("[ OK ] WOZ1 writeFlux → saveDirty → re-load round-trip\n");
    return true;
}

bool testWoz2WriteBackRoundTrip()
{
    std::vector<uint8_t> bitData = {0xD5, 0xAA, 0x96, 0xEB};
    while (bitData.size() < 512) bitData.push_back(0xFF);
    constexpr uint32_t bitCount = 4 * 8;
    const std::string path =
        writeTempFile(buildMinimalWoz2(bitData, bitCount), "v2_wb");

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: load WOZ2: %s\n", img.getLastError().c_str());
        return false;
    }
    img.setWriteBackEnabled(true);

    // Overwrite cells 0..7 with $5A (01011010 MSB-first).
    const std::array<uint8_t, 8> p = {0,1,0,1,1,0,1,0};
    writeCellWindow(img, 0, p);

    if (!img.saveDirty()) {
        std::printf("FAIL: WOZ2 saveDirty(): %s\n",
                    img.getLastError().c_str());
        return false;
    }
    const auto bytes = readWholeFile(path);
    // WOZ2 bit data lives at file offset 1536 (block 3).
    if (bytes.size() <= 1536 || bytes[1536] != 0x5A) {
        std::printf("FAIL: WOZ2 saved bit-data byte 0 = $%02X, expected $5A\n",
                    bytes.size() > 1536 ? bytes[1536] : 0xFF);
        return false;
    }
    DiskImage img2;
    if (!img2.loadFile(path)) {
        std::printf("FAIL: re-load WOZ2: %s\n", img2.getLastError().c_str());
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (img2.bitAt(0, i) != p[i]) {
            std::printf("FAIL: WOZ2 reload bitAt(0,%d)=%d, expected %d\n",
                        i, img2.bitAt(0, i), p[i]);
            return false;
        }
    }
    std::printf("[ OK ] WOZ2 writeFlux → saveDirty → re-load round-trip\n");
    return true;
}

bool testWoz1FileWriteProtected()
{
    // Build a WOZ1 with INFO.write_protected = 1. Even with the user
    // toggling writeBackEnabled(true), the file-level WP byte must keep
    // isWriteProtected() true — a physically WP'd source disk should
    // not be silently overwritten.
    std::vector<uint8_t> bitData(6646, 0xFF);
    bitData[0] = 0xD5; bitData[1] = 0xAA; bitData[2] = 0x96;
    const std::string path =
        writeTempFile(buildMinimalWoz1(bitData, 6646 * 8, /*fileWp=*/true),
                      "v1_wp");

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: load WOZ1-WP: %s\n", img.getLastError().c_str());
        return false;
    }
    img.setWriteBackEnabled(true);
    if (!img.isWriteProtected()) {
        std::printf("FAIL: file-WP WOZ became writable under writeBackEnabled\n");
        return false;
    }
    std::printf("[ OK ] WOZ1 INFO.write_protected respects user toggle\n");
    return true;
}

bool testWozUnsplicableQuarterTrackRefusesSave()
{
    // A dirty quarter-track whose bit stream is gone cannot be spliced back
    // into wozRaw. `saveDirty` used to `continue` past it, then
    // `wozQtDirty.fill(false)` + `return true`: the guest's writes to that
    // track were dropped while `hasUnsavedChanges()` went false and
    // `getLastError()` stayed empty, so the eject path saw a clean success.
    // Same rule as `reportUndecodable` for the sector formats — refuse, keep
    // the changes, let the user retry. (Bug hunt 2026-09-06 #7.)
    //
    // Reached here through the legacy nibble gate: `writeNibbleAt` on a WOZ
    // takes the non-WOZ branch, whose `invalidateWholeTrack` clears
    // `bitStream[qt]` for a quarter-track `writeFlux` had already marked
    // dirty.
    std::vector<uint8_t> bitData = {0xD5, 0xAA, 0x96, 0xEB};
    while (bitData.size() < 6646) bitData.push_back(0xFF);
    const std::string path =
        writeTempFile(buildMinimalWoz1(bitData, 6646 * 8), "v1_unsplicable");

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: load WOZ1: %s\n", img.getLastError().c_str());
        return false;
    }
    img.setWriteBackEnabled(true);

    const std::array<uint8_t, 8> a5 = {1,0,1,0,0,1,0,1};
    writeCellWindow(img, 0, a5);
    if (!img.hasUnsavedChanges()) {
        std::printf("FAIL: writeFlux left QT0 clean\n"); return false;
    }
    const auto pristine = readWholeFile(path);

    // Drop the bit stream QT0's dirty flag refers to.
    img.writeNibbleAt(0, 0, static_cast<uint8_t>(img.nibbleAt(0, 0) ^ 0xFF));

    if (img.saveDirty()) {
        std::printf("FAIL: save reported success with an unsplicable QT\n");
        return false;
    }
    if (!img.hasUnsavedChanges()) {
        std::printf("FAIL: refused save laundered the dirty flags\n");
        return false;
    }
    if (img.getLastError().empty()) {
        std::printf("FAIL: refused save left no error to show the user\n");
        return false;
    }
    if (readWholeFile(path) != pristine) {
        std::printf("FAIL: refused save wrote to the image anyway\n");
        return false;
    }
    std::printf("[ OK ] WOZ unsplicable quarter-track refuses the save\n");
    return true;
}

}  // namespace

int main()
{
    if (!testWoz1WriteBackRoundTrip())  return 1;
    if (!testWoz2WriteBackRoundTrip())  return 1;
    if (!testWoz1FileWriteProtected())  return 1;
    if (!testWozUnsplicableQuarterTrackRefusesSave()) return 1;
    std::printf("OK woz_writeback_smoke\n");
    return 0;
}
