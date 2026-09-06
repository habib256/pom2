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

// M68705P3 — see header.
//
// This is a verbatim port of MAME's `src/devices/cpu/m6805/m6805.cpp` +
// `m68705.cpp` HMOS variant. Every instruction implementation mirrors
// `src/devices/cpu/m6805/6805ops.hxx`, every addressing-mode helper
// mirrors `m6805defs.h`, and every interrupt / port / timer behaviour is
// drawn from `m68705.cpp` lines 220-1053. The cycle counts are MAME's
// `s_hmos_cycles[]` table (mouse.cpp's M68705P3 instance uses the HMOS
// "small" address-space variant — 11-bit PC, sp_mask=$7F, sp_floor=$60).
//
// What is NOT modelled (deliberate, scope-bounded omissions):
//
//   * EPROM programming via PCR ($000B). The runtime emulator never
//     programs the EPROM — the firmware is a fixed input.
//   * The MOR (Mask Option Register) at offset $0784 inside the EPROM.
//     MAME consults this for timer source / divisor configuration; on
//     the Apple Mouse Card the firmware sets up TCR explicitly so the
//     MOR is irrelevant here. We default to TIMER_PGM (programmable),
//     matching the M68705 EPROM family.
//   * STOP / WAIT instructions. MAME aborts on these (`fatalerror`).
//     We park PC on the instruction so it's diagnosable but never
//     advance — the mouse firmware never executes either.
//   * The bootstrap ROM at $07F8 of the user EPROM space (one entry).
//     Loaded as part of the same 2 KB firmware image but never given
//     special treatment.

#include "M68705P3.h"
#include "Logger.h"

#include <cstdio>
#include <cstring>
#include <fstream>

// ─── Cycle count table (MAME `s_hmos_cycles[]`, with XX = 4) ─────────────
const uint8_t M68705P3::kHmosCycles[256] = {
    /* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
    /*0*/ 10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,
    /*1*/  7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    /*2*/  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    /*3*/  6, 4, 4, 6, 6, 4, 6, 6, 6, 6, 6, 4, 6, 6, 4, 6,
    /*4*/  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    /*5*/  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    /*6*/  7, 4, 4, 7, 7, 4, 7, 7, 7, 7, 7, 4, 7, 7, 4, 7,
    /*7*/  6, 4, 4, 6, 6, 4, 6, 6, 6, 6, 6, 4, 6, 6, 4, 6,
    /*8*/  9, 6, 4,11, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    /*9*/  4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 4, 2,
    /*A*/  2, 2, 2, 2, 2, 2, 2, 4, 2, 2, 2, 2, 4, 8, 2, 4,
    /*B*/  4, 4, 4, 4, 4, 4, 4, 5, 4, 4, 4, 4, 3, 7, 4, 5,
    /*C*/  5, 5, 5, 5, 5, 5, 5, 6, 5, 5, 5, 5, 4, 8, 5, 6,
    /*D*/  6, 6, 6, 6, 6, 6, 6, 7, 6, 6, 6, 6, 5, 9, 6, 7,
    /*E*/  5, 5, 5, 5, 5, 5, 5, 6, 5, 5, 5, 5, 4, 8, 5, 6,
    /*F*/  4, 4, 4, 4, 4, 4, 4, 5, 4, 4, 4, 4, 3, 7, 4, 5,
};

M68705P3::M68705P3()
{
    // Port C is 4 bits wide on the P3 (bits 7..4 forced to 0).
    ports[0].mask = 0x00;
    ports[1].mask = 0x00;
    ports[2].mask = 0xF0;
}

bool M68705P3::loadRomFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        pom2::log().warn("M68705",
            "Cannot open EPROM file: " + path);
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size != 0x800) {     // 2048 bytes — chip address $0000 not stored
        pom2::log().warn("M68705",
            "EPROM size mismatch (" + std::to_string(size) +
            " bytes, expected 2048): " + path);
        return false;
    }
    std::vector<uint8_t> buf(0x800);
    f.read(reinterpret_cast<char*>(buf.data()), 0x800);
    if (!f) {
        pom2::log().warn("M68705", "Short read on EPROM file: " + path);
        return false;
    }
    // The 2 KB image is the chip's full address space; we keep only the
    // upper 0x780 bytes ($0080-$07FF) since the lower portion overlaps
    // I/O regs and RAM and is never executable anyway.
    std::memcpy(eprom.data(), buf.data() + 0x80, 0x780);
    romLoaded = true;
    pom2::log().info("M68705", "Loaded EPROM " + path + " (2048 bytes)");
    return true;
}

bool M68705P3::loadRomBytes(const uint8_t* data, std::size_t len)
{
    if (len != 0x800) return false;
    std::memcpy(eprom.data(), data + 0x80, 0x780);
    romLoaded = true;
    return true;
}

void M68705P3::reset()
{
    reg.A  = 0;
    reg.X  = 0;
    reg.S  = kSpMask;
    reg.CC = IFLAG;     // I set on reset
    reg.PC = 0;
    pending_interrupts = 0;
    irq_line_state = false;
    icount = 0;

    // Reset port state.
    for (auto& p : ports) {
        p.latch = 0xFF;
        p.ddr   = 0x00;
        p.input = 0xFF;
    }
    // Reset timer.
    timer.tdr      = 0xFF;
    timer.tcr      = 0x7F;
    timer.prescale = 0x7F;
    timer.divisor  = 7;

    // Fetch reset vector from $07FE/$07FF (big-endian).
    if (romLoaded) {
        const uint8_t hi = rdmem(kRstVector);
        const uint8_t lo = rdmem(kRstVector + 1);
        reg.PC = static_cast<uint16_t>((hi << 8) | lo) & kAddrMask;
    } else {
        reg.PC = 0;
    }
}

// ─── Bus ────────────────────────────────────────────────────────────────

uint8_t M68705P3::rdmem(uint16_t addr)
{
    addr &= kAddrMask;
    // I/O regs.
    switch (addr) {
        case 0x0000: return readPortPin(0);
        case 0x0001: return readPortPin(1);
        case 0x0002: return readPortPin(2);
        case 0x0004:
        case 0x0005:
        case 0x0006:
            // DDRs are write-only; reads return $FF (open bus convention).
            return 0xFF;
        case 0x0008: return timer.tdr;
        case 0x0009:
            // PSC bit reads as 0 for non-MOR chips; we model TIMER_PGM.
            return timer.tcr & ~0x08;
        default: break;
    }
    if (addr >= 0x10 && addr <= 0x7F) return ram[addr - 0x10];
    if (addr >= 0x80) return eprom[addr - 0x80];
    return 0xFF;
}

void M68705P3::wrmem(uint16_t addr, uint8_t v)
{
    addr &= kAddrMask;
    switch (addr) {
        case 0x0000: writePortLatch(0, v); return;
        case 0x0001: writePortLatch(1, v); return;
        case 0x0002: writePortLatch(2, v); return;
        case 0x0004: writePortDdr(0, v); return;
        case 0x0005: writePortDdr(1, v); return;
        case 0x0006: writePortDdr(2, v); return;
        case 0x0008: timer.tdr = v; return;
        case 0x0009: timerWriteTcr(v); return;
        default: break;
    }
    if (addr >= 0x10 && addr <= 0x7F) { ram[addr - 0x10] = v; return; }
    // Writes to the EPROM region are silently dropped at run-time (the
    // PCR-gated programming path is not modelled — see header).
}

// ─── Stack helpers ──────────────────────────────────────────────────────

void M68705P3::pushbyte(uint8_t v)
{
    wrmem(reg.S, v);
    if (reg.S == kSpFloor) reg.S = kSpMask;
    else                   --reg.S;
}

void M68705P3::pushword(uint16_t w)
{
    pushbyte(static_cast<uint8_t>(w & 0xFF));        // PCL
    pushbyte(static_cast<uint8_t>((w >> 8) & 0xFF)); // PCH
}

uint8_t M68705P3::pullbyte()
{
    if (reg.S == kSpMask) reg.S = kSpFloor;
    else                  ++reg.S;
    return rdmem(reg.S);
}

uint16_t M68705P3::pullword()
{
    const uint8_t hi = pullbyte();
    const uint8_t lo = pullbyte();
    return static_cast<uint16_t>((hi << 8) | lo);
}

// ─── Addressing modes ───────────────────────────────────────────────────

uint16_t M68705P3::direct_ea()
{
    const uint16_t a = reg.PC;
    reg.PC = (reg.PC + 1) & kAddrMask;
    return rdmem(a);
}

uint16_t M68705P3::extended_ea()
{
    const uint16_t hi = rdmem(reg.PC);
    reg.PC = (reg.PC + 1) & kAddrMask;
    const uint16_t lo = rdmem(reg.PC);
    reg.PC = (reg.PC + 1) & kAddrMask;
    return static_cast<uint16_t>((hi << 8) | lo);
}

uint16_t M68705P3::indexed1_ea()
{
    const uint16_t off = rdmem(reg.PC);
    reg.PC = (reg.PC + 1) & kAddrMask;
    return static_cast<uint16_t>(off + reg.X);
}

uint16_t M68705P3::indexed2_ea()
{
    const uint16_t hi = rdmem(reg.PC);
    reg.PC = (reg.PC + 1) & kAddrMask;
    const uint16_t lo = rdmem(reg.PC);
    reg.PC = (reg.PC + 1) & kAddrMask;
    return static_cast<uint16_t>(((hi << 8) | lo) + reg.X);
}

// ─── Ports ──────────────────────────────────────────────────────────────

void M68705P3::writePortLatch(int n, uint8_t v)
{
    ports[n].latch = static_cast<uint8_t>(v & ~ports[n].mask);
    if (portWriteCb) portWriteCb(n);
}

void M68705P3::writePortDdr(int n, uint8_t v)
{
    ports[n].ddr = static_cast<uint8_t>(v & ~ports[n].mask);
    if (portWriteCb) portWriteCb(n);
}

uint8_t M68705P3::readPortPin(int n)
{
    // Refresh input from external host BEFORE composing the read. The
    // mouse card uses this to synthesise quadrature edges per call.
    if (portReadCb) ports[n].input = portReadCb(n);

    // MAME formula: mask | (latch & DDR) | (input & ~DDR)
    // For Port A which is open-drain with internal pull-ups, "input" is
    // already 0xFF when nothing pulls the pin low — we don't model the
    // open-drain semantics explicitly because the mouse-card firmware
    // always sets DDR before reading and the PIA / MCU bridge drives
    // input pins explicitly via setPortInput().
    return static_cast<uint8_t>(ports[n].mask
        | (ports[n].latch & ports[n].ddr)
        | (ports[n].input & ~ports[n].ddr));
}

void M68705P3::setPortInput(int port, uint8_t value)
{
    if (port < 0 || port >= 3) return;
    ports[port].input = static_cast<uint8_t>(value & ~ports[port].mask);
}

uint8_t M68705P3::getPortPins(int port) const
{
    if (port < 0 || port >= 3) return 0xFF;
    // Output bits surface the latch; input bits float high (open
    // drain w/ pull-up). External observers see this composite.
    const uint8_t outBits = static_cast<uint8_t>(ports[port].latch & ports[port].ddr);
    const uint8_t inBits  = static_cast<uint8_t>(0xFF & ~ports[port].ddr);
    return static_cast<uint8_t>((outBits | inBits) & ~ports[port].mask);
}

// ─── Timer ──────────────────────────────────────────────────────────────

void M68705P3::timerWriteTcr(uint8_t v)
{
    // MAME's tcr_w with TIMER_PGM (programmable):
    //   set_divisor(v & TCR_PS);
    //   set_source(timer_source((v & (TCR_TIN | TCR_TIE)) >> 4));
    timer.divisor = v & 0x07;
    // Source field decoded from TCR bits 4..5; for the mouse card the
    // 68705 uses internal clock as timer source (CLOCK = 0). We honour
    // the bits so the firmware's timer config is round-trippable, but
    // we always tick from the internal clock — external timer pin is
    // not wired on the mouse card.
    if (v & 0x08) timer.prescale = 0;     // TCR_PSC: prescaler clear
    timer.tcr = static_cast<uint8_t>((timer.tcr & (v & 0x80)) | (v & ~(0x80 | 0x08)));
    // TIM bit gates the timer interrupt request. MAME models this as a
    // LEVEL-sensitive line: tcr_w calls set_input_line(M6805_INT_TIMER,
    // (m_tcr & TCR_TIR) && !(m_tcr & TCR_TIM)), which both asserts AND
    // de-asserts. So a TCR write that masks (TIM=1) or acknowledges (TIR=0)
    // the request must immediately drop the pending bit — otherwise a
    // request latched while I was set re-enters the ISR spuriously.
    if ((timer.tcr & 0x80) && !(timer.tcr & 0x40)) {
        pending_interrupts |= (1u << 1);     // bit 1 = TIMER
    } else {
        pending_interrupts &= ~(1u << 1);
    }
}

void M68705P3::timerUpdate(unsigned count)
{
    // Compute new prescaler value and counter decrements.
    const unsigned prescale = (timer.prescale & ((1u << timer.divisor) - 1u)) + count;
    const unsigned decrements = prescale >> timer.divisor;
    const bool zero_cross = (timer.tdr ? unsigned(timer.tdr) : 256u) <= decrements;

    timer.prescale = static_cast<uint8_t>(prescale & 0x7F);
    timer.tdr = static_cast<uint8_t>(timer.tdr - decrements);

    if (zero_cross) {
        timer.tcr |= 0x80;     // TCR_TIR: timer interrupt request
        if (!(timer.tcr & 0x40)) {     // TCR_TIM clear → request line up
            pending_interrupts |= (1u << 1);
        }
    }
}

// ─── External pins ──────────────────────────────────────────────────────

void M68705P3::setIrq(bool asserted)
{
    if (asserted == irq_line_state) return;
    irq_line_state = asserted;
    if (asserted) pending_interrupts |= (1u << 0);
}

// ─── Interrupt service ──────────────────────────────────────────────────

void M68705P3::serviceInterrupt()
{
    // Push PCH, PCL, X, A, CC (low-to-high addresses).
    pushword(reg.PC);
    pushbyte(reg.X);
    pushbyte(reg.A);
    pushbyte(reg.CC);
    reg.CC |= IFLAG;

    uint16_t vec = kIntVector;
    if (pending_interrupts & (1u << 0)) {
        vec = kIntVector;
        pending_interrupts &= ~(1u << 0);
    } else if (pending_interrupts & (1u << 1)) {
        vec = kTmrVector;
        // LEVEL-sensitive line (MAME set_input_line in tcr_w/timer
        // expiry): taking the vector does NOT clear the request — the
        // line stays asserted while TCR.TIR=1 && TCR.TIM=0, so an ISR
        // that returns without acknowledging (clearing TIR) re-enters
        // after RTI, exactly as on real silicon. Dropping the pending
        // bit here turned the line edge-triggered.
        if (!((timer.tcr & 0x80) && !(timer.tcr & 0x40)))
            pending_interrupts &= ~(1u << 1);
    }
    const uint8_t hi = rdmem(vec);
    const uint8_t lo = rdmem(vec + 1);
    reg.PC = static_cast<uint16_t>((hi << 8) | lo) & kAddrMask;

    icount -= 11;
    timerUpdate(11);
}

// ─── Instruction execution ──────────────────────────────────────────────
//
// One big switch on opcode. Implementations follow MAME's 6805ops.hxx
// verbatim (modulo the C++-template→runtime translation). Each opcode
// handler reads its own operands via the addressing-mode helpers above
// and updates the register state.

namespace {

// Sign-extend an 8-bit value to a signed 16-bit offset.
inline int16_t sext8(uint8_t b) { return int16_t(int8_t(b)); }

}  // namespace

int M68705P3::step()
{
    // Service pending interrupt if I-flag clear.
    if (pending_interrupts && !(reg.CC & IFLAG)) {
        serviceInterrupt();
    }

    const uint8_t op = rdmem(reg.PC);
    reg.PC = (reg.PC + 1) & kAddrMask;

    // Helpers (lambdas) for read-modify-write on a memory location and
    // for branch-on-condition. Captured by reference so they read/write
    // `reg` directly.
    auto rmw_mem = [&](uint16_t ea, auto fn) {
        uint8_t t = rdmem(ea);
        t = fn(t);
        wrmem(ea, t);
    };
    auto rmw_reg = [&](uint8_t& r, auto fn) {
        r = fn(r);
    };
    auto branch = [&](bool cond) {
        const uint8_t t = rdmem(reg.PC);
        reg.PC = (reg.PC + 1) & kAddrMask;
        if (cond) reg.PC = static_cast<uint16_t>((reg.PC + sext8(t)) & kAddrMask);
    };
    auto bit_branch = [&](unsigned B, bool wantSet) {
        // BRSET / BRCLR: read direct byte, read offset, branch if bit B
        // matches `wantSet`. Sets/clears C accordingly per MAME semantics.
        const uint16_t ea = direct_ea();
        const uint8_t r = rdmem(ea);
        const uint8_t t = rdmem(reg.PC);
        reg.PC = (reg.PC + 1) & kAddrMask;
        const bool bit_set = (r >> B) & 1;
        if (wantSet) {
            // BRSET: SEC if bit set; branch if (bit set == wantSet)
            reg.CC = (bit_set) ? (reg.CC | CFLAG) : (reg.CC & ~CFLAG);
            if (bit_set) reg.PC = static_cast<uint16_t>((reg.PC + sext8(t)) & kAddrMask);
        } else {
            // BRCLR: SEC if bit set, branch if bit CLEAR
            reg.CC = (bit_set) ? (reg.CC | CFLAG) : (reg.CC & ~CFLAG);
            if (!bit_set) reg.PC = static_cast<uint16_t>((reg.PC + sext8(t)) & kAddrMask);
        }
    };
    auto bset_op = [&](unsigned B) {
        const uint16_t ea = direct_ea();
        wrmem(ea, static_cast<uint8_t>(rdmem(ea) | (1u << B)));
    };
    auto bclr_op = [&](unsigned B) {
        const uint16_t ea = direct_ea();
        wrmem(ea, static_cast<uint8_t>(rdmem(ea) & ~(1u << B)));
    };

    // Opcode-handler closures. Use the addressing-mode helpers to
    // derive the effective address / operand byte first, then dispatch
    // the ALU op.
    auto neg_op = [&](uint8_t t) {
        const uint16_t r = static_cast<uint16_t>(0u - t);
        clr_nzc();
        set_nzc8(r);
        return static_cast<uint8_t>(r);
    };
    auto com_op = [&](uint8_t t) {
        t = static_cast<uint8_t>(~t);
        clr_nz();
        set_nz8(t);
        reg.CC |= CFLAG;
        return t;
    };
    auto lsr_op = [&](uint8_t t) {
        clr_nzc();
        if (t & 1) reg.CC |= CFLAG;
        t = static_cast<uint8_t>(t >> 1);
        set_z8(t);
        return t;
    };
    auto ror_op = [&](uint8_t t) {
        const uint8_t r = static_cast<uint8_t>(((reg.CC & CFLAG) << 7) | (t >> 1));
        clr_nzc();
        if (t & 1) reg.CC |= CFLAG;
        set_nz8(r);
        return r;
    };
    auto asr_op = [&](uint8_t t) {
        clr_nzc();
        if (t & 1) reg.CC |= CFLAG;
        t = static_cast<uint8_t>((t >> 1) | (t & 0x80));
        set_nz8(t);
        return t;
    };
    auto lsl_op = [&](uint8_t t) {
        const uint16_t r = static_cast<uint16_t>(t) << 1;
        clr_nzc();
        set_nzc8(r);
        return static_cast<uint8_t>(r);
    };
    auto rol_op = [&](uint8_t t) {
        const uint16_t r = static_cast<uint16_t>((reg.CC & CFLAG) | (t << 1));
        clr_nzc();
        set_nzc8(r);
        return static_cast<uint8_t>(r);
    };
    auto dec_op = [&](uint8_t t) {
        --t;
        clr_nz();
        set_nz8(t);
        return t;
    };
    auto inc_op = [&](uint8_t t) {
        ++t;
        clr_nz();
        set_nz8(t);
        return t;
    };
    auto tst_op = [&](uint8_t t) {
        clr_nz();
        set_nz8(t);
        return t;
    };
    auto clr_op = [&](uint8_t /*t*/) {
        clr_nz();
        reg.CC |= ZFLAG;
        return uint8_t(0);
    };
    // 6805 CLR on a memory operand is WRITE-ONLY: it computes the EA and
    // stores 0 WITHOUT reading the location (MAME `clr` uses ARGADDR, not
    // ARGBYTE). Routing it through rmw_mem would do a spurious rdmem(ea),
    // firing read side-effects on the port latches ($00 Port A / $01 Port B
    // / $02 Port C) — e.g. an unintended quadrature-edge read in the mouse
    // bridge. (Register CLRA/CLRX go through rmw_reg and never read memory.)
    auto clr_mem = [&](uint16_t ea) { wrmem(ea, clr_op(0)); };

    auto suba_imm = [&](uint16_t t) {
        const uint16_t r = reg.A - t;
        clr_nzc();
        set_nzc8(r);
        reg.A = static_cast<uint8_t>(r);
    };
    auto cmpa_imm = [&](uint16_t t) {
        const uint16_t r = reg.A - t;
        clr_nzc();
        set_nzc8(r);
    };
    auto sbca_imm = [&](uint16_t t) {
        const uint16_t r = reg.A - t - (reg.CC & CFLAG ? 1u : 0u);
        clr_nzc();
        set_nzc8(r);
        reg.A = static_cast<uint8_t>(r);
    };
    auto cpx_imm = [&](uint16_t t) {
        const uint16_t r = reg.X - t;
        clr_nzc();
        set_nzc8(r);
    };
    auto anda_imm = [&](uint16_t t) {
        reg.A = static_cast<uint8_t>(reg.A & t);
        clr_nz();
        set_nz8(reg.A);
    };
    auto bita_imm = [&](uint16_t t) {
        const uint8_t r = static_cast<uint8_t>(reg.A & t);
        clr_nz();
        set_nz8(r);
    };
    auto lda_imm = [&](uint16_t t) {
        reg.A = static_cast<uint8_t>(t);
        clr_nz();
        set_nz8(reg.A);
    };
    auto eora_imm = [&](uint16_t t) {
        reg.A = static_cast<uint8_t>(reg.A ^ t);
        clr_nz();
        set_nz8(reg.A);
    };
    auto adca_imm = [&](uint16_t t) {
        const uint16_t r = reg.A + t + (reg.CC & CFLAG ? 1u : 0u);
        clr_hnzc();
        set_hnzc8(reg.A, static_cast<uint8_t>(t), r);
        reg.A = static_cast<uint8_t>(r);
    };
    auto ora_imm = [&](uint16_t t) {
        reg.A = static_cast<uint8_t>(reg.A | t);
        clr_nz();
        set_nz8(reg.A);
    };
    auto adda_imm = [&](uint16_t t) {
        const uint16_t r = reg.A + t;
        clr_hnzc();
        set_hnzc8(reg.A, static_cast<uint8_t>(t), r);
        reg.A = static_cast<uint8_t>(r);
    };
    auto ldx_imm = [&](uint16_t t) {
        reg.X = static_cast<uint8_t>(t);
        clr_nz();
        set_nz8(reg.X);
    };

    // Opcode dispatch.
    switch (op) {
        // BRSET 0..7 / BRCLR 0..7 — $00..$0F (alternating).
        case 0x00: bit_branch(0, true);  break;
        case 0x01: bit_branch(0, false); break;
        case 0x02: bit_branch(1, true);  break;
        case 0x03: bit_branch(1, false); break;
        case 0x04: bit_branch(2, true);  break;
        case 0x05: bit_branch(2, false); break;
        case 0x06: bit_branch(3, true);  break;
        case 0x07: bit_branch(3, false); break;
        case 0x08: bit_branch(4, true);  break;
        case 0x09: bit_branch(4, false); break;
        case 0x0A: bit_branch(5, true);  break;
        case 0x0B: bit_branch(5, false); break;
        case 0x0C: bit_branch(6, true);  break;
        case 0x0D: bit_branch(6, false); break;
        case 0x0E: bit_branch(7, true);  break;
        case 0x0F: bit_branch(7, false); break;

        // BSET / BCLR — $10..$1F.
        case 0x10: bset_op(0); break;
        case 0x11: bclr_op(0); break;
        case 0x12: bset_op(1); break;
        case 0x13: bclr_op(1); break;
        case 0x14: bset_op(2); break;
        case 0x15: bclr_op(2); break;
        case 0x16: bset_op(3); break;
        case 0x17: bclr_op(3); break;
        case 0x18: bset_op(4); break;
        case 0x19: bclr_op(4); break;
        case 0x1A: bset_op(5); break;
        case 0x1B: bclr_op(5); break;
        case 0x1C: bset_op(6); break;
        case 0x1D: bclr_op(6); break;
        case 0x1E: bset_op(7); break;
        case 0x1F: bclr_op(7); break;

        // Branches — $20..$2F.
        case 0x20: branch(true); break;     // BRA
        case 0x21: branch(false); break;    // BRN
        case 0x22: branch(!(reg.CC & (CFLAG | ZFLAG))); break;     // BHI
        case 0x23: branch( (reg.CC & (CFLAG | ZFLAG))); break;     // BLS
        case 0x24: branch(!(reg.CC & CFLAG)); break;               // BCC
        case 0x25: branch( (reg.CC & CFLAG)); break;               // BCS
        case 0x26: branch(!(reg.CC & ZFLAG)); break;               // BNE
        case 0x27: branch( (reg.CC & ZFLAG)); break;               // BEQ
        case 0x28: branch(!(reg.CC & HFLAG)); break;               // BHCC
        case 0x29: branch( (reg.CC & HFLAG)); break;               // BHCS
        case 0x2A: branch(!(reg.CC & NFLAG)); break;               // BPL
        case 0x2B: branch( (reg.CC & NFLAG)); break;               // BMI
        case 0x2C: branch(!(reg.CC & IFLAG)); break;               // BMC
        case 0x2D: branch( (reg.CC & IFLAG)); break;               // BMS
        // BIL / BIH test the external interrupt line. The mouse card's
        // firmware doesn't use these — we model the IRQ pin as the only
        // input, so BIL = "is IRQ asserted".
        case 0x2E: branch(irq_line_state); break;                  // BIL
        case 0x2F: branch(!irq_line_state); break;                 // BIH

        // RMW direct — $30..$3F.
        case 0x30: rmw_mem(direct_ea(), neg_op); break;
        case 0x33: rmw_mem(direct_ea(), com_op); break;
        case 0x34: rmw_mem(direct_ea(), lsr_op); break;
        case 0x36: rmw_mem(direct_ea(), ror_op); break;
        case 0x37: rmw_mem(direct_ea(), asr_op); break;
        case 0x38: rmw_mem(direct_ea(), lsl_op); break;
        case 0x39: rmw_mem(direct_ea(), rol_op); break;
        case 0x3A: rmw_mem(direct_ea(), dec_op); break;
        case 0x3C: rmw_mem(direct_ea(), inc_op); break;
        case 0x3D: rmw_mem(direct_ea(), tst_op); break;
        case 0x3F: clr_mem(direct_ea()); break;

        // RMW A — $40..$4F.
        case 0x40: rmw_reg(reg.A, neg_op); break;
        case 0x42: {                            // MUL — X:A = X × A (HMOS op)
            const uint16_t r = static_cast<uint16_t>(reg.X) *
                               static_cast<uint16_t>(reg.A);
            clr_hc();                           // MUL clears H and C
            reg.X = static_cast<uint8_t>(r >> 8);
            reg.A = static_cast<uint8_t>(r & 0xFF);
            break;
        }
        case 0x43: rmw_reg(reg.A, com_op); break;
        case 0x44: rmw_reg(reg.A, lsr_op); break;
        case 0x46: rmw_reg(reg.A, ror_op); break;
        case 0x47: rmw_reg(reg.A, asr_op); break;
        case 0x48: rmw_reg(reg.A, lsl_op); break;
        case 0x49: rmw_reg(reg.A, rol_op); break;
        case 0x4A: rmw_reg(reg.A, dec_op); break;
        case 0x4C: rmw_reg(reg.A, inc_op); break;
        case 0x4D: rmw_reg(reg.A, tst_op); break;
        case 0x4F: rmw_reg(reg.A, clr_op); break;

        // RMW X — $50..$5F.
        case 0x50: rmw_reg(reg.X, neg_op); break;
        case 0x53: rmw_reg(reg.X, com_op); break;
        case 0x54: rmw_reg(reg.X, lsr_op); break;
        case 0x56: rmw_reg(reg.X, ror_op); break;
        case 0x57: rmw_reg(reg.X, asr_op); break;
        case 0x58: rmw_reg(reg.X, lsl_op); break;
        case 0x59: rmw_reg(reg.X, rol_op); break;
        case 0x5A: rmw_reg(reg.X, dec_op); break;
        case 0x5C: rmw_reg(reg.X, inc_op); break;
        case 0x5D: rmw_reg(reg.X, tst_op); break;
        case 0x5F: rmw_reg(reg.X, clr_op); break;

        // RMW indexed,1 byte offset — $60..$6F.
        case 0x60: rmw_mem(indexed1_ea(), neg_op); break;
        case 0x63: rmw_mem(indexed1_ea(), com_op); break;
        case 0x64: rmw_mem(indexed1_ea(), lsr_op); break;
        case 0x66: rmw_mem(indexed1_ea(), ror_op); break;
        case 0x67: rmw_mem(indexed1_ea(), asr_op); break;
        case 0x68: rmw_mem(indexed1_ea(), lsl_op); break;
        case 0x69: rmw_mem(indexed1_ea(), rol_op); break;
        case 0x6A: rmw_mem(indexed1_ea(), dec_op); break;
        case 0x6C: rmw_mem(indexed1_ea(), inc_op); break;
        case 0x6D: rmw_mem(indexed1_ea(), tst_op); break;
        case 0x6F: clr_mem(indexed1_ea()); break;

        // RMW indexed (no offset) — $70..$7F.
        case 0x70: rmw_mem(indexed_ea(), neg_op); break;
        case 0x73: rmw_mem(indexed_ea(), com_op); break;
        case 0x74: rmw_mem(indexed_ea(), lsr_op); break;
        case 0x76: rmw_mem(indexed_ea(), ror_op); break;
        case 0x77: rmw_mem(indexed_ea(), asr_op); break;
        case 0x78: rmw_mem(indexed_ea(), lsl_op); break;
        case 0x79: rmw_mem(indexed_ea(), rol_op); break;
        case 0x7A: rmw_mem(indexed_ea(), dec_op); break;
        case 0x7C: rmw_mem(indexed_ea(), inc_op); break;
        case 0x7D: rmw_mem(indexed_ea(), tst_op); break;
        case 0x7F: clr_mem(indexed_ea()); break;

        // Inherent — $80..$8F.
        case 0x80: { // RTI
            reg.CC = pullbyte();
            reg.A  = pullbyte();
            reg.X  = pullbyte();
            reg.PC = pullword() & kAddrMask;
            break;
        }
        case 0x81: { // RTS
            reg.PC = pullword() & kAddrMask;
            break;
        }
        case 0x83: { // SWI
            pushword(reg.PC);
            pushbyte(reg.X);
            pushbyte(reg.A);
            pushbyte(reg.CC);
            reg.CC |= IFLAG;
            const uint8_t hi = rdmem(kSwiVector);
            const uint8_t lo = rdmem(kSwiVector + 1);
            reg.PC = static_cast<uint16_t>((hi << 8) | lo) & kAddrMask;
            break;
        }
        // STOP / WAIT — park PC on the instruction (firmware never
        // reaches these on the mouse card; matches MAME's fatalerror
        // for diagnostic visibility without crashing the host).
        case 0x8E: case 0x8F:
            reg.PC = (reg.PC - 1) & kAddrMask;
            break;

        // Inherent — $97..$9F.
        case 0x97: reg.X = reg.A; break;     // TAX
        case 0x98: reg.CC &= ~CFLAG; break;  // CLC
        case 0x99: reg.CC |=  CFLAG; break;  // SEC
        case 0x9A: reg.CC &= ~IFLAG; break;  // CLI
        case 0x9B: reg.CC |=  IFLAG; break;  // SEI
        case 0x9C: reg.S = kSpMask; break;   // RSP
        case 0x9D: break;                    // NOP
        case 0x9F: reg.A = reg.X; break;     // TXA

        // Immediate — $A0..$AF.
        case 0xA0: suba_imm(rdmem(imm8_ea())); break;
        case 0xA1: cmpa_imm(rdmem(imm8_ea())); break;
        case 0xA2: sbca_imm(rdmem(imm8_ea())); break;
        case 0xA3: cpx_imm (rdmem(imm8_ea())); break;
        case 0xA4: anda_imm(rdmem(imm8_ea())); break;
        case 0xA5: bita_imm(rdmem(imm8_ea())); break;
        case 0xA6: lda_imm (rdmem(imm8_ea())); break;
        case 0xA8: eora_imm(rdmem(imm8_ea())); break;
        case 0xA9: adca_imm(rdmem(imm8_ea())); break;
        case 0xAA: ora_imm (rdmem(imm8_ea())); break;
        case 0xAB: adda_imm(rdmem(imm8_ea())); break;
        case 0xAD: { // BSR rel
            const uint8_t t = rdmem(reg.PC);
            reg.PC = (reg.PC + 1) & kAddrMask;
            pushword(reg.PC);
            reg.PC = static_cast<uint16_t>((reg.PC + sext8(t)) & kAddrMask);
            break;
        }
        case 0xAE: ldx_imm(rdmem(imm8_ea())); break;

        // Direct — $B0..$BF.
        case 0xB0: suba_imm(rdmem(direct_ea())); break;
        case 0xB1: cmpa_imm(rdmem(direct_ea())); break;
        case 0xB2: sbca_imm(rdmem(direct_ea())); break;
        case 0xB3: cpx_imm (rdmem(direct_ea())); break;
        case 0xB4: anda_imm(rdmem(direct_ea())); break;
        case 0xB5: bita_imm(rdmem(direct_ea())); break;
        case 0xB6: lda_imm (rdmem(direct_ea())); break;
        case 0xB7: { uint16_t ea = direct_ea(); clr_nz(); set_nz8(reg.A); wrmem(ea, reg.A); break; }
        case 0xB8: eora_imm(rdmem(direct_ea())); break;
        case 0xB9: adca_imm(rdmem(direct_ea())); break;
        case 0xBA: ora_imm (rdmem(direct_ea())); break;
        case 0xBB: adda_imm(rdmem(direct_ea())); break;
        case 0xBC: reg.PC = direct_ea() & kAddrMask; break;     // JMP dir
        case 0xBD: { uint16_t ea = direct_ea(); pushword(reg.PC); reg.PC = ea & kAddrMask; break; }
        case 0xBE: ldx_imm(rdmem(direct_ea())); break;
        case 0xBF: { uint16_t ea = direct_ea(); clr_nz(); set_nz8(reg.X); wrmem(ea, reg.X); break; }

        // Extended — $C0..$CF.
        case 0xC0: suba_imm(rdmem(extended_ea())); break;
        case 0xC1: cmpa_imm(rdmem(extended_ea())); break;
        case 0xC2: sbca_imm(rdmem(extended_ea())); break;
        case 0xC3: cpx_imm (rdmem(extended_ea())); break;
        case 0xC4: anda_imm(rdmem(extended_ea())); break;
        case 0xC5: bita_imm(rdmem(extended_ea())); break;
        case 0xC6: lda_imm (rdmem(extended_ea())); break;
        case 0xC7: { uint16_t ea = extended_ea(); clr_nz(); set_nz8(reg.A); wrmem(ea, reg.A); break; }
        case 0xC8: eora_imm(rdmem(extended_ea())); break;
        case 0xC9: adca_imm(rdmem(extended_ea())); break;
        case 0xCA: ora_imm (rdmem(extended_ea())); break;
        case 0xCB: adda_imm(rdmem(extended_ea())); break;
        case 0xCC: reg.PC = extended_ea() & kAddrMask; break;
        case 0xCD: { uint16_t ea = extended_ea(); pushword(reg.PC); reg.PC = ea & kAddrMask; break; }
        case 0xCE: ldx_imm(rdmem(extended_ea())); break;
        case 0xCF: { uint16_t ea = extended_ea(); clr_nz(); set_nz8(reg.X); wrmem(ea, reg.X); break; }

        // Indexed,2 byte offset — $D0..$DF.
        case 0xD0: suba_imm(rdmem(indexed2_ea())); break;
        case 0xD1: cmpa_imm(rdmem(indexed2_ea())); break;
        case 0xD2: sbca_imm(rdmem(indexed2_ea())); break;
        case 0xD3: cpx_imm (rdmem(indexed2_ea())); break;
        case 0xD4: anda_imm(rdmem(indexed2_ea())); break;
        case 0xD5: bita_imm(rdmem(indexed2_ea())); break;
        case 0xD6: lda_imm (rdmem(indexed2_ea())); break;
        case 0xD7: { uint16_t ea = indexed2_ea(); clr_nz(); set_nz8(reg.A); wrmem(ea, reg.A); break; }
        case 0xD8: eora_imm(rdmem(indexed2_ea())); break;
        case 0xD9: adca_imm(rdmem(indexed2_ea())); break;
        case 0xDA: ora_imm (rdmem(indexed2_ea())); break;
        case 0xDB: adda_imm(rdmem(indexed2_ea())); break;
        case 0xDC: reg.PC = indexed2_ea() & kAddrMask; break;
        case 0xDD: { uint16_t ea = indexed2_ea(); pushword(reg.PC); reg.PC = ea & kAddrMask; break; }
        case 0xDE: ldx_imm(rdmem(indexed2_ea())); break;
        case 0xDF: { uint16_t ea = indexed2_ea(); clr_nz(); set_nz8(reg.X); wrmem(ea, reg.X); break; }

        // Indexed,1 byte offset — $E0..$EF.
        case 0xE0: suba_imm(rdmem(indexed1_ea())); break;
        case 0xE1: cmpa_imm(rdmem(indexed1_ea())); break;
        case 0xE2: sbca_imm(rdmem(indexed1_ea())); break;
        case 0xE3: cpx_imm (rdmem(indexed1_ea())); break;
        case 0xE4: anda_imm(rdmem(indexed1_ea())); break;
        case 0xE5: bita_imm(rdmem(indexed1_ea())); break;
        case 0xE6: lda_imm (rdmem(indexed1_ea())); break;
        case 0xE7: { uint16_t ea = indexed1_ea(); clr_nz(); set_nz8(reg.A); wrmem(ea, reg.A); break; }
        case 0xE8: eora_imm(rdmem(indexed1_ea())); break;
        case 0xE9: adca_imm(rdmem(indexed1_ea())); break;
        case 0xEA: ora_imm (rdmem(indexed1_ea())); break;
        case 0xEB: adda_imm(rdmem(indexed1_ea())); break;
        case 0xEC: reg.PC = indexed1_ea() & kAddrMask; break;
        case 0xED: { uint16_t ea = indexed1_ea(); pushword(reg.PC); reg.PC = ea & kAddrMask; break; }
        case 0xEE: ldx_imm(rdmem(indexed1_ea())); break;
        case 0xEF: { uint16_t ea = indexed1_ea(); clr_nz(); set_nz8(reg.X); wrmem(ea, reg.X); break; }

        // Indexed (no offset) — $F0..$FF.
        case 0xF0: suba_imm(rdmem(indexed_ea())); break;
        case 0xF1: cmpa_imm(rdmem(indexed_ea())); break;
        case 0xF2: sbca_imm(rdmem(indexed_ea())); break;
        case 0xF3: cpx_imm (rdmem(indexed_ea())); break;
        case 0xF4: anda_imm(rdmem(indexed_ea())); break;
        case 0xF5: bita_imm(rdmem(indexed_ea())); break;
        case 0xF6: lda_imm (rdmem(indexed_ea())); break;
        case 0xF7: { uint16_t ea = indexed_ea(); clr_nz(); set_nz8(reg.A); wrmem(ea, reg.A); break; }
        case 0xF8: eora_imm(rdmem(indexed_ea())); break;
        case 0xF9: adca_imm(rdmem(indexed_ea())); break;
        case 0xFA: ora_imm (rdmem(indexed_ea())); break;
        case 0xFB: adda_imm(rdmem(indexed_ea())); break;
        case 0xFC: reg.PC = indexed_ea() & kAddrMask; break;
        case 0xFD: { uint16_t ea = indexed_ea(); pushword(reg.PC); reg.PC = ea & kAddrMask; break; }
        case 0xFE: ldx_imm(rdmem(indexed_ea())); break;
        case 0xFF: { uint16_t ea = indexed_ea(); clr_nz(); set_nz8(reg.X); wrmem(ea, reg.X); break; }

        default:
            // Illegal opcode — every illegal slot is one of the entries
            // marked XX in the cycle table above and behaves as a NOP at
            // run-time. MAME logs and continues; we follow suit.
            break;
    }

    const int cycles = kHmosCycles[op];
    icount -= cycles;
    timerUpdate(cycles);
    return cycles;
}

int M68705P3::run(int cycles)
{
    icount = cycles;
    int total = 0;
    while (icount > 0) {
        const int n = step();
        total += n;
        if (n == 0) break;     // safety
    }
    return total;
}

// ── Snapshot ──────────────────────────────────────────────────────────────
// Everything the running firmware can observe: registers, the 112-byte
// RAM, the port latch/DDR/input triples, the timer and the interrupt
// latches. The 2 KB EPROM is ROM (reloaded at construction) and the
// callbacks are wiring, so neither travels.

void M68705P3::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back(static_cast<uint8_t>(reg.PC));
    out.push_back(static_cast<uint8_t>(reg.PC >> 8));
    out.push_back(reg.S);
    out.push_back(reg.A);
    out.push_back(reg.X);
    out.push_back(reg.CC);
    out.insert(out.end(), ram.begin(), ram.end());
    for (const Port& pt : ports) {
        out.push_back(pt.latch);
        out.push_back(pt.ddr);
        out.push_back(pt.input);
    }
    out.push_back(timer.tdr);
    out.push_back(timer.tcr);
    out.push_back(timer.prescale);
    out.push_back(static_cast<uint8_t>(timer.divisor));
    out.push_back(static_cast<uint8_t>(pending_interrupts));
    out.push_back(static_cast<uint8_t>(pending_interrupts >> 8));
    out.push_back(irq_line_state ? 1 : 0);
    const uint32_t ic = static_cast<uint32_t>(icount);
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(ic >> (8 * i)));
}

size_t M68705P3::loadSnapshotState(const uint8_t* data, size_t len)
{
    if (data == nullptr || len < kSnapshotBytes) return 0;
    size_t p = 0;
    reg.PC = static_cast<uint16_t>(data[p] | (data[p + 1] << 8)); p += 2;
    reg.PC &= kAddrMask;
    // The 68705's stack is a 32-byte window that WRAPS between kSpFloor and
    // kSpMask; push/pull compare S against those two ends by equality, so a
    // restored value outside [kSpFloor, kSpMask] never hits either and walks
    // S straight out of the window, writing over RAM (or reading it) one
    // push at a time. RESET's own value is kSpMask, so that is the fallback.
    reg.S  = data[p++];
    if (reg.S < kSpFloor || reg.S > kSpMask) reg.S = kSpMask;
    reg.A  = data[p++];
    reg.X  = data[p++];
    reg.CC = data[p++];
    std::memcpy(ram.data(), data + p, ram.size()); p += ram.size();
    for (Port& pt : ports) {
        pt.latch = data[p++];
        pt.ddr   = data[p++];
        pt.input = data[p++];
    }
    timer.tdr      = data[p++];
    timer.tcr      = data[p++];
    timer.prescale = data[p++];
    timer.divisor  = data[p++];
    if (timer.divisor == 0) timer.divisor = 1;   // untrusted: no div-by-zero
    pending_interrupts =
        static_cast<uint16_t>(data[p] | (data[p + 1] << 8)); p += 2;
    irq_line_state = data[p++] != 0;
    uint32_t ic = 0;
    for (int i = 0; i < 4; ++i) ic |= static_cast<uint32_t>(data[p++]) << (8 * i);
    icount = static_cast<int>(ic);
    return p;
}
