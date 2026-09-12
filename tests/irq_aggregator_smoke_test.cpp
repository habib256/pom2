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

// IRQ aggregator smoke test. Pins the wire-OR semantics of
// `M6502::setIrqLine(sourceId, asserted)`:
//
//   1. Multiple sources can assert IRQ in parallel; releasing one does
//      NOT drop the line while another is still asserting. This is the
//      headline bug the aggregator was added to fix — Mockingboard +
//      SSC + Mouse plugged together previously stomped each other's
//      IRQ state via the last-writer-wins `cpu->setIRQ(0|1)` path.
//   2. The legacy `setIRQ(int)` entry point lives on its own back-compat
//      source bit and coexists with per-slot sources.
//   3. `getIrqSourceMask()` exposes the OR'd contributor mask.
//   4. Calls are idempotent — re-asserting / re-clearing the same source
//      doesn't disturb other bits.
//   5. End-to-end: the dispatch loop honours the aggregated mask — a
//      step with any bit set and I=0 vectors through $FFFE.
//   6. Two-thread hammer: the line IS the mask. A source held asserted by
//      one thread is never dropped by another thread's deassert of a
//      different source (the lost-update the derived `IRQ` flag allowed).

#include "M6502.h"
#include "Memory.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

// Plant the handler in RAM. The test puts Memory in flat-RAM (Klaus)
// mode so writes to $FFFE/$FFFF and arbitrary RAM addresses bypass the
// I/O page and language-card ROM protection.
constexpr uint16_t kIrqHandlerAddr = 0x0900;
constexpr uint16_t kPcStart        = 0x0300;

// Plant: NOP sled at kPcStart, RTI ($40) at the IRQ handler, $FFFE/$FFFF
// pointing at the handler. Execute CLI to clear the interrupt-disable
// flag so IRQs can actually fire. After this, PC = kPcStart and I = 0.
//
// The handler is a single RTI: an IRQ vectors there, pulls P + PC, and
// restores us to kPcStart with I = 0. This lets the test fire many IRQs
// in a row without manually clearing I between each one.
void primeForIrq(Memory& mem, M6502& cpu)
{
    mem.memWrite(0xFFFE, kIrqHandlerAddr & 0xFF);
    mem.memWrite(0xFFFF, (kIrqHandlerAddr >> 8) & 0xFF);
    for (uint16_t i = 0; i < 16; ++i) mem.memWrite(kPcStart + i, 0xEA);  // NOP
    mem.memWrite(kIrqHandlerAddr, 0x40);                                  // RTI
    // CLI ($58) at $0200, then jump to kPcStart for the actual test path.
    mem.memWrite(0x0200, 0x58);   // CLI
    cpu.setProgramCounter(0x0200);
    cpu.step();                   // executes CLI → I = 0
    cpu.setProgramCounter(kPcStart);
}

// One step. Returns true iff the CPU vectored through the IRQ handler
// (RTI restores PC to kPcStart). Returns false iff it ran the NOP at
// kPcStart (PC advances to kPcStart + 1).
bool stepFiredIrq(M6502& cpu)
{
    cpu.setProgramCounter(kPcStart);
    cpu.step();
    const uint16_t pc = cpu.getProgramCounter();
    if (pc == kPcStart)       return true;    // RTI returned here
    if (pc == kPcStart + 1)   return false;   // NOP advanced
    std::printf("FAIL: unexpected PC after step: 0x%04X\n", pc);
    std::abort();
}

}  // namespace

int main()
{
    // ─── Test 1: wire-OR of two slot sources ──────────────────────────────
    {
        Memory mem;
        mem.setTestMode(true);
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        primeForIrq(mem, cpu);

        // Slot 2 asserts.
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT2, true);
        assert(cpu.getIrqSourceMask() == (1u << 2));
        assert(stepFiredIrq(cpu));

        // Slot 4 also asserts (both pulling).
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT4, true);
        assert(cpu.getIrqSourceMask() == ((1u << 2) | (1u << 4)));
        assert(stepFiredIrq(cpu));

        // Slot 2 releases — slot 4 still asserts → IRQ still fires.
        // This is the wire-OR bug the aggregator was added to fix.
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT2, false);
        assert(cpu.getIrqSourceMask() == (1u << 4));
        assert(stepFiredIrq(cpu));

        // Slot 4 releases — mask is now zero → IRQ does NOT fire.
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT4, false);
        assert(cpu.getIrqSourceMask() == 0);
        assert(!stepFiredIrq(cpu));

        std::printf("[ OK ] wire-OR of two slot sources\n");
    }

    // ─── Test 2: legacy setIRQ() coexists with setIrqLine() ───────────────
    {
        Memory mem;
        mem.setTestMode(true);
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        primeForIrq(mem, cpu);

        cpu.setIrqLine(M6502::IRQ_SRC_SLOT6, true);
        cpu.setIRQ(1);
        const uint32_t mask = cpu.getIrqSourceMask();
        assert((mask & (1u << 6))                       != 0);
        assert((mask & (1u << M6502::IRQ_SRC_LEGACY))   != 0);
        assert(stepFiredIrq(cpu));

        // Legacy clears. Slot 6 must keep the line asserted.
        cpu.setIRQ(0);
        assert(cpu.getIrqSourceMask() == (1u << 6));
        assert(stepFiredIrq(cpu));

        // Slot 6 releases too → no IRQ.
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT6, false);
        assert(cpu.getIrqSourceMask() == 0);
        assert(!stepFiredIrq(cpu));

        std::printf("[ OK ] legacy setIRQ() coexists with per-source\n");
    }

    // ─── Test 3: idempotent assertion / clear ─────────────────────────────
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);

        cpu.setIrqLine(M6502::IRQ_SRC_VBL, true);
        cpu.setIrqLine(M6502::IRQ_SRC_VBL, true);     // dup
        assert(cpu.getIrqSourceMask() == (1u << M6502::IRQ_SRC_VBL));

        cpu.setIrqLine(M6502::IRQ_SRC_SLOT3, true);
        cpu.setIrqLine(M6502::IRQ_SRC_VBL, false);
        cpu.setIrqLine(M6502::IRQ_SRC_VBL, false);    // dup
        assert(cpu.getIrqSourceMask() == (1u << M6502::IRQ_SRC_SLOT3));

        std::printf("[ OK ] idempotent assert/release\n");
    }

    // ─── Test 4: two-thread hammer — the line cannot lag the mask ─────────
    // `setIrqLine` used to do TWO stores: the mask's atomic RMW, then a
    // derived `std::atomic<int> IRQ` mirroring `(mask != 0)`. Two stores are
    // not one atomic operation. Interleave a deassert with an assert —
    //
    //   B: fetch_and(~B)  → its newMask reads 0        (A has not set yet)
    //   A: fetch_or(A)    → mask = {A}
    //   A: IRQ.store(1)
    //   B: IRQ.store(0)                                 ← lands last
    //
    // — and the machine is left with mask = {A} and the line DOWN: a card
    // that wants service and never gets it, until some unrelated source
    // happens to touch setIrqLine again. Off-CPU-thread callers are real
    // (the SSC's TCP worker, the FujiNet relay), so this is reachable.
    //
    // The pin: main holds slot 6 asserted for the whole run, so `mask != 0`
    // is an invariant no interleaving can break, and steps the CPU in a loop
    // — every step MUST vector. Two worker threads hammer their own bits
    // meanwhile. With the derived flag present this fails within a few
    // hundred thousand toggles; with `step()` reading the mask itself it
    // cannot fail at all, because there is no second store to lose.
    {
        Memory mem;
        mem.setTestMode(true);
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        primeForIrq(mem, cpu);

        cpu.setIrqLine(M6502::IRQ_SRC_SLOT6, true);      // held for the run

        std::atomic<bool> stop{false};
        auto hammer = [&cpu, &stop](int src) {
            while (!stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < 512; ++i) {
                    cpu.setIrqLine(src, true);
                    cpu.setIrqLine(src, false);
                }
            }
        };
        std::thread t2(hammer, M6502::IRQ_SRC_SLOT2);
        std::thread t4(hammer, M6502::IRQ_SRC_SLOT4);

        int missed = 0;
        for (int i = 0; i < 400000; ++i) {
            if (!stepFiredIrq(cpu)) ++missed;
        }
        stop.store(true, std::memory_order_relaxed);
        t2.join();
        t4.join();

        if (missed != 0) {
            std::printf("FAIL: slot 6 held the IRQ line asserted for the whole "
                        "run, yet %d of 400000 steps did not vector — the line "
                        "is being derived by a store that races the mask.\n",
                        missed);
            return 1;
        }
        // Both workers stopped with their bits clear; drop slot 6 too.
        assert(cpu.getIrqSourceMask() == (1u << 6));
        cpu.setIrqLine(M6502::IRQ_SRC_SLOT6, false);
        assert(cpu.getIrqSourceMask() == 0);
        assert(!stepFiredIrq(cpu));

        std::printf("[ OK ] two-thread hammer: a held source is never lost\n");
    }

    std::printf("All IRQ aggregator smoke tests passed.\n");
    return 0;
}
