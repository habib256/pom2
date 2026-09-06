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

// 6502/65C02 instruction CYCLE-COUNT test. POM2's Klaus + cmos tests pin
// instruction *results* but not their *cycle counts* — which is exactly how
// the read-modify-write undercount slipped in: INC/DEC absolute charged 5
// cycles instead of 6 (missing the RMW dummy bus cycle), drifting disk
// timing on tight RWTS loops (Mr. Robot 4am boot — found via a MAME
// cycle-trace diff, 2026-05-23). This file gates the RMW timings against
// the canonical NEC/MOS 6502 table (and MAME `om6502.lst` / `ow65c02.lst`).

#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

// Execute exactly one instruction at `at` (regs as-is) and return the
// cycles it consumed. run(1) runs until >=1 cycle elapsed = one opcode.
int oneInstr(M6502& cpu, Memory& mem, std::initializer_list<uint8_t> bytes,
             uint16_t at)
{
    uint16_t a = at;
    for (uint8_t b : bytes) mem.memWrite(a++, b);
    cpu.setProgramCounter(at);
    return cpu.run(1);
}

// Measure a target opcode after first setting X=$04 (for indexed modes),
// so the indexed effective address stays on-page (no spurious page cross).
int withX4(M6502& cpu, Memory& mem, std::initializer_list<uint8_t> target)
{
    mem.memWrite(0x0200, 0xA2);   // LDX #$04
    mem.memWrite(0x0201, 0x04);
    uint16_t a = 0x0202;
    for (uint8_t b : target) mem.memWrite(a++, b);
    cpu.setProgramCounter(0x0200);
    (void)cpu.run(1);             // LDX #$04
    return cpu.run(1);            // target instruction
}

struct Case { const char* name; std::initializer_list<uint8_t> code; int expect; bool idx; };

}  // namespace

int main()
{
    // Every check below also asserts, but asserts vanish under NDEBUG and
    // a build that lost tests/CMakeLists.txt's -UNDEBUG would report a
    // silent pass. Count the failures explicitly and exit non-zero.
    int failures = 0;

    Memory mem;
    mem.setTestMode(true);
    M6502 cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);
    cpu.hardReset();

    // Read-modify-write: zp=5, zp,X=6, abs=6, abs,X=7 for ALL of
    // ASL/LSR/ROL/ROR/INC/DEC. INC/DEC are the ones that were wrong.
    const Case cases[] = {
        // INC ($E6/$F6/$EE/$FE)
        {"INC zp",    {0xE6, 0x40},             5, false},
        {"INC zp,X",  {0xF6, 0x40},             6, true },
        {"INC abs",   {0xEE, 0x00, 0x03},       6, false},
        {"INC abs,X", {0xFE, 0x00, 0x03},       7, true },
        // DEC ($C6/$D6/$CE/$DE)
        {"DEC zp",    {0xC6, 0x40},             5, false},
        {"DEC zp,X",  {0xD6, 0x40},             6, true },
        {"DEC abs",   {0xCE, 0x00, 0x03},       6, false},
        {"DEC abs,X", {0xDE, 0x00, 0x03},       7, true },
        // ASL ($06/$16/$0E/$1E) — already correct; pinned as control.
        {"ASL zp",    {0x06, 0x40},             5, false},
        {"ASL abs",   {0x0E, 0x00, 0x03},       6, false},
        {"ASL abs,X", {0x1E, 0x00, 0x03},       7, true },
        // ROR ($66/$6E) control.
        {"ROR zp",    {0x66, 0x40},             5, false},
        {"ROR abs",   {0x6E, 0x00, 0x03},       6, false},
        // (zp,X) indexed-indirect = 6 cycles (the dummy unindexed-pointer
        // read). Was undercounted at 5 (round 10 #2). withX4 sets X=4 so the
        // zp pointer is $44 — (zp,X) has no page-cross, always 6.
        {"LDA (zp,X)",{0xA1, 0x40},             6, true },
        {"ADC (zp,X)",{0x61, 0x40},             6, true },
        {"STA (zp,X)",{0x81, 0x40},             6, true },
        // Sanity anchors (must stay correct).
        {"LDA #imm",  {0xA9, 0x00},             2, false},
        {"NOP",       {0xEA},                   2, false},
        {"JMP abs",   {0x4C, 0x00, 0x02},       3, false},
        {"LDA abs,X", {0xBD, 0x00, 0x03},       4, true },  // no page cross
    };

    for (const Case& c : cases) {
        const int got = c.idx ? withX4(cpu, mem, c.code)
                              : oneInstr(cpu, mem, c.code, 0x0200);
        if (got != c.expect) {
            std::printf("FAIL %-10s expected %d cycles, got %d\n",
                        c.name, c.expect, got);
            ++failures;
            assert(got == c.expect);
        }
        std::printf("%-10s = %d cycles: OK\n", c.name, got);
    }

    // ── 65C02 INC A / DEC A ($1A/$3A) = 2 cycles ─────────────────────────
    // Implied-accumulator op: fetch (1) + Imp (1), body adds nothing.
    // Regression: these used to add +2, charging 4 cycles each.
    {
        M6502 ccpu(&mem);
        ccpu.setCpuMode(M6502::CpuMode::CMOS);
        ccpu.hardReset();
        mem.memWrite(0x0200, 0x1A);            // INA
        ccpu.setProgramCounter(0x0200);
        const int ina = ccpu.run(1);
        mem.memWrite(0x0200, 0x3A);            // DEA
        ccpu.setProgramCounter(0x0200);
        const int dea = ccpu.run(1);
        if (ina != 2 || dea != 2) {
            std::printf("FAIL INA/DEA: expected 2/2, got %d/%d\n", ina, dea);
            ++failures;
            assert(ina == 2 && dea == 2);
        }
        std::printf("INA = %d, DEA = %d cycles: OK\n", ina, dea);
    }

    // ── 65C02 ASL/LSR/ROL/ROR abs,X = 6 cycles (7 on page-cross) ─────────
    // vs NMOS fixed 7. INC/DEC abs,X stay 7 on both CPUs (round 10 #6).
    {
        M6502 ccpu(&mem);
        ccpu.setCpuMode(M6502::CpuMode::CMOS);
        ccpu.hardReset();
        // X=4: base $0300 + 4 = $0304 (same page) → 6 cycles.
        const int aslNoCross = withX4(ccpu, mem, {0x1E, 0x00, 0x03});
        // base $03FF + 4 = $0403 (page cross) → 7 cycles.
        const int aslCross   = withX4(ccpu, mem, {0x1E, 0xFF, 0x03});
        // ROR abs,X no-cross → 6 (cover another shift op).
        const int rorNoCross = withX4(ccpu, mem, {0x7E, 0x00, 0x03});
        // INC abs,X must STAY 7 on CMOS (the fixed-max RMW path).
        const int incCmos    = withX4(ccpu, mem, {0xFE, 0x00, 0x03});
        if (aslNoCross != 6 || aslCross != 7 || rorNoCross != 6 || incCmos != 7) {
            std::printf("FAIL CMOS abs,X: ASL %d/%d (want 6/7), ROR %d (want 6), "
                        "INC %d (want 7)\n", aslNoCross, aslCross, rorNoCross, incCmos);
            ++failures;
            assert(aslNoCross == 6 && aslCross == 7 && rorNoCross == 6 && incCmos == 7);
        }
        std::printf("CMOS abs,X: ASL %d/%d, ROR %d, INC %d cycles: OK\n",
                    aslNoCross, aslCross, rorNoCross, incCmos);
    }

    // ── 65C02 undocumented-NOP cycle counts ─────────────────────────────
    // The generic Unoff2/Unoff3 handlers charged 3/5 cycles; real 65C02
    // undoc-NOP timings vary by form. Byte counts were already right (no
    // desync) — this pins the cycle totals (imm=2, zp,X=4, abs,X=4; zp=3
    // control; $5C oddball=8 on CMOS, plain NOP abs,X=4 on NMOS).
    {
        M6502 ccpu(&mem);
        ccpu.setCpuMode(M6502::CpuMode::CMOS);
        ccpu.hardReset();
        const int nopImm  = oneInstr(ccpu, mem, {0x02, 0x00},       0x0200);
        const int nopZpX  = oneInstr(ccpu, mem, {0x54, 0x40},       0x0200);
        const int nopAbsX = oneInstr(ccpu, mem, {0xDC, 0x00, 0x03}, 0x0200);
        const int nopZp   = oneInstr(ccpu, mem, {0x44, 0x40},       0x0200);
        const int nop5c   = oneInstr(ccpu, mem, {0x5C, 0x00, 0x03}, 0x0200);
        M6502 n2cpu(&mem);
        n2cpu.setCpuMode(M6502::CpuMode::NMOS);
        n2cpu.hardReset();
        const int nop5cN  = oneInstr(n2cpu, mem, {0x5C, 0x00, 0x03}, 0x0200);
        if (nopImm != 2 || nopZpX != 4 || nopAbsX != 4 || nopZp != 3 ||
            nop5c != 8 || nop5cN != 4) {
            std::printf("FAIL undoc NOP cycles: #imm=%d(want 2) zp,X=%d(want 4) "
                        "abs,X=%d(want 4) zp=%d(want 3) $5C=%d(want 8) "
                        "$5C-NMOS=%d(want 4)\n",
                        nopImm, nopZpX, nopAbsX, nopZp, nop5c, nop5cN);
            ++failures;
            assert(nopImm == 2 && nopZpX == 4 && nopAbsX == 4 && nopZp == 3 &&
                   nop5c == 8 && nop5cN == 4);
        }
        std::printf("undoc NOP cycles: #imm=%d zp,X=%d abs,X=%d zp=%d $5C=%d/%d: OK\n",
                    nopImm, nopZpX, nopAbsX, nopZp, nop5c, nop5cN);
    }

    // ── NMOS-mode remapped undoc-NOP cycle counts (bug-hunt 2026-07-29) ──
    // setCpuMode(NMOS) replaces the 65C02-only opcodes with undoc-NOP
    // stand-ins; the generic Unoff2/Unoff3 (3/5 cyc) contradicted MAME
    // om6502 for the mode-dependent forms: $14/$34/$74 NOP zp,X = 4,
    // $0C NOP abs = 4, $1C/$3C/$7C NOP abs,X = 4 (no page cross), and
    // $80/$89 NOP #imm = 2.
    {
        M6502 ncpu(&mem);
        ncpu.setCpuMode(M6502::CpuMode::NMOS);
        ncpu.hardReset();
        const int n14 = oneInstr(ncpu, mem, {0x14, 0x40},       0x0200);
        const int n74 = oneInstr(ncpu, mem, {0x74, 0x40},       0x0200);
        const int n0c = oneInstr(ncpu, mem, {0x0C, 0x00, 0x03}, 0x0200);
        const int n1c = oneInstr(ncpu, mem, {0x1C, 0x00, 0x03}, 0x0200);
        const int n80 = oneInstr(ncpu, mem, {0x80, 0x00},       0x0200);
        const int n89 = oneInstr(ncpu, mem, {0x89, 0x00},       0x0200);
        if (n14 != 4 || n74 != 4 || n0c != 4 || n1c != 4 ||
            n80 != 2 || n89 != 2) {
            std::printf("FAIL NMOS remapped NOP cycles: $14=%d $74=%d(want 4) "
                        "$0C=%d $1C=%d(want 4) $80=%d $89=%d(want 2)\n",
                        n14, n74, n0c, n1c, n80, n89);
            ++failures;
            assert(n14 == 4 && n74 == 4 && n0c == 4 && n1c == 4 &&
                   n80 == 2 && n89 == 2);
        }
        std::printf("NMOS remapped NOP cycles: zp,X=%d/%d abs=%d abs,X=%d "
                    "#imm=%d/%d: OK\n", n14, n74, n0c, n1c, n80, n89);
    }

    // ── NMOS undoc 2-byte ops consume their operand (no PC desync) ────────
    // $0B/$2B = ANC #imm, $EB = USBC #imm are 2-byte on NMOS. The 65C02 table
    // left them as 1-byte NOPs; in NMOS mode they MUST advance PC by 2 or the
    // operand byte is mis-decoded as the next opcode.
    {
        M6502 ncpu(&mem);
        ncpu.setCpuMode(M6502::CpuMode::NMOS);
        ncpu.hardReset();
        for (uint8_t op : {uint8_t(0x0B), uint8_t(0x2B), uint8_t(0xEB)}) {
            mem.memWrite(0x0200, op);
            mem.memWrite(0x0201, 0x55);    // operand — must be consumed
            ncpu.setProgramCounter(0x0200);
            (void)ncpu.run(1);
            const uint16_t pc = ncpu.getProgramCounter();
            if (pc != 0x0202) {
                std::printf("FAIL NMOS undoc $%02X: PC=$%04X (want $0202 — "
                            "operand not consumed)\n", op, pc);
                ++failures;
                assert(pc == 0x0202);
            }
        }
        std::printf("NMOS undoc 2-byte $0B/$2B/$EB consume operand: OK\n");
    }

    // ── NMOS undoc multi-byte lengths: the FULL set, not just $0B/$2B/$EB ──
    // Same desync class as above (MAME `om6502.lst` implements them all;
    // POM2 models length-correct NOP placeholders):
    //   $x3 column (SLO/RLA/SRE/RRA/SAX/LAX/DCP/ISC (zp,X)/(zp),Y) → 2 bytes
    //   $4B ALR / $6B ARR / $8B XAA / $AB LAX / $CB SBX #imm       → 2 bytes
    //   $1B..$FB abs,Y forms (SLO/RLA/SRE/RRA/TAS/LAS/DCP/ISC)     → 3 bytes
    // $CB/$DB regression: a previous revision mapped them to 1-byte NOPs
    // ("WAI/STP undefined on NMOS") — but on NMOS they are SBX #imm (2) and
    // DCP abs,Y (3).
    {
        M6502 ncpu(&mem);
        ncpu.setCpuMode(M6502::CpuMode::NMOS);
        ncpu.hardReset();
        const uint8_t two[]   = {0x03, 0x73, 0xB3, 0xF3,
                                 0x4B, 0x6B, 0x8B, 0xAB, 0xCB};
        const uint8_t three[] = {0x1B, 0x3B, 0x5B, 0x7B,
                                 0x9B, 0xBB, 0xDB, 0xFB};
        for (uint8_t op : two) {
            mem.memWrite(0x0200, op);
            mem.memWrite(0x0201, 0x55);
            ncpu.setProgramCounter(0x0200);
            (void)ncpu.run(1);
            if (ncpu.getProgramCounter() != 0x0202) {
                std::printf("FAIL NMOS undoc $%02X: PC=$%04X (want $0202)\n",
                            op, ncpu.getProgramCounter());
                ++failures;
                assert(ncpu.getProgramCounter() == 0x0202);
            }
        }
        for (uint8_t op : three) {
            mem.memWrite(0x0200, op);
            mem.memWrite(0x0201, 0x55);
            mem.memWrite(0x0202, 0x02);   // abs,Y target page $02xx (RAM)
            ncpu.setProgramCounter(0x0200);
            (void)ncpu.run(1);
            if (ncpu.getProgramCounter() != 0x0203) {
                std::printf("FAIL NMOS undoc $%02X: PC=$%04X (want $0203)\n",
                            op, ncpu.getProgramCounter());
                ++failures;
                assert(ncpu.getProgramCounter() == 0x0203);
            }
        }
        std::printf("NMOS undoc $x3/$xB column byte lengths: OK\n");
    }

    // ── 1-byte NOP cycle split: 65C02 reserved = 1 cyc, NMOS undoc = 2 ────
    // MAME `ow65c02.lst` (fetch-only `nop_c_imp`) vs `om6502.lst` `nop_imp`.
    // Plus WAI/STP = 3 cycles (not 4) on the 65C02 (`ow65c02.lst`).
    {
        M6502 ccpu(&mem);
        ccpu.setCpuMode(M6502::CpuMode::CMOS);
        ccpu.hardReset();
        const int c03 = oneInstr(ccpu, mem, {0x03}, 0x0200); // reserved $x3
        const int c0b = oneInstr(ccpu, mem, {0x0B}, 0x0200); // reserved $xB
        const int wai = oneInstr(ccpu, mem, {0xCB}, 0x0200); // WAI (no IRQ)
        const int stp = oneInstr(ccpu, mem, {0xDB}, 0x0200); // STP (halts)
        ccpu.hardReset();                                    // clear STP halt
        M6502 ncpu(&mem);
        ncpu.setCpuMode(M6502::CpuMode::NMOS);
        ncpu.hardReset();
        const int n1a = oneInstr(ncpu, mem, {0x1A}, 0x0200); // NMOS NOP imp
        if (c03 != 1 || c0b != 1 || wai != 3 || stp != 3 || n1a != 2) {
            std::printf("FAIL 1-byte NOP/WAI/STP cycles: $03=%d $0B=%d (want 1), "
                        "WAI=%d STP=%d (want 3), NMOS $1A=%d (want 2)\n",
                        c03, c0b, wai, stp, n1a);
            ++failures;
            assert(c03 == 1 && c0b == 1 && wai == 3 && stp == 3 && n1a == 2);
        }
        std::printf("1-byte NOP cycles (C02=1, NMOS=2) + WAI/STP=3: OK\n");
    }

    // ── NMOS decimal-mode SBC: V is deterministic (binary difference) ─────
    // MAME `m6502.cpp` `do_sbc_d` sets F_V from `(A^val)&(A^diff)&0x80`
    // unconditionally — "undefined" is documentation-speak, not silicon.
    // Regression: POM2's NMOS decimal SBC left V STALE from the previous
    // instruction (the decimal ADC branch set it correctly all along).
    {
        M6502 ncpu(&mem);
        ncpu.setCpuMode(M6502::CpuMode::NMOS);
        ncpu.hardReset();
        // Set V first (binary $7F+$01 overflows), then SED/SEC + SBC #$01
        // with A=$00: binary diff $FF → V must come out CLEAR, proving SBC
        // wrote the flag instead of inheriting the stale 1.
        const uint8_t prog1[] = {0xD8, 0x18, 0xA9, 0x7F, 0x69, 0x01,  // CLD CLC LDA ADC → V=1
                                 0xF8, 0x38, 0xA9, 0x00, 0xE9, 0x01}; // SED SEC LDA SBC
        uint16_t a = 0x0200;
        for (uint8_t b : prog1) mem.memWrite(a++, b);
        ncpu.setProgramCounter(0x0200);
        for (int i = 0; i < 4; ++i) (void)ncpu.run(1);   // CLD CLC LDA ADC
        const bool vAfterAdc = (ncpu.getStatusRegister() & 0x40) != 0;
        for (int i = 0; i < 4; ++i) (void)ncpu.run(1);   // SED SEC LDA SBC
        const bool vClear = (ncpu.getStatusRegister() & 0x40) == 0;
        // And the V-set case: A=$80 − $01 (C=1) → binary diff $7F → V=1
        // (starting from V=0 left by the previous SBC).
        const uint8_t prog2[] = {0xF8, 0x38, 0xA9, 0x80, 0xE9, 0x01};
        a = 0x0300;
        for (uint8_t b : prog2) mem.memWrite(a++, b);
        ncpu.setProgramCounter(0x0300);
        for (int i = 0; i < 4; ++i) (void)ncpu.run(1);
        const bool vSet = (ncpu.getStatusRegister() & 0x40) != 0;
        if (!vAfterAdc || !vClear || !vSet) {
            std::printf("FAIL NMOS decimal SBC V: setup V=%d (want 1), "
                        "$00-$01 V-clear=%d (want 1), $80-$01 V-set=%d (want 1)\n",
                        vAfterAdc, vClear, vSet);
            ++failures;
            assert(vAfterAdc && vClear && vSet);
        }
        std::printf("NMOS decimal SBC V flag (binary-difference rule): OK\n");
    }

    // ── Interrupt-entry cycles (IRQ + NMI) = 7, on both NMOS and CMOS ─────
    // POM2 runs the 7-cycle entry sequence AND the first handler
    // instruction in a single step(), so one step charges 7 + (first
    // handler instr). Regression: the 7 entry cycles used to be dropped
    // (executeOpcode reseeds cycles=1 after handleIRQ/handleNMI), charging
    // every interrupt 0 cycles and desyncing cycleCounter-derived clocks.
    {
        Memory imem;
        imem.setTestMode(true);
        M6502 icpu(&imem);
        icpu.setCpuMode(M6502::CpuMode::NMOS);
        icpu.hardReset();

        imem.memWrite(0xFFFE, 0x00);           // IRQ vector → $0400
        imem.memWrite(0xFFFF, 0x04);
        imem.memWrite(0x0400, 0xEA);           // handler: NOP (2 cyc)
        imem.memWrite(0xFFFA, 0x00);           // NMI vector → $0500
        imem.memWrite(0xFFFB, 0x05);
        imem.memWrite(0x0500, 0xEA);           // handler: NOP (2 cyc)

        // CLI to clear I so the IRQ can be taken.
        imem.memWrite(0x0200, 0x58);           // CLI
        imem.memWrite(0x0201, 0xEA);           // NOP (interrupted instr stand-in)
        icpu.setProgramCounter(0x0200);
        (void)icpu.run(1);                     // CLI

        icpu.setIRQ(1);                         // assert IRQ line
        const int irqStep = icpu.run(1);       // entry(7) + handler NOP(2)
        if (irqStep != 9) {
            std::printf("FAIL IRQ entry: expected 9, got %d\n", irqStep);
            ++failures;
            assert(irqStep == 9);
        }
        std::printf("IRQ entry + NOP = %d cycles: OK\n", irqStep);

        // NMI (ignores I, which handleIRQ has now set). PC is at $0401.
        icpu.setProgramCounter(0x0401);
        imem.memWrite(0x0401, 0xEA);
        icpu.setNMI();
        const int nmiStep = icpu.run(1);       // entry(7) + handler NOP(2)
        if (nmiStep != 9) {
            std::printf("FAIL NMI entry: expected 9, got %d\n", nmiStep);
            ++failures;
            assert(nmiStep == 9);
        }
        std::printf("NMI entry + NOP = %d cycles: OK\n", nmiStep);
    }

    if (failures != 0) {
        std::printf("cpu_cycle_count FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("cpu_cycle_count OK\n");
    return 0;
}
