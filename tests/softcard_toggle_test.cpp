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

// POM2 — Microsoft SoftCard (Z80) smoke test.
//
// Pins the MAME a2softcard.cpp port (see SoftCardZ80.h for line refs):
//  1. dma_r/dma_w window arithmetic — all six Z80→6502 windows incl.
//     boundaries (LC remap, I/O window, zero-page wrap).
//  2. write_cnxx toggle protocol: $CnXX WRITE grants/releases the bus,
//     reads don't; first grant after reset resets the Z80 (FirstZ80Boot),
//     later grants resume in place (WAIT semantics); the granting 6502
//     yields its run() chunk at the instruction boundary.
//  3. Z80 code executing out of translated Apple RAM, writing Apple RAM,
//     and releasing the bus itself through the $E000 I/O window.
//  4. Snapshot blob round-trip (SLOTn payload).
//  5. EmulationController integration: a tickFrame() slice hands the
//     budget 6502 → Z80 → 6502 within one frame (runCpuSlice DMA path).
//
// Gate for SoftCardZ80.cpp, the SlotPeripheral DMA hook and
// EmulationController::runCpuSlice.

#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "SoftCardZ80.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

void testXlate()
{
    // MAME a2softcard.cpp:111-176 — six windows, both directions checked
    // at the edges.
    assert(SoftCardZ80::xlate(0x0000) == 0x1000);
    assert(SoftCardZ80::xlate(0xAFFF) == 0xBFFF);
    assert(SoftCardZ80::xlate(0xB000) == 0xD000);   // LC bank
    assert(SoftCardZ80::xlate(0xBFFF) == 0xDFFF);
    assert(SoftCardZ80::xlate(0xC000) == 0xE000);
    assert(SoftCardZ80::xlate(0xCFFF) == 0xEFFF);
    assert(SoftCardZ80::xlate(0xD000) == 0xF000);
    assert(SoftCardZ80::xlate(0xDFFF) == 0xFFFF);
    assert(SoftCardZ80::xlate(0xE000) == 0xC000);   // I/O space
    assert(SoftCardZ80::xlate(0xE400) == 0xC400);   // slot 4 ROM window
    assert(SoftCardZ80::xlate(0xEFFF) == 0xCFFF);
    assert(SoftCardZ80::xlate(0xF000) == 0x0000);   // zero page
    assert(SoftCardZ80::xlate(0xFFFF) == 0x0FFF);
}

void testToggleAndResume()
{
    Memory mem;
    M6502 cpu(&mem);

    auto owned = std::make_unique<SoftCardZ80>();
    SoftCardZ80* card = owned.get();
    card->setMemory(&mem);
    card->setCpu(&cpu);
    mem.slotBus().plug(4, std::move(owned));

    // Z80 program, segment 1 (Z80 $0000 = 6502 $1000):
    //   LD A,$5A ; LD ($7000),A  → 6502 $8000
    //   LD ($E400),A             → 6502 $C400 = release the bus
    const uint8_t z80Seg1[] = { 0x3E, 0x5A, 0x32, 0x00, 0x70,
                                0x32, 0x00, 0xE4 };
    // Segment 2 (Z80 $0008 — resume point after seg 1's release):
    //   LD A,$77 ; LD ($7001),A  → 6502 $8001
    //   LD ($F050),A             → 6502 $0050 (zero-page window)
    //   LD ($E400),A             → release again
    const uint8_t z80Seg2[] = { 0x3E, 0x77, 0x32, 0x01, 0x70,
                                0x32, 0x50, 0xF0, 0x32, 0x00, 0xE4 };
    uint16_t a = 0x1000;
    for (uint8_t b : z80Seg1) mem.memWrite(a++, b);
    for (uint8_t b : z80Seg2) mem.memWrite(a++, b);

    // A $CnXX READ must NOT toggle (MAME implements write_cnxx only).
    (void)mem.memRead(0xC400);
    assert(!card->dmaActive());
    assert(card->firstBootPending());

    // 6502 grants the bus: STA $C400 at $0300. The card halts the CPU at
    // that instruction boundary, so run(4096) returns after ~10 cycles
    // (LDA #$00 + STA abs) instead of burning the chunk.
    mem.memWrite(0x0300, 0xA9); mem.memWrite(0x0301, 0x00);   // LDA #$00
    mem.memWrite(0x0302, 0x8D); mem.memWrite(0x0303, 0x00);   // STA $C400
    mem.memWrite(0x0304, 0xC4);
    cpu.setProgramCounter(0x0300);
    const int spent6502 = cpu.run(4096);
    assert(card->dmaActive());
    assert(!card->firstBootPending());     // first grant reset the Z80
    assert(spent6502 < 4096);              // yielded mid-chunk
    assert(cpu.getProgramCounter() == 0x0305);

    // Z80 runs segment 1 out of translated RAM and releases the bus.
    // The parked 6502 still holds the granting STA's in-flight count (4),
    // which every bus timestamp adds; during the Z80's tenure it must be 0.
    assert(cpu.getCurrentInstructionCycles() > 0);
    const int z1 = card->dmaRun(4096);
    assert(cpu.getCurrentInstructionCycles() == 0);
    assert(!card->dmaActive());
    assert(mem.memRead(0x8000) == 0x5A);
    // seg 1 = 7+13+13 = 33 T-states = 16.5 → 16 cycles + carry.
    assert(z1 >= 16 && z1 <= 17);
    assert(card->z80().getPC() == 0x0008);

    // Second grant: NO reset — the Z80 resumes at $0008 (WAIT released).
    cpu.setProgramCounter(0x0300);
    cpu.run(4096);
    assert(card->dmaActive());
    (void)card->dmaRun(4096);
    assert(!card->dmaActive());
    assert(mem.memRead(0x8001) == 0x77);
    assert(mem.memRead(0x0050) == 0x77);   // zero-page window write

    // Snapshot round-trip: capture, corrupt live state, restore.
    std::vector<uint8_t> blob;
    card->appendSnapshotState(blob);
    assert(!blob.empty());
    cpu.setProgramCounter(0x0300);
    cpu.run(4096);                          // re-grant (mutates card state)
    assert(card->dmaActive());
    card->loadSnapshotState(blob.data(), blob.size());
    assert(!card->dmaActive());             // restored to released state
    assert(card->z80().getPC() == 0x0013);  // end of segment 2

    // Foreign blob must be ignored.
    const uint8_t junk[8] = { 'X', 'Y', 'Z', 'W', 0, 1, 2, 3 };
    card->loadSnapshotState(junk, sizeof(junk));
    assert(card->z80().getPC() == 0x0013);

    // Bus RESET: disarm + re-latch first boot (MAME reset_from_bus).
    mem.slotBus().reset();
    assert(!card->dmaActive());
    assert(card->firstBootPending());
    assert(card->z80().getPC() == 0x0000);
}

void testControllerArbitration()
{
    // Full loop through EmulationController::tickFrame — one frame's
    // budget must flow 6502 → Z80 → 6502 via runCpuSlice's DMA path.
    EmulationController ctl;
    Memory& mem = ctl.memory();

    auto owned = std::make_unique<SoftCardZ80>();
    SoftCardZ80* card = owned.get();
    card->setMemory(&mem);
    card->setCpu(&ctl.cpu());
    mem.slotBus().plug(4, std::move(owned));

    // Z80 (first boot → PC $0000 = 6502 $1000):
    //   LD A,$33 ; LD ($6000),A → 6502 $7000 ; LD ($E400),A → release
    const uint8_t z80Prog[] = { 0x3E, 0x33, 0x32, 0x00, 0x60,
                                0x32, 0x00, 0xE4 };
    uint16_t a = 0x1000;
    for (uint8_t b : z80Prog) mem.memWrite(a++, b);

    // 6502 at $0300:
    //   LDA #$11 ; STA $6000     (before hand-over)
    //   STA $C400                (grant)
    //   LDA #$22 ; STA $6001     (after hand-back)
    //   JMP *                    (burn the rest of the frame)
    const uint8_t prog6502[] = {
        0xA9, 0x11, 0x8D, 0x00, 0x60,
        0x8D, 0x00, 0xC4,
        0xA9, 0x22, 0x8D, 0x01, 0x60,
        0x4C, 0x0D, 0x03,
    };
    a = 0x0300;
    for (uint8_t b : prog6502) mem.memWrite(a++, b);

    ctl.cpu().setProgramCounter(0x0300);
    ctl.setMode(EmulationController::Mode::Running);
    ctl.tickFrame();                 // single-threaded frame (WASM path)
    ctl.setMode(EmulationController::Mode::Stopped);

    assert(mem.memRead(0x6000) == 0x11);   // 6502 pre-grant write
    assert(mem.memRead(0x7000) == 0x33);   // Z80 ran inside the frame
    assert(mem.memRead(0x6001) == 0x22);   // 6502 resumed after release
    assert(!card->dmaActive());
    assert(ctl.cpu().getProgramCounter() == 0x030D);   // parked on JMP *
}

} // namespace

int main()
{
    testXlate();
    testToggleAndResume();
    testControllerArbitration();
    printf("softcard_toggle_test: all assertions passed\n");
    return 0;
}
