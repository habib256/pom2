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

// SmartPortCard smoke test.
//
// Pins the //e Liron-class card's byte-stream protocol:
//   $C0n0 write = drive select (0 / 1)
//   $C0n1/n2    = block LO/HI of the active drive
//   $C0n3 read  = streaming data byte from the active drive
//   $C0n3 write = streaming data byte into the active drive
//   $C0n4 read  = status (bit7=no disk, bit6=write-protected)
//
// Test plan:
//   1. Plug the card with two synthetic Disk35Image objects pre-populated
//      with distinguishable block payloads.
//   2. Read block 5 of drive 1 — verify all 512 bytes match.
//   3. Switch to drive 2, read block 7 — verify it does NOT match drive 1
//      (drive selector actually picks the right image).
//   4. Read status with no disk → bit7 set; with disk → bit7 clear.
//   5. Enable write-back, write a 512 B pattern to drive 1 block 10,
//      eject + remount the image, read back — verifies the write path
//      reached Disk35Image and survived the disk-image internals (dirty
//      tracking, write commit on block-boundary).

#include "Disk35Image.h"
#include "SmartPort35Unit.h"
#include "MediaWritePolicy.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlockBytes  = 512;
constexpr size_t kBlocks      = 1600;     // 800 K = 1600 × 512

// Build an 800 K .po image where block N is filled with byte `seed + N`.
// The seed lets the two drives have different content so a swapped
// drive-select returns wrong data we can detect.
std::string writeSyntheticPo(const char* tag, uint8_t seed)
{
    const auto p = fs::temp_directory_path()
        / (std::string("pom2_spcard_") + tag + ".po");
    std::vector<uint8_t> img(kBlocks * kBlockBytes);
    for (size_t b = 0; b < kBlocks; ++b) {
        const uint8_t fill = static_cast<uint8_t>(seed + b);
        std::memset(img.data() + b * kBlockBytes, fill, kBlockBytes);
    }
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp .po for writing");
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return p.string();
}

void writeReg(pom2::SmartPortCard& card, uint8_t low4, uint8_t v) {
    card.deviceSelectWrite(low4, v);
}
uint8_t readReg(pom2::SmartPortCard& card, uint8_t low4) {
    return card.deviceSelectRead(low4);
}

// Set up active drive + block, then stream 512 bytes out of $C0n3.
bool readBlockViaCard(pom2::SmartPortCard& card,
                      int drive, uint16_t block,
                      uint8_t out[kBlockBytes])
{
    writeReg(card, 0x0, static_cast<uint8_t>(drive));
    writeReg(card, 0x1, static_cast<uint8_t>(block & 0xFF));
    writeReg(card, 0x2, static_cast<uint8_t>((block >> 8) & 0xFF));
    for (size_t i = 0; i < kBlockBytes; ++i) {
        out[i] = readReg(card, 0x3);
    }
    return true;
}

// Same but for writes — drive + block setup then 512 stores at $C0n3.
void writeBlockViaCard(pom2::SmartPortCard& card,
                       int drive, uint16_t block,
                       const uint8_t in[kBlockBytes])
{
    writeReg(card, 0x0, static_cast<uint8_t>(drive));
    writeReg(card, 0x1, static_cast<uint8_t>(block & 0xFF));
    writeReg(card, 0x2, static_cast<uint8_t>((block >> 8) & 0xFF));
    for (size_t i = 0; i < kBlockBytes; ++i) {
        writeReg(card, 0x3, in[i]);
    }
}

bool testDriveSelectAndRead()
{
    const std::string path0 = writeSyntheticPo("d1", 0x10);
    const std::string path1 = writeSyntheticPo("d2", 0xA0);

    auto u0 = std::make_unique<pom2::SmartPort35Unit>();
    auto u1 = std::make_unique<pom2::SmartPort35Unit>();
    if (!u0->loadImage(path0)) {
        std::printf("FAIL: load d1: %s\n", u0->lastError().c_str()); return false;
    }
    if (!u1->loadImage(path1)) {
        std::printf("FAIL: load d2: %s\n", u1->lastError().c_str()); return false;
    }
    pom2::SmartPortCard card(5);
    card.setUnit(0, std::move(u0));
    card.setUnit(1, std::move(u1));

    // Drive 1, block 5 — fill byte = 0x10 + 5 = 0x15.
    uint8_t buf[kBlockBytes];
    readBlockViaCard(card, 0, 5, buf);
    for (size_t i = 0; i < kBlockBytes; ++i) {
        if (buf[i] != 0x15) {
            std::printf("FAIL: d1 blk5 byte %zu = %02X (want 15)\n", i, buf[i]);
            return false;
        }
    }

    // Drive 2, block 7 — fill byte = 0xA0 + 7 = 0xA7.
    readBlockViaCard(card, 1, 7, buf);
    for (size_t i = 0; i < kBlockBytes; ++i) {
        if (buf[i] != 0xA7) {
            std::printf("FAIL: d2 blk7 byte %zu = %02X (want A7)\n", i, buf[i]);
            return false;
        }
    }

    // Sanity: read d1 again to make sure d2's setup didn't leak.
    readBlockViaCard(card, 0, 5, buf);
    if (buf[0] != 0x15) {
        std::printf("FAIL: d1 blk5 re-read = %02X (want 15)\n", buf[0]);
        return false;
    }
    std::printf("OK : drive select + streaming reads\n");
    return true;
}

bool testStatusByte()
{
    pom2::SmartPortCard card(5);
    auto u0 = std::make_unique<pom2::SmartPort35Unit>();
    auto u1 = std::make_unique<pom2::SmartPort35Unit>();
    pom2::SmartPort35Unit* u0raw = u0.get();
    card.setUnit(0, std::move(u0));
    card.setUnit(1, std::move(u1));

    // Both empty → status bit7 = 1 on both drives.
    writeReg(card, 0x0, 0);
    if ((readReg(card, 0x4) & 0x80) == 0) {
        std::printf("FAIL: empty d1 status bit7 not set\n"); return false;
    }
    writeReg(card, 0x0, 1);
    if ((readReg(card, 0x4) & 0x80) == 0) {
        std::printf("FAIL: empty d2 status bit7 not set\n"); return false;
    }

    // Mount d1 → bit7 clears.
    const std::string p = writeSyntheticPo("st", 0x33);
    if (!u0raw->loadImage(p)) {
        std::printf("FAIL: load: %s\n", u0raw->lastError().c_str()); return false;
    }
    writeReg(card, 0x0, 0);
    if ((readReg(card, 0x4) & 0x80) != 0) {
        std::printf("FAIL: mounted d1 status bit7 still set\n"); return false;
    }
    // A fresh mount follows the policy default (MediaWritePolicy.h: writable
    // in the product, protected under the suite's environment) → bit6 = !policy.
    const bool wpDefault = ((readReg(card, 0x4) & 0x40) != 0);
    if (wpDefault == pom2::mediaWritableByDefault()) {
        std::printf("FAIL: default-mounted d1 WP bit does not follow the policy\n"); return false;
    }
    // The user's write-protect → bit6 set.
    u0raw->setWriteBackEnabled(false);
    if ((readReg(card, 0x4) & 0x40) == 0) {
        std::printf("FAIL: WP bit not set after write-protecting d1\n"); return false;
    }
    // Lift it → bit6 clears again.
    u0raw->setWriteBackEnabled(true);
    if ((readReg(card, 0x4) & 0x40) != 0) {
        std::printf("FAIL: WP bit still set after lifting the write-protect\n");
        return false;
    }
    std::printf("OK : status byte (no-disk / WP)\n");
    return true;
}

bool testWriteBackRoundtrip()
{
    const std::string p = writeSyntheticPo("wb", 0x00);
    pom2::SmartPortCard card(5);
    auto u0 = std::make_unique<pom2::SmartPort35Unit>();
    pom2::SmartPort35Unit* u0raw = u0.get();
    if (!u0->loadImage(p)) {
        std::printf("FAIL: load: %s\n", u0->lastError().c_str()); return false;
    }
    u0->setWriteBackEnabled(true);
    card.setUnit(0, std::move(u0));
    card.setUnit(1, std::make_unique<pom2::SmartPort35Unit>());

    // Write a recognisable pattern into block 10.
    uint8_t pattern[kBlockBytes];
    for (size_t i = 0; i < kBlockBytes; ++i)
        pattern[i] = static_cast<uint8_t>((i * 31) ^ 0x5A);
    writeBlockViaCard(card, 0, 10, pattern);

    // Read it back via the card → must match.
    uint8_t roundtrip[kBlockBytes];
    readBlockViaCard(card, 0, 10, roundtrip);
    if (std::memcmp(pattern, roundtrip, kBlockBytes) != 0) {
        for (size_t i = 0; i < kBlockBytes; ++i) {
            if (pattern[i] != roundtrip[i]) {
                std::printf("FAIL: byte %zu pattern=%02X read=%02X\n",
                            i, pattern[i], roundtrip[i]);
                return false;
            }
        }
    }

    // Unit-level read should agree too — verifies the card's write
    // actually reached the underlying Disk35Image (via the unit), not
    // just an internal cache invisible to the rest of the emulator.
    uint8_t direct[kBlockBytes];
    if (!u0raw->readBlock(10, direct)) {
        std::printf("FAIL: unit readBlock(10)\n"); return false;
    }
    if (std::memcmp(pattern, direct, kBlockBytes) != 0) {
        std::printf("FAIL: pattern didn't reach Disk35Image\n"); return false;
    }
    std::printf("OK : write-back roundtrip\n");
    return true;
}

// STATUS block-count registers ($C0n5/$C0n6) for an exactly-32 MiB
// (65536-block) HDV unit must clamp to $FFFF, not truncate to 0 —
// Block512Backing::kMaxBlocks deliberately admits 65536 blocks (block
// INDEXES stay 16-bit) and a 0-block STATUS makes ProDOS treat the
// volume as empty/offline. Also pins the exact pass-through for an
// 800 K unit (1600 = $0640).
bool testBlockCountClamp()
{
    // Sparse 32 MiB .hdv (only the logical size matters; reads as zeros).
    const auto p = fs::temp_directory_path() / "pom2_spcard_32mib.hdv";
    {
        std::ofstream f(p, std::ios::binary);
        assert(f && "open temp .hdv");
        f.seekp(static_cast<std::streamoff>(65536ull * kBlockBytes) - 1);
        const char z = 0;
        f.write(&z, 1);
        assert(f.good());
    }

    pom2::SmartPortCard card(5);
    auto hdv = std::make_unique<pom2::SmartPortHdvUnit>();
    if (!hdv->loadImage(p.string())) {
        std::printf("FAIL: load 32 MiB hdv: %s\n", hdv->lastError().c_str());
        return false;
    }
    if (hdv->blockCount() != 65536u) {
        std::printf("FAIL: 32 MiB unit blockCount = %u\n", hdv->blockCount());
        return false;
    }
    card.setUnit(0, std::move(hdv));

    auto u35 = std::make_unique<pom2::SmartPort35Unit>();
    if (!u35->loadImage(writeSyntheticPo("clamp35", 0x10))) {
        std::printf("FAIL: load 800K unit\n"); return false;
    }
    card.setUnit(1, std::move(u35));

    writeReg(card, 0x0, 0);                  // select the 32 MiB unit
    const uint8_t lo = readReg(card, 0x5);
    const uint8_t hi = readReg(card, 0x6);
    if (lo != 0xFF || hi != 0xFF) {
        std::printf("FAIL: 65536-block STATUS count = %02X%02X, want FFFF "
                    "(truncated to 0?)\n", hi, lo);
        return false;
    }

    writeReg(card, 0x0, 1);                  // 800 K unit: exact count
    const uint8_t lo35 = readReg(card, 0x5);
    const uint8_t hi35 = readReg(card, 0x6);
    if (lo35 != 0x40 || hi35 != 0x06) {
        std::printf("FAIL: 800K STATUS count = %02X%02X, want 0640\n",
                    hi35, lo35);
        return false;
    }
    std::printf("OK : STATUS block count (clamp at $FFFF + exact 800K)\n");
    return true;
}

bool testRomSignature()
{
    pom2::SmartPortCard card(5);
    // ProDOS / SmartPort signature bytes — see SmartPortCard.cpp::buildRom.
    if (card.slotRomRead(0x01) != 0x20) { std::printf("FAIL: $Cn01\n"); return false; }
    if (card.slotRomRead(0x03) != 0x00) { std::printf("FAIL: $Cn03\n"); return false; }
    if (card.slotRomRead(0x05) != 0x03) { std::printf("FAIL: $Cn05\n"); return false; }
    if (card.slotRomRead(0x07) == 0x00) { std::printf("FAIL: $Cn07 zero\n"); return false; }
    if (card.slotRomRead(0xFF) == 0x00) { std::printf("FAIL: $CnFF zero\n"); return false; }
    std::printf("OK : ROM signature\n");
    return true;
}

// Per-unit access LED. Three properties, each of which was broken or
// absent before: the light comes on for the unit that was actually read
// (not card-wide), it does so with NO FloppySoundDevice attached (the
// bump used to sit behind `noteAccess`'s `sound_` early-out), and it
// bleeds off on its own so it cannot latch on forever — which is what
// would have happened had the SmartPort simply reported the
// `Block512Backing` counter nothing decays for it.
bool testAccessLed()
{
    pom2::SmartPortCard card(5);          // no sound sink attached
    auto u0 = std::make_unique<pom2::SmartPort35Unit>();
    auto u1 = std::make_unique<pom2::SmartPort35Unit>();
    if (!u0->loadImage(writeSyntheticPo("led0", 0x30)) ||
        !u1->loadImage(writeSyntheticPo("led1", 0x70))) {
        std::printf("FAIL: LED test image load\n"); return false;
    }
    card.setUnit(0, std::move(u0));
    card.setUnit(1, std::move(u1));

    if (card.bayInfo(0).busy || card.bayInfo(1).busy) {
        std::printf("FAIL: a bay reported busy before any access\n");
        return false;
    }

    uint8_t buf[kBlockBytes];
    if (!readBlockViaCard(card, 0, 5, buf)) {
        std::printf("FAIL: LED test block read\n"); return false;
    }
    if (!card.bayInfo(0).busy) {
        std::printf("FAIL: unit 0 read left its access LED dark\n");
        return false;
    }
    if (card.bayInfo(1).busy) {
        std::printf("FAIL: unit 0 read lit unit 1's LED too\n");
        return false;
    }

    // Decay is driven by the host, one step per frame. It must reach zero
    // in a bounded number of steps; 64 is far past the 8-frame hysteresis
    // and still catches a counter that never moves.
    int frames = 0;
    for (; frames < 64 && card.bayInfo(0).busy; ++frames)
        for (size_t u = 0; u < pom2::SmartPortCard::kMaxUnits; ++u)
            if (auto* unit = card.unit(u)) unit->tickActivityDecay();
    if (card.bayInfo(0).busy) {
        std::printf("FAIL: access LED never decayed (stuck on)\n");
        return false;
    }
    std::printf("OK : per-unit access LED (lit unit 0 only, decayed in %d "
                "frames, no sound device)\n", frames);
    return true;
}

} // anon namespace

int main() {
    bool ok = true;
    ok &= testRomSignature();
    ok &= testDriveSelectAndRead();
    ok &= testStatusByte();
    ok &= testWriteBackRoundtrip();
    ok &= testBlockCountClamp();
    ok &= testAccessLed();
    return ok ? 0 : 1;
}
