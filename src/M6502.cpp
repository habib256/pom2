// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2000-2026 Verhille Arnaud
// Copyright (C) 2012 John D. Corrado
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#include "M6502.h"
#include "Logger.h"
#include "Memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace {
// Shared switch with Memory.cpp: `POM2_TRACE_IIE_REBOOT=1` arms the
// $FA62 reset-entry trap below (mirrors the IIe paging + auto-INTCXROM
// traces emitted from Memory).
bool iieRebootTraceEnabled()
{
    static const bool e = []() {
        const char* env = std::getenv("POM2_TRACE_IIE_REBOOT");
        return env && env[0] != '\0' && env[0] != '0';
    }();
    return e;
}
}


M6502::M6502()
{
   memory = nullptr;
   statusRegister = 0x24;
   IRQ = 0;
   irqSourceMask = 0;
   NMI = 0;
   // Initialiser tous les registres
   accumulator = 0;
   xRegister = 0;
   yRegister = 0;
   stackPointer = 0xFF;
   programCounter = 0;
   cycles = 0;
   running = 0;
   setCpuMode(CpuMode::CMOS);
}

M6502::M6502(Memory * mem)
{
   statusRegister = 0x24;
   statusRegister |= M6502::Status::I; // Set interrupt disable flag
   IRQ = 0;
   irqSourceMask = 0;
   NMI = 0;
   memory = mem;
   // Initialiser tous les registres
   accumulator = 0;
   xRegister = 0;
   yRegister = 0;
   stackPointer = 0xFF;
   cycles = 0;
   running = 0;
   setCpuMode(CpuMode::CMOS);

   // Initialiser le program counter depuis le vecteur de reset
   // Si la mémoire est disponible, lire le vecteur, sinon utiliser 0xF800 (Monitor Apple II)
   if (memory != nullptr) {
       programCounter = memReadAbsolute(0xFFFC);
   } else {
       programCounter = 0xF800; // Apple II Monitor entry
   }
}

uint16_t M6502::memReadAbsolute(uint16_t adr)
{
  return (memory->memRead(adr) | memory->memRead((unsigned short)(adr + 1)) << 8);
}

void M6502::pushProgramCounter(void)
{
    memory->memWrite((unsigned short)(stackPointer + 0x100), (unsigned char)(programCounter >> 8));
    stackPointer--;
   memory->memWrite((unsigned short)(stackPointer + 0x100), (unsigned char)programCounter);
   stackPointer--;
    cycles += 2;
}

void M6502::popProgramCounter(void)
{
    // Sur le 6502, on push d'abord le high byte, puis le low byte
    // Donc on pop d'abord le low byte, puis le high byte
    stackPointer++;
    uint8_t lowByte = memory->memRead((unsigned short)(stackPointer + 0x100));
    stackPointer++;
    uint8_t highByte = memory->memRead((unsigned short)(stackPointer + 0x100));
    programCounter = lowByte | (highByte << 8);
    cycles += 2;
}

void M6502::handleIRQ(void)
{
    pushProgramCounter();
    memory->memWrite((unsigned short)(0x100 + stackPointer), (unsigned char)((statusRegister & ~0x10) | 0x20));
    stackPointer--;
    statusRegister |= M6502::Status::I;
    // 65C02 clears D on interrupt entry (MAME ow65c02.lst:259 brk_c_imp:
    // `m_P = (m_P | F_I) & ~F_D;`). NMOS leaves D untouched.
    if (cpuMode == CpuMode::CMOS) statusRegister &= ~M6502::Status::D;
    programCounter = memReadAbsolute(0xFFFE);
    cycles += 5;
}

void M6502::handleNMI(void)
{
    pushProgramCounter();
    memory->memWrite((unsigned short)(0x100 + stackPointer), (unsigned char)((statusRegister & ~0x10) | 0x20));
    stackPointer--;
    statusRegister |= M6502::Status::I;
    if (cpuMode == CpuMode::CMOS) statusRegister &= ~M6502::Status::D;
    NMI = 0;
    programCounter = memReadAbsolute(0xFFFA);
    cycles += 5;
}

void M6502::Imp(void)
{
    cycles++;
}

void M6502::Imm(void)
{
    // Mode immédiat : op pointe vers l'adresse de la valeur immédiate
    // Ainsi memRead(op) retournera la valeur immédiate correctement
    op = programCounter++;
}

void M6502::Zero(void)
{
    op = memory->memRead(programCounter++);
    cycles++;
}

void M6502::ZeroX(void)
{
    // zp,X cost = 2 bus cycles (zp fetch + dummy read at the unindexed
    // address before the indexed read). MAME's m6502 tables charge 4
    // cycles for `LDA $zp,X` / 4 for `STZ $zp,X` / 6 for `ASL $zp,X`,
    // which only works if the addressing mode itself contributes 2.
    op = (memory->memRead(programCounter++) + xRegister) & 0xFF;
    cycles += 2;
}

void M6502::ZeroY(void)
{
    // Same dummy-read accounting as ZeroX — used by LDX $zp,Y (4 cycles).
    op = (memory->memRead(programCounter++) + yRegister) & 0xFF;
    cycles += 2;
}

void M6502::Abs(void)
{
    op = memReadAbsolute(programCounter);
    programCounter += 2;
    cycles += 2;
}

void M6502::AbsX(void)
{
    uint16_t base = memory->memRead(programCounter++);
    base |= (uint16_t)memory->memRead(programCounter++) << 8;
    op = base + xRegister;
    cycles += 2;
    if ((base & 0xFF00) != (op & 0xFF00))
        cycles++;
}

void M6502::AbsY(void)
{
    uint16_t base = memory->memRead(programCounter++);
    base |= (uint16_t)memory->memRead(programCounter++) << 8;
    op = base + yRegister;
    cycles += 2;
    if ((base & 0xFF00) != (op & 0xFF00))
        cycles++;
}

void M6502::Ind(void)
{
    // JMP ($abs) indirect. The NMOS 6502 has a famous page-wrap bug:
    // when the pointer's low byte is $FF, the high byte is read from
    // the SAME page rather than the next page. The 65C02 fixes that at
    // the cost of one extra cycle (5 → 6). MAME `om6502.lst:649-656`
    // (jmp_ind, NMOS, buggy) vs `ow65c02.lst:387-395` (jmp_c_ind, CMOS,
    // fixed + extra cycle).
    uint8_t  lo = memory->memRead(programCounter++);
    uint16_t hi = (uint16_t)memory->memRead(programCounter++) << 8;
    uint16_t ptrLo;
    uint16_t ptrHi;
    if (cpuMode == CpuMode::CMOS) {
        ptrLo = (uint16_t)(hi | lo);
        ptrHi = (uint16_t)(ptrLo + 1);  // linear, carries into hi byte
        cycles += 5;                    // CMOS: 6 total with fetch
    } else {
        ptrLo = (uint16_t)(hi | lo);
        ptrHi = (uint16_t)(hi | ((lo + 1) & 0xFF));  // NMOS page-wrap bug
        cycles += 4;                    // NMOS: 5 total with fetch
    }
    op  = memory->memRead(ptrLo);
    op |= (uint16_t)memory->memRead(ptrHi) << 8;
}

void M6502::IndZeroX(void)
{
    uint8_t zp = (memory->memRead(programCounter++) + xRegister) & 0xFF;
    op = memory->memRead(zp);
    op |= (uint16_t)memory->memRead((uint8_t)((zp + 1) & 0xFF)) << 8;
    // 6 cycles total for (zp,X) ops (fetch 1 + this 4 + op 1): the real CPU
    // does a dummy read of the unindexed zero-page pointer before adding X
    // (MAME om6502/ow65c02 `*_idx`). `ZeroX` charges the analogous dummy;
    // this used to add only 3 → a 1-cycle undercount on all 8 (zp,X) ops.
    cycles += 4;
}

// 65C02 zero-page indirect (zp). Same as (zp,X) without the X offset.
void M6502::IndZero(void)
{
    uint8_t zp = memory->memRead(programCounter++);
    op = memory->memRead(zp);
    op |= (uint16_t)memory->memRead((uint8_t)((zp + 1) & 0xFF)) << 8;
    cycles += 3;
}

// 65C02 (abs,X) for JMP. Reads the 16-bit pointer at base+X, no page-wrap
// bug (the 6502 JMP () bug doesn't apply on 65C02). MAME
// `ow65c02.lst:377-386` charges 6 cycles.
void M6502::IndAbsX(void)
{
    uint16_t base = memory->memRead(programCounter++);
    base |= (uint16_t)memory->memRead(programCounter++) << 8;
    base = static_cast<uint16_t>(base + xRegister);
    op = memory->memRead(base);
    op |= (uint16_t)memory->memRead(static_cast<uint16_t>(base + 1)) << 8;
    cycles += 5;  // 6 total with fetch
}

void M6502::IndZeroY(void)
{
    uint8_t zp = memory->memRead(programCounter++);
    uint16_t base = memory->memRead(zp);
    base |= (uint16_t)memory->memRead((uint8_t)((zp + 1) & 0xFF)) << 8;
    op = base + yRegister;
    cycles += 3;
    if ((base & 0xFF00) != (op & 0xFF00))
        cycles++;
}

void M6502::Rel(void)
{
    uint8_t offset = memory->memRead(programCounter++);
    if (offset & 0x80)
        op = (programCounter + offset - 256) & 0xFFFF;
    else
        op = (programCounter + offset) & 0xFFFF;
    cycles++;
}

void M6502::WAbsX(void)
{
    uint16_t base = memory->memRead(programCounter++);
    base |= (uint16_t)memory->memRead(programCounter++) << 8;
    op = base + xRegister;
    cycles += 3;
}

// abs,X for the 65C02 RMW shift/rotate ops (ASL/LSR/ROL/ROR). On the 65C02
// these take 6 cycles, 7 only when the index crosses a page (6502.org: "On
// the 65C02 they take 6 cycles when a page boundary is not crossed"); NMOS
// takes the fixed 7. INC/DEC abs,X always take 7 on BOTH CPUs, so those keep
// the fixed-max WAbsX path.
void M6502::RmwAbsX(void)
{
    uint16_t base = memory->memRead(programCounter++);
    base |= (uint16_t)memory->memRead(programCounter++) << 8;
    op = base + xRegister;
    if (cpuMode == CpuMode::CMOS) {
        cycles += 2;
        if ((base & 0xFF00) != (op & 0xFF00)) cycles++;
    } else {
        cycles += 3;
    }
}

void M6502::WAbsY(void)
{
    uint16_t base = memory->memRead(programCounter++);
    base |= (uint16_t)memory->memRead(programCounter++) << 8;
    op = base + yRegister;
    cycles += 3;
}

void M6502::WIndZeroY(void)
{
    uint8_t zp = memory->memRead(programCounter++);
    uint16_t base = memory->memRead(zp);
    base |= (uint16_t)memory->memRead((uint8_t)((zp + 1) & 0xFF)) << 8;
    op = base + yRegister;
    cycles += 4;
}

void M6502::setStatusRegisterNZ(uint8_t val)
{
    if (val & 0x80)
        statusRegister |= M6502::Status::N;
    else
        statusRegister &= ~M6502::Status::N;

    if (!val)
        statusRegister |= M6502::Status::Z;
    else
        statusRegister &= ~M6502::Status::Z;
}

void M6502::LDA(void)
{
    accumulator = memory->memRead(op);
    setStatusRegisterNZ(accumulator);
    cycles++;
}

void M6502::LDX(void)
{
  xRegister = memory->memRead(op);
    setStatusRegisterNZ(xRegister);
    cycles++;
}

void M6502::LDY(void)
{
    yRegister = memory->memRead(op);
    setStatusRegisterNZ(yRegister);
    cycles++;
}

void M6502::STA(void)
{
memory->memWrite(op, accumulator);
    cycles++;
}

void M6502::STX(void)
{
memory->memWrite(op, xRegister);
    cycles++;
}

void M6502::STY(void)
{
memory->memWrite(op, yRegister);
    cycles++;
}

void M6502::setFlagCarry(int val)
{
    if (val & 0x100)
        statusRegister |= M6502::Status::C;
    else
        statusRegister &= ~M6502::Status::C;
}

void M6502::ADC(void)
{
 uint8_t Op1 = accumulator, Op2 = memory->memRead(op);
    cycles++;
    // Carry-in captured before any flag math overwrites C below — the CMOS
    // decimal-mode V recomputation needs it after C has been clobbered with
    // the BCD carry-out.
    const int carryIn = (statusRegister & M6502::Status::C) ? 1 : 0;

    if (statusRegister & M6502::Status::D)
    {
    if (!((Op1 + Op2 + (statusRegister & M6502::Status::C ? 1 : 0)) & 0xFF))
       statusRegister |= M6502::Status::Z;
     else
    statusRegister &= ~M6502::Status::Z;

   tmp = (Op1 & 0x0F) + (Op2 & 0x0F) + (statusRegister & M6502::Status::C ? 1 : 0);
        // Low-nibble BCD correction. The naive `tmp + 6` overflows into bit 5
        // when the *unadjusted* low sum reaches $1A-$1F (only possible with an
        // invalid BCD operand digit), so `accumulator & 0xF0` below would feed
        // $20 into the high nibble instead of the single $10 carry — inflating
        // the result by $10 (Tom Harte `6502/v1/69` invalid-BCD vectors). Real
        // silicon re-packs the nibble: `((sum + 6) & 0x0F) + 0x10`. For valid
        // BCD (low sum ≤ $13) this is identical to `tmp + 6`, so no behaviour
        // change for real software — only invalid-digit edge cases are fixed.
        accumulator = tmp < 0x0A ? tmp : ((tmp + 6) & 0x0F) + 0x10;
 // BCD low→high carry lives in bit 4 of the adjusted accumulator after the +6.
 // Reading it from `tmp` instead drops the carry whenever the unadjusted sum is in $0A-$0F.
 tmp = (Op1 & 0xF0) + (Op2 & 0xF0) + (accumulator & 0xF0);

        if (tmp & 0x80)
            statusRegister |= M6502::Status::N;
        else
            statusRegister &= ~M6502::Status::N;

 // V flag in BCD mode is undefined on NMOS 6502; this matches real hardware behavior
 if (((Op1 ^ tmp) & ~(Op1 ^ Op2)) & 0x80)
      statusRegister |= M6502::Status::V;
 else
    statusRegister &= ~M6502::Status::V;

        tmp = (accumulator & 0x0F) | (tmp < 0xA0 ? tmp : tmp + 0x60);

        // Decimal carry-out = "corrected sum ≥ $100". The high-nibble +$60
        // BCD fix can push an invalid-BCD sum past $1FF (e.g. $F? + $E? lands
        // the carry in bit 9, not bit 8), so the old `tmp & 0x100` bit-test
        // dropped the carry there. `tmp >= 0x100` is the architectural rule and
        // is identical for valid BCD (corrected sum ≤ $190 < $200). Tom Harte
        // `6502/v1/69` invalid-BCD carry vectors.
        if (tmp >= 0x100)
            statusRegister |= M6502::Status::C;
        else
            statusRegister &= ~M6502::Status::C;

        accumulator = tmp & 0xFF;
        // 65C02 decimal mode: recompute N and Z from the final BCD-
        // adjusted accumulator + add 1 extra cycle (MAME
        // `ow65c02.lst:11-14` `adc_c_aba`: `if(P & F_D) { read_pc();
        // set_nz(m_A); }`). NMOS leaves N/Z from the intermediate
        // binary sum (which is what the code above set).
        if (cpuMode == CpuMode::CMOS) {
            // 65C02 decimal ADC recomputes N and Z from the final adjusted
            // accumulator and burns 1 extra cycle. V, however, is the
            // high-nibble-sum overflow already latched above (line `((Op1 ^
            // tmp)...`) — NOT the binary overflow. Tom Harte `wdc65c02/v1/69`
            // invalid-BCD vectors disagree with the binary-overflow V that an
            // earlier revision forced here, so we leave the high-sum V in place
            // (it is bit-identical for valid BCD).
            setStatusRegisterNZ(accumulator);
            (void)carryIn;
            cycles++;
        }
    }
    else
    {
        tmp = Op1 + Op2 + (statusRegister & M6502::Status::C ? 1 : 0);
        accumulator = tmp & 0xFF;

        if (((Op1 ^ accumulator) & ~(Op1 ^ Op2)) & 0x80)
            statusRegister |= M6502::Status::V;
        else
            statusRegister &= ~M6502::Status::V;

        setFlagCarry(tmp);
        setStatusRegisterNZ(accumulator);
    }
}

void M6502::setFlagBorrow(int val)
{
    if (!(val & 0x100))
        statusRegister |= M6502::Status::C;
    else
        statusRegister &= ~M6502::Status::C;
}

void M6502::SBC(void)
{
uint8_t Op1 = accumulator, Op2 = memory->memRead(op);
    cycles++;

    if (statusRegister & M6502::Status::D)
    {
        const int borrow = (statusRegister & M6502::Status::C) ? 0 : 1;
        if (cpuMode == CpuMode::CMOS) {
            // WDC 65C02 decimal SBC — MAME `w65c02.cpp:28-46` (do_sbc_cd),
            // whose own comment names the difference: "SBC allows interdigit
            // carry from decimal adjustment on 65C02". The CMOS part packs
            // BOTH nibble differences first and only then applies the `-6` /
            // `-$60` decimal adjustments to the whole byte, so a low-nibble
            // `-6` can borrow up into the high nibble. The NMOS part corrects
            // each nibble in isolation and never propagates that borrow —
            // hence the separate branches. Divergence is confined to invalid
            // BCD digits (valid-BCD operands never underflow past -$0A), so
            // no correct-software behaviour changes; it takes
            // `wdc65c02/v1/{e1,e5,e9,ed,f1,f2,f5,f9,fd}` from ~96.5% to 100%.
            const uint8_t al = static_cast<uint8_t>((Op1 & 0x0F) - (Op2 & 0x0F) - borrow);
            const uint8_t ah = static_cast<uint8_t>((Op1 >> 4) - (Op2 >> 4) -
                                                   (static_cast<int8_t>(al) < 0 ? 1 : 0));
            uint8_t res = static_cast<uint8_t>((ah << 4) | (al & 0x0F));
            if (static_cast<int8_t>(al) < 0) res = static_cast<uint8_t>(res - 0x06);
            if (static_cast<int8_t>(ah) < 0) res = static_cast<uint8_t>(res - 0x60);
            accumulator = res;
        } else {
            tmp = (Op1 & 0x0F) - (Op2 & 0x0F) - borrow;
            // Low-nibble BCD borrow correction. The naive `tmp - 6` leaves the
            // low-nibble result un-repacked, so its bit 4 (the borrow that the
            // high-nibble subtraction below reads via `accumulator & 0x10`) lands
            // wrong whenever the unadjusted low difference underflows past -$0A
            // (only reachable with an invalid BCD digit) — dropping the borrow and
            // inflating the result by $10 (Tom Harte `6502/v1/e9` invalid-BCD
            // vectors). Silicon re-packs as `((diff - 6) & 0x0F) - 0x10`, which is
            // bit-identical to `tmp - 6` for valid BCD (low diff ≥ -$0A) — so real
            // software is unaffected; only invalid-digit edge cases are fixed.
            accumulator = !(tmp & 0x10) ? tmp : ((tmp - 6) & 0x0F) - 0x10;
            tmp = (Op1 & 0xF0) - (Op2 & 0xF0) - (accumulator & 0x10);
            accumulator = (accumulator & 0x0F) | (!(tmp & 0x100) ? tmp : tmp - 0x60);
        }
     tmp = Op1 - Op2 - borrow;
        setFlagBorrow(tmp);
        // NMOS: V undefined in BCD; N/Z from binary intermediate
        // `tmp`. CMOS: N/Z recomputed from final adjusted accumulator
        // + V also valid + 1 extra cycle (MAME `ow65c02.lst:11-14`
        // sbc_c_aba mirrors adc_c_aba).
        if (cpuMode == CpuMode::CMOS) {
            setStatusRegisterNZ(accumulator);
            // V on CMOS SBC is the binary-difference overflow (valid in decimal
            // mode just as in binary) — same expression MAME's do_sbc_cd uses,
            // and identical to the C it derives from `diff & 0xff00`. The
            // decimal *result* now follows the WDC interdigit-carry rule (see
            // the CMOS branch above), so `wdc65c02/v1/e9` is silicon-exact
            // like `6502/v1/e9`. See DEV.md § Tom Harte.
            if (((Op1 ^ Op2) & (Op1 ^ (uint8_t)tmp)) & 0x80)
                statusRegister |= M6502::Status::V;
            else
                statusRegister &= ~M6502::Status::V;
            cycles++;
        } else {
            setStatusRegisterNZ((uint8_t)tmp);
            // NMOS decimal SBC: "undefined" V is in fact deterministic —
            // real silicon derives it from the binary difference, exactly
            // like binary mode (MAME `m6502.cpp` `do_sbc_d` sets F_V from
            // `(A^val) & (A^diff) & 0x80` unconditionally). The NMOS
            // decimal ADC branch above already does this; leaving V stale
            // here was an oversight, not a modelling choice.
            if (((Op1 ^ Op2) & (Op1 ^ (uint8_t)tmp)) & 0x80)
                statusRegister |= M6502::Status::V;
            else
                statusRegister &= ~M6502::Status::V;
        }
    }
    else
    {
      tmp = Op1 - Op2 - (statusRegister & M6502::Status::C ? 0 : 1);
        accumulator = tmp & 0xFF;

      if (((Op1 ^ Op2) & (Op1 ^ accumulator)) & 0x80)
            statusRegister |= M6502::Status::V;
       else
            statusRegister &= ~M6502::Status::V;

        setFlagBorrow(tmp);
        setStatusRegisterNZ(accumulator);
    }
}

void M6502::CMP(void)
{
 tmp = accumulator - memory->memRead(op);
    cycles++;
    setFlagBorrow(tmp);
    setStatusRegisterNZ((uint8_t)tmp);
}

void M6502::CPX(void)
{
  tmp = xRegister - memory->memRead(op);
    cycles++;
    setFlagBorrow(tmp);
    setStatusRegisterNZ((uint8_t)tmp);
}

void M6502::CPY(void)
{
    tmp = yRegister - memory->memRead(op);
    cycles++;
    setFlagBorrow(tmp);
    setStatusRegisterNZ((uint8_t)tmp);
}

void M6502::AND(void)
{
 accumulator &= memory->memRead(op);
    cycles++;
    setStatusRegisterNZ(accumulator);
}

void M6502::ORA(void)
{
    accumulator |= memory->memRead(op);
    cycles++;
    setStatusRegisterNZ(accumulator);
}

void M6502::EOR(void)
{
   accumulator ^= memory->memRead(op);
    cycles++;
    setStatusRegisterNZ(accumulator);
}

void M6502::rmwSecondBusCycle(uint16_t addr, uint8_t origValue)
{
    if (cpuMode == CpuMode::CMOS) {
        // CMOS: dummy read of the same address. softSwitchAccess will
        // toggle on the read just like on a write.
        (void)memory->memRead(addr);
    } else {
        // NMOS: explicit write of the original value back to the bus.
        memory->memWrite(addr, origValue);
    }
}

void M6502::ASL(void)
{
    const uint8_t orig = memory->memRead(op);
    // RMW bus pattern (MAME `om6502.lst:161-164` for NMOS,
    // `ow65c02.lst` for CMOS). The third bus cycle is a write of the
    // original on NMOS, a dummy read on CMOS. Both cases dispatch the
    // intermediate cycle through `softSwitchAccess` when `op` is in
    // $C000-$C07F, so `INC $C030` / `ROL $C030` toggle the speaker
    // twice on writes, paddle resets fire twice on $C070, etc.
    rmwSecondBusCycle(op, orig);

    if (orig & 0x80) statusRegister |= M6502::Status::C;
    else             statusRegister &= ~M6502::Status::C;

    uint8_t val = static_cast<uint8_t>(orig << 1);
    setStatusRegisterNZ(val);
    memory->memWrite(op, val);
    cycles += 3;
}

void M6502::ASL_A(void)
{
    tmp = accumulator << 1;
    accumulator = tmp & 0xFF;
    setFlagCarry(tmp);
    setStatusRegisterNZ(accumulator);
}

void M6502::LSR(void)
{
    const uint8_t orig = memory->memRead(op);
    rmwSecondBusCycle(op, orig);

    if (orig & 1) statusRegister |= M6502::Status::C;
    else          statusRegister &= ~M6502::Status::C;

    uint8_t val = static_cast<uint8_t>(orig >> 1);
    setStatusRegisterNZ(val);
    memory->memWrite(op, val);
    cycles += 3;
}

void M6502::LSR_A(void)
{
    if (accumulator & 1)
        statusRegister |= M6502::Status::C;
    else
        statusRegister &= ~M6502::Status::C;

    accumulator >>= 1;
    setStatusRegisterNZ(accumulator);
}

void M6502::ROL(void)
{
    const uint8_t orig = memory->memRead(op);
    rmwSecondBusCycle(op, orig);
    const uint8_t newCarry = orig & 0x80;
    uint8_t val = static_cast<uint8_t>(
        (orig << 1) | (statusRegister & M6502::Status::C ? 1 : 0));

    if (newCarry) statusRegister |= M6502::Status::C;
    else          statusRegister &= ~M6502::Status::C;

    setStatusRegisterNZ(val);
    memory->memWrite(op, val);
    cycles += 3;
}

void M6502::ROL_A(void)
{
    tmp = (accumulator << 1) | (statusRegister & M6502::Status::C ? 1 : 0);
    accumulator = tmp & 0xFF;
    setFlagCarry(tmp);
    setStatusRegisterNZ(accumulator);
}

void M6502::ROR(void)
{
    const uint8_t orig = memory->memRead(op);
    rmwSecondBusCycle(op, orig);
    const int newCarry = orig & 1;
    uint8_t val = static_cast<uint8_t>(
        (orig >> 1) | (statusRegister & M6502::Status::C ? 0x80 : 0));

    if (newCarry) statusRegister |= M6502::Status::C;
    else          statusRegister &= ~M6502::Status::C;

    setStatusRegisterNZ(val);
    memory->memWrite(op, val);
    cycles += 3;
}

void M6502::ROR_A(void)
{
    tmp = accumulator | (statusRegister & M6502::Status::C ? 0x100 : 0);

    if (accumulator & 1)
        statusRegister |= M6502::Status::C;
    else
        statusRegister &= ~M6502::Status::C;

    accumulator = tmp >> 1;
    setStatusRegisterNZ(accumulator);
}

void M6502::INC(void)
{
    const uint8_t orig = memory->memRead(op);
    rmwSecondBusCycle(op, orig);
    uint8_t val = static_cast<uint8_t>(orig + 1);
    setStatusRegisterNZ(val);
    memory->memWrite(op, val);
    cycles += 3;     // RMW: read + dummy (rmwSecondBusCycle) + write — was
                     // 2, undercounting the dummy cycle that ASL/LSR/ROL/
                     // ROR/TSB/TRB already charge. The 1-cycle shortfall
                     // drifted disk timing on tight RWTS loops (Mr. Robot
                     // 4am boot). MAME `om6502.lst` / `ow65c02.lst`: INC
                     // mem = 5/6/6/7 (zp/abs/zp,X/abs,X).
}

void M6502::DEC(void)
{
    const uint8_t orig = memory->memRead(op);
    rmwSecondBusCycle(op, orig);
    uint8_t val = static_cast<uint8_t>(orig - 1);
    setStatusRegisterNZ(val);
    memory->memWrite(op, val);
    cycles += 3;     // RMW: read + dummy + write (see INC) — fixes the
                     // 1-cycle undercount that broke Mr. Robot's boot.
}

void M6502::INX(void)
{
    xRegister++;
    setStatusRegisterNZ(xRegister);
}

void M6502::INY(void)
{
    yRegister++;
    setStatusRegisterNZ(yRegister);
}

void M6502::DEX(void)
{
    xRegister--;
    setStatusRegisterNZ(xRegister);
}

void M6502::DEY(void)
{
    yRegister--;
    setStatusRegisterNZ(yRegister);
}

void M6502::BIT(void)
{
    uint8_t val = memory->memRead(op);

    if (val & 0x40)
        statusRegister |= M6502::Status::V;
    else
        statusRegister &= ~M6502::Status::V;

    if (val & 0x80)
        statusRegister |= M6502::Status::N;
    else
        statusRegister &= ~M6502::Status::N;

    if (!(val & accumulator))
        statusRegister |= M6502::Status::Z;
    else
        statusRegister &= ~M6502::Status::Z;

    cycles++;
}

void M6502::PHA(void)
{
memory->memWrite((uint16_t)(0x100 + stackPointer), accumulator);
    stackPointer--;
    cycles++;
}

void M6502::PHP(void)
{
    // PHP pushes P with the B flag (bit 4) and the "always-1" unused bit
    // (bit 5) both set. These two bits don't physically exist as flags in
    // the CPU — they're synthesised only when the status byte is pushed to
    // the stack by PHP or BRK. IRQ/NMI handlers push bit 5 set and bit 4
    // cleared instead; see handleIRQ / handleNMI.
    memory->memWrite((uint16_t)(0x100 + stackPointer),
                     statusRegister | M6502::Status::B | 0x20);
    stackPointer--;
    cycles++;
}

void M6502::PLA(void)
{
    stackPointer++;
accumulator = memory->memRead((uint16_t)(stackPointer + 0x100));
    setStatusRegisterNZ(accumulator);
    cycles += 2;
}

void M6502::PLP(void)
{
    stackPointer++;
    // MAME om6502.lst:959 + m6502.cpp:408 force U=1 AND B=1 on every P
    // pop (PLP/RTI). The "B" and "U" bits aren't physical flags; the
    // pushed byte carries them as 1 from PHP/BRK and as B=0/U=1 from
    // IRQ/NMI. Without the OR with 0x30, an RTI from IRQ leaves B=0 in
    // the popped status, which a subsequent PHP would then push back —
    // visible to any handler that inspects pushed P.
    statusRegister = memory->memRead((uint16_t)(stackPointer + 0x100)) | 0x30;
    cycles += 2;
}

void M6502::BRK(void)
{
    // Optional diagnostic — dumps CPU state + recent control-flow transfers
    // when BRK fires. Off by default; enable via setDebugBrkTrace(true) from
    // the UI/debug console when you need to trace an unexpected reset loop.
    // programCounter here still points at the byte AFTER the $00 opcode, so
    // the BRK opcode itself is at PC-1.
    if (debugBrkTrace) {
        std::ostringstream oss;
        oss << std::hex << std::uppercase << std::setfill('0');
        oss << "BRK PC=$" << std::setw(4) << static_cast<int>(programCounter - 1)
            << " A=" << std::setw(2) << static_cast<int>(accumulator)
            << " X=" << std::setw(2) << static_cast<int>(xRegister)
            << " Y=" << std::setw(2) << static_cast<int>(yRegister)
            << " SP=" << std::setw(2) << static_cast<int>(stackPointer)
            << " stack(next 6):";
        for (int i = 1; i <= 6; ++i) {
            uint8_t sp = static_cast<uint8_t>(stackPointer + i);
            oss << " " << std::setw(2)
                << static_cast<int>(memory->memRead(0x100 + sp));
        }
        pom2::log().warn("CPU", oss.str());
        dumpPcTrace("BRK trace");
        pom2::log().warn("CPU", "bus state:" + memory->busStateSummary());
    }
    // BRK is a 2-byte instruction: the $00 opcode plus a "signature" byte
    // (officially unused, sometimes used for software vectoring). The CPU
    // already incremented PC once after fetching the opcode, so PC now
    // points at the signature byte. The return address pushed to the stack
    // must skip *past* the signature byte, hence the extra ++ here.
    // Missing this offset makes RTI from a BRK handler return to the
    // signature byte (which the CPU then tries to execute as an opcode).
    programCounter++;
    pushProgramCounter();
    memory->memWrite((uint16_t)(0x100 + stackPointer), statusRegister | M6502::Status::B | 0x20);
    stackPointer--;
    statusRegister |= M6502::Status::I;
    if (cpuMode == CpuMode::CMOS) statusRegister &= ~M6502::Status::D;
    programCounter = memReadAbsolute(0xFFFE);
    // BRK = 7 cycles on both NMOS and CMOS (MAME `om6502.lst` /
    // `ow65c02.lst:234-261`). fetch(1)+Imp(1)+pushPC(2)+body(3) = 7.
    cycles += 3;
}

void M6502::RTI(void)
{
    // RTI = 6 cycles. fetch(1)+Imp(1)+PLP body(2)+popPC body(2) = 6,
    // matching MAME `om6502.lst:1050-1059`. The previous extra
    // `cycles++` over-counted by 1.
    PLP();
    popProgramCounter();
}

void M6502::JMP(void)
{
    programCounter = op;
}

void M6502::RTS(void)
{
    popProgramCounter();
    programCounter++;
    cycles += 2;
}

void M6502::JSR(void)
{
    uint8_t lo = memory->memRead(programCounter++);
    pushProgramCounter();
    programCounter = lo + (memory->memRead(programCounter) << 8);
    cycles += 3;
}

void M6502::branch(void)
{
    cycles++;
    if ((programCounter & 0xFF00) != (op & 0xFF00))
        cycles++;
    programCounter = op;
}

void M6502::BNE(void)
{
    if (!(statusRegister & M6502::Status::Z))
        branch();
}

void M6502::BEQ(void)
{
    if (statusRegister & M6502::Status::Z)
        branch();
}

void M6502::BVC(void)
{
    if (!(statusRegister & M6502::Status::V))
        branch();
}

void M6502::BVS(void)
{
    if (statusRegister & M6502::Status::V)
        branch();
}

void M6502::BCC(void)
{
    if (!(statusRegister & M6502::Status::C))
        branch();
}

void M6502::BCS(void)
{
    if (statusRegister & M6502::Status::C)
        branch();
}

void M6502::BPL(void)
{
    if (!(statusRegister & M6502::Status::N))
        branch();
}

void M6502::BMI(void)
{
    if (statusRegister & M6502::Status::N)
        branch();
}

void M6502::TAX(void)
{
    xRegister = accumulator;
    setStatusRegisterNZ(accumulator);
}

void M6502::TXA(void)
{
    accumulator = xRegister;
    setStatusRegisterNZ(accumulator);
}

void M6502::TAY(void)
{
    yRegister = accumulator;
    setStatusRegisterNZ(accumulator);
}

void M6502::TYA(void)
{
    accumulator = yRegister;
    setStatusRegisterNZ(accumulator);
}

void M6502::TXS(void)
{
    stackPointer = xRegister;
}

void M6502::TSX(void)
{
    xRegister = stackPointer;
    setStatusRegisterNZ(xRegister);
}

void M6502::CLC(void)
{
    statusRegister &= ~M6502::Status::C;
}

void M6502::SEC(void)
{
    statusRegister |= M6502::Status::C;
}

void M6502::CLI(void)
{
    statusRegister &= ~M6502::Status::I;
}

void M6502::SEI(void)
{
    statusRegister |= M6502::Status::I;
}

void M6502::CLV(void)
{
    statusRegister &= ~M6502::Status::V;
}

void M6502::CLD(void)
{
    statusRegister &= ~M6502::Status::D;
}

void M6502::SED(void)
{
    statusRegister |= M6502::Status::D;
}

void M6502::NOP(void)
{
}

// ─── 65C02 additions ──────────────────────────────────────────────────────

void M6502::BRA(void)
{
    // Unconditional branch. Mirrors the cycle-cost rules of any other
    // taken branch (1 extra cycle for the branch, +1 if it crosses a page
    // boundary). The Rel() addressing mode has already computed `op` as
    // the destination address.
    cycles++;
    if ((programCounter & 0xFF00) != (op & 0xFF00)) cycles++;
    programCounter = op;
}

void M6502::STZ(void)
{
    memory->memWrite(op, 0);
    cycles++;
}

// 65C02 INC A / DEC A ($1A/$3A) are 2-cycle implied-accumulator ops:
// the fetch (executeOpcode seeds cycles=1) plus the implied cycle added
// by the Imp addressing handler. Like INX/DEX/INY/DEY, the operation
// body adds NOTHING further. (Previously these added +2, charging 4.)
void M6502::INA(void)
{
    accumulator++;
    setStatusRegisterNZ(accumulator);
}

void M6502::DEA(void)
{
    accumulator--;
    setStatusRegisterNZ(accumulator);
}

void M6502::PHX(void)
{
    memory->memWrite(static_cast<uint16_t>(0x100 + stackPointer), xRegister);
    stackPointer--;
    cycles++;
}

void M6502::PHY(void)
{
    memory->memWrite(static_cast<uint16_t>(0x100 + stackPointer), yRegister);
    stackPointer--;
    cycles++;
}

void M6502::PLX(void)
{
    stackPointer++;
    xRegister = memory->memRead(static_cast<uint16_t>(stackPointer + 0x100));
    setStatusRegisterNZ(xRegister);
    cycles += 2;
}

void M6502::PLY(void)
{
    stackPointer++;
    yRegister = memory->memRead(static_cast<uint16_t>(stackPointer + 0x100));
    setStatusRegisterNZ(yRegister);
    cycles += 2;
}

void M6502::BIT_imm(void)
{
    // BIT #imm (65C02 only). Unlike BIT zp/abs, the immediate variant only
    // affects Z — V and N stay put. (Imm() set op to the PC of the
    // immediate byte, so memRead(op) reads the value.) MAME
    // `ow65c02.lst:210-217` charges 2 cycles; we add 1 here to bring
    // fetch(1)+Imm(0)+BIT_imm(1) to MAME's 2.
    const uint8_t val = memory->memRead(op);
    if (!(val & accumulator)) statusRegister |= M6502::Status::Z;
    else                      statusRegister &= ~M6502::Status::Z;
    cycles += 1;
}

void M6502::TSB(void)
{
    // Test and Set Bits: Z = (mem AND A == 0); mem = mem | A.
    const uint8_t orig = memory->memRead(op);
    rmwSecondBusCycle(op, orig);
    if (!(orig & accumulator)) statusRegister |= M6502::Status::Z;
    else                       statusRegister &= ~M6502::Status::Z;
    memory->memWrite(op, static_cast<uint8_t>(orig | accumulator));
    cycles += 3;     // RMW: read + 2 internal + write
}

void M6502::TRB(void)
{
    // Test and Reset Bits: Z = (mem AND A == 0); mem = mem & ~A.
    const uint8_t orig = memory->memRead(op);
    rmwSecondBusCycle(op, orig);
    if (!(orig & accumulator)) statusRegister |= M6502::Status::Z;
    else                       statusRegister &= ~M6502::Status::Z;
    memory->memWrite(op, static_cast<uint8_t>(orig & ~accumulator));
    cycles += 3;
}

// ── Rockwell SMBn / RMBn / BBRn / BBSn ────────────────────────────────
// Templated on the bit index N (0..7). Each generates a distinct
// member-function instantiation that the dispatch table can target.

// Rockwell zp-bit ops are 5-cycle on real silicon. MAME
// `ow65c02.lst:497-504, 700-707` (RMB/SMB) and `:168-181` (BBR/BBS
// not-taken) all charge 5; BBR/BBS-taken adds +1, +1 more on page cross.
// `executeOpcode` seeds cycles=1 for the opcode fetch, so the bodies
// below add 4 (RMB/SMB) or 4 baseline (BBR/BBS).

template <int N>
void M6502::SMBn(void)
{
    const uint8_t zp = memory->memRead(programCounter++);
    uint8_t v = memory->memRead(zp);
    v |= static_cast<uint8_t>(1u << N);
    memory->memWrite(zp, v);
    cycles += 4;
}

template <int N>
void M6502::RMBn(void)
{
    const uint8_t zp = memory->memRead(programCounter++);
    uint8_t v = memory->memRead(zp);
    v &= static_cast<uint8_t>(~(1u << N));
    memory->memWrite(zp, v);
    cycles += 4;
}

template <int N>
void M6502::BBRn(void)
{
    const uint8_t zp     = memory->memRead(programCounter++);
    const int8_t  offset = static_cast<int8_t>(memory->memRead(programCounter++));
    const uint8_t v      = memory->memRead(zp);
    cycles += 4;
    if ((v & (1u << N)) == 0) {
        const uint16_t old = programCounter;
        programCounter = static_cast<uint16_t>(programCounter + offset);
        cycles += 1;
        if ((old & 0xFF00u) != (programCounter & 0xFF00u)) cycles += 1;
    }
}

template <int N>
void M6502::BBSn(void)
{
    const uint8_t zp     = memory->memRead(programCounter++);
    const int8_t  offset = static_cast<int8_t>(memory->memRead(programCounter++));
    const uint8_t v      = memory->memRead(zp);
    cycles += 4;
    if ((v & (1u << N)) != 0) {
        const uint16_t old = programCounter;
        programCounter = static_cast<uint16_t>(programCounter + offset);
        cycles += 1;
        if ((old & 0xFF00u) != (programCounter & 0xFF00u)) cycles += 1;
    }
}

// Explicit instantiations so the function pointers in opcodeTable resolve.
template void M6502::SMBn<0>(); template void M6502::SMBn<1>();
template void M6502::SMBn<2>(); template void M6502::SMBn<3>();
template void M6502::SMBn<4>(); template void M6502::SMBn<5>();
template void M6502::SMBn<6>(); template void M6502::SMBn<7>();
template void M6502::RMBn<0>(); template void M6502::RMBn<1>();
template void M6502::RMBn<2>(); template void M6502::RMBn<3>();
template void M6502::RMBn<4>(); template void M6502::RMBn<5>();
template void M6502::RMBn<6>(); template void M6502::RMBn<7>();
template void M6502::BBRn<0>(); template void M6502::BBRn<1>();
template void M6502::BBRn<2>(); template void M6502::BBRn<3>();
template void M6502::BBRn<4>(); template void M6502::BBRn<5>();
template void M6502::BBRn<6>(); template void M6502::BBRn<7>();
template void M6502::BBSn<0>(); template void M6502::BBSn<1>();
template void M6502::BBSn<2>(); template void M6502::BBSn<3>();
template void M6502::BBSn<4>(); template void M6502::BBSn<5>();
template void M6502::BBSn<6>(); template void M6502::BBSn<7>();

// WDC WAI / STP. We model the halt by rewinding PC to the WAI/STP
// opcode so the next step() re-fetches it (and the IRQ/NMI handler
// runs first if asserted, breaking the loop).
void M6502::WAI(void)
{
    // WDC `WAI` suspends the CPU until an IRQ or NMI is asserted. Crucial
    // detail (MAME `ow65c02.lst:797-803`): WAI wakes even when I=1 —
    // the wake is unconditional on the interrupt line, only the
    // vectoring step honours the I flag. Concretely, if WAI runs with
    // I=1 and IRQ is then asserted, execution continues at PC+1
    // **without** taking the vector. Some low-power patterns rely on
    // this (poll-and-wait-for-IRQ-with-CLI-deferred).
    //
    // Previously we modelled the halt by decrementing PC so step()
    // re-fetched WAI; that deadlocks with I=1 because step() bails on
    // `!(P & I) && IRQ` and the PC never advances. Removing the decrement
    // makes WAI act as "consume 3 cycles, fall through to PC+1" — the
    // next step() then sees IRQ/NMI (handled at top of step()) and
    // either vectors (NMI / IRQ+I=0) or just executes the byte after
    // WAI (IRQ+I=1). Software relying on WAI as a "wait here forever"
    // is rare on the Apple II target and would equally well use a
    // BNE-to-self loop.
    //
    // 3 cycles total (MAME `ow65c02.lst` WAI: fetch + 2 internal) — the
    // fetch already seeded 1.
    cycles += 2;
}

void M6502::STP(void)
{
    // WDC `STP` ($CB) halts the CPU until a RESET. Per MAME
    // `ow65c02.lst:715-718` this is a `for(;;) { eat_all_cycles; }`
    // loop inside the dispatch handler — only `reset_c` (RESET line)
    // can break out. NMI does **not** wake STP (unlike WAI).
    //
    // POM2 can't run a forever loop inside one instruction (we're
    // cooperative-stepping), so we set a sticky `halted` flag.
    // `step()` short-circuits to a cycle-consuming no-op while the
    // flag is set, and `softReset()` / `hardReset()` clear it.
    halted = true;
    // 3 cycles total for the instruction itself (MAME `ow65c02.lst` STP);
    // the halt then eats cycles via the `halted` short-circuit in step().
    cycles += 2;
}

void M6502::Unoff(void)
{
    // 65C02 1-byte reserved NOPs ($x3 / $xB columns): 1 cycle on real
    // silicon (MAME `m6502/ow65c02.lst` — `nop_c_imp` is fetch-only).
    // cycles is seeded to 1 by the fetch, so add nothing.
}

void M6502::setCpuMode(CpuMode mode)
{
    cpuMode = mode;
    std::memcpy(opcodeTable, kCmosTable, sizeof(opcodeTable));
    if (mode == CpuMode::CMOS) return;

    // NMOS 6502: replace every 65C02-only addition with a placeholder
    // matching the original NMOS behaviour (NOP of correct byte length,
    // or KIL/Hang for the (zp)-mode opcodes that NMOS halts on).
    auto u1 = OpcodeEntry{&M6502::Unoff1, nullptr};   // 1-byte, 2 cyc (NMOS)
    auto u2 = OpcodeEntry{&M6502::Unoff2, nullptr};
    auto u3 = OpcodeEntry{&M6502::Unoff3, nullptr};
    auto kil = OpcodeEntry{&M6502::Hang,  nullptr};

    // 1-byte additions.
    opcodeTable[0x1A] = u1; // INA
    opcodeTable[0x3A] = u1; // DEA
    opcodeTable[0x5A] = u1; // PHY
    opcodeTable[0x7A] = u1; // PLY
    opcodeTable[0xDA] = u1; // PHX
    opcodeTable[0xFA] = u1; // PLX

    // 2-byte additions. On NMOS these decode as undocumented NOPs whose
    // TIMING follows the addressing mode (MAME om6502.lst) — the generic
    // 3-cycle Unoff2 is only right for the zp forms. zp,X costs 4 (same
    // class as $54/$D4/$F4 = UnoffZpX) and #imm costs 2 (same class as
    // $82/$C2/$E2 = UnoffImm); the old flat mapping drifted 1 cycle per
    // instruction in cycle-counted loops (the Mr. Robot RWTS drift class).
    opcodeTable[0x04] = u2;                                     // NOP zp (3)
    opcodeTable[0x14] = OpcodeEntry{&M6502::UnoffZpX, nullptr}; // NOP zp,X (4)
    opcodeTable[0x34] = OpcodeEntry{&M6502::UnoffZpX, nullptr}; // NOP zp,X (4)
    opcodeTable[0x64] = u2;                                     // NOP zp (3)
    opcodeTable[0x74] = OpcodeEntry{&M6502::UnoffZpX, nullptr}; // NOP zp,X (4)
    opcodeTable[0x80] = OpcodeEntry{&M6502::UnoffImm, nullptr}; // NOP #imm (2)
    opcodeTable[0x89] = OpcodeEntry{&M6502::UnoffImm, nullptr}; // NOP #imm (2)

    // (zp)-indirect mode opcodes — NMOS treats these as KIL (halt).
    opcodeTable[0x12] = kil; // ORA (zp)
    opcodeTable[0x32] = kil; // AND (zp)
    opcodeTable[0x52] = kil; // EOR (zp)
    opcodeTable[0x72] = kil; // ADC (zp)
    opcodeTable[0x92] = kil; // STA (zp)
    opcodeTable[0xB2] = kil; // LDA (zp) — the canonical NMOS halt
    opcodeTable[0xD2] = kil; // CMP (zp)
    opcodeTable[0xF2] = kil; // SBC (zp)

    // Reserved $x2 column on NMOS = also KIL (halt). The CMOS table
    // remapped these to 2-byte NOP (the 65C02 reserved them as future
    // expansion; behaviour observed on real silicon = fetch operand,
    // discard). For NMOS we restore the documented halt semantics.
    // Klaus's 6502 functional test doesn't exercise these (skip_nop
    // skips them when the CPU == 6502), but tightly-cracked NMOS-only
    // software occasionally embeds $02/$22/$42/$62 to trip 65C02-port
    // pirates — keep the halt path faithful.
    opcodeTable[0x02] = kil;
    opcodeTable[0x22] = kil;
    opcodeTable[0x42] = kil;
    opcodeTable[0x62] = kil;

    // 3-byte additions. Same rule: NMOS NOP abs ($0C) and NOP abs,X
    // ($1C/$3C/$7C) cost 4 cycles (MAME om6502 — abs,X is 4+p; the +1
    // page-cross penalty is not modelled here, matching the $DC/$FC/$5C
    // treatment). SHY/SHX ($9C/$9E) really are 5 — u3 stays right there.
    opcodeTable[0x0C] = OpcodeEntry{&M6502::UnoffAbs4, nullptr}; // NOP abs (4)
    opcodeTable[0x1C] = OpcodeEntry{&M6502::UnoffAbsX, nullptr}; // NOP abs,X (4+p)
    opcodeTable[0x3C] = OpcodeEntry{&M6502::UnoffAbsX, nullptr}; // NOP abs,X (4+p)
    opcodeTable[0x7C] = OpcodeEntry{&M6502::UnoffAbsX, nullptr}; // NOP abs,X (4+p)
    opcodeTable[0x9C] = u3; // SHY abs,X (5)
    opcodeTable[0x9E] = u3; // SHX abs,Y (5)
    // $5C: the 65C02 8-cycle oddball is CMOS-only; on NMOS it is a plain
    // undocumented NOP abs,X (3 bytes, 4 cycles — MAME om6502.lst).
    opcodeTable[0x5C] = OpcodeEntry{&M6502::UnoffAbsX, nullptr};
    // $DC/$FC are NOP abs,X on BOTH parts, but only NMOS pays the
    // page-cross penalty (the 65C02's are a flat 4 — base table).
    opcodeTable[0xDC] = OpcodeEntry{&M6502::UnoffAbsX, nullptr};
    opcodeTable[0xFC] = OpcodeEntry{&M6502::UnoffAbsX, nullptr};

    // Rockwell SMBn / RMBn (2-byte) and BBRn / BBSn (3-byte).
    for (int n = 0; n < 8; ++n) {
        opcodeTable[0x07 + n * 0x10] = u2; // RMBn zp ($07..$77)
        opcodeTable[0x87 + n * 0x10] = u2; // SMBn zp ($87..$F7)
        opcodeTable[0x0F + n * 0x10] = u3; // BBRn zp,offset
        opcodeTable[0x8F + n * 0x10] = u3; // BBSn zp,offset
    }
    // NMOS undocumented multi-byte opcodes. Full semantics (SLO/RLA/SRE/
    // RRA/SAX/LAX/DCP/ISC, ANC/ALR/ARR/XAA/SBX, TAS/LAS/AHX) are NOT
    // modelled — MAME `m6502/m6502.cpp` (`om6502.lst`) implements them all;
    // nothing in the POM2 corpus needs the results yet. What MUST be right
    // is the byte length, or the instruction stream desyncs (the operand
    // bytes get decoded as opcodes — cracked NMOS loaders embed these).
    //
    //   $x3 column — (zp,X)/(zp),Y RMW combos: all 2-byte on NMOS.
    //   $xB column — ANC/ALR/ARR/XAA/LAX/SBX #imm: 2-byte;
    //                SLO/RLA/SRE/RRA/TAS/LAS/DCP/ISC abs,Y: 3-byte.
    //
    // Note $CB/$DB: WAI/STP exist only on WDC 65C02; on NMOS these are
    // SBX #imm (2-byte) and DCP abs,Y (3-byte) — mapping them to 1-byte
    // NOPs (as a previous revision did) desynced the stream.
    auto uimm = OpcodeEntry{&M6502::UnoffImm, nullptr};
    for (int hi = 0; hi < 16; ++hi)
        opcodeTable[hi * 0x10 + 0x03] = u2;                 // $x3: 2-byte
    opcodeTable[0x0B] = uimm; // ANC #imm
    opcodeTable[0x2B] = uimm; // ANC #imm
    opcodeTable[0x4B] = uimm; // ALR #imm
    opcodeTable[0x6B] = uimm; // ARR #imm
    opcodeTable[0x8B] = uimm; // XAA #imm
    opcodeTable[0xAB] = uimm; // LAX #imm
    opcodeTable[0xCB] = uimm; // SBX #imm (65C02 WAI lives here)
    opcodeTable[0xEB] = uimm; // USBC #imm
    opcodeTable[0x1B] = u3;   // SLO abs,Y
    opcodeTable[0x3B] = u3;   // RLA abs,Y
    opcodeTable[0x5B] = u3;   // SRE abs,Y
    opcodeTable[0x7B] = u3;   // RRA abs,Y
    opcodeTable[0x9B] = u3;   // TAS abs,Y
    opcodeTable[0xBB] = u3;   // LAS abs,Y
    opcodeTable[0xDB] = u3;   // DCP abs,Y (65C02 STP lives here)
    opcodeTable[0xFB] = u3;   // ISC abs,Y
}

void M6502::Unoff1(void)
{
    // NMOS 1-byte undocumented NOPs ($1A/$3A/$5A/$7A/$DA/$FA): 2 cycles
    // (MAME `m6502/om6502.lst` `nop_imp`). Distinct from the 65C02
    // 1-cycle reserved NOPs (Unoff).
    cycles += 1;
}

void M6502::Unoff2(void)
{
    programCounter++;
    cycles += 2;
}

void M6502::Unoff3(void)
{
    programCounter += 2;
    cycles += 4;
}

// Cycle-accurate undocumented-NOP variants (cycles seeded to 1 for the fetch
// in executeOpcode, so these add total-1). The byte counts were already
// correct via Unoff2/Unoff3; these only fix the cycle totals for the few
// undoc opcodes whose timing differs from the generic 2-byte-3 / 3-byte-5.
void M6502::UnoffImm(void)   // 2 bytes, 2 cycles: 65C02 NOP #imm, NMOS DOP/ANC/SBC #imm
{
    programCounter++;
    cycles += 1;
}
void M6502::UnoffZpX(void)   // 2 bytes, 4 cycles: NOP zp,X ($54/$D4/$F4)
{
    programCounter++;
    cycles += 3;
}
void M6502::UnoffAbs4(void)  // 3 bytes, 4 cycles: NOP abs ($0C, NMOS TOP)
{
    programCounter += 2;
    cycles += 3;
}
void M6502::UnoffAbsX(void)  // 3 bytes, 4+p cycles: NOP abs,X (NMOS TOP)
{
    // MAME om6502 nop_abx: the undocumented NOP abs,X still performs the
    // indexed read, so it pays the +1 page-cross penalty like every other
    // abs,X read. POM2 charged a flat 4 for $1C/$3C/$5C/$7C/$DC/$FC,
    // drifting one cycle whenever the index crossed a page — the same
    // drift class as the Mr. Robot RWTS bug.
    uint16_t base = memory->memRead(programCounter++);
    base |= static_cast<uint16_t>(memory->memRead(programCounter++)) << 8;
    const uint16_t ea = static_cast<uint16_t>(base + xRegister);
    cycles += 3;
    if ((base & 0xFF00) != (ea & 0xFF00)) cycles++;
}
void M6502::Unoff5C(void)    // 3 bytes, 8 cycles: the 65C02 oddball $5C
{
    // $5C on the 65C02 fetches its operand, then burns 5 more cycles
    // reading $FFxx (MAME `ow65c02.lst` nop_5c — 8 cycles total). The
    // dummy bus reads are not replayed; only the length/timing matter.
    programCounter += 2;
    cycles += 7;
}

void M6502::Hang(void)
{
    // NMOS KIL/JAM: the sequencer stops dead — the address bus freezes
    // and NOTHING but RESET recovers, not even NMI. Re-pointing PC at
    // the opcode (the old model) left the CPU executing a 2-cycle loop
    // that step() still interrupted, so an IRQ-driven program could walk
    // out of a jam that real silicon never releases. Route through the
    // same `halted` latch STP uses: step() short-circuits BEFORE the
    // IRQ/NMI poll, and soft/hardReset clear it (the latch is also
    // snapshotted, so a rewind out of a jam works).
    programCounter--;
    cycles += 2;
    halted = true;
}

// Master 65C02 dispatch table: each entry is {addressingMode, operation}.
// For single-function opcodes (JSR, Hang, Unoff*), operation is nullptr.
// The instance `opcodeTable` is initialised from this and may be mutated
// by `setCpuMode(CpuMode::NMOS)` to remove 65C02 additions.
const M6502::OpcodeEntry M6502::kCmosTable[256] = {
    /* 0x00 */ {&M6502::Imp,      &M6502::BRK},
    /* 0x01 */ {&M6502::IndZeroX,  &M6502::ORA},
    /* 0x02 */ {&M6502::UnoffImm,  nullptr},          // 65C02: NOP imm (2 bytes, 2 cyc)
                                                       // NMOS:  KIL — overridden in setCpuMode(NMOS)
    /* 0x03 */ {&M6502::Unoff,     nullptr},
    /* 0x04 */ {&M6502::Zero,      &M6502::TSB},     // 65C02 TSB zp
    /* 0x05 */ {&M6502::Zero,      &M6502::ORA},
    /* 0x06 */ {&M6502::Zero,      &M6502::ASL},
    /* 0x07 */ {&M6502::RMBn<0>,   nullptr},
    /* 0x08 */ {&M6502::Imp,       &M6502::PHP},
    /* 0x09 */ {&M6502::Imm,       &M6502::ORA},
    /* 0x0A */ {&M6502::Imp,       &M6502::ASL_A},
    /* 0x0B */ {&M6502::Unoff,     nullptr},          // 65C02: NOP 1B; NMOS: undoc ANC #imm
    /* 0x0C */ {&M6502::Abs,       &M6502::TSB},     // 65C02 TSB abs
    /* 0x0D */ {&M6502::Abs,       &M6502::ORA},
    /* 0x0E */ {&M6502::Abs,       &M6502::ASL},
    /* 0x0F */ {&M6502::BBRn<0>,   nullptr},

    /* 0x10 */ {&M6502::Rel,       &M6502::BPL},
    /* 0x11 */ {&M6502::IndZeroY,  &M6502::ORA},
    /* 0x12 */ {&M6502::IndZero,   &M6502::ORA},     // 65C02 ORA (zp)
    /* 0x13 */ {&M6502::Unoff,     nullptr},
    /* 0x14 */ {&M6502::Zero,      &M6502::TRB},     // 65C02 TRB zp
    /* 0x15 */ {&M6502::ZeroX,     &M6502::ORA},
    /* 0x16 */ {&M6502::ZeroX,     &M6502::ASL},
    /* 0x17 */ {&M6502::RMBn<1>,   nullptr},
    /* 0x18 */ {&M6502::Imp,       &M6502::CLC},
    /* 0x19 */ {&M6502::AbsY,      &M6502::ORA},
    /* 0x1A */ {&M6502::Imp,       &M6502::INA},     // 65C02 INA
    /* 0x1B */ {&M6502::Unoff,     nullptr},
    /* 0x1C */ {&M6502::Abs,       &M6502::TRB},     // 65C02 TRB abs
    /* 0x1D */ {&M6502::AbsX,      &M6502::ORA},
    /* 0x1E */ {&M6502::RmwAbsX,   &M6502::ASL},
    /* 0x1F */ {&M6502::BBRn<1>,   nullptr},

    /* 0x20 */ {&M6502::JSR,       nullptr},
    /* 0x21 */ {&M6502::IndZeroX,  &M6502::AND},
    /* 0x22 */ {&M6502::UnoffImm,  nullptr},          // 65C02: NOP imm; NMOS: KIL
    /* 0x23 */ {&M6502::Unoff,     nullptr},
    /* 0x24 */ {&M6502::Zero,      &M6502::BIT},
    /* 0x25 */ {&M6502::Zero,      &M6502::AND},
    /* 0x26 */ {&M6502::Zero,      &M6502::ROL},
    /* 0x27 */ {&M6502::RMBn<2>,   nullptr},
    /* 0x28 */ {&M6502::Imp,       &M6502::PLP},
    /* 0x29 */ {&M6502::Imm,       &M6502::AND},
    /* 0x2A */ {&M6502::Imp,       &M6502::ROL_A},
    /* 0x2B */ {&M6502::Unoff,     nullptr},          // 65C02: NOP 1B; NMOS: undoc ANC #imm
    /* 0x2C */ {&M6502::Abs,       &M6502::BIT},
    /* 0x2D */ {&M6502::Abs,       &M6502::AND},
    /* 0x2E */ {&M6502::Abs,       &M6502::ROL},
    /* 0x2F */ {&M6502::BBRn<2>,   nullptr},

    /* 0x30 */ {&M6502::Rel,       &M6502::BMI},
    /* 0x31 */ {&M6502::IndZeroY,  &M6502::AND},
    /* 0x32 */ {&M6502::IndZero,   &M6502::AND},     // 65C02 AND (zp)
    /* 0x33 */ {&M6502::Unoff,     nullptr},
    /* 0x34 */ {&M6502::ZeroX,     &M6502::BIT},     // 65C02 BIT zp,X
    /* 0x35 */ {&M6502::ZeroX,     &M6502::AND},
    /* 0x36 */ {&M6502::ZeroX,     &M6502::ROL},
    /* 0x37 */ {&M6502::RMBn<3>,   nullptr},
    /* 0x38 */ {&M6502::Imp,       &M6502::SEC},
    /* 0x39 */ {&M6502::AbsY,      &M6502::AND},
    /* 0x3A */ {&M6502::Imp,       &M6502::DEA},     // 65C02 DEA
    /* 0x3B */ {&M6502::Unoff,     nullptr},
    /* 0x3C */ {&M6502::AbsX,      &M6502::BIT},     // 65C02 BIT abs,X
    /* 0x3D */ {&M6502::AbsX,      &M6502::AND},
    /* 0x3E */ {&M6502::RmwAbsX,   &M6502::ROL},
    /* 0x3F */ {&M6502::BBRn<3>,   nullptr},

    /* 0x40 */ {&M6502::Imp,       &M6502::RTI},
    /* 0x41 */ {&M6502::IndZeroX,  &M6502::EOR},
    /* 0x42 */ {&M6502::UnoffImm,  nullptr},          // 65C02: NOP imm; NMOS: KIL
    /* 0x43 */ {&M6502::Unoff,     nullptr},
    /* 0x44 */ {&M6502::Unoff2,    nullptr},
    /* 0x45 */ {&M6502::Zero,      &M6502::EOR},
    /* 0x46 */ {&M6502::Zero,      &M6502::LSR},
    /* 0x47 */ {&M6502::RMBn<4>,   nullptr},
    /* 0x48 */ {&M6502::Imp,       &M6502::PHA},
    /* 0x49 */ {&M6502::Imm,       &M6502::EOR},
    /* 0x4A */ {&M6502::Imp,       &M6502::LSR_A},
    /* 0x4B */ {&M6502::Unoff,     nullptr},
    /* 0x4C */ {&M6502::Abs,       &M6502::JMP},
    /* 0x4D */ {&M6502::Abs,       &M6502::EOR},
    /* 0x4E */ {&M6502::Abs,       &M6502::LSR},
    /* 0x4F */ {&M6502::BBRn<4>,   nullptr},

    /* 0x50 */ {&M6502::Rel,       &M6502::BVC},
    /* 0x51 */ {&M6502::IndZeroY,  &M6502::EOR},
    /* 0x52 */ {&M6502::IndZero,   &M6502::EOR},     // 65C02 EOR (zp)
    /* 0x53 */ {&M6502::Unoff,     nullptr},
    /* 0x54 */ {&M6502::UnoffZpX,  nullptr},          // NOP zp,X (2 bytes, 4 cyc)
    /* 0x55 */ {&M6502::ZeroX,     &M6502::EOR},
    /* 0x56 */ {&M6502::ZeroX,     &M6502::LSR},
    /* 0x57 */ {&M6502::RMBn<5>,   nullptr},
    /* 0x58 */ {&M6502::Imp,       &M6502::CLI},
    /* 0x59 */ {&M6502::AbsY,      &M6502::EOR},
    /* 0x5A */ {&M6502::Imp,       &M6502::PHY},     // 65C02 PHY
    /* 0x5B */ {&M6502::Unoff,     nullptr},
    /* 0x5C */ {&M6502::Unoff5C,   nullptr},          // 65C02 oddball: 3 bytes, 8 cyc
    /* 0x5D */ {&M6502::AbsX,      &M6502::EOR},
    /* 0x5E */ {&M6502::RmwAbsX,   &M6502::LSR},
    /* 0x5F */ {&M6502::BBRn<5>,   nullptr},

    /* 0x60 */ {&M6502::Imp,       &M6502::RTS},
    /* 0x61 */ {&M6502::IndZeroX,  &M6502::ADC},
    /* 0x62 */ {&M6502::UnoffImm,  nullptr},          // 65C02: NOP imm; NMOS: KIL
    /* 0x63 */ {&M6502::Unoff,     nullptr},
    /* 0x64 */ {&M6502::Zero,      &M6502::STZ},     // 65C02 STZ zp
    /* 0x65 */ {&M6502::Zero,      &M6502::ADC},
    /* 0x66 */ {&M6502::Zero,      &M6502::ROR},
    /* 0x67 */ {&M6502::RMBn<6>,   nullptr},
    /* 0x68 */ {&M6502::Imp,       &M6502::PLA},
    /* 0x69 */ {&M6502::Imm,       &M6502::ADC},
    /* 0x6A */ {&M6502::Imp,       &M6502::ROR_A},
    /* 0x6B */ {&M6502::Unoff,     nullptr},
    /* 0x6C */ {&M6502::Ind,       &M6502::JMP},
    /* 0x6D */ {&M6502::Abs,       &M6502::ADC},
    /* 0x6E */ {&M6502::Abs,       &M6502::ROR},
    /* 0x6F */ {&M6502::BBRn<6>,   nullptr},

    /* 0x70 */ {&M6502::Rel,       &M6502::BVS},
    /* 0x71 */ {&M6502::IndZeroY,  &M6502::ADC},
    /* 0x72 */ {&M6502::IndZero,   &M6502::ADC},     // 65C02 ADC (zp)
    /* 0x73 */ {&M6502::Unoff,     nullptr},
    /* 0x74 */ {&M6502::ZeroX,     &M6502::STZ},     // 65C02 STZ zp,X
    /* 0x75 */ {&M6502::ZeroX,     &M6502::ADC},
    /* 0x76 */ {&M6502::ZeroX,     &M6502::ROR},
    /* 0x77 */ {&M6502::RMBn<7>,   nullptr},
    /* 0x78 */ {&M6502::Imp,       &M6502::SEI},
    /* 0x79 */ {&M6502::AbsY,      &M6502::ADC},
    /* 0x7A */ {&M6502::Imp,       &M6502::PLY},     // 65C02 PLY
    /* 0x7B */ {&M6502::Unoff,     nullptr},
    /* 0x7C */ {&M6502::IndAbsX,   &M6502::JMP},     // 65C02 JMP (abs,X)
    /* 0x7D */ {&M6502::AbsX,      &M6502::ADC},
    /* 0x7E */ {&M6502::RmwAbsX,   &M6502::ROR},
    /* 0x7F */ {&M6502::BBRn<7>,   nullptr},

    /* 0x80 */ {&M6502::Rel,       &M6502::BRA},     // 65C02 BRA
    /* 0x81 */ {&M6502::IndZeroX,  &M6502::STA},
    /* 0x82 */ {&M6502::UnoffImm,  nullptr},          // 65C02: NOP imm; NMOS: DOP #imm
    /* 0x83 */ {&M6502::Unoff,     nullptr},
    /* 0x84 */ {&M6502::Zero,      &M6502::STY},
    /* 0x85 */ {&M6502::Zero,      &M6502::STA},
    /* 0x86 */ {&M6502::Zero,      &M6502::STX},
    /* 0x87 */ {&M6502::SMBn<0>,   nullptr},
    /* 0x88 */ {&M6502::Imp,       &M6502::DEY},
    /* 0x89 */ {&M6502::Imm,       &M6502::BIT_imm}, // 65C02 BIT #imm
    /* 0x8A */ {&M6502::Imp,       &M6502::TXA},
    /* 0x8B */ {&M6502::Unoff,     nullptr},
    /* 0x8C */ {&M6502::Abs,       &M6502::STY},
    /* 0x8D */ {&M6502::Abs,       &M6502::STA},
    /* 0x8E */ {&M6502::Abs,       &M6502::STX},
    /* 0x8F */ {&M6502::BBSn<0>,   nullptr},

    /* 0x90 */ {&M6502::Rel,       &M6502::BCC},
    /* 0x91 */ {&M6502::WIndZeroY, &M6502::STA},
    /* 0x92 */ {&M6502::IndZero,   &M6502::STA},     // 65C02 STA (zp)
    /* 0x93 */ {&M6502::Unoff,     nullptr},
    /* 0x94 */ {&M6502::ZeroX,     &M6502::STY},
    /* 0x95 */ {&M6502::ZeroX,     &M6502::STA},
    /* 0x96 */ {&M6502::ZeroY,     &M6502::STX},
    /* 0x97 */ {&M6502::SMBn<1>,   nullptr},
    /* 0x98 */ {&M6502::Imp,       &M6502::TYA},
    /* 0x99 */ {&M6502::WAbsY,     &M6502::STA},
    /* 0x9A */ {&M6502::Imp,       &M6502::TXS},
    /* 0x9B */ {&M6502::Unoff,     nullptr},
    /* 0x9C */ {&M6502::Abs,       &M6502::STZ},     // 65C02 STZ abs
    /* 0x9D */ {&M6502::WAbsX,     &M6502::STA},
    /* 0x9E */ {&M6502::WAbsX,     &M6502::STZ},     // 65C02 STZ abs,X
    /* 0x9F */ {&M6502::BBSn<1>,   nullptr},

    /* 0xA0 */ {&M6502::Imm,       &M6502::LDY},
    /* 0xA1 */ {&M6502::IndZeroX,  &M6502::LDA},
    /* 0xA2 */ {&M6502::Imm,       &M6502::LDX},
    /* 0xA3 */ {&M6502::Unoff,     nullptr},
    /* 0xA4 */ {&M6502::Zero,      &M6502::LDY},
    /* 0xA5 */ {&M6502::Zero,      &M6502::LDA},
    /* 0xA6 */ {&M6502::Zero,      &M6502::LDX},
    /* 0xA7 */ {&M6502::SMBn<2>,   nullptr},
    /* 0xA8 */ {&M6502::Imp,       &M6502::TAY},
    /* 0xA9 */ {&M6502::Imm,       &M6502::LDA},
    /* 0xAA */ {&M6502::Imp,       &M6502::TAX},
    /* 0xAB */ {&M6502::Unoff,     nullptr},
    /* 0xAC */ {&M6502::Abs,       &M6502::LDY},
    /* 0xAD */ {&M6502::Abs,       &M6502::LDA},
    /* 0xAE */ {&M6502::Abs,       &M6502::LDX},
    /* 0xAF */ {&M6502::BBSn<2>,   nullptr},

    /* 0xB0 */ {&M6502::Rel,       &M6502::BCS},
    /* 0xB1 */ {&M6502::IndZeroY,  &M6502::LDA},
    /* 0xB2 */ {&M6502::IndZero,   &M6502::LDA},     // 65C02 LDA (zp)
    /* 0xB3 */ {&M6502::Unoff,     nullptr},
    /* 0xB4 */ {&M6502::ZeroX,     &M6502::LDY},
    /* 0xB5 */ {&M6502::ZeroX,     &M6502::LDA},
    /* 0xB6 */ {&M6502::ZeroY,     &M6502::LDX},
    /* 0xB7 */ {&M6502::SMBn<3>,   nullptr},
    /* 0xB8 */ {&M6502::Imp,       &M6502::CLV},
    /* 0xB9 */ {&M6502::AbsY,      &M6502::LDA},
    /* 0xBA */ {&M6502::Imp,       &M6502::TSX},
    /* 0xBB */ {&M6502::Unoff,     nullptr},
    /* 0xBC */ {&M6502::AbsX,      &M6502::LDY},
    /* 0xBD */ {&M6502::AbsX,      &M6502::LDA},
    /* 0xBE */ {&M6502::AbsY,      &M6502::LDX},
    /* 0xBF */ {&M6502::BBSn<3>,   nullptr},

    /* 0xC0 */ {&M6502::Imm,       &M6502::CPY},
    /* 0xC1 */ {&M6502::IndZeroX,  &M6502::CMP},
    /* 0xC2 */ {&M6502::UnoffImm,  nullptr},          // 65C02: NOP imm; NMOS: DOP #imm
    /* 0xC3 */ {&M6502::Unoff,     nullptr},
    /* 0xC4 */ {&M6502::Zero,      &M6502::CPY},
    /* 0xC5 */ {&M6502::Zero,      &M6502::CMP},
    /* 0xC6 */ {&M6502::Zero,      &M6502::DEC},
    /* 0xC7 */ {&M6502::SMBn<4>,   nullptr},
    /* 0xC8 */ {&M6502::Imp,       &M6502::INY},
    /* 0xC9 */ {&M6502::Imm,       &M6502::CMP},
    /* 0xCA */ {&M6502::Imp,       &M6502::DEX},
    /* 0xCB */ {&M6502::WAI,       nullptr},
    /* 0xCC */ {&M6502::Abs,       &M6502::CPY},
    /* 0xCD */ {&M6502::Abs,       &M6502::CMP},
    /* 0xCE */ {&M6502::Abs,       &M6502::DEC},
    /* 0xCF */ {&M6502::BBSn<4>,   nullptr},

    /* 0xD0 */ {&M6502::Rel,       &M6502::BNE},
    /* 0xD1 */ {&M6502::IndZeroY,  &M6502::CMP},
    /* 0xD2 */ {&M6502::IndZero,   &M6502::CMP},     // 65C02 CMP (zp)
    /* 0xD3 */ {&M6502::Unoff,     nullptr},
    /* 0xD4 */ {&M6502::UnoffZpX,  nullptr},          // NOP zp,X (2 bytes, 4 cyc)
    /* 0xD5 */ {&M6502::ZeroX,     &M6502::CMP},
    /* 0xD6 */ {&M6502::ZeroX,     &M6502::DEC},
    /* 0xD7 */ {&M6502::SMBn<5>,   nullptr},
    /* 0xD8 */ {&M6502::Imp,       &M6502::CLD},
    /* 0xD9 */ {&M6502::AbsY,      &M6502::CMP},
    /* 0xDA */ {&M6502::Imp,       &M6502::PHX},     // 65C02 PHX
    /* 0xDB */ {&M6502::STP,       nullptr},
    /* 0xDC */ {&M6502::UnoffAbs4, nullptr},          // NOP abs,X (3 bytes, 4 cyc)
    /* 0xDD */ {&M6502::AbsX,      &M6502::CMP},
    /* 0xDE */ {&M6502::WAbsX,     &M6502::DEC},
    /* 0xDF */ {&M6502::BBSn<5>,   nullptr},

    /* 0xE0 */ {&M6502::Imm,       &M6502::CPX},
    /* 0xE1 */ {&M6502::IndZeroX,  &M6502::SBC},
    /* 0xE2 */ {&M6502::UnoffImm,  nullptr},          // 65C02: NOP imm; NMOS: DOP #imm
    /* 0xE3 */ {&M6502::Unoff,     nullptr},
    /* 0xE4 */ {&M6502::Zero,      &M6502::CPX},
    /* 0xE5 */ {&M6502::Zero,      &M6502::SBC},
    /* 0xE6 */ {&M6502::Zero,      &M6502::INC},
    /* 0xE7 */ {&M6502::SMBn<6>,   nullptr},
    /* 0xE8 */ {&M6502::Imp,       &M6502::INX},
    /* 0xE9 */ {&M6502::Imm,       &M6502::SBC},
    /* 0xEA */ {&M6502::Imp,       &M6502::NOP},
    /* 0xEB */ {&M6502::Unoff,     nullptr},          // 65C02: NOP 1B; NMOS: undoc SBC #imm
    /* 0xEC */ {&M6502::Abs,       &M6502::CPX},
    /* 0xED */ {&M6502::Abs,       &M6502::SBC},
    /* 0xEE */ {&M6502::Abs,       &M6502::INC},
    /* 0xEF */ {&M6502::BBSn<6>,   nullptr},

    /* 0xF0 */ {&M6502::Rel,       &M6502::BEQ},
    /* 0xF1 */ {&M6502::IndZeroY,  &M6502::SBC},
    /* 0xF2 */ {&M6502::IndZero,   &M6502::SBC},     // 65C02 SBC (zp)
    /* 0xF3 */ {&M6502::Unoff,     nullptr},
    /* 0xF4 */ {&M6502::UnoffZpX,  nullptr},          // NOP zp,X (2 bytes, 4 cyc)
    /* 0xF5 */ {&M6502::ZeroX,     &M6502::SBC},
    /* 0xF6 */ {&M6502::ZeroX,     &M6502::INC},
    /* 0xF7 */ {&M6502::SMBn<7>,   nullptr},
    /* 0xF8 */ {&M6502::Imp,       &M6502::SED},
    /* 0xF9 */ {&M6502::AbsY,      &M6502::SBC},
    /* 0xFA */ {&M6502::Imp,       &M6502::PLX},     // 65C02 PLX
    /* 0xFB */ {&M6502::Unoff,     nullptr},
    /* 0xFC */ {&M6502::UnoffAbs4, nullptr},          // NOP abs,X (3 bytes, 4 cyc)
    /* 0xFD */ {&M6502::AbsX,      &M6502::SBC},
    /* 0xFE */ {&M6502::WAbsX,     &M6502::INC},
    /* 0xFF */ {&M6502::BBSn<7>,   nullptr},
};

// Temporary diagnostic — ring buffer of the last N non-sequential PC
// transitions (JMP/JSR/RTS/branch/IRQ). Sequential walks are collapsed into a
// single slot that records the *first* PC of the run, so the dump shows the
// 24 most recent control-flow transfers leading up to a BRK.
namespace {
constexpr int kPcTraceSize = 256;
struct PcEdge {
    uint16_t from;   // PC of the last instruction before the transfer
    uint16_t to;     // PC landed on after the transfer
};
PcEdge   g_pcTrace[kPcTraceSize] = {};
int      g_pcTraceIdx = 0;
uint16_t g_prevPc = 0;
bool     g_prevValid = false;

// Opt-in diagnostics, resolved once at load. They used to be function-local
// statics inside step()/executeOpcode(); a function-local static costs an
// initialisation-guard check (an acquire load + branch) on EVERY call, and
// these sit on the per-instruction path — measured at ~3 % of a banner
// profile for the three of them together. Namespace scope = plain loads.
const bool  g_crashTrap    = std::getenv("POM2_TRACE_HANG") != nullptr;
const bool  g_traceIllegal = std::getenv("POM2_TRACE_ILLEGAL") != nullptr;
FILE* const g_pcTrace_file = [] {
    const char* p = std::getenv("POM2_TRACE_PC");
    return p ? std::fopen(p, "w") : static_cast<FILE*>(nullptr);
}();
// Cap so a forgotten trace can't fill the disk. 3M instructions is ~130 MB
// and about 10 s of emulated time — raise it with POM2_TRACE_PC_MAX when
// the interesting window sits further in (a print after a boot + menu walk).
const long  g_pcTraceMax = [] {
    const char* m = std::getenv("POM2_TRACE_PC_MAX");
    const long v = m ? std::strtol(m, nullptr, 10) : 0;
    return v > 0 ? v : 3000000L;
}();
long        g_pcTraceN = 0;
}
void M6502::executeOpcode(void)
{
    // Count the opcode fetch itself so per-instruction timing matches 6502 totals.
    cycles = 1;
    // Detect non-sequential PC transitions. "Sequential" = new PC is within a
    // few bytes of the previous instruction's start (covers 1..3 byte opcodes
    // plus taken short branches within the same page). Anything else is a
    // control-flow event worth recording.
    if (g_prevValid) {
        int delta = static_cast<int>(programCounter) - static_cast<int>(g_prevPc);
        if (delta < 0 || delta > 3) {
            g_pcTrace[g_pcTraceIdx] = {g_prevPc, programCounter};
            g_pcTraceIdx = (g_pcTraceIdx + 1) % kPcTraceSize;
        }
    }
    g_prevPc = programCounter;
    g_prevValid = true;

    // Trap any landing on the IIe Enhanced ROM reset entry ($FA62).
    // Triggered by hard/soft reset (PC fetched from $FFFC) *and* by any
    // JMP/JSR that targets $FA62 directly (e.g. games that exit via the
    // ROM cold-start path). Dumps the PC trace, the soft-reset magic
    // bytes ($03F2-$03F4), and the top of the stack so we can tell who
    // bounced us into the ROM.
    if (programCounter == 0xFA62 && iieRebootTraceEnabled()) {
        std::ostringstream oss;
        oss << std::hex << std::uppercase << std::setfill('0');
        oss << "IIe reset entry $FA62 hit"
            << " prevPC=$" << std::setw(4) << static_cast<int>(g_prevPc)
            << " A=" << std::setw(2) << static_cast<int>(accumulator)
            << " X=" << std::setw(2) << static_cast<int>(xRegister)
            << " Y=" << std::setw(2) << static_cast<int>(yRegister)
            << " SP=" << std::setw(2) << static_cast<int>(stackPointer)
            << " $03F2=" << std::setw(2) << static_cast<int>(memory->memRead(0x03F2))
            << " $03F3=" << std::setw(2) << static_cast<int>(memory->memRead(0x03F3))
            << " $03F4=" << std::setw(2) << static_cast<int>(memory->memRead(0x03F4))
            << " stack(next 6):";
        for (int i = 1; i <= 6; ++i) {
            uint8_t sp = static_cast<uint8_t>(stackPointer + i);
            oss << " " << std::setw(2)
                << static_cast<int>(memory->memRead(0x100 + sp));
        }
        pom2::log().warn("CPU", oss.str());
        dumpPcTrace("IIe reset $FA62");
    }

    unsigned char opcode = memory->memRead(programCounter++);

    // DIAGNOSTIC (POM2_TRACE_PC=path): log every instruction (PC, opcode,
    // A/X/Y) to a file for trace-diffing against a MAME reference. Capped.
    if (g_pcTrace_file) {
        FILE* const pcTrace  = g_pcTrace_file;
        long&       pcTraceN = g_pcTraceN;
        if (pcTraceN < g_pcTraceMax) {
            // cum = cumulative CPU cycles BEFORE this instruction (memory's
            // counter is bumped by advanceCycles after each opcode). Lets us
            // diff cycle accounting against a MAME totalcycles trace.
            const unsigned long long cum = memory
                ? static_cast<unsigned long long>(memory->getCycleCounter())
                : 0ULL;
            std::fprintf(pcTrace, "%04X %02X A=%02X X=%02X Y=%02X SP=%02X cyc=%llu\n",
                static_cast<unsigned>(programCounter - 1), opcode,
                accumulator, xRegister, yRegister, stackPointer, cum);
            if ((++pcTraceN % 4096) == 0) std::fflush(pcTrace);
        }
    }

    const OpcodeEntry& entry = opcodeTable[opcode];
    // DIAGNOSTIC (POM2_TRACE_ILLEGAL=1): log when execution hits an
    // unimplemented/undocumented opcode (Unoff* = NOP placeholder) or a
    // KIL (Hang). Used to find whether a crack's loader depends on an
    // illegal opcode POM2 doesn't model. Cheap when off.
    {
        const bool kTraceIllegal = g_traceIllegal;
        if (kTraceIllegal &&
            (entry.addrMode == &M6502::Unoff     ||
             entry.addrMode == &M6502::Unoff1    ||
             entry.addrMode == &M6502::Unoff2    ||
             entry.addrMode == &M6502::Unoff3    ||
             // Cycle/length-accurate placeholder variants — the NMOS
             // remap routes the loader-relevant illegals (ANC/ALR/ARR/
             // LAX/SBX #imm, $5C, $DC/$FC) through these; omitting them
             // made the detector blind to exactly those opcodes.
             entry.addrMode == &M6502::UnoffImm  ||
             entry.addrMode == &M6502::UnoffZpX  ||
             entry.addrMode == &M6502::UnoffAbs4 ||
             entry.addrMode == &M6502::UnoffAbsX ||
             entry.addrMode == &M6502::Unoff5C   ||
             entry.addrMode == &M6502::Hang)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                "illegal/NOP opcode $%02X at $%04X", opcode,
                static_cast<int>(programCounter - 1));
            pom2::log().warn("CPU", buf);
        }
    }
    (this->*entry.addrMode)();
    if (entry.operation)
        (this->*entry.operation)();
}

void M6502::dumpPcTrace(const char* tag)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << tag << " (from->to):";
    for (int i = 0; i < kPcTraceSize; ++i) {
        int idx = (g_pcTraceIdx + i) % kPcTraceSize;
        const PcEdge& e = g_pcTrace[idx];
        if (e.from == 0 && e.to == 0) continue;
        oss << " $" << std::setw(4) << static_cast<int>(e.from)
            << "->$" << std::setw(4) << static_cast<int>(e.to);
    }
    pom2::log().warn("CPU", oss.str());
}

void M6502::hardReset(void)
{
    statusRegister = 0x24;
    statusRegister |= M6502::Status::I;
    stackPointer = 0xFF;
    accumulator = 0;
    xRegister = 0;
    yRegister = 0;
    halted = false;
    // RESET drops any pending NMI edge and clears the IRQ source mask.
    // Every reset path deasserts the device side first (slotBus().reset()
    // runs the onReset hooks, resetSoftSwitches clears the VBL source), so
    // the mask restarts in lock-step with the devices — and
    // Memory::resetSoftSwitches documents exactly this contract.
    NMI = 0;
    irqSourceMask.store(0, std::memory_order_relaxed);
    IRQ.store(0, std::memory_order_relaxed);

    // Don't wipe the stack page on F12. Real 6502 reset only decrements
    // SP (the BRK-emulating reset sequence pushes PC/P without storing),
    // matching MAME `apple2.cpp:325-331` which leaves RAM untouched.
    // `EmulationController::coldBoot()` zero-fills $0000-$01FF via
    // `Memory::clearRam()` when the user explicitly asks for a power-cycle.

    programCounter = memReadAbsolute(0xFFFC);
}

uint64_t M6502::getCycleCountNow() const
{
    if (!memory) return static_cast<uint64_t>(cycles);
    return memory->getCycleCounter() + static_cast<uint64_t>(cycles);
}

void M6502::softReset(void)
{
    statusRegister |= M6502::Status::I;
    // 65C02 (CMOS) reset also clears D — MAME `ow65c02.lst:814`:
    // `m_P = (m_P | F_I) & ~F_D;`. NMOS leaves D undefined; the safer
    // behaviour is to NOT touch D in NMOS mode.
    if (cpuMode == CpuMode::CMOS) {
        statusRegister &= ~M6502::Status::D;
    }
    halted = false;
    // Same interrupt-latch policy as hardReset: RESET drops a pending NMI
    // edge and clears the source mask (devices were deasserted by the
    // slot-bus reset that precedes the CPU reset on every path).
    NMI = 0;
    irqSourceMask.store(0, std::memory_order_relaxed);
    IRQ.store(0, std::memory_order_relaxed);
    // Real 6502 reset sequence is a faked BRK that pushes PC + P
    // WITHOUT writing to the stack page (the read/write line stays
    // high), but decrements SP by 3. POM2 used to snap SP=$FF which
    // matched a power-on but diverged from Ctrl-Reset (B-1-3).
    stackPointer = static_cast<uint8_t>(stackPointer - 3);
    programCounter = memReadAbsolute(0xFFFC);
}

void M6502::setIrqLine(int sourceId, bool asserted)
{
    // Wire-OR semantics: every asserting source sets its own bit; the
    // CPU IRQ flag follows `irqSourceMask != 0`. Without this, two
    // cards plugged at once (e.g. Mockingboard + SSC) would race —
    // whichever one released last won, even if the other still wanted
    // the line asserted.
    const uint32_t bit = 1u << (sourceId & 31);
    // Atomic RMW so a cross-thread caller (the SSC TCP worker) and the CPU
    // thread can't lose each other's update. `newMask` is the post-op value
    // — deriving IRQ from it keeps the level correct even if two threads
    // interleave (the surviving asserted bit still drives IRQ=1).
    const uint32_t newMask = asserted
        ? (irqSourceMask.fetch_or(bit,  std::memory_order_relaxed) | bit)
        : (irqSourceMask.fetch_and(~bit, std::memory_order_relaxed) & ~bit);
    IRQ.store(newMask != 0 ? 1 : 0, std::memory_order_relaxed);
}

void M6502::setIRQ(int state)
{
    setIrqLine(IRQ_SRC_LEGACY, state != 0);
}

void M6502::setNMI(void)
{
    NMI = 1;
}

void M6502::step(void)
{
    // CPU crash trap (POM2_TRACE_HANG=1): a deterministic "plantage" where the
    // PC jumps into non-code (stack page or $C0xx I/O) — i.e. it executes RAM
    // data / soft-switch bytes as opcodes — is the signature of a corrupted
    // jump/return after the city decompression. Catch the first such PC and
    // dump the trail of the last 32 PCs so we see WHERE the bad jump came from.
    {
        const bool crashTrap = g_crashTrap;
        if (crashTrap) {
            static const int RN = 512;
            static uint16_t ring[512] = {0};
            static unsigned rpos = 0;
            static bool dumped = false;
            static uint16_t prevPc = 0xFFFF;
            static long sameCount = 0;
            ring[rpos % RN] = programCounter;
            ++rpos;
            const uint16_t pc = programCounter;
            // Trace every entry to the $0080 ZP bank-switch trampoline with the
            // ALTZP state + caller. Normal calls run with ALTZP=0 (main ZP, the
            // only bank the routine was copied to); the fatal call has ALTZP=1
            // → executes empty aux ZP → crash. The caller of that one is the
            // culprit jump.
            if ((pc == 0x0080 || pc == 0x0081) && memory) {
                static int tn = 0;
                if (tn++ < 90) {
                    const uint16_t caller = ring[(rpos + RN - 2) % RN];
                    std::fprintf(stderr,
                        "[TRAMP] enter $%04X ALTZP=%d  caller=$%04X\n",
                        pc, (memory->iieModeFlags() & 0x10) ? 1 : 0, caller);
                }
            }
            // Self-loop detection: PC literally not advancing = a JMP-self /
            // BRK-to-self-vector spin (the real freeze: $0081, $3231, …). Dump
            // EARLY (after 200 identical PCs) so the 512-ring still holds the
            // ~300 PCs BEFORE the loop = the jump that landed in garbage.
            if (pc == prevPc) ++sameCount; else { sameCount = 0; prevPc = pc; }
            const bool selfLoop = (sameCount == 200);
            const bool ioCrash  = (pc >= 0xC000 && pc <= 0xC0FF);
            if ((selfLoop || ioCrash) && !dumped) {
                dumped = true;
                std::fprintf(stderr,
                    "\n*** POM2 %s — PC=$%04X ***\n",
                    selfLoop ? "HANG: PC stuck (self-loop / BRK trap)"
                             : "CPU CRASH (executing $C0xx I/O as code)",
                    pc);
                std::fprintf(stderr,
                    "  A=%02X X=%02X Y=%02X SP=%02X P=%02X\n",
                    getAccumulator(), getXRegister(), getYRegister(),
                    getStackPointer(), getStatusRegister());
                std::fprintf(stderr,
                    "  PC trail (oldest -> newest, %d entries) — find where it left valid code:\n   ",
                    RN);
                for (int i = 0; i < RN; ++i) {
                    std::fprintf(stderr, " %04X", ring[(rpos + i) % RN]);
                    if ((i & 15) == 15) std::fprintf(stderr, "\n   ");
                }
                std::fprintf(stderr, "\n");
                std::fflush(stderr);
            }
        }
    }

    // STP-halted CPU: burn cycles, ignore IRQ/NMI (only RESET wakes,
    // and that's `softReset()` / `hardReset()` clearing `halted`).
    if (halted) {
        cycles = 2;
        if (memory != nullptr) memory->advanceCycles(cycles);
        return;
    }

    // NMI has higher priority than IRQ on real silicon. MAME models
    // this by picking the NMI vector inside `brk_c_imp` when both are
    // pending (ow65c02.lst:247 + m6502.cpp prefetch_end). The previous
    // order here let an IRQ run first when both fired in the same
    // instruction window, masking the NMI for one instruction.
    //
    // The interrupt-entry sequence costs 7 bus cycles on both NMOS and
    // CMOS (MAME om6502.lst irq/nmi sequences; pushPC + flags + vector
    // fetch). handleNMI/handleIRQ accumulate those into `cycles`, but
    // executeOpcode() reseeds `cycles = 1` for the opcode fetch and would
    // wipe them. Capture the entry cost first, then fold it back in after
    // the next instruction runs. Previously every IRQ/NMI was charged 0
    // cycles, silently desyncing every cycleCounter-derived clock (VBL,
    // slot-peripheral timers, cassette) on interrupt-driven software.
    // ── Owned deviation: interrupt sampling is instruction-granular ──────
    // Real silicon samples IRQ/NMI during phi2 of an instruction's
    // penultimate cycle, and CLI / SEI / PLP land their new I flag *after*
    // that sample point — so an IRQ pending across a `CLI` is taken one
    // instruction later than you would naively expect, and an `SEI` does
    // not cancel an interrupt already sampled. MAME's m6502 models this
    // by inhibiting interrupts for one instruction after those opcodes.
    //
    // POM2 tests the I flag here, at an instruction boundary, so it takes
    // the interrupt one instruction EARLIER than hardware in the CLI case.
    // This is structural, not an oversight: step() is instruction-granular
    // (`cycles` is charged in one lump to memory->advanceCycles), so there
    // is no penultimate cycle to sample at. Modelling the delay would mean
    // a cycle-stepped core.
    //
    // Not covered by Klaus or Tom Harte — neither drives the interrupt
    // lines. Consequences are confined to software that counts cycles
    // through an IRQ entry; the Apple II titles in the corpus set up their
    // handlers and rely on the VIA/timer period, not on CLI placement.
    // (Distinct from the VIA `syncToCpuCycle` one-instruction over-count
    // fixed 2026-05-25 — see DEV.md "Lazy timer sync".)
    int interruptCycles = 0;
    if (NMI) {
        cycles = 0;
        handleNMI();
        interruptCycles = cycles;
    } else if (!(statusRegister & M6502::Status::I) &&
               IRQ.load(std::memory_order_relaxed)) {
        cycles = 0;
        handleIRQ();
        interruptCycles = cycles;
    }

    // ── An interrupt entry ENDS the step, but only for a watcher ─────────
    // Without this, one step() both vectors and runs the handler's first
    // instruction, so `M6502::run`'s debugged loop never offers the handler
    // PC to `onInstruction`: a breakpoint on an IRQ/NMI entry point could not
    // fire, and a watchpoint tripped by handler[0] was attributed to the
    // INTERRUPTED instruction's PC (Debugger::curPc_ is latched per
    // onInstruction call). Returning here leaves the PC on the vector target,
    // so the next iteration offers it like any other instruction.
    //
    // Gated on `debugHook_` so nothing outside a debugging session changes.
    // The cycle total is identical either way — the same 7 entry cycles and
    // the same opcode, split across two steps instead of summed in one — but
    // `memory->advanceCycles` then sees them as two smaller advances, which
    // moves the sub-instruction phase every lazily-synced peripheral clock
    // derives from (Mockingboard/Phasor T1, the Disk II LSS, the video beam).
    // That phase is pinned by mockingboard_t1_irq_phase / via_t1_rearm_chain /
    // dix_menu_raster_probe, so it is not something to perturb for a machine
    // nobody is watching.
    if (debugHook_ != nullptr && interruptCycles != 0) {
        cycles = interruptCycles;
        if (memory != nullptr) {
            memory->advanceCycles(cycles);
        }
        return;
    }

    executeOpcode();
    cycles += interruptCycles;
    if (memory != nullptr) {
        memory->advanceCycles(cycles);
    }
}

int M6502::run(int maxCycles)
{
    int cyclesExecuted = 0;
    running = 1;

    // Two loops, chosen ONCE per call. The debugged loop pays a virtual call
    // per instruction, which is the right trade when somebody is watching;
    // the fast loop below is byte-for-byte what it always was, so a session
    // with no debugger attached pays a single predictable branch per
    // 4096-cycle chunk. Measured as unchanged on all three pom2_bench
    // workloads — see docs/PERFORMANCE.md § Debugger hooks.
    if (debugHook_) {
        while (running && cyclesExecuted < maxCycles) {
            // Checked BEFORE the instruction, so a breakpoint stops with the
            // PC still on it: the instruction has not run, and the register
            // dump the user reads is the state going in, not coming out.
            if (debugHook_->onInstruction(programCounter)) break;
            step();
            cyclesExecuted += cycles;
            // A watchpoint fires mid-instruction and cannot un-do the access,
            // so it halts at the first instruction boundary after it. The
            // hook reports that through the same `running` flag stop() uses.
            if (!running) break;
        }
        return cyclesExecuted;
    }

    while (running && cyclesExecuted < maxCycles) {
        step();
        cyclesExecuted += cycles;
    }
    return cyclesExecuted;
}

void M6502::start(void)
{
    running = 1;
}

void M6502::stop(void)
{
    running = 0;
}

