// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2012 John D. Corrado
// Copyright (C) 2000-2026 Verhille Arnaud
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

#ifndef M6502_H
#define M6502_H

#include "Memory.h"

#include <atomic>

/// Per-instruction debug hook. Deliberately an interface declared HERE rather
/// than an include of Debugger.h: the CPU must not depend on the debugger, and
/// this way the dependency runs one way only (Debugger implements this).
///
/// Costs nothing when unset — `M6502::run` picks between two loops once per
/// call, so an un-armed hook is a branch per 4096-cycle chunk, not per
/// instruction. See Debugger.h.
class M6502DebugHook
{
public:
    virtual ~M6502DebugHook() = default;
    /// Called BEFORE the instruction at `pc` executes. Returning true halts
    /// the CPU with `pc` unchanged, so the instruction has not run and a
    /// later resume re-tries it.
    virtual bool onInstruction(uint16_t pc) = 0;
};

class M6502
{
public:
    /// Bits du registre P (éviter #define N,C,I,… qui cassent les en-têtes Windows).
    struct Status {
        static constexpr uint8_t N = 0x80;
        static constexpr uint8_t V = 0x40;
        static constexpr uint8_t B = 0x10;
        static constexpr uint8_t D = 0x08;
        static constexpr uint8_t I = 0x04;
        static constexpr uint8_t Z = 0x02;
        static constexpr uint8_t C = 0x01;
    };

    enum class CpuMode { NMOS, CMOS };

    M6502();
    M6502(Memory* mem);

    void start(void);
    void stop(void);
    void softReset(void);
    void hardReset(void);

    /// IRQ source identifiers for `setIrqLine()`. The 6502 IRQ pin is
    /// active-low and **wire-OR**'d on real hardware — any device pulling
    /// it asserts IRQ, and the line only releases once *every* device
    /// stops pulling. `setIrqLine()` maintains a 32-bit OR'd source mask
    /// so multiple cards can assert independently without one card's
    /// deassertion clobbering another card's still-pending IRQ.
    ///
    /// Slot N (1..7) reserves bit N. Source-level IDs above 7 cover
    /// motherboard interrupts. Bit 31 is the back-compat slot used by
    /// the legacy `setIRQ(int)` entry point.
    enum IrqSource : int {
        IRQ_SRC_SLOT1   = 1,
        IRQ_SRC_SLOT2   = 2,
        IRQ_SRC_SLOT3   = 3,
        IRQ_SRC_SLOT4   = 4,
        IRQ_SRC_SLOT5   = 5,
        IRQ_SRC_SLOT6   = 6,
        IRQ_SRC_SLOT7   = 7,
        IRQ_SRC_VBL     = 8,
        IRQ_SRC_LEGACY  = 31,
    };
    /// Set or clear one source's contribution to the IRQ line.
    /// Cards assert with their slot number (`slot_`). Idempotent.
    void setIrqLine(int sourceId, bool asserted);
    /// Wire-OR mask of currently asserted sources. Debug / test hook.
    uint32_t getIrqSourceMask() const { return irqSourceMask.load(std::memory_order_relaxed); }

    /// Legacy entry — equivalent to `setIrqLine(IRQ_SRC_LEGACY, state!=0)`.
    /// Kept so existing callers (and the Klaus harness, snapshot tools)
    /// still compile. Prefer `setIrqLine` for new code so deassertions
    /// don't drop other sources' IRQs.
    void setIRQ(int state);
    void setNMI(void);
    void dumpPcTrace(const char* tag);

    /// Switch the dispatch table between NMOS 6502 and 65C02 (CMOS)
    /// behaviour at runtime. NMOS mode replaces every 65C02-only
    /// addition (STZ, BRA, INA/DEA, PHX/PHY/PLX/PLY, BIT #imm/zp,X/abs,X,
    /// TSB/TRB, JMP (abs,X), zp-indirect ORA/AND/EOR/ADC/STA/LDA/CMP/SBC,
    /// SMBn/RMBn/BBRn/BBSn, WAI/STP) with the matching NMOS 1/2/3-byte
    /// NOP placeholder so programs that don't use them keep running.
    /// `code 0xB2` and the other (zp)-mode opcodes that NMOS treats as
    /// `KIL` (halt) become Hang in NMOS mode — exactly what real silicon
    /// would do.
    void    setCpuMode(CpuMode mode);
    CpuMode getCpuMode() const { return cpuMode; }

    /// Debug: when true, BRK logs a full CPU+stack dump + recent control-flow
    /// trace + bus state on every execution. Off by default. The `dumpPcTrace`
    /// ring buffer is always live (cheap) and can be dumped on demand.
    void setDebugBrkTrace(bool enabled) { debugBrkTrace = enabled; }
    bool getDebugBrkTrace() const { return debugBrkTrace; }
    uint16_t memReadAbsolute(uint16_t adr);
    
    // Nouvelles méthodes pour l'exécution et l'affichage
    /// Attach (or detach, with nullptr) the per-instruction debug hook. The
    /// CPU keeps a bare pointer and never owns it; the hook must outlive the
    /// CPU or be detached first. Set and cleared under `stateMutex`, like
    /// every other piece of emulated state.
    void setDebugHook(M6502DebugHook* hook) { debugHook_ = hook; }
    M6502DebugHook* getDebugHook() const { return debugHook_; }

    void step(void);  // Exécuter une instruction
    /// Run until at least `maxCycles` 6502 cycles have elapsed (or `stop()`
    /// is called). Per-instruction granularity means we typically overshoot
    /// by 1-6 cycles; the actual cycle count is returned so callers pacing
    /// against a wallclock budget can deduct what was really consumed
    /// (otherwise the overshoot accumulates and the CPU runs faster than
    /// the nominal POM2_CPU_CLOCK_HZ rate).
    int run(int maxCycles);
    bool isRunning(void) const { return running; }
    
    // Accesseurs pour les registres (pour le débogueur)
    uint8_t getAccumulator(void) const { return accumulator; }
    uint8_t getXRegister(void) const { return xRegister; }
    uint8_t getYRegister(void) const { return yRegister; }
    uint8_t getStatusRegister(void) const { return statusRegister; }
    uint8_t getStackPointer(void) const { return stackPointer; }
    uint16_t getProgramCounter(void) const { return programCounter; }
    /// Jump the PC to an arbitrary address without going through RESET.
    /// Used by the Klaus Dormann functional test harness (the test binary
    /// sets its reset vector to an error trap and expects callers to jump
    /// directly into $0400).
    void setProgramCounter(uint16_t pc) { programCounter = pc; }

    /// Register setters — used by snapshot restore (and the debugger) to
    /// reconstruct the exact CPU state. Without these the snapshot CPU
    /// section could only restore PC, leaving A/X/Y/P/SP stale on load.
    void setAccumulator(uint8_t v)    { accumulator    = v; }
    void setXRegister(uint8_t v)      { xRegister      = v; }
    void setYRegister(uint8_t v)      { yRegister      = v; }
    void setStatusRegister(uint8_t v) { statusRegister = v; }
    void setStackPointer(uint8_t v)   { stackPointer   = v; }
    /// STP ($DB, WDC 65C02) halt latch — guest-visible state that only
    /// RESET clears, so snapshot/rewind must carry it: restoring a
    /// pre-halt frame used to leave the machine frozen (live `halted`
    /// kept), and restoring a halted snapshot woke STP without RESET.
    bool isHalted() const   { return halted; }
    void setHalted(bool v)  { halted = v; }

    /// Cycles accumulated inside the *current* opcode (reset to 1 at the
    /// fetch by executeOpcode and incremented as the instruction runs).
    /// Memory::cycleCounter is updated only at the end of each step(), so
    /// `cycleCounter + getCurrentInstructionCycles()` is the best
    /// sub-instruction approximation of "absolute CPU cycle right now" —
    /// used by the speaker to timestamp $C030 toggles. The error is
    /// bounded by a single opcode (≤ 7 cycles, ~7 µs), well under one
    /// audio sample at 44 kHz.
    int getCurrentInstructionCycles() const { return cycles; }

    /// Absolute CPU cycle count right now, including in-flight sub-
    /// instruction cycles. Equivalent to `memory->getCycleCounter() +
    /// getCurrentInstructionCycles()` — exposed here so slot peripherals
    /// can lazy-sync their timers on each MMIO access without taking a
    /// Memory back-pointer. Used by Mockingboard's 6522 VIA to advance
    /// T1/T2 up to "now" before a detection routine reads IFR. Implemented
    /// out-of-line in M6502.cpp because the inline body would need a full
    /// Memory definition (forward-declared here).
    uint64_t getCycleCountNow() const;

private:


private :

    Memory *memory;

    bool debugBrkTrace = false;   // see setDebugBrkTrace()
    uint8_t accumulator, xRegister, yRegister, statusRegister, stackPointer;
    // `IRQ` and `irqSourceMask` are atomic because off-CPU-thread cards (the
    // SSC TCP worker) call setIrqLine() concurrently with the CPU thread's
    // own setIrqLine() / per-step `IRQ` read. Relaxed atomics make the
    // read-modify-write race-free; on x86 the hot-path load is a plain mov.
    std::atomic<int> IRQ{0};
    int NMI = 0;
    /// OR'd contributions from every IRQ source registered via
    /// `setIrqLine()`. `IRQ` mirrors `(irqSourceMask != 0)` after every
    /// update so the dispatch loop stays a single-int test.
    std::atomic<uint32_t> irqSourceMask{0};
    uint16_t programCounter;
    uint16_t op;
    int tmp;
    int cycles;
    int running;


    void pushProgramCounter(void);
    void popProgramCounter(void);
    void handleIRQ(void);
    void handleNMI(void);
    void Imp(void);
    void Imm(void);
    void Zero(void);
    void ZeroX(void);
    void ZeroY(void);
    void Abs(void);
    void AbsX(void);
    void AbsY(void);
    void Ind(void);
    void IndZero(void);    // 65C02 (zp)
    void IndAbsX(void);    // 65C02 (abs,X) for JMP
    void IndZeroX(void);
    void IndZeroY(void);
    void Rel(void);
    void WAbsX(void);
    void RmwAbsX(void);   // 65C02 ASL/LSR/ROL/ROR abs,X (6c, +1 on page-cross)
    void WAbsY(void);
    void WIndZeroY(void);
    void setStatusRegisterNZ(unsigned char val);
    void LDA(void);
    void LDX(void);
    void LDY(void);
    void STA(void);
    void STX(void);
    void STY(void);
    void setFlagCarry(int val);
    void ADC(void);
    void setFlagBorrow(int val);
    void SBC(void);
    void CMP(void);
    void CPX(void);
    void CPY(void);
    void AND(void);
    void ORA(void);
    void EOR(void);
    void ASL(void);
    void ASL_A(void);
    void LSR(void);
    void LSR_A(void);
    void ROL(void);
    void ROL_A(void);
    void ROR(void);
    void ROR_A(void);
    void INC(void);
    void DEC(void);
    void INX(void);
    void INY(void);
    void DEX(void);
    void DEY(void);
    void BIT(void);
    void PHA(void);
    void PHP(void);
    void PLA(void);
    void PLP(void);
    void BRK(void);
    void RTI(void);
    void JMP(void);
    void RTS(void);
    void JSR(void);
    void branch(void);
    void BNE(void);
    void BEQ(void);
    void BVC(void);
    void BVS(void);
    void BCC(void);
    void BCS(void);
    void BPL(void);
    void BMI(void);
    void TAX(void);
    void TXA(void);
    void TAY(void);
    void TYA(void);
    void TXS(void);
    void TSX(void);
    void CLC(void);
    void SEC(void);
    void CLI(void);
    void SEI(void);
    void CLV(void);
    void CLD(void);
    void SED(void);
    void NOP(void);
    // 65C02 instruction set additions. The original 6502 dispatched all
    // these opcodes to Unoff (no-op) or Hang (PC-- spin) — fine for clean
    // 6502 code, but it bricks 65C02-targeted ProDOS software (most IIe
    // Enhanced / IIc / IIc Plus games and shells).
    void BRA(void);          // $80 — branch always
    void STZ(void);          // $64/$74/$9C/$9E — store zero
    void INA(void);          // $1A — increment A
    void DEA(void);          // $3A — decrement A
    void PHX(void);          // $DA — push X
    void PHY(void);          // $5A — push Y
    void PLX(void);          // $FA — pull X
    void PLY(void);          // $7A — pull Y
    void BIT_imm(void);      // $89 — BIT #imm (only Z affected)
    void TSB(void);          // $04/$0C — test + set bits
    void TRB(void);          // $14/$1C — test + reset bits

    // Rockwell 65C02 SMBn / RMBn (set/reset bit n in zp), 2 bytes,
    // 5 cycles. Distinct opcodes per bit so the dispatch table can
    // call them directly. RMB0=0x07, RMB1=0x17, ..., RMB7=0x77.
    // SMB0=0x87, SMB1=0x97, ..., SMB7=0xF7 — the low half of the $x7
    // column RESETS, the high half SETS, as kCmosTable and
    // Disassembler6502 have it. (This comment had the two swapped.)
    template <int N> void SMBn(void);
    template <int N> void RMBn(void);

    // Rockwell 65C02 BBRn / BBSn (branch on bit reset/set in zp),
    // 3 bytes (opcode + zp + signed offset), 5 cycles + 1 if branch
    // taken (+1 more if page-crossed). BBR0=0x0F, ..., BBR7=0x7F.
    // BBS0=0x8F, ..., BBS7=0xFF.
    template <int N> void BBRn(void);
    template <int N> void BBSn(void);

    // WDC 65C02 WAI / STP. WAI halts the CPU until an IRQ/NMI fires;
    // we model the halt by parking PC at the WAI instruction (so it
    // re-executes once IRQ clears the I flag and wakes the CPU).
    // STP halts until reset; we just hang PC the same way.
    void WAI(void);
    void STP(void);

    void Unoff(void);
    void Unoff1(void);
    void Unoff2(void);
    void Unoff3(void);
    void UnoffImm(void);    // 2-byte, 2-cycle undoc NOP (imm) / NMOS ANC/SBC #imm
    void UnoffZpX(void);    // 2-byte, 4-cycle undoc NOP (zp,X)
    void UnoffAbs4(void);   // 3-byte, 4-cycle undoc NOP (abs)
    void UnoffAbsX(void);   // 3-byte, 4+p-cycle undoc NOP (abs,X, NMOS)
    void Unoff5C(void);     // 3-byte, 8-cycle 65C02 oddball $5C
    void Hang(void);
    void executeOpcode(void);

    /// Per-instruction debug hook, or nullptr. See setDebugHook().
    M6502DebugHook* debugHook_ = nullptr;

    /// Emit the intermediate RMW bus cycle between the initial read and
    /// the modified-value write. On NMOS (MAME `om6502.lst:161-164`)
    /// it's a write of the *original* value; on CMOS (`ow65c02.lst`)
    /// it's a dummy read. Both dispatch through `softSwitchAccess`
    /// when the address is in $C000-$C07F, which is what makes
    /// `INC $C030` actually toggle the speaker twice on writes (vs
    /// once before this fix). The reset-arm of $C070 paddle latch
    /// similarly fires twice for `INC $C070`, etc.
    void rmwSecondBusCycle(uint16_t addr, uint8_t origValue);

    struct OpcodeEntry {
        void (M6502::*addrMode)();
        void (M6502::*operation)();
    };
    // Master 65C02 table — used as the source for every setCpuMode()
    // rebuild. The instance `opcodeTable` is mutated in place when
    // switching to NMOS so the dispatch loop stays a simple array
    // lookup.
    static const OpcodeEntry kCmosTable[256];
    OpcodeEntry opcodeTable[256]{};
    CpuMode     cpuMode = CpuMode::CMOS;

    /// Set by `STP` ($CB on 65C02 / W65C02). When true, `step()` skips
    /// opcode dispatch *and* IRQ/NMI service — only `softReset()` /
    /// `hardReset()` can clear it (matches WDC + MAME
    /// `ow65c02.lst:715-718` where STP is a `for(;;)` loop that only
    /// `reset_c` can exit). Previously POM2 parked PC and let NMI vector
    /// out, which woke STP spuriously.
    bool halted = false;




};

#endif // M6502_H
