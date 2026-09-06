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
// The whole class is guarded by the emulator's `stateMutex`, and by nothing
// else. That is not a shortcut, it is the cheapest CORRECT arrangement: the
// CPU worker already holds that lock for the whole of its 4096-cycle chunk, so
// every `checkPc` / `noteAccess` call below happens inside it, and a UI thread
// that mutates a breakpoint under the same lock cannot race them. No atomics,
// no second mutex, no lock-free bitmap. Callers on the UI side must take
// `controller->stateMutex()` around every mutating call — `Debugger_ImGui`
// does, in one place.
//
// ── Cost when nobody is debugging ────────────────────────────────────────
// `armed()` is false until the first breakpoint or watchpoint exists, and both
// hot paths are written so that costs one predictable branch and nothing else:
//
//   * `M6502::run` picks between two loops ONCE per call — per 4096-cycle
//     chunk, not per instruction — so an un-armed debugger is unmeasurable.
//   * `Memory::memRead`/`memWrite` test one pointer at the top and otherwise
//     run byte-for-byte the code they ran before. That one IS per access, on
//     the hottest path in the emulator, so it was measured rather than
//     assumed — see DEV.md § Debugger for the numbers.

#ifndef POM2_DEBUGGER_H
#define POM2_DEBUGGER_H

#include "M6502.h"
#include "MemoryWatchSink.h"

#include <atomic>
#include <cstdint>
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
    };

    struct Hit {
        Reason   reason = Reason::None;
        uint16_t pc     = 0;      ///< PC at the moment of the stop
        uint16_t addr   = 0;      ///< watched address (watchpoints only)
        uint8_t  value  = 0;      ///< byte read or written (watchpoints only)
        bool     valid() const { return reason != Reason::None; }
    };

    /// Watch a read, a write, or both. `None` removes the watchpoint.
    enum Access : uint8_t { None = 0, Read = 1, Write = 2, ReadWrite = 3 };

    // ── Breakpoints ──────────────────────────────────────────────────────
    void addBreakpoint(uint16_t addr);
    void removeBreakpoint(uint16_t addr);
    void toggleBreakpoint(uint16_t addr);
    void clearBreakpoints();
    bool hasBreakpoint(uint16_t addr) const;
    /// Sorted, for the UI list. O(64K) — a UI-frame operation, never hot.
    std::vector<uint16_t> breakpoints() const;
    std::size_t breakpointCount() const { return bpCount_; }

    // ── Watchpoints ──────────────────────────────────────────────────────
    void setWatchpoint(uint16_t addr, Access access);
    Access watchpointAt(uint16_t addr) const;
    void clearWatchpoints();
    struct Watch { uint16_t addr; Access access; };
    std::vector<Watch> watchpoints() const;
    std::size_t watchpointCount() const { return wpCount_; }

    // ── Transient stops ──────────────────────────────────────────────────
    /// Break once at `addr`, then forget it. Used by step-over (the address
    /// after a JSR) and run-to-cursor. A transient at an address that also
    /// carries a real breakpoint leaves the real one alone.
    void setTransient(uint16_t addr, Reason reason);
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
        return bpCount_ || wpCount_ ||
               transientArmed_.load(std::memory_order_relaxed);
    }
    /// Watchpoints specifically — what Memory gates its hook on.
    bool watchArmed() const { return wpCount_ != 0; }

    /// M6502DebugHook. Called before the instruction at `pc` is executed;
    /// true halts the CPU with `pc` still pointing at it, so the instruction
    /// has NOT run and resuming re-tries it.
    bool onInstruction(uint16_t pc) override;

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

    /// True once something has asked the machine to stop and the stop has not
    /// been consumed yet. `EmulationController` drains this at its chunk
    /// boundary and parks the worker.
    bool stopRequested() const { return hit_.valid(); }

    Hit  lastHit() const { return hit_; }
    void clearHit();

    /// Suppress the breakpoint at `pc` for exactly one instruction. Resuming
    /// from a stop would otherwise re-trigger the same breakpoint forever,
    /// because the PC still points at it.
    void armResumeFrom(uint16_t pc) { resumeSkip_ = pc; resumeSkipArmed_ = true; }

private:
    void ensureBpBits();
    void ensureWpBits();

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

    uint16_t transientAddr_   = 0;
    Reason   transientReason_ = Reason::None;
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

    uint16_t resumeSkip_      = 0;
    bool     resumeSkipArmed_ = false;

    /// PC of the instruction currently executing, latched by `onInstruction`.
    /// `noteAccess` fires from inside that instruction, where the CPU's own
    /// programCounter has already walked past the opcode and operands.
    uint16_t curPc_ = 0;

    Hit hit_{};
};

} // namespace pom2

#endif // POM2_DEBUGGER_H
