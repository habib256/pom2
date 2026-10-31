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

// Z80 — standalone Zilog Z80 CPU core (Phase 1 of the Microsoft SoftCard /
// CP/M plan). Deliberately independent from the Apple II side: all bus
// traffic goes through the abstract Z80Bus interface, so the core is unit-
// testable on a flat 64 KB RAM (zexdoc/zexall harness) before it ever
// touches Memory. SoftCardZ80 (Phase 2, shipped) implements Z80Bus with
// the SoftCard's six-window address translation (MAME a2softcard.cpp
// dma_r/dma_w — NOT a plain +$1000 wrap; see SoftCardZ80.h) and routes
// everything through Memory::memRead/memWrite.
//
// Reference: MAME src/devices/cpu/z80/z80.cpp (flag semantics, T-states)
// and Sean Young's "The Undocumented Z80 Documented" (X/Y flags, MEMPTR,
// IXH/IXL, DD CB result write-back). Validated against Frank Cringle's
// zexdoc + zexall exercisers — zexall requires the undocumented X/Y flag
// behaviour *and* MEMPTR (BIT n,(HL) leaks WZ's high byte into X/Y), so
// both are modelled, not approximated.
//
// Scope notes:
//  - Documented T-state totals per instruction (returned by step()/run()),
//    and `busT()`: the T-state at which the memory access in progress
//    starts, from the Zilog M-cycle shapes (M1 4 T, read/write 3 T, the 5
//    internal T of (IX+d), the 2 of a DDCB/FDCB op, the 1 before a stack
//    push). The SoftCard stamps its Apple bus accesses with it. Other
//    internal T-states come after the last access and move no stamp.
//  - IM0 assumes the interrupting device places an RST opcode on the bus
//    (the only IM0 use ever seen on Apple II SoftCard software: none —
//    CP/M runs with interrupts disabled).
//  - The SCF/CCF X/Y flags use the "copy from A" NMOS rule (no Q-register
//    model); this is the behaviour zexall's CRCs were computed against.

#ifndef POM2_Z80_H
#define POM2_Z80_H

#include "CardTraceSink.h"

#include <cstdint>

namespace pom2 {

/// Abstract bus the Z80 core drives. Implementations: flat 64 KB RAM in
/// the test harnesses; the SoftCard address-translation shim in Phase 2.
class Z80Bus
{
public:
    virtual ~Z80Bus() = default;
    virtual uint8_t z80MemRead (uint16_t addr) = 0;
    virtual void    z80MemWrite(uint16_t addr, uint8_t v) = 0;
    /// 16-bit port address on the bus (B in the high byte for the
    /// register forms). The SoftCard decodes no I/O — default open bus.
    virtual uint8_t z80IoRead (uint16_t /*port*/) { return 0xFF; }
    virtual void    z80IoWrite(uint16_t /*port*/, uint8_t /*v*/) {}
    /// Side-effect-free read for an observer (the OS-call trace). A bus whose
    /// reads touch a latch or the 6502's I/O must override it.
    virtual uint8_t z80Peek(uint16_t addr) { return z80MemRead(addr); }
};

class Z80;

/// Told of every instruction before it runs — not of an interrupt entry, a
/// HALT idle cycle or the opcode behind a DD/FD prefix. Unset, it costs one
/// pointer test per step().
/// Its `onCardLine` (CardTraceSink.h) takes the card's own lines — the
/// SoftCard II's 6502 BIOS calls (SoftCard2Protocol.h) — while it is hung.
class Z80Observer : public CardTraceSink
{
public:
    virtual void onZ80Instruction(const Z80& z80) = 0;
};

class Z80
{
public:
    /// F register bits.
    struct Flag {
        static constexpr uint8_t S  = 0x80;  // sign
        static constexpr uint8_t Z  = 0x40;  // zero
        static constexpr uint8_t Y  = 0x20;  // undocumented (bit 5 copy)
        static constexpr uint8_t H  = 0x10;  // half carry
        static constexpr uint8_t X  = 0x08;  // undocumented (bit 3 copy)
        static constexpr uint8_t PV = 0x04;  // parity / overflow
        static constexpr uint8_t N  = 0x02;  // add/subtract
        static constexpr uint8_t C  = 0x01;  // carry
    };

    /// Full architectural state — snapshot/rewind payload and test hook.
    struct State {
        uint8_t  a = 0xFF, f = 0xFF, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
        uint8_t  a2 = 0, f2 = 0, b2 = 0, c2 = 0, d2 = 0, e2 = 0, h2 = 0, l2 = 0;
        uint16_t ix = 0xFFFF, iy = 0xFFFF, sp = 0xFFFF, pc = 0, wz = 0;
        uint8_t  i = 0, r = 0, im = 0;
        bool     iff1 = false, iff2 = false, halted = false;
        bool     irqLine = false, nmiPending = false, afterEi = false;
        uint8_t  irqData = 0xFF;
        /// A DD/FD prefix retired by the previous step() whose opcode
        /// hasn't been fetched yet (0 = none, 1 = IX, 2 = IY). Each
        /// prefix is its own interruptible-boundary 4-T M1 cycle — but
        /// INT/NMI are NOT accepted between a prefix and its opcode
        /// (hardware defers them), so step() skips the interrupt check
        /// while this is set. Folding a whole DD/FD run into one step()
        /// let a prefix sea consume unbounded T-states in one call.
        uint8_t  pendingPrefix = 0;
    };

    explicit Z80(Z80Bus& bus);

    /// Power-on / RESET pin: PC=0, I=R=0, IFF=0, IM0, SP=AF=$FFFF.
    void reset();

    /// Execute one instruction (or service a pending interrupt, burn a
    /// HALT idle M-cycle, or retire one DD/FD prefix M1 — see
    /// State::pendingPrefix). Returns the T-states consumed. This is the
    /// only execution entry point: budget pacing lives in the caller
    /// (SoftCardZ80::dmaRun, test harnesses) because each has its own
    /// abort condition a generic run() can't know about.
    int step();

    /// Level-triggered maskable INT line. `dataBusValue` is what the
    /// interrupting device drives during the acknowledge cycle (IM2
    /// vector low byte / IM0 opcode).
    void setIrqLine(bool asserted, uint8_t dataBusValue = 0xFF);
    /// Edge-triggered NMI — latched, serviced before the next instruction.
    void triggerNmi();

    bool isHalted() const { return st.halted; }
    /// T-states from the start of the current step() to the access in
    /// progress — valid inside a Z80Bus callback (see the header note).
    int  busT() const { return busT_; }

    void setObserver(Z80Observer* o) { observer_ = o; }
    Z80Observer* observer() const { return observer_; }
    /// Through Z80Bus::z80Peek: no side effect on the card.
    uint8_t peek(uint16_t addr) const { return bus.z80Peek(addr); }

    const State& getState() const { return st; }
    void         setState(const State& s) { st = s; }

    // Register accessors used by tests and (later) debug UI.
    uint16_t getPC() const { return st.pc; }
    void     setPC(uint16_t v) { st.pc = v; }
    uint16_t getSP() const { return st.sp; }
    void     setSP(uint16_t v) { st.sp = v; }
    uint8_t  getA() const { return st.a; }
    uint8_t  getF() const { return st.f; }
    uint16_t getBC() const { return pair(st.b, st.c); }
    uint16_t getDE() const { return pair(st.d, st.e); }
    uint16_t getHL() const { return pair(st.h, st.l); }
    uint16_t getIX() const { return st.ix; }
    uint16_t getIY() const { return st.iy; }

private:
    // Index-register substitution mode for the current instruction:
    // 0 = HL, 1 = IX, 2 = IY (set by a DD/FD prefix, cleared after).
    enum : int { MODE_HL = 0, MODE_IX = 1, MODE_IY = 2 };

    Z80Bus& bus;
    Z80Observer* observer_ = nullptr;
    State   st;
    int     cyc = 0;   // T-states consumed by the in-flight step()
    int     busT_ = 0; // T-state of the next bus access in this step()

    static uint8_t szpTable[256];   // S,Z,X,Y,parity lookup
    static bool    tablesReady;
    static void    initTables();

    /// Re-derive X/Y/H/PV on the REPEAT branch of INIR/OTIR/INDR/OTDR.
    /// MAME `z80.cpp:580-604` block_io_interrupted_flags(); `ioByte` is the
    /// byte just transferred. Must be called AFTER `pc -= 2`.
    void blockIoInterruptedFlags(uint8_t ioByte);

    // ── bus helpers ──────────────────────────────────────────────────
    uint8_t  rd (uint16_t a) { const uint8_t v = bus.z80MemRead(a); busT_ += 3; return v; }
    void     wr (uint16_t a, uint8_t v) { bus.z80MemWrite(a, v); busT_ += 3; }
    uint8_t  fetchOp() { const uint8_t v = rd(st.pc++); ++busT_; return v; }   // M1: 4 T
    uint16_t rd16(uint16_t a) { return rd(a) | (rd(uint16_t(a + 1)) << 8); }
    void     wr16(uint16_t a, uint16_t v) { wr(a, uint8_t(v)); wr(uint16_t(a + 1), uint8_t(v >> 8)); }
    uint8_t  fetch8()  { return rd(st.pc++); }
    uint16_t fetch16() { uint16_t v = rd16(st.pc); st.pc += 2; return v; }
    void     push16(uint16_t v) { ++busT_; st.sp -= 2; wr16(st.sp, v); }   // 5-T M1/internal first
    uint16_t pop16() { uint16_t v = rd16(st.sp); st.sp += 2; return v; }
    void     bumpR() { st.r = uint8_t((st.r & 0x80) | ((st.r + 1) & 0x7F)); }

    static uint16_t pair(uint8_t hi, uint8_t lo) { return uint16_t((hi << 8) | lo); }

    // ── register-pair accessors ──────────────────────────────────────
    uint16_t bc() const { return pair(st.b, st.c); }
    uint16_t de() const { return pair(st.d, st.e); }
    uint16_t hl() const { return pair(st.h, st.l); }
    void setBC(uint16_t v) { st.b = uint8_t(v >> 8); st.c = uint8_t(v); }
    void setDE(uint16_t v) { st.d = uint8_t(v >> 8); st.e = uint8_t(v); }
    void setHL(uint16_t v) { st.h = uint8_t(v >> 8); st.l = uint8_t(v); }

    /// HL / IX / IY depending on the active prefix mode.
    uint16_t idxPair(int mode) const;
    void     setIdxPair(int mode, uint16_t v);

    /// 8-bit register file view, r-field encoding 0..7 = B C D E H L (HL) A.
    /// In IX/IY mode, H/L map to IXH/IXL *unless* the instruction also
    /// touches (IX+d) — callers pass mode=MODE_HL in that case (the real
    /// hardware rule: a memory-form instruction uses the true H/L).
    uint8_t readReg (int r, int mode) const;
    void    writeReg(int r, int mode, uint8_t v);

    // ── ALU / flag helpers (operate on st.a / st.f) ──────────────────
    void add8(uint8_t v, int carry);
    void sub8(uint8_t v, int carry);
    void and8(uint8_t v);
    void xor8(uint8_t v);
    void or8 (uint8_t v);
    void cp8 (uint8_t v);
    void aluDispatch(int op, uint8_t v);   // op 0..7 = ADD ADC SUB SBC AND XOR OR CP
    uint8_t inc8(uint8_t v);
    uint8_t dec8(uint8_t v);
    uint16_t add16(uint16_t dst, uint16_t src);          // ADD HL/IX/IY,rr
    void     adc16(uint16_t src);
    void     sbc16(uint16_t src);
    void daa();

    uint8_t rotShift(int op, uint8_t v);   // CB op 0..7 = RLC RRC RL RR SLA SRA SLL SRL
    void bitMem(int n, uint8_t v, uint8_t xyFrom);   // BIT n,(HL)/(IX+d)
    void bitReg(int n, uint8_t v);                   // BIT n,r

    // ── execution ────────────────────────────────────────────────────
    void execMain(uint8_t op, int mode);
    void execCB();                 // unprefixed CB page
    void execIndexedCB(int mode);  // DD CB d op / FD CB d op
    void execED();
    /// Fetch the displacement byte and form the (IX+d)/(IY+d) effective
    /// address (also latches it into WZ, as hardware does). MODE_HL → HL.
    uint16_t memEA(int mode);

    void serviceNmi();
    void serviceIrq();
};

} // namespace pom2

#endif // POM2_Z80_H
