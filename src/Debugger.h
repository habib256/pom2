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

// Run-control debugger: breakpoints, watchpoints, and the stop reason.
//
// Why this exists, in the words of the 2026-08-22 architecture audit: POM2 had
// every brick of a debugger except the machinery that stops the machine.
// `Disassembler6502` and `MemoryViewer_ImGui` were already there, and
// `EmulationController` already had a Step mode — but nothing anywhere could
// answer "stop when the PC reaches $C600" or "stop when somebody writes $3F0".
// The cost of that gap was not hypothetical and it was countable: `tests/` had
// accumulated 24 one-off trace / dump / probe binaries, 5535 lines, of which
// 4129 were registered as no test at all and simply rebuilt on every build.
// Every parity hunt was paying for a throwaway binary because there was no way
// to interrogate a running machine.
//
// So this is not a user feature. It is the instrument POM2 is debugged WITH,
// and the item whose return is every other item in the backlog.
//
// ── Threading ────────────────────────────────────────────────────────────
// State is guarded by the emulator's stateMutex, except for transient
// cancellation below. The CPU worker holds that lock for its 4096-cycle chunk, so
// every onInstruction / noteBankAccess call happens inside it. UI mutations
// take controller->lockState() so they cannot race the worker. clearTransient
// is the exception: its atomic flag lets setMode(Stopped) cancel run control
// without acquiring the non-recursive state lock. See that method below.
//
// ── Cost when nobody is debugging ────────────────────────────────────────
// armed() includes breakpoints, watches, transients, profiling and history.
// With all disabled, the production CPU/memory fast paths stay intact:
//
//   * `M6502::run` picks between two loops ONCE per call — per 4096-cycle
//     chunk, not per instruction — so an un-armed debugger is unmeasurable.
//   * Write watches divert addresses through writable[]; read watches use
//     derived fast-read gates. Neither adds a pointer test per bus access.
//     See MemoryWatch.cpp and DEV.md § Debugger for the measurements.

#ifndef POM2_DEBUGGER_H
#define POM2_DEBUGGER_H

#include "InstructionTraceSink.h"
#include "InterruptLoad.h"
#include "M6502.h"
#include "MemoryWatchSink.h"
#include "OsCallTrace.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <vector>

namespace pom2 {

class Debugger : public M6502DebugHook, public MemoryWatchSink
{
public:
    /// Why the machine stopped. `None` means it did not.
    enum class Reason {
        None,
        Breakpoint,     ///< PC reached an armed address
        WatchRead,      ///< an armed address was read
        WatchWrite,     ///< an armed address was written
        StepOver,       ///< the transient breakpoint a step-over installs
        RunToCursor,    ///< the transient breakpoint run-to-cursor installs
        StepOut,        ///< the current subroutine has returned
        RunToBeam,      ///< the beam reached the position asked for
        StepBack,       ///< one instruction back, through the rewind ring
        RunBackStart,   ///< Run Back met no breakpoint: the oldest point kept
        RunBackPaused,  ///< Run Back's time was up, no breakpoint so far
    };

    struct Hit {
        Reason   reason = Reason::None;
        uint16_t pc     = 0;      ///< PC at the moment of the stop
        uint16_t addr   = 0;      ///< watched address (watchpoints only)
        uint8_t  value  = 0;      ///< byte read or written (watchpoints only)
        DebugBank pcBank{}, bank{};
        /// A watch hit made by a DMA card's processor (the SoftCard Z80)
        /// while it held the bus: `pc` is the parked 6502's, not the writer.
        bool     busMaster = false;
        /// Beam clock (`Memory::videoNow`) at the stop: the instruction
        /// boundary for a PC stop, the ACCESS's own cycle for a memory one.
        uint64_t videoCycle = 0;
        bool     valid() const { return reason != Reason::None; }
    };

    /// Watch a read, a write, or both. `None` removes the watchpoint.
    enum Access : uint8_t { None = 0, Read = 1, Write = 2, ReadWrite = 3 };

    /// Bind the live memory once; mapping is only queried on the debug loop.
    void setMemory(const Memory* memory) { memory_ = memory; }
    void addBreakpoint(uint16_t addr, DebugBank bank);
    void removeBreakpoint(uint16_t addr, DebugBank bank);
    bool hasBreakpoint(uint16_t addr, DebugBank bank) const;
    std::vector<DebugAddress> bankBreakpoints() const;
    // ── Breakpoints ──────────────────────────────────────────────────────
    void addBreakpoint(uint16_t addr);
    void removeBreakpoint(uint16_t addr);
    void toggleBreakpoint(uint16_t addr);
    void clearBreakpoints();
    bool hasBreakpoint(uint16_t addr) const;
    enum class Register { A, X, Y, P, SP, PC };
    enum class Compare { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
    struct Condition {
        Register reg = Register::A;
        Compare compare = Compare::Equal;
        uint16_t value = 0;
        uint16_t mask = 0xFFFF;
    };
    /// One register predicate per PC breakpoint. Unconditional add replaces
    /// an existing predicate; CPU lifetime must include the debugger's.
    void addConditionalBreakpoint(uint16_t addr, Condition condition, const M6502& cpu);
    void addConditionalBreakpoint(uint16_t addr, Condition condition, const M6502& cpu, DebugBank bank);
    const std::map<DebugAddress, Condition>& bankConditions() const { return bankConditions_; }
    const std::map<uint16_t, Condition>& conditions() const { return conditions_; }
    /// Add or replace the register predicate of an EXISTING breakpoint, or
    /// drop it (`on` false). Nothing else about the breakpoint changes.
    void setCondition(DebugAddress at, bool on, Condition condition, const M6502& cpu);

    // ── Gates: "stop only when…" ─────────────────────────────────────────
    // French Touch's request (2026-11-27). A breakpoint — on the PC or on a
    // memory access — may carry a gate: a window of the BEAM it must be in
    // (stop on a write to $C054 only outside horizontal blanking), a test
    // on the byte read or written (memory breakpoints only), and a number
    // of qualifying passes to let go by first. Every breakpoint counts its
    // qualifying passes, gate or not.
    //
    // The beam is the one of BeamPosition.h: V = line in the frame, H =
    // cycle in the 65-cycle line. A PC breakpoint is tested at the
    // instruction's first cycle; a memory breakpoint at the cycle of the
    // access itself (`Memory::videoAccessCycle`). A window whose minimum is
    // above its maximum WRAPS (V 300..10 = the end of vertical blanking
    // and the top of the picture).
    struct Gate {
        bool     beam = false;
        uint16_t vMin = 0, vMax = 311;
        uint8_t  hMin = 0, hMax = 64;
        bool     valueOn = false;
        Compare  valueCompare = Compare::Equal;
        uint8_t  value = 0;
        uint32_t ignore = 0;          ///< qualifying passes let go by
        bool plain() const { return !beam && !valueOn && ignore == 0; }
    };
    struct GateState {
        Gate     gate;
        uint64_t passes = 0;          ///< qualifying passes so far
        uint64_t countedAt = 0;       ///< instruction that counted the last one
        bool     countedWrite = false; ///< ...and it was a write (the RMW rule)
    };
    using GateMap = std::map<DebugAddress, GateState>;
    /// Keyed like the lists: bank `Any` for an every-bank breakpoint.
    /// Setting a gate starts its pass count over.
    /// No such breakpoint: nothing happens (a gate never outlives or
    /// precedes the breakpoint it qualifies).
    void setBreakpointGate(DebugAddress at, const Gate& gate);
    void setWatchGate(DebugAddress at, const Gate& gate);
    const GateMap& breakpointGates() const { return bpGates_; }
    const GateMap& watchGates() const      { return wpGates_; }
    void resetPassCounts();
    static bool compare(Compare how, unsigned value, unsigned against);
    static bool beamInside(const Gate& gate, int line, int h);

    /// Sorted, for the UI list. O(64K) — a UI-frame operation, never hot.
    std::vector<uint16_t> breakpoints() const;
    std::size_t breakpointCount() const { return bpCount_ + bankBreakpoints_.size(); }

    // ── Watchpoints ──────────────────────────────────────────────────────
    void setWatchpoint(uint16_t addr, Access access);
    Access watchpointAt(uint16_t addr) const;
    void clearWatchpoints();
    void setWatchpoint(uint16_t addr, Access access, DebugBank bank);
    struct Watch { uint16_t addr; Access access; DebugBank bank{}; };
    std::vector<Watch> watchpoints() const;
    std::size_t watchpointCount() const { return wpCount_ + bankWatches_.size(); }

    // ── Transient stops ──────────────────────────────────────────────────
    /// Break once at addr/bank, then forget it (run-to-cursor). Call stepping
    /// uses setStepOver/setStepOut's frame tracking instead of a PC+3 target.
    /// A transient leaves a user breakpoint at the same address installed.
    void setTransient(uint16_t addr, Reason reason, DebugBank bank = {});
    /// Follow the actual outer RTS, including inline arguments, recursion and
    /// stack wrap. Only the debugged loop consults the CPU/memory references.
    void setStepOver(M6502& cpu, const Memory& memory);
    void setStepOut(M6502& cpu, const Memory& memory);
    /// Stop at the first instruction boundary at or after beam-clock cycle
    /// `videoCycle` (`Memory::videoNow`'s clock) — so up to one instruction
    /// past the position asked for, never before it.
    void setRunToBeam(uint64_t videoCycle);
    /// The same stop while a card's processor holds the bus: no 6502
    /// instruction is offered then, so the controller asks between the
    /// card's own instructions (and runs it in single ones while armed).
    bool beamStopArmed() const {
        return transientArmed_.load(std::memory_order_relaxed) && !stepCpu_ &&
               transientReason_ == Reason::RunToBeam;
    }
    bool beamStopDue(uint16_t parkedPc);
    struct ProfileEntry { uint16_t pc; uint64_t visits; uint64_t cycles; DebugBank bank{}; };
    struct TraceEntry {
        uint16_t pc, nextPc;
        uint8_t a, x, y, p, sp;
        int cycles;
        DebugBank bank{};
    };
    void setProfiling(bool enabled);
    void setHistoryEnabled(bool enabled);
    /// UI-017: every instruction to a tracer (InstructionTrace.h), or null.
    /// Arms the debugger like profiling does; the caller holds `stateMutex`
    /// and re-syncs the hook (EmulationController::syncDebugHook). The sink
    /// must outlive its attachment.
    void setInstructionTrace(InstructionTraceSink* sink) { trace_ = sink; }
    InstructionTraceSink* instructionTrace() const { return trace_; }
    bool profiling() const { return profiling_; }
    bool historyEnabled() const { return historyEnabled_; }
    void clearMeasurements();
    std::vector<ProfileEntry> profile(std::size_t limit = 20) const;
    std::vector<TraceEntry> history() const;
    /// Share of each video frame spent inside IRQ/NMI handlers, measured
    /// while profiling is on (InterruptLoad.h). Cleared with the measurements.
    const InterruptLoad& interruptLoad() const { return interruptLoad_; }
    void onStepCompleted(const M6502& cpu, uint16_t pc, int cycles) override;
    void onInterruptEntered(const M6502& cpu, uint16_t interruptedPc, int cycles) override;
    /// Disarm without firing. Safe to call from a thread that does NOT hold
    /// `stateMutex` — the only member of this class for which that is true,
    /// and deliberately so: `EmulationController::setMode(Mode::Stopped)` is
    /// the Stop verb and MUST NOT take the lock (some of its callers reach it
    /// already holding it, and the lock is non-recursive). See the
    /// `transientArmed_` declaration.
    void clearTransient();
    bool hasTransient() const {
        return transientArmed_.load(std::memory_order_relaxed);
    }

    // ── State the CPU and Memory consult ─────────────────────────────────
    /// Anything at all to check. False → the CPU stays on its fast loop.
    bool armed() const {
        return breakpointCount() || watchpointCount() || profiling_ || historyEnabled_ ||
               trace_ != nullptr ||
               osCalls_.any6502() ||
               transientArmed_.load(std::memory_order_relaxed);
    }
    /// Watchpoints specifically — what Memory gates its hook on.
    bool watchArmed() const { return watchpointCount() != 0; }

    /// M6502DebugHook. Called before the instruction at `pc` is executed;
    /// true halts the CPU with `pc` still pointing at it, so the instruction
    /// has NOT run and resuming re-tries it.
    bool onInstruction(uint16_t pc) override;
    /// Explicit stepping needs attribution without testing PC breakpoints.
    void beginStep(uint16_t pc, bool explicitStep = false);

    /// Called by Memory for an access to a watched address. Records the hit;
    /// the CPU halts at the END of the current instruction (the access is
    /// already in flight and cannot be un-done), so the reported PC is the
    /// instruction that performed it — `onInstruction` returns the stop at
    /// the next boundary, where the machine's live PC is the instruction
    /// AFTER the write. Two different, both useful, facts: `Hit::pc` is who
    /// wrote, the CPU's PC is where you resume.
    ///
    /// Both halves reach here, by different routes: a write watch is an
    /// address diverted off `memWrite`'s fast path through `writable[]`
    /// (free, armed or not); a read watch flips `Memory::readDivert_`, which
    /// sends EVERY read through `memReadSlow` while one is armed (free when
    /// none is). Memory.h § Write / § Read watchpoints, PERFORMANCE § 8.3/8.5.
    void noteAccess(uint16_t addr, uint8_t value, bool write) override;
    void noteBankAccess(uint16_t addr, uint8_t value, bool write, DebugBank bank) override;

    /// Run `slice` (a DMA card's `dmaRun`) with its bus accesses credited to
    /// the card's processor: a watch hit it makes is `Hit::busMaster`.
    template <class F> int runAsBusMaster(F&& slice)
    {
        busMaster_ = true;
        const int spent = slice();
        busMaster_ = false;
        return spent;
    }

    /// The debugger's OWN write into guest memory (the assembler's patch),
    /// through the CPU's write mapping. Neither a watch hit nor a time jump:
    /// the stop it is made from, its resume amnesty and the open OS-call
    /// trace all stay as they are. Caller holds `stateMutex`.
    void patchMemory(Memory& memory, uint16_t addr, const uint8_t* bytes, std::size_t count);

    // ── Replaying history (Step Back / Run Back) ─────────────────────────
    // EmulationController_TimeTravel.cpp re-executes instructions from a
    // rewind frame. That is the PAST being read again, not the program
    // running: while a replay is open no pass is counted and no "ignore N"
    // applies (a breakpoint is asked "would you qualify here", every time),
    // and the caller steps the CPU directly — the hook stays attached, none
    // of its methods is called — so nothing is traced, profiled or offered
    // to the OS-call trace.
    void setReplaying(bool on) { replaying_ = on; osCalls_.setMuted(on); }
    /// POM2's own writes into guest memory in progress (a boot's screen and
    /// zero-page priming): no stop, no pass. patchMemory does it for itself.
    void setOwnWrites(bool on) { ownWrite_ = on; }
    /// Would a breakpoint stop at the boundary before `pc`? True for a PC
    /// breakpoint there and for a memory breakpoint the PREVIOUS instruction
    /// tripped; the hit is then in lastHit(). Replay only.
    bool probeInstruction(uint16_t pc);
    /// Report `hit` as the reason the machine stands where it does.
    void setHit(const Hit& hit) { hit_ = hit; }

    /// True once something has asked the machine to stop and the stop has not
    /// been consumed yet. `EmulationController` drains this at its chunk
    /// boundary and parks the worker.
    bool stopRequested() const { return hit_.valid(); }

    Hit  lastHit() const { return hit_; }
    void clearHit();

    /// Suppress the breakpoint at `pc` for exactly one instruction. Resuming
    /// from a stop would otherwise re-trigger the same breakpoint forever,
    /// because the PC still points at it. Spent when that instruction
    /// EXECUTES (onStepCompleted at `pc`), not when it is offered: an
    /// interrupt taken first offers it again after the RTI (DBG-1).
    /// `sp` is the stack pointer at the stop. An amnesty armed from an
    /// interrupt handler (a deeper stack) keeps the one its interrupt is
    /// holding up; any other is replaced. With no `sp` (-1), replaced always.
    void armResumeFrom(uint16_t pc, int sp = -1);

    /// Drop every per-TIMELINE transient. Called when the machine jumps in
    /// time — a rewind scrub, a `.pom2snap` load, an AI `/snapshot/load`.
    ///
    /// All four pieces of state below name an address in a timeline that no
    /// longer exists. The armed resume amnesty is the one that bit: it
    /// suppresses the breakpoint at ONE pc for ONE instruction, and after a
    /// restore that pc is wherever the user last resumed from, so the first
    /// breakpoint the restored machine reaches at that address is silently
    /// skipped. The latched `hit_` is worse in the other direction — it
    /// still reads `stopRequested()`, so the controller parks the worker
    /// again the instant it resumes, reporting a breakpoint the restored
    /// machine never hit.
    ///
    /// Breakpoints and watchpoints themselves are USER state and survive:
    /// they are addresses in a program, not in a timeline. Caller must hold
    /// `stateMutex`.
    /// `keepMeasurements`: Step Back and Run Back keep the profile and the
    /// history (trimmed by the caller) — wiping the trail on every step
    /// back emptied the very list the user was walking back through.
    void clearForTimeJump(bool keepMeasurements = false);
    /// Forget the newest history entry if it is the instruction at `pc`
    /// (the one a Step Back just un-did); or all of it (Run Back).
    void dropHistoryOf(uint16_t pc);
    void clearHistory() { historyHead_ = historyCount_ = 0; }

    /// ProDOS MLI / CP/M BDOS call trace. Its MLI half runs off this hook;
    /// switch it through EmulationController::setOsCallTrace, which also
    /// re-syncs the hook and hangs the BDOS half on the card Z80s.
    OsCallTrace& osCalls() { return osCalls_; }
    const OsCallTrace& osCalls() const { return osCalls_; }

private:
    void ensureBpBits();
    void ensureWpBits();
    bool conditionMatches(uint16_t addr) const;
    bool conditionMatches(const Condition& condition) const;
    GateMap bpGates_, wpGates_;
    /// The qualifying-pass test of one breakpoint; counts the pass.
    bool gateOpens(GateMap& gates, const DebugAddress& key, uint64_t videoCycle,
                   int value, bool write, bool mayRepeat);
    /// Take back the pass `key` counted for the instruction now offered.
    void uncountPass(GateMap& gates, const DebugAddress& key);
    uint64_t beamTarget_ = 0;             // setRunToBeam
    bool replaying_ = false;              // see setReplaying
    uint64_t hitSerial_ = 0;              // instruction that latched hit_ (memory hits)
    bool ownWrite_ = false;               // patchMemory in progress: not the guest's access
    // (A halted CPU — an NMOS JAM; the 65C02 of POM2's machines is run as
    // the NCR part, where WAI and STP are no-ops — is offered its PC every
    // two cycles, and a gated
    // breakpoint there counts each offer. A rule that judged it once was
    // tried on 2026-11-27 and removed the same day: four ways to miss a real
    // stop, for a case no Apple II program reaches on purpose.)
    bool steppedOffAmnesty_ = false;      // see beginStep
    bool breakpointStops(uint16_t pc);    // the PC-breakpoint half of onInstruction
    uint64_t serial_ = 0;                 // instructions offered, for countedAt
    std::map<DebugAddress, Condition> bankConditions_;
    std::map<uint16_t, Condition> conditions_;
    const M6502* conditionCpu_ = nullptr;
    bool profiling_ = false;
    bool historyEnabled_ = false;
    std::map<DebugAddress, ProfileEntry> profile_;
    InterruptLoad interruptLoad_;
    /// Video frame the instruction that just retired STARTED in.
    uint64_t frameOfRetired(int cycles) const;
    const Memory* memory_ = nullptr;
    InstructionTraceSink* trace_ = nullptr;
    DebugBank curBank_{};
    DebugBank resolveBank(uint16_t addr, bool write = false) const;
    std::map<DebugAddress, bool> bankBreakpoints_;
    std::map<DebugAddress, Access> bankWatches_;
    std::vector<TraceEntry> history_;
    std::size_t historyHead_ = 0;
    std::size_t historyCount_ = 0;

    // 64 Ki addresses, one bit each = 8 KiB. Flat, O(1), no allocation on the
    // check path, and it fits in L1 alongside everything else the CPU loop
    // touches. Allocated lazily so a session that never debugs never pays it.
    std::vector<uint8_t> bpBits_;
    // Two bits per address (read / write) packed as one byte for simplicity:
    // watchpoints are rare enough that 64 KiB is not worth compressing, and a
    // byte load beats a shift-and-mask on the memory hot path.
    std::vector<uint8_t> wpBits_;

    std::size_t bpCount_ = 0;
    std::size_t wpCount_ = 0;

    DebugBank transientBank_{};
    uint16_t transientAddr_   = 0;
    Reason   transientReason_ = Reason::None;
    M6502* stepCpu_ = nullptr;
    const Memory* stepMemory_ = nullptr;
    uint8_t stepStack_ = 0;
    bool stepReturning_ = false;
    // Nested calls inside a step: the SP each JSR was executed at. A frame
    // closes when SP climbs back to that level (its RTS, or a routine that
    // pops its own return address and jumps). See onInstruction.
    std::vector<uint8_t> stepFrames_;
    bool    stepSkipFirst_ = false;   // the step-over's own JSR is not nested
    uint16_t stepJsrPc_    = 0;       // ...and is the one at this PC and SP
    bool    stepOutMode_   = false;
    uint8_t stepStartSp_   = 0;
    /// Step Out: the stack as it stood above the start level when the step
    /// began (index = slot - $0100). See stepOutPopIsAReturn.
    std::array<uint8_t, 256> stepStackAtStart_{};
    bool stepOutPopIsAReturn(int after) const;
    /// Atomic, alone among this class's members, because Stop clears it from
    /// the UI thread while the CPU worker may be reading it in
    /// `onInstruction`. Everything else here is guarded by `stateMutex`
    /// (see § Threading), but the Stop verb — `setMode(Mode::Stopped)` —
    /// cannot take that lock: callers such as the Disk II Library's boot
    /// buttons already hold it when they reach setMode, and it is not
    /// recursive. A relaxed load is exactly the cost of the plain bool it
    /// replaces, and only on the debugged loop. `transientAddr_`/`Reason`
    /// stay plain: they are only ever READ once this flag is seen true, and
    /// the clear never rewrites them.
    std::atomic<bool> transientArmed_{false};

    /// Resume amnesties (armResumeFrom). Usually one; more only while an
    /// interrupt holds up the instruction an earlier one names: stop on X
    /// with an IRQ due, Run, stop on the handler's H, Run — X's must survive
    /// H's, or X stops again after the RTI having executed nothing, its pass
    /// counted twice (stabilisation pass 2026-11-27).
    struct Amnesty { uint16_t pc = 0; DebugBank bank{}; int sp = -1; };
    std::vector<Amnesty> amnesties_;
    Amnesty steppedOff_{};                // see beginStep / steppedOffAmnesty_
    bool hasAmnesty(uint16_t pc) const;
    bool takeAmnesty(uint16_t pc, Amnesty* taken = nullptr);

    /// PC of the instruction currently executing, latched by `onInstruction`.
    /// `noteAccess` fires from inside that instruction, where the CPU's own
    /// programCounter has already walked past the opcode and operands.
    uint16_t curPc_ = 0;
    bool busMaster_ = false;              // see runAsBusMaster

    Hit hit_{};
    OsCallTrace osCalls_;
};

} // namespace pom2

#endif // POM2_DEBUGGER_H
