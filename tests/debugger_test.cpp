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

// Run-control debugger — pins src/Debugger.{h,cpp} and the M6502 hook.
//
// The contract these cases exist to hold, in order of how badly a regression
// would hurt:
//
//   1. A breakpoint stops BEFORE its instruction runs. That is the whole
//      point of a breakpoint: the registers the user reads are the state
//      going in, and resuming re-tries the instruction rather than skipping
//      it. A hook called after the instruction would look almost right and be
//      useless.
//   2. Resuming actually moves. The PC still points at the breakpoint when
//      you press Run, so without one instruction of amnesty the machine
//      re-triggers the same stop forever and the button appears dead.
//   3. An un-armed debugger leaves the CPU on its ordinary loop. This is the
//      performance contract: measured on pom2_bench, the hook costs nothing
//      when detached, and it only stays nothing while `armed()` gates it.
//   4. Transients are one-shot. Step-over installs a breakpoint at the return
//      address; leaving it behind would turn every step-over into a permanent
//      breakpoint the user never asked for.
//   5. A write watchpoint stops the machine, names the instruction that wrote,
//      and the write still LANDS. Watchpoints are implemented by diverting the
//      address off memWrite's fast path (clearing its `writable[]` byte), so
//      the danger specific to this design is that arming a watch silently
//      write-protects the address — which would corrupt the machine being
//      debugged rather than merely failing to stop it.

#include "CassetteDevice.h"
#include "Debugger.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {

// A tiny program at $0300:
//   $0300  LDA #$11     A9 11
//   $0302  LDA #$22     A9 22
//   $0304  LDA #$33     A9 33
//   $0306  JMP $0306    4C 06 03
constexpr uint16_t kStart = 0x0300;
const uint8_t kProgram[] = {
    0xA9, 0x11,
    0xA9, 0x22,
    0xA9, 0x33,
    0x4C, 0x06, 0x03,
};

void loadProgram(Memory& mem)
{
    for (std::size_t i = 0; i < sizeof(kProgram); ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(kStart + i), kProgram[i]);
}

// A program that STORES, at $0310:
//   $0310  LDA #$5A     A9 5A
//   $0312  STA $0350    8D 50 03
//   $0315  LDA #$77     A9 77
//   $0317  JMP $0317    4C 17 03
constexpr uint16_t kStoreStart = 0x0310;
constexpr uint16_t kStorePc    = 0x0312;   // the STA itself
constexpr uint16_t kStoreNext  = 0x0315;   // where the machine resumes
constexpr uint16_t kStoreAddr  = 0x0350;
const uint8_t kStoreProgram[] = {
    0xA9, 0x5A,
    0x8D, 0x50, 0x03,
    0xA9, 0x77,
    0x4C, 0x17, 0x03,
};

void loadStoreProgram(Memory& mem)
{
    for (std::size_t i = 0; i < sizeof(kStoreProgram); ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(kStoreStart + i), kStoreProgram[i]);
}

// Arm a write watchpoint the way EmulationController::syncDebugHook() does:
// the Debugger owns the user-facing set, Memory owns the diversion. A test
// that armed only one of the two would pass while the shipped path was broken.
void armWriteWatch(Memory& mem, pom2::Debugger& dbg, uint16_t addr)
{
    dbg.setWatchpoint(addr, pom2::Debugger::Write);
    mem.setWatchSink(&dbg);
    mem.setWriteWatch(addr, true);
}

void disarmWriteWatch(Memory& mem, pom2::Debugger& dbg, uint16_t addr)
{
    dbg.setWatchpoint(addr, pom2::Debugger::None);
    mem.setWriteWatch(addr, false);
}

// ── 1. Bookkeeping ───────────────────────────────────────────────────────
void testBreakpointBookkeeping()
{
    pom2::Debugger dbg;
    assert(!dbg.armed() && "a fresh debugger must be un-armed");
    assert(dbg.breakpointCount() == 0);

    dbg.addBreakpoint(0x0302);
    assert(dbg.armed());
    assert(dbg.hasBreakpoint(0x0302));
    assert(!dbg.hasBreakpoint(0x0303));
    assert(dbg.breakpointCount() == 1);

    // Adding twice must not double-count, or removing once would leave a
    // phantom that keeps the CPU on its slow loop forever.
    dbg.addBreakpoint(0x0302);
    assert(dbg.breakpointCount() == 1);

    dbg.addBreakpoint(0xFFFF);          // top of the address space
    dbg.addBreakpoint(0x0000);          // and the bottom
    assert(dbg.breakpointCount() == 3);
    const std::vector<uint16_t> list = dbg.breakpoints();
    assert(list.size() == 3);
    assert(list[0] == 0x0000 && list[1] == 0x0302 && list[2] == 0xFFFF &&
           "breakpoints() must come back sorted for the UI list");

    dbg.toggleBreakpoint(0x0302);
    assert(!dbg.hasBreakpoint(0x0302) && dbg.breakpointCount() == 2);
    dbg.toggleBreakpoint(0x0302);
    assert(dbg.hasBreakpoint(0x0302) && dbg.breakpointCount() == 3);

    // Removing one that is not there must not underflow the count.
    dbg.removeBreakpoint(0x1234);
    assert(dbg.breakpointCount() == 3);

    dbg.clearBreakpoints();
    assert(!dbg.armed() && dbg.breakpointCount() == 0);
    assert(!dbg.hasBreakpoint(0x0302));

    // Watchpoint bookkeeping: the access is stored as asked, because both
    // halves can fire (cases 7 and 10), and re-arming with a different access
    // REPLACES rather than accumulates — one address, one entry.
    dbg.setWatchpoint(0x0300, pom2::Debugger::Read);
    assert(dbg.watchpointAt(0x0300) == pom2::Debugger::Read);
    assert(dbg.watchpointCount() == 1 && dbg.armed());
    dbg.setWatchpoint(0x0300, pom2::Debugger::Write);
    assert(dbg.watchpointAt(0x0300) == pom2::Debugger::Write);
    assert(dbg.watchpointCount() == 1);
    dbg.setWatchpoint(0x0300, pom2::Debugger::None);
    assert(dbg.watchpointAt(0x0300) == pom2::Debugger::None);
    assert(dbg.watchpointCount() == 0 && !dbg.armed());

    std::printf("[ OK ] breakpoint bookkeeping\n");
}

// ── 2. THE case: a breakpoint stops before its instruction ───────────────
void testBreakStopsBeforeTheInstruction()
{
    Memory mem;
    M6502  cpu(&mem);
    pom2::Debugger dbg;
    loadProgram(mem);

    // Break on the SECOND LDA. When it fires, A must still hold $11 from the
    // first one — proof the instruction at the breakpoint has not run.
    dbg.addBreakpoint(0x0302);
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(kStart);

    const int spent = cpu.run(1000);
    assert(dbg.stopRequested() && "the breakpoint did not fire");

    const pom2::Debugger::Hit hit = dbg.lastHit();
    assert(hit.reason == pom2::Debugger::Reason::Breakpoint);
    assert(hit.pc == 0x0302);
    assert(cpu.getProgramCounter() == 0x0302 &&
           "the PC must still be ON the breakpoint");
    assert(cpu.getAccumulator() == 0x11 &&
           "the breakpoint's own instruction was executed — it must not be");
    assert(spent > 0 && spent < 1000 && "the run must end early, not run out");

    // The case that actually DISCRIMINATES, and the reason this one is
    // written out separately: "check before the instruction at PC" and
    // "check after the previous instruction" put the machine in the same
    // state everywhere except the very first instruction of a run. Break on
    // the ENTRY point and the difference is total — checking before stops
    // immediately with nothing executed, checking after runs the instruction
    // first and then never matches, so the breakpoint is simply missed.
    //
    // That is not a corner case: it is run-to-cursor on the current line, and
    // it is a loop re-entering its own head.
    {
        Memory mem2;
        M6502  cpu2(&mem2);
        pom2::Debugger dbg2;
        loadProgram(mem2);

        dbg2.addBreakpoint(kStart);
        cpu2.setDebugHook(&dbg2);
        cpu2.setProgramCounter(kStart);
        cpu2.setAccumulator(0x99);          // a sentinel the program overwrites

        const int spent2 = cpu2.run(1000);
        assert(dbg2.stopRequested() &&
               "a breakpoint on the entry PC was missed entirely");
        assert(cpu2.getProgramCounter() == kStart);
        assert(cpu2.getAccumulator() == 0x99 &&
               "the entry instruction ran — the check is happening too late");
        assert(spent2 == 0 && "nothing should have been executed at all");
    }

    std::printf("[ OK ] a breakpoint stops before its instruction\n");
}

// ── 3. Resuming moves ────────────────────────────────────────────────────
void testResumeMakesProgress()
{
    Memory mem;
    M6502  cpu(&mem);
    pom2::Debugger dbg;
    loadProgram(mem);

    dbg.addBreakpoint(0x0302);
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(kStart);
    cpu.run(1000);
    assert(cpu.getProgramCounter() == 0x0302);

    // Resume the way EmulationController::debugResume does.
    dbg.armResumeFrom(cpu.getProgramCounter());
    dbg.clearHit();
    assert(!dbg.stopRequested());

    cpu.run(1000);
    // The amnesty is ONE instruction: $0302 ran (A = $22), the program went
    // on, and — since it loops at $0306 and never returns to $0302 — nothing
    // stops it again. Without the amnesty this run would halt at $0302 again
    // having executed nothing, which is the "Run button does nothing" bug.
    assert(cpu.getAccumulator() == 0x33 &&
           "the resumed run did not get past the breakpoint");
    assert(cpu.getProgramCounter() == 0x0306);

    std::printf("[ OK ] resuming past a breakpoint makes progress\n");
}

// ── 4. The performance contract ──────────────────────────────────────────
void testUnarmedIsDetached()
{
    Memory mem;
    M6502  cpu(&mem);
    pom2::Debugger dbg;
    loadProgram(mem);

    // What EmulationController::syncDebugHook does: attach only while armed.
    auto sync = [&] { cpu.setDebugHook(dbg.armed() ? &dbg : nullptr); };

    sync();
    assert(cpu.getDebugHook() == nullptr &&
           "an un-armed debugger must leave the CPU's fast loop alone");

    dbg.addBreakpoint(0x0302);
    sync();
    assert(cpu.getDebugHook() == &dbg);

    dbg.clearBreakpoints();
    sync();
    assert(cpu.getDebugHook() == nullptr &&
           "clearing the last breakpoint must detach the hook again");

    // And with no hook the CPU runs the program to completion, untouched.
    cpu.setProgramCounter(kStart);
    cpu.run(1000);
    assert(cpu.getAccumulator() == 0x33);
    assert(cpu.getProgramCounter() == 0x0306);

    std::printf("[ OK ] an un-armed debugger is detached from the CPU\n");
}

// ── 5. Transients are one-shot ───────────────────────────────────────────
void testTransientFiresOnce()
{
    Memory mem;
    M6502  cpu(&mem);
    pom2::Debugger dbg;

    // $0300 LDA #$11 / $0302 JMP $0300 — comes back round to $0302 forever,
    // so a transient that failed to clear itself would fire a second time.
    mem.writeRamUnchecked(0x0300, 0xA9);
    mem.writeRamUnchecked(0x0301, 0x11);
    mem.writeRamUnchecked(0x0302, 0x4C);
    mem.writeRamUnchecked(0x0303, 0x00);
    mem.writeRamUnchecked(0x0304, 0x03);

    dbg.setTransient(0x0302, pom2::Debugger::Reason::StepOver);
    assert(dbg.armed() && dbg.hasTransient());
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(0x0300);

    cpu.run(1000);
    assert(dbg.lastHit().reason == pom2::Debugger::Reason::StepOver);
    assert(cpu.getProgramCounter() == 0x0302);
    assert(!dbg.hasTransient() && "a transient must clear itself when it fires");
    assert(!dbg.armed() && "a spent transient must leave the debugger un-armed");

    // Round the loop again: nothing should stop it now.
    dbg.armResumeFrom(cpu.getProgramCounter());
    dbg.clearHit();
    cpu.run(1000);
    assert(!dbg.stopRequested() && "the spent transient fired a second time");

    std::printf("[ OK ] a transient breakpoint fires exactly once\n");
}

// ── 6. A real breakpoint outlives a transient at the same address ────────
void testTransientDoesNotEatARealBreakpoint()
{
    pom2::Debugger dbg;
    dbg.addBreakpoint(0x0400);
    dbg.setTransient(0x0400, pom2::Debugger::Reason::RunToCursor);

    // The transient wins the stop (it is checked first) but must not take the
    // user's breakpoint with it when it clears.
    assert(dbg.onInstruction(0x0400));
    assert(dbg.lastHit().reason == pom2::Debugger::Reason::RunToCursor);
    assert(!dbg.hasTransient());
    assert(dbg.hasBreakpoint(0x0400) &&
           "run-to-cursor deleted the user's breakpoint at the same address");
    assert(dbg.armed());

    std::printf("[ OK ] a transient does not consume a real breakpoint\n");
}

// ── 7. THE watchpoint case ───────────────────────────────────────────────
void testWriteWatchStopsAndTheWriteLands()
{
    Memory mem;
    M6502  cpu(&mem);
    pom2::Debugger dbg;
    loadStoreProgram(mem);

    armWriteWatch(mem, dbg, kStoreAddr);
    assert(dbg.armed() && dbg.watchArmed());
    assert(mem.hasWriteWatch(kStoreAddr));
    assert(mem.writeWatchCount() == 1);

    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(kStoreStart);
    const int spent = cpu.run(1000);

    assert(dbg.stopRequested() && "the write watchpoint did not fire");
    const pom2::Debugger::Hit hit = dbg.lastHit();
    assert(hit.reason == pom2::Debugger::Reason::WatchWrite);
    assert(hit.addr  == kStoreAddr);
    assert(hit.value == 0x5A);
    // The instruction that WROTE, not the one the machine is now on. The CPU's
    // programCounter has already walked past the opcode and its operands by
    // the time Memory reports, so this can only be right if the debugger
    // latched the instruction PC in onInstruction().
    assert(hit.pc == kStorePc && "the hit must name the instruction that wrote");

    // The stop lands at the NEXT instruction: the access is in flight and
    // cannot be un-done, so stopping "on" the store would mean re-running it.
    assert(cpu.getProgramCounter() == kStoreNext);
    assert(cpu.getAccumulator() == 0x5A && "the LDA after the store must not have run");
    assert(spent > 0 && spent < 1000 && "the run must end early, not run out");

    // THE thing that would make this feature worse than useless: the write is
    // diverted off the fast path by clearing writable[], so a bug here
    // silently write-protects the address and corrupts the machine under the
    // debugger's nose.
    assert(mem.peekMainRam(kStoreAddr) == 0x5A && "the watched write was swallowed");

    // Disarming restores the address to an ordinary writable cell — including
    // its place on memWrite's FAST path, which is only observable by writing.
    disarmWriteWatch(mem, dbg, kStoreAddr);
    assert(!dbg.armed() && "the last watchpoint must un-arm the debugger");
    assert(mem.writeWatchCount() == 0 && !mem.hasWriteWatch(kStoreAddr));
    mem.memWrite(kStoreAddr, 0x31);
    assert(mem.peekMainRam(kStoreAddr) == 0x31 &&
           "disarming left the address write-protected");

    std::printf("[ OK ] a write watchpoint stops, names the writer, and the write lands\n");
}

// ── 8. The diversion must not invent write permission ────────────────────
void testWatchDoesNotUnprotectRom()
{
    Memory mem;
    pom2::Debugger dbg;
    mem.setWatchSink(&dbg);

    // $8000-$8FFF marked ROM BEFORE the watch: the shadowed permission has to
    // record "not writable", or arming a watch would hand the machine a
    // writable ROM.
    mem.markRomRange(0x8000, 0x8FFF);
    armWriteWatch(mem, dbg, 0x8100);
    const uint8_t was = mem.peekMainRam(0x8100);
    mem.memWrite(0x8100, static_cast<uint8_t>(was ^ 0xFF));
    assert(dbg.lastHit().reason == pom2::Debugger::Reason::WatchWrite &&
           "a watch must fire on the access even when the write is dropped");
    assert(mem.peekMainRam(0x8100) == was && "a watched ROM address became writable");
    dbg.clearHit();

    // …and marked ROM AFTER the watch is armed, which is what a profile
    // switch reloading ROMs does. The shadow has to follow.
    armWriteWatch(mem, dbg, 0x9100);
    mem.markRomRange(0x9000, 0x9FFF);
    const uint8_t was2 = mem.peekMainRam(0x9100);
    mem.memWrite(0x9100, static_cast<uint8_t>(was2 ^ 0xFF));
    assert(mem.peekMainRam(0x9100) == was2 &&
           "markRomRegion did not reach the watchpoint's shadowed permission");
    dbg.clearHit();

    // Disarming both leaves ROM as ROM.
    disarmWriteWatch(mem, dbg, 0x8100);
    disarmWriteWatch(mem, dbg, 0x9100);
    mem.memWrite(0x8100, 0x5C);
    mem.memWrite(0x9100, 0x5C);
    assert(mem.peekMainRam(0x8100) == was && mem.peekMainRam(0x9100) == was2 &&
           "disarming a watch on ROM left it writable");

    std::printf("[ OK ] the watch diversion never grants write permission\n");
}

// ── 9. A snapshot/rewind restore still restores a watched address ────────
void testRestoreIgnoresTheDiversion()
{
    Memory mem;
    pom2::Debugger dbg;
    mem.setWatchSink(&dbg);
    armWriteWatch(mem, dbg, 0x0350);

    // restoreMainRam skips non-writable cells so a snapshot cannot clobber the
    // ROM mirror. A diverted address reads as non-writable, so without the
    // shadow this byte — and only this byte — would silently survive every
    // rewind and every snapshot load for as long as the watch stayed armed.
    std::vector<uint8_t> blob(0x10000, 0);
    blob[0x0350] = 0xC7;
    blob[0x0351] = 0xC8;
    mem.restoreMainRam(blob.data(), blob.size());
    assert(mem.peekMainRam(0x0350) == 0xC7 &&
           "a watched address was skipped by a state restore");
    assert(mem.peekMainRam(0x0351) == 0xC8);

    std::printf("[ OK ] a state restore ignores the watchpoint diversion\n");
}

// A program that LOADS, at $0320:
//   $0320  LDA $0360    AD 60 03
//   $0323  LDA #$77     A9 77
//   $0325  JMP $0325    4C 25 03
constexpr uint16_t kLoadStart = 0x0320;
constexpr uint16_t kLoadNext  = 0x0323;
constexpr uint16_t kLoadAddr  = 0x0360;
const uint8_t kLoadProgram[] = {
    0xAD, 0x60, 0x03,
    0xA9, 0x77,
    0x4C, 0x25, 0x03,
};

void loadLoadProgram(Memory& mem)
{
    for (std::size_t i = 0; i < sizeof(kLoadProgram); ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(kLoadStart + i), kLoadProgram[i]);
    mem.writeRamUnchecked(kLoadAddr, 0x5A);
}

// Arm a read watch the way syncDebugHook() does — both tables, or the test
// passes while the shipped wiring is broken (same rule as armWriteWatch).
void armReadWatch(Memory& mem, pom2::Debugger& dbg, uint16_t addr)
{
    dbg.setWatchpoint(addr, pom2::Debugger::Read);
    mem.setWatchSink(&dbg);
    mem.setReadWatch(addr, true);
}

void disarmReadWatch(Memory& mem, pom2::Debugger& dbg, uint16_t addr)
{
    dbg.setWatchpoint(addr, pom2::Debugger::None);
    mem.setReadWatch(addr, false);
}

// ── 10. THE read watchpoint case ─────────────────────────────────────────
void testReadWatchStopsAndTheReadHappened()
{
    Memory mem;
    M6502  cpu(&mem);
    pom2::Debugger dbg;
    loadLoadProgram(mem);

    // Un-armed, the address is an ordinary fast-path read: nothing to see.
    assert(mem.readWatchCount() == 0 && !mem.hasReadWatch(kLoadAddr));

    armReadWatch(mem, dbg, kLoadAddr);
    assert(dbg.armed() && mem.hasReadWatch(kLoadAddr) && mem.readWatchCount() == 1);

    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(kLoadStart);
    const int spent = cpu.run(1000);

    assert(dbg.stopRequested() && "the read watchpoint did not fire");
    const pom2::Debugger::Hit hit = dbg.lastHit();
    assert(hit.reason == pom2::Debugger::Reason::WatchRead);
    assert(hit.addr  == kLoadAddr);
    assert(hit.value == 0x5A && "the hit must carry the value the bus read");
    assert(hit.pc == kLoadStart && "the hit must name the instruction that read");
    // Same stop discipline as a write: the read is done, the machine halts at
    // the next boundary, and the load itself must have landed in A.
    assert(cpu.getProgramCounter() == kLoadNext);
    assert(cpu.getAccumulator() == 0x5A && "the watched read was not performed");
    assert(spent > 0 && spent < 1000);
    dbg.clearHit();

    // A read watch on the instruction's OWN address fires on the opcode
    // fetch — that is how "who checks the ROM ID byte at $FBB3" is answered.
    disarmReadWatch(mem, dbg, kLoadAddr);
    armReadWatch(mem, dbg, kLoadStart);
    cpu.setProgramCounter(kLoadStart);
    cpu.run(1000);
    assert(dbg.stopRequested());
    assert(dbg.lastHit().reason == pom2::Debugger::Reason::WatchRead);
    assert(dbg.lastHit().addr == kLoadStart && dbg.lastHit().value == 0xAD);
    dbg.clearHit();

    // A soft-switch read ($C000+) fires too — it was already on the slow path.
    disarmReadWatch(mem, dbg, kLoadStart);
    armReadWatch(mem, dbg, 0xC000);
    mem.writeRamUnchecked(kLoadStart + 1, 0x00);   // LDA $C000
    mem.writeRamUnchecked(kLoadStart + 2, 0xC0);
    cpu.setProgramCounter(kLoadStart);
    cpu.run(1000);
    assert(dbg.stopRequested() && dbg.lastHit().addr == 0xC000);
    dbg.clearHit();

    // Disarming the last read watch drops the diversion: the table is gone
    // and a run no longer stops.
    disarmReadWatch(mem, dbg, 0xC000);
    assert(!dbg.armed() && mem.readWatchCount() == 0 && !mem.hasReadWatch(0xC000));
    cpu.setDebugHook(nullptr);
    cpu.setProgramCounter(kLoadStart);
    const int full = cpu.run(1000);
    assert(full >= 1000 && !dbg.stopRequested());

    std::printf("[ OK ] a read watchpoint stops, names the reader, and the read happened\n");
}

// ── 11. The read diversion is precise: an unwatched read does not stop ───
void testUnwatchedReadDoesNotStop()
{
    Memory mem;
    M6502  cpu(&mem);
    pom2::Debugger dbg;
    loadLoadProgram(mem);

    // Armed on the byte NEXT to the one the program reads. Every read is now
    // diverted through memReadSlow, which is exactly where a sloppy report
    // ("something was read while a watch is armed") would show up.
    armReadWatch(mem, dbg, static_cast<uint16_t>(kLoadAddr + 1));
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(kLoadStart);
    const int spent = cpu.run(1000);
    assert(!dbg.stopRequested() && "an unwatched read fired the watchpoint");
    assert(spent >= 1000 && cpu.getAccumulator() == 0x77);
    // …and the diverted read returned the right byte.
    assert(mem.memRead(kLoadAddr) == 0x5A);

    std::printf("[ OK ] an unwatched read under an armed read watch does not stop\n");
}

// ── 12. A breakpoint on an interrupt handler's first instruction ─────────
// M6502::step() used to VECTOR and then execute the handler's first
// instruction in the same call, so `run`'s debugged loop only ever offered
// the PRE-interrupt PC to onInstruction: a breakpoint on $FFFE's target could
// not fire at all (the handler was already one instruction in by the time the
// loop came round), and a watchpoint tripped by that first instruction was
// attributed to the interrupted instruction's PC. Both are pinned here.
void testBreakpointOnInterruptHandlerEntry()
{
    // Guest: the $0300 loop from loadProgram(). Handler at $0400:
    //   $0400  LDA #$EE   A9 EE      ← the breakpoint / the watched STA
    //   $0402  STA $0350  8D 50 03
    //   $0405  RTI        40
    constexpr uint16_t kHandler = 0x0400;
    auto buildMachine = [](Memory& mem) {
        loadProgram(mem);
        mem.writeRamUnchecked(0x0400, 0xA9); mem.writeRamUnchecked(0x0401, 0xEE);
        mem.writeRamUnchecked(0x0402, 0x8D); mem.writeRamUnchecked(0x0403, 0x50);
        mem.writeRamUnchecked(0x0404, 0x03);
        mem.writeRamUnchecked(0x0405, 0x40);
        // The IRQ/BRK vector lives in ROM space, where writeRamUnchecked
        // refuses to go — so page in Language-Card RAM (two odd $C08B reads
        // arm the sticky write-enable) and write $FFFE/$FFFF through it. The
        // CPU's vector fetch reads the same overlay.
        (void)mem.memRead(0xC08B);
        (void)mem.memRead(0xC08B);
        mem.memWrite(0xFFFE, 0x00);            // IRQ/BRK vector → $0400
        mem.memWrite(0xFFFF, 0x04);
    };

    {
        Memory mem;
        M6502  cpu(&mem);
        pom2::Debugger dbg;
        buildMachine(mem);

        dbg.addBreakpoint(kHandler);
        cpu.setDebugHook(&dbg);
        cpu.setProgramCounter(kStart);
        cpu.setStatusRegister(0x20);           // I clear — the IRQ is takeable
        cpu.setAccumulator(0x99);              // sentinel the handler overwrites
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT1, true);

        const int spent = cpu.run(1000);
        assert(dbg.stopRequested() &&
               "a breakpoint on the IRQ handler's first instruction never fired");
        const pom2::Debugger::Hit hit = dbg.lastHit();
        assert(hit.reason == pom2::Debugger::Reason::Breakpoint);
        assert(hit.pc == kHandler);
        assert(cpu.getProgramCounter() == kHandler &&
               "the PC must be ON the handler's first instruction");
        assert(cpu.getAccumulator() == 0x99 &&
               "the handler's first instruction ran — the stop is one late");
        // The 7 entry cycles were still charged: the interrupt happened, it
        // is only the handler that has not started.
        assert(spent >= 7 && spent < 1000);
    }

    // …and the watchpoint attribution that rides on the same fix: the STA
    // inside the handler must report the HANDLER's PC, not the PC of the
    // instruction the interrupt suspended.
    {
        Memory mem;
        M6502  cpu(&mem);
        pom2::Debugger dbg;
        buildMachine(mem);

        armWriteWatch(mem, dbg, kStoreAddr);
        cpu.setDebugHook(&dbg);
        cpu.setProgramCounter(kStart);
        cpu.setStatusRegister(0x20);
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT1, true);

        cpu.run(1000);
        assert(dbg.stopRequested());
        const pom2::Debugger::Hit hit = dbg.lastHit();
        assert(hit.reason == pom2::Debugger::Reason::WatchWrite);
        assert(hit.addr == kStoreAddr && hit.value == 0xEE);
        assert(hit.pc == 0x0402 &&
               "the write was blamed on the interrupted instruction");
        disarmWriteWatch(mem, dbg, kStoreAddr);
    }

    std::printf("[ OK ] a breakpoint on an interrupt handler's entry fires\n");
}

// ── 13. An un-hooked CPU is not perturbed by the interrupt-entry split ───
// The § 12 fix is gated on `debugHook_`; a machine nobody is watching must
// still retire the handler's first instruction in the SAME step() that took
// the interrupt, because every lazily-synced peripheral clock derives its
// phase from how `Memory::advanceCycles` is called (see M6502.cpp).
void testInterruptEntryUnchangedWithoutAHook()
{
    Memory mem;
    M6502  cpu(&mem);
    loadProgram(mem);
    mem.writeRamUnchecked(0x0400, 0xA9); mem.writeRamUnchecked(0x0401, 0xEE);
    mem.writeRamUnchecked(0x0402, 0x40);   // RTI
    (void)mem.memRead(0xC08B);             // LC RAM in, for the vector
    (void)mem.memRead(0xC08B);
    mem.memWrite(0xFFFE, 0x00);
    mem.memWrite(0xFFFF, 0x04);

    cpu.setDebugHook(nullptr);
    cpu.setProgramCounter(kStart);
    cpu.setStatusRegister(0x20);
    cpu.setAccumulator(0x99);
    cpu.setIrqLine(M6502::IRQ_SRC_SLOT1, true);

    cpu.step();
    assert(cpu.getAccumulator() == 0xEE &&
           "the un-hooked step must vector AND run the handler's first "
           "instruction, as it always has");
    assert(cpu.getProgramCounter() == 0x0402);

    std::printf("[ OK ] the un-hooked interrupt entry is byte-for-byte what it was\n");
}

// ── 14. A transient does not outlive Stop, a reset or a profile switch ───
// Step-over and run-to-cursor arm a one-shot breakpoint at an address in the
// code that was running. If the user presses Stop instead of letting it
// arrive — or resets, or switches profile, which is `applyProfile` step 11
// calling hardReset() — that address means nothing any more, but the
// transient stayed armed and halted the machine the first time the PC
// happened past it, minutes later and with no visible cause.
void testTransientIsDisarmedByStopAndReset()
{
    EmulationController ctrl;

    auto armTransient = [&](uint16_t at) {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ctrl.debugger().setTransient(at, pom2::Debugger::Reason::RunToCursor);
        ctrl.syncDebugHook();
        assert(ctrl.debugger().hasTransient());
        assert(ctrl.cpu().getDebugHook() != nullptr);
    };

    // Stop. Reached without `stateMutex` on purpose — that is the constraint
    // the fix is shaped around (setMode is also called by holders of the lock
    // and by the worker itself, and the lock is not recursive).
    armTransient(0x0300);
    ctrl.setMode(EmulationController::Mode::Stopped);
    assert(!ctrl.debugger().hasTransient() &&
           "Stop left a step-over breakpoint armed");
    assert(!ctrl.debugger().armed());

    // Hard reset (F12 — and the profile switch that runs through it).
    armTransient(0x0301);
    ctrl.hardReset();
    assert(!ctrl.debugger().hasTransient() &&
           "a hard reset left a step-over breakpoint armed");
    assert(ctrl.cpu().getDebugHook() == nullptr &&
           "nothing is armed any more — the hook must be detached again");

    // Cold boot: the RAM the transient pointed into has literally been wiped.
    armTransient(0x0302);
    ctrl.coldBoot();
    assert(!ctrl.debugger().hasTransient() &&
           "a cold boot left a step-over breakpoint armed");
    assert(ctrl.cpu().getDebugHook() == nullptr);

    // A real breakpoint is NOT collateral damage: only the transient goes.
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ctrl.debugger().addBreakpoint(0x0310);
        ctrl.debugger().setTransient(0x0311, pom2::Debugger::Reason::StepOver);
        ctrl.syncDebugHook();
    }
    ctrl.hardReset();
    assert(!ctrl.debugger().hasTransient());
    assert(ctrl.debugger().hasBreakpoint(0x0310) &&
           "the reset ate a breakpoint the user placed");
    assert(ctrl.cpu().getDebugHook() != nullptr);
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ctrl.debugger().clearBreakpoints();
        ctrl.syncDebugHook();
    }

    std::printf("[ OK ] a transient is disarmed by Stop, hard reset and cold boot\n");
}

// ── 15. bootFromSlot is coldBoot-equivalent, cassette included ───────────
// CLAUDE.md § Reset architecture calls bootFromSlot "coldBoot-equivalent
// (inlined)". The inlining dropped `tape->resetCpuSide()`, so a boot from the
// Disk II Library left the cassette's output flip-flop set and its cycle
// timebase un-rebased — `lastOutputToggleCycle` then sat in the future of a
// zeroed `currentCycle` and the next $C020 toggle recorded a wrapped pulse.
void testBootFromSlotResetsTheCassetteCpuSide()
{
    // The observable: resetCpuSide() clears `outputLevel`, and toggleOutput()
    // returns the NEW level. Toggle once (level → true, returns $80); after a
    // reset of the CPU side the next toggle must return $80 again, not $00.
    auto levelAfterBootFrom = [](void (*boot)(EmulationController&)) {
        EmulationController ctrl;
        assert(ctrl.cassette().toggleOutput() == 0x80);
        boot(ctrl);
        return ctrl.cassette().toggleOutput();
    };

    const uint8_t afterCold = levelAfterBootFrom(
        [](EmulationController& c) { c.coldBoot(); });
    const uint8_t afterBoot = levelAfterBootFrom(
        [](EmulationController& c) { (void)c.bootFromSlot(6); });
    const uint8_t afterNothing = levelAfterBootFrom(
        [](EmulationController&) {});

    assert(afterNothing == 0x00 && "the observable does not discriminate");
    assert(afterCold == 0x80);
    assert(afterBoot == 0x80 &&
           "bootFromSlot skipped the cassette CPU-side reset coldBoot does");

    std::printf("[ OK ] bootFromSlot resets the cassette CPU side like coldBoot\n");
}

// ── 16. An idle hook is detached at the next run slice (R3) ─────────────
// `M6502::step` gates its interrupt-entry split on `debugHook_ != nullptr`
// and NOT on "is anything armed", because testing a flag on that path costs
// 7.2 % (PERFORMANCE § 8.5). The split changes how an IRQ entry's 7 cycles
// reach `memory->advanceCycles` — two small advances instead of one sum —
// which moves the sub-instruction phase every lazily-synced peripheral clock
// derives from (Mockingboard/Phasor T1, the Disk II LSS, the video beam).
//
// A step-over attaches the hook; `setMode(Mode::Stopped)` drops the transient
// that armed it but deliberately does NOT re-sync (it must not take the
// lock). So after ONE step-over plus Run the machine kept a debugged CPU for
// the rest of the session. `runCpuSlice` now reconciles the two.
void testIdleHookIsDetachedOnResume()
{
    EmulationController ctrl;
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        Memory& mem = ctrl.memory();
        mem.memWrite(0x0800, 0xEA);          // NOP
        mem.memWrite(0x0801, 0x4C);          // JMP $0800
        mem.memWrite(0x0802, 0x00);
        mem.memWrite(0x0803, 0x08);
        ctrl.cpu().setProgramCounter(0x0800);
        ctrl.debugger().setTransient(0x0801, pom2::Debugger::Reason::StepOver);
        ctrl.syncDebugHook();
    }
    assert(ctrl.cpu().getDebugHook() != nullptr);

    // Stop drops the transient without re-syncing — the state the fix targets.
    ctrl.setMode(EmulationController::Mode::Stopped);
    assert(!ctrl.debugger().armed());
    assert(ctrl.cpu().getDebugHook() != nullptr &&
           "setMode is not supposed to re-sync; the case would prove nothing");

    // Resuming through the one funnel both CPU drivers use must reconcile it.
    ctrl.setMode(EmulationController::Mode::Running);
    ctrl.tickFrame();
    assert(ctrl.cpu().getDebugHook() == nullptr &&
           "an idle debug hook survived a run slice — every IRQ entry in this "
           "session is still charged as a separate advanceCycles");

    // A hook that IS armed stays attached.
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ctrl.debugger().addBreakpoint(0x0900);
        ctrl.syncDebugHook();
    }
    ctrl.tickFrame();
    assert(ctrl.cpu().getDebugHook() != nullptr &&
           "a live breakpoint lost its hook");

    std::printf("[ OK ] an idle debug hook is detached at the next run slice\n");
}

// ── 17. A time jump drops the debugger's per-timeline transients (S15) ───
// A rewind scrub or a snapshot load restores RAM and the PC. The armed resume
// amnesty, the latched hit and the current-PC shadow all name addresses in
// the timeline that was abandoned: the amnesty silently skips the first
// breakpoint at that pc on the restored machine, and the latched hit still
// reads `stopRequested()`, so the worker parks again the instant it resumes,
// blaming a breakpoint nothing hit.
void testTimeJumpClearsDebuggerTransients()
{
    EmulationController ctrl;
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        Memory& mem = ctrl.memory();
        mem.memWrite(0x0800, 0x4C);
        mem.memWrite(0x0801, 0x00);
        mem.memWrite(0x0802, 0x08);
        ctrl.cpu().setProgramCounter(0x0800);
        ctrl.debugger().addBreakpoint(0x0800);
        ctrl.syncDebugHook();
    }

    // Run until the breakpoint latches a hit.
    ctrl.setMode(EmulationController::Mode::Running);
    ctrl.tickFrame();
    assert(ctrl.debugger().stopRequested() && "no hit to clear");

    ctrl.noteTimeJump();

    assert(!ctrl.debugger().stopRequested() &&
           "a latched hit survived the time jump — the restored machine would "
           "park again on a breakpoint it never reached");
    assert(ctrl.debugger().hasBreakpoint(0x0800) &&
           "the time jump ate a breakpoint the user placed");

    std::printf("[ OK ] a time jump clears the debugger's transients, keeps "
                "the user's breakpoints\n");
}

}  // namespace

int main()
{
    testBreakpointBookkeeping();
    testBreakStopsBeforeTheInstruction();
    testResumeMakesProgress();
    testUnarmedIsDetached();
    testTransientFiresOnce();
    testTransientDoesNotEatARealBreakpoint();
    testWriteWatchStopsAndTheWriteLands();
    testWatchDoesNotUnprotectRom();
    testRestoreIgnoresTheDiversion();
    testReadWatchStopsAndTheReadHappened();
    testUnwatchedReadDoesNotStop();
    testBreakpointOnInterruptHandlerEntry();
    testInterruptEntryUnchangedWithoutAHook();
    testTransientIsDisarmedByStopAndReset();
    testIdleHookIsDetachedOnResume();
    testTimeJumpClearsDebuggerTransients();
    testBootFromSlotResetsTheCassetteCpuSide();
    std::printf("debugger: all assertions passed\n");
    return 0;
}
