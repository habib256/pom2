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

// M68705P3 — Motorola MC68705P3 microcontroller. Verbatim port of MAME's
// `src/devices/cpu/m6805/m6805.cpp` + `m68705.cpp` HMOS variant, stripped
// of the MAME machine-config plumbing and condensed into a single self-
// contained class for use by the Apple II Mouse Card (which embeds a
// real 68705P3 + 6821 PIA per Apple's original schematic, mirrored by
// MAME `bus/a2bus/mouse.cpp`).
//
// Memory map (11-bit address space, $0000-$07FF):
//
//   $0000  Port A data (R: pin levels masked by DDR; W: data latch)
//   $0001  Port B data
//   $0002  Port C data (4 bits — bits 7..4 are not present, forced to 0)
//   $0003  unused on P3
//   $0004  Port A DDR (write-only; reads back as $FF)
//   $0005  Port B DDR
//   $0006  Port C DDR
//   $0007  unused
//   $0008  TDR — Timer Data Register
//   $0009  TCR — Timer Control Register
//   $000A  unused (Misc register only when port D is present)
//   $000B  PCR — Programming Control Register (EPROM control; not used at
//          run-time once the firmware is loaded — only relevant during
//          OTP programming, which we don't model)
//   $000C-$000F  unused
//   $0010-$007F  112 bytes of internal RAM (stack lives in $0060-$007F)
//   $0080-$07F7  User EPROM (firmware) — for the 68705P3 specifically
//                $0080-$0784 is the user EPROM proper and $0785-$07F7 is
//                the bootstrap ROM. We treat the whole window as one
//                read-only image loaded from a 2 KB file.
//   $07F6/$07F7  Bootstrap vector (unused at run-time)
//   $07F8/$07F9  Timer interrupt vector (big-endian)
//   $07FA/$07FB  External /INT vector
//   $07FC/$07FD  SWI vector
//   $07FE/$07FF  Reset vector
//
// Stack: 8-bit SP register, but only bits 0-6 are effective; SP wraps in
// [$60, $7F] (matches MAME's `m_sp_floor=0x60, m_sp_mask=0x7F`).
//
// CPU clock: the 68705P3 runs internally at the crystal frequency divided
// by 4 (every machine cycle = 4 oscillator cycles). The MAME tables count
// machine cycles per instruction, NOT clock ticks. Higher-level code is
// responsible for calling `run(cycles)` with machine-cycle budgets.
//
// Source-of-truth invariants pinned by tests/m68705_decode_smoke_test.cpp:
//
//   * Reset vector fetched big-endian from $07FE/$07FF and SP set to $7F.
//   * SEI on reset (IRQ disabled until firmware clears I).
//   * Push order on IRQ: PCH, PCL, X, A, CC (low to high addresses).
//   * Pull order on RTI: CC, A, X, PCH, PCL.
//   * IRQ vector at $07FA/$07FB; timer at $07F8/$07F9; SWI at $07FC/$07FD.
//   * SP wraps strictly inside $60-$7F (push past $60 wraps to $7F; pull
//     past $7F wraps to $60).
//   * Cycle counts match MAME `s_hmos_cycles[]` exactly (e.g. RTI=9,
//     RTS=6, BSR=8, BRSET=10, JSR-EX=8).

#ifndef POM2_M68705P3_H
#define POM2_M68705P3_H

#include <array>
#include <vector>
#include <cstdint>
#include <functional>
#include <string>

class M68705P3
{
public:

    /// Snapshot surface (raw, no magic — the owning card wraps it).
    /// Registers + 112 B RAM + port latches/DDRs + timer + interrupt
    /// latches. The EPROM is ROM, reloaded at construction, so it is
    /// deliberately NOT serialized.
    /// PC(2) + S/A/X/CC(4) + RAM(112) + 3 ports × (latch/ddr/input)(9) +
    /// timer(4) + pending_interrupts(2) + irq_line(1) + icount(4) = 138.
    /// The register group is FOUR bytes, not three — the old 137 let a
    /// short blob past the loader's length guard, which then read one
    /// byte past the caller's buffer.
    static constexpr size_t kSnapshotBytes =
        2 + 4 + 112 + 3 * 3 + 4 + 2 + 1 + 4;
    void   appendSnapshotState(std::vector<uint8_t>& out) const;
    size_t loadSnapshotState(const uint8_t* data, size_t len);
    M68705P3();

    // ─── ROM loading ───────────────────────────────────────────────────
    /// Load the firmware EPROM image. The file must be exactly 2048
    /// bytes — file offset 0 maps to chip address $0080. (For the Mouse
    /// Card this is `roms/mouse_341-0269.bin`.)
    bool loadRomFile(const std::string& path);
    /// Load from an in-memory image. `len` must equal 2048.
    bool loadRomBytes(const uint8_t* data, std::size_t len);
    bool isRomLoaded() const { return romLoaded; }

    // ─── Reset ─────────────────────────────────────────────────────────
    /// Pull /RESET low and back high again. Re-fetches the reset vector
    /// at $07FE/$07FF, clears all registers, sets I, and parks the chip
    /// at the firmware's reset entry point.
    void reset();

    // ─── Execution ─────────────────────────────────────────────────────
    /// Step one instruction. Returns cycles consumed.
    int step();
    /// Run until at least `cycles` machine cycles have elapsed since the
    /// last `run()` call. Returns the actual count (may overshoot by up
    /// to one instruction's worth, like every other M6502/6805 driver).
    int run(int cycles);

    // ─── External-pin interface ────────────────────────────────────────
    /// Drive the /INT pin. true = asserted (low on the wire). The 68705
    /// latches the request internally — once asserted, the IRQ stays
    /// pending until serviced even if the line is later released.
    void setIrq(bool asserted);

    /// Drive the input level on a port pin. The chip composes the
    /// effective read value as `(latch & DDR) | (input & ~DDR)` plus the
    /// per-port mask of "not present" bits (which always read 0). Only
    /// bits where DDR=0 are visible — bits where DDR=1 read back the
    /// latch (the chip is driving them).
    void setPortInput(int port, uint8_t value);

    /// Read the chip's current output on a port — i.e. what an external
    /// observer would see on the pins. Bits with DDR=0 are open
    /// (returned as 1 here, modelling the internal pull-up on Port A);
    /// bits with DDR=1 surface the latch.
    uint8_t getPortPins(int port) const;

    /// Latch register for `port`. Useful for tests / diagnostics.
    uint8_t getPortLatch(int port) const { return ports[port].latch; }
    uint8_t getPortDdr(int port)   const { return ports[port].ddr; }

    /// Fired every time a port latch or DDR is written by the firmware.
    /// Lets the host (the Mouse Card's PIA bridge) sample the new pin
    /// state synchronously.
    using PortWriteFn = std::function<void(int port)>;
    void setPortWriteCallback(PortWriteFn fn) { portWriteCb = std::move(fn); }

    /// Fired just BEFORE the firmware reads a port — lets the host
    /// drive a fresh input value into the chip before the read composes
    /// its result. The Apple II Mouse Card uses this for Port B, where
    /// each read of PB by the MCU's firmware triggers a quadrature-edge
    /// computation in `mcu_port_b_r()` (mirroring MAME `mouse.cpp`'s
    /// per-call edge synthesis). Returns the byte to use as the input
    /// pin level for that port.
    using PortReadFn = std::function<uint8_t(int port)>;
    void setPortReadCallback(PortReadFn fn) { portReadCb = std::move(fn); }

    // ─── Test / diagnostic accessors ───────────────────────────────────
    uint8_t  getA()  const { return reg.A;  }
    uint8_t  getX()  const { return reg.X;  }
    uint8_t  getS()  const { return reg.S;  }
    uint16_t getPC() const { return reg.PC; }
    uint8_t  getCC() const { return reg.CC; }
    /// Direct RAM peek (no bus side effects). $10..$7F valid.
    uint8_t  peekRam(uint8_t addr) const {
        return (addr >= 0x10 && addr <= 0x7F) ? ram[addr - 0x10] : 0xFF;
    }
    /// Direct RAM poke for tests.
    void pokeRam(uint8_t addr, uint8_t v) {
        if (addr >= 0x10 && addr <= 0x7F) ram[addr - 0x10] = v;
    }
    /// The Mask Option Register byte the loaded EPROM carries at $0784,
    /// already masked the way MAME's `m68705p3_device::get_mask_options()`
    /// masks it (`& 0xf7`, "no SNM bit"). $FF when no ROM is loaded.
    uint8_t  getMaskOptions() const {
        return romLoaded ? static_cast<uint8_t>(eprom[kMorAddr - 0x80] & 0xF7)
                         : uint8_t{0xFF};
    }
    /// True when the MOR selected TIMER_MOR (MOR_TOPT set) — the timer
    /// divisor/source are then fixed by the mask option and TCR writes
    /// cannot change them.
    bool     timerIsMorConfigured() const { return timerMor_; }
    /// Current prescaler divisor exponent (0..7 → ÷1..÷128).
    unsigned getTimerDivisor() const { return timer.divisor; }
    uint8_t  getTimerTdr() const { return timer.tdr; }

private:
    static constexpr uint8_t  kSpMask     = 0x7F;
    static constexpr uint8_t  kSpFloor    = 0x60;
    static constexpr uint16_t kAddrMask   = 0x07FF;
    static constexpr uint16_t kRstVector  = 0x07FE;
    static constexpr uint16_t kSwiVector  = 0x07FC;
    static constexpr uint16_t kIntVector  = 0x07FA;
    static constexpr uint16_t kTmrVector  = 0x07F8;
    /// Mask Option Register — MAME `m68705.cpp:54` (`{ 0x0784, "MOR" }`).
    static constexpr uint16_t kMorAddr    = 0x0784;

    // MOR bit assignments — MAME `m68705.h:249-253`.
    static constexpr uint8_t MOR_PS   = 0x07;   // prescaler divisor
    static constexpr uint8_t MOR_TIE  = 0x10;   // timer external enable
    static constexpr uint8_t MOR_CLS  = 0x20;   // clock source
    static constexpr uint8_t MOR_TOPT = 0x40;   // timer option
    // TCR bit assignments — MAME `m68705.h:93-101`.
    static constexpr uint8_t TCR_PS   = 0x07;
    static constexpr uint8_t TCR_PSC  = 0x08;
    static constexpr uint8_t TCR_TIE  = 0x10;
    static constexpr uint8_t TCR_TIN  = 0x20;
    static constexpr uint8_t TCR_TIM  = 0x40;
    static constexpr uint8_t TCR_TIR  = 0x80;
    // m6805_timer::timer_source — MAME `m68705.h:40-45`.
    static constexpr uint8_t kSrcClock      = 0;   // internal clock
    static constexpr uint8_t kSrcClockTimer = 1;   // internal AND external
    static constexpr uint8_t kSrcDisabled   = 2;
    static constexpr uint8_t kSrcExternal   = 3;   // external input only

    // Condition-code flags (bits 0..4 of CC).
    static constexpr uint8_t CFLAG = 0x01;
    static constexpr uint8_t ZFLAG = 0x02;
    static constexpr uint8_t NFLAG = 0x04;
    static constexpr uint8_t IFLAG = 0x08;
    static constexpr uint8_t HFLAG = 0x10;

    // Registers.
    struct {
        uint16_t PC = 0;
        uint8_t  S  = kSpMask;
        uint8_t  A  = 0;
        uint8_t  X  = 0;
        uint8_t  CC = IFLAG;     // I set on reset
    } reg;

    // 112 bytes RAM at $0010-$007F (offset 0 = $10).
    std::array<uint8_t, 112> ram{};
    // 2 KB EPROM at $0080-$07FF (offset 0 = $80).
    std::array<uint8_t, 0x780> eprom{};
    bool romLoaded = false;

    // Per-port state. 3 ports A/B/C. Port C has only 4 bits (mask =
    // 0xF0 = bits 7..4 not present); ports A and B are full 8-bit.
    struct Port {
        uint8_t latch = 0xFF;     // data latch (what the firmware wrote)
        uint8_t ddr   = 0x00;     // 1 = output, 0 = input
        uint8_t input = 0xFF;     // pin level driven from outside
        uint8_t mask  = 0x00;     // bits NOT present (force 0 on read)
    };
    std::array<Port, 3> ports;

    // Timer state. Mirrors MAME's `m6805_timer` struct.
    struct {
        uint8_t tdr      = 0xFF;     // Timer Data Register
        uint8_t tcr      = 0x7F;     // Timer Control Register
        uint8_t prescale = 0x7F;
        unsigned divisor = 7;        // prescaler exponent (÷1 … ÷128)
    } timer;
    /// Timer configuration taken from the MOR at EPROM-load time (MAME
    /// does it in `device_start`, so `reset()` must not disturb it).
    bool    timerMor_    = false;         // TIMER_MOR vs TIMER_PGM
    uint8_t timerSource_ = kSrcClock;     // m6805_timer::timer_source

    // Interrupts. The 68705 latches IRQ requests internally — once
    // asserted, the request stays pending until the CPU services it,
    // even if the external line is released.
    uint16_t pending_interrupts = 0;     // bitfield: bit 0 = /INT, bit 1 = TIMER
    bool     irq_line_state     = false; // current external level (true = asserted)

    // Cycle counter. Decremented by every executed opcode.
    int icount = 0;

    PortWriteFn portWriteCb;
    PortReadFn  portReadCb;

    // ─── Memory access ─────────────────────────────────────────────────
    uint8_t rdmem(uint16_t addr);
    void    wrmem(uint16_t addr, uint8_t v);
    uint8_t rdop_arg(uint16_t addr) { return rdmem(addr); }

    // ─── Stack ────────────────────────────────────────────────────────
    void pushbyte(uint8_t v);
    void pushword(uint16_t w);
    uint8_t pullbyte();
    uint16_t pullword();

    // ─── Reads / addressing modes (return effective address) ──────────
    uint16_t direct_ea();
    uint16_t extended_ea();
    uint16_t indexed_ea()  { return reg.X; }
    uint16_t indexed1_ea();
    uint16_t indexed2_ea();
    uint16_t imm8_ea() { uint16_t a = reg.PC; reg.PC = (reg.PC + 1) & kAddrMask; return a; }

    // ─── Flag helpers ─────────────────────────────────────────────────
    void clr_nz()   { reg.CC &= ~(NFLAG | ZFLAG); }
    void clr_nzc()  { reg.CC &= ~(NFLAG | ZFLAG | CFLAG); }
    void clr_hc()   { reg.CC &= ~(HFLAG | CFLAG); }
    void clr_hnzc() { reg.CC &= ~(HFLAG | NFLAG | ZFLAG | CFLAG); }
    void set_z8(uint8_t a)            { if (!a) reg.CC |= ZFLAG; }
    void set_n8(uint8_t a)            { reg.CC |= (a & 0x80) >> 5; }
    void set_h(uint8_t a, uint8_t b, uint8_t r) { reg.CC |= (a ^ b ^ r) & HFLAG; }
    void set_c8(uint16_t a)           { if (a & 0x100) reg.CC |= CFLAG; }
    void set_nz8(uint8_t a)           { set_n8(a); set_z8(a); }
    void set_nzc8(uint16_t r)         { set_nz8(uint8_t(r)); set_c8(r); }
    void set_hnzc8(uint8_t a, uint8_t b, uint16_t r) { set_h(a, b, uint8_t(r)); set_nzc8(r); }

    // ─── Per-port memory-mapped helpers ───────────────────────────────
    void writePortLatch(int n, uint8_t v);
    void writePortDdr(int n, uint8_t v);
    uint8_t readPortPin(int n);

    // ─── Timer ────────────────────────────────────────────────────────
    /// Apply the loaded EPROM's MOR to the timer (MAME `device_start`).
    void configureTimerFromMor();
    void timerWriteTcr(uint8_t v);
    void timerUpdate(unsigned count);

    // ─── Interrupt service ────────────────────────────────────────────
    void serviceInterrupt();

    // ─── Static cycle-count table (MAME s_hmos_cycles) ────────────────
    static const uint8_t kHmosCycles[256];
};

#endif // POM2_M68705P3_H
