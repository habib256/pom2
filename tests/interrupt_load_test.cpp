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

// interrupt_load — the share of each video frame spent in IRQ/NMI handlers
// (InterruptLoad.h, fed by the Debugger while profiling is on).
//
// Part 1 drives the class alone: nesting, a stack that wraps through $00,
// the RTI charged to its handler, frame rollover and the summary.
// Part 2 runs a real 6502 with one IRQ per video frame whose handler costs an
// exact, hand-counted number of cycles, and checks every completed frame
// reports exactly that many interrupt cycles.

#include "CpuClock.h"
#include "Debugger.h"
#include "InterruptLoad.h"
#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

using pom2::InterruptLoad;

static void testClassAlone()
{
    InterruptLoad load;
    // Frame 0: 100 main cycles, an IRQ (7) at SP $F0 -> $ED, 20 handler
    // cycles with a JSR/RTS inside (SP dips and comes back, still nested),
    // then the RTI (6) that restores SP $F0.
    load.stepped(0xF0, 100, 0);
    CHECK(!load.inInterrupt());
    load.entered(0xED, 7, 0);
    CHECK(load.inInterrupt());
    load.stepped(0xEB, 6, 0);          // JSR: SP $EB
    load.stepped(0xEB, 8, 0);          // body
    load.stepped(0xED, 6, 0);          // RTS: back to the handler's level
    CHECK(load.inInterrupt() && "an RTS inside the handler does not end it");
    load.stepped(0xF0, 6, 0);          // RTI
    CHECK(!load.inInterrupt() && "the RTI ends the handler");
    load.stepped(0xF0, 50, 0);
    CHECK(load.current().cycles == 100 + 7 + 6 + 8 + 6 + 6 + 50);
    CHECK(load.current().interruptCycles == 7 + 6 + 8 + 6 + 6 && "the RTI is handler time");
    CHECK(load.current().entries == 1);
    CHECK(load.frames().empty() && "frame 0 is still open");

    // Frame 1 closes frame 0. An NMI nested inside an IRQ: two levels.
    load.stepped(0xF0, 10, 1);
    CHECK(load.frames().size() == 1 && load.frames()[0].index == 0);
    load.entered(0xED, 7, 1);          // IRQ
    load.entered(0xEA, 7, 1);          // NMI on top of it
    CHECK(load.depth() == 2);
    load.stepped(0xED, 6, 1);          // NMI's RTI
    CHECK(load.depth() == 1 && "only the NMI closed");
    load.stepped(0xF0, 6, 1);          // IRQ's RTI
    CHECK(load.depth() == 0);
    CHECK(load.current().interruptCycles == 26);

    // A stack that wraps: IRQ at SP $01 pushes to $FE. $FE is BELOW $01 on
    // a wrapped stack, so the handler is still open there.
    load.entered(0xFE, 7, 1);
    load.stepped(0xFE, 2, 1);
    CHECK(load.inInterrupt() && "a wrapped stack is not a return");
    load.stepped(0x01, 6, 1);
    CHECK(!load.inInterrupt());

    // A handler that resets the stack (LDX #$FF / TXS) and never RTIs
    // still ends: SP is above its level.
    load.entered(0xED, 7, 1);
    load.stepped(0xFF, 2, 1);
    CHECK(!load.inInterrupt());

    // Summary over completed frames; a skipped frame index just closes.
    load.stepped(0xFF, 10, 5);
    const auto s = load.summary();
    CHECK(s.frames == 2);
    const double f0 = 33.0 / 183.0;
    CHECK(s.peak >= f0 - 1e-9);
    CHECK(s.last == load.frames().back().share());

    load.reset();
    CHECK(load.frames().empty() && load.current().cycles == 0 && !load.inInterrupt());

    // The ring keeps the last kHistory frames.
    for (uint64_t f = 0; f < InterruptLoad::kHistory + 10; ++f) load.stepped(0xFF, 3, f);
    CHECK(load.frames().size() == InterruptLoad::kHistory);
    CHECK(load.frames().front().index == 9);
    std::printf("[ OK ] InterruptLoad alone: nesting, wrap, RTI, rollover, ring\n");
}

// ── Part 2: a real CPU, one IRQ per frame, a handler of known length ─────
//   $0300  JMP $0300            main loop, 3 cycles
//   $0400  LDX #N        2
//   $0402  DEX           2  \  N times; BNE taken 3, last one 2
//   $0403  BNE $0402     3  /
//   $0405  RTI           6
// Entry 7 + 2 + 5(N-1) + 2 + 2 + 6 = 5N + 14.
static void testRealCpu()
{
    constexpr int kN = 200;
    constexpr uint64_t kHandlerCycles = 5 * kN + 14;

    Memory mem;
    M6502 cpu(&mem);
    pom2::Debugger dbg;
    dbg.setMemory(&mem);

    mem.writeRamUnchecked(0x0300, 0x4C);
    mem.writeRamUnchecked(0x0301, 0x00);
    mem.writeRamUnchecked(0x0302, 0x03);
    const uint8_t handler[] = {0xA2, kN, 0xCA, 0xD0, 0xFD, 0x40};
    for (unsigned i = 0; i < sizeof handler; ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(0x0400 + i), handler[i]);
    (void)mem.memRead(0xC08B);           // LC RAM, write-enabled, for $FFFE
    (void)mem.memRead(0xC08B);
    mem.memWrite(0xFFFE, 0x00);
    mem.memWrite(0xFFFF, 0x04);

    dbg.setProfiling(true);
    cpu.setDebugHook(&dbg);
    cpu.setProgramCounter(0x0300);
    cpu.setStatusRegister(0x20);
    cpu.setStackPointer(0xFF);

    const uint64_t fc = static_cast<uint64_t>(pom2VideoTiming(mem.videoStandard()).cyclesPerFrame);
    auto frameNow = [&] { return mem.videoNow() / fc; };

    // Let the first (partial) frame run out, then one IRQ at each frame start.
    const uint64_t first = frameNow();
    while (frameNow() == first) cpu.run(1);
    for (int i = 0; i < 6; ++i) {
        const uint64_t f = frameNow();
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT4, true);
        cpu.run(1);                       // takes the interrupt (7 cycles)
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT4, false);
        while (frameNow() == f) cpu.run(1);
    }
    cpu.run(1);                           // closes the last frame

    const auto frames = dbg.interruptLoad().frames();
    int checked = 0;
    for (const auto& fr : frames) {
        if (fr.index <= first) continue;  // the partial frame before the first IRQ
        CHECK(fr.entries == 1);
        if (fr.interruptCycles != kHandlerCycles)
            std::fprintf(stderr, "frame %llu: %llu interrupt cycles, expected %llu\n",
                         (unsigned long long)fr.index, (unsigned long long)fr.interruptCycles,
                         (unsigned long long)kHandlerCycles);
        CHECK(fr.interruptCycles == kHandlerCycles);
        CHECK(fr.cycles + 7 >= fc && fr.cycles <= fc + 7 && "a frame holds one frame of cycles");
        ++checked;
    }
    CHECK(checked == 6);
    const auto s = dbg.interruptLoad().summary();
    const double expect = static_cast<double>(kHandlerCycles) / static_cast<double>(fc);
    CHECK(s.peak > expect * 0.99 && s.peak < expect * 1.01);

    // Clearing the measurements clears the load too.
    dbg.clearMeasurements();
    CHECK(dbg.interruptLoad().frames().empty());

    // Profiling off: nothing is measured.
    dbg.setProfiling(false);
    for (int i = 0; i < 2000; ++i) cpu.run(1);
    CHECK(dbg.interruptLoad().frames().empty() && dbg.interruptLoad().current().cycles == 0);
    std::printf("[ OK ] real CPU: %d frames at %llu interrupt cycles each (%.2f %%)\n",
                checked, (unsigned long long)kHandlerCycles, 100.0 * expect);
}

int main()
{
    testClassAlone();
    testRealCpu();
    std::printf("interrupt_load: all OK\n");
    return 0;
}
