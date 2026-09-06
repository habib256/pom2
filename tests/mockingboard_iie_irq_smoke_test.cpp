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

// Mockingboard IRQ delivery through the actual Apple //e Enhanced
// Monitor ROM — pins the same path Ultima IV / Nox Archaist / every
// IRQ-driven music driver runs.
//
// Why the existing `mockingboard_irq_delivery_smoke_test` is not enough:
// it hand-wires $FFFE/$FFFF to a tiny trampoline and runs against a
// bare Memory. Real games run with `apple2e.rom` loaded. The //e
// Monitor IRQ entry at $C3FA does several things our simpler harness
// skips, including `STA $C007` (SETINTCXROM) which routes the
// $C100-$CFFF read window to motherboard internal ROM. After that
// write, *reading* a Mockingboard register returns internal ROM bytes,
// not VIA state. Music drivers that ack T1 via a read of $C404 break
// silently. Drivers that ack via *writing* $7F to $C40D (clearing IFR
// bits 0..6) keep working — writes ignore INTCXROM in our model
// (`Memory.cpp:1050-1066`).
//
// This test boots the //e ROM cold, installs a tiny user IRQ handler
// at $03FE/$03FF that:
//   1. Writes $7F → $C40D (clear all VIA #1 interrupt flags — the
//      Nox Archaist ack pattern, found at HDV file offset 0x00B6F0).
//   2. Bumps a counter byte in zero page.
//   3. RTIs.
//
// Then it programs T1 in continuous mode and runs the CPU long enough
// to see multiple IRQs reach the user handler. Skips silently if the
// IIe ROM isn't present.

#include "Memory.h"
#include "M6502.h"
#include "Mockingboard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kMockSlot       = 4;
constexpr uint16_t kHandlerPC = 0x0300;   // DOS scratch — RAM, no ROM
constexpr uint16_t kIdlePC    = 0x03A0;   // somewhere clear of the handler
constexpr uint16_t kCounterZp = 0x06;

bool loadIIeRom(Memory& mem)
{
    static const char* candidates[] = {
        "../../roms/apple2e.rom",
        "../roms/apple2e.rom",
        "roms/apple2e.rom",
    };
    mem.setIIEMode(true);
    for (const char* p : candidates) {
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        if (mem.loadAppleIIRom(p)) return true;
    }
    return false;
}

int main_impl()
{
    Memory mem;
    if (!loadIIeRom(mem)) {
        std::fprintf(stderr,
            "skip: apple2e.rom not found — this test needs the //e "
            "Enhanced ROM to exercise the real Monitor IRQ entry path\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    M6502 cpu(&mem);
    mem.setCpu(&cpu);   // installs SlotBus IRQ router

    auto card = std::make_unique<MockingboardCard>(kMockSlot);
    card->setCpu(&cpu); // lazy-sync back-channel for VIA timers
    MockingboardCard* cardPtr = card.get();
    mem.slotBus().plug(kMockSlot, std::move(card));
    cardPtr->onReset();

    // ─── User IRQ handler at $0300 ───────────────────────────────────
    // The //e Monitor IRQ entry at $C3FA eventually chains to user
    // code via $03FE/$03FF. The handler must:
    //   - PHA (preserve A)
    //   - Write $7F → $C40D (clear all VIA #1 IFR bits — this is the
    //     write-side ack; reads of $C404 would land in internal ROM
    //     because the Monitor entry forced INTCXROM=on at $C3FD).
    //   - INC $06 (telemetry counter)
    //   - PLA (restore A)
    //   - RTI
    const uint8_t handler[] = {
        0x48,                     // PHA
        0xA9, 0x7F,               // LDA #$7F
        0x8D, 0x0D, 0xC4,         // STA $C40D — clear VIA #1 IFR bits
        0xE6, kCounterZp,         // INC $06
        0x68,                     // PLA
        0x40,                     // RTI
    };
    for (size_t i = 0; i < sizeof(handler); ++i) {
        mem.memWrite(static_cast<uint16_t>(kHandlerPC + i), handler[i]);
    }

    // $03FE/$03FF — the //e Monitor IRQ continuation vector (also
    // ProDOS interrupt chain entry).
    mem.memWrite(0x03FE, static_cast<uint8_t>(kHandlerPC & 0xFF));
    mem.memWrite(0x03FF, static_cast<uint8_t>(kHandlerPC >> 8));

    // ─── Idle loop at $03A0: CLI ; JMP self ──────────────────────────
    // CLI clears the I flag so IRQs from T1 are delivered. JMP $03A1
    // is the tight spin (3 cycles/iter).
    mem.memWrite(kIdlePC + 0, 0x58);                                 // CLI
    mem.memWrite(kIdlePC + 1, 0x4C);                                 // JMP
    mem.memWrite(kIdlePC + 2, static_cast<uint8_t>((kIdlePC + 1) & 0xFF));
    mem.memWrite(kIdlePC + 3, static_cast<uint8_t>((kIdlePC + 1) >> 8));

    // ─── Mockingboard T1 setup ───────────────────────────────────────
    // T1 latch = 200 cycles, ACR = continuous, IER enable T1. Reaches
    // the VIA via memWrite → slot bus.
    mem.memWrite(0xC406, 200);    // T1LL
    mem.memWrite(0xC407, 0);      // T1LH  (also reloads counter)
    mem.memWrite(0xC40B, 0x40);   // ACR = continuous, no PB7
    mem.memWrite(0xC405, 0);      // T1CH  (load + start latch=200)
    mem.memWrite(0xC40E, 0xC0);   // IER  = set bit 7+6 → enable T1 IRQ

    mem.memWrite(kCounterZp, 0x00);
    cpu.hardReset();
    cpu.setProgramCounter(kIdlePC);

    // Step long enough for multiple T1 underflows. At 200-cycle latch
    // and ≈3 cycles/JMP, expect 1 IRQ every ~67 steps. 4000 steps →
    // ~60 IRQs. Generous lower bound: ≥ 5.
    const int kSteps = 4000;
    for (int i = 0; i < kSteps; ++i) cpu.step();

    const uint8_t irqCount = mem.memRead(kCounterZp);
    if (irqCount < 5) {
        std::fprintf(stderr,
            "iie IRQ delivery: counter = %u after %d steps "
            "(≈%d cycles); expected ≥5 IRQs through the //e Monitor "
            "→ $03FE chain to the user handler. IRQ-driven music "
            "(Ultima IV, Nox Archaist) cannot work without this path.\n",
            irqCount, kSteps, kSteps * 3);
        return 1;
    }
    std::printf("mockingboard_iie_irq_smoke OK: %u IRQs delivered "
                "through //e ROM in %d CPU steps\n", irqCount, kSteps);
    return 0;
}

}  // namespace

int main() { return main_impl(); }
