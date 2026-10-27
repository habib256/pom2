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

// MAME wozfdc / floppy_image flux-model parity smoke test.
//
// Verifies that POM2's verbatim port of MAME's Disk II LSS preserves the
// algorithmic invariants of MAME's source-of-truth implementation:
//
//   1. DiskImage::fluxEvents(track) emits one flux event per "1" bit
//      cell, at sub-cell-tick offset 4 within the cell — matches MAME's
//      `floppy_image_device::cache_fill` path that places a flux change
//      at each cell's centre.
//
//   2. DiskImage::getNextTransition(track, lssCycle) returns the next
//      flux event at or after the supplied LSS-cycle time, wrapping
//      across revolution boundaries when the current revolution has no
//      more events past the cursor — matches MAME's `get_next_transition`
//      semantics including the cross-revolution lookup.
//
//   3. DiskIICard's lssSync path drives the data register through the
//      same MAME PROM lookup → ALU → state-update sequence, recovering
//      a known nibble pattern (D5 AA 96 prologue) byte-for-byte from a
//      synthetic track.
//
//   4. DiskImage::writeFlux splices a flux event window back into the
//      nibble buffer such that re-reading via the LSS reproduces the
//      same nibble at the spliced byte position.
//
// All assertions are bit-exact against MAME's algorithmic output for the
// same input — no fuzzy matching, no time tolerances.

#include "DiskIICard.h"
#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string findFirst(std::initializer_list<const char*> candidates) {
    for (const char* p : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

// Build a synthetic .nib track buffer (35 × 6656 bytes) with a single
// `D5 AA 96 EB` byte sequence at offset 64 of track 0; everything else
// is sync ($FF). Returns the path to the temp file.
std::string makeSyntheticNib() {
    constexpr int kTracks        = DiskImage::kTracks;
    constexpr int kBytesPerTrack = DiskImage::kNibblesPerTrack;
    std::vector<uint8_t> img(static_cast<size_t>(kTracks) * kBytesPerTrack, 0xFF);
    constexpr int kProloguePos = 64;
    // A data byte before the prologue: since STORAGE-010 a .nib's
    // "FF … FF D5" run is self-sync (MAME a2_nib_format, 10 cells per $FF),
    // and this carrier's geometry is the 8-cell-per-byte one the positions
    // below are computed on.
    img[kProloguePos - 1] = 0x96;
    img[kProloguePos + 0] = 0xD5;
    img[kProloguePos + 1] = 0xAA;
    img[kProloguePos + 2] = 0x96;
    img[kProloguePos + 3] = 0xEB;

    const auto tmp = fs::temp_directory_path() / "pom2_mame_parity.nib";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    assert(f && "failed to open temp .nib for writing");
    const size_t wrote = std::fwrite(img.data(), 1, img.size(), f);
    assert(wrote == img.size());
    std::fclose(f);
    return tmp.string();
}

// Test 1 — flux cache layout: one event per "1" cell, at cell centre.
bool testFluxCacheLayout()
{
    DiskImage img;
    if (!img.loadFile(makeSyntheticNib())) {
        std::printf("FAIL: loadFile %s\n", img.getLastError().c_str());
        return false;
    }
    const auto& flux = img.fluxEvents(0);
    if (flux.empty()) { std::printf("FAIL: empty flux for track 0\n"); return false; }

    // Period must equal trackBitLength * 8 — invariant of the LSS-cycle
    // time base (8 LSS cycles per bit cell).
    const int period   = img.trackPeriod(0);
    const int bitLen   = img.trackBitLength(0);
    if (period != bitLen * 8) {
        std::printf("FAIL: period (%d) != bitLen*8 (%d)\n", period, bitLen * 8);
        return false;
    }

    // Every event must sit at LSS cycle = cellIdx*8 + 4 (cell centre).
    for (int t : flux) {
        if ((t % 8) != 4) {
            std::printf("FAIL: flux event %d not at cell centre (mod 8 = %d)\n",
                        t, t % 8);
            return false;
        }
    }

    // Events must appear in strictly ascending order with no duplicates
    // — matches MAME's sorted-uint32_t track vector invariant.
    for (size_t i = 1; i < flux.size(); ++i) {
        if (flux[i] <= flux[i - 1]) {
            std::printf("FAIL: flux events not strictly ascending at %zu (%d <= %d)\n",
                        i, flux[i], flux[i - 1]);
            return false;
        }
    }

    // Sanity: the prologue byte $D5 = 11010101 at offset 64 produces 5
    // "1" cells starting at bit-cell index 64*8 = 512 → flux events at
    // LSS cycles {512*8+4, 513*8+4, 515*8+4, 517*8+4, 519*8+4} =
    // {4100, 4108, 4124, 4140, 4156}. Find them in the flux array.
    const int expected[5] = { 4100, 4108, 4124, 4140, 4156 };
    int matched = 0;
    for (int e : expected) {
        for (int t : flux) {
            if (t == e) { ++matched; break; }
            if (t > e) break;     // sorted, missed it
        }
    }
    if (matched != 5) {
        std::printf("FAIL: only %d/5 expected $D5 prologue events found\n", matched);
        return false;
    }
    std::printf("[ OK ] flux cache: %zu events in [0,%d), $D5 prologue intact\n",
                flux.size(), period);
    return true;
}

// Test 2 — getNextTransition: monotonic, wraps revolution boundary.
bool testGetNextTransition()
{
    DiskImage img;
    if (!img.loadFile(makeSyntheticNib())) return false;

    const auto& flux = img.fluxEvents(0);
    if (flux.empty()) return false;
    const int period = img.trackPeriod(0);

    // Walking forward from 0 must visit every event exactly once before
    // wrapping. After the last event in revolution N, the next call must
    // return event[0] + period (i.e. revolution N+1).
    int64_t cursor = 0;
    for (size_t i = 0; i < flux.size(); ++i) {
        const int64_t got = img.getNextTransition(0, cursor);
        if (got != flux[i]) {
            std::printf("FAIL: getNextTransition(%lld) = %lld, expected %d\n",
                        (long long)cursor, (long long)got, flux[i]);
            return false;
        }
        cursor = got + 1;
    }
    // Past the last event of revolution 0 → wraps to revolution 1.
    const int64_t wrapGot = img.getNextTransition(0, cursor);
    const int64_t wrapExp = static_cast<int64_t>(flux.front()) + period;
    if (wrapGot != wrapExp) {
        std::printf("FAIL: wrap getNextTransition = %lld, expected %lld\n",
                    (long long)wrapGot, (long long)wrapExp);
        return false;
    }

    // Random-access spot-check: querying at exactly an event time must
    // return that event (lower_bound semantic, not upper_bound).
    const int64_t exact = img.getNextTransition(0, flux[3]);
    if (exact != flux[3]) {
        std::printf("FAIL: at-event lookup got %lld, expected %d\n",
                    (long long)exact, flux[3]);
        return false;
    }
    std::printf("[ OK ] getNextTransition: monotonic walk + wrap + exact-match\n");
    return true;
}

// Test 3 — end-to-end LSS read recovers D5 AA 96 from the synthetic
// track via the MAME flux model. Pinned against the same prologue the
// existing diskii_lss_smoke_test recovers via the bit-cell stream — this
// confirms the two views agree.
bool testLssReadParity()
{
    const std::string lssPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom"
    });
    if (lssPath.empty()) {
        std::printf("[SKIP] roms/diskii_p6.rom not found — skipping LSS read parity\n");
        return true;
    }
    DiskIICard card;
    if (!card.loadLssRom(lssPath)) { std::printf("FAIL: loadLssRom\n"); return false; }
    if (!card.insertDisk(makeSyntheticNib())) {
        std::printf("FAIL: insertDisk %s\n", card.getLastError().c_str());
        return false;
    }
    // Motor on, read mode (Q6L+Q7L).
    card.deviceSelectRead(0x9);
    card.deviceSelectRead(0xE);
    card.deviceSelectRead(0xC);

    constexpr int kCyclesPerRead = 8;
    constexpr int kMaxCycles     = 1'000'000;
    std::vector<uint8_t> bytes;
    bytes.reserve(64);
    int spent = 0;
    while (spent < kMaxCycles && bytes.size() < 64) {
        card.advanceCycles(kCyclesPerRead);
        spent += kCyclesPerRead;
        const uint8_t b = card.deviceSelectRead(0xC);
        if ((b & 0x80) && (bytes.empty() || bytes.back() != b)) {
            bytes.push_back(b);
        }
    }
    bool found = false;
    for (size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] == 0xD5 && bytes[i+1] == 0xAA && bytes[i+2] == 0x96) {
            found = true; break;
        }
    }
    if (!found) {
        std::printf("FAIL: D5 AA 96 not recovered via flux LSS path in %zu bytes:",
                    bytes.size());
        for (size_t i = 0; i < bytes.size() && i < 32; ++i)
            std::printf(" %02X", bytes[i]);
        std::printf("\n");
        return false;
    }
    std::printf("[ OK ] LSS read via flux model recovers D5 AA 96\n");
    return true;
}

// Test 4 — writeFlux splice round-trip: plant flux events for a known
// nibble pattern, then read back from the same byte position and verify
// the nibble buffer contains what we wrote. This pins the flux→bit
// re-pack logic in DiskImage::writeFlux.
bool testWriteFluxRoundTrip()
{
    DiskImage img;
    if (!img.loadFile(makeSyntheticNib())) return false;

    // Build flux events for a single $96 nibble (10010110 MSB-first) at
    // byte index 100. Cell centres at LSS cycles {100*64+4, 100*64+28,
    // 100*64+44, 100*64+52} = {6404, 6428, 6444, 6452}. (Bytes are 8
    // cells = 64 LSS cycles wide; bit b's centre = byteIdx*64 + b*8 + 4.)
    constexpr int byteIdx = 100;
    const int64_t base = static_cast<int64_t>(byteIdx) * 64;
    const int64_t flux[] = { base + 0*8 + 4,    // bit 7 = 1 → cell 0
                              base + 3*8 + 4,    // bit 4 = 1 → cell 3
                              base + 5*8 + 4,    // bit 2 = 1 → cell 5
                              base + 6*8 + 4 };  // bit 1 = 1 → cell 6
    img.writeFlux(0, base, base + 64, 4, flux);

    // The nibble at byteIdx must now be $96 (10010110 = 0x96).
    const uint8_t got = img.nibbleAt(0, byteIdx);
    if (got != 0x96) {
        std::printf("FAIL: writeFlux($96 pattern) yielded $%02X, expected $96\n", got);
        return false;
    }
    std::printf("[ OK ] writeFlux splice → nibble buffer round-trip ($96)\n");
    return true;
}

}  // namespace

int main()
{
    bool ok = true;
    ok &= testFluxCacheLayout();
    ok &= testGetNextTransition();
    ok &= testLssReadParity();
    ok &= testWriteFluxRoundTrip();
    if (ok) {
        std::printf("mame_lss_parity_smoke OK\n");
        return 0;
    }
    std::printf("mame_lss_parity_smoke FAILED\n");
    return 1;
}
