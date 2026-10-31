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

// Z80 core implementation. See Z80.h for scope + references.
//
// Decoder layout follows the classic x/y/z/p/q opcode-field decomposition
// (op = xx yyy zzz, p = y>>1, q = y&1 — cf. "Decoding Z80 opcodes",
// z80.info/decoding.htm, the same structure MAME's z80.cpp tables encode):
//   x=1 → LD r,r' matrix (+HALT), x=2 → ALU A,r matrix, x=0/3 → z-keyed
//   switch. This keeps the main page at ~40 cases instead of 256.
//
// Flag semantics cross-checked against MAME src/devices/cpu/z80/z80.cpp
// (SZP/SZHVC tables + per-op F computation) and validated bit-exact by
// zexall (tests/z80_zex_test.cpp): X/Y always mirror bits 3/5 of the
// "result byte" each instruction publishes — which for CP A,r is the
// *operand*, for LDI/CPI is an internal sum, and for BIT n,(HL) is the
// hidden WZ/MEMPTR high byte. Those three are the classic zexall killers;
// none of them is approximated here.

#include "Z80.h"

namespace pom2 {

using F = Z80::Flag;

uint8_t Z80::szpTable[256];
bool    Z80::tablesReady = false;

void Z80::initTables()
{
    for (int v = 0; v < 256; ++v) {
        uint8_t f = uint8_t(v) & (F::S | F::X | F::Y);
        if (v == 0)
            f |= F::Z;
        int bits = 0;
        for (int b = 0; b < 8; ++b)
            bits += (v >> b) & 1;
        if ((bits & 1) == 0)
            f |= F::PV;
        szpTable[v] = f;
    }
    tablesReady = true;
}

Z80::Z80(Z80Bus& b) : bus(b)
{
    if (!tablesReady)
        initTables();
    reset();
}

void Z80::reset()
{
    State fresh;
    // RESET defines PC/I/R/IFF/IM; AF/SP come up $FFFF on real silicon,
    // the rest is left as State's defaults (don't-care on hardware).
    fresh.irqLine = st.irqLine;   // the INT line level survives a reset pulse
    fresh.irqData = st.irqData;
    st = fresh;
}

void Z80::setIrqLine(bool asserted, uint8_t dataBusValue)
{
    st.irqLine = asserted;
    st.irqData = dataBusValue;
}

void Z80::triggerNmi()
{
    st.nmiPending = true;
}

// ── register-file helpers ────────────────────────────────────────────

uint16_t Z80::idxPair(int mode) const
{
    switch (mode) {
    case MODE_IX: return st.ix;
    case MODE_IY: return st.iy;
    default:      return hl();
    }
}

void Z80::setIdxPair(int mode, uint16_t v)
{
    switch (mode) {
    case MODE_IX: st.ix = v; break;
    case MODE_IY: st.iy = v; break;
    default:      setHL(v);  break;
    }
}

uint8_t Z80::readReg(int r, int mode) const
{
    switch (r) {
    case 0: return st.b;
    case 1: return st.c;
    case 2: return st.d;
    case 3: return st.e;
    case 4:
        if (mode == MODE_IX) return uint8_t(st.ix >> 8);
        if (mode == MODE_IY) return uint8_t(st.iy >> 8);
        return st.h;
    case 5:
        if (mode == MODE_IX) return uint8_t(st.ix);
        if (mode == MODE_IY) return uint8_t(st.iy);
        return st.l;
    case 7: return st.a;
    }
    return 0xFF;   // r == 6 is the (HL) slot — callers handle it themselves
}

void Z80::writeReg(int r, int mode, uint8_t v)
{
    switch (r) {
    case 0: st.b = v; break;
    case 1: st.c = v; break;
    case 2: st.d = v; break;
    case 3: st.e = v; break;
    case 4:
        if      (mode == MODE_IX) st.ix = uint16_t((st.ix & 0x00FF) | (v << 8));
        else if (mode == MODE_IY) st.iy = uint16_t((st.iy & 0x00FF) | (v << 8));
        else st.h = v;
        break;
    case 5:
        if      (mode == MODE_IX) st.ix = uint16_t((st.ix & 0xFF00) | v);
        else if (mode == MODE_IY) st.iy = uint16_t((st.iy & 0xFF00) | v);
        else st.l = v;
        break;
    case 7: st.a = v; break;
    }
}

uint16_t Z80::memEA(int mode)
{
    if (mode == MODE_HL)
        return hl();
    int8_t d = int8_t(fetch8());
    busT_ += 5;      // the index add, before the access
    uint16_t base = (mode == MODE_IX) ? st.ix : st.iy;
    uint16_t ea = uint16_t(base + d);
    st.wz = ea;      // hardware latches the effective address into WZ
    cyc += 8;        // displacement fetch (3) + internal add (5)
    return ea;
}

// ── ALU / flag helpers ───────────────────────────────────────────────

void Z80::add8(uint8_t v, int carry)
{
    int res = st.a + v + carry;
    uint8_t r8 = uint8_t(res);
    uint8_t f = r8 & (F::S | F::X | F::Y);
    if (!r8)                                 f |= F::Z;
    if ((st.a ^ v ^ r8) & 0x10)              f |= F::H;
    if ((~(st.a ^ v) & (st.a ^ r8)) & 0x80)  f |= F::PV;
    if (res & 0x100)                         f |= F::C;
    st.a = r8;
    st.f = f;
}

void Z80::sub8(uint8_t v, int carry)
{
    int res = st.a - v - carry;
    uint8_t r8 = uint8_t(res);
    uint8_t f = F::N | (r8 & (F::S | F::X | F::Y));
    if (!r8)                                f |= F::Z;
    if ((st.a ^ v ^ r8) & 0x10)             f |= F::H;
    if (((st.a ^ v) & (st.a ^ r8)) & 0x80)  f |= F::PV;
    if (res & 0x100)                        f |= F::C;
    st.a = r8;
    st.f = f;
}

void Z80::and8(uint8_t v) { st.a &= v; st.f = uint8_t(szpTable[st.a] | F::H); }
void Z80::xor8(uint8_t v) { st.a ^= v; st.f = szpTable[st.a]; }
void Z80::or8 (uint8_t v) { st.a |= v; st.f = szpTable[st.a]; }

void Z80::cp8(uint8_t v)
{
    // Like SUB but A is preserved and — the classic zexall trap — X/Y
    // come from the *operand*, not the result.
    int res = st.a - v;
    uint8_t r8 = uint8_t(res);
    uint8_t f = F::N | (r8 & F::S) | (v & (F::X | F::Y));
    if (!r8)                                f |= F::Z;
    if ((st.a ^ v ^ r8) & 0x10)             f |= F::H;
    if (((st.a ^ v) & (st.a ^ r8)) & 0x80)  f |= F::PV;
    if (res & 0x100)                        f |= F::C;
    st.f = f;
}

void Z80::aluDispatch(int op, uint8_t v)
{
    switch (op & 7) {
    case 0: add8(v, 0);            break;   // ADD
    case 1: add8(v, st.f & F::C);  break;   // ADC
    case 2: sub8(v, 0);            break;   // SUB
    case 3: sub8(v, st.f & F::C);  break;   // SBC
    case 4: and8(v);               break;
    case 5: xor8(v);               break;
    case 6: or8(v);                break;
    case 7: cp8(v);                break;
    }
}

uint8_t Z80::inc8(uint8_t v)
{
    uint8_t res = uint8_t(v + 1);
    uint8_t f = uint8_t((st.f & F::C) | (res & (F::S | F::X | F::Y)));
    if (!res)               f |= F::Z;
    if ((res & 0x0F) == 0)  f |= F::H;
    if (res == 0x80)        f |= F::PV;
    st.f = f;
    return res;
}

uint8_t Z80::dec8(uint8_t v)
{
    uint8_t res = uint8_t(v - 1);
    uint8_t f = uint8_t((st.f & F::C) | F::N | (res & (F::S | F::X | F::Y)));
    if (!res)               f |= F::Z;
    if ((v & 0x0F) == 0)    f |= F::H;
    if (res == 0x7F)        f |= F::PV;
    st.f = f;
    return res;
}

uint16_t Z80::add16(uint16_t dst, uint16_t src)
{
    // ADD HL/IX/IY,rr — S/Z/PV survive, H from bit-11 carry, X/Y from
    // the result's high byte.
    uint32_t res = uint32_t(dst) + src;
    st.wz = uint16_t(dst + 1);
    uint8_t f = uint8_t(st.f & (F::S | F::Z | F::PV));
    f |= uint8_t(((dst ^ src ^ res) >> 8) & F::H);
    f |= uint8_t((res >> 8) & (F::X | F::Y));
    if (res & 0x10000)
        f |= F::C;
    st.f = f;
    return uint16_t(res);
}

void Z80::adc16(uint16_t src)
{
    uint16_t dst = hl();
    uint32_t res = uint32_t(dst) + src + (st.f & F::C);
    uint16_t r16 = uint16_t(res);
    st.wz = uint16_t(dst + 1);
    uint8_t f = uint8_t((res >> 8) & (F::S | F::X | F::Y));
    if (!r16)                                        f |= F::Z;
    f |= uint8_t(((dst ^ src ^ res) >> 8) & F::H);
    if ((~(dst ^ src) & (dst ^ r16)) & 0x8000)       f |= F::PV;
    if (res & 0x10000)                               f |= F::C;
    st.f = f;
    setHL(r16);
}

void Z80::sbc16(uint16_t src)
{
    uint16_t dst = hl();
    uint32_t res = uint32_t(dst) - src - (st.f & F::C);
    uint16_t r16 = uint16_t(res);
    st.wz = uint16_t(dst + 1);
    uint8_t f = uint8_t(F::N | ((res >> 8) & (F::S | F::X | F::Y)));
    if (!r16)                                        f |= F::Z;
    f |= uint8_t(((dst ^ src ^ res) >> 8) & F::H);
    if (((dst ^ src) & (dst ^ r16)) & 0x8000)        f |= F::PV;
    if (res & 0x10000)                               f |= F::C;
    st.f = f;
    setHL(r16);
}

void Z80::daa()
{
    uint8_t a = st.a;
    uint8_t adjust = 0;
    uint8_t carry = uint8_t(st.f & F::C);
    if ((st.f & F::H) || (a & 0x0F) > 9)
        adjust |= 0x06;
    if (carry || a > 0x99) {
        adjust |= 0x60;
        carry = F::C;
    }
    uint8_t res = (st.f & F::N) ? uint8_t(a - adjust) : uint8_t(a + adjust);
    st.f = uint8_t((st.f & F::N) | carry | ((a ^ res) & F::H) | szpTable[res]);
    st.a = res;
}

uint8_t Z80::rotShift(int op, uint8_t v)
{
    uint8_t res = 0, carry = 0;
    switch (op & 7) {
    case 0: res = uint8_t((v << 1) | (v >> 7)); carry = v >> 7;    break;  // RLC
    case 1: res = uint8_t((v >> 1) | (v << 7)); carry = v & 1;     break;  // RRC
    case 2: res = uint8_t((v << 1) | (st.f & F::C)); carry = v >> 7; break; // RL
    case 3: res = uint8_t((v >> 1) | ((st.f & F::C) << 7)); carry = v & 1; break; // RR
    case 4: res = uint8_t(v << 1); carry = v >> 7;                 break;  // SLA
    case 5: res = uint8_t((v >> 1) | (v & 0x80)); carry = v & 1;   break;  // SRA
    case 6: res = uint8_t((v << 1) | 1); carry = v >> 7;           break;  // SLL (undoc)
    case 7: res = uint8_t(v >> 1); carry = v & 1;                  break;  // SRL
    }
    st.f = uint8_t(szpTable[res] | (carry ? F::C : 0));
    return res;
}

void Z80::bitReg(int n, uint8_t v)
{
    // S/Z/PV come from "AND v,(1<<n)" with the result discarded, but X/Y
    // copy bits 3/5 of the FULL register value regardless of n (zexall's
    // <bit n,r> CRC pins this; the masked result only feeds S).
    uint8_t res = uint8_t(v & (1 << n));
    uint8_t f = uint8_t((st.f & F::C) | F::H | (res & F::S) | (v & (F::X | F::Y)));
    if (!res)
        f |= F::Z | F::PV;
    st.f = f;
}

void Z80::bitMem(int n, uint8_t v, uint8_t xyFrom)
{
    // BIT n,(HL): X/Y leak from WZ's high byte; BIT n,(IX+d): from the
    // effective address's high byte. This is what zexall's <bit n,(hl)>
    // CRCs encode.
    uint8_t res = uint8_t(v & (1 << n));
    uint8_t f = uint8_t((st.f & F::C) | F::H | (res & F::S) | (xyFrom & (F::X | F::Y)));
    if (!res)
        f |= F::Z | F::PV;
    st.f = f;
}

// ── interrupt service ────────────────────────────────────────────────

void Z80::serviceNmi()
{
    st.halted = false;
    bumpR();
    st.iff1 = false;           // IFF2 keeps the pre-NMI IFF1 for RETN
    push16(st.pc);
    st.pc = 0x0066;
    st.wz = st.pc;
    cyc = 11;
}

void Z80::serviceIrq()
{
    st.halted = false;
    bumpR();
    st.iff1 = st.iff2 = false;
    switch (st.im) {
    case 2: {
        push16(st.pc);
        uint16_t vec = uint16_t((st.i << 8) | st.irqData);
        st.pc = rd16(vec);
        cyc = 19;
        break;
    }
    case 1:
        push16(st.pc);
        st.pc = 0x0038;
        cyc = 13;
        break;
    default:
        // IM0: assume the device drives an RST opcode (11ttt111).
        push16(st.pc);
        st.pc = uint16_t(st.irqData & 0x38);
        cyc = 13;
        break;
    }
    st.wz = st.pc;
}

// ── top-level execution ──────────────────────────────────────────────

int Z80::step()
{
    cyc = 0;
    busT_ = 0;
    int mode = MODE_HL;
    if (st.pendingPrefix) {
        // Resuming a DD/FD chain: no interrupt sampling between a prefix
        // and its opcode (hardware defers INT and NMI across a prefix).
        mode = (st.pendingPrefix == 1) ? MODE_IX : MODE_IY;
        st.pendingPrefix = 0;
    } else {
        if (st.nmiPending) {
            st.nmiPending = false;
            serviceNmi();
            return cyc;
        }
        if (st.irqLine && st.iff1 && !st.afterEi) {
            serviceIrq();
            return cyc;
        }
        st.afterEi = false;
        if (st.halted) {
            bumpR();
            cyc = 4;
            return cyc;
        }
        if (observer_) observer_->onZ80Instruction(*this);
    }

    uint8_t op = fetchOp();
    bumpR();
    if (op == 0xDD || op == 0xFD) {
        // Each prefix retires as its own 4-T M1 cycle so a run of
        // prefixes can't fold unbounded work into one step() (a crashed
        // guest jumping into a $DD/$FD sea used to hold the host lock
        // for the whole run — or forever on a wrapped 64 K of prefixes).
        st.pendingPrefix = (op == 0xDD) ? 1 : 2;
        cyc += 4;
        return cyc;
    }

    if (op == 0xCB) {
        if (mode == MODE_HL)
            execCB();
        else
            execIndexedCB(mode);
    } else if (op == 0xED) {
        execED();               // a dangling DD/FD prefix is ignored by ED
    } else {
        execMain(op, mode);
    }
    return cyc;
}

// ── main page ────────────────────────────────────────────────────────

void Z80::execMain(uint8_t op, int mode)
{
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;
    const int p = y >> 1;
    const int q = y & 1;

    // x=1 — LD r,r' matrix + HALT
    if (x == 1) {
        if (op == 0x76) {              // HALT
            st.halted = true;
            cyc += 4;
            return;
        }
        if (z == 6) {                  // LD r,(HL)/(IX+d) — dest uses true H/L
            uint16_t ea = memEA(mode);
            writeReg(y, MODE_HL, rd(ea));
            cyc += 7;
        } else if (y == 6) {           // LD (HL)/(IX+d),r — src uses true H/L
            uint16_t ea = memEA(mode);
            wr(ea, readReg(z, MODE_HL));
            cyc += 7;
        } else {
            writeReg(y, mode, readReg(z, mode));
            cyc += 4;
        }
        return;
    }

    // x=2 — ALU A,r matrix
    if (x == 2) {
        if (z == 6) {
            uint16_t ea = memEA(mode);
            aluDispatch(y, rd(ea));
            cyc += 7;
        } else {
            aluDispatch(y, readReg(z, mode));
            cyc += 4;
        }
        return;
    }

    // x=0 — assorted, keyed on z
    if (x == 0) {
        switch (z) {
        case 0:
            switch (y) {
            case 0:                    // NOP
                cyc += 4;
                break;
            case 1: {                  // EX AF,AF'
                uint8_t t;
                t = st.a; st.a = st.a2; st.a2 = t;
                t = st.f; st.f = st.f2; st.f2 = t;
                cyc += 4;
                break;
            }
            case 2: {                  // DJNZ d
                int8_t d = int8_t(fetch8());
                if (--st.b) {
                    st.pc = uint16_t(st.pc + d);
                    st.wz = st.pc;
                    cyc += 13;
                } else {
                    cyc += 8;
                }
                break;
            }
            case 3: {                  // JR d
                int8_t d = int8_t(fetch8());
                st.pc = uint16_t(st.pc + d);
                st.wz = st.pc;
                cyc += 12;
                break;
            }
            default: {                 // JR cc,d (y=4..7 → NZ Z NC C)
                int8_t d = int8_t(fetch8());
                bool taken = false;
                switch (y - 4) {
                case 0: taken = !(st.f & F::Z); break;
                case 1: taken =  (st.f & F::Z); break;
                case 2: taken = !(st.f & F::C); break;
                case 3: taken =  (st.f & F::C); break;
                }
                if (taken) {
                    st.pc = uint16_t(st.pc + d);
                    st.wz = st.pc;
                    cyc += 12;
                } else {
                    cyc += 7;
                }
                break;
            }
            }
            break;

        case 1:
            if (q == 0) {              // LD rp,nn
                uint16_t nn = fetch16();
                switch (p) {
                case 0: setBC(nn); break;
                case 1: setDE(nn); break;
                case 2: setIdxPair(mode, nn); break;
                case 3: st.sp = nn; break;
                }
                cyc += 10;
            } else {                   // ADD HL/IX/IY,rp
                uint16_t dst = idxPair(mode);
                uint16_t src = 0;
                switch (p) {
                case 0: src = bc(); break;
                case 1: src = de(); break;
                case 2: src = idxPair(mode); break;   // ADD IX,IX under DD
                case 3: src = st.sp; break;
                }
                setIdxPair(mode, add16(dst, src));
                cyc += 11;
            }
            break;

        case 2:
            switch ((p << 1) | q) {
            case 0:                    // LD (BC),A
                wr(bc(), st.a);
                st.wz = uint16_t((st.a << 8) | ((bc() + 1) & 0xFF));
                cyc += 7;
                break;
            case 1:                    // LD A,(BC)
                st.a = rd(bc());
                st.wz = uint16_t(bc() + 1);
                cyc += 7;
                break;
            case 2:                    // LD (DE),A
                wr(de(), st.a);
                st.wz = uint16_t((st.a << 8) | ((de() + 1) & 0xFF));
                cyc += 7;
                break;
            case 3:                    // LD A,(DE)
                st.a = rd(de());
                st.wz = uint16_t(de() + 1);
                cyc += 7;
                break;
            case 4: {                  // LD (nn),HL/IX/IY
                uint16_t nn = fetch16();
                wr16(nn, idxPair(mode));
                st.wz = uint16_t(nn + 1);
                cyc += 16;
                break;
            }
            case 5: {                  // LD HL/IX/IY,(nn)
                uint16_t nn = fetch16();
                setIdxPair(mode, rd16(nn));
                st.wz = uint16_t(nn + 1);
                cyc += 16;
                break;
            }
            case 6: {                  // LD (nn),A
                uint16_t nn = fetch16();
                wr(nn, st.a);
                st.wz = uint16_t((st.a << 8) | ((nn + 1) & 0xFF));
                cyc += 13;
                break;
            }
            case 7: {                  // LD A,(nn)
                uint16_t nn = fetch16();
                st.a = rd(nn);
                st.wz = uint16_t(nn + 1);
                cyc += 13;
                break;
            }
            }
            break;

        case 3: {                      // INC/DEC rp
            int delta = q ? -1 : 1;
            switch (p) {
            case 0: setBC(uint16_t(bc() + delta)); break;
            case 1: setDE(uint16_t(de() + delta)); break;
            case 2: setIdxPair(mode, uint16_t(idxPair(mode) + delta)); break;
            case 3: st.sp = uint16_t(st.sp + delta); break;
            }
            cyc += 6;
            break;
        }

        case 4:                        // INC r
            if (y == 6) {
                uint16_t ea = memEA(mode);
                { const uint8_t v = inc8(rd(ea)); ++busT_; wr(ea, v); }   // read M-cycle is 4 T
                cyc += 11;
            } else {
                writeReg(y, mode, inc8(readReg(y, mode)));
                cyc += 4;
            }
            break;

        case 5:                        // DEC r
            if (y == 6) {
                uint16_t ea = memEA(mode);
                { const uint8_t v = dec8(rd(ea)); ++busT_; wr(ea, v); }   // read M-cycle is 4 T
                cyc += 11;
            } else {
                writeReg(y, mode, dec8(readReg(y, mode)));
                cyc += 4;
            }
            break;

        case 6:                        // LD r,n
            if (y == 6) {
                // LD (IX+d),n fetches d and n back-to-back: 19 T total
                // (with the 4 the prefix already charged), not 4+8+10.
                if (mode == MODE_HL) {
                    uint8_t n = fetch8();
                    wr(hl(), n);
                    cyc += 10;
                } else {
                    int8_t d = int8_t(fetch8());
                    uint16_t base = (mode == MODE_IX) ? st.ix : st.iy;
                    uint16_t ea = uint16_t(base + d);
                    st.wz = ea;
                    uint8_t n = fetch8();
                    busT_ += 2;            // n's M-cycle is 5 T (index add)
                    wr(ea, n);
                    cyc += 15;
                }
            } else {
                writeReg(y, mode, fetch8());
                cyc += 7;
            }
            break;

        case 7:
            switch (y) {
            case 0: {                  // RLCA
                uint8_t carry = st.a >> 7;
                st.a = uint8_t((st.a << 1) | carry);
                st.f = uint8_t((st.f & (F::S | F::Z | F::PV)) | (st.a & (F::X | F::Y)) | carry);
                cyc += 4;
                break;
            }
            case 1: {                  // RRCA
                uint8_t carry = st.a & 1;
                st.a = uint8_t((st.a >> 1) | (carry << 7));
                st.f = uint8_t((st.f & (F::S | F::Z | F::PV)) | (st.a & (F::X | F::Y)) | carry);
                cyc += 4;
                break;
            }
            case 2: {                  // RLA
                uint8_t carry = st.a >> 7;
                st.a = uint8_t((st.a << 1) | (st.f & F::C));
                st.f = uint8_t((st.f & (F::S | F::Z | F::PV)) | (st.a & (F::X | F::Y)) | carry);
                cyc += 4;
                break;
            }
            case 3: {                  // RRA
                uint8_t carry = st.a & 1;
                st.a = uint8_t((st.a >> 1) | ((st.f & F::C) << 7));
                st.f = uint8_t((st.f & (F::S | F::Z | F::PV)) | (st.a & (F::X | F::Y)) | carry);
                cyc += 4;
                break;
            }
            case 4:                    // DAA
                daa();
                cyc += 4;
                break;
            case 5:                    // CPL
                st.a = uint8_t(~st.a);
                st.f = uint8_t((st.f & (F::S | F::Z | F::PV | F::C)) | F::H | F::N
                               | (st.a & (F::X | F::Y)));
                cyc += 4;
                break;
            case 6:                    // SCF — X/Y from A (NMOS rule, see Z80.h)
                st.f = uint8_t((st.f & (F::S | F::Z | F::PV)) | F::C | (st.a & (F::X | F::Y)));
                cyc += 4;
                break;
            case 7: {                  // CCF
                uint8_t oldc = uint8_t(st.f & F::C);
                st.f = uint8_t((st.f & (F::S | F::Z | F::PV)) | (oldc ? F::H : F::C)
                               | (st.a & (F::X | F::Y)));
                cyc += 4;
                break;
            }
            }
            break;
        }
        return;
    }

    // x=3 — control flow, stack, I/O, prefixless remainder
    auto ccTest = [this](int ccIdx) -> bool {
        switch (ccIdx) {
        case 0: return !(st.f & F::Z);
        case 1: return  (st.f & F::Z) != 0;
        case 2: return !(st.f & F::C);
        case 3: return  (st.f & F::C) != 0;
        case 4: return !(st.f & F::PV);
        case 5: return  (st.f & F::PV) != 0;
        case 6: return !(st.f & F::S);
        default: return (st.f & F::S) != 0;
        }
    };

    switch (z) {
    case 0:                            // RET cc
        ++busT_;                       // its M1 is 5 T (Zilog UM0080), before the pops
        if (ccTest(y)) {
            st.pc = pop16();
            st.wz = st.pc;
            cyc += 11;
        } else {
            cyc += 5;
        }
        break;

    case 1:
        if (q == 0) {                  // POP rp2
            uint16_t v = pop16();
            switch (p) {
            case 0: setBC(v); break;
            case 1: setDE(v); break;
            case 2: setIdxPair(mode, v); break;
            case 3: st.a = uint8_t(v >> 8); st.f = uint8_t(v); break;
            }
            cyc += 10;
        } else {
            switch (p) {
            case 0:                    // RET
                st.pc = pop16();
                st.wz = st.pc;
                cyc += 10;
                break;
            case 1: {                  // EXX
                uint8_t t;
                t = st.b; st.b = st.b2; st.b2 = t;
                t = st.c; st.c = st.c2; st.c2 = t;
                t = st.d; st.d = st.d2; st.d2 = t;
                t = st.e; st.e = st.e2; st.e2 = t;
                t = st.h; st.h = st.h2; st.h2 = t;
                t = st.l; st.l = st.l2; st.l2 = t;
                cyc += 4;
                break;
            }
            case 2:                    // JP (HL)/(IX)/(IY)
                st.pc = idxPair(mode);
                cyc += 4;
                break;
            case 3:                    // LD SP,HL/IX/IY
                st.sp = idxPair(mode);
                cyc += 6;
                break;
            }
        }
        break;

    case 2: {                          // JP cc,nn
        uint16_t nn = fetch16();
        st.wz = nn;
        if (ccTest(y))
            st.pc = nn;
        cyc += 10;
        break;
    }

    case 3:
        switch (y) {
        case 0: {                      // JP nn
            uint16_t nn = fetch16();
            st.pc = nn;
            st.wz = nn;
            cyc += 10;
            break;
        }
        case 2: {                      // OUT (n),A
            uint8_t n = fetch8();
            uint16_t port = uint16_t((st.a << 8) | n);
            bus.z80IoWrite(port, st.a);
            st.wz = uint16_t((st.a << 8) | ((n + 1) & 0xFF));
            cyc += 11;
            break;
        }
        case 3: {                      // IN A,(n)
            uint8_t n = fetch8();
            uint16_t port = uint16_t((st.a << 8) | n);
            st.a = bus.z80IoRead(port);
            st.wz = uint16_t(port + 1);
            cyc += 11;
            break;
        }
        case 4: {                      // EX (SP),HL/IX/IY
            // Zilog UM0080: M1 4, MR (SP) 3, MR (SP+1) 4, MW (SP+1) 3,
            // MW (SP) 5 — the high byte is written FIRST.
            uint16_t t = rd16(st.sp);
            ++busT_;
            const uint16_t v = idxPair(mode);
            wr(uint16_t(st.sp + 1), uint8_t(v >> 8));
            wr(st.sp, uint8_t(v));
            setIdxPair(mode, t);
            st.wz = t;
            cyc += 19;
            break;
        }
        case 5: {                      // EX DE,HL — never IX/IY
            uint16_t t = de();
            setDE(hl());
            setHL(t);
            cyc += 4;
            break;
        }
        case 6:                        // DI
            st.iff1 = st.iff2 = false;
            cyc += 4;
            break;
        case 7:                        // EI — takes effect after next insn
            st.iff1 = st.iff2 = true;
            st.afterEi = true;
            cyc += 4;
            break;
        }
        break;

    case 4: {                          // CALL cc,nn
        uint16_t nn = fetch16();
        st.wz = nn;
        if (ccTest(y)) {
            push16(st.pc);
            st.pc = nn;
            cyc += 17;
        } else {
            cyc += 10;
        }
        break;
    }

    case 5:
        if (q == 0) {                  // PUSH rp2
            uint16_t v = 0;
            switch (p) {
            case 0: v = bc(); break;
            case 1: v = de(); break;
            case 2: v = idxPair(mode); break;
            case 3: v = uint16_t((st.a << 8) | st.f); break;
            }
            push16(v);
            cyc += 11;
        } else {                       // CALL nn (p==0; 1..3 are prefixes)
            uint16_t nn = fetch16();
            st.wz = nn;
            push16(st.pc);
            st.pc = nn;
            cyc += 17;
        }
        break;

    case 6:                            // ALU A,n
        aluDispatch(y, fetch8());
        cyc += 7;
        break;

    case 7:                            // RST y*8
        push16(st.pc);
        st.pc = uint16_t(y * 8);
        st.wz = st.pc;
        cyc += 11;
        break;
    }
}

// ── CB page ──────────────────────────────────────────────────────────

void Z80::execCB()
{
    uint8_t op = fetchOp();
    bumpR();
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;

    if (z == 6) {
        uint16_t ea = hl();
        uint8_t v = rd(ea);
        if (x != 1) ++busT_;           // the read M-cycle is 4 T before a write
        switch (x) {
        case 0: wr(ea, rotShift(y, v)); cyc += 15; break;
        case 1: bitMem(y, v, uint8_t(st.wz >> 8)); cyc += 12; break;
        case 2: wr(ea, uint8_t(v & ~(1 << y))); cyc += 15; break;
        case 3: wr(ea, uint8_t(v | (1 << y)));  cyc += 15; break;
        }
    } else {
        uint8_t v = readReg(z, MODE_HL);
        switch (x) {
        case 0: writeReg(z, MODE_HL, rotShift(y, v)); break;
        case 1: bitReg(y, v); break;
        case 2: writeReg(z, MODE_HL, uint8_t(v & ~(1 << y))); break;
        case 3: writeReg(z, MODE_HL, uint8_t(v | (1 << y)));  break;
        }
        cyc += 8;
    }
}

void Z80::execIndexedCB(int mode)
{
    // DD CB d op — d comes BEFORE the sub-opcode, and neither byte is an
    // M1 fetch (R already bumped twice for DD + CB by step()).
    int8_t d = int8_t(fetch8());
    uint16_t base = (mode == MODE_IX) ? st.ix : st.iy;
    uint16_t ea = uint16_t(base + d);
    st.wz = ea;
    uint8_t op = fetch8();
    busT_ += 2;      // the op byte's M-cycle is 5 T (3 + 2 internal)
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;

    if (x == 1) {                      // BIT n,(IX+d) — every z alias
        bitMem(y, rd(ea), uint8_t(ea >> 8));
        cyc += 16;                     // 20 total with the prefix's 4
        return;
    }

    uint8_t v = rd(ea);
    uint8_t res = 0;
    switch (x) {
    case 0: res = rotShift(y, v); break;
    case 2: res = uint8_t(v & ~(1 << y)); break;
    case 3: res = uint8_t(v | (1 << y));  break;
    }
    ++busT_;                           // the read M-cycle is 4 T before the write
    wr(ea, res);
    if (z != 6)                        // undocumented: result also lands in r
        writeReg(z, MODE_HL, res);
    cyc += 19;                         // 23 total with the prefix's 4
}

// ── ED page ──────────────────────────────────────────────────────────

// Verbatim port of MAME `z80.cpp:580-604` block_io_interrupted_flags(),
// which the inir/otir/indr/otdr macros call after `PC -= 2`
// (`z80.lst:769-880`). The REPEATING block-I/O instructions do NOT simply
// leave the per-iteration INI/OUTI flags in place: on every iteration that
// still has work to do, X/Y come from PCH (as for LDIR/CPIR) and H + P/V are
// re-derived from B, the carry, and bit 7 of the byte just transferred.
//
// zexdoc/zexall cannot reach this: they run under CP/M and never execute an
// I/O block instruction, so POM2 shipped the non-repeating flag formula for
// the repeating opcodes. Tom Harte `z80/v1/{ed b2,ed b3,ed ba,ed bb}` fails
// ~99.5% of vectors without this.
void Z80::blockIoInterruptedFlags(uint8_t ioByte)
{
    // PC has already been rewound to the instruction, so this is the same
    // "X/Y from PCH on a repeat" rule the LDIR/CPIR branch uses.
    st.f = uint8_t((st.f & ~(F::X | F::Y)) | ((st.pc >> 8) & (F::X | F::Y)));

    const uint8_t pvOld = uint8_t(st.f & F::PV);
    uint8_t pvNew;
    if (st.f & F::C) {
        // Carry set: the ±1 that the decrement carried out of the low nibble
        // also perturbs the parity source, and H reports that nibble edge.
        uint8_t h = 0;
        if (ioByte & 0x80) {
            pvNew = uint8_t(szpTable[(st.b - 1) & 0x07] & F::PV);
            if ((st.b & 0x0F) == 0x00) h = F::H;
        } else {
            pvNew = uint8_t(szpTable[(st.b + 1) & 0x07] & F::PV);
            if ((st.b & 0x0F) == 0x0F) h = F::H;
        }
        st.f = uint8_t((st.f & ~F::H) | h);
    } else {
        pvNew = uint8_t(szpTable[st.b & 0x07] & F::PV);
    }
    // MAME stores `(pv_old ^ pv()) & PF` into its LAZY `pv_val` field, whose
    // getter re-parities the stored byte — so the flag that actually reaches
    // get_f() is the INVERSE of that xor: P/V lands SET when old and new
    // agree. Written out directly here since POM2's F is not lazy.
    st.f = uint8_t((st.f & ~F::PV) | ((pvOld == pvNew) ? F::PV : 0));
}

void Z80::execED()
{
    uint8_t op = fetchOp();
    bumpR();
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;
    const int p = y >> 1;
    const int q = y & 1;

    if (x == 1) {
        switch (z) {
        case 0: {                      // IN r,(C) — y==6: flags only (ED 70)
            // WZ latches the PORT address + 1 — capture it before the
            // register write: IN B,(C) / IN C,(C) modify BC itself.
            const uint16_t port = bc();
            uint8_t v = bus.z80IoRead(port);
            st.f = uint8_t((st.f & F::C) | szpTable[v]);
            if (y != 6)
                writeReg(y, MODE_HL, v);
            st.wz = uint16_t(port + 1);
            cyc += 12;
            break;
        }
        case 1:                        // OUT (C),r — y==6: OUT (C),0 (NMOS)
            bus.z80IoWrite(bc(), (y == 6) ? 0 : readReg(y, MODE_HL));
            st.wz = uint16_t(bc() + 1);
            cyc += 12;
            break;
        case 2: {                      // SBC/ADC HL,rp
            uint16_t src = 0;
            switch (p) {
            case 0: src = bc(); break;
            case 1: src = de(); break;
            case 2: src = hl(); break;
            case 3: src = st.sp; break;
            }
            if (q == 0) sbc16(src);
            else        adc16(src);
            cyc += 15;
            break;
        }
        case 3: {                      // LD (nn),rp / LD rp,(nn)
            uint16_t nn = fetch16();
            if (q == 0) {
                uint16_t v = 0;
                switch (p) {
                case 0: v = bc(); break;
                case 1: v = de(); break;
                case 2: v = hl(); break;
                case 3: v = st.sp; break;
                }
                wr16(nn, v);
            } else {
                uint16_t v = rd16(nn);
                switch (p) {
                case 0: setBC(v); break;
                case 1: setDE(v); break;
                case 2: setHL(v); break;
                case 3: st.sp = v; break;
                }
            }
            st.wz = uint16_t(nn + 1);
            cyc += 20;
            break;
        }
        case 4: {                      // NEG (all y aliases)
            uint8_t v = st.a;
            st.a = 0;
            sub8(v, 0);
            cyc += 8;
            break;
        }
        case 5:                        // RETN / RETI (all y aliases)
            st.iff1 = st.iff2;
            st.pc = pop16();
            st.wz = st.pc;
            cyc += 14;
            break;
        case 6:                        // IM 0/1/2 (with aliases)
            switch (y & 3) {
            case 2:  st.im = 1; break;
            case 3:  st.im = 2; break;
            default: st.im = 0; break;
            }
            cyc += 8;
            break;
        case 7:
            switch (y) {
            case 0:                    // LD I,A
                st.i = st.a;
                cyc += 9;
                break;
            case 1:                    // LD R,A
                st.r = st.a;
                cyc += 9;
                break;
            case 2: {                  // LD A,I
                st.a = st.i;
                st.f = uint8_t((st.f & F::C) | (st.a & (F::S | F::X | F::Y))
                               | (st.a == 0 ? F::Z : 0) | (st.iff2 ? F::PV : 0));
                cyc += 9;
                break;
            }
            case 3: {                  // LD A,R
                st.a = st.r;
                st.f = uint8_t((st.f & F::C) | (st.a & (F::S | F::X | F::Y))
                               | (st.a == 0 ? F::Z : 0) | (st.iff2 ? F::PV : 0));
                cyc += 9;
                break;
            }
            case 4: {                  // RRD
                uint8_t v = rd(hl());
                busT_ += 4;            // 4 internal T between read and write
                wr(hl(), uint8_t((v >> 4) | (st.a << 4)));
                st.a = uint8_t((st.a & 0xF0) | (v & 0x0F));
                st.f = uint8_t((st.f & F::C) | szpTable[st.a]);
                st.wz = uint16_t(hl() + 1);
                cyc += 18;
                break;
            }
            case 5: {                  // RLD
                uint8_t v = rd(hl());
                busT_ += 4;            // 4 internal T between read and write
                wr(hl(), uint8_t((v << 4) | (st.a & 0x0F)));
                st.a = uint8_t((st.a & 0xF0) | (v >> 4));
                st.f = uint8_t((st.f & F::C) | szpTable[st.a]);
                st.wz = uint16_t(hl() + 1);
                cyc += 18;
                break;
            }
            default:                   // ED 77/7F — NONI
                cyc += 8;
                break;
            }
            break;
        }
        return;
    }

    if (x == 2 && z <= 3 && y >= 4) {  // block transfer / search / I/O
        const bool increment = (y & 1) == 0;   // A0/A2… vs A8/AA…
        const bool repeat    = y >= 6;         // B0… vs A0…
        const int  delta     = increment ? 1 : -1;

        switch (z) {
        case 0: {                      // LDI / LDD / LDIR / LDDR
            uint8_t v = rd(hl());
            wr(de(), v);
            setHL(uint16_t(hl() + delta));
            setDE(uint16_t(de() + delta));
            setBC(uint16_t(bc() - 1));
            uint8_t n = uint8_t(st.a + v);
            st.f = uint8_t((st.f & (F::S | F::Z | F::C))
                           | (bc() ? F::PV : 0)
                           | (n & F::X) | ((n & 0x02) ? F::Y : 0));
            if (repeat && bc()) {
                st.pc = uint16_t(st.pc - 2);
                st.wz = uint16_t(st.pc + 1);
                // MAME `z80.lst` ldir/lddr repeat branch: `m_f.yx_val =
                // PC >> 8` — on a REPEATING iteration the undocumented
                // X/Y flags come from PCH, not from the per-iteration
                // data formula above. POM2 kept the data-derived value,
                // so F was wrong for the whole run of the instruction
                // (visible to any code that PUSH AF / POP AF around it,
                // and to flag-exactness test suites).
                st.f = uint8_t((st.f & ~(F::X | F::Y))
                               | ((st.pc >> 8) & (F::X | F::Y)));
                cyc += 21;
            } else {
                cyc += 16;
            }
            break;
        }
        case 1: {                      // CPI / CPD / CPIR / CPDR
            uint8_t v = rd(hl());
            int res = st.a - v;
            uint8_t r8 = uint8_t(res);
            bool halfBorrow = ((st.a ^ v ^ r8) & 0x10) != 0;
            setHL(uint16_t(hl() + delta));
            setBC(uint16_t(bc() - 1));
            uint8_t n = uint8_t(r8 - (halfBorrow ? 1 : 0));
            st.f = uint8_t((st.f & F::C) | F::N
                           | (r8 & F::S) | (r8 == 0 ? F::Z : 0)
                           | (halfBorrow ? F::H : 0)
                           | (bc() ? F::PV : 0)
                           | (n & F::X) | ((n & 0x02) ? F::Y : 0));
            st.wz = uint16_t(st.wz + delta);
            if (repeat && bc() && r8 != 0) {
                st.pc = uint16_t(st.pc - 2);
                st.wz = uint16_t(st.pc + 1);
                // Same PCH override as LDIR/LDDR — MAME `z80.lst`
                // cpir/cpdr repeat branch (`m_f.yx_val = PC >> 8`).
                st.f = uint8_t((st.f & ~(F::X | F::Y))
                               | ((st.pc >> 8) & (F::X | F::Y)));
                cyc += 21;
            } else {
                cyc += 16;
            }
            break;
        }
        case 2: {                      // INI / IND / INIR / INDR
            // (repeat branch flag fixup: see blockIoInterruptedFlags)
            st.wz = uint16_t(bc() + delta);
            uint8_t v = bus.z80IoRead(bc());
            wr(hl(), v);
            setHL(uint16_t(hl() + delta));
            st.b--;
            int t = v + ((st.c + delta) & 0xFF);
            st.f = uint8_t((st.b & (F::S | F::X | F::Y)) | (st.b == 0 ? F::Z : 0)
                           | ((v & 0x80) ? F::N : 0)
                           | ((t > 0xFF) ? (F::H | F::C) : 0)
                           | (szpTable[(t & 7) ^ st.b] & F::PV));
            if (repeat && st.b) {
                st.pc = uint16_t(st.pc - 2);
                st.wz = uint16_t(st.pc + 1);
                blockIoInterruptedFlags(v);
                cyc += 21;
            } else {
                cyc += 16;
            }
            break;
        }
        case 3: {                      // OUTI / OUTD / OTIR / OTDR
            uint8_t v = rd(hl());
            st.b--;
            bus.z80IoWrite(bc(), v);
            setHL(uint16_t(hl() + delta));
            st.wz = uint16_t(bc() + delta);
            int t = v + st.l;
            st.f = uint8_t((st.b & (F::S | F::X | F::Y)) | (st.b == 0 ? F::Z : 0)
                           | ((v & 0x80) ? F::N : 0)
                           | ((t > 0xFF) ? (F::H | F::C) : 0)
                           | (szpTable[(t & 7) ^ st.b] & F::PV));
            if (repeat && st.b) {
                st.pc = uint16_t(st.pc - 2);
                st.wz = uint16_t(st.pc + 1);
                blockIoInterruptedFlags(v);
                cyc += 21;
            } else {
                cyc += 16;
            }
            break;
        }
        }
        return;
    }

    // Every other ED slot is a 2-byte NOP (NONI).
    cyc += 8;
}

} // namespace pom2
