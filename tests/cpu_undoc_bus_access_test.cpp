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

// Undocumented opcodes that READ still touch the bus — bug hunt #14.
//
// POM2 models the undocumented NMOS opcodes as length- and cycle-correct
// NOPs (no SLO/LAX/... results — see DEV.md § CPU). The BUS CYCLE is a
// separate matter: on silicon every one of the read forms performs a real
// read at its effective address, and on an Apple II that address can be a
// soft switch. `NOP $C030` ($0C) clicks the speaker on a ][ / ][+ /
// unenhanced //e; `LAX $C0EC` ($AF) advances the Disk II data latch;
// `NOP $C083,X` ($1C/$3C/$7C/$DC/$FC) touches the Language Card. POM2 emitted
// NOTHING, which is the same defect class as the missing indexed dummy read
// (bug hunt #8, pinned by cpu_nmos_index_dummy_read) only bigger: that one
// lost a second access, this one lost the only one.
//
// The witness is the speaker toggle counter, exactly as in
// cpu_nmos_index_dummy_read_test, and every case also asserts its cycle
// count so the pin guards the timing the fix must NOT move.
//
// The store forms ($8F/$83/$93 SAX/AHX, $9B/$9C/$9E/$9F TAS/SHY/SHX/AHX) and
// the RMW forms ($x3/$x7/$xB/$xF SLO/RLA/SRE/RRA/DCP/ISC) deliberately stay
// silent: their bus cycle is a WRITE whose value comes from semantics POM2
// does not model, and writing a wrong byte into RAM would be worse than
// writing none. Their silence is asserted below so the boundary is explicit.

#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

struct Run { uint64_t toggles; int cycles; };

// Run `instrs` instructions from $0200 and report the $C030 accesses the CPU
// made (the speaker toggles on ANY access, read or write) plus the cycles.
Run run(M6502::CpuMode mode, std::initializer_list<uint8_t> program, int instrs,
        uint8_t x, uint8_t y)
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    cpu.setCpuMode(mode);
    mem.memWrite(0x30, 0x30);          // ($30) -> $C030
    mem.memWrite(0x31, 0xC0);
    uint16_t a = 0x0200;
    for (uint8_t b : program) mem.memWrite(a++, b);
    cpu.setProgramCounter(0x0200);
    cpu.setXRegister(x);
    cpu.setYRegister(y);
    const uint64_t before = mem.getSpeakerToggleCount();
    int cycles = 0;
    for (int i = 0; i < instrs; ++i) cycles += cpu.run(1);
    return { mem.getSpeakerToggleCount() - before, cycles };
}

int failures = 0;

void check(const char* what, M6502::CpuMode mode,
           std::initializer_list<uint8_t> program, int instrs,
           uint8_t x, uint8_t y, uint64_t wantToggles, int wantCycles)
{
    const Run r = run(mode, program, instrs, x, y);
    const bool ok = (r.toggles == wantToggles) && (r.cycles == wantCycles);
    std::printf("  %-30s %-5s $C030 accesses=%llu (want %llu) cycles=%d (want %d)%s\n",
                what, mode == M6502::CpuMode::NMOS ? "NMOS" : "65C02",
                static_cast<unsigned long long>(r.toggles),
                static_cast<unsigned long long>(wantToggles),
                r.cycles, wantCycles, ok ? "" : "   <-- FAIL");
    if (!ok) ++failures;
    assert(ok && "undocumented read opcode: wrong bus access or cycle count");
}

}  // namespace

int main()
{
    const auto N = M6502::CpuMode::NMOS;
    const auto C = M6502::CpuMode::CMOS;

    std::printf("undocumented READ forms must touch $C030:\n");
    // 3-byte absolute reads. NOP abs ($0C) and LAX abs ($AF) on NMOS.
    check("NOP $C030      ($0C)", N, {0x0C, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    check("LAX $C030      ($AF)", N, {0xAF, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    // NOP abs,X ($1C/$3C/$5C/$7C/$DC/$FC on NMOS) — X=0, no page cross.
    check("NOP $C030,X    ($1C)", N, {0x1C, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    check("NOP $C030,X    ($3C)", N, {0x3C, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    check("NOP $C030,X    ($5C)", N, {0x5C, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    check("NOP $C030,X    ($7C)", N, {0x7C, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    check("NOP $C030,X    ($DC)", N, {0xDC, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    check("NOP $C030,X    ($FC)", N, {0xFC, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    // LAS / LAX abs,Y and LAX (zp),Y / (zp,X).
    check("LAS $C030,Y    ($BB)", N, {0xBB, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    check("LAX $C030,Y    ($BF)", N, {0xBF, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    check("LAX ($30),Y    ($B3)", N, {0xB3, 0x30},       1, 0, 0, 1, 5);
    check("LAX ($30,X)    ($A3)", N, {0xA3, 0x30},       1, 0, 0, 1, 6);

    // The 65C02 keeps $DC/$FC as 3-byte/4-cycle absolute NOPs, and they read
    // their effective address too (W65C02S datasheet table 5-2 / MAME
    // ow65c02.lst nop_c_aba). Same page, same witness.
    check("NOP $C030      ($DC)", C, {0xDC, 0x30, 0xC0}, 1, 0, 0, 1, 4);
    check("NOP $C030      ($FC)", C, {0xFC, 0x30, 0xC0}, 1, 0, 0, 1, 4);

    std::printf("page-crossing indexed reads also pay the un-fixed dummy read:\n");
    // $C0F0 + $40 = $C130: the un-fixed address is $C030 (one access), the
    // fixed one $C130 (slot ROM, no side effect). Same rule as the documented
    // LDA abs,X pinned by cpu_nmos_index_dummy_read.
    check("LDX #$40 : NOP $C0F0,X ($1C)", N, {0xA2, 0x40, 0x1C, 0xF0, 0xC0},
          2, 0, 0, 1, 2 + 5);
    check("LDY #$40 : LAX $C0F0,Y ($BF)", N, {0xA0, 0x40, 0xBF, 0xF0, 0xC0},
          2, 0, 0, 1, 2 + 5);

    std::printf("store / RMW forms stay silent (documented boundary):\n");
    check("SAX $C030      ($8F)", N, {0x8F, 0x30, 0xC0}, 1, 0, 0, 0, 4);
    check("DCP $C030      ($CF)", N, {0xCF, 0x30, 0xC0}, 1, 0, 0, 0, 6);
    check("SHY $C030,X    ($9C)", N, {0x9C, 0x30, 0xC0}, 1, 0, 0, 0, 5);
    check("SLO ($30),Y    ($13)", N, {0x13, 0x30},       1, 0, 0, 0, 8);

    if (failures) {
        std::printf("cpu_undoc_bus_access FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("cpu_undoc_bus_access OK\n");
    return 0;
}
