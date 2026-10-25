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

// SmartPort slot-ROM driver WRITE-dispatch regression test.
//
// The card's ProDOS driver dispatch at $Cn50 decodes the command in $42:
// 1 = read, 2 = write, 0 = status. A one-byte branch-offset error made the
// "BEQ write" land in the middle of the READ routine, so a ProDOS
// WRITE_BLOCK (cmd=2) silently READ the block into the caller's buffer,
// wrote nothing to media, and returned "success" — silent data loss on
// every save (the card advertises itself write-capable via $CnFE=$13, and
// ProDOS dispatches writes through $CnFF=$50 → $Cn50).
//
// This drives the REAL firmware through a 6502: it JSRs $Cn50 with cmd=2
// and a distinctive RAM buffer, then checks the mounted unit's block now
// holds that buffer (write actually happened) rather than its original
// content (the read-instead-of-write bug).

#include "M6502.h"
#include "Memory.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlockBytes = 512;
constexpr size_t kBlocks     = 1600;       // 800 K 3.5"

// 800 K .po where block N is filled with byte (seed + N).
std::string writeSyntheticPo(uint8_t seed) {
    const auto p = fs::temp_directory_path() / "pom2_sp_writedispatch.po";
    std::vector<uint8_t> img(kBlocks * kBlockBytes);
    for (size_t b = 0; b < kBlocks; ++b)
        std::memset(img.data() + b * kBlockBytes,
                    static_cast<uint8_t>(seed + b), kBlockBytes);
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp .po");
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return p.string();
}

}  // namespace

int main()
{
    constexpr int      kSlot      = 5;
    constexpr uint16_t kBlockNum  = 10;
    constexpr uint8_t  kBufFill   = 0xC3;   // distinct from synthetic block 10 (=0x0A)

    const std::string po = writeSyntheticPo(/*seed=*/0x00);

    Memory mem;                              // II+ default → slot ROM served
    M6502 cpu(&mem);
    cpu.hardReset();

    auto unit = std::make_unique<pom2::SmartPort35Unit>();
    if (!unit->loadImage(po)) {
        std::printf("FAIL: load .po: %s\n", unit->lastError().c_str());
        return 1;
    }
    unit->setWriteBackEnabled(true);         // clears the WP status bit
    pom2::SmartPort35Unit* uraw = unit.get();

    auto card = std::make_unique<pom2::SmartPortCard>(kSlot);
    card->setUnit(0, std::move(unit));
    card->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
    mem.slotBus().plug(kSlot, std::move(card));

    // Confirm the unit's block 10 starts at its synthetic value (0x0A).
    uint8_t before[kBlockBytes];
    assert(uraw->readBlock(kBlockNum, before));
    assert(before[0] == 0x0A);

    // ProDOS driver parameters in zero page.
    mem.memWrite(0x42, 0x02);                // command = WRITE
    mem.memWrite(0x43, 0x50);                // unit byte: slot 5, drive 1
    mem.memWrite(0x44, 0x00);                // buffer LO
    mem.memWrite(0x45, 0x08);                // buffer HI  → $0800
    mem.memWrite(0x46, kBlockNum & 0xFF);    // block LO
    mem.memWrite(0x47, 0x00);                // block HI
    for (int i = 0; i < (int)kBlockBytes; ++i)
        mem.memWrite(0x0800 + i, kBufFill);  // buffer payload

    // Caller: JSR $Cn50 ; then spin so the CPU parks after the driver RTS.
    const uint8_t romHi = static_cast<uint8_t>(0xC0 + kSlot);   // $C5
    mem.memWrite(0x0300, 0x20);              // JSR
    mem.memWrite(0x0301, 0x50);              // $Cn50 lo (kDriverOff)
    mem.memWrite(0x0302, romHi);             // $Cn50 hi
    mem.memWrite(0x0303, 0x4C);              // JMP $0303 (park)
    mem.memWrite(0x0304, 0x03);
    mem.memWrite(0x0305, 0x03);

    cpu.setProgramCounter(0x0300);
    cpu.run(60000);                          // ample for the 512-byte loop + park

    // The write must have reached the unit's block 10.
    uint8_t after[kBlockBytes];
    if (!uraw->readBlock(kBlockNum, after)) {
        std::printf("FAIL: unit readBlock(%u)\n", kBlockNum);
        return 1;
    }
    int bad = 0;
    for (size_t i = 0; i < kBlockBytes; ++i) if (after[i] != kBufFill) ++bad;
    if (bad) {
        std::printf("FAIL: cmd=2 did not write — block 10 byte0=%02X (want %02X), "
                    "%d/512 bytes wrong (the BEQ-write-into-read bug)\n",
                    after[0], kBufFill, bad);
        return 1;
    }

    // And the caller's buffer must be intact (a read-instead-of-write would
    // have clobbered $0800 with the disk's block-10 content, 0x0A).
    if (mem.memRead(0x0800) != kBufFill) {
        std::printf("FAIL: caller buffer clobbered: $0800=%02X (want %02X)\n",
                    mem.memRead(0x0800), kBufFill);
        return 1;
    }

    std::printf("OK smartport_write_dispatch (cmd=2 writes block 10)\n");
    return 0;
}
