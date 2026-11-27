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

// Breakpoint gates (Debugger.h § Gates) and run-to-beam — included by
// debugger_test.cpp, which is at its line ceiling. What these hold:
//
//   * "ignore N" lets exactly N qualifying passes go by, and the count is of
//     ACCESSES: an NMOS read-modify-write is one, though it writes twice.
//   * A value gate judges the byte that STAYS, not the RMW's dummy write.
//   * A beam window stops only inside it, at the cycle of the access for a
//     memory breakpoint.
//   * Run-to-beam stops at or just after the position, never before.

#include "BeamPosition.h"
#include "Block512Backing.h"
#include "DebuggerWatchList.h"
#include "HostIoEpoch.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>

static void testGateIgnoreCountOnAPcBreakpoint()
{
    using D = pom2::Debugger;
    Memory mem;
    M6502 cpu(&mem);
    D dbg;
    mem.memWrite(0x0800, 0xE8);                                   // INX
    mem.memWrite(0x0801, 0x4C); mem.memWrite(0x0802, 0x00); mem.memWrite(0x0803, 0x08);
    cpu.setProgramCounter(0x0800);
    cpu.setXRegister(0);
    dbg.addBreakpoint(0x0800);
    D::Gate gate; gate.ignore = 3;
    dbg.setBreakpointGate({0x0800, {}}, gate);
    cpu.setDebugHook(&dbg);
    cpu.run(1000);
    // Passes 1-3 go by (X = 0, 1, 2 going in); the 4th stops with X = 3.
    assert(dbg.stopRequested() && cpu.getXRegister() == 3);
    assert(dbg.breakpointGates().at({0x0800, {}}).passes == 4);
    // Removing the breakpoint forgets the gate: a new one there is plain.
    dbg.removeBreakpoint(0x0800);
    assert(dbg.breakpointGates().empty());
    std::printf("[ OK ] a breakpoint lets its ignored passes go by, and counts them\n");
}

static void testEditingAConditionInPlace()
{
    using D = pom2::Debugger;
    Memory mem;
    M6502 cpu(&mem);
    D dbg;
    mem.memWrite(0x0800, 0xE8);
    mem.memWrite(0x0801, 0x4C); mem.memWrite(0x0802, 0x00); mem.memWrite(0x0803, 0x08);
    cpu.setProgramCounter(0x0800);
    cpu.setXRegister(0);
    dbg.addBreakpoint(0x0800);
    dbg.setCondition({0x0800, {}}, true, {D::Register::X, D::Compare::Equal, 5}, cpu);
    assert(dbg.conditions().count(0x0800) == 1 && dbg.breakpointCount() == 1);
    cpu.setDebugHook(&dbg);
    cpu.run(1000);
    assert(dbg.stopRequested() && cpu.getXRegister() == 5);
    dbg.setCondition({0x0800, {}}, false, {}, cpu);
    assert(dbg.conditions().empty() && dbg.hasBreakpoint(0x0800));
    // No breakpoint there: nothing to hang a condition on.
    dbg.setCondition({0x0900, {}}, true, {D::Register::X, D::Compare::Equal, 5}, cpu);
    assert(dbg.conditions().empty() && dbg.breakpointCount() == 1);
    std::printf("[ OK ] a condition is added to and dropped from an existing breakpoint\n");
}

static void testGateOnTheValueWritten()
{
    using D = pom2::Debugger;
    Memory mem;
    M6502 cpu(&mem);
    D dbg;
    // LDA #1 / STA $0350 / LDA #2 / STA $0350 / LDA #3 / STA $0350 / JMP *
    const uint8_t program[] = {0xA9,1, 0x8D,0x50,0x03, 0xA9,2, 0x8D,0x50,0x03,
                               0xA9,3, 0x8D,0x50,0x03, 0x4C,0x0F,0x08};
    for (std::size_t i = 0; i < sizeof program; ++i)
        mem.memWrite(static_cast<uint16_t>(0x0800 + i), program[i]);
    armWriteWatch(mem, dbg, 0x0350);
    D::Gate gate; gate.valueOn = true; gate.value = 2;
    dbg.setWatchGate({0x0350, {}}, gate);
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(0x0800);
    (void)cpu.run(1000);
    assert(dbg.stopRequested());
    assert(dbg.lastHit().value == 2 && dbg.lastHit().pc == 0x0807);
    assert(dbg.watchGates().at({0x0350, {}}).passes == 1 && "only the matching write is a pass");
    cpu.setDebugHook(nullptr);
    disarmWriteWatch(mem, dbg, 0x0350);
    assert(dbg.watchGates().empty());
    std::printf("[ OK ] a memory breakpoint gated on the value stops on that write only\n");
}

// An NMOS INC writes the old value back, then the new one.
static void testGateJudgesTheRmwsLastWrite()
{
    using D = pom2::Debugger;
    for (int wanted : {5, 6}) {
        Memory mem;
        M6502 cpu(&mem);
        cpu.setCpuMode(M6502::CpuMode::NMOS);
        D dbg;
        mem.memWrite(0x0010, 0x05);
        mem.memWrite(0x0300, 0xE6); mem.memWrite(0x0301, 0x10);     // INC $10
        mem.memWrite(0x0302, 0x4C); mem.memWrite(0x0303, 0x02); mem.memWrite(0x0304, 0x03);
        armWriteWatch(mem, dbg, 0x0010);
        D::Gate gate; gate.valueOn = true; gate.value = static_cast<uint8_t>(wanted);
        dbg.setWatchGate({0x0010, {}}, gate);
        cpu.setDebugHook(&dbg);
        cpu.setProgramCounter(0x0300);
        (void)cpu.run(40);
        if (wanted == 5) assert(!dbg.stopRequested() && "stopped on the dummy write");
        else             assert(dbg.stopRequested() && dbg.lastHit().value == 6);
        cpu.setDebugHook(nullptr);
        disarmWriteWatch(mem, dbg, 0x0010);
    }
    // And one INC is ONE pass: with one pass ignored, the second INC stops.
    Memory mem;
    M6502 cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);
    D dbg;
    mem.memWrite(0x0010, 0x05);
    for (uint16_t a = 0x0300; a < 0x0308; a += 2) { mem.memWrite(a, 0xE6); mem.memWrite(a + 1, 0x10); }
    armWriteWatch(mem, dbg, 0x0010);
    D::Gate gate; gate.ignore = 1;
    dbg.setWatchGate({0x0010, {}}, gate);
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(0x0300);
    (void)cpu.run(100);
    assert(dbg.stopRequested());
    assert(dbg.lastHit().pc == 0x0302 && dbg.lastHit().value == 0x07);
    assert(dbg.watchGates().at({0x0010, {}}).passes == 2);
    cpu.setDebugHook(nullptr);
    disarmWriteWatch(mem, dbg, 0x0010);
    std::printf("[ OK ] a gate judges an RMW's last write, and counts it once\n");
}

static void testBeamWindowArithmetic()
{
    using D = pom2::Debugger;
    D::Gate g; g.beam = true; g.vMin = 100; g.vMax = 110; g.hMin = 25; g.hMax = 64;
    assert(D::beamInside(g, 100, 25) && D::beamInside(g, 110, 64));
    assert(!D::beamInside(g, 99, 30) && !D::beamInside(g, 105, 24));
    g.vMin = 300; g.vMax = 10; g.hMin = 0; g.hMax = 64;      // wraps through line 0
    assert(D::beamInside(g, 305, 0) && D::beamInside(g, 5, 0) && !D::beamInside(g, 150, 0));
    std::printf("[ OK ] a beam window is inclusive, and wraps when min > max\n");
}

static void testBeamGateAndRunToBeam()
{
    using D = pom2::Debugger;
    EmulationController ctrl;
    VideoStandard standard;
    {
        auto st = ctrl.lockState();
        Memory& m = st.memory();
        // $0800: STA $0350 / JMP $0800 — a write every 7 cycles.
        const uint8_t program[] = {0x8D,0x50,0x03, 0x4C,0x00,0x08};
        for (std::size_t i = 0; i < sizeof program; ++i)
            m.memWrite(static_cast<uint16_t>(0x0800 + i), program[i]);
        st.cpu().setProgramCounter(0x0800);
        standard = m.videoStandard();
        ctrl.debugger().setWatchpoint(0x0350, D::Write);
        D::Gate gate; gate.beam = true; gate.vMin = gate.vMax = 100; gate.hMin = 25; gate.hMax = 64;
        ctrl.debugger().setWatchGate({0x0350, {}}, gate);
        ctrl.syncDebugHook();
    }
    ctrl.setMode(EmulationController::Mode::Running);
    for (int i = 0; i < 4 && !ctrl.debugger().lastHit().valid(); ++i) ctrl.tickFrame();
    D::Hit hit = ctrl.debugger().lastHit();
    assert(hit.reason == D::Reason::WatchWrite);
    pom2::BeamPosition at = pom2::beamPositionAt(hit.videoCycle, standard);
    assert(at.line == 100 && at.h >= 25 && "the write stopped outside its beam window");
    assert(at.h < 25 + 7 && "an earlier write inside the window was missed");

    // Run to V 50, H 30: the first instruction boundary at or after it.
    {
        auto st = ctrl.lockState();
        ctrl.debugger().clearWatchpoints();
        ctrl.syncDebugHook();
    }
    ctrl.debugRunToBeam(50, 30);
    for (int i = 0; i < 4 && ctrl.getMode() == EmulationController::Mode::Running; ++i) ctrl.tickFrame();
    hit = ctrl.debugger().lastHit();
    assert(hit.reason == D::Reason::RunToBeam);
    at = pom2::beamPositionAt(hit.videoCycle, standard);
    assert(at.line == 50 && at.h >= 30 && at.h < 30 + 7);
    {
        auto st = ctrl.lockState();
        assert(!ctrl.debugger().hasTransient() && "run-to-beam must be one-shot");
    }
    std::printf("[ OK ] a beam window gates a memory breakpoint; run-to-beam stops on the position\n");
}

// The Watch window's list round-trips through its one settings value, and a
// hand-edited value costs only the items that are wrong.
static void testWatchListText()
{
    using W = pom2::WatchEntry;
    pom2::DebuggerWatchList list;
    pom2::DebugBank aux; aux.kind = pom2::DebugBank::Aux;
    assert(list.add({{0x0010, {}}, W::Format::Byte}));
    assert(list.add({{0x00FE, {}}, W::Format::Word}));
    assert(list.add({{0x0800, aux}, W::Format::Binary}));
    assert(!list.add({{0x0010, {}}, W::Format::Byte}) && "a duplicate was added");
    assert(list.add({{0x0010, {}}, W::Format::Word}) && "same address, another format, is another row");
    const std::string text = list.text();
    assert(text == "*:$0010/b,*:$00FE/w,aux0:$0800/n,*:$0010/w");
    pom2::DebuggerWatchList back;
    back.parse(text);
    assert(back.entries() == list.entries());
    back.parse("*:$0010/b,garbage,main:$C054/b,*:$12/b,*:$0010/q,");
    assert(back.entries().size() == 2 && back.entries()[1].at.addr == 0xC054);
    back.parse("");
    assert(back.empty());
    assert(pom2::DebuggerWatchList::value(W::Format::Byte, 0x5A, 0xC6) == "$5A");
    assert(pom2::DebuggerWatchList::value(W::Format::Word, 0x00, 0xC6) == "$C600");
    assert(pom2::DebuggerWatchList::value(W::Format::Binary, 0xA5, 0) == "%10100101");
    for (unsigned i = 0; i < 200; ++i) list.add({{static_cast<uint16_t>(0x1000 + i), {}}, W::Format::Byte});
    assert(list.entries().size() == pom2::DebuggerWatchList::kMax);
    std::printf("[ OK ] the watch list round-trips through its text, and survives a bad item\n");
}

// Step Back and Run Back (EmulationController_TimeTravel.cpp): the rewind
// ring's previous frame, re-executed to the instruction before. The loop:
//   $0800 INC $10 (5 cycles)   $0802 INX (2)   $0803 STA $0350 (4)   $0806 JMP $0800 (3)
static void testStepBackAndRunBack()
{
    using D = pom2::Debugger;
    using Mode = EmulationController::Mode;
    {
        EmulationController off;        // the rewind is not recording: no way back
        const char* why = nullptr;
        auto st = off.lockState();
        assert(!off.debugCanStepBack(&why) && why && *why);
    }
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        const uint8_t program[] = {0xE6,0x10, 0xE8, 0x8D,0x50,0x03, 0x4C,0x00,0x08};
        for (std::size_t i = 0; i < sizeof program; ++i)
            st.memory().memWrite(static_cast<uint16_t>(0x0800 + i), program[i]);
        st.cpu().setProgramCounter(0x0800);
    }
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 6; ++i) ctrl.tickFrame();
    {
        auto st = ctrl.lockState();
        ctrl.debugger().addBreakpoint(0x0802);
        ctrl.syncDebugHook();
    }
    ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped);
    struct At { uint16_t pc; uint8_t x, counter; uint64_t cycle; };
    const auto at = [&] {
        auto st = ctrl.lockState();
        return At{st.cpu().getProgramCounter(), st.cpu().getXRegister(),
                  st.memory().peekCpuView(0x0010), st.memory().getCycleCounter()};
    };
    const At stop = at();
    assert(stop.pc == 0x0802);
    { auto st = ctrl.lockState(); assert(ctrl.debugCanStepBack()); }

    // Back over INC $10: the byte is what it was, five cycles earlier.
    assert(ctrl.debugStepBack());
    At now = at();
    assert(now.pc == 0x0800 && now.cycle == stop.cycle - 5);
    assert(now.counter == static_cast<uint8_t>(stop.counter - 1) && now.x == stop.x);
    assert(ctrl.debugger().lastHit().reason == D::Reason::StepBack);
    assert(ctrl.getMode() == Mode::Stopped);
    // ...over the JMP, the STA, and the INX of the turn before.
    assert(ctrl.debugStepBack() && at().pc == 0x0806 && at().cycle == stop.cycle - 8);
    assert(ctrl.debugStepBack() && at().pc == 0x0803 && at().x == stop.x);
    assert(ctrl.debugStepBack());
    now = at();
    assert(now.pc == 0x0802 && now.x == static_cast<uint8_t>(stop.x - 1));
    assert(now.cycle == stop.cycle - 14);

    // Run Back: the breakpoint at $0802, one turn of the loop earlier.
    assert(ctrl.debugRunBack());
    now = at();
    assert(ctrl.debugger().lastHit().reason == D::Reason::Breakpoint);
    assert(now.pc == 0x0802 && now.cycle == stop.cycle - 28);
    assert(now.x == static_cast<uint8_t>(stop.x - 2));

    // Run Back to a memory breakpoint with a value: the INC that wrote it.
    // (The NEXT INC writes that value back first, on an NMOS part — the
    // dummy cycle: the replay must not take that for the write.)
    const uint8_t wanted = static_cast<uint8_t>(now.counter - 3);
    {
        auto st = ctrl.lockState();
        ctrl.debugger().clearBreakpoints();
        ctrl.debugger().setWatchpoint(0x0010, D::Write);
        D::Gate gate; gate.valueOn = true; gate.value = wanted;
        ctrl.debugger().setWatchGate({0x0010, {}}, gate);
        ctrl.syncDebugHook();
    }
    assert(ctrl.debugRunBack());
    now = at();
    D::Hit hit = ctrl.debugger().lastHit();
    assert(hit.reason == D::Reason::WatchWrite && hit.addr == 0x0010 && hit.value == wanted);
    assert(hit.pc == 0x0800 && now.pc == 0x0802 && now.counter == wanted);
    assert(ctrl.debugger().watchGates().at({0x0010, {}}).passes == 0 && "a replay counted passes");

    // No breakpoint left: the oldest point of the history, and no way back.
    {
        auto st = ctrl.lockState();
        ctrl.debugger().clearWatchpoints();
        ctrl.syncDebugHook();
    }
    uint64_t oldest = 0;
    { auto st = ctrl.lockState(); oldest = ctrl.rewind().oldestCycle(); }
    // Out of time after one frame: it lands there and says so; again, and
    // it goes on; with time enough, to the oldest point.
    ctrl.setRunBackBudget(std::chrono::milliseconds(0));
    const uint64_t beforePause = at().cycle;
    assert(ctrl.debugRunBack());
    assert(ctrl.debugger().lastHit().reason == D::Reason::RunBackPaused);
    const uint64_t paused = at().cycle;
    assert(paused < beforePause && paused > oldest && "a paused Run Back lands part of the way");
    assert(ctrl.debugRunBack() && at().cycle < paused);
    ctrl.setRunBackBudget(std::chrono::hours(1));
    assert(ctrl.debugRunBack());
    assert(ctrl.debugger().lastHit().reason == D::Reason::RunBackStart);
    assert(at().cycle == oldest);
    {
        const char* why = nullptr;
        auto st = ctrl.lockState();
        assert(!ctrl.debugCanStepBack(&why) && why);
    }
    assert(!ctrl.debugStepBack() && !ctrl.debugRunBack());
    // Running again from there records again, and the way back reopens.
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 3; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    { auto st = ctrl.lockState(); assert(ctrl.debugCanStepBack()); }
    std::printf("[ OK ] Step Back and Run Back replay the rewind ring to the instruction\n");
}

// The same loop, harder: forward then back must be the identity on the whole
// machine the CPU sees; a long walk back crosses rewind frames and must keep
// the loop's invariant on the way; and the worker thread, when there is one,
// must be parked under it.
static void loadBackLoop(EmulationController& ctrl)
{
    auto st = ctrl.lockState();
    const uint8_t program[] = {0xE6,0x10, 0xE8, 0x8D,0x50,0x03, 0x4C,0x00,0x08};
    for (std::size_t i = 0; i < sizeof program; ++i)
        st.memory().memWrite(static_cast<uint16_t>(0x0800 + i), program[i]);
    st.cpu().setProgramCounter(0x0800);
}

static void testStepBackIsTheInverseOfStep()
{
    using Mode = EmulationController::Mode;
    EmulationController ctrl;
    loadBackLoop(ctrl);
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 8; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    struct Machine { std::vector<uint8_t> ram; uint16_t pc; uint8_t a, x, y, p, sp; uint64_t cycle;
        bool operator==(const Machine& o) const {
            return ram == o.ram && pc == o.pc && a == o.a && x == o.x && y == o.y && p == o.p &&
                   sp == o.sp && cycle == o.cycle; } };
    const auto machine = [&] {
        auto st = ctrl.lockState();
        Machine m;
        m.ram.assign(0x10000, 0);
        st.memory().snapshotCpuView(m.ram.data());
        m.pc = st.cpu().getProgramCounter(); m.a = st.cpu().getAccumulator();
        m.x = st.cpu().getXRegister(); m.y = st.cpu().getYRegister();
        m.p = st.cpu().getStatusRegister(); m.sp = st.cpu().getStackPointer();
        m.cycle = st.memory().getCycleCounter();
        return m;
    };
    const Machine before = machine();
    ctrl.requestStep(301);
    ctrl.tickFrame();
    assert(machine().cycle > before.cycle);
    for (int i = 0; i < 301; ++i) assert(ctrl.debugStepBack());
    assert(machine() == before && "301 steps forward then 301 back is not where it started");

    // A long walk back, across several rewind frames (a frame is ~5800 of
    // these instructions). X and $10 both count turns of the loop: their
    // difference is constant wherever the machine stands between two turns.
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 4; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    const auto delta = [](const Machine& m) { return static_cast<uint8_t>(m.x - m.ram[0x10]); };
    Machine prev = machine();
    while (prev.pc != 0x0800) { assert(ctrl.debugStepBack()); prev = machine(); }
    const uint8_t constant = delta(prev);
    const uint64_t start = prev.cycle;
    std::size_t framesBefore = 0;
    { auto st = ctrl.lockState(); framesBefore = ctrl.rewind().size(); }
    for (int i = 0; i < 6000; ++i) {     // 21000 cycles: more than a frame
        assert(ctrl.debugStepBack());
        const Machine m = machine();
        const uint64_t went = prev.cycle - m.cycle;
        // Going back over: JMP (3) to $0806, STA (4) to $0803, INX (2) to $0802, INC (5) to $0800.
        const uint16_t expectPc = prev.pc == 0x0800 ? 0x0806 : prev.pc == 0x0806 ? 0x0803
                                : prev.pc == 0x0803 ? 0x0802 : 0x0800;
        const uint64_t expectCycles = expectPc == 0x0806 ? 3 : expectPc == 0x0803 ? 4
                                    : expectPc == 0x0802 ? 2 : 5;
        assert(m.pc == expectPc && went == expectCycles && "a step back lost its place");
        if (m.pc == 0x0800) assert(delta(m) == constant);
        prev = m;
    }
    assert(start - prev.cycle == 6000 / 4 * 14);
    { auto st = ctrl.lockState(); assert(ctrl.rewind().size() < framesBefore && "no frame was crossed"); }
    std::printf("[ OK ] Step Back is the inverse of Step, and holds across rewind frames\n");
}

static void testStepBackUnderTheWorkerThread()
{
    using D = pom2::Debugger;
    using Mode = EmulationController::Mode;
    EmulationController ctrl;
    loadBackLoop(ctrl);
    ctrl.rewind().setEnabled(true);
    ctrl.start();
    const auto waitFor = [&](auto&& done) {
        for (int i = 0; i < 4000 && !done(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return done();
    };
    assert(waitFor([&] { auto st = ctrl.lockState(); return ctrl.rewind().size() >= 6; }));
    {
        auto st = ctrl.lockState();
        ctrl.debugger().addBreakpoint(0x0802);
        ctrl.syncDebugHook();
    }
    assert(waitFor([&] { return ctrl.getMode() == Mode::Stopped; }));
    (void)ctrl.debugStepBack();                 // whatever it answers, it must not deadlock
    ctrl.setRunBackBudget(std::chrono::hours(1));
    uint64_t cycle = 0;
    { auto st = ctrl.lockState(); cycle = st.memory().getCycleCounter(); }
    assert(ctrl.debugRunBack());
    {
        auto st = ctrl.lockState();
        assert(ctrl.debugger().lastHit().reason == D::Reason::Breakpoint);
        assert(st.cpu().getProgramCounter() == 0x0802 && st.memory().getCycleCounter() < cycle);
        cycle = st.memory().getCycleCounter();
    }
    // The machine stays where Run Back put it: the worker is parked.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        auto st = ctrl.lockState();
        assert(st.memory().getCycleCounter() == cycle && "the worker ran under a stopped machine");
        ctrl.debugger().clearBreakpoints();
        ctrl.syncDebugHook();
    }
    // And it runs on from there when asked to.
    { auto st = ctrl.lockState(); ctrl.debugResume(); }
    ctrl.setMode(Mode::Running);
    assert(waitFor([&] { auto st = ctrl.lockState(); return st.memory().getCycleCounter() > cycle + 50000; }));
    ctrl.stop();
    std::printf("[ OK ] Run Back lands, and Step Back does not deadlock, under the worker thread\n");
}

// A read-and-write breakpoint on an RMW's operand: the read is an access of
// its own. The dummy write's pass is taken back; the read's is not.
static void testReadWriteGateCountsAnRmwAsTwo()
{
    using D = pom2::Debugger;
    Memory mem;
    M6502 cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);
    D dbg;
    mem.memWrite(0x0010, 0x05);
    mem.memWrite(0x0300, 0xE6); mem.memWrite(0x0301, 0x10);     // INC $10
    mem.memWrite(0x0302, 0x4C); mem.memWrite(0x0303, 0x02); mem.memWrite(0x0304, 0x03);
    dbg.setWatchpoint(0x0010, D::ReadWrite);
    mem.setWatchSink(&dbg);
    mem.setWriteWatch(0x0010, true);
    mem.setReadWatch(0x0010, true);
    D::Gate gate; gate.ignore = 1000;
    dbg.setWatchGate({0x0010, {}}, gate);
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(0x0300);
    (void)cpu.run(40);
    assert(!dbg.stopRequested());
    assert(dbg.watchGates().at({0x0010, {}}).passes == 2 && "the read and the write: two accesses");
    cpu.setDebugHook(nullptr);
    dbg.setWatchpoint(0x0010, D::None);
    mem.setWriteWatch(0x0010, false);
    mem.setReadWatch(0x0010, false);
    std::printf("[ OK ] a read-write breakpoint counts an RMW's read and its write, once each\n");
}

// Interrupts. The IRQ line is held: the handler ($0400 INC $11 / $0402 RTI)
// is re-entered as soon as it returns, the interrupted instruction at $0800
// being offered each time and never run. An entry is 7 cycles, INC 5, RTI 6.
static void testGoingBackThroughInterrupts()
{
    using D = pom2::Debugger;
    using Mode = EmulationController::Mode;
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        Memory& m = st.memory();
        const uint8_t main[] = {0xE6,0x10, 0x4C,0x00,0x08};
        for (std::size_t i = 0; i < sizeof main; ++i) m.memWrite(static_cast<uint16_t>(0x0800 + i), main[i]);
        const uint8_t handler[] = {0xE6,0x11, 0x40};
        for (std::size_t i = 0; i < sizeof handler; ++i) m.memWrite(static_cast<uint16_t>(0x0400 + i), handler[i]);
        (void)m.memRead(0xC08B); (void)m.memRead(0xC08B);        // LC RAM, for the vector
        m.memWrite(0xFFFE, 0x00); m.memWrite(0xFFFF, 0x04);
        st.cpu().setProgramCounter(0x0800);
        st.cpu().setStatusRegister(0x20);
        st.cpu().setIrqLine(M6502::IRQ_SRC_SLOT1, true);
        // A breakpoint that lets its passes go by, on the instruction the
        // interrupt keeps taking the place of: offered, never run.
        ctrl.debugger().addBreakpoint(0x0800);
        D::Gate gate; gate.ignore = 1u << 30;
        ctrl.debugger().setBreakpointGate({0x0800, {}}, gate);
        ctrl.syncDebugHook();
    }
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 6; ++i) ctrl.tickFrame();
    uint64_t ranAtMain = 0;
    {
        auto st = ctrl.lockState();
        ranAtMain = st.memory().peekCpuView(0x0010);     // times $0800 really ran
        const uint64_t passes = ctrl.debugger().breakpointGates().at({0x0800, {}}).passes;
        assert(passes <= ranAtMain + 1 && "an instruction offered and not run was counted as a pass");
        ctrl.debugger().clearBreakpoints();
        ctrl.debugger().addBreakpoint(0x0400);                 // the handler's first instruction
        ctrl.syncDebugHook();
    }
    ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped);
    const auto where = [&] {
        auto st = ctrl.lockState();
        return std::make_pair(st.cpu().getProgramCounter(), st.memory().getCycleCounter());
    };
    const auto stop = where();
    assert(stop.first == 0x0400);
    // Run Back: the same breakpoint, one interrupt earlier. A replay that
    // took the entry and the handler's first instruction as one step never
    // stood on $0400 and ran past every one of them.
    ctrl.setRunBackBudget(std::chrono::hours(1));
    assert(ctrl.debugRunBack());
    assert(ctrl.debugger().lastHit().reason == D::Reason::Breakpoint);
    assert(where().first == 0x0400 && where().second == stop.second - 18);
    // Step Back from the handler's first instruction undoes the ENTRY...
    assert(ctrl.debugStepBack());
    assert(where().first == 0x0800 && where().second == stop.second - 25);
    // ...and from there, the RTI before it.
    assert(ctrl.debugStepBack());
    assert(where().first == 0x0402 && where().second == stop.second - 31);
    std::printf("[ OK ] going back stops on, and steps over, an interrupt entry\n");
}

// No replay crosses a change of the accelerator's speed: the worker samples
// the speed every 4096 cycles, a replay keeps its frame's, and the beam would
// end up elsewhere. The history of both verbs starts at the last change.
static void testNoGoingBackAcrossAnAcceleratorSpeedChange()
{
    using D = pom2::Debugger;
    using Mode = EmulationController::Mode;
    struct Accelerator final : SlotPeripheral {
        double speed = 1.0;
        std::string_view name() const override { return "speed test"; }
        double cpuSpeedMultiplier() const override { return speed; }
    };
    EmulationController ctrl;
    auto card = std::make_unique<Accelerator>();
    Accelerator* accelerator = card.get();
    loadBackLoop(ctrl);
    { auto st = ctrl.lockState(); st.memory().slotBus().plug(3, std::move(card)); }
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 6; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    uint64_t changeAt = 0;
    {
        auto st = ctrl.lockState();
        assert(ctrl.debugCanStepBack() && "a constant speed must not close the way back");
        changeAt = st.memory().getCycleCounter();
        accelerator->speed = 2.0;                 // a slow window closing
    }
    ctrl.setMode(Mode::Running);
    ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    for (int i = 0; i < 3; ++i) { ctrl.setMode(Mode::Running); ctrl.tickFrame(); ctrl.setMode(Mode::Stopped); }
    // Frames exist on this side of the change: back is allowed, up to it.
    { auto st = ctrl.lockState(); assert(ctrl.debugCanStepBack()); }
    ctrl.setRunBackBudget(std::chrono::hours(1));  // not the wall clock's business here
    assert(ctrl.debugRunBack());                  // no breakpoint: the oldest USABLE point
    assert(ctrl.debugger().lastHit().reason == D::Reason::RunBackStart);
    {
        const char* why = nullptr;
        auto st = ctrl.lockState();
        assert(st.memory().getCycleCounter() >= changeAt && "Run Back crossed a speed change");
        assert(ctrl.rewind().oldestCycle() < changeAt && "the ring should still hold older frames");
        assert(!ctrl.debugCanStepBack(&why) && why && std::string(why).find("accelerator") != std::string::npos);
    }
    assert(!ctrl.debugStepBack() && !ctrl.debugRunBack());
    std::printf("[ OK ] no Step Back or Run Back across an accelerator speed change\n");
}

// A 65C02 read-modify-write reads its operand twice (the dummy cycle is a
// read there, a write on an NMOS part): one read, for a read breakpoint.
static void testCmosRmwIsOneRead()
{
    using D = pom2::Debugger;
    Memory mem;
    M6502 cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    D dbg;
    mem.memWrite(0x0010, 0x05);
    mem.memWrite(0x0300, 0xE6); mem.memWrite(0x0301, 0x10);     // INC $10
    mem.memWrite(0x0302, 0x4C); mem.memWrite(0x0303, 0x02); mem.memWrite(0x0304, 0x03);
    dbg.setWatchpoint(0x0010, D::Read);
    mem.setWatchSink(&dbg);
    mem.setReadWatch(0x0010, true);
    D::Gate gate; gate.ignore = 1000;
    dbg.setWatchGate({0x0010, {}}, gate);
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(0x0300);
    (void)cpu.run(40);
    assert(dbg.watchGates().at({0x0010, {}}).passes == 1 && "the 65C02's dummy read counted a pass");
    cpu.setDebugHook(nullptr);
    dbg.setWatchpoint(0x0010, D::None);
    mem.setReadWatch(0x0010, false);
    std::printf("[ OK ] a 65C02 read-modify-write is one read for a read breakpoint\n");
}

// The debugger's own writes (a Memory viewer edit, the assembler) are not
// the guest's: no pass, no stop, and a pending stop is neither rewritten nor
// erased — even by an edit of the very byte it stopped on, failing its gate.
static void testOwnWritesAreNotAccesses()
{
    using D = pom2::Debugger;
    Memory mem;
    M6502 cpu(&mem);
    D dbg;
    loadStoreProgram(mem);                      // LDA #$5A / STA $0350
    armWriteWatch(mem, dbg, kStoreAddr);
    D::Gate gate; gate.valueOn = true; gate.value = 0x5A;
    dbg.setWatchGate({kStoreAddr, {}}, gate);
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(kStoreStart);
    (void)cpu.run(1000);
    assert(dbg.stopRequested() && dbg.lastHit().value == 0x5A);
    const uint8_t edited = 0x00;
    dbg.patchMemory(mem, kStoreAddr, &edited, 1);
    assert(dbg.stopRequested() && dbg.lastHit().value == 0x5A && "an edit rewrote or erased the stop");
    assert(dbg.watchGates().at({kStoreAddr, {}}).passes == 1 && "an edit counted a pass");
    assert(mem.peekMainRam(kStoreAddr) == 0x00 && "the edit did not land");
    cpu.setDebugHook(nullptr);
    disarmWriteWatch(mem, dbg, kStoreAddr);
    std::printf("[ OK ] the debugger's own writes are not accesses\n");
}

// A pass is an ARRIVAL, however the machine got there: stepping through a
// loop by hand counts the passes of the breakpoint in it; the step that
// leaves the stop which already counted one does not count it again.
static void testSteppedPassesCount()
{
    using D = pom2::Debugger;
    using Mode = EmulationController::Mode;
    EmulationController ctrl;
    loadBackLoop(ctrl);                         // INC $10 / INX / STA / JMP, 4 instructions a turn
    {
        auto st = ctrl.lockState();
        ctrl.debugger().addBreakpoint(0x0802);
        D::Gate gate; gate.ignore = 1u << 30;
        ctrl.debugger().setBreakpointGate({0x0802, {}}, gate);
        ctrl.syncDebugHook();
    }
    const auto passes = [&] {
        auto st = ctrl.lockState();
        const auto& gates = ctrl.debugger().breakpointGates();
        const auto found = gates.find({0x0802, {}});
        return found == gates.end() ? uint64_t(0) : found->second.passes;
    };
    for (int i = 0; i < 12; ++i) { ctrl.requestStep(1); ctrl.tickFrame(); }   // three turns
    assert(ctrl.getMode() == Mode::Stopped);
    assert(passes() == 3 && "stepping past a breakpoint is a pass of it");

    // STOPPED on it (the stop counts the pass), a step away is not a second
    // arrival — whichever Step it is: the Debugger's goes through
    // debugResume, the toolbar's and the CLI's go straight to requestStep.
    {
        auto st = ctrl.lockState();
        D::Gate plain;
        ctrl.debugger().setBreakpointGate({0x0802, {}}, plain);   // stops now; count from 0
        ctrl.syncDebugHook();
    }
    ctrl.setMode(Mode::Running);
    ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped && passes() == 1);
    ctrl.requestStep(1);                        // the toolbar's Step
    ctrl.tickFrame();
    assert(passes() == 1 && "a step off the breakpoint it is stopped on counted a second pass");
    ctrl.setMode(Mode::Running);
    ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped && passes() == 2);
    ctrl.debugStepInstruction();                // the Debugger's Step
    ctrl.tickFrame();
    assert(passes() == 2);
    std::printf("[ OK ] a breakpoint's passes count when stepped through by hand\n");
}

// A reset is not replayed: the history of Step Back starts after it.
static void testNoGoingBackAcrossAReset()
{
    using Mode = EmulationController::Mode;
    EmulationController ctrl;
    loadBackLoop(ctrl);
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 6; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    { auto st = ctrl.lockState(); assert(ctrl.debugCanStepBack()); }
    ctrl.softReset();
    ctrl.setMode(Mode::Stopped);
    {
        const char* why = nullptr;
        auto st = ctrl.lockState();
        assert(!ctrl.rewind().empty() && "the ring is expected to keep its frames across a soft reset");
        assert(!ctrl.debugCanStepBack(&why) && why && std::string(why).find("reset") != std::string::npos);
    }
    assert(!ctrl.debugStepBack() && !ctrl.debugRunBack());
    std::printf("[ OK ] no Step Back or Run Back across a reset\n");
}

// A replay that writes to a medium a rewind cannot undo is given up, and the
// machine is put back EXACTLY as it stood — from the frame captured on the
// way in, not from a second replay that would diverge the same way.
static void testAbandonedReplayRestoresThePresent()
{
    using Mode = EmulationController::Mode;
    struct WritesAMedium final : SlotPeripheral {
        bool armed = false;
        std::string_view name() const override { return "media write test"; }
        void deviceSelectWrite(uint8_t, uint8_t) override { if (armed) pom2::noteMediaWrite(); }
    };
    EmulationController ctrl;
    auto card = std::make_unique<WritesAMedium>();
    WritesAMedium* medium = card.get();
    {
        auto st = ctrl.lockState();
        // $0800 INC $10 / STA $C0B0 (slot 3's device select) / JMP $0800
        const uint8_t program[] = {0xE6,0x10, 0x8D,0xB0,0xC0, 0x4C,0x00,0x08};
        for (std::size_t i = 0; i < sizeof program; ++i)
            st.memory().memWrite(static_cast<uint16_t>(0x0800 + i), program[i]);
        st.cpu().setProgramCounter(0x0800);
        st.memory().slotBus().plug(3, std::move(card));
    }
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 6; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    {
        auto st = ctrl.lockState();
        ctrl.debugger().addBreakpoint(0x0802);  // a stop the debugger is SHOWING, off a frame's stamp
        pom2::Debugger::Gate gate; gate.ignore = 50;    // ...and fifty turns (fifty writes) past it
        ctrl.debugger().setBreakpointGate({0x0802, {}}, gate);
        ctrl.syncDebugHook();
    }
    ctrl.setMode(Mode::Running);
    ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped);
    struct Machine { std::vector<uint8_t> ram; uint16_t pc; uint8_t a, x, sp; uint64_t cycle;
        bool operator==(const Machine& o) const {
            return ram == o.ram && pc == o.pc && a == o.a && x == o.x && sp == o.sp && cycle == o.cycle; } };
    const auto machine = [&] {
        auto st = ctrl.lockState();
        Machine m;
        m.ram.assign(0x10000, 0);
        st.memory().snapshotCpuView(m.ram.data());
        m.pc = st.cpu().getProgramCounter(); m.a = st.cpu().getAccumulator();
        m.x = st.cpu().getXRegister(); m.sp = st.cpu().getStackPointer();
        m.cycle = st.memory().getCycleCounter();
        return m;
    };
    const Machine before = machine();
    { auto st = ctrl.lockState(); assert(ctrl.debugCanStepBack()); }
    medium->armed = true;                       // from now on the replayed code "saves a file"
    assert(!ctrl.debugStepBack() && "a replay that wrote to a medium was accepted");
    medium->armed = false;
    assert(machine() == before && "the machine is not back where it stood");
    // ...and the debugger with it: the stop it was showing is still there,
    // so the Run that follows gets its amnesty instead of stopping again on
    // the same breakpoint having executed nothing.
    assert(ctrl.debugger().lastHit().reason == pom2::Debugger::Reason::Breakpoint &&
           ctrl.debugger().lastHit().pc == 0x0802);
    {
        auto st = ctrl.lockState();
        assert(ctrl.rewind().empty() && "the history must end at a write a rewind cannot undo");
        assert(!ctrl.debugCanStepBack());
    }
    std::printf("[ OK ] an abandoned replay puts the machine back exactly, and ends the history\n");
}

// Every soft-switch throw names the instruction that made it (the Screen
// Inspector's list): three-byte absolute forms, and a two-byte indirect.
static void testVideoEventsNameTheirInstruction()
{
    using Mode = EmulationController::Mode;
    using K = Memory::VideoEventKind;
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        Memory& m = st.memory();
        // $0800 LDA $C055 (PAGE2) / $0803 STA $C054 (PAGE1) /
        // $0806 STA ($06),Y -> $C057 (HIRES) / $0808 LDA $C056 (LORES) /
        // $080B STA ($C0),Y -> $C050 (GRAPHICS): an indirect whose POINTER is
        // at $C0, the byte an absolute form has there too / $080D JMP $0800
        const uint8_t program[] = {0xAD,0x55,0xC0, 0x8D,0x54,0xC0, 0x91,0x06,
                                   0xAD,0x56,0xC0, 0x91,0xC0, 0x4C,0x00,0x08};
        m.memWrite(0x00C0, 0x50); m.memWrite(0x00C1, 0xC0);
        for (std::size_t i = 0; i < sizeof program; ++i)
            m.memWrite(static_cast<uint16_t>(0x0800 + i), program[i]);
        m.memWrite(0x0006, 0x57); m.memWrite(0x0007, 0xC0);
        st.cpu().setProgramCounter(0x0800);
        st.cpu().setYRegister(0);
    }
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 4; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    Memory::DisplayFrame frame;
    { auto st = ctrl.lockState(); frame = st.memory().displayFrame(); }
    int page2 = 0, page1 = 0, hires = 0, lores = 0, graphics = 0;
    for (const Memory::VideoEvent& e : frame.events) {
        if (e.kind == K::Page2 && e.value)  { assert(e.pc == 0x0800); ++page2; }
        if (e.kind == K::Page2 && !e.value) { assert(e.pc == 0x0803); ++page1; }
        if (e.kind == K::HiRes && e.value)  { assert(e.pc == 0x0806 && "the indirect form is two bytes"); ++hires; }
        if (e.kind == K::HiRes && !e.value) { assert(e.pc == 0x0808); ++lores; }
        if (e.kind == K::TextMode && !e.value) { assert(e.pc == 0x080B && "the opcode decides, not the $C0 before the PC"); ++graphics; }
    }
    assert(page2 > 10 && page1 > 10 && hires > 10 && lores > 10 && graphics > 10 &&
           "the frame recorded no throws");
    std::printf("[ OK ] a soft-switch throw names the instruction that made it\n");
}

// Run-to-beam names a TIME, not an address: it must stop while a card's
// processor holds the bus and no 6502 instruction is ever offered.
static void testRunToBeamUnderABusMaster()
{
    using D = pom2::Debugger;
    using Mode = EmulationController::Mode;
    struct HoldsTheBus final : SlotPeripheral {
        Memory* memory = nullptr;
        std::string_view name() const override { return "bus master test"; }
        bool dmaActive() const override { return true; }
        int dmaRun(int budget) override {          // "instructions" of 4 cycles
            const int spent = budget < 4 ? (budget < 1 ? 1 : budget) : 4;
            memory->advanceCycles(spent);
            return spent;
        }
    };
    EmulationController ctrl;
    auto card = std::make_unique<HoldsTheBus>();
    VideoStandard standard;
    {
        auto st = ctrl.lockState();
        card->memory = &st.memory();
        standard = st.memory().videoStandard();
        st.memory().slotBus().plug(4, std::move(card));
    }
    ctrl.setMode(Mode::Running);
    ctrl.tickFrame();
    ctrl.debugRunToBeam(100, 30);
    for (int i = 0; i < 4 && ctrl.getMode() == Mode::Running; ++i) ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped && "run-to-beam never fired under a bus master");
    const D::Hit hit = ctrl.debugger().lastHit();
    assert(hit.reason == D::Reason::RunToBeam && hit.busMaster);
    const pom2::BeamPosition at = pom2::beamPositionAt(hit.videoCycle, standard);
    assert(at.line == 100 && at.h >= 30 && at.h < 30 + 4);
    std::printf("[ OK ] run-to-beam stops while a card's processor holds the bus\n");
}

// A Step Back takes ONE entry off the execution history, the instruction it
// un-did; it does not wipe the trail (nor the profile) behind it.
static void testStepBackKeepsTheHistory()
{
    using Mode = EmulationController::Mode;
    EmulationController ctrl;
    loadBackLoop(ctrl);
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 6; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    {
        auto st = ctrl.lockState();
        ctrl.debugger().setHistoryEnabled(true);
        ctrl.debugger().setProfiling(true);
        ctrl.syncDebugHook();
    }
    for (int i = 0; i < 6; ++i) { ctrl.requestStep(1); ctrl.tickFrame(); }
    std::size_t before = 0, profiled = 0;
    uint16_t lastPc = 0;
    {
        auto st = ctrl.lockState();
        const auto history = ctrl.debugger().history();
        before = history.size();
        lastPc = history.back().pc;
        profiled = ctrl.debugger().profile(1000).size();
    }
    assert(before == 6 && profiled > 0);
    assert(ctrl.debugStepBack());
    {
        auto st = ctrl.lockState();
        const auto history = ctrl.debugger().history();
        assert(history.size() == 5 && "a Step Back must take one entry off the history, not all");
        assert(st.cpu().getProgramCounter() == lastPc && "the entry removed is the instruction un-done");
        assert(ctrl.debugger().profile(1000).size() == profiled && "a Step Back wiped the profile");
    }
    std::printf("[ OK ] a Step Back takes one entry off the history and keeps the profile\n");
}

// A byte exchanged with the outside (serial, network, MIDI) cannot be taken
// back: no replay crosses one. Three ways to meet it: by a single step, in
// the frame the machine then stops in, and frames ago.
static void testNoGoingBackAcrossAHostExchange()
{
    using D = pom2::Debugger;
    using Mode = EmulationController::Mode;
    struct TalksToTheHost final : SlotPeripheral {
        bool talking = false;
        std::string_view name() const override { return "host exchange test"; }
        void deviceSelectWrite(uint8_t, uint8_t) override { if (talking) pom2::noteHostIo(); }
    };
    EmulationController ctrl;
    auto card = std::make_unique<TalksToTheHost>();
    TalksToTheHost* peer = card.get();
    {
        auto st = ctrl.lockState();
        const uint8_t program[] = {0xE6,0x10, 0x8D,0xB0,0xC0, 0x4C,0x00,0x08};   // INC / STA $C0B0 / JMP
        for (std::size_t i = 0; i < sizeof program; ++i)
            st.memory().memWrite(static_cast<uint16_t>(0x0800 + i), program[i]);
        st.cpu().setProgramCounter(0x0800);
        st.memory().slotBus().plug(3, std::move(card));
        ctrl.debugger().addBreakpoint(0x0800);              // a known place to stop
        ctrl.syncDebugHook();
    }
    ctrl.rewind().setEnabled(true);
    const auto canGoBack = [&](const char** why = nullptr) {
        auto st = ctrl.lockState();
        return ctrl.debugCanStepBack(why);
    };
    const auto where = [&] {
        auto st = ctrl.lockState();
        return std::make_pair(st.cpu().getProgramCounter(), st.memory().getCycleCounter());
    };
    ctrl.setMode(Mode::Running);
    ctrl.tickFrame();                           // stops on $0800 at once
    {
        auto st = ctrl.lockState();
        pom2::Debugger::Gate gate; gate.ignore = 60;        // from now on: sixty turns, then stop
        ctrl.debugger().setBreakpointGate({0x0800, {}}, gate);
    }
    for (int i = 0; i < 6; ++i) { ctrl.setMode(Mode::Running); ctrl.tickFrame(); }
    assert(ctrl.getMode() == Mode::Stopped && where().first == 0x0800 && canGoBack());

    // 1. By a step: INC, then STA sends the byte, then JMP.
    peer->talking = true;
    ctrl.debugStepInstruction(); ctrl.tickFrame();          // INC $10
    ctrl.debugStepInstruction(); ctrl.tickFrame();          // STA $C0B0: the byte leaves
    const uint64_t sent = where().second;
    ctrl.debugStepInstruction(); ctrl.tickFrame();          // JMP
    peer->talking = false;
    assert(canGoBack() && "the step after the byte can be taken back");
    assert(ctrl.debugStepBack() && where().second == sent);
    const char* why = nullptr;
    assert(!canGoBack(&why) && why && std::string(why).find("outside") != std::string::npos);
    assert(!ctrl.debugStepBack() && where().second == sent && "the step that sent the byte was replayed");

    // 2. In the frame the machine stops in: nothing has captured since.
    { auto st = ctrl.lockState(); ctrl.debugger().resetPassCounts(); }      // sixty turns again
    peer->talking = true;
    ctrl.setMode(Mode::Running);
    ctrl.tickFrame();                           // sixty turns, each sending, then the breakpoint
    peer->talking = false;
    assert(ctrl.getMode() == Mode::Stopped);
    why = nullptr;
    // (Refused either way: by the stop itself where no frame was captured —
    // the worker's path — or by the barrier on the frame captured at the
    // stop — this single-threaded path. Both say "outside".)
    assert(!canGoBack(&why) && why && std::string(why).find("outside") != std::string::npos);
    assert(!ctrl.debugStepBack() && !ctrl.debugRunBack());

    // 3. Frames ago: back is open again, down to the first frame after it.
    const uint64_t lastSent = where().second;
    { auto st = ctrl.lockState(); ctrl.debugger().clearBreakpoints(); ctrl.syncDebugHook(); }
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 3; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    assert(canGoBack());
    ctrl.setRunBackBudget(std::chrono::hours(1));
    assert(ctrl.debugRunBack());
    assert(ctrl.debugger().lastHit().reason == D::Reason::RunBackStart);
    assert(where().second >= lastSent && "Run Back crossed a byte sent to the host");
    assert(!canGoBack());
    std::printf("[ OK ] no Step Back or Run Back across a byte exchanged with the host\n");
}

// Host input is something a replay cannot reproduce by re-executing. What
// arrives while the machine STANDS STILL closes nothing (the pointer crossing
// the screen on its way to the Debugger must not grey Step Back out); when the
// machine then executes with it, a frame is captured right there, so the way
// back stays open down to that point — and not across it.
static void testHostInputAndTheWayBack()
{
    using Mode = EmulationController::Mode;
    EmulationController ctrl;
    loadBackLoop(ctrl);
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 6; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    const auto canGoBack = [&](const char** why = nullptr) {
        auto st = ctrl.lockState();
        return ctrl.debugCanStepBack(why);
    };
    const auto cycle = [&] { auto st = ctrl.lockState(); return st.memory().getCycleCounter(); };
    assert(canGoBack());
    // Standing still: a key, a paddle, change nothing behind the machine.
    ctrl.memory().queueKey('A');                // its own lock (kbMutex), as the UI calls it
    { auto st = ctrl.lockState(); st.memory().setPaddle(0, 200); }
    assert(canGoBack() && "input received while stopped closed the way back");
    // Three steps taken WITH them: back is open down to where they began...
    const uint64_t withInput = cycle();
    for (int i = 0; i < 3; ++i) { ctrl.requestStep(1); ctrl.tickFrame(); }
    assert(canGoBack() && "a step taken with new input closed the way back altogether");
    for (int i = 0; i < 3; ++i) assert(ctrl.debugStepBack());
    assert(cycle() == withInput);
    // ...and not across that point: before it, the machine had no such key.
    const char* why = nullptr;
    assert(!canGoBack(&why) && why && std::string(why).find("a key") != std::string::npos);
    assert(!ctrl.debugStepBack());
    // A Run is the same: input that arrived at a standstill, then a stop
    // INSIDE the frame that follows — back is open down to the resume.
    ctrl.memory().queueKey('B');
    {
        auto st = ctrl.lockState();
        ctrl.debugger().addBreakpoint(0x0802);
        pom2::Debugger::Gate gate; gate.ignore = 40;        // stop mid-frame, forty turns in
        ctrl.debugger().setBreakpointGate({0x0802, {}}, gate);
        ctrl.syncDebugHook();
    }
    const uint64_t resumed = cycle();
    ctrl.setMode(Mode::Running);
    ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped && cycle() > resumed);
    assert(canGoBack() && "a stop inside the frame after a resume with new input closed the way back");
    ctrl.setRunBackBudget(std::chrono::hours(1));
    { auto st = ctrl.lockState(); ctrl.debugger().clearBreakpoints(); ctrl.syncDebugHook(); }
    assert(ctrl.debugRunBack() && cycle() == resumed && "back goes down to the resume, and stops there");
    std::printf("[ OK ] input at a standstill closes nothing; back stays open down to where the machine ran with it\n");
}

// A rewind frame restores the keyboard it was captured with: the latch and
// its strobe are not in the machine snapshot, and a replay that reads $C000
// must find the key the original run found.
static void testReplayRestoresTheKeyboard()
{
    using Mode = EmulationController::Mode;
    EmulationController ctrl;
    loadBackLoop(ctrl);                         // never reads the keyboard
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 4; ++i) ctrl.tickFrame();
    ctrl.memory().queueKey('K');                // latched, strobe set, nobody takes it
    for (int i = 0; i < 4; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    // (memRead: a debugger's peek does not read the keyboard, by design.)
    const auto latch = [&] { auto st = ctrl.lockState(); return st.memory().memRead(0xC000); };
    {
        auto st = ctrl.lockState();
        assert(ctrl.debugCanStepBack());
        assert(st.memory().memRead(0xC000) == ('K' | 0x80));
        (void)st.memory().memRead(0xC010);      // the strobe goes (as a guest's $C010 would)
    }
    assert((latch() & 0x80) == 0);
    assert(ctrl.debugStepBack());
    assert(latch() == ('K' | 0x80) && "the frame's keyboard was not restored: the replay had no key");
    std::printf("[ OK ] a replay finds the keyboard its frame was captured with\n");
}

// The priming writes of a Library "Boot" (the text screen, the zero page)
// are POM2's, not the guest's: no pass for a breakpoint on one of them.
static void testBootPrimingIsNotAPass()
{
    using D = pom2::Debugger;
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        st.memory().slotBus().plug(5, std::make_unique<BootableStub>());
        ctrl.debugger().setWatchpoint(0x0036, D::Write);
        D::Gate gate; gate.ignore = 1000;
        ctrl.debugger().setWatchGate({0x0036, {}}, gate);
        ctrl.syncDebugHook();
    }
    assert(ctrl.bootFromSlot(5));
    {
        auto st = ctrl.lockState();
        const auto& gates = ctrl.debugger().watchGates();
        const auto found = gates.find({0x0036, {}});
        assert((found == gates.end() || found->second.passes == 0) && "a boot's priming counted a pass");
        // ...and the flag that says "POM2's own writes" was put down again.
        const uint8_t before = st.memory().peekCpuView(0x0036);
        st.memory().memWrite(0x0036, static_cast<uint8_t>(before ^ 0xFF));
        assert(ctrl.debugger().watchGates().at({0x0036, {}}).passes == 1 &&
               "memory breakpoints stayed switched off after the boot");
    }
    std::printf("[ OK ] a boot's priming writes are no pass, and breakpoints work after it\n");
}

// A display switch thrown by a card's processor holding the bus names no
// 6502 instruction; and a stop made there gives the parked PC no amnesty.
static void testBusMasterThrowsAndStops()
{
    using D = pom2::Debugger;
    using Mode = EmulationController::Mode;
    struct Z80Like final : SlotPeripheral {
        Memory* memory = nullptr;
        bool hold = true;
        std::string_view name() const override { return "bus master switch test"; }
        bool dmaActive() const override { return hold; }
        int dmaRun(int) override {
            memory->memWrite(0xC055, 0);            // PAGE2, by the card
            memory->advanceCycles(4);
            memory->memWrite(0xC054, 0);
            memory->advanceCycles(4);
            return 8;
        }
    };
    EmulationController ctrl;
    auto card = std::make_unique<Z80Like>();
    Z80Like* z80 = card.get();
    uint16_t parked = 0;
    {
        auto st = ctrl.lockState();
        card->memory = &st.memory();
        st.memory().memWrite(0x0800, 0xEA); st.memory().memWrite(0x0801, 0xEA);   // NOP NOP
        st.memory().memWrite(0x0802, 0x4C); st.memory().memWrite(0x0803, 0x02); st.memory().memWrite(0x0804, 0x08);
        st.cpu().setProgramCounter(0x0800);
        parked = 0x0800;
        st.memory().slotBus().plug(4, std::move(card));
    }
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 3; ++i) ctrl.tickFrame();
    Memory::DisplayFrame frame;
    { auto st = ctrl.lockState(); frame = st.memory().displayFrame(); }
    int throws = 0;
    for (const Memory::VideoEvent& e : frame.events)
        if (e.kind == Memory::VideoEventKind::Page2) { assert(e.pc == Memory::VideoEvent::kNoPc); ++throws; }
    assert(throws > 10 && "the card's switch throws were not recorded");

    // Run to the beam: stops with the card holding the bus...
    ctrl.debugRunToBeam(60, 30);
    for (int i = 0; i < 4 && ctrl.getMode() == Mode::Running; ++i) ctrl.tickFrame();
    assert(ctrl.debugger().lastHit().reason == D::Reason::RunToBeam && ctrl.debugger().lastHit().busMaster);
    // ...and a breakpoint then set on the parked instruction stops the first
    // time the 6502 gets there, once the card lets go: no amnesty skipped it.
    {
        auto st = ctrl.lockState();
        assert(st.cpu().getProgramCounter() == parked);
        ctrl.debugger().addBreakpoint(parked);
        ctrl.syncDebugHook();
        ctrl.debugResume();
        z80->hold = false;
    }
    ctrl.setMode(Mode::Running);
    ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped);
    assert(ctrl.debugger().lastHit().reason == D::Reason::Breakpoint && ctrl.debugger().lastHit().pc == parked);
    { auto st = ctrl.lockState(); assert(st.cpu().getProgramCounter() == parked && "the parked instruction ran past its breakpoint"); }
    std::printf("[ OK ] a card's switch throw names no instruction; a stop under it leaves no amnesty\n");
}

// A change made by hand while the machine stands still (a byte in the Memory
// viewer, the PC) is not something a replay reproduces: a Step Back from a
// frame before it would silently undo it. The way back closes there.
static void testChangeByHandClosesTheWayBack()
{
    using Mode = EmulationController::Mode;
    EmulationController ctrl;
    loadBackLoop(ctrl);
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 6; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    {
        const char* why = nullptr;
        auto st = ctrl.lockState();
        assert(ctrl.debugCanStepBack());
        const uint8_t byte = 0x77;
        ctrl.debugger().patchMemory(st.memory(), 0x0010, &byte, 1);
        ctrl.noteChangedByHand();
        assert(!ctrl.debugCanStepBack(&why) && why && std::string(why).find("by hand") != std::string::npos);
    }
    assert(!ctrl.debugStepBack());
    { auto st = ctrl.lockState(); assert(st.memory().peekCpuView(0x0010) == 0x77 && "the edit was undone"); }
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 2; ++i) ctrl.tickFrame();
    ctrl.setMode(Mode::Stopped);
    { auto st = ctrl.lockState(); assert(ctrl.debugCanStepBack()); }
    std::printf("[ OK ] a change made by hand closes the way back until the machine runs on\n");
}

// THE CHECK. A rewind frame does not hold everything that steers a machine
// (here: a card answering from a counter of its own, as a clock card answers
// from the host's time). The replay then takes another path, and must be
// caught: the verb is refused and the machine is exactly as it was — never
// landed one instruction before the end of a path it did not take.
static void testAReplayThatDivergesIsNotLandedOn()
{
    using Mode = EmulationController::Mode;
    struct AnswersFromOutside final : SlotPeripheral {
        uint8_t next = 0;                       // not in any snapshot
        std::string_view name() const override { return "unrecorded state test"; }
        uint8_t deviceSelectRead(uint8_t) override { return next++; }
    };
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        // $0800 LDA $C0B0 (the card) / STA $10 / INC $11 / JMP $0800
        const uint8_t program[] = {0xAD,0xB0,0xC0, 0x85,0x10, 0xE6,0x11, 0x4C,0x00,0x08};
        for (std::size_t i = 0; i < sizeof program; ++i)
            st.memory().memWrite(static_cast<uint16_t>(0x0800 + i), program[i]);
        st.cpu().setProgramCounter(0x0800);
        st.memory().slotBus().plug(3, std::make_unique<AnswersFromOutside>());
    }
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 4; ++i) ctrl.tickFrame();
    {
        auto st = ctrl.lockState();
        ctrl.debugger().addBreakpoint(0x0805);  // after the STA: $10 holds what the card said
        pom2::Debugger::Gate gate; gate.ignore = 30;        // thirty turns into the next frame
        ctrl.debugger().setBreakpointGate({0x0805, {}}, gate);
        ctrl.syncDebugHook();
    }
    ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped);
    struct Seen { uint16_t pc; uint8_t a, said, turns; uint64_t cycle;
        bool operator==(const Seen& o) const {
            return pc == o.pc && a == o.a && said == o.said && turns == o.turns && cycle == o.cycle; } };
    const auto seen = [&] {
        auto st = ctrl.lockState();
        return Seen{st.cpu().getProgramCounter(), st.cpu().getAccumulator(),
                    st.memory().peekCpuView(0x0010), st.memory().peekCpuView(0x0011),
                    st.memory().getCycleCounter()};
    };
    const Seen before = seen();
    { auto st = ctrl.lockState(); assert(ctrl.debugCanStepBack() && "nothing says this stretch cannot be replayed"); }
    assert(!ctrl.debugStepBack() && "a replay that read other values was landed on");
    assert(seen() == before && "the machine is not as it was");
    assert(ctrl.debugger().lastHit().reason == pom2::Debugger::Reason::Breakpoint);
    assert(!ctrl.debugRunBack() && seen() == before);
    // And it is not tried again from here: the way back is closed until the machine runs on.
    { auto st = ctrl.lockState(); assert(!ctrl.debugCanStepBack()); }
    std::printf("[ OK ] a replay that takes another path is caught, and the machine put back\n");
}

// After a Step Back the screen still has its page flips: the soft-switch log
// of the frame in progress and the frame published before it are rebuilt by
// re-executing the frames that lead to the landing (a restore empties them).
static void testStepBackKeepsThePageFlips()
{
    using Mode = EmulationController::Mode;
    using K = Memory::VideoEventKind;
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        // $0800 LDA $C055 / LDA $C054 / INC $10 / JMP $0800 — PAGE2 and back, every 16 cycles
        const uint8_t program[] = {0xAD,0x55,0xC0, 0xAD,0x54,0xC0, 0xE6,0x10, 0x4C,0x00,0x08};
        for (std::size_t i = 0; i < sizeof program; ++i)
            st.memory().memWrite(static_cast<uint16_t>(0x0800 + i), program[i]);
        st.cpu().setProgramCounter(0x0800);
    }
    ctrl.rewind().setEnabled(true);
    ctrl.setMode(Mode::Running);
    for (int i = 0; i < 6; ++i) ctrl.tickFrame();
    {
        auto st = ctrl.lockState();
        ctrl.debugger().addBreakpoint(0x0806);
        pom2::Debugger::Gate gate; gate.ignore = 300;       // a quarter of a frame in
        ctrl.debugger().setBreakpointGate({0x0806, {}}, gate);
        ctrl.syncDebugHook();
    }
    ctrl.tickFrame();
    assert(ctrl.getMode() == Mode::Stopped);
    const auto flips = [&](bool beamView) {
        auto st = ctrl.lockState();
        st.memory().setBeamView(beamView);
        const Memory::DisplayFrame frame = st.memory().displayFrame();
        std::size_t n = 0;
        for (const Memory::VideoEvent& e : frame.events) if (e.kind == K::Page2) ++n;
        return n;
    };
    const std::size_t publishedBefore = flips(false), compositeBefore = flips(true);
    assert(publishedBefore > 100 && compositeBefore > 100 && "the frame recorded no page flips");
    assert(ctrl.debugStepBack());
    const std::size_t published = flips(false), composite = flips(true);
    assert(published == publishedBefore && "the published frame lost its page flips in the step back");
    assert(composite + 2 >= compositeBefore && composite <= compositeBefore &&
           "the frame in progress lost its page flips in the step back");
    { auto st = ctrl.lockState(); st.memory().setBeamView(false); }
    std::printf("[ OK ] a Step Back keeps the frame's page flips on screen\n");
}

// Stop on X with an IRQ due, Run: the handler runs first. Stop on the
// handler's H, Run: H's amnesty must not replace X's, or X stops again after
// the RTI having executed nothing, its pass counted twice. And the toolbar's
// Step (no amnesty, only the latched hit) is protected the same way.
static void testAnInterruptKeepsTheAmnestyItHoldsUp()
{
    EmulationController ctrl;
    auto st = ctrl.lockState(); auto& cpu = st.cpu(); auto& d = ctrl.debugger();
    d.addBreakpoint(0x0800);
    d.addBreakpoint(0xFA62);
    d.setBreakpointGate({0x0800, {}}, pom2::Debugger::Gate{});
    assert(d.onInstruction(0x0800));
    const uint64_t passes = d.breakpointGates().at({0x0800, {}}).passes;
    d.clearHit(); d.armResumeFrom(0x0800, 0xF0);         // Run from X, SP $F0
    assert(!d.onInstruction(0x0800));                    // X offered again: amnestied
    d.onInterruptEntered(cpu, 0x0800, 7);                // IRQ taken in X's place
    assert(d.onInstruction(0xFA62) && "the handler's breakpoint stops");
    d.clearHit(); d.armResumeFrom(0xFA62, 0xED);         // Run from H, three bytes deeper
    assert(!d.onInstruction(0xFA62));
    d.onStepCompleted(cpu, 0xFA62, 2);
    assert(!d.onInstruction(0x0800) && "X kept its amnesty through H's");
    assert(d.breakpointGates().at({0x0800, {}}).passes == passes && "X counted once");
    d.onStepCompleted(cpu, 0x0800, 3);
    assert(d.onInstruction(0x0800) && "spent once X has run");
    // A resume from the SAME depth replaces: a stale amnesty would skip a stop.
    d.clearHit(); d.armResumeFrom(0x0800, 0xF0);
    d.clearHit(); d.armResumeFrom(0x0900, 0xF0);
    assert(d.onInstruction(0x0800) && "an abandoned amnesty does not linger");

    // The toolbar's Step from a latched stop, an interrupt taken instead.
    d.clearHit();
    pom2::Debugger t;
    t.addBreakpoint(0x0800);
    assert(t.onInstruction(0x0800));
    t.beginStep(0x0800, true);
    t.onInterruptEntered(cpu, 0x0800, 7);
    assert(!t.onInstruction(0xFA00));
    t.onStepCompleted(cpu, 0xFA00, 2);
    assert(!t.onInstruction(0x0800) && "the stepped-off stop does not come back after the RTI");
    t.onStepCompleted(cpu, 0x0800, 3);
    assert(t.onInstruction(0x0800));
    std::printf("[ OK ] an interrupt keeps the amnesty of the instruction it holds up\n");
}

static void runGateCases()
{
    testAnInterruptKeepsTheAmnestyItHoldsUp();
    testStepBackKeepsThePageFlips();
    testAReplayThatDivergesIsNotLandedOn();
    testHostInputAndTheWayBack();
    testReplayRestoresTheKeyboard();
    testBootPrimingIsNotAPass();
    testBusMasterThrowsAndStops();
    testChangeByHandClosesTheWayBack();
    testNoGoingBackAcrossAHostExchange();
    testRunToBeamUnderABusMaster();
    testStepBackKeepsTheHistory();
    testCmosRmwIsOneRead();
    testOwnWritesAreNotAccesses();
    testSteppedPassesCount();
    testNoGoingBackAcrossAReset();
    testAbandonedReplayRestoresThePresent();
    testVideoEventsNameTheirInstruction();
    testNoGoingBackAcrossAnAcceleratorSpeedChange();
    testReadWriteGateCountsAnRmwAsTwo();
    testGoingBackThroughInterrupts();
    testStepBackIsTheInverseOfStep();
    testStepBackUnderTheWorkerThread();
    testStepBackAndRunBack();
    testWatchListText();
    testGateIgnoreCountOnAPcBreakpoint();
    testEditingAConditionInPlace();
    testGateOnTheValueWritten();
    testGateJudgesTheRmwsLastWrite();
    testBeamWindowArithmetic();
    testBeamGateAndRunToBeam();
}
