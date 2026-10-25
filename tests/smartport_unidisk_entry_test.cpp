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

// SmartPort $Cn0A real-hardware driver entry regression test.
//
// The Apple Disk 3.5 / Liron ("Unidisk") firmware exposes its block driver at
// a FIXED $Cn0A. Software that talks to the card directly — rather than via the
// ProDOS $CnFF indirection — hardcodes `JSR $Cn0A` with the ProDOS-style ZP
// param block at $42-$47. French Touch's DIX anthology (`boot_unidisk.a`:
// `modread JSR $C50A`) is the motivating case: before this entry existed,
// `$Cn0A` was an unimplemented $00 = BRK, so DIX's block load hit a BRK storm
// (and, with LC RAM read enabled, stormed the BRK vector out of cold Language-
// Card RAM) — it showed the "FRENCH TOUCH" banner then froze.
//
// This drives the REAL firmware through a 6502: it JSRs $Cn0A with cmd=1 (read)
// and verifies the caller's buffer now holds the requested block — i.e. the
// $Cn0A → $Cn50 redirect reaches the dispatch and the read happened.

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
    const auto p = fs::temp_directory_path() / "pom2_sp_unidisk_entry.po";
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
    constexpr int      kSlot     = 5;
    constexpr uint16_t kBlockNum = 7;
    const uint8_t      kExpect   = static_cast<uint8_t>(0x00 + kBlockNum);  // block fill

    const std::string po = writeSyntheticPo(/*seed=*/0x00);

    Memory mem;
    M6502 cpu(&mem);
    cpu.hardReset();

    auto unit = std::make_unique<pom2::SmartPort35Unit>();
    if (!unit->loadImage(po)) {
        std::printf("FAIL: load .po: %s\n", unit->lastError().c_str());
        return 1;
    }
    auto card = std::make_unique<pom2::SmartPortCard>(kSlot);
    card->setUnit(0, std::move(unit));
    card->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
    mem.slotBus().plug(kSlot, std::move(card));

    // ProDOS-style params (exactly what DIX's boot_unidisk.a stores): read,
    // unit 0, buffer $0800, block 7.
    mem.memWrite(0x42, 0x01);                  // command = READ
    mem.memWrite(0x43, 0x50);                  // unit byte: slot 5, drive 1 (DIX bSLOT)
    mem.memWrite(0x44, 0x00);                  // buffer LO
    mem.memWrite(0x45, 0x08);                  // buffer HI → $0800
    mem.memWrite(0x46, kBlockNum & 0xFF);      // block LO
    mem.memWrite(0x47, 0x00);                  // block HI
    for (int i = 0; i < (int)kBlockBytes; ++i)
        mem.memWrite(0x0800 + i, 0xEE);        // poison the buffer first

    // Caller: JSR $Cn0A (the Unidisk/Liron entry — NOT $Cn50), then park.
    const uint8_t romHi = static_cast<uint8_t>(0xC0 + kSlot);  // $C5
    mem.memWrite(0x0300, 0x20);                // JSR
    mem.memWrite(0x0301, 0x0A);                // $Cn0A lo  ← the real-HW entry
    mem.memWrite(0x0302, romHi);               // $Cn0A hi
    mem.memWrite(0x0303, 0x4C);                // JMP $0303 (park)
    mem.memWrite(0x0304, 0x03);
    mem.memWrite(0x0305, 0x03);

    cpu.setProgramCounter(0x0300);
    cpu.run(60000);

    // The read via $Cn0A must have filled the buffer with block 7's content.
    int bad = 0;
    for (size_t i = 0; i < kBlockBytes; ++i)
        if (mem.memRead(static_cast<uint16_t>(0x0800 + i)) != kExpect) ++bad;
    if (bad) {
        std::printf("FAIL: JSR $Cn0A did not read block %u — $0800=%02X (want %02X), "
                    "%d/512 wrong (the unimplemented $Cn0A=BRK bug)\n",
                    kBlockNum, mem.memRead(0x0800), kExpect, bad);
        return 1;
    }

    std::printf("OK smartport_unidisk_entry ($Cn0A reads block %u)\n", kBlockNum);
    return 0;
}
