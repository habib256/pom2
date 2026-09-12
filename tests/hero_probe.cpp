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

// Diagnostic probe for the H.E.R.O. .dsk hang.
//
// Loads H.E.R.O. (or any .dsk passed as the first arg), inspects the
// raw nibble buffer DiskImage produces at the track the CPU was stuck
// on, and walks the bit stream to verify D5 AA 96 is recoverable
// through the same path the LSS reads. Splits the problem into:
//   1. GCR encoder side — is D5 AA 96 actually present in tracks[].
//   2. Bit-cell stream side — is D5 AA 96 reachable via bitAt() with
//      proper alignment.
//   3. LSS side — emulate the LSS read on the bit stream and report
//      the byte sequence the CPU would see.

#include "DiskIICard.h"
#include "DiskImage.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

bool fileExists(const std::string& p)
{
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) if (fileExists(c)) return c;
    return {};
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string dskPath = argc > 1 ? argv[1] :
        findFirst({
            "disks_5.4/H.E.R.O. (1984)(Activision)[cr 4am][48K].dsk",
            "../disks_5.4/H.E.R.O. (1984)(Activision)[cr 4am][48K].dsk",
        });
    int probeTrack = argc > 2 ? std::atoi(argv[2]) : 2;

    if (dskPath.empty()) {
        std::fprintf(stderr, "missing disk image\n");
        return 1;
    }
    std::printf("disk = %s\n", dskPath.c_str());
    std::printf("probe track = %d\n", probeTrack);

    DiskImage img;
    if (!img.loadFile(dskPath)) {
        std::fprintf(stderr, "loadFile failed: %s\n",
                     img.getLastError().c_str());
        return 1;
    }
    if (!img.isLoaded()) {
        std::fprintf(stderr, "image not loaded\n");
        return 1;
    }

    // ── 1. Scan the nibble buffer for the D5 AA 96 prologue. ──────────
    // Use Memory's per-byte data via getNibble? DiskImage exposes
    // nibbleAt(track, idx).
    const int qt = probeTrack * 4;
    int matches = 0;
    int firstMatch = -1;
    for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i) {
        const int j = (i + 1) % DiskImage::kNibblesPerTrack;
        const int k = (i + 2) % DiskImage::kNibblesPerTrack;
        if (img.nibbleAt(probeTrack, i) == 0xD5
            && img.nibbleAt(probeTrack, j) == 0xAA
            && img.nibbleAt(probeTrack, k) == 0x96) {
            if (matches == 0) firstMatch = i;
            ++matches;
        }
    }
    std::printf("\n[1] Nibble buffer at track %d:\n", probeTrack);
    std::printf("    D5 AA 96 occurrences: %d (first at idx %d)\n",
                matches, firstMatch);

    // Print the bytes around the first match.
    if (firstMatch >= 0) {
        std::printf("    Around first match (idx %d):", firstMatch);
        for (int o = -3; o < 13; ++o) {
            const int idx = ((firstMatch + o) % DiskImage::kNibblesPerTrack
                             + DiskImage::kNibblesPerTrack)
                            % DiskImage::kNibblesPerTrack;
            std::printf(" %02X", img.nibbleAt(probeTrack, idx));
        }
        std::printf("\n");
    }

    // ── 2. Walk the bit stream and reconstruct GCR bytes via a
    //      MAME-style LSS-equivalent state machine.
    const int bitCount = img.trackBitLength(qt);
    std::printf("\n[2] Bit stream at qt %d: %d cells\n", qt, bitCount);

    // Simple LSS-equivalent: a sliding 8-bit shifter. When the high bit
    // is set, latch a byte; on the next cell, the byte stays exposed
    // (real LSS would do CLR on QA=1 then shift in). Approximation good
    // enough to count D5 AA 96 occurrences.
    uint8_t shifter = 0;
    int byteIdx = 0;
    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(bitCount) / 8 + 1);
    for (int i = 0; i < bitCount; ++i) {
        const uint8_t b = img.bitAt(qt, i);
        shifter = static_cast<uint8_t>((shifter << 1) | (b & 1));
        if (shifter & 0x80) {
            bytes.push_back(shifter);
            shifter = 0;
        }
        ++byteIdx;
    }
    int bitMatches = 0;
    int firstBitMatch = -1;
    for (size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] == 0xD5 && bytes[i+1] == 0xAA && bytes[i+2] == 0x96) {
            if (bitMatches == 0) firstBitMatch = static_cast<int>(i);
            ++bitMatches;
        }
    }
    std::printf("    Reconstructed bytes: %zu\n", bytes.size());
    std::printf("    D5 AA 96 occurrences (bit-stream walk): %d (first at byte %d)\n",
                bitMatches, firstBitMatch);
    if (firstBitMatch >= 0) {
        std::printf("    Around first bit-stream match:");
        const int start = std::max(0, firstBitMatch - 3);
        const int end   = std::min<int>(static_cast<int>(bytes.size()),
                                        firstBitMatch + 16);
        for (int j = start; j < end; ++j) {
            std::printf(" %02X", bytes[j]);
        }
        std::printf("\n");
    }

    // ── 3. Flux event view: how many events on this track?
    const auto& flux = img.fluxEvents(qt);
    const int period = img.trackPeriod(qt);
    std::printf("\n[3] Flux events at qt %d: %zu events over period %d LSS cycles\n",
                qt, flux.size(), period);
    if (!flux.empty()) {
        std::printf("    first 8 events:");
        for (size_t i = 0; i < 8 && i < flux.size(); ++i) {
            std::printf(" %d", flux[i]);
        }
        std::printf("\n    last event: %d\n", flux.back());
    }

    // ── 4. Now drive the actual controller LSS through 5 revolutions
    //      and record the bytes the CPU would see.
    DiskIICard card(6);
    const std::string p6Path = findFirst({
        "roms/diskii_p6.rom", "../roms/diskii_p6.rom" });
    if (!p6Path.empty()) card.loadLssRom(p6Path);
    if (!card.insertDisk(dskPath)) {
        std::fprintf(stderr, "insertDisk failed\n");
        return 1;
    }

    // Walk head to probeTrack via phase pulses.
    card.deviceSelectRead(0x09);   // motor on
    auto step = [&](int phase) {
        card.deviceSelectRead(static_cast<uint8_t>(phase * 2 + 1));
        card.deviceSelectRead(static_cast<uint8_t>(phase * 2 + 0));
    };
    // From qt 0, step `probeTrack * 2` times via phase 0→1 pulses.
    for (int i = 0; i < probeTrack * 2; ++i) {
        step((i + 1) % 4);
    }
    std::printf("\n[4] After seek to qt %d: card qt=%d\n",
                qt, card.getQuarterTrack(0));

    card.deviceSelectRead(0x0E);   // Q7L
    card.deviceSelectRead(0x0C);   // Q6L
    // Sample with realistic timing — 32 CPU cycles between reads
    // (matches a tight LDA $C0EC / BPL loop at byte cadence).
    std::vector<uint8_t> sampled;
    sampled.reserve(800);
    for (int i = 0; i < 800; ++i) {
        card.advanceCycles(8);                  // ~ time for 1 bit cell
        const uint8_t b = card.deviceSelectRead(0xC);
        if (b & 0x80) {
            if (sampled.empty() || sampled.back() != b) sampled.push_back(b);
        }
    }
    int lssMatches = 0;
    int firstLssMatch = -1;
    for (size_t i = 0; i + 2 < sampled.size(); ++i) {
        if (sampled[i] == 0xD5 && sampled[i+1] == 0xAA && sampled[i+2] == 0x96) {
            if (lssMatches == 0) firstLssMatch = static_cast<int>(i);
            ++lssMatches;
        }
    }
    std::printf("    LSS-read bytes (deduped consecutive): %zu\n",
                sampled.size());
    std::printf("    D5 AA 96 occurrences: %d (first at sample %d)\n",
                lssMatches, firstLssMatch);
    std::printf("    First 32 bytes:");
    for (int i = 0; i < 32 && i < static_cast<int>(sampled.size()); ++i) {
        std::printf(" %02X", sampled[i]);
    }
    std::printf("\n");

    return 0;
}
