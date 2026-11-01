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

// Z80 block-I/O repeat flags pin: INIR / OTIR / INDR / OTDR ($ED B2/B3/BA/BB).
//
// The repeating block-I/O instructions do NOT just leave the per-iteration
// INI/OUTI flag formula in place. On every iteration that still has work to do
// (B != 0 after the decrement), the Z80 re-derives:
//
//   * X/Y  from the high byte of the REWOUND PC (same rule LDIR/CPIR use)
//   * H    from the low nibble of B, when carry is set
//   * P/V  from the parity of B±1 (or B) xored against the incoming P/V
//   * WZ   = PC + 1
//
// [D] MAME calls this `block_io_interrupted_flags()` (`z80.cpp:580-604`),
// invoked by the inir/otir/indr/otdr macros right after `PC -= 2`
// (`z80.lst:769-880`) — but WITHOUT the WZ term: MAME added `WZ = PC + 1` in
// 580447173e (2023-12-03) and dropped it in 0c42667cf0 (2024-06-03).
// [A] Visual Z80 (transistor-level die simulation, hoglet67/Z80Decoder#2) is
// what backs `WZ = PC + 1`; POM2 keeps it against MAME.
//
// Why this needed its own pin: zexdoc and zexall run under CP/M and never
// execute an I/O block instruction, so they cannot see this at all — POM2 used
// the non-repeating formula for the repeating opcodes and both suites stayed
// green. Tom Harte `z80/v1/{ed b2,ed b3,ed ba,ed bb}` failed ~99.5% of 4000
// vectors; the four non-repeating forms (`ed a2/a3/aa/ab`) were already exact.
//
// Vectors below are taken verbatim from that corpus, chosen to span every arm
// of the rule: carry set and clear, bit 7 of the transferred byte set and
// clear, and both H nibble edges ($x0 and $xF).

#include "Z80.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

class FlatBus : public pom2::Z80Bus
{
public:
    uint8_t ram[0x10000]{};
    uint8_t ioValue = 0xFF;          // what an IN reads
    uint8_t lastOutPort = 0, lastOutValue = 0;
    bool    sawOut = false;

    uint8_t z80MemRead(uint16_t a) override { return ram[a]; }
    void    z80MemWrite(uint16_t a, uint8_t v) override { ram[a] = v; }
    uint8_t z80IoRead(uint16_t) override { return ioValue; }
    void    z80IoWrite(uint16_t p, uint8_t v) override
    {
        lastOutPort = static_cast<uint8_t>(p & 0xFF);
        lastOutValue = v; sawOut = true;
    }
};

struct Vec {
    uint8_t  b0, c0, f0, h0, l0;     // initial B C F H L
    uint16_t pc0, wz0;
    uint8_t  io;                     // byte transferred (IN data / mem at HL)
    uint8_t  b1, f1, h1;             // expected final B F H
    uint16_t pc1, wz1;               // expected final PC WZ
};

// `isIn`: true for INIR/INDR (reads a port, writes memory).
void check(const char* label, uint8_t op2, bool isIn, const Vec* v, size_t n)
{
    FlatBus bus;
    pom2::Z80 cpu(bus);
    cpu.reset();

    for (size_t k = 0; k < n; ++k) {
        const Vec& t = v[k];
        std::memset(bus.ram, 0, sizeof(bus.ram));
        bus.ram[t.pc0]     = 0xED;
        bus.ram[t.pc0 + 1] = op2;
        const uint16_t hl = static_cast<uint16_t>((t.h0 << 8) | t.l0);
        if (isIn) bus.ioValue = t.io;
        else      bus.ram[hl] = t.io;
        bus.sawOut = false;

        pom2::Z80::State s{};
        s.b = t.b0; s.c = t.c0; s.f = t.f0; s.h = t.h0; s.l = t.l0;
        s.pc = t.pc0; s.wz = t.wz0;
        cpu.setState(s);

        const int cycles = cpu.step();
        const pom2::Z80::State& g = cpu.getState();

        bool ok = true;
        char why[192] = {0};
        auto bad = [&](const char* what, unsigned got, unsigned want) {
            if (ok) std::snprintf(why, sizeof why, "%s got $%X want $%X", what, got, want);
            ok = false;
        };
        if (g.b != t.b1)  bad("B", g.b, t.b1);
        if (g.f != t.f1)  bad("F", g.f, t.f1);
        if (g.h != t.h1)  bad("H", g.h, t.h1);
        if (g.pc != t.pc1) bad("PC", g.pc, t.pc1);
        if (g.wz != t.wz1) bad("WZ", g.wz, t.wz1);
        // Repeating iteration: 21 T-states (16 + 5 for the extra no-MREQ).
        if (cycles != 21) bad("T-states", static_cast<unsigned>(cycles), 21u);
        if (!isIn && !bus.sawOut) bad("port write", 0, 1);

        if (!ok) {
            ++failures;
            std::printf("  FAIL %s[%zu]: B=$%02X C=$%02X F=$%02X io=$%02X -> %s\n",
                        label, k, t.b0, t.c0, t.f0, t.io, why);
        }
    }
}

// { b0, c0, f0, h0, l0, pc0, wz0, io, b1, f1, h1, pc1, wz1 }
constexpr Vec kInir[] = {
    { 0x6D, 0x9D, 0x74, 0x13, 0xB2, 0x7BD7, 0x538A, 0x76, 0x6C, 0x29, 0x13, 0x7BD7, 0x7BD8 },
    { 0x32, 0x37, 0x1B, 0x42, 0xE9, 0x361D, 0x4F98, 0x9A, 0x31, 0x22, 0x42, 0x361D, 0x361E },
    { 0xC0, 0x8B, 0xC9, 0xAF, 0xBC, 0x1BCF, 0xE172, 0xB7, 0xBF, 0x8B, 0xAF, 0x1BCF, 0x1BD0 },
    { 0x11, 0xA2, 0xF3, 0x1E, 0xC7, 0xBB30, 0x4F66, 0x7E, 0x10, 0x29, 0x1E, 0xBB30, 0xBB31 },
};
constexpr Vec kOtir[] = {
    { 0x49, 0xEA, 0x7D, 0xF6, 0x98, 0x5E36, 0xC16A, 0x1D, 0x48, 0x0C, 0xF6, 0x5E36, 0x5E37 },
    { 0xBA, 0xFA, 0xBE, 0xB4, 0xA3, 0x10EE, 0xF276, 0xC6, 0xB9, 0x87, 0xB4, 0x10EE, 0x10EF },
    { 0x91, 0xBE, 0x5F, 0x16, 0x69, 0x5B52, 0xA448, 0x76, 0x90, 0x8C, 0x16, 0x5B52, 0x5B53 },
    { 0x71, 0x8B, 0x63, 0x60, 0x20, 0xCCDA, 0xC89F, 0xF5, 0x70, 0x1F, 0x60, 0xCCDA, 0xCCDB },
};
constexpr Vec kIndr[] = {
    { 0x62, 0x06, 0xB6, 0xC4, 0xB7, 0x4776, 0xC5F9, 0x51, 0x61, 0x04, 0xC4, 0x4776, 0x4777 },
    { 0xFC, 0xD4, 0x06, 0xBB, 0xB2, 0x9F58, 0x5A78, 0xB7, 0xFB, 0x8B, 0xBB, 0x9F58, 0x9F59 },
    { 0xC0, 0xBD, 0x0C, 0x1D, 0xF9, 0xDD76, 0x18E7, 0xAD, 0xBF, 0x8F, 0x1D, 0xDD76, 0xDD77 },
    { 0xA0, 0x96, 0x82, 0x60, 0xB7, 0x72A3, 0xF434, 0x73, 0x9F, 0xB5, 0x60, 0x72A3, 0x72A4 },
};
constexpr Vec kOtdr[] = {
    { 0xE1, 0xA0, 0x44, 0x12, 0x5C, 0xDD5F, 0xB47B, 0xD5, 0xE0, 0x9F, 0x12, 0xDD5F, 0xDD60 },
    { 0xCF, 0xB8, 0x51, 0xF0, 0x13, 0x52BF, 0xABC0, 0xA2, 0xCE, 0x86, 0xF0, 0x52BF, 0x52C0 },
    { 0xFC, 0x6A, 0xFD, 0xD6, 0xAA, 0x7D6C, 0x2FAC, 0x7D, 0xFB, 0xAD, 0xD6, 0x7D6C, 0x7D6D },
    { 0xE0, 0xB0, 0x70, 0xE4, 0xB7, 0x18CF, 0xFCB9, 0xA4, 0xDF, 0x8F, 0xE4, 0x18CF, 0x18D0 },
};

}  // namespace

int main()
{
    std::puts("=== Z80 block-I/O repeat flags (INIR/OTIR/INDR/OTDR) ===");
    check("INIR", 0xB2, /*isIn=*/true,  kInir, sizeof(kInir) / sizeof(*kInir));
    check("OTIR", 0xB3, /*isIn=*/false, kOtir, sizeof(kOtir) / sizeof(*kOtir));
    check("INDR", 0xBA, /*isIn=*/true,  kIndr, sizeof(kIndr) / sizeof(*kIndr));
    check("OTDR", 0xBB, /*isIn=*/false, kOtdr, sizeof(kOtdr) / sizeof(*kOtdr));

    if (failures) {
        std::printf("z80_block_io_flags_test: FAIL (%d mismatch(es))\n", failures);
        return 1;
    }
    std::puts("z80_block_io_flags_test: OK (16 corpus vectors)");
    return 0;
}
