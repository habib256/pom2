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

// CFFA 2.0 card smoke test — pins the MAME-faithful port end-to-end through
// the memory map, driving the REAL firmware ROM's view of the card:
//   - 4 KB firmware ROM loads; per-slot $CnXX image visible at $C700 (slot 7)
//   - ProDOS/SmartPort boot signature $Cn01/03/05/07 = $20/$00/$03/$3C
//     (CFFA is F8-autobootable, unlike the synthetic HDV's $Cn07=$01)
//   - $C0nX → ATA decode (8↔16-bit data latch, taskfile regs): READ a sector
//     and WRITE a sector round-trip through AtaBlockDevice via Memory I/O.
//
// ROM-gated: SKIPs cleanly (exit 0) when roms/cffa20ee02.bin is absent.
// Full ProDOS-boot parity is covered by the MAME oracle
// (`mame apple2ee -sl7 cffa2 -hard1 <img>`), not this lean pin.

#include "CffaCard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string findRom() {
    for (const char* p : { "roms/cffa20ee02.bin",
                           "../roms/cffa20ee02.bin",
                           "../../roms/cffa20ee02.bin" }) {
        std::ifstream f(p, std::ios::binary);
        if (f.good()) return p;
    }
    return {};
}

uint8_t pat(uint32_t blk, size_t i) {
    return static_cast<uint8_t>((blk * 11u + i * 5u + 0x07u) & 0xFF);
}

} // namespace

int main() {
    const std::string romPath = findRom();
    if (romPath.empty()) {
        std::printf("cffa_card_smoke: SKIP (roms/cffa20ee02.bin not found)\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    constexpr int kSlot = 7;
    constexpr size_t kBlk = pom2::Block512Backing::kBlockBytes;
    Memory mem;

    auto card = std::make_unique<pom2::CffaCard>(kSlot);
    assert(card->loadRom(romPath));

    // MAME a2cffa.cpp device_start() patches two EEPROM config bytes every
    // boot (enable slave + 13 devices/connector). loadRom must apply the
    // same patch — the raw dump ships 0x04/0x00 ($C801=0 disables connector
    // 2). $C800 window byte 0/1 = rom_[0x800]/rom_[0x801].
    assert(card->expansionRomRead(0x000) == 0x0D);   // $C800 config byte
    assert(card->expansionRomRead(0x001) == 0x0D);   // $C801 config byte

    // 16-block synthetic image with a known per-block pattern.
    const uint32_t blocks = 16;
    std::vector<uint8_t> img(blocks * kBlk);
    for (uint32_t b = 0; b < blocks; ++b)
        for (size_t i = 0; i < kBlk; ++i)
            img[b * kBlk + i] = pat(b, i);
    assert(card->loadImageFromBytes(img, "cffa-smoke", ""));
    assert(card->getBlockCount() == blocks);

    pom2::CffaCard* cffa = card.get();
    mem.slotBus().plug(kSlot, std::move(card));

    // ── Slot ROM: firmware page visible + boot signature ──────────────────
    assert(mem.memRead(0xC700) == 0xA9);   // LDA #imm — first firmware byte
    assert(mem.memRead(0xC701) == 0x20);   // $Cn01
    assert(mem.memRead(0xC703) == 0x00);   // $Cn03
    assert(mem.memRead(0xC705) == 0x03);   // $Cn05
    assert(mem.memRead(0xC707) == 0x3C);   // $Cn07 — F8-bootable (Disk II/SmartPort)

    // Device-select window for slot 7 = $C0F0..$C0FF.
    constexpr uint16_t kBase = 0xC0F0;
    auto regW = [&](uint8_t off, uint8_t v) { mem.memWrite(kBase + off, v); };
    auto regR = [&](uint8_t off) { return mem.memRead(kBase + off); };

    auto setLba = [&](uint32_t lba, uint8_t count) {
        regW(0xA, count);                 // sector count (cs0 reg 2)
        regW(0xB, lba & 0xFF);            // LBA  0..7   (reg 3)
        regW(0xC, (lba >> 8) & 0xFF);     // LBA  8..15  (reg 4)
        regW(0xD, (lba >> 16) & 0xFF);    // LBA 16..23  (reg 5)
        regW(0xE, 0xE0 | ((lba >> 24) & 0x0F)); // LBA mode, drive 0 (reg 6)
    };

    // ── READ block 3 via the ATA taskfile, compare to the pattern ─────────
    setLba(3, 1);
    regW(0xF, pom2::AtaBlockDevice::kCmdRead); // command (reg 7)
    assert(regR(0xF) & pom2::AtaBlockDevice::kStDRQ); // status: data request
    for (size_t i = 0; i < kBlk; i += 2) {
        const uint8_t lo = regR(0x8);  // data low byte ($C0n8: cs0_r(0))
        const uint8_t hi = regR(0x0);  // data high byte ($C0n0: latched)
        assert(lo == pat(3, i));
        assert(hi == pat(3, i + 1));
    }
    assert((regR(0xF) & pom2::AtaBlockDevice::kStDRQ) == 0); // transfer drained

    // ── WRITE block 5 via the ATA taskfile, then read it back ─────────────
    std::vector<uint8_t> src(kBlk);
    for (size_t i = 0; i < kBlk; ++i) src[i] = static_cast<uint8_t>(0xA5 ^ (i * 3));
    setLba(5, 1);
    regW(0xF, pom2::AtaBlockDevice::kCmdWrite);
    assert(regR(0xF) & pom2::AtaBlockDevice::kStDRQ);
    for (size_t i = 0; i < kBlk; i += 2) {
        regW(0x0, src[i + 1]);  // high byte latch ($C0n0)
        regW(0x8, src[i]);      // low byte + 16-bit commit ($C0n8)
    }
    assert((regR(0xF) & pom2::AtaBlockDevice::kStDRQ) == 0);

    // Read it back through a fresh ATA read via the memory map.
    assert(cffa->hasUnsavedChanges()); // block 5 marked dirty in the backing
    uint8_t back[kBlk];
    setLba(5, 1);
    regW(0xF, pom2::AtaBlockDevice::kCmdRead);
    for (size_t i = 0; i < kBlk; i += 2) {
        back[i]     = regR(0x8);
        back[i + 1] = regR(0x0);
    }
    for (size_t i = 0; i < kBlk; ++i) assert(back[i] == src[i]);

    std::printf("cffa_card_smoke: OK (%s)\n", romPath.c_str());
    return 0;
}
