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

// The Mockingboard MMIO access-cycle sync — the fix OLDSKOOL FORT ET VERT
// (French Touch, SHADOW 2021) depends on.
//
// A stable-raster T1-IRQ demo reads the FREE-RUNNING VIA T1 counter ($C404)
// inside its handler to phase-measure the beam and dispatch. That read must
// reflect the counter as of the access's DATA cycle — the last cycle of the
// instruction that performs it — because a real 6522 has already decremented
// T1 on that φ2. POM2's lazy sync uses `getCycleCountNow() = cycleCounter +
// cpu.cycles`, and cpu.cycles does NOT yet count the in-flight data cycle, so
// without the +1 in `MockingboardCard::syncToCpuCycle()` the counter reads one
// too high. In OLDSKOOL that one count wrapped a `mem[$03] - T1CL - $19` phase
// from $00 to $FF, jumping a self-modified BVC into a slice that RTS'd off the
// 3-byte IRQ frame -> SP=0 -> crash ~10 s in.
//
// This pins the mechanism WITHOUT the disk: arm T1 free-running, peek the raw
// counter just before an executed `LDA $C404`, run the instruction, and
// require the value the CPU read to reflect the data cycle (4-cycle LDA abs ->
// counter advanced to cycleCounter+4, then the -1 read-back). Reverting the +1
// yields one-too-high and fails here.
//
// Distinct from via_t1_rearm_chain (direct VIA read-back, no MMIO sync — the
// -1 bias TRIBU's closed-loop re-arm needs, which this fix leaves untouched)
// and mockingboard_sync_smoke (MMIO/batch convergence over a window).

#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <utility>

// MockingboardCard lives in the global namespace.

// A debug hook that is ATTACHED but never stops anything — the shape the CPU
// is left in after a step-over fires and the transient that armed it is
// dropped. `M6502::step` gates its interrupt-entry split on the hook POINTER
// (see the comment there: testing an "armed" flag on that path costs 7.2 %),
// so an idle hook left attached still splits an IRQ entry into two
// `advanceCycles` calls and moves the sub-instruction phase this test pins.
// `EmulationController::runCpuSlice` therefore detaches it as soon as nothing
// is armed — pinned by `debugger` case "idle hook detaches". Here we pin the
// other half: with a hook attached and nothing armed, the phase read is
// IDENTICAL, so a debugging session that has gone quiet cannot drift OLDSKOOL.
class IdleHook : public M6502DebugHook
{
public:
    bool onInstruction(uint16_t) override { calls++; return false; }
    int  calls = 0;
};

// The probe: arm T1 free-running, peek the raw counter just before an
// executed `LDA $C404`, run it, and return { value read, expected }.
static std::pair<uint8_t, uint8_t> phaseProbe(M6502DebugHook* hook)
{
    Memory mem;
    M6502  cpu(&mem);
    auto cardp = std::make_unique<MockingboardCard>(4);
    cardp->setCpu(&cpu);
    MockingboardCard* card = cardp.get();
    mem.slotBus().plug(4, std::move(cardp));
    cpu.hardReset();
    cpu.setDebugHook(hook);
    mem.slotBus().reset();

    // Program at $0300: arm T1 continuous (latch $2000, no underflow in
    // window) via executed stores, a few NOPs to settle, then `LDA $C404`.
    uint16_t p = 0x0300;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) mem.memWrite(p++, b);
    };
    emit({0xA9, 0x00, 0x8D, 0x06, 0xC4});   // LDA #$00 ; STA $C406 (T1LL)
    emit({0xA9, 0x20, 0x8D, 0x07, 0xC4});   // LDA #$20 ; STA $C407 (T1LH)
    emit({0xA9, 0x40, 0x8D, 0x0B, 0xC4});   // LDA #$40 ; STA $C40B (ACR continuous)
    emit({0xA9, 0x20, 0x8D, 0x05, 0xC4});   // LDA #$20 ; STA $C405 (T1CH arms)
    for (int i = 0; i < 4; ++i) emit({0xEA});                // settle
    const uint16_t ldaPc = p;
    emit({0xAD, 0x04, 0xC4});               // LDA $C404 (the phase read)
    emit({0xEA});

    cpu.setProgramCounter(0x0300);
    // `run(1)` rather than `step()`: the per-instruction hook is consulted by
    // `M6502::run`'s debugged loop, not by `step()`. One instruction either
    // way (run returns as soon as the budget is met), and the same driver is
    // used with and without a hook so the two probes are comparable.
    while (cpu.getProgramCounter() != ldaPc) cpu.run(1);

    // Peek the RAW T1 counter as of the pre-instruction cycle (peek returns
    // t1Counter, no read-back bias, no sync).
    const uint16_t peekBefore =
        static_cast<uint16_t>(card->peekViaRegister(0, 0x04)) |
        static_cast<uint16_t>(card->peekViaRegister(0, 0x05) << 8);

    cpu.run(1);                                  // execute LDA $C404
    const uint8_t got = cpu.getAccumulator();    // value the CPU read

    // LDA abs = 4 cycles; the data-cycle sync lands on cycleCounter+4, and the
    // VIA read applies the -1 read-back: got == (peekBefore - 4 - 1) low byte.
    const uint8_t expected = static_cast<uint8_t>((peekBefore - 5) & 0xFF);
    return { got, expected };
}

int main()
{
    const auto bare = phaseProbe(nullptr);
    const uint8_t got = bare.first, expected = bare.second;

    if (got != expected) {
        std::fprintf(stderr,
            "mockingboard_t1_irq_phase: LDA $C404 read $%02X, expected $%02X "
            "The MMIO access-cycle +1 sync is missing — OLDSKOOL's "
            "stable-raster phase would be one too high.\n",
            got, expected);
        return 1;
    }
    // Same probe with a debug hook attached that arms nothing.
    IdleHook idle;
    const auto hooked = phaseProbe(&idle);
    if (idle.calls == 0) {
        std::fprintf(stderr, "mockingboard_t1_irq_phase: the idle hook was "
                             "never called — the case proves nothing.\n");
        return 1;
    }
    if (hooked.first != hooked.second || hooked.first != got) {
        std::fprintf(stderr,
            "mockingboard_t1_irq_phase: with an idle debug hook attached the "
            "$C404 phase read $%02X, expected $%02X (bare run read $%02X). An "
            "attached-but-idle hook must not move the MMIO sync phase.\n",
            hooked.first, hooked.second, got);
        return 1;
    }

    std::printf("mockingboard_t1_irq_phase OK: $C404 read reflects the "
                "access data cycle (got $%02X), and an attached-but-idle "
                "debug hook (%d instructions seen) does not move it\n",
                got, idle.calls);
    return 0;
}
