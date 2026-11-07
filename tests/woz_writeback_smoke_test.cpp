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
// dirty quarter-tracks back into the original file bytes (recomputing the
// CRC32 header over what reaches the file), and isWriteProtected()
// honours the user's writeBackEnabled opt-in.
//
// What this gates:
//
//   1. After load + setWriteBackEnabled(true), writeFlux on a known
//      cell range updates bitStream[qt] visibly via bitAt().
//   2. saveDirty() succeeds and produces a file that:
//        - has the same magic + structure as the input
//        - carries the CRC32 of its own bytes 12..EOF (it used to be left
//          at zero, the "not computed" sentinel: legal, and the end of the
//          file's only integrity check)
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

// `sharedTrack`: also give quarter-tracks 3, 4 and 5 ONE second TRK (the
// shape every real image has — a whole track owns two or three TMAP slots).
std::vector<uint8_t> buildMinimalWoz2(const std::vector<uint8_t>& bitData,
                                      uint32_t bitCount,
                                      bool sharedTrack = false)
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
    if (sharedTrack) tmap[3] = tmap[4] = tmap[5] = 1;
    addChunk("TMAP", tmap);

    // trk 0 header: starting_block=3, block_count=1, bit_count=bitCount.
    std::vector<uint8_t> trks;
    putU16LE(trks, 3);
    putU16LE(trks, 1);
    putU32LE(trks, bitCount);
    if (sharedTrack) {                   // trk 1: the block after it
        putU16LE(trks, 4);
        putU16LE(trks, 1);
        putU32LE(trks, bitCount);
    }
    while (trks.size() < 160 * 8) trks.push_back(0);
    while (trks.size() < (1536 - 12 - 8 - 60 - 8 - 160 - 8)) {
        trks.push_back(0);
    }
    std::vector<uint8_t> blockData(512, 0);
    const size_t copy = std::min<size_t>(bitData.size(), 512);
    // An empty bitData has a null data(), which memcpy may not take even
    // with a zero count (UBSan nonnull-attribute).
    if (copy) std::memcpy(blockData.data(), bitData.data(), copy);
    trks.insert(trks.end(), blockData.begin(), blockData.end());
    if (sharedTrack) trks.insert(trks.end(), blockData.begin(), blockData.end());
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

    // File on disk must carry the CRC32 of its own contents and the first
    // byte of the TRKS bit data updated to $A5.
    const auto bytes = readWholeFile(path);
    if (bytes.size() < 12) {
        std::printf("FAIL: saved file truncated\n"); return false;
    }
    {
        const uint32_t stored = static_cast<uint32_t>(bytes[8]) |
                                (static_cast<uint32_t>(bytes[9]) << 8) |
                                (static_cast<uint32_t>(bytes[10]) << 16) |
                                (static_cast<uint32_t>(bytes[11]) << 24);
        const uint32_t want =
            DiskImage::wozCrc32(bytes.data() + 12, bytes.size() - 12);
        if (stored == 0 || stored != want) {
            std::printf("FAIL: saved CRC32 = $%08X, expected $%08X\n",
                        stored, want);
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
    // Reached here through `loadMediaSnapshot` — the rewind media codec —
    // whose `invalidateAllBitStreams()` clears `bitStream[qt]` for a
    // quarter-track `writeFlux` had already marked dirty, and which
    // `expandTrackBits` refuses to rebuild for a WOZ. (`writeNibbleAt` used
    // to be the way in; it now refuses on a WOZ, because a nibble write to a
    // bit-cell medium blanked the whole quarter-track — hunt 12.)
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
    std::vector<uint8_t> snap(DiskImage::kMediaSnapshotBytes, 0xFF);
    img.loadMediaSnapshot(snap.data(), snap.size());

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

// ── Quarter-tracks sharing one TRK are one surface ───────────────────────
// loadWoz unpacked every TMAP slot into its own bitStream, so a write on
// qt 4 was invisible from qt 3 and qt 5, and when two of them went dirty
// saveDirty spliced both into the same file offset — the higher qt's stale
// copy landed last and the other's write was gone from the file.
bool testSharedTrkQuarterTracksAlias()
{
    std::vector<uint8_t> bits(512, 0xFF);
    const std::string path = writeTempFile(
        buildMinimalWoz2(bits, 512 * 8, /*sharedTrack=*/true), "alias");
    DiskImage img;
    if (!img.loadFile(path)) { std::printf("FAIL: load alias WOZ2\n"); return false; }
    img.setWriteBackEnabled(true);

    auto writeOn = [&](int qt, int cellOffset, const std::array<uint8_t, 8>& c) {
        std::vector<int64_t> tr;
        for (int b = 0; b < 8; ++b)
            if (c[b]) tr.push_back(static_cast<int64_t>(cellOffset + b) * 8 + 4);
        img.writeFlux(qt, static_cast<int64_t>(cellOffset) * 8,
                      static_cast<int64_t>(cellOffset + 8) * 8,
                      static_cast<int>(tr.size()), tr.data());
    };
    const std::array<uint8_t, 8> a5 = {1,0,1,0,0,1,0,1};   // $A5
    const std::array<uint8_t, 8> c3 = {1,1,0,0,0,0,1,1};   // $C3
    writeOn(4, 0, a5);
    for (int i = 0; i < 8; ++i) {
        if (img.bitAt(3, i) != a5[i] || img.bitAt(5, i) != a5[i]) {
            std::printf("FAIL: write on qt 4 not visible from qt %d at cell %d\n",
                        img.bitAt(3, i) != a5[i] ? 3 : 5, i);
            return false;
        }
    }
    writeOn(5, 16, c3);
    if (!img.saveDirty()) { std::printf("FAIL: saveDirty alias\n"); return false; }
    DiskImage img2;
    if (!img2.loadFile(path)) { std::printf("FAIL: reload alias WOZ2\n"); return false; }
    for (int i = 0; i < 8; ++i) {
        if (img2.bitAt(4, i) != a5[i] || img2.bitAt(4, 16 + i) != c3[i]) {
            std::printf("FAIL: after save+reload, edit on qt %d lost at cell %d\n",
                        img2.bitAt(4, i) != a5[i] ? 4 : 5, i);
            return false;
        }
    }
    std::error_code ec;
    fs::remove(path, ec);
    std::printf("  ok: quarter-tracks sharing a TRK write and save as one surface\n");
    return true;
}

std::vector<uint8_t> buildFluxWoz(bool protectedInfo = false)
{
    auto woz = buildMinimalWoz2({}, 200);
    // Standard WOZ2 layout: INFO payload 20, TMAP payload 88,
    // TRKS headers 256, track block 1536, FLUX chunk 2048.
    woz[20] = 3;             // INFO version 3 (WOZ 2.1)
    woz[22] = protectedInfo ? 1 : 0;
    woz[66] = 4;             // FLUX chunk at block 4
    woz[68] = 1;             // largest flux track: one block
    woz[88] = 0xFF;          // FLUX, not TMAP, owns qt 0
    woz[260] = 9;            // 9 delta bytes in TRKS entry 0
    const std::array<uint8_t, 9> flux = {
        33, 33, 31,         // events at 33, 66 and 97 ticks
        255, 255, 255, 255, 255, 228 // wrap: period 1600 ticks
    };
    std::copy(flux.begin(), flux.end(), woz.begin() + 1536);
    woz.insert(woz.end(), {'F','L','U','X'});
    putU32LE(woz, 160);
    std::vector<uint8_t> map(160, 0xFF);
    map[0] = map[1] = 0;    // two quarter tracks, one physical surface
    woz.insert(woz.end(), map.begin(), map.end());
    return woz;
}

// ── The clock these FLUX mechanics tests run on ──────────────────────────
// A flux stream counts 125 ns ticks and the LSS counts half bus cycles, so
// their ratio is the machine's: 176/45 ticks per cycle on an NTSC Apple II,
// 256/65 on a PAL one (DiskImage.h, fluxBusHz_) — pinned by
// testFluxTimeBase below. The tests in between are about windows, aliases,
// seams and slot growth; they run on a 1 MHz bus, where a cycle is exactly
// 4 ticks, so their positions stay readable (tick 1600 = cycle 400).
constexpr double kTickAlignedHz = 1000000.0;

bool testFluxWriteBackRoundTrip()
{
    const auto path = writeTempFile(buildFluxWoz(), "flux");
    DiskImage img;
    img.setFluxClockHz(kTickAlignedHz);
    if (!img.loadFile(path)) {
        std::printf("FAIL: load FLUX: %s\n", img.getLastError().c_str());
        return false;
    }
    img.setWriteBackEnabled(true);
    if (img.isWriteProtected() || img.trackPeriod(0) != 400) {
        std::printf("FAIL: writable FLUX loaded as protected or wrong period\n");
        return false;
    }
    int64_t pulse = 109;
    img.writeFlux(0, 100, 120, 1, &pulse);
    pulse = 149;
    img.writeFlux(1, 140, 160, 1, &pulse);
    // 0 = the stream's closing transition (its last byte, 228).
    const std::vector<int> expected{0, 8, 16, 24, 109, 149};
    if (img.fluxEvents(0) != expected || img.fluxEvents(1) != expected) {
        std::printf("FAIL: FLUX aliases diverged before save\n"); return false;
    }
    if (!img.saveDirty()) {
        std::printf("FAIL: save FLUX: %s\n", img.getLastError().c_str());
        return false;
    }
    const auto saved = readWholeFile(path);
    if (saved[1536] != 33 || saved[1537] != 33 || saved[1538] != 31 ||
        saved[260] <= 9 || saved[2048] != 'F') {
        std::printf("FAIL: FLUX deltas/header/chunk not preserved\n"); return false;
    }
    DiskImage reloaded;
    reloaded.setFluxClockHz(kTickAlignedHz);
    if (!reloaded.loadFile(path) || reloaded.fluxEvents(0) != expected ||
        reloaded.fluxEvents(1) != expected) {
        std::printf("FAIL: FLUX save/reload lost transition\n"); return false;
    }
    const auto wp = writeTempFile(buildFluxWoz(true), "flux_wp");
    DiskImage protectedImage;
    protectedImage.setFluxClockHz(kTickAlignedHz);
    if (!protectedImage.loadFile(wp) || !protectedImage.isWriteProtected()) {
        std::printf("FAIL: INFO write protect ignored for FLUX\n"); return false;
    }
    protectedImage.setWriteBackEnabled(true);
    protectedImage.writeFlux(0, 100, 120, 1, &pulse);
    if (protectedImage.hasUnsavedChanges()) {
        std::printf("FAIL: protected FLUX accepted write\n"); return false;
    }
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(wp, ec);
    std::printf("  ok: FLUX ticks, alias, save/reload, and INFO protection\n");
    return true;
}

// Bug hunt 2026-11-01 round 4. (a) A FLUX record whose `block_count` runs
// past the TRKS chunk (here into the FLUX chunk) mounted with the whole count
// as its write capacity, and the save refused every write of the track. Its
// capacity is now what TRKS gives it, and a save neither fails nor touches
// the FLUX chunk. (b) [B] WOZ 2.1 reference, TRKS: 160 TRK records; a FLUX
// entry past them is damage, not a record.
bool testFluxRecordBounds()
{
    auto woz = buildFluxWoz();
    woz[258] = 2;            // block_count 2: blocks 3-4, but block 4 is FLUX
    const auto path = writeTempFile(woz, "flux_overrun");
    DiskImage img;
    img.setFluxClockHz(kTickAlignedHz);
    if (!img.loadFile(path) || img.trackPeriod(0) != 400) {
        std::printf("FAIL: overrunning FLUX record did not mount: %s\n",
                    img.getLastError().c_str());
        return false;
    }
    img.setWriteBackEnabled(true);
    int64_t pulse = 109;
    img.writeFlux(0, 100, 120, 1, &pulse);
    if (!img.saveDirty()) {
        std::printf("FAIL: save of an overrunning FLUX record: %s\n",
                    img.getLastError().c_str());
        return false;
    }
    const auto saved = readWholeFile(path);
    if (saved.size() < 2056 || saved[2048] != 'F' || saved[2049] != 'L' ||
        saved[2056] != 0 || saved[2057] != 0) {
        std::printf("FAIL: the FLUX save ran over the FLUX chunk\n"); return false;
    }

    auto bad = buildFluxWoz();
    const size_t fluxMap = bad.size() - 160;
    bad[fluxMap + 8] = 200;  // qt 8 → TRK record 200: in the data
    // ...where a plausible header sits (TRKS payload 256 + 200 × 8 = 1856,
    // inside the data block): block 3, one block, 9 bytes.
    const uint8_t fakeHdr[8] = {3, 0, 1, 0, 9, 0, 0, 0};
    std::copy(std::begin(fakeHdr), std::end(fakeHdr), bad.begin() + 1856);
    const auto badPath = writeTempFile(bad, "flux_idx200");
    DiskImage img2;
    if (!img2.loadFile(badPath) || img2.trackPeriod(8) != 0 ||
        img2.integrityWarning().empty()) {
        std::printf("FAIL: FLUX index 200 was not reported as damage\n");
        return false;
    }
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(badPath, ec);
    std::printf("  ok: FLUX capacity stays in TRKS; a FLUX index >= 160 is damage\n");
    return true;
}

// [B] WOZ 2.1 reference, TRKS: every non-255 byte of a flux stream is a
// transition, the LAST one included — it closes the loop at position 0
// (audit parite-flopimg-as_dsk-wozfdc-2026-11-01, constat 1). So (a) an
// unchanged track rewrites byte for byte, and (b) a track whose transition
// at 0 was overwritten must still END its stream on a real transition —
// rotated, not padded with a pulse the surface never had.
bool testFluxStreamEndsOnTransition()
{
    const auto path = writeTempFile(buildFluxWoz(), "flux_close");
    const auto original = readWholeFile(path);
    DiskImage img;
    img.setFluxClockHz(kTickAlignedHz);
    if (!img.loadFile(path)) { std::printf("FAIL: load FLUX (close)\n"); return false; }
    img.setWriteBackEnabled(true);
    int64_t pulse = 109;
    img.writeFlux(0, 100, 120, 1, &pulse);
    img.writeFlux(0, 100, 120, 0, nullptr);    // back to the loaded surface
    if (!img.saveDirty()) { std::printf("FAIL: save FLUX (close)\n"); return false; }
    auto saved = readWholeFile(path);
    if (!std::equal(original.begin() + 1536, original.begin() + 1545, saved.begin() + 1536) ||
        saved[260] != 9) {
        std::printf("FAIL: unchanged FLUX track did not rewrite byte for byte\n");
        return false;
    }
    img.writeFlux(0, 0, 4, 0, nullptr);        // erase the closing transition
    if (!img.saveDirty()) { std::printf("FAIL: save FLUX (erase)\n"); return false; }
    DiskImage reloaded;
    reloaded.setFluxClockHz(kTickAlignedHz);
    if (!reloaded.loadFile(path) || reloaded.trackPeriod(0) != 400 ||
        reloaded.fluxEvents(0) != std::vector<int>{0, 8, 16}) {
        std::printf("FAIL: FLUX stream without its seam transition reloaded "
                    "%zu events (want 3, rotated: 0 8 16)\n",
                    reloaded.fluxEvents(0).size());
        return false;
    }
    std::error_code ec;
    fs::remove(path, ec);
    std::printf("  ok: a FLUX stream closes on a transition (rewrite + rotate)\n");
    return true;
}

// A write window longer than one revolution: the surface keeps the LAST
// revolution's flux (STORAGE-013). Period 400 LSS cycles; the window runs
// 0..900, so 500..900 is what stays. Transitions at 30 and 470 belong to
// revolutions the head wrote over again; 610 (position 210) and 850
// (position 50) are the last pass. The old code kept the first revolution
// and folded the rest onto it: {30, 50, 70, 210}.
bool testFluxLongWindowKeepsLastRevolution()
{
    const auto path = writeTempFile(buildFluxWoz(), "flux_long");
    DiskImage img;
    img.setFluxClockHz(kTickAlignedHz);
    if (!img.loadFile(path) || img.trackPeriod(0) != 400) {
        std::printf("FAIL: load FLUX for the long window\n"); return false;
    }
    img.setWriteBackEnabled(true);
    const int64_t pulses[] = {30, 470, 610, 850};
    img.writeFlux(0, 0, 900, 4, pulses);
    const std::vector<int> expected{50, 210};
    if (img.fluxEvents(0) != expected) {
        std::printf("FAIL: long FLUX window kept %zu events, not the last revolution\n",
                    img.fluxEvents(0).size());
        return false;
    }
    std::error_code ec;
    fs::remove(path, ec);
    std::printf("  ok: a FLUX window past one revolution keeps the last one\n");
    return true;
}

bool testFluxSlotGrowth()
{
    auto source = buildFluxWoz();
    source[260] = 0; source[261] = 2; // 512 delta bytes
    std::fill(source.begin() + 1536, source.begin() + 2047, 4);
    source[2047] = 36;               // 2080 ticks = 520 LSS cycles
    const auto path = writeTempFile(source, "flux_grow");
    DiskImage img;
    img.setFluxClockHz(kTickAlignedHz);
    if (!img.loadFile(path) || img.trackPeriod(0) != 520) {
        std::printf("FAIL: load dense FLUX\n"); return false;
    }
    img.setWriteBackEnabled(true);
    std::vector<int64_t> pulses;
    for (int i = 0; i < 513; ++i) pulses.push_back(i);
    img.writeFlux(0, 0, 520, static_cast<int>(pulses.size()), pulses.data());
    if (!img.saveDirty()) {
        std::printf("FAIL: grow FLUX: %s\n", img.getLastError().c_str());
        return false;
    }
    const auto saved = readWholeFile(path);
    if (saved[256] != 4 || saved[258] != 2 ||
        // 513 transitions → 512 deltas + the closing one (on the pulse at
        // 0) = 513 bytes. The writer used to open with a 0-tick byte for
        // that pulse and close on a padding byte: 514, one transition too
        // many under the WOZ 2.1 reading (every non-255 byte is one).
        saved[260] != 1 || saved[261] != 2 ||
        saved[66] != 6 || saved[3072] != 'F') {
        std::printf("FAIL: grown TRKS/INFO/FLUX offsets\n"); return false;
    }
    DiskImage reloaded;
    reloaded.setFluxClockHz(kTickAlignedHz);
    if (!reloaded.loadFile(path) || reloaded.fluxEvents(0).size() != 513 ||
        reloaded.fluxEvents(1).size() != 513) {
        std::printf("FAIL: grown FLUX does not reload\n"); return false;
    }
    img.writeFlux(1, 10, 11, 0, nullptr); // save again via the alias
    if (!img.saveDirty()) {
        std::printf("FAIL: second save after FLUX growth\n"); return false;
    }
    DiskImage twice;
    twice.setFluxClockHz(kTickAlignedHz);
    if (!twice.loadFile(path) || twice.fluxEvents(0).size() != 512 ||
        twice.fluxEvents(1).size() != 512) {
        std::printf("FAIL: second FLUX save lost the grown slot\n"); return false;
    }
    std::error_code ec;
    fs::remove(path, ec);
    std::printf("  ok: FLUX TRKS allocation grows across FLUX chunk\n");
    return true;
}

// The same growth inside a MacBinary wrapper: the header's data-fork length
// (bytes 83-86) and the padding to 128 must follow the bigger WOZ, or the
// reload cuts it at the old length and refuses the disk.
bool testFluxSlotGrowthInMacBinary()
{
    auto woz = buildFluxWoz();
    woz[260] = 0; woz[261] = 2;
    std::fill(woz.begin() + 1536, woz.begin() + 2047, 4);
    woz[2047] = 36;
    std::vector<uint8_t> file(128, 0);
    file[1] = 5; std::memcpy(&file[2], "a.woz", 5);
    const std::size_t len = woz.size();
    file[83] = uint8_t(len >> 24); file[84] = uint8_t(len >> 16);
    file[85] = uint8_t(len >> 8);  file[86] = uint8_t(len);
    file.insert(file.end(), woz.begin(), woz.end());
    while (file.size() % 128) file.push_back(0);
    const auto path = writeTempFile(file, "mb_flux_grow");
    DiskImage img;
    img.setFluxClockHz(kTickAlignedHz);
    if (!img.loadFile(path)) { std::printf("FAIL: load MacBinary FLUX\n"); return false; }
    img.setWriteBackEnabled(true);
    std::vector<int64_t> pulses;
    for (int i = 0; i < 513; ++i) pulses.push_back(i);
    img.writeFlux(0, 0, 520, static_cast<int>(pulses.size()), pulses.data());
    if (!img.saveDirty()) { std::printf("FAIL: grow MacBinary FLUX\n"); return false; }
    const auto saved = readWholeFile(path);
    const std::size_t fork = (std::size_t(saved[83]) << 24) | (std::size_t(saved[84]) << 16) |
                             (std::size_t(saved[85]) << 8)  |  std::size_t(saved[86]);
    if (fork <= len || saved.size() % 128 != 0 || saved.size() < 128 + fork) {
        std::printf("FAIL: MacBinary fork length/padding not refitted\n"); return false;
    }
    DiskImage reloaded;
    reloaded.setFluxClockHz(kTickAlignedHz);
    if (!reloaded.loadFile(path) || reloaded.fluxEvents(0).size() != 513) {
        std::printf("FAIL: grown MacBinary WOZ does not reload: %s\n",
                    reloaded.getLastError().c_str());
        return false;
    }
    std::error_code ec;
    fs::remove(path, ec);
    std::printf("  ok: MacBinary envelope follows a grown FLUX slot\n");
    return true;
}

// ── The flux time base ───────────────────────────────────────────────────
// [B] WOZ 2.1 reference: a flux stream counts 125 ns ticks.
// [B] Sather: the sequencer steps twice per bus cycle — 488.9 ns on an NTSC
//     machine (1 022 727 Hz), 492.3 ns on a PAL one (1 015 625 Hz).
// A revolution of 1 600 000 ticks (200 ms) is therefore 409 091 LSS cycles
// (NTSC) or 406 250 (PAL), and a transition every 32 ticks falls every
// 8.18 cycles. POM2 divided by a flat 4 until 2026-11-06 — 400 000 and
// "every 8" — which played every flux track 2.27 % fast.
bool testFluxTimeBase()
{
    // One revolution, a transition every 4 us: 50 000 delta bytes of 32,
    // stored after the FLUX chunk (track blocks are absolute file offsets).
    auto woz = buildFluxWoz();
    constexpr uint32_t kDeltas = 50000;
    // ...inside a chunk of its own, so the chunk walk steps over it.
    const size_t wrapper = woz.size();
    woz.insert(woz.end(), {'P','A','D','S', 0, 0, 0, 0});
    woz.resize(5 * 512, 0);
    woz.insert(woz.end(), kDeltas, 32);
    woz.resize((woz.size() + 511) / 512 * 512, 0);
    const uint32_t wrapped = static_cast<uint32_t>(woz.size() - wrapper - 8);
    for (int i = 0; i < 4; ++i)
        woz[wrapper + 4 + i] = static_cast<uint8_t>(wrapped >> (8 * i));
    woz[256] = 5; woz[257] = 0;                       // starting block
    woz[258] = static_cast<uint8_t>((kDeltas + 511) / 512);
    woz[259] = 0;
    woz[260] = static_cast<uint8_t>(kDeltas);
    woz[261] = static_cast<uint8_t>(kDeltas >> 8);
    woz[262] = woz[263] = 0;
    const auto path = writeTempFile(woz, "flux_timebase");

    DiskImage img;                                    // a bare image is NTSC
    if (!img.loadFile(path)) {
        std::printf("FAIL: load 200 ms FLUX: %s\n", img.getLastError().c_str());
        return false;
    }
    if (img.fluxClockHz() != 1022727 || img.trackPeriod(0) != 409091) {
        std::printf("FAIL: NTSC flux period %d (want 409091)\n", img.trackPeriod(0));
        return false;
    }
    const std::vector<int> ntsc = img.fluxEvents(0);
    // 32 ticks = 8.18 cycles, and a transition belongs to the cycle it falls
    // in: 0, 8.18, 16.36, 24.55, 32.73, 40.91, 49.09 (the first one a flat
    // "8 per cell" misses), ... and the 45th lands in 368 — it was 360.
    if (ntsc.size() != kDeltas || ntsc[0] != 0 || ntsc[1] != 8 || ntsc[2] != 16 ||
        ntsc[3] != 24 || ntsc[6] != 49 || ntsc[45] != 368 || ntsc.back() >= 409091) {
        std::printf("FAIL: NTSC flux events misplaced (%zu, [6]=%d, [45]=%d)\n",
                    ntsc.size(), ntsc.size() > 6 ? ntsc[6] : -1,
                    ntsc.size() > 45 ? ntsc[45] : -1);
        return false;
    }
    for (size_t i = 1; i < ntsc.size(); ++i) {
        const int gap = ntsc[i] - ntsc[i - 1];
        if (gap != 8 && gap != 9) {
            std::printf("FAIL: a 4 us interval read as %d cycles\n", gap);
            return false;
        }
    }
    // The synthetic cell view follows: one cell per 8 cycles of the period.
    if (img.trackBitLength(0) != (409091 + 7) / 8) {
        std::printf("FAIL: flux cell count %d\n", img.trackBitLength(0));
        return false;
    }

    // The same surface in a PAL machine's drive: re-projected, not reloaded.
    img.setFluxClockHz(1015625.0);
    if (img.trackPeriod(0) != 406250 || img.fluxEvents(0).size() != kDeltas ||
        img.fluxEvents(0)[45] != 365 || img.trackBitLength(0) != (406250 + 7) / 8) {
        std::printf("FAIL: PAL flux period %d / [45]=%d\n", img.trackPeriod(0),
                    img.fluxEvents(0)[45]);
        return false;
    }
    // A clock that is no bus clock is refused, not divided by.
    img.setFluxClockHz(0.0);
    img.setFluxClockHz(4000000.0);
    if (img.fluxClockHz() != 1015625) {
        std::printf("FAIL: an impossible bus clock was accepted\n"); return false;
    }
    img.setFluxClockHz(1022727.0);
    if (img.fluxEvents(0) != ntsc) {
        std::printf("FAIL: back on NTSC the projection differs\n"); return false;
    }

    // A write lands on the cycle it was written at, survives the file, and
    // leaves the surface outside its window alone — at the real ratio, where
    // cycles and ticks are each other's rounding.
    img.setWriteBackEnabled(true);
    const int64_t pulses[] = {100001, 100009, 100017};
    img.writeFlux(0, 100000, 100024, 3, pulses);
    std::vector<int> expected;
    for (int e : ntsc) if (e < 100000 || e >= 100024) expected.push_back(e);
    expected.insert(expected.end(), {100001, 100009, 100017});
    std::sort(expected.begin(), expected.end());
    if (img.fluxEvents(0) != expected) {
        std::printf("FAIL: NTSC flux write moved (%zu events, want %zu)\n",
                    img.fluxEvents(0).size(), expected.size());
        return false;
    }
    if (!img.saveDirty()) {
        std::printf("FAIL: save NTSC FLUX: %s\n", img.getLastError().c_str());
        return false;
    }
    const auto first = readWholeFile(path);
    DiskImage reloaded;
    if (!reloaded.loadFile(path) || reloaded.trackPeriod(0) != 409091 ||
        reloaded.fluxEvents(0) != expected) {
        std::printf("FAIL: NTSC flux write did not survive save + reload\n");
        return false;
    }
    // Writing the same flux again changes nothing, in memory or in the file.
    reloaded.setWriteBackEnabled(true);
    reloaded.writeFlux(0, 100000, 100024, 3, pulses);
    if (reloaded.hasUnsavedChanges() && !reloaded.saveDirty()) {
        std::printf("FAIL: re-save NTSC FLUX\n"); return false;
    }
    if (readWholeFile(path) != first) {
        std::printf("FAIL: rewriting identical flux changed the file\n"); return false;
    }
    // A window that runs to the end of the revolution takes the last ticks.
    reloaded.writeFlux(0, 409000, 409091, 0, nullptr);
    if (!reloaded.fluxEvents(0).empty() && reloaded.fluxEvents(0).back() >= 409000) {
        std::printf("FAIL: flux survived an erase to the end of the revolution "
                    "(last event %d)\n", reloaded.fluxEvents(0).back());
        return false;
    }
    std::error_code ec;
    fs::remove(path, ec);
    std::printf("  ok: FLUX ticks fall on the machine's own LSS cycles (NTSC, PAL)\n");
    return true;
}

int main()
{
    if (!testFluxTimeBase()) return 1;
    if (!testFluxWriteBackRoundTrip()) return 1;
    if (!testFluxLongWindowKeepsLastRevolution()) return 1;
    if (!testFluxStreamEndsOnTransition()) return 1;
    if (!testFluxSlotGrowth()) return 1;
    if (!testFluxSlotGrowthInMacBinary()) return 1;
    if (!testFluxRecordBounds()) return 1;
    if (!testSharedTrkQuarterTracksAlias()) return 1;
    if (!testWoz1WriteBackRoundTrip())  return 1;
    if (!testWoz2WriteBackRoundTrip())  return 1;
    if (!testWoz1FileWriteProtected())  return 1;
    if (!testWozUnsplicableQuarterTrackRefusesSave()) return 1;
    std::printf("OK woz_writeback_smoke\n");
    return 0;
}
