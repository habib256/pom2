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

// Mixed-unit smoke for SmartPortCard: unit 0 = 3.5" Sony, unit 1 = ProDOS
// HDV. Round-trips reads + writes through the ROM-emitted slot driver
// stream protocol (drive-select / block LO/HI / streaming byte port).
// Pins:
//   * Unit 0 reads come from the 3.5" image, unit 1 reads from the HDV
//     image — i.e. drive-select actually routes to the right unit.
//   * Status byte tracks per-unit (loaded / write-protected) state.
//   * Write-back to the HDV unit persists at the byte level and survives
//     a read-back via the card.

#include "Disk35Image.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kBlockBytes = pom2::SmartPortUnit::kBlockBytes;
constexpr size_t k35Blocks   = 1600;     // 800 K
constexpr size_t kHdvBlocks  = 64;       // 32 KB synthetic — small + fast

std::string writeSynth35(const char* tag, uint8_t fill)
{
    std::vector<uint8_t> data(k35Blocks * kBlockBytes);
    for (size_t b = 0; b < k35Blocks; ++b) {
        std::memset(data.data() + b * kBlockBytes,
                    static_cast<uint8_t>(fill + (b & 0xFF)),
                    kBlockBytes);
    }
    const std::string p =
        (std::filesystem::temp_directory_path() /
         (std::string("pom2_spmixed_") + tag + ".po")).string();
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return p;
}

std::string writeSynthHdv(const char* tag, uint8_t fill)
{
    std::vector<uint8_t> data(kHdvBlocks * kBlockBytes);
    for (size_t b = 0; b < kHdvBlocks; ++b) {
        std::memset(data.data() + b * kBlockBytes,
                    static_cast<uint8_t>(fill + b),
                    kBlockBytes);
    }
    const std::string p =
        (std::filesystem::temp_directory_path() /
         (std::string("pom2_spmixed_") + tag + ".hdv")).string();
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return p;
}

void writeReg(pom2::SmartPortCard& c, uint8_t low4, uint8_t v) {
    c.deviceSelectWrite(low4, v);
}
uint8_t readReg(pom2::SmartPortCard& c, uint8_t low4) {
    return c.deviceSelectRead(low4);
}

void readBlockViaCard(pom2::SmartPortCard& c, uint8_t unit, uint16_t blk,
                      uint8_t out[kBlockBytes]) {
    writeReg(c, 0x0, unit);
    writeReg(c, 0x1, static_cast<uint8_t>(blk & 0xFF));
    writeReg(c, 0x2, static_cast<uint8_t>((blk >> 8) & 0xFF));
    for (size_t i = 0; i < kBlockBytes; ++i) out[i] = readReg(c, 0x3);
}

void writeBlockViaCard(pom2::SmartPortCard& c, uint8_t unit, uint16_t blk,
                       const uint8_t in[kBlockBytes]) {
    writeReg(c, 0x0, unit);
    writeReg(c, 0x1, static_cast<uint8_t>(blk & 0xFF));
    writeReg(c, 0x2, static_cast<uint8_t>((blk >> 8) & 0xFF));
    for (size_t i = 0; i < kBlockBytes; ++i) writeReg(c, 0x3, in[i]);
}

bool testMixedReads()
{
    const std::string p35  = writeSynth35 ("u0", 0x10);
    const std::string phdv = writeSynthHdv("u1", 0x80);

    auto u35  = std::make_unique<pom2::SmartPort35Unit>();
    auto uhdv = std::make_unique<pom2::SmartPortHdvUnit>();
    if (!u35->loadImage(p35)) {
        std::printf("FAIL: 35 load: %s\n", u35->lastError().c_str());
        return false;
    }
    if (!uhdv->loadImage(phdv)) {
        std::printf("FAIL: hdv load: %s\n", uhdv->lastError().c_str());
        return false;
    }

    pom2::SmartPortCard card(5);
    card.setUnit(0, std::move(u35));
    card.setUnit(1, std::move(uhdv));

    // Unit 0 (3.5") block 5 → fill = 0x10 + 5 = 0x15
    uint8_t buf[kBlockBytes];
    readBlockViaCard(card, 0, 5, buf);
    for (size_t i = 0; i < kBlockBytes; ++i) {
        if (buf[i] != 0x15) {
            std::printf("FAIL: u0/blk5 byte %zu = %02X (want 15)\n", i, buf[i]);
            return false;
        }
    }
    // Unit 1 (HDV) block 3 → fill = 0x80 + 3 = 0x83
    readBlockViaCard(card, 1, 3, buf);
    for (size_t i = 0; i < kBlockBytes; ++i) {
        if (buf[i] != 0x83) {
            std::printf("FAIL: u1/blk3 byte %zu = %02X (want 83)\n", i, buf[i]);
            return false;
        }
    }
    // Switch back to unit 0 — make sure unit 1's selection didn't leak.
    readBlockViaCard(card, 0, 5, buf);
    if (buf[0] != 0x15) {
        std::printf("FAIL: u0/blk5 re-read = %02X (want 15)\n", buf[0]);
        return false;
    }
    std::printf("OK : mixed-unit reads (3.5\" + HDV)\n");
    return true;
}

bool testMixedStatus()
{
    // Empty 3.5", loaded HDV — exercise the per-unit status routing.
    pom2::SmartPortCard card(5);
    card.setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    auto uhdv = std::make_unique<pom2::SmartPortHdvUnit>();
    const std::string phdv = writeSynthHdv("status", 0x44);
    if (!uhdv->loadImage(phdv)) {
        std::printf("FAIL: hdv load: %s\n", uhdv->lastError().c_str());
        return false;
    }
    pom2::SmartPortHdvUnit* uraw = uhdv.get();
    card.setUnit(1, std::move(uhdv));

    writeReg(card, 0x0, 0);
    if ((readReg(card, 0x4) & 0x80) == 0) {
        std::printf("FAIL: empty 35 status bit7 not set\n"); return false;
    }
    writeReg(card, 0x0, 1);
    if ((readReg(card, 0x4) & 0x80) != 0) {
        std::printf("FAIL: loaded HDV status bit7 still set\n"); return false;
    }
    // ONE rule per card (TODO.md R1, settled by G5-1): a unit is
    // write-protected when the medium is, OR when write-back is off — the
    // contract `SmartPortUnit.h` declares and the 3.5" unit has always
    // honoured. This test used to pin the opposite for the HDV unit ("WP
    // bit stays clear either way"), which made two bays of the same card
    // answer the same toggle differently, and let a write-back-off HDV take
    // a session of writes into RAM and drop them at eject. Media are
    // writable by default since 2026-09-08, so protect the HDV explicitly.
    uraw->setWriteBackEnabled(false);
    if ((readReg(card, 0x4) & 0x40) == 0) {
        std::printf("FAIL: HDV WP bit clear with write-back off\n");
        return false;
    }
    uint8_t blockBuf[kBlockBytes] = {0};
    if (uraw->writeBlock(3, blockBuf)) {
        std::printf("FAIL: HDV unit accepted a write with write-back off\n");
        return false;
    }
    uraw->setWriteBackEnabled(true);
    if ((readReg(card, 0x4) & 0x40) != 0) {
        std::printf("FAIL: HDV WP bit set after writeBack on\n");
        return false;
    }
    // Both bays, same toggle, same answer.
    {
        auto u35 = std::make_unique<pom2::SmartPort35Unit>();
        const std::string p35 = writeSynth35("wpc", 0x11);
        if (!u35->loadImage(p35)) {
            std::printf("FAIL: 35 load: %s\n", u35->lastError().c_str());
            return false;
        }
        u35->setWriteBackEnabled(false);
        uraw->setWriteBackEnabled(false);
        if (u35->isWriteProtected() != uraw->isWriteProtected()) {
            std::printf("FAIL: 3.5\" and HDV units disagree on WP (write-back off)\n");
            return false;
        }
        u35->setWriteBackEnabled(true);
        uraw->setWriteBackEnabled(true);
        if (u35->isWriteProtected() != uraw->isWriteProtected()) {
            std::printf("FAIL: 3.5\" and HDV units disagree on WP (write-back on)\n");
            return false;
        }
    }
    std::printf("OK : per-unit status routing\n");
    return true;
}

bool testHdvWriteback()
{
    const std::string phdv = writeSynthHdv("wb", 0x00);
    auto uhdv = std::make_unique<pom2::SmartPortHdvUnit>();
    if (!uhdv->loadImage(phdv)) {
        std::printf("FAIL: hdv load: %s\n", uhdv->lastError().c_str());
        return false;
    }
    uhdv->setWriteBackEnabled(true);
    pom2::SmartPortHdvUnit* uraw = uhdv.get();

    pom2::SmartPortCard card(5);
    card.setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    card.setUnit(1, std::move(uhdv));

    uint8_t pattern[kBlockBytes];
    for (size_t i = 0; i < kBlockBytes; ++i)
        pattern[i] = static_cast<uint8_t>((i * 17) ^ 0xC3);
    writeBlockViaCard(card, 1, 7, pattern);

    uint8_t roundtrip[kBlockBytes];
    readBlockViaCard(card, 1, 7, roundtrip);
    if (std::memcmp(pattern, roundtrip, kBlockBytes) != 0) {
        std::printf("FAIL: card roundtrip mismatch\n"); return false;
    }
    uint8_t direct[kBlockBytes];
    if (!uraw->readBlock(7, direct)) {
        std::printf("FAIL: unit readBlock(7)\n"); return false;
    }
    if (std::memcmp(pattern, direct, kBlockBytes) != 0) {
        std::printf("FAIL: pattern didn't reach HDV bytes\n"); return false;
    }
    std::printf("OK : HDV unit write-back roundtrip\n");
    return true;
}

bool testUnitFactory()
{
    auto u35  = pom2::makeSmartPortUnit("35");
    auto uhdv = pom2::makeSmartPortUnit("hdv");
    auto bad  = pom2::makeSmartPortUnit("nonsense");
    if (!u35 || u35->kindKey() != "35") {
        std::printf("FAIL: factory \"35\"\n"); return false;
    }
    if (!uhdv || uhdv->kindKey() != "hdv") {
        std::printf("FAIL: factory \"hdv\"\n"); return false;
    }
    if (bad) {
        std::printf("FAIL: factory accepted nonsense\n"); return false;
    }
    std::printf("OK : factory dispatch\n");
    return true;
}

} // anon namespace

int main()
{
    bool ok = true;
    ok &= testUnitFactory();
    ok &= testMixedReads();
    ok &= testMixedStatus();
    ok &= testHdvWriteback();
    return ok ? 0 : 1;
}
