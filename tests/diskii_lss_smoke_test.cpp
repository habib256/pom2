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

// Disk II Logic State Sequencer (LSS) smoke test.
//
// Pins the bit-level LSS port (DiskIICard + DiskImage::bitAt) against
// the apple2js reference implementation we transcribed from. Specifically:
//
//   1. The bundled `roms/diskii_p6.rom` loads (if present) and is 256 B.
//   2. DiskImage::trackBitLength(0) wraps a stock 6656-byte nibble track
//      to a non-trivial bit-cell count > 50 000 (verifies sync padding).
//   3. End-to-end: feed the LSS a track containing a single $D5 $AA $96
//      address-mark prologue, drive it for one revolution, and confirm
//      it surfaces those three bytes in order at $C0EC.
//   4. With no P6 PROM loaded, the legacy 32-cycle gate produces the
//      same three bytes (regression guard for the fallback path).
//
// The test deliberately avoids loading a full Apple II ROM — it pokes
// the controller's switches directly to spin the motor, sit at track 0,
// and read $C0EC bytes back. Keeps the test fast and isolated from any
// machine-mode (II+ / IIe) wiring.

#include "DiskIICard.h"
#include "DiskImage.h"
#include "M6502.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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
// `D5 AA 96` prologue at a known position, surrounded by sync-FF runs.
// All other tracks are filled with $FF (= sync). Returns the path to
// the temporary file that DiskImage can ingest.
std::string makeSyntheticNib() {
    constexpr int kTracks = DiskImage::kTracks;
    constexpr int kBytesPerTrack = DiskImage::kNibblesPerTrack;
    std::vector<uint8_t> img(static_cast<size_t>(kTracks) * kBytesPerTrack, 0xFF);
    // Drop the prologue at offset 64 of track 0 — far enough past the
    // start that any LSS startup transient has settled by the time the
    // head reaches it.
    constexpr int kProloguePos = 64;
    img[kProloguePos + 0] = 0xD5;
    img[kProloguePos + 1] = 0xAA;
    img[kProloguePos + 2] = 0x96;
    // Add a follow-on byte so the test can verify the stream advances
    // past the prologue too (no premature wrap on the third byte).
    img[kProloguePos + 3] = 0xEB;

    const auto tmp = fs::temp_directory_path() / "pom2_lss_synthetic.nib";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    assert(f && "failed to open temp .nib for writing");
    const size_t wrote = std::fwrite(img.data(), 1, img.size(), f);
    assert(wrote == img.size());
    std::fclose(f);
    return tmp.string();
}

// Drive the controller for `cpuCycles` worth of "spin" — calls the
// soft-switch read at $C0EC repeatedly with `kCyclesPerRead` advance
// between each, collecting bytes that have bit-7 set (= valid GCR
// nibble landed in the data register). Stops after `maxBytes` valid
// reads OR `cpuCycles` exhausted.
//
// This exactly mirrors the spin-loop DOS / ProDOS / Copy II+ all use:
//   `LDA $C0EC ; BPL loop`. Each LDA-BPL pair is ~7 cycles in real
// code; we space them at 8 to give the LSS room to walk through one
// bit cell between samples.
std::vector<uint8_t> spinAndCollect(DiskIICard& card,
                                    int cpuCycles,
                                    size_t maxBytes,
                                    int cyclesPerRead = 8)
{
    std::vector<uint8_t> out;
    out.reserve(maxBytes);

    // Make sure motor's running and we're in read mode (Q6L+Q7L).
    card.deviceSelectRead(0x9);   // motor on
    card.deviceSelectRead(0xE);   // Q7L
    card.deviceSelectRead(0xC);   // Q6L

    int spent = 0;
    while (spent < cpuCycles && out.size() < maxBytes) {
        card.advanceCycles(cyclesPerRead);
        spent += cyclesPerRead;
        const uint8_t b = card.deviceSelectRead(0xC);
        if (b & 0x80) {
            // Avoid spamming duplicates: only record when the latch
            // changed since the last sample. A real LDA / BPL spin sees
            // the same nibble on consecutive reads until the LSS shifts
            // a new one in; we capture the same de-duped sequence here.
            if (out.empty() || out.back() != b) out.push_back(b);
        }
    }
    return out;
}

// Test 1: Bundled P6 ROM file integrity check (skipped if missing).
bool testRomLoad() {
    const std::string lssPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom"
    });
    if (lssPath.empty()) {
        std::printf("[SKIP] roms/diskii_p6.rom not found — skipping ROM test\n");
        return true;
    }
    DiskIICard card;
    const bool ok = card.loadLssRom(lssPath);
    if (!ok) { std::printf("FAIL: loadLssRom returned false\n"); return false; }
    if (!card.hasLssRom()) { std::printf("FAIL: hasLssRom false\n"); return false; }
    std::printf("[ OK ] P6 ROM loads from %s\n", lssPath.c_str());
    return true;
}

// Test 2: bit-stream length is sane.
bool testBitStreamLength() {
    DiskImage img;
    const std::string nibPath = makeSyntheticNib();
    if (!img.loadFile(nibPath)) {
        std::printf("FAIL: loadFile %s: %s\n", nibPath.c_str(),
                    img.getLastError().c_str());
        return false;
    }
    const int len = img.trackBitLength(0);
    // .nib uses no sync padding (no semantic FF runs) → exactly 8 cells
    // per byte = 6656 × 8 = 53248. .dsk would be larger because of FF
    // sync padding.
    if (len != DiskImage::kNibblesPerTrack * 8) {
        std::printf("FAIL: trackBitLength(0) = %d, expected %d\n",
                    len, DiskImage::kNibblesPerTrack * 8);
        return false;
    }
    // Sanity: bitAt should return only 0/1.
    for (int i = 0; i < 64; ++i) {
        const uint8_t b = img.bitAt(0, i);
        if (b > 1) { std::printf("FAIL: bitAt %d = %d\n", i, b); return false; }
    }
    // Sanity: the prologue at byte 64 must produce 0xD5 = 11010101 in
    // the bit stream starting at bit 64*8 = 512.
    const int p = 64 * 8;
    const uint8_t expected[8] = {1,1,0,1,0,1,0,1};   // 0xD5 MSB-first
    for (int i = 0; i < 8; ++i) {
        if (img.bitAt(0, p + i) != expected[i]) {
            std::printf("FAIL: bitAt(0, %d) = %d, expected %d\n",
                        p + i, img.bitAt(0, p + i), expected[i]);
            return false;
        }
    }
    std::printf("[ OK ] bit-stream length = %d, prologue bits MSB-first\n", len);
    return true;
}

// Test 3: end-to-end LSS recovers D5 AA 96 from the synthetic track.
bool testAddressMarkRecovery() {
    const std::string lssPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom"
    });
    if (lssPath.empty()) {
        std::printf("[SKIP] roms/diskii_p6.rom not found — skipping LSS test\n");
        return true;
    }
    DiskIICard card;
    if (!card.loadLssRom(lssPath)) {
        std::printf("FAIL: loadLssRom\n"); return false;
    }
    const std::string nibPath = makeSyntheticNib();
    if (!card.insertDisk(nibPath)) {
        std::printf("FAIL: insertDisk: %s\n", card.getLastError().c_str());
        return false;
    }

    // Spin for up to ~5 revolutions (each rev ≈ 200 ms ≈ 200K CPU cyc).
    const auto bytes = spinAndCollect(card, 1'000'000, 64);

    // Find D5 AA 96 anywhere in the recovered stream.
    bool found = false;
    for (size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] == 0xD5 && bytes[i+1] == 0xAA && bytes[i+2] == 0x96) {
            found = true;
            std::printf("[ OK ] LSS recovered D5 AA 96 at offset %zu of %zu collected\n",
                        i, bytes.size());
            break;
        }
    }
    if (!found) {
        std::printf("FAIL: D5 AA 96 not found in %zu collected bytes:",
                    bytes.size());
        for (size_t i = 0; i < bytes.size() && i < 64; ++i)
            std::printf(" %02X", bytes[i]);
        std::printf("\n");
        return false;
    }
    return true;
}

// Test 4: LSS write path. Drive Q7H + Q6H stores at 32-CPU-cycle
// pacing (matches DOS 3.3 RWTS WRITE6 cadence) and assert the LSS
// shifter assembles complete nibbles into the track buffer at the
// ANGULAR position the revolution anchor predicts. Mirrors
// disk_write_controller_smoke_test but goes through the bit-level LSS
// instead of the legacy 32-cycle gate.
//
// The positional assertion pins the MAME write_flux ↔ find_position
// anchor parity (imagedev/floppy.cpp ~:1050-1125): the motor spins up
// at a non-zero lssCycle, so the drive's revolution anchor is non-zero,
// and the written bits must land at `(writeStart - revStart) mod
// period` — NOT at `writeStart mod period`, which is one full anchor
// offset away.
bool testLssWrite() {
    const std::string lssPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom"
    });
    if (lssPath.empty()) {
        std::printf("[SKIP] roms/diskii_p6.rom not found — skipping LSS write\n");
        return true;
    }
    DiskIICard card;
    if (!card.loadLssRom(lssPath)) { std::printf("FAIL: loadLssRom\n"); return false; }
    const std::string nibPath = makeSyntheticNib();
    if (!card.insertDisk(nibPath)) {
        std::printf("FAIL: insertDisk: %s\n", card.getLastError().c_str());
        return false;
    }
    // Write-back ON so the LSS splices land in the image and eject
    // persists them to the temp .nib (which makeSyntheticNib regenerates
    // for every test, so this can't leak into the other cases).
    card.setWriteBackEnabled(true);

    // Run the controller for a while with the motor OFF, so the
    // spin-up below anchors the revolution at a NON-zero lssCycle —
    // the unanchored-write regression is invisible at revStart = 0.
    card.advanceCycles(1024);     // cpuCycleTotal = 1024 → revStart = 2048
    card.deviceSelectRead(0x9);   // motor on (lssStart: revStart = 2048)
    card.deviceSelectRead(0xE);   // Q7L
    card.deviceSelectRead(0xC);   // Q6L (read shift)
    card.advanceCycles(1024);     // let the LSS run; lssCycle → 4096

    // Switch to write mode. CPU loads byte via $C0ED store (Q6H+Q7H),
    // then waits ~32 cycles for the LSS to shift it out, then loads the
    // next byte. This is exactly DOS 3.3's WRITE6 inner loop.
    const uint8_t pattern[] = { 0xFF, 0xFF, 0xD5, 0xAA, 0xAD, 0x96, 0xEB, 0xFF };
    card.deviceSelectRead(0xF);   // Q7H (write enable; writeStart = 4096)

    const uint64_t flushesBefore = card.getWriteFlushCount();
    for (uint8_t b : pattern) {
        // RWTS WRITE cadence: STA $C0ED pulses Q6H to latch the byte,
        // ORA/LDA $C0EC drops back to Q6L so the LSS shifts it out.
        // (Holding Q6H continuously makes the LSS re-load every cycle
        // and stream solid 1-bits — not a byte write.)
        card.deviceSelectWrite(0xD, b);   // Q6H + latch
        card.advanceCycles(4);            // LSS runs in load mode briefly
        card.deviceSelectRead(0xC);       // Q6L — shift out
        card.advanceCycles(28);           // rest of the 32-cycle window
    }
    card.deviceSelectRead(0xE);   // Q7L — flushes the splice (anchored)
    const uint64_t flushesAfter = card.getWriteFlushCount();
    if (flushesAfter == flushesBefore) {
        std::printf("FAIL: LSS write produced no writeFlux flush\n");
        return false;
    }

    // Verify the controller still reads valid GCR nibbles afterwards
    // (= LSS state recovers cleanly from the write→read transition).
    card.deviceSelectRead(0xC);   // Q6L
    const auto bytes = spinAndCollect(card, 500'000, 32);
    bool foundD5 = false;
    for (const auto b : bytes) {
        if (b == 0xD5) { foundD5 = true; break; }
    }
    if (!foundD5) {
        std::printf("FAIL: LSS write→read transition lost sync (no D5 in %zu bytes)\n",
                    bytes.size());
        return false;
    }

    // POSITIONAL pin. Persist via eject (save-on-eject rewrites the
    // .nib), then locate where the splice landed in track 0.
    //
    // Expected angle: writeStart = lssCycle at Q7H = 2×(1024+1024) =
    // 4096; revStart = 2048 (motor-on at cpuCycleTotal = 1024). A .nib
    // expands to exactly 8 cells/byte, so the splice's first cell is
    // (4096 - 2048) / 8 = bit 256 → nibble 32. The exact byte values
    // depend on sub-cell write phase (the LSS may rotate them against
    // the nibble grid), so the pin is the FIRST nibble that differs
    // from the original image: the leading two $FF pattern bytes are
    // no-ops over the $FF background, so the first visible change is a
    // few nibbles past 32 — far from the unanchored bug's landing spot
    // (4096 / 8 / 8 = nibble 64, right on top of the prologue).
    card.ejectDisk(0);
    std::ifstream f(nibPath, std::ios::binary);
    std::vector<uint8_t> track0(DiskImage::kNibblesPerTrack);
    f.read(reinterpret_cast<char*>(track0.data()),
           static_cast<std::streamsize>(track0.size()));
    if (!f) { std::printf("FAIL: re-read %s\n", nibPath.c_str()); return false; }

    auto originalNib = [](int i) -> uint8_t {
        // makeSyntheticNib: all $FF except D5 AA 96 EB at 64..67.
        switch (i) {
            case 64: return 0xD5;
            case 65: return 0xAA;
            case 66: return 0x96;
            case 67: return 0xEB;
            default: return 0xFF;
        }
    };
    int firstDiff = -1;
    for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i) {
        if (track0[i] != originalNib(i)) { firstDiff = i; break; }
    }
    if (firstDiff < 0) {
        std::printf("FAIL: LSS write changed nothing in track 0\n");
        return false;
    }
    const int expected = 32;                 // splice start nibble
    if (firstDiff < expected || firstDiff > expected + 12) {
        std::printf("FAIL: write landed at nibble %d, expected ~%d "
                    "(unanchored `writeStart %% period` lands at ~64) — "
                    "revolution anchor not applied to writeFlux\n",
                    firstDiff, expected);
        return false;
    }
    std::printf("[ OK ] LSS write path: anchored splice first-diff at "
                "nibble %d (expected ~%d), read recovers afterwards\n",
                firstDiff, expected);
    return true;
}

// Test 5: legacy gate (no P6 ROM) recovers the same prologue. This is
// the regression guard — even if a user removes `roms/diskii_p6.rom`,
// stock DOS / ProDOS reads must keep working via the 32-cycle gate.
bool testLegacyFallback() {
    DiskIICard card;     // no loadLssRom call → useBitLss stays false
    const std::string nibPath = makeSyntheticNib();
    if (!card.insertDisk(nibPath)) {
        std::printf("FAIL: insertDisk: %s\n", card.getLastError().c_str());
        return false;
    }

    // Legacy gate: 32 CPU cycles per nibble. Spin for one revolution
    // (~6656 nibbles × 32 cyc = ~213K). 64 bytes is plenty.
    const auto bytes = spinAndCollect(card, 250'000, 64);
    bool found = false;
    for (size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] == 0xD5 && bytes[i+1] == 0xAA && bytes[i+2] == 0x96) {
            found = true; break;
        }
    }
    if (!found) {
        std::printf("FAIL: legacy gate did not recover D5 AA 96 in %zu bytes:",
                    bytes.size());
        for (size_t i = 0; i < bytes.size() && i < 64; ++i)
            std::printf(" %02X", bytes[i]);
        std::printf("\n");
        return false;
    }
    std::printf("[ OK ] legacy gate also recovers D5 AA 96\n");
    return true;
}

// Test: SubInstructionScope guard is no-op when cpu->cycles == 0.
//
// `DiskIICard::setCpu(M6502*)` enables sub-instruction cycle accuracy
// for MMIO accesses: during deviceSelectRead/Write, cpuCycleTotal is
// temporarily inflated by `cpu->getCurrentInstructionCycles()` so the
// LSS catches up to the precise sub-cycle of the access (mirroring
// MAME's per-cycle state machine in `m6502.cpp`).
//
// We can't drive M6502::cycles to a non-zero value from outside (no
// public setter — it's owned by the per-instruction dispatcher). What
// we CAN pin: with a default-constructed M6502 (cycles=0), wiring up
// `setCpu(&cpu)` must be a no-op vs the unwired path. If the
// SubInstructionScope ever starts adding cycles when `cpu->cycles` is
// 0 — e.g. through a refactor that mistakenly adds something else — the
// recovered byte stream will diverge from the unwired path and this
// test will catch it.
//
// (For the stronger invariant — that nonzero cpu->cycles actually
// shifts the LSS read window — we'd need cycle-accurate end-to-end
// scenarios driving an actual 6502 program at $C0EC, which is the job
// of `disk_boot_smoke` / `dos33_save_smoke`. Those tests exercise the
// real `M6502::step` → `cycles=4` → MMIO access at sub-cycle 3 path.)
bool testSubInstructionCycleAccuracyNoOpWithZero() {
    const std::string lssPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom"
    });
    if (lssPath.empty()) {
        std::printf("[SKIP] roms/diskii_p6.rom not found — "
                    "skipping sub-instruction test\n");
        return true;
    }
    const std::string nibPath = makeSyntheticNib();

    auto runReads = [&](DiskIICard& card) {
        card.deviceSelectRead(0x9); card.deviceSelectRead(0xE); card.deviceSelectRead(0xC);
        card.advanceCycles(1024);
        std::vector<uint8_t> out;
        for (int i = 0; i < 64; ++i) {
            card.advanceCycles(6);
            const uint8_t v = card.deviceSelectRead(0xC);
            if (v & 0x80) {
                if (out.empty() || out.back() != v) out.push_back(v);
            }
        }
        return out;
    };

    DiskIICard cardA;
    if (!cardA.loadLssRom(lssPath) || !cardA.insertDisk(nibPath)) return false;
    M6502 cpu;     // freshly constructed → cycles == 0
    cardA.setCpu(&cpu);
    const auto a = runReads(cardA);

    DiskIICard cardB;
    if (!cardB.loadLssRom(lssPath) || !cardB.insertDisk(nibPath)) return false;
    // No setCpu.
    const auto b = runReads(cardB);

    if (a != b) {
        std::printf("FAIL: setCpu(&cpu, cycles=0) diverged from no-cpu path "
                    "(a.size=%zu, b.size=%zu)\n", a.size(), b.size());
        return false;
    }
    std::printf("[ OK ] SubInstructionScope no-op with cpu->cycles=0 "
                "(%zu bytes match no-cpu path)\n", a.size());
    return true;
}

// Test 6: end-to-end LSS recovers the FULL byte stream (not just the
// prologue) without dropping cells. Pin against the off-by-one bug in
// `lss_sync` where `getNextTransition(lssCycle - 1)` returned the
// previous (already-processed) flux event whenever lss_sync re-entered
// at exactly `prev_nextFluxDown`, causing the inner loop to skip every
// flux event in (prev_event, cyclesLimit] and silently corrupt sustained
// reads. The bug is invisible to `testAddressMarkRecovery` because that
// test only requires D5 AA 96 to surface *somewhere* in a 5-revolution
// scan — D5 AA 96 has wide enough margin that re-finding it on the next
// revolution masks the lost cells.
//
// CRITICAL — the bug triggers only when lssCycle lands at exactly
// `prev_event_cycle + 1` at lss_sync entry. For our flux placement at
// `cell*8 + 4`, that's lssCycle ≡ 5 (mod 8). With the canonical RWTS
// spin loop `LDA $C0EC ; BPL loop` (4 + 2 = 6 CPU cycles per iter), the
// LSS-cycle stride is 12, and post-`advanceCycles(4)` lssCycles land at
// `12k + 1` ≡ {1, 5} alternating, hitting the trigger every other LDA.
// The 8-cycle stride spin used by the previous tests lands at lssCycle
// ≡ 1 (mod 8) only, NEVER touching 5 (mod 8) — which is precisely why
// the existing diskii_lss_smoke and dos33_save / prodos_save tests miss
// the bug despite being end-to-end disk-read tests.
//
// This test pins the EXACT byte sequence the synthetic track encodes:
// D5 AA 96 EB followed by sync $FF runs, all repeating after one
// revolution. We deliberately use 6 CPU cycles per read (RWTS pacing)
// so the bug condition fires every other iteration. With the off-by-one
// fix, every D5 occurrence in the recovered stream must be followed by
// AA 96 EB. With the bug, the pattern desyncs and follow-on bytes are
// garbage.
bool testFullSectorReadback() {
    const std::string lssPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom"
    });
    if (lssPath.empty()) {
        std::printf("[SKIP] roms/diskii_p6.rom not found — "
                    "skipping full-readback test\n");
        return true;
    }
    DiskIICard card;
    if (!card.loadLssRom(lssPath)) {
        std::printf("FAIL: loadLssRom\n"); return false;
    }
    const std::string nibPath = makeSyntheticNib();
    if (!card.insertDisk(nibPath)) {
        std::printf("FAIL: insertDisk: %s\n", card.getLastError().c_str());
        return false;
    }

    // 6 CPU cycles per read = RWTS-canonical `LDA $C0EC ; BPL loop`
    // pacing. This forces the bug condition (lssCycle ≡ 5 mod 8 at
    // lss_sync entry) to fire on every other LDA. Spin for ~5
    // revolutions; 256 unique-transition bytes spans many prologues.
    const auto bytes = spinAndCollect(card, 2'000'000, 256, /*cyclesPerRead=*/6);

    // Find the FIRST valid prologue. Before that point, the LSS may be
    // mid-byte from startup and any spurious D5 lookalikes are tolerated.
    size_t firstValid = bytes.size();
    for (size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] == 0xD5 && bytes[i+1] == 0xAA
            && bytes[i+2] == 0x96 && bytes[i+3] == 0xEB) {
            firstValid = i;
            break;
        }
    }
    if (firstValid >= bytes.size()) {
        std::printf("FAIL: no valid D5 AA 96 EB prologue in %zu bytes\n",
                    bytes.size());
        return false;
    }

    // From the first valid prologue forward, EVERY D5 must be followed
    // by AA 96 EB. With the lss_sync off-by-one bug, reads desync after
    // the first prologue and subsequent D5s are followed by garbage.
    int prologuesSeen = 0;
    int prologuesValid = 0;
    for (size_t i = firstValid; i + 3 < bytes.size(); ++i) {
        if (bytes[i] != 0xD5) continue;
        ++prologuesSeen;
        if (bytes[i+1] == 0xAA && bytes[i+2] == 0x96 && bytes[i+3] == 0xEB) {
            ++prologuesValid;
        } else {
            std::printf("    D5 at offset %zu followed by %02X %02X %02X "
                        "(expected AA 96 EB)\n",
                        i, bytes[i+1], bytes[i+2], bytes[i+3]);
        }
    }

    if (prologuesSeen < 2) {
        std::printf("FAIL: only saw %d D5 occurrences after startup in "
                    "%zu bytes — not enough revolutions\n",
                    prologuesSeen, bytes.size());
        return false;
    }
    if (prologuesValid < prologuesSeen) {
        std::printf("FAIL: %d / %d D5 occurrences had a corrupt "
                    "follow-on after the first valid prologue at offset "
                    "%zu (expected AA 96 EB after every D5)\n",
                    prologuesValid, prologuesSeen, firstValid);
        return false;
    }
    std::printf("[ OK ] full readback: %d / %d D5 prologues followed by "
                "AA 96 EB (first valid at offset %zu of %zu)\n",
                prologuesValid, prologuesSeen, firstValid, bytes.size());
    return true;
}

}  // namespace

// Test: motor spin-down must complete even when the selected drive is empty.
// Bug (round 9 #4): advanceCycles early-returned on !isLoaded BEFORE ticking
// the spin-down delay, so motor-off on an empty selected drive left motorOn /
// active stuck on forever (drive LED never clears, no spin-down sound).
bool testSpinDownNoDisk() {
    const std::string lssPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom"
    });
    if (lssPath.empty()) {
        std::printf("[SKIP] roms/diskii_p6.rom not found — skipping spin-down test\n");
        return true;
    }
    DiskIICard card;
    if (!card.loadLssRom(lssPath)) { std::printf("FAIL: loadLssRom\n"); return false; }
    // Insert then eject so the LSS path is latched active (useBitLss) but the
    // selected drive (0) is now EMPTY — the exact bug condition.
    const std::string nibPath = makeSyntheticNib();
    if (!card.insertDisk(nibPath)) {
        std::printf("FAIL: insertDisk: %s\n", card.getLastError().c_str());
        return false;
    }
    card.ejectDisk(0);
    assert(!card.isDiskLoaded(0));

    card.deviceSelectRead(0x9);   // motor on  → MODE_ACTIVE
    card.deviceSelectRead(0x8);   // motor off → MODE_DELAY (≈1.02M-cycle spin-down)
    if (!card.isMotorOn()) {
        std::printf("FAIL: motor should still be on during the spin-down delay\n");
        return false;
    }
    card.advanceCycles(2'000'000);   // well past the spin-down delay
    if (card.isMotorOn()) {
        std::printf("FAIL: motor stuck on after spin-down (empty selected drive)\n");
        return false;
    }

    // Round 9 #7: out-of-range drive indices must return safe defaults, not
    // index images[]/headQuarterTrack[]/trackPos[] out of bounds.
    assert(!card.isDiskLoaded(99));
    assert(card.getCurrentTrack(99) == 0);
    assert(card.getQuarterTrack(-1) == 0);
    assert(card.getTrackPosition(7) == 0);
    assert(card.getDiskPath(-1).empty());
    assert(!card.hasUnsavedChanges(5));

    std::printf("[ OK ] spin-down completes with no disk in the selected drive\n");
    return true;
}

// Test: snapshot restore keeps the card's write-back flag and the
// per-drive DiskImages in lock-step. loadSnapshotState used to restore
// the raw `writeBackEnabled` member WITHOUT the setWriteBackEnabled
// fan-out, so the images kept their pre-restore setting: a snapshot
// taken with write-back ON restored into a session that started with
// it OFF left every image's isWriteProtected() true (RWTS saw a WP
// disk) and saveDirty() refusing to persist. Pin via save-on-eject:
// after the restore, a legacy-gate write must reach the host file.
bool testSnapshotWriteBackPropagation() {
    // Source card: disk + write-back ON → snapshot.
    DiskIICard cardA;
    if (!cardA.insertDisk(makeSyntheticNib())) {
        std::printf("FAIL: insertDisk (A): %s\n", cardA.getLastError().c_str());
        return false;
    }
    cardA.setWriteBackEnabled(true);
    std::vector<uint8_t> snap;
    cardA.appendSnapshotState(snap);

    // Target card: same media, write-back OFF (the default), restore.
    DiskIICard cardB;
    const std::string nibPath = makeSyntheticNib();
    if (!cardB.insertDisk(nibPath)) {
        std::printf("FAIL: insertDisk (B): %s\n", cardB.getLastError().c_str());
        return false;
    }
    cardB.loadSnapshotState(snap.data(), snap.size());
    if (!cardB.isWriteBackEnabled()) {
        std::printf("FAIL: card write-back flag not restored\n");
        return false;
    }

    // Write one distinctive nibble through the legacy gate…
    cardB.deviceSelectRead(0x9);          // motor on
    cardB.deviceSelectRead(0xF);          // Q7H (write mode)
    cardB.deviceSelectRead(0xD);          // Q6H (load)
    cardB.deviceSelectWrite(0xD, 0xD7);   // latch nibble $D7
    cardB.advanceCycles(64);              // ≥ 1 nibble period
    if (!cardB.hasUnsavedChanges(0)) {
        std::printf("FAIL: legacy-gate write didn't dirty the image\n");
        return false;
    }
    // …and eject: save-on-eject goes through DiskImage::saveDirty, which
    // gates on the IMAGE's writeBackEnabled — the flag the restore used
    // to leave stale.
    cardB.ejectDisk(0);
    std::ifstream f(nibPath, std::ios::binary);
    std::vector<uint8_t> track0(DiskImage::kNibblesPerTrack);
    f.read(reinterpret_cast<char*>(track0.data()),
           static_cast<std::streamsize>(track0.size()));
    if (!f) { std::printf("FAIL: re-read %s\n", nibPath.c_str()); return false; }
    bool found = false;
    for (uint8_t b : track0) if (b == 0xD7) { found = true; break; }
    if (!found) {
        std::printf("FAIL: $D7 write not persisted on eject — snapshot "
                    "restore left the DiskImage write-back flag stale\n");
        return false;
    }
    std::printf("[ OK ] snapshot restore propagates write-back to the "
                "per-drive images\n");
    return true;
}

namespace {

// A .nib whose track 0 is entirely $00 — a loaded disk sitting on a track
// with no flux at all. Every other track keeps sync $FF so the image is
// otherwise ordinary. This is the shape a WOZ TMAP $FF entry, a track past
// 34, or an erased half-track under a nibble scanner presents to the LSS.
std::string makeBlankTrackNib(const char* name, uint8_t fill) {
    constexpr int kTracks = DiskImage::kTracks;
    constexpr int kBytesPerTrack = DiskImage::kNibblesPerTrack;
    std::vector<uint8_t> img(static_cast<size_t>(kTracks) * kBytesPerTrack, 0xFF);
    for (int i = 0; i < kBytesPerTrack; ++i) img[i] = fill;
    const auto tmp = fs::temp_directory_path() / name;
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    assert(f);
    const size_t wrote = std::fwrite(img.data(), 1, img.size(), f);
    assert(wrote == img.size());
    std::fclose(f);
    return tmp.string();
}

}  // namespace

// Test: `LDA $C08C,X / BPL` on a LOADED disk parked over a track with no flux
// must terminate.
//
// `getNextTransition` answers kFluxNever for exactly that track, so PULSE
// never fired, the sequencer shifted in nothing but zeros, bit 7 of the data
// register never came up, and the universal wait-for-a-nibble loop spun
// forever — with the machine and the UI frozen. Identical in shape to the
// empty-drive hang (Ultima V $D407) that `advanceNoise` was written for; this
// is the same wire one step further in, with media actually present. MAME
// gets here from the other end: `cache_weakness_setup` (floppy.cpp:1176)
// marks a cell buffer of one entry or less weak and serves noise.
bool testBlankTrackDoesNotHang() {
    const std::string lssPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom"
    });
    if (lssPath.empty()) {
        std::printf("[SKIP] roms/diskii_p6.rom not found — blank-track test\n");
        return true;
    }
    DiskIICard card;
    if (!card.loadLssRom(lssPath)) { std::printf("FAIL: loadLssRom\n"); return false; }
    const std::string nib = makeBlankTrackNib("pom2_lss_blank_track.nib", 0x00);
    if (!card.insertDisk(nib)) {
        std::printf("FAIL: insertDisk: %s\n", card.getLastError().c_str());
        return false;
    }
    assert(card.isDiskLoaded(0));

    // Two emulated seconds of `LDA $C0EC / BPL`, exactly as the guest writes
    // it. RWTS does not read one byte and stop — it reads a whole sector's
    // worth before its retry counter drains, so the bar is a STREAM: sixteen
    // successive byte-ready events, not one. (Before the fix the sequencer
    // managed exactly one, from its power-up latch, and then never again.)
    constexpr size_t kWant = 16;
    const std::vector<uint8_t> got = spinAndCollect(card, 2'000'000, kWant);
    if (got.size() < kWant) {
        std::printf("FAIL: blank track — only %zu of %zu byte-ready events in "
                    "2 emulated seconds; `LDA $C0EC / BPL` would hang the "
                    "machine\n", got.size(), kWant);
        return false;
    }
    for (uint8_t b : got) {
        if (!(b & 0x80)) { std::printf("FAIL: collected byte without bit 7\n"); return false; }
    }
    std::printf("[ OK ] blank track on a loaded disk still raises byte-ready "
                "(%zu bytes)\n", got.size());
    return true;
}

// Test: weak-bit noise on a flux gap wider than MAME's
// `m_amplifier_freakout_time` (floppy.cpp:293, 16 µs).
//
// A read amplifier with nothing to lock onto follows its own noise, and MAME
// models that as one hash-drawn blip per weak zone per REVOLUTION. Without
// it, a long erased run read back byte-identical on every pass, so weak-bit
// protections stopped being a coin toss and became a constant. The check is
// at the flux level (deterministic, no ROM needed): the angular position of
// the reported transition inside one weak zone must move from revolution to
// revolution — and a normal GCR track must NOT move, or every ordinary read
// has just been made non-reproducible.
bool testWeakBitNoise() {
    // Track 0 = one sync $FF then all $00: one real transition per
    // revolution and a gap of most of a track — far past 32 LSS cycles.
    const std::string weakPath = makeBlankTrackNib("pom2_lss_weak.nib", 0x00);
    {
        std::fstream f(weakPath, std::ios::in | std::ios::out | std::ios::binary);
        const uint8_t ff = 0xFF;
        f.seekp(0);
        f.write(reinterpret_cast<const char*>(&ff), 1);
    }
    DiskImage weak;
    if (!weak.loadFile(weakPath)) { std::printf("FAIL: load weak nib\n"); return false; }
    const int period = weak.trackPeriod(0);
    if (period <= 0) { std::printf("FAIL: weak track period %d\n", period); return false; }

    std::vector<int64_t> angles;
    for (int rev = 0; rev < 12; ++rev) {
        const int64_t from = static_cast<int64_t>(rev) * period + 64;
        const int64_t t = weak.getNextTransition(0, from, /*revolutionStart=*/0);
        if (t == DiskImage::kFluxNever) {
            std::printf("FAIL: weak track reported no transition at all\n");
            return false;
        }
        angles.push_back(t - static_cast<int64_t>(rev) * period);
    }
    size_t distinct = 0;
    for (size_t i = 0; i < angles.size(); ++i) {
        bool seen = false;
        for (size_t j = 0; j < i; ++j) if (angles[j] == angles[i]) seen = true;
        if (!seen) ++distinct;
    }
    if (distinct < 3) {
        std::printf("FAIL: weak zone read the same on 12 revolutions "
                    "(%zu distinct angles) — no amplifier noise\n", distinct);
        return false;
    }

    // The other half of the contract: an ordinary GCR surface has no gap
    // anywhere near 16 µs (the widest legal run is a sync $FF's two pad
    // cells, 3 cells = 24 LSS cycles), so it must stay perfectly repeatable.
    DiskImage normal;
    if (!normal.loadFile(makeSyntheticNib())) { std::printf("FAIL: load nib\n"); return false; }
    const int nperiod = normal.trackPeriod(0);
    for (int rev = 0; rev < 4; ++rev) {
        for (int off = 0; off < nperiod; off += nperiod / 37 + 1) {
            const int64_t a = normal.getNextTransition(0, off, 0);
            const int64_t b = normal.getNextTransition(
                0, static_cast<int64_t>(rev) * nperiod + off, 0);
            if (b - static_cast<int64_t>(rev) * nperiod != a) {
                std::printf("FAIL: an ordinary GCR track moved between "
                            "revolutions (rev %d, offset %d)\n", rev, off);
                return false;
            }
        }
    }
    std::printf("[ OK ] weak zone (> 16 us gap) draws %zu distinct angles over "
                "12 revolutions; ordinary GCR stays repeatable\n", distinct);
    return true;
}

int main() {
    bool ok = true;
    ok &= testRomLoad();
    ok &= testBitStreamLength();
    ok &= testAddressMarkRecovery();
    ok &= testLssWrite();
    ok &= testLegacyFallback();
    ok &= testFullSectorReadback();
    ok &= testSubInstructionCycleAccuracyNoOpWithZero();
    ok &= testSpinDownNoDisk();
    ok &= testSnapshotWriteBackPropagation();
    ok &= testBlankTrackDoesNotHang();
    ok &= testWeakBitNoise();
    if (ok) {
        std::printf("diskii_lss_smoke OK\n");
        return 0;
    }
    std::printf("diskii_lss_smoke FAILED\n");
    return 1;
}
