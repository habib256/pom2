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

// SmartPort slot-ROM driver I/O-error regression test.
//
// The block driver streamed 512 bytes per ProDOS READ/WRITE_BLOCK and then
// returned CLC ("success") unconditionally. So an out-of-range / failed
// block transfer was reported to ProDOS as success — a READ returned a
// garbage (0xFF) buffer with carry clear, and a WRITE silently dropped the
// data, both with no error. The driver now latches an I/O-error status
// ($C0n4 bit 0) when readBlock/writeBlock fails and the ROM read/write
// routines test it, returning carry-set (ProDOS $27) on failure.
//
// This drives the REAL firmware through a 6502: it JSRs $Cn50 with an
// in-range block (expects carry clear) and an out-of-range block (expects
// carry set + A=$27), for both READ (cmd=1) and WRITE (cmd=2). It also
// re-validates the dispatch BEQ-write offset (the WRITE positive control
// actually writes the block).

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
constexpr size_t kBlocks     = 1600;       // 800 K 3.5" → valid blocks 0..1599

std::string writeSyntheticPo() {
    const auto p = fs::temp_directory_path() / "pom2_sp_ioerror.po";
    std::vector<uint8_t> img(kBlocks * kBlockBytes);
    for (size_t b = 0; b < kBlocks; ++b)
        std::memset(img.data() + b * kBlockBytes,
                    static_cast<uint8_t>(b), kBlockBytes);
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp .po");
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return p.string();
}

struct CallResult { uint8_t carry; uint8_t a; };

}  // namespace

int main()
{
    constexpr int     kSlot   = 5;
    constexpr uint8_t kBufFill = 0xC3;
    const uint8_t romHi = static_cast<uint8_t>(0xC0 + kSlot);   // $C5

    const std::string po = writeSyntheticPo();

    Memory mem;
    M6502 cpu(&mem);
    cpu.hardReset();

    auto unit = std::make_unique<pom2::SmartPort35Unit>();
    assert(unit->loadImage(po) && "load .po");
    unit->setWriteBackEnabled(true);          // clears WP so writes pass the precheck
    pom2::SmartPort35Unit* uraw = unit.get();

    auto card = std::make_unique<pom2::SmartPortCard>(kSlot);
    card->setUnit(0, std::move(unit));
    card->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
    mem.slotBus().plug(kSlot, std::move(card));

    // Caller: JSR $Cn50, capture A (error code) and the carry flag.
    //   0300 JSR $Cn50 / 0303 STA $11 / 0305 LDA #$00 / 0307 ROL A
    //   0308 STA $10 / 030A JMP $030A (park)
    mem.memWrite(0x0300, 0x20); mem.memWrite(0x0301, 0x50); mem.memWrite(0x0302, romHi);
    mem.memWrite(0x0303, 0x85); mem.memWrite(0x0304, 0x11);
    mem.memWrite(0x0305, 0xA9); mem.memWrite(0x0306, 0x00);
    mem.memWrite(0x0307, 0x2A);
    mem.memWrite(0x0308, 0x85); mem.memWrite(0x0309, 0x10);
    mem.memWrite(0x030A, 0x4C); mem.memWrite(0x030B, 0x0A); mem.memWrite(0x030C, 0x03);

    auto callDriver = [&](uint8_t cmd, uint16_t block) -> CallResult {
        mem.memWrite(0x42, cmd);
        mem.memWrite(0x43, 0x00);                 // unit byte: drive 0
        mem.memWrite(0x44, 0x00);
        mem.memWrite(0x45, 0x08);                 // buffer → $0800
        mem.memWrite(0x46, block & 0xFF);
        mem.memWrite(0x47, (block >> 8) & 0xFF);
        for (int i = 0; i < (int)kBlockBytes; ++i)
            mem.memWrite(0x0800 + i, kBufFill);   // write payload / read scratch
        mem.memWrite(0x10, 0xEE);                 // poison the capture slots
        mem.memWrite(0x11, 0xEE);
        cpu.setProgramCounter(0x0300);
        cpu.run(60000);
        return { mem.memRead(0x10), mem.memRead(0x11) };
    };

    // (A) READ in-range → success (carry clear).
    {
        CallResult r = callDriver(0x01, 10);
        assert(r.carry == 0 && "in-range read must return CLC");
    }

    // (B) READ out-of-range (block 2000 ≥ 1600) → carry set + ProDOS $27.
    {
        CallResult r = callDriver(0x01, 2000);
        assert(r.carry == 1 && "out-of-range read must return SEC");
        assert(r.a == 0x27 && "out-of-range read must return error $27");
    }

    // (C) WRITE in-range → success (carry clear) AND the block is written.
    {
        CallResult r = callDriver(0x02, 10);
        assert(r.carry == 0 && "in-range write must return CLC");
        uint8_t after[kBlockBytes];
        assert(uraw->readBlock(10, after));
        int bad = 0;
        for (size_t i = 0; i < kBlockBytes; ++i) if (after[i] != kBufFill) ++bad;
        assert(bad == 0 && "in-range write must reach the unit (dispatch offset)");
    }

    // (D) WRITE out-of-range → carry set + ProDOS $27 (no silent success).
    {
        CallResult r = callDriver(0x02, 2000);
        assert(r.carry == 1 && "out-of-range write must return SEC");
        assert(r.a == 0x27 && "out-of-range write must return error $27");
    }

    // (E) FORMAT ($03) through the ProDOS driver (TN ProDOS #16): the page
    // advertises it in $CnFE bit 3 (the real Liron's $BF does too), so it
    // must be answered, not refused with $27 as it was until 2026-10-20.
    assert((mem.memRead(static_cast<uint16_t>((romHi << 8) | 0xFE)) & 0x08) &&
           "$CnFE must advertise FORMAT");
    {
        CallResult r = callDriver(0x03, 0);
        assert(r.carry == 0 && r.a == 0x00 && "FORMAT on a writable disk succeeds");
    }
    {
        auto callUnit = [&](uint8_t unitByte, uint8_t cmd) -> CallResult {
            mem.memWrite(0x42, cmd);
            mem.memWrite(0x43, unitByte);
            mem.memWrite(0x10, 0xEE);
            mem.memWrite(0x11, 0xEE);
            cpu.setProgramCounter(0x0300);
            cpu.run(60000);
            return { mem.memRead(0x10), mem.memRead(0x11) };
        };
        CallResult r = callUnit(0x80, 0x03);
        assert(r.carry == 1 && r.a == 0x28 && "FORMAT on an empty bay: no device");
        uraw->setWriteBackEnabled(false);         // the notch: write-protected
        r = callUnit(0x00, 0x03);
        assert(r.carry == 1 && r.a == 0x2B && "FORMAT on a protected disk: $2B");
        uraw->setWriteBackEnabled(true);
        r = callUnit(0x00, 0x04);
        assert(r.carry == 1 && r.a == 0x27 && "an unknown driver command stays $27");
    }

    fs::remove(po);
    std::printf("OK smartport_io_error (in-range CLC; out-of-range SEC+$27, read & write;"
                " FORMAT answered)\n");
    return 0;
}
