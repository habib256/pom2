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

// WOZ2 optimal_bit_timing smoke test.
//
// Pins the INFO+39 read in DiskImage::loadWoz against the flux-event
// view: the spacing between two consecutive flux pulses must scale with
// `optimal_bit_timing` (in 125ns units) divided by 4 (LSS clock = 2 MHz
// → 1 LSS cycle = 500ns = 4 × 125ns). At the default value of 32 (= 4µs
// cells) a "1" bit is centred at LSS cycle `i*8 + 4`. With 40 (= 5µs
// cells) the same bit is centred at `i*10 + 5`.
//
// We build three minimal WOZ2 images differing only in INFO[39]:
//   - 32 (default 5.25" timing)
//   - 40 (a +25% slower master, the spec's example for slow disks)
//   - 28 (a -12.5% faster master, e.g. an experimental tightly-packed image)
//
// Each image carries the same bit pattern in track 0 (a single "1" bit
// followed by zeros), and we verify:
//   1. DiskImage::trackPeriod() scales linearly with the timing field.
//   2. The first flux event lands at the expected centre cycle.
//
// Building on the helpers in woz_load_smoke_test.cpp, but kept self-
// contained so this TU compiles independently in CI bisects.

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

// Minimal WOZ2 with `optimal_bit_timing` settable. Track 0 carries
// `bitData` MSB-first; bit_count = bitCount.
std::vector<uint8_t> buildWoz2WithBitTiming(uint8_t optimalBitTiming,
                                            const std::vector<uint8_t>& bitData,
                                            uint32_t bitCount)
{
    std::vector<uint8_t> woz;
    woz.insert(woz.end(),
        {'W', 'O', 'Z', '2', 0xFF, 0x0A, 0x0D, 0x0A});
    putU32LE(woz, 0);                       // CRC32 (unchecked)

    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };

    // INFO (60 bytes; only the fields the loader inspects matter).
    std::vector<uint8_t> info(60, 0);
    info[0]  = 2;                           // info_version 2 (WOZ2)
    info[1]  = 1;                           // disk_type 5.25"
    info[2]  = 0;                           // write_protected
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    info[37] = 1;                           // disk_sides
    info[38] = 0;                           // boot_sector_format (irrelevant)
    info[39] = optimalBitTiming;            // ← the field under test
    addChunk("INFO", info);

    // TMAP (track 0 only).
    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;
    addChunk("TMAP", tmap);

    // TRKS — 160 × 8B headers + 1 block of bit data at file offset 1536.
    std::vector<uint8_t> trks;
    trks.reserve(1280 + 512);
    putU16LE(trks, 3);                      // starting_block
    putU16LE(trks, 1);                      // block_count
    putU32LE(trks, bitCount);               // bit_count (u32 LE)
    while (trks.size() < 160 * 8) trks.push_back(0);
    while (trks.size() < (1536 - 12 - 8 - 60 - 8 - 160 - 8)) trks.push_back(0);
    std::vector<uint8_t> blockData(512, 0);
    const size_t copy = std::min<size_t>(bitData.size(), 512);
    std::memcpy(blockData.data(), bitData.data(), copy);
    trks.insert(trks.end(), blockData.begin(), blockData.end());
    addChunk("TRKS", trks);
    return woz;
}

std::string writeTempFile(const std::vector<uint8_t>& bytes, const char* tag) {
    const auto p = fs::temp_directory_path()
        / (std::string("pom2_woz_bt_") + tag + ".woz");
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp WOZ for writing");
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.close();
    return p.string();
}

// Verify that an image with `optimal_bit_timing = obt` produces:
//   trackPeriod = 32 cells × (obt/4) LSS cycles
//   first flux event at (obt/4)/2 + 0  (since bit 0 is the only "1")
bool checkTimingForObt(uint8_t obt) {
    // 4 bytes (= 32 bits): MSB of byte 0 set, rest zero. Yields one
    // flux event at cell index 0 — easy to assert against.
    std::vector<uint8_t> bitData = { 0x80, 0x00, 0x00, 0x00 };
    const uint32_t bitCount = 32;

    const auto woz = buildWoz2WithBitTiming(obt, bitData, bitCount);
    const std::string path = writeTempFile(
        woz, std::to_string(obt).c_str());

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: load WOZ2 obt=%u: %s\n",
                    static_cast<unsigned>(obt), img.getLastError().c_str());
        return false;
    }
    if (!img.isWoz()) {
        std::printf("FAIL: WOZ flag not set for obt=%u\n",
                    static_cast<unsigned>(obt));
        return false;
    }

    const int expectedCyc    = obt / 4;          // LSS cycles per cell
    const int expectedPeriod = 32 * expectedCyc; // 32 bit cells in our payload
    const int actualPeriod   = img.trackPeriod(0);
    if (actualPeriod != expectedPeriod) {
        std::printf("FAIL: obt=%u trackPeriod=%d expected=%d\n",
                    static_cast<unsigned>(obt), actualPeriod, expectedPeriod);
        return false;
    }

    const auto& flux = img.fluxEvents(0);
    if (flux.size() != 1) {
        std::printf("FAIL: obt=%u flux count=%zu (expected 1)\n",
                    static_cast<unsigned>(obt), flux.size());
        return false;
    }
    const int expectedCentre = expectedCyc / 2;  // i=0 cell centre
    if (flux[0] != expectedCentre) {
        std::printf("FAIL: obt=%u flux[0]=%d expected=%d\n",
                    static_cast<unsigned>(obt), flux[0], expectedCentre);
        return false;
    }

    std::printf("OK : obt=%u → cyc=%d, period=%d, flux[0]=%d\n",
                static_cast<unsigned>(obt), expectedCyc,
                actualPeriod, flux[0]);
    return true;
}

// Sanity: WOZ1 (no INFO+39 field) must fall back to the 32-default.
bool checkWoz1FallsBackTo32() {
    // Build a minimal WOZ1: same shape as woz_load_smoke_test::buildMinimalWoz1
    // but inlined here so this TU is independent.
    std::vector<uint8_t> woz;
    woz.insert(woz.end(), {'W', 'O', 'Z', '1', 0xFF, 0x0A, 0x0D, 0x0A});
    putU32LE(woz, 0);
    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };
    std::vector<uint8_t> info(60, 0);
    info[0] = 1; info[1] = 1; info[2] = 0;
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    addChunk("INFO", info);
    std::vector<uint8_t> tmap(160, 0xFF); tmap[0] = 0;
    addChunk("TMAP", tmap);
    std::vector<uint8_t> trks(6656, 0);
    trks[0]    = 0x80;                       // bit 0 = 1, rest = 0
    trks[6646] = 1;                          // bytes_used LE u16 = 1
    trks[6648] = 32; trks[6649] = 0;         // bit_count LE u16 = 32
    trks[6650] = 0xFF; trks[6651] = 0xFF;    // splice_point = none
    addChunk("TRKS", trks);
    const std::string path = writeTempFile(woz, "v1_default");

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: WOZ1 load: %s\n", img.getLastError().c_str());
        return false;
    }
    // WOZ1 → optimalBitTiming stays at the constructor default (32) →
    // cyc = 8 → period = 32*8 = 256, flux[0] = 4.
    const int actualPeriod = img.trackPeriod(0);
    if (actualPeriod != 32 * 8) {
        std::printf("FAIL: WOZ1 trackPeriod=%d expected=%d\n",
                    actualPeriod, 32 * 8);
        return false;
    }
    const auto& flux = img.fluxEvents(0);
    if (flux.size() != 1 || flux[0] != 4) {
        std::printf("FAIL: WOZ1 flux size=%zu flux[0]=%d expected (1, 4)\n",
                    flux.size(), flux.empty() ? -1 : flux[0]);
        return false;
    }
    std::printf("OK : WOZ1 default → period=%d flux[0]=%d\n",
                actualPeriod, flux[0]);
    return true;
}

// ── FLUX-chunk revolution period ────────────────────────────────────────
//
// A WOZ 2.1 FLUX track's revolution period is the SUM of its tick
// deltas (1 tick = 125 ns; MAME `as_dsk.cpp:61-81` accumulates the same
// stream) — it is NOT a bit-count × optimal_bit_timing product.
// Regression: loadFluxTrack sized a synthetic bit stream at a hard-coded
// 8 LSS cycles/cell while trackPeriod multiplied the bit length by
// optimal_bit_timing/4, so for obt ≠ 32 the reported period was scaled
// by obt/32 (and even at obt = 32 it was ceil-rounded up to a cell
// boundary) — the flux timeline slipped against the angular wrap every
// revolution. trackPeriod must return the true tick-sum period for
// flux-loaded quarter-tracks.
bool checkFluxTrackPeriod(uint8_t obt) {
    // Layout (all offsets fixed):
    //   0     magic + CRC                (12 B)
    //   12    INFO chunk  (8 + 60)       → 80
    //   80    TMAP chunk  (8 + 160)      → 248  (all $FF — no bit tracks)
    //   248   TRKS chunk  (8 + 1280)     → 1536 (header table only)
    //   1536  FLUX chunk at block 3 (8 + 160 FIDX) → 1864
    //   2048  flux delta stream at block 4
    const std::vector<uint8_t> deltas = { 100, 100, 0xFF, 55, 100, 45 };
    // totalTicks = 655 → periodLss = ceil(655/4) = 164.
    // Events at cumulative ticks 100, 200, 510, 610 → LSS 25, 50, 127, 152
    // (0xFF = no-flux continuation; the final byte never emits).
    const int expectedPeriod = 164;
    const std::vector<int> expectedFlux = { 25, 50, 127, 152 };

    std::vector<uint8_t> woz;
    woz.insert(woz.end(), {'W', 'O', 'Z', '2', 0xFF, 0x0A, 0x0D, 0x0A});
    putU32LE(woz, 0);                       // CRC32 (unchecked)
    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };

    std::vector<uint8_t> info(60, 0);
    info[0]  = 3;                           // info_version 3 (v2.1+: FLUX)
    info[1]  = 1;                           // disk_type 5.25"
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    info[37] = 1;                           // disk_sides
    info[39] = obt;                         // optimal_bit_timing
    info[46] = 3; info[47] = 0;             // flux_block = 3
    info[48] = 1; info[49] = 0;             // largest_flux_track ≠ 0
    addChunk("INFO", info);

    std::vector<uint8_t> tmap(160, 0xFF);   // no bit-cell tracks
    addChunk("TMAP", tmap);

    std::vector<uint8_t> trks(1280, 0);     // TRK header table only
    // fidx 0 header: starting_block = 4, block_count = 1, track_size =
    // delta byte count.
    trks[0] = 4; trks[1] = 0;
    trks[2] = 1; trks[3] = 0;
    trks[4] = static_cast<uint8_t>(deltas.size());
    addChunk("TRKS", trks);
    assert(woz.size() == 1536 && "FLUX chunk must start at block 3");

    std::vector<uint8_t> fidx(160, 0xFF);
    fidx[0] = 0;                            // qt 0 → TRK header 0
    addChunk("FLUX", fidx);

    while (woz.size() < 2048) woz.push_back(0);   // pad to block 4
    woz.insert(woz.end(), deltas.begin(), deltas.end());

    const std::string path = writeTempFile(
        woz, (std::string("flux_") + std::to_string(obt)).c_str());

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: FLUX load obt=%u: %s\n",
                    static_cast<unsigned>(obt), img.getLastError().c_str());
        return false;
    }
    // Writable FLUX tracks can now be encoded back into the WOZ container.
    // The separate write-back test covers persistence; this timing test also
    // guards against mistakenly marking them protected on load.
    img.setWriteBackEnabled(true);
    if (img.isWriteProtected()) {
        std::printf("FAIL: writable FLUX image loaded as protected\n");
        return false;
    }
    const int period = img.trackPeriod(0);
    if (period != expectedPeriod) {
        std::printf("FAIL: FLUX obt=%u trackPeriod=%d expected=%d "
                    "(tick-sum), the obt-scaled synthetic-cell product "
                    "desyncs every revolution\n",
                    static_cast<unsigned>(obt), period, expectedPeriod);
        return false;
    }
    const auto& flux = img.fluxEvents(0);
    if (flux.size() != expectedFlux.size() ||
        !std::equal(flux.begin(), flux.end(), expectedFlux.begin())) {
        std::printf("FAIL: FLUX obt=%u event list mismatch (%zu events)\n",
                    static_cast<unsigned>(obt), flux.size());
        return false;
    }
    // Wrap math runs through the same period: past the last event the
    // next transition is the first event of the NEXT revolution.
    const int64_t wrapped = img.getNextTransition(0, 153);
    if (wrapped != expectedPeriod + expectedFlux.front()) {
        std::printf("FAIL: FLUX obt=%u wrap → %lld, expected %d\n",
                    static_cast<unsigned>(obt),
                    static_cast<long long>(wrapped),
                    expectedPeriod + expectedFlux.front());
        return false;
    }
    std::printf("OK : FLUX obt=%u → period=%d (tick-sum), %zu events, "
                "wrap=%lld\n",
                static_cast<unsigned>(obt), period, flux.size(),
                static_cast<long long>(wrapped));
    return true;
}

bool checkHostileFluxExpansionRejected()
{
    // 40 KiB of maximum deltas describes >2.5 million synthetic cells at
    // obt=8. The source is tiny, but the old loader allocated from the delta
    // sum and could multiply it through 160 duplicate FIDX entries.
    const std::vector<uint8_t> deltas(40u * 1024u, 0xFF);
    std::vector<uint8_t> woz;
    woz.insert(woz.end(), {'W','O','Z','2',0xFF,0x0A,0x0D,0x0A});
    putU32LE(woz, 0);
    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };
    std::vector<uint8_t> info(60, 0);
    info[0] = 3; info[1] = 1; info[37] = 1; info[39] = 8;
    info[46] = 3; info[48] = 1;
    addChunk("INFO", info);
    addChunk("TMAP", std::vector<uint8_t>(160, 0xFF));
    std::vector<uint8_t> trks(1280, 0);
    trks[0] = 4; trks[2] = 80; // enough blocks; exact byte length is below
    const uint32_t n = static_cast<uint32_t>(deltas.size());
    trks[4] = static_cast<uint8_t>(n);
    trks[5] = static_cast<uint8_t>(n >> 8);
    trks[6] = static_cast<uint8_t>(n >> 16);
    trks[7] = static_cast<uint8_t>(n >> 24);
    addChunk("TRKS", trks);
    assert(woz.size() == 1536);
    std::vector<uint8_t> fidx(160, 0); // repeat the hostile track 160 times
    addChunk("FLUX", fidx);
    while (woz.size() < 2048) woz.push_back(0);
    woz.insert(woz.end(), deltas.begin(), deltas.end());

    const std::string path = writeTempFile(woz, "hostile_flux");
    DiskImage img;
    if (img.loadFile(path)) {
        std::printf("FAIL: hostile FLUX expansion was accepted\n");
        return false;
    }
    std::printf("OK : hostile FLUX expansion rejected before allocation\n");
    return true;
}

}  // namespace

int main() {
    bool allOk = true;
    allOk &= checkWoz1FallsBackTo32();   // baseline: no INFO+39 → 32
    allOk &= checkTimingForObt(32);      // explicit default
    allOk &= checkTimingForObt(40);      // 5 µs cells (slower master)
    allOk &= checkTimingForObt(28);      // 3.5 µs cells (faster master)
    allOk &= checkFluxTrackPeriod(32);   // FLUX period = tick sum (default)
    allOk &= checkFluxTrackPeriod(40);   // FLUX period independent of obt
    allOk &= checkHostileFluxExpansionRejected();
    return allOk ? 0 : 1;
}
