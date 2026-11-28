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

// memory_heatmap — UI-022: the recording (MemoryHeatmap), the diversion that
// feeds it (Memory::setHeatSink) and the view that paints it
// (MemoryHeatmapView).
//
//  1. The acceptance criterion: a program that clears HGR page 1 lights
//     exactly $2000-$3FFF as written, in the bank RAMWRT selects — MAIN
//     without it, AUX with it — and the program's own bytes as executed,
//     not read.
//  2. Every bank apart, in its physical layout: the language card's $D000
//     bank 1 in the $C0 rows, bank 2 in the $D0 rows, ALTZP's in AUX, the
//     ROM and I/O space in its own plane, RamWorks banks in theirs (and
//     their bytes readable without paging them in).
//  3. Who: the debugger's own pokes are not recorded; a card's processor
//     holding the bus is DMA; counts and the interrupt load.
//  4. Locate / cpuAddress are inverses (what a range breakpoint relies on).
//  5. Detached, nothing is left; a time jump back clears; shared with the
//     watchpoints; a frozen copy does not follow the live one.
//  6. The view: FT's background and colours (its exact palette and POM2's),
//     the fade, the rails for the pages on screen, the regions, the layout's
//     hit test and zoom.
//  7. French Touch's monitoring marks ($C067/$C066 a section, $C069/$C068
//     an IRQ span): a 6502 program times itself to the exact cycle; a READ
//     of $C066 does nothing; FT's bars and their scales.
//  8. Max-pooling: an isolated access survives any downscale, per kind.
//  9. Where a byte is on the screen: the inverse of Memory's text and
//     hi-res row offsets, holes included.

#include "M6502.h"
#include "Memory.h"
#include "MemoryHeatmap.h"
#include "MemoryHeatmapView.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using pom2::HeatData;
using pom2::MemoryHeatmap;
namespace hv = pom2::heatview;

// A program at $0300: clear $2000-$3FFF (LDA #0, a 32-page STA ($06),Y
// loop), then park on itself.
//   0300 A9 00     LDA #$00
//   0302 85 06     STA $06
//   0304 A9 20     LDA #$20
//   0306 85 07     STA $07
//   0308 A2 20     LDX #$20
//   030A A0 00     LDY #$00
//   030C A9 00     LDA #$00
//   030E 91 06     STA ($06),Y
//   0310 C8        INY
//   0311 D0 FB     BNE $030E
//   0313 E6 07     INC $07
//   0315 CA        DEX
//   0316 D0 F6     BNE $030E
//   0318 4C 18 03  JMP $0318
const uint8_t kProgram[] = {
    0xA9, 0x00, 0x85, 0x06, 0xA9, 0x20, 0x85, 0x07, 0xA2, 0x20, 0xA0, 0x00,
    0xA9, 0x00, 0x91, 0x06, 0xC8, 0xD0, 0xFB, 0xE6, 0x07, 0xCA, 0xD0, 0xF6,
    0x4C, 0x18, 0x03,
};
constexpr uint16_t kOrg = 0x0300, kEnd = 0x0318;

void load(Memory& mem)
{
    for (unsigned i = 0; i < sizeof kProgram; ++i)
        mem.memWrite(static_cast<uint16_t>(kOrg + i), kProgram[i]);
    for (uint32_t a = 0x2000; a < 0x4000; ++a) mem.memWrite(static_cast<uint16_t>(a), 0xFF);
}

// Runs the program with the heatmap attached, feeding it each instruction
// the way the Debugger's hook does.
void run(const Memory& mem, M6502& cpu, MemoryHeatmap& heat)
{
    cpu.setProgramCounter(kOrg);
    for (int guard = 0; guard < 200000; ++guard) {
        const uint16_t pc = cpu.getProgramCounter();
        if (pc == kEnd) break;
        heat.noteHeatExec(pc, mem.debugBank(pc));
        cpu.step();
    }
    assert(cpu.getProgramCounter() == kEnd && "the clear loop must finish");
}

uint32_t stampOf(const HeatData& h, int plane, int kind, uint16_t phys)
{
    return h.byteInfo(plane, phys).stamp[static_cast<std::size_t>(kind)];
}

uint32_t countOf(const HeatData& h, int plane, int kind, uint16_t phys)
{
    return h.byteInfo(plane, phys).count[static_cast<std::size_t>(kind)];
}

void checkCleared(MemoryHeatmap& heat, int plane, const char* what)
{
    HeatData::PlaneCopy p, other;
    heat.copyPlane(plane, false, 0, 0xFFFF, p);
    heat.copyPlane(plane == HeatData::kMain ? HeatData::kAux0 : HeatData::kMain, false, 0, 0xFFFF, other);
    const auto& w = p.v[HeatData::Write];
    const auto& wo = other.v[HeatData::Write];
    for (uint32_t a = 0x2000; a < 0x4000; ++a) {
        if (!w[a] || wo[a]) {
            std::printf("FAIL %s: $%04X write stamp %u (other plane %u)\n", what, a, w[a], wo[a]);
            assert(false);
        }
    }
    // Nothing else was written but the two pointer bytes $06/$07.
    for (uint32_t a = 0; a < 0x10000; ++a) {
        if (a >= 0x2000 && a < 0x4000) continue;
        const bool expected = plane == HeatData::kMain && (a == 0x06 || a == 0x07);
        if ((w[a] != 0) != expected && !(a >= kOrg && a < kOrg + sizeof kProgram)) {
            std::printf("FAIL %s: $%04X write stamp %u\n", what, a, w[a]);
            assert(false);
        }
    }
    // The program's bytes are execution, not data reads (main plane: RAMRD off).
    for (uint16_t a = kOrg; a < kEnd; ++a) {     // the parking JMP never runs
        assert(stampOf(heat, HeatData::kMain, HeatData::Exec, a) != 0 && "every program byte is executed");
        assert(stampOf(heat, HeatData::kMain, HeatData::Read, a) == 0 && "an instruction's own fetch is not a data read");
    }
    // The pointer is read by STA ($06),Y — data, with the instruction named.
    const auto b = heat.byteInfo(HeatData::kMain, 0x06);
    assert(b.stamp[HeatData::Read] != 0 && b.readPc == 0x030E);
    assert(b.readPcBank.kind == pom2::DebugBank::Main);
    // The last write to $3FFF was made by STA ($06),Y; 32 pages x 256 = the
    // loop wrote each byte once, so each count is 1.
    assert(heat.byteInfo(plane, 0x3FFF).writePc == 0x030E);
    assert(countOf(heat, plane, HeatData::Write, 0x2345) == 1);
    // The loop body ran 8192 times: its STA's bytes were executed as often.
    assert(countOf(heat, HeatData::kMain, HeatData::Exec, 0x030E) == 8192);
}

void acceptance()
{
    for (int ramwrt = 0; ramwrt < 2; ++ramwrt) {
        Memory mem;
        mem.setIIEMode(true);
        M6502 cpu(&mem);
        load(mem);
        if (ramwrt) mem.memWrite(0xC005, 0);   // RAMWRT on: $0200-$BFFF writes to aux
        MemoryHeatmap heat(mem, cpu);
        mem.setHeatSink(&heat);
        assert(mem.heatArmed());
        run(mem, cpu, heat);
        if (ramwrt) {
            // Zero page ($06/$07) follows ALTZP, not RAMWRT: it stays MAIN.
            checkCleared(heat, HeatData::kAux0, "RAMWRT: aux");
            assert(mem.auxData()[0x2000] == 0 && mem.data()[0x2000] == 0xFF);
        } else {
            checkCleared(heat, HeatData::kMain, "main");
            assert(mem.data()[0x2000] == 0);
        }
        mem.setHeatSink(nullptr);
        std::printf("  clear HGR1 lights $2000-$3FFF in %s OK\n", ramwrt ? "AUX" : "MAIN");
    }
}

// ── 2. Every bank apart ────────────────────────────────────────────────────

void banksApart()
{
    Memory mem;
    mem.setIIEMode(true);
    M6502 cpu(&mem);
    MemoryHeatmap heat(mem, cpu);
    mem.setHeatSink(&heat);
    // The language card: $C08B twice = bank 1, read and write RAM.
    (void)mem.memRead(0xC08B);
    (void)mem.memRead(0xC08B);
    assert(stampOf(heat, HeatData::kRom, HeatData::Read, 0xC08B) != 0 && "a soft switch is in the I/O plane");
    mem.memWrite(0xD123, 0x11);
    assert(stampOf(heat, HeatData::kMain, HeatData::Write, 0xC123) != 0 && "bank 1 of $D000 in the $C0 rows");
    assert(stampOf(heat, HeatData::kMain, HeatData::Write, 0xD123) == 0);
    assert(mem.lcPhysicalPeek(0xC123) == 0x11);
    // $C083 twice = bank 2.
    (void)mem.memRead(0xC083);
    (void)mem.memRead(0xC083);
    mem.memWrite(0xD123, 0x22);
    assert(stampOf(heat, HeatData::kMain, HeatData::Write, 0xD123) != 0 && "bank 2 of $D000 in the $D0 rows");
    assert(mem.lcPhysicalPeek(0xD123) == 0x22 && mem.lcPhysicalPeek(0xC123) == 0x11);
    mem.memWrite(0xE456, 0x33);
    assert(stampOf(heat, HeatData::kMain, HeatData::Write, 0xE456) != 0);
    // ALTZP: the language card of AUX.
    mem.memWrite(0xC009, 0);
    mem.memWrite(0xD123, 0x44);
    assert(stampOf(heat, HeatData::kAux0, HeatData::Write, 0xD123) != 0 && "ALTZP: the aux language card");
    assert(mem.auxPhysicalPeek(0, 0xD123) == 0x44);
    mem.memWrite(0xC008, 0);
    // ROM: the language card reading ROM ($C082).
    (void)mem.memRead(0xC082);
    (void)mem.memRead(0xF800);
    assert(stampOf(heat, HeatData::kRom, HeatData::Read, 0xF800) != 0 && "ROM in the ROM plane");
    assert(stampOf(heat, HeatData::kMain, HeatData::Read, 0xF800) == 0);
    std::printf("  LC banks 1 / 2, aux LC, ROM and I/O each in their place OK\n");

    // RamWorks: bank 2 of 4, by $C073, in its own plane; its bytes are
    // read back from the backing store once another bank is selected.
    Memory rw;
    rw.setIIEMode(true);
    rw.setRamWorksBanks(4);
    M6502 cpu2(&rw);
    MemoryHeatmap heat2(rw, cpu2);
    rw.setHeatSink(&heat2);
    rw.memWrite(0xC073, 2);
    rw.memWrite(0xC005, 0);              // RAMWRT
    rw.memWrite(0x1234, 0x5A);
    assert(heat2.hasPlane(HeatData::auxPlane(2)) && "a RamWorks plane is made on its first access");
    assert(stampOf(heat2, HeatData::auxPlane(2), HeatData::Write, 0x1234) != 0);
    assert(stampOf(heat2, HeatData::kAux0, HeatData::Write, 0x1234) == 0);
    assert(rw.auxPhysicalPeek(2, 0x1234) == 0x5A);
    rw.memWrite(0xC073, 0);
    assert(rw.auxPhysicalPeek(2, 0x1234) == 0x5A && "bank 2 read from the backing store");
    assert(rw.auxPhysicalPeek(0, 0x1234) == 0 && "bank 0 never written");
    rw.setHeatSink(nullptr);
    std::printf("  RamWorks bank 2 in its own plane OK\n");
    mem.setHeatSink(nullptr);
}

// ── 3. Who ─────────────────────────────────────────────────────────────────

void who()
{
    Memory mem;
    mem.setIIEMode(true);
    M6502 cpu(&mem);
    MemoryHeatmap heat(mem, cpu);
    mem.setHeatSink(&heat);
    // The debugger's own pokes (Debugger::patchMemory) are not the program's.
    mem.setUnattributedAccess(true);
    mem.memWrite(0x3000, 1);
    (void)mem.memRead(0x3001);
    mem.setUnattributedAccess(false);
    assert(stampOf(heat, HeatData::kMain, HeatData::Write, 0x3000) == 0);
    assert(stampOf(heat, HeatData::kMain, HeatData::Read, 0x3001) == 0);
    // A card's processor holding the bus: DMA, not the parked 6502.
    heat.noteHeatBusMaster(true);
    (void)mem.memRead(0x4000);
    mem.memWrite(0x4001, 7);
    heat.noteHeatBusMaster(false);
    assert(stampOf(heat, HeatData::kMain, HeatData::Dma, 0x4000) != 0);
    assert(stampOf(heat, HeatData::kMain, HeatData::Dma, 0x4001) != 0);
    assert(stampOf(heat, HeatData::kMain, HeatData::Read, 0x4000) == 0);
    assert(stampOf(heat, HeatData::kMain, HeatData::Write, 0x4001) == 0);
    // Counts.
    for (int i = 0; i < 3; ++i) mem.memWrite(0x5000, static_cast<uint8_t>(i));
    assert(countOf(heat, HeatData::kMain, HeatData::Write, 0x5000) == 3);
    std::printf("  debugger pokes left out, DMA apart, counts OK\n");

    // The interrupt load: frame 0 = 100 cycles of which 30 in a handler.
    heat.noteHeatRetired(0xFF, 70, 0);
    heat.noteHeatInterrupt(0xFC, 7, 0);
    heat.noteHeatRetired(0xFC, 20, 0);
    heat.noteHeatRetired(0xFF, 3, 0);     // the RTI: SP back, the handler closes
    heat.noteHeatRetired(0xFF, 10, 1);    // frame 1 begins: frame 0 is complete
    const auto frames = heat.interruptLoad().frames();
    assert(frames.size() == 1 && frames[0].cycles == 100 && frames[0].interruptCycles == 30);
    std::printf("  interrupt load per frame OK\n");
    mem.setHeatSink(nullptr);
}

// ── 4. Locate and cpuAddress ───────────────────────────────────────────────

void inverses()
{
    for (int plane : {int{HeatData::kMain}, int{HeatData::kAux0}, HeatData::auxPlane(5)}) {
        for (uint32_t phys = 0; phys < 0x10000; phys += 0x37) {
            const pom2::DebugAddress at = HeatData::cpuAddress(plane, static_cast<uint16_t>(phys));
            const HeatData::Cell c = HeatData::locate(at.addr, at.bank);
            if (c.plane != plane || c.phys != phys) {
                std::printf("FAIL plane %d phys $%04X -> %s -> plane %d $%04X\n", plane, phys,
                            at.text().c_str(), c.plane, c.phys);
                assert(false);
            }
        }
    }
    const pom2::DebugAddress lc1 = HeatData::cpuAddress(HeatData::kMain, 0xC123);
    assert(lc1.addr == 0xD123 && lc1.bank.kind == pom2::DebugBank::Language && lc1.bank.segment == 1);
    const pom2::DebugAddress rom = HeatData::cpuAddress(HeatData::kRom, 0xF800);
    assert(rom.addr == 0xF800 && rom.bank.kind == pom2::DebugBank::Any);
    pom2::DebugBank slot6;
    slot6.kind = pom2::DebugBank::Slot;
    slot6.index = 6;
    assert(HeatData::locate(0xC600, slot6).plane == HeatData::kRom);
    pom2::DebugBank ignored;
    ignored.kind = pom2::DebugBank::Ignored;
    assert(HeatData::locate(0xD000, ignored).plane < 0);
    std::printf("  locate / cpuAddress inverses OK\n");
}

// ── 5. Lifetime ────────────────────────────────────────────────────────────

void lifetime()
{
    {
        Memory mem;
        mem.setIIEMode(true);
        M6502 cpu(&mem);
        MemoryHeatmap heat(mem, cpu);
        mem.setHeatSink(&heat);
        mem.setHeatSink(nullptr);
        assert(!mem.heatArmed() && mem.readWatchCount() == 0);
        mem.memWrite(0x1234, 0x5A);
        assert(stampOf(heat, HeatData::kMain, HeatData::Write, 0x1234) == 0 && mem.data()[0x1234] == 0x5A);
        for (uint32_t a = 0; a < 0xC000; ++a)
            assert(mem.ramWritable(static_cast<uint16_t>(a)));
        std::printf("  detached: nothing recorded, RAM writable OK\n");
        // A time jump back (rewind, snapshot load) clears the map on the
        // next access rather than leaving stamps from the abandoned future.
        mem.setCycleCounter(1u << 20);
        mem.setHeatSink(&heat);
        mem.memWrite(0x1000, 1);
        const HeatData frozen = heat.freeze();
        mem.setCycleCounter(0);
        mem.memWrite(0x1001, 2);
        assert(stampOf(heat, HeatData::kMain, HeatData::Write, 0x1000) == 0);
        const uint32_t s = stampOf(heat, HeatData::kMain, HeatData::Write, 0x1001);
        assert(s != 0 && s <= heat.now());
        // The frozen copy kept its picture.
        assert(stampOf(frozen, HeatData::kMain, HeatData::Write, 0x1000) != 0);
        assert(stampOf(frozen, HeatData::kMain, HeatData::Write, 0x1001) == 0);
        mem.setHeatSink(nullptr);
        std::printf("  a time jump back clears the map, a frozen copy stays OK\n");
    }
    {
        struct Sink : pom2::MemoryWatchSink {
            int hits = 0;
            void noteAccess(uint16_t, uint8_t, bool) override { ++hits; }
        } sink;
        Memory mem;
        M6502 cpu(&mem);
        mem.setWatchSink(&sink);
        MemoryHeatmap heat(mem, cpu);
        mem.setHeatSink(&heat);
        mem.setWriteWatch(0x2000, true);
        mem.setReadWatch(0x3000, true);
        mem.setHeatSink(nullptr);
        assert(mem.hasWriteWatch(0x2000) && mem.ramWritable(0x2000));
        mem.memWrite(0x2000, 1);
        assert(sink.hits == 1 && mem.data()[0x2000] == 1);
        (void)mem.memRead(0x3000);
        assert(sink.hits == 2);
        mem.setHeatSink(&heat);
        mem.memWrite(0x2000, 2);
        assert(sink.hits == 3 && mem.data()[0x2000] == 2);
        mem.clearWriteWatches();
        mem.clearReadWatches();
        mem.memWrite(0x2000, 3);
        assert(stampOf(heat, HeatData::kMain, HeatData::Write, 0x2000) != 0 && sink.hits == 3 &&
               mem.data()[0x2000] == 3);
        mem.setHeatSink(nullptr);
        for (uint32_t a = 0; a < 0xC000; ++a)
            assert(mem.ramWritable(static_cast<uint16_t>(a)));
        std::printf("  shared with write / read watches OK\n");
    }
}

// ── 6. The view ────────────────────────────────────────────────────────────

uint8_t red(uint32_t c) { return static_cast<uint8_t>(c); }
uint8_t green(uint32_t c) { return static_cast<uint8_t>(c >> 8); }
uint8_t blue(uint32_t c) { return static_cast<uint8_t>(c >> 16); }

void view()
{
    // FT's background: (7 + bits) << 2, and 0 for $00.
    assert(hv::bitsLuma(0x00) == 0);
    assert(hv::bitsLuma(0x01) == 32);
    assert(hv::bitsLuma(0xFF) == 60);

    hv::Style style;
    style.frameCycles = 17030;
    style.fadeFrames = 10;
    style.trail = false;
    style.palette = hv::Palette::Ft;   // FT's own palette, exactly
    const uint32_t horizon = 10u * 17030u >> HeatData::kStampShift;
    const uint32_t now = 100000;
    uint32_t v[HeatData::kKinds] = {0, 0, 0, 0};
    // Untouched: the background only.
    uint32_t c = hv::cellColour(v, now, 0xFF, style, 0);
    assert(red(c) == 60 && green(c) == 60 && blue(c) == 60);
    // Just written: red at full, over a $00 byte.
    v[HeatData::Write] = now;
    c = hv::cellColour(v, now, 0x00, style, 0);
    assert(red(c) == 255 && green(c) < 80 && blue(c) < 80);
    // Halfway through the fade: dimmer, still there; past it: gone.
    v[HeatData::Write] = now - horizon / 2;
    const uint8_t half = red(hv::cellColour(v, now, 0x00, style, 0));
    assert(half > 100 && half < 200);
    v[HeatData::Write] = now - horizon - 1;
    assert(red(hv::cellColour(v, now, 0x00, style, 0)) == 0);
    // ...unless the trail keeps a tint of it.
    style.trail = true;
    assert(red(hv::cellColour(v, now, 0x00, style, 0)) > 0);
    // Read is blue, execute green; a hidden kind draws nothing.
    v[HeatData::Write] = 0;
    v[HeatData::Read] = now;
    c = hv::cellColour(v, now, 0x00, style, 0);
    assert(blue(c) > red(c) && blue(c) > 200);
    style.show[HeatData::Read] = false;
    assert(hv::cellColour(v, now, 0x00, style, 0) == 0xFF000000u);
    style.show[HeatData::Read] = true;
    // Cumulative: the most-hit byte at full, a byte hit once dimmer.
    style.cumulative = true;
    const float logMax = std::log2(1001.0f);
    assert(hv::intensity(1000, 0, style, logMax) > 0.99f);
    assert(hv::intensity(1, 0, style, logMax) < 0.5f && hv::intensity(1, 0, style, logMax) > 0.0f);
    assert(hv::intensity(0, 0, style, logMax) == 0.0f);
    // FT's formula, bit for bit: r = W | bg, g = X | (B >> 1) | bg, b = B | bg.
    style.cumulative = false;
    hv::HeatCell cell;
    cell.k[HeatData::Write] = 200;
    cell.bg = 32;
    c = hv::compose(cell, style);
    assert(red(c) == (200 | 32) && green(c) == 32 && blue(c) == 32);
    cell = {};
    cell.k[HeatData::Read] = 200;
    cell.k[HeatData::Exec] = 64;
    c = hv::compose(cell, style);
    assert(red(c) == 0 && green(c) == (64 | (200 >> 1)) && blue(c) == 200);
    // FT's fade is linear: half the horizon, half the level.
    assert(std::fabs(hv::intensity(now - horizon / 2, now, style, 0) - 0.5f) < 0.01f);
    // POM2's palette: the background dimmer than FT's, a fresh write red and
    // brighter than FT's (the flash), two kinds mixing.
    style.palette = hv::Palette::Pom2;
    v[HeatData::Read] = 0;
    c = hv::cellColour(v, now, 0xFF, style, 0);
    assert(red(c) < 60 && blue(c) > red(c) && "a dim, cool background");
    v[HeatData::Write] = now;
    c = hv::cellColour(v, now, 0x00, style, 0);
    assert(red(c) == 255 && green(c) > 80 && green(c) < 160 && "a fresh write flashes");
    v[HeatData::Read] = now;
    c = hv::cellColour(v, now, 0x00, style, 0);
    assert(red(c) > 200 && blue(c) > 200 && "write + read = magenta");
    // The table heatCells fades with gives the levels intensity() does.
    {
        HeatData::PlaneCopy pc;
        for (auto& k : pc.v) k.assign(0x10000, 0u);
        pc.v[HeatData::Write][0x1000] = now;
        pc.v[HeatData::Write][0x1001] = now - horizon / 2;
        pc.v[HeatData::Write][0x1002] = now - horizon - 5;
        pc.v[HeatData::Read][0x1003] = now - 7;
        std::vector<hv::HeatCell> cells;
        hv::heatCells(HeatData::kMain, pc, nullptr, now, style, 0, cells);
        for (uint16_t a = 0x1000; a <= 0x1003; ++a)
            for (int k : {HeatData::Write, HeatData::Read}) {
                const uint32_t st = pc.v[static_cast<std::size_t>(k)][a];
                const int want = st ? static_cast<int>(hv::intensity(st, now, style, 0) * 255.0f + 0.5f) : 0;
                assert(std::abs(cells[a].k[static_cast<std::size_t>(k)] - want) <= 1);
            }
    }
    assert(hv::glowColour(hv::HeatCell{}, style) == 0);
    cell = {};
    cell.k[HeatData::Exec] = 255;
    assert((hv::glowColour(cell, style) >> 24) == 255 && "a fresh access glows");
    cell.k[HeatData::Exec] = 150;
    assert(hv::glowColour(cell, style) == 0 && "an older one does not");
    std::printf("  FT background, colours, fade, trail, cumulative; POM2 palette OK\n");

    // The pages on screen (FT's white rail).
    auto shown = [](const hv::VideoSwitches& s, bool iie, int page, bool aux) {
        hv::Rails m, a;
        hv::markShown(s, iie, m, a);
        return (aux ? a : m).shown[static_cast<std::size_t>(page)];
    };
    hv::VideoSwitches s;                                  // TEXT, page 1
    assert(shown(s, true, 0x04, false) && shown(s, true, 0x07, false) && !shown(s, true, 0x08, false));
    assert(!shown(s, true, 0x04, true));
    s.page2 = true;                                       // page 2
    assert(shown(s, true, 0x08, false) && !shown(s, true, 0x04, false));
    s.store80 = true;                                     // 80STORE: page 1 whatever PAGE2
    assert(shown(s, true, 0x04, false) && !shown(s, true, 0x08, false));
    s = {};
    s.text = false; s.hires = true; s.page2 = true;       // HGR page 2
    assert(shown(s, true, 0x40, false) && shown(s, true, 0x5F, false) && !shown(s, true, 0x20, false));
    assert(!shown(s, true, 0x04, false));
    s.page2 = false; s.col80 = true; s.dhgr = true;       // DHGR page 1: aux too
    assert(shown(s, true, 0x20, true) && shown(s, true, 0x20, false));
    assert(!shown(s, false, 0x20, true) && "no aux on a ][");
    s = {};
    s.col80 = true;                                       // 80 columns
    assert(shown(s, true, 0x04, true));
    std::printf("  pages on screen OK\n");

    // Regions.
    assert(std::strcmp(hv::regionAt(HeatData::kMain, 0x2000)->tag, "HGR1") == 0);
    assert(std::strcmp(hv::regionAt(HeatData::kMain, 0xC123)->tag, "LC1") == 0);
    assert(std::strcmp(hv::regionAt(HeatData::kAux0, 0x0150)->tag, "STK") == 0);
    assert(std::strcmp(hv::regionAt(HeatData::kRom, 0xF800)->tag, "MON") == 0);
    assert(std::strcmp(hv::regionAt(HeatData::kRom, 0xC030)->tag, "I/O") == 0);
    for (int plane : {HeatData::kMain, HeatData::kRom}) {   // the regions tile their plane
        uint32_t next = static_cast<uint32_t>(hv::firstRow(plane)) << 8;
        for (const hv::Region& r : hv::regions(plane)) { assert(r.lo == next); next = r.hi + 1u; }
        assert(next == 0x10000);
    }
    assert(hv::planeTitle(HeatData::auxPlane(3), 8) == "AUX bank 3" && hv::planeTitle(HeatData::kAux0, 1) == "AUX");
    std::printf("  regions OK\n");

    // Layout: $0000 at the bottom (FT), the ROM column only $C0-$FF.
    const hv::Layout l = hv::makeLayout({HeatData::kRom, HeatData::kMain, HeatData::kAux0}, true);
    assert(l.columns.size() == 3 && l.width > 3 * 256.0f);
    int col = -1;
    uint16_t phys = 0;
    const float mainX = l.columns[1].x;
    assert(hv::hit(l, mainX + 0.5f, 255.5f, col, phys) && col == 1 && phys == 0x0000);
    assert(hv::hit(l, mainX + 255.5f, 0.5f, col, phys) && phys == 0xFFFF);
    assert(hv::hit(l, mainX + 0x34 + 0.2f, 255.0f - 0x12 + 0.7f, col, phys) && phys == 0x1234);
    assert(!hv::hit(l, l.columns[0].x + 3.0f, 255.5f, col, phys) && "no ROM below $C000");
    assert(hv::hit(l, l.columns[0].x + 3.0f, 0.5f, col, phys) && col == 0 && phys == 0xFF03);
    assert(!hv::hit(l, mainX + 256.0f + 4.0f, 10.0f, col, phys) && "a rail is not a byte");
    float cx = 0, cy = 0;
    assert(hv::cellOrigin(l, HeatData::kAux0, 0x2A13, cx, cy));
    assert(hv::hit(l, cx + 0.5f, cy + 0.5f, col, phys) && col == 2 && phys == 0x2A13);
    const hv::Layout td = hv::makeLayout({HeatData::kMain}, false);
    assert(hv::hit(td, 0.5f, 0.5f, col, phys) && phys == 0x0000);

    // Transform: fit, zoom about a point, clamp.
    hv::Transform t = hv::fit(l, 900.0f, 300.0f);
    assert(t.scale > 0.0f && t.toScreenX(l.width) <= 900.5f && t.toScreenY(256.0f) <= 300.5f);
    const float sx = 400.0f, sy = 120.0f;
    const float before = t.toCanvasX(sx), beforeY = t.toCanvasY(sy);
    hv::zoomAt(t, l, 900.0f, 300.0f, sx, sy, 8.0f);
    assert(std::fabs(t.toCanvasX(sx) - before) < 0.01f && std::fabs(t.toCanvasY(sy) - beforeY) < 0.01f);
    hv::zoomAt(t, l, 900.0f, 300.0f, sx, sy, 1e-6f);   // never below the fit
    assert(std::fabs(t.scale - hv::fit(l, 900.0f, 300.0f).scale) < 1e-4f);
    hv::centreOn(t, l, 900.0f, 300.0f, cx, cy, 30.0f);
    assert(t.scale == 30.0f && std::fabs(t.toScreenX(cx) - 450.0f) < 0.5f);
    std::printf("  layout, hit test, zoom OK\n");
}

void options()
{
    // The window's choices survive a session as one settings value.
    hv::Options o;
    o.style.cumulative = true;
    o.style.fadeFrames = 333;
    o.style.background = hv::Background::Value;
    o.style.trail = false;
    o.style.show[HeatData::Dma] = false;
    o.showRom = false;
    o.auxChoice = 5;
    o.bottomUp = false;
    o.fill = false;
    o.loupe = false;
    o.style.palette = hv::Palette::Ft;
    o.minimap = false;
    o.glow = false;
    const hv::Options back = hv::parseOptions(hv::optionsText(o));
    assert(hv::optionsText(back) == hv::optionsText(o));
    assert(back.style.cumulative && back.style.fadeFrames == 333 && !back.style.trail);
    assert(back.style.background == hv::Background::Value && !back.style.show[HeatData::Dma]);
    assert(back.style.show[HeatData::Read] && !back.showRom && back.showMain && back.auxChoice == 5);
    assert(!back.bottomUp && !back.fill && !back.loupe && back.rails);
    assert(back.style.palette == hv::Palette::Ft && !back.minimap && !back.glow && back.labels);
    // A string saved before the palette, minimap, labels and glow existed
    // (2026-11-27) still loads, and the new choices take their defaults.
    const hv::Options old = hv::parseOptions(
        "mode=recent fade=40 bg=bits trail=1 show=1101 rom=1 main=1 aux=0 auxbank=-1 "
        "bottomup=1 fill=1 regions=1 rails=1 loupe=1 inspector=0");
    assert(old.style.fadeFrames == 40 && !old.showAux && !old.inspector && !old.style.show[HeatData::Exec]);
    assert(old.style.palette == hv::Palette::Pom2 && old.minimap && old.labels && old.glow);
    // The loupe and the inspector are off by default (owner, 2026-11-28);
    // a string that names them still turns them on.
    assert(!hv::Options{}.loupe && !hv::Options{}.inspector);
    assert(old.loupe);
    // Empty, unknown or out-of-range words keep the defaults.
    const hv::Options d = hv::parseOptions("bogus fade=99999 auxbank=x future=1");
    assert(d.style.fadeFrames == 1500 && d.auxChoice == -1 && d.bottomUp && !d.style.cumulative);
    assert(hv::optionsText(hv::parseOptions("")) == hv::optionsText(hv::Options{}));
    std::printf("  options round-trip OK\n");
}

void hotSpots()
{
    Memory mem;
    M6502 cpu(&mem);
    MemoryHeatmap heat(mem, cpu);
    mem.setHeatSink(&heat);
    for (int i = 0; i < 5; ++i) mem.memWrite(0x0800, 1);
    for (int i = 0; i < 9; ++i) mem.memWrite(0x0900, 1);
    mem.memWrite(0x0A00, 1);
    const auto top = hv::hottest(heat, HeatData::kMain, HeatData::Write, 2);
    assert(top.size() == 2 && top[0].phys == 0x0900 && top[0].count == 9 && top[1].phys == 0x0800);
    mem.setHeatSink(nullptr);
    std::printf("  hottest bytes OK\n");
}

// ── 7. French Touch's monitoring marks ─────────────────────────────────────

// $0300: time a 10-turn DEX loop between $C067 and $C066, then a 3-NOP
// "handler" between $C069 and $C068, then READ $C066, then park.
//   0300 8D 67 C0  STA $C067     start the section
//   0303 A2 0A     LDX #10       2
//   0305 CA        DEX           2 x 10
//   0306 D0 FD     BNE $0305     3 x 9 + 2
//   0308 8D 66 C0  STA $C066     stop: 4 + 2 + 49 = 55 cycles after the start
//   030B 8D 69 C0  STA $C069     start the IRQ span
//   030E EA EA EA  NOP x 3       6
//   0311 8D 68 C0  STA $C068     stop: 4 + 6 = 10
//   0314 AD 66 C0  LDA $C066     a READ: the paddle, no mark
//   0317 4C 17 03  JMP $0317
const uint8_t kMonitorProgram[] = {
    0x8D, 0x67, 0xC0, 0xA2, 0x0A, 0xCA, 0xD0, 0xFD, 0x8D, 0x66, 0xC0, 0x8D,
    0x69, 0xC0, 0xEA, 0xEA, 0xEA, 0x8D, 0x68, 0xC0, 0xAD, 0x66, 0xC0, 0x4C,
    0x17, 0x03,
};

void monitor()
{
    using pom2::CycleMonitor;
    Memory mem;
    mem.setIIEMode(true);
    M6502 cpu(&mem);
    for (unsigned i = 0; i < sizeof kMonitorProgram; ++i)
        mem.memWrite(static_cast<uint16_t>(0x0300 + i), kMonitorProgram[i]);
    const uint8_t paddleBefore = static_cast<uint8_t>(mem.memRead(0xC066) & 0x80);
    MemoryHeatmap heat(mem, cpu);
    mem.setHeatSink(&heat);
    cpu.setProgramCounter(0x0300);
    for (int guard = 0; guard < 1000 && cpu.getProgramCounter() != 0x0317; ++guard) {
        heat.noteHeatExec(cpu.getProgramCounter(), mem.debugBank(cpu.getProgramCounter()));
        cpu.step();
    }
    assert(cpu.getProgramCounter() == 0x0317);
    const CycleMonitor& m = heat.cycleMonitor();
    if (m.section().last != 55 || m.irq().last != 10) {
        std::printf("FAIL section %llu (want 55), irq %llu (want 10)\n",
                    static_cast<unsigned long long>(m.section().last),
                    static_cast<unsigned long long>(m.irq().last));
        assert(false);
    }
    assert(m.section().lastVideo == 55 && m.irq().lastVideo == 10 && "no accelerator: video = CPU");
    assert(m.marks() == 4 && m.section().count == 1 && m.irq().count == 1 && "the READ of $C066 is no mark");
    assert(!m.sectionOpen() && !m.irqOpen() && m.orphanStops() == 0);
    assert(m.irqInLastSection() == 0 && "the IRQ span came after the section");
    // The marks change nothing on the machine: the paddle reads as before,
    // and the write is on the map like any other (the I/O plane).
    assert(static_cast<uint8_t>(mem.memRead(0xC066) & 0x80) == paddleBefore);
    assert(stampOf(heat, HeatData::kRom, HeatData::Write, 0xC067) != 0);
    // A frozen copy keeps the figures; Clear forgets them.
    const HeatData frozen = heat.freeze();
    heat.clear();
    assert(frozen.cycleMonitor().section().last == 55 && heat.cycleMonitor().marks() == 0);
    mem.setHeatSink(nullptr);
    // Detached, the marks are not seen.
    mem.memWrite(0xC067, 0);
    assert(heat.cycleMonitor().marks() == 0);
    std::printf("  $C067/$C066 and $C069/$C068 time a program to the cycle; reads ignored OK\n");

    // The figures, fed directly: min / max / average, an IRQ inside the
    // section, a stop with no start.
    CycleMonitor c;
    c.mark(CycleMonitor::kSectionStart, 1000, 1000);
    c.mark(CycleMonitor::kIrqStart, 1100, 1100);
    c.mark(CycleMonitor::kIrqStop, 1150, 1150);
    c.mark(CycleMonitor::kSectionStop, 1300, 1300);
    c.mark(CycleMonitor::kSectionStart, 2000, 2000);
    c.mark(CycleMonitor::kSectionStop, 2100, 2100);
    assert(c.section().count == 2 && c.section().min == 100 && c.section().max == 300);
    assert(std::fabs(c.section().average() - 200.0) < 1e-9 && c.section().last == 100);
    c.mark(CycleMonitor::kSectionStart, 3000, 3000);
    c.mark(CycleMonitor::kIrqStart, 3010, 3010);
    c.mark(CycleMonitor::kIrqStop, 3040, 3040);
    c.mark(CycleMonitor::kSectionStop, 3500, 3500);
    assert(c.irqInLastSection() == 30 && c.irq().count == 2 && c.irq().max == 50);
    c.mark(CycleMonitor::kIrqStop, 4000, 4000);
    assert(c.orphanStops() == 1 && c.irq().count == 2 && "a stop with no start is not a span");
    assert(c.sectionHistory().size == 3 && c.sectionHistory().at(0) == 300 && c.sectionHistory().at(2) == 500);
    assert(!CycleMonitor::isMark(0xC065) && CycleMonitor::isMark(0xC066) &&
           CycleMonitor::isMark(0xC069) && !CycleMonitor::isMark(0xC06A));

    // FT's bars: green within a frame, orange to 4 frames, red beyond; the
    // frame is the machine's (PAL here), the bar clamped at 255 of 256.
    const int pal = 20280;
    CycleMonitor::Bar b = CycleMonitor::sectionBar(pal / 2, pal);
    assert(b.frames == 1 && b.g == 0xDD && b.r == 0 && std::fabs(b.fill - 0.5f) < 1e-3f);
    b = CycleMonitor::sectionBar(3u * pal, pal);
    assert(b.frames == 4 && b.r == 0xFF && b.g == 0x77 && std::fabs(b.fill - 0.75f) < 1e-3f);
    b = CycleMonitor::sectionBar(6u * pal, pal);
    assert(b.frames == 8 && b.r == 0xFF && b.g == 0 && std::fabs(b.fill - 0.75f) < 1e-3f);
    assert(CycleMonitor::sectionBar(100u * pal, pal).fill == 255.0f / 256.0f);
    b = CycleMonitor::irqBar(pal / 4, pal);
    assert(b.b == 0xFF && b.g == 0x70 && std::fabs(b.fill - 0.25f) < 1e-3f);
    assert(std::fabs(CycleMonitor::lines(650) - 10.0) < 1e-9);
    std::printf("  monitor figures and FT's bars OK\n");
}

// ── 8. Max-pooling ─────────────────────────────────────────────────────────

void pooling()
{
    std::vector<hv::HeatCell> src(4u * 256u);
    src[2u * 256u + 137u].k[HeatData::Write] = 255;   // one isolated write
    src[2u * 256u + 138u].k[HeatData::Read] = 90;     // a read beside it
    for (auto& cl : src) cl.bg = 40;
    src[0].bg = 0;
    std::vector<hv::HeatCell> dst;
    for (int outW = 1; outW <= 256; ++outW) {
        for (int outH : {1, 2, 3, 4}) {
            hv::maxPool(src, 4, outW, outH, dst);
            int hot = 0;
            for (const auto& cl : dst) hot += cl.k[HeatData::Write] == 255;
            if (hot != 1) {
                std::printf("FAIL pool %dx%d: the write is in %d pixels\n", outW, outH, hot);
                assert(false);
            }
        }
    }
    // Per kind: at 128 px a pixel holds bytes 136-137 or 138-139; at 64 px,
    // 136-139 — the write AND the read survive in one pixel.
    hv::maxPool(src, 4, 64, 4, dst);
    const hv::HeatCell& px = dst[2u * 64u + 34u];
    assert(px.k[HeatData::Write] == 255 && px.k[HeatData::Read] == 90);
    // The background is box-filtered (texture, not heat).
    hv::maxPool(src, 4, 128, 2, dst);
    assert(dst[0].bg == (0 + 40 + 40 + 40) / 4 && dst[1].bg == 40);
    // ...by the area each byte covers: a $00 / $FF fill (POM2's power-on
    // RAM) pooled to 230 px is one even grey, not stripes.
    std::vector<hv::HeatCell> fill(256u);
    for (std::size_t x = 0; x < fill.size(); ++x) fill[x].bg = (x & 1) ? 60 : 0;
    hv::maxPool(fill, 1, 230, 1, dst);
    int lo = 255, hi = 0;
    for (const auto& cl : dst) { lo = std::min<int>(lo, cl.bg); hi = std::max<int>(hi, cl.bg); }
    assert(hi - lo <= 7 && lo >= 25 && hi <= 36);
    // ...and at 1-3 px a byte, drawn native, the same fill low-passed:
    // one grey, ends included; the heat untouched.
    fill[100].k[HeatData::Write] = 255;
    hv::smoothBackground(fill, 1);
    for (const auto& cl : fill) assert(cl.bg == 30);
    assert(fill[100].k[HeatData::Write] == 255 && fill[101].k[HeatData::Write] == 0);
    std::printf("  max-pooling keeps an isolated access at every size, per kind OK\n");
}

// ── 9. Where a byte is on the screen ───────────────────────────────────────

void screenSpots()
{
    for (int page = 1; page <= 2; ++page) {
        int holes = 0;
        for (uint32_t a = 0; a < 0x400; ++a) {
            const auto phys = static_cast<uint16_t>((page == 1 ? 0x0400 : 0x0800) + a);
            holes += hv::screenSpot(HeatData::kMain, phys).hole;
        }
        assert(holes == 64);
        for (int row = 0; row < 24; ++row)
            for (int col = 0; col < 40; ++col) {
                const auto phys = static_cast<uint16_t>((page == 1 ? 0x0400 : 0x0800) +
                                                        Memory::textRowOffset(row) + col);
                const hv::ScreenSpot sp = hv::screenSpot(HeatData::kAux0, phys);
                assert(sp.kind == hv::ScreenSpot::Text && sp.page == page && !sp.hole && sp.aux);
                assert(sp.row == row && sp.col == col);
            }
        holes = 0;
        for (uint32_t a = 0; a < 0x2000; ++a) {
            const auto phys = static_cast<uint16_t>((page == 1 ? 0x2000 : 0x4000) + a);
            holes += hv::screenSpot(HeatData::kMain, phys).hole;
        }
        assert(holes == 512);
        for (int y = 0; y < 192; ++y)
            for (int col = 0; col < 40; ++col) {
                const auto phys = static_cast<uint16_t>((page == 1 ? 0x2000 : 0x4000) +
                                                        Memory::hiResLineOffset(y) + col);
                const hv::ScreenSpot sp = hv::screenSpot(HeatData::kMain, phys);
                assert(sp.kind == hv::ScreenSpot::Hires && sp.page == page && !sp.hole && !sp.aux);
                if (sp.row != y || sp.col != col) {
                    std::printf("FAIL $%04X -> line %d byte %d, want %d %d\n", phys, sp.row, sp.col, y, col);
                    assert(false);
                }
            }
    }
    assert(hv::screenSpot(HeatData::kMain, 0x0C00).kind == hv::ScreenSpot::None);
    assert(hv::screenSpot(HeatData::kRom, 0x2000).kind == hv::ScreenSpot::None);
    const std::string d = hv::describeSpot(hv::screenSpot(HeatData::kMain, 0x2000 + 0x0400 * 5 + 0x80 * 4 + 12));
    assert(d.find("line 37") != std::string::npos && d.find("byte 12") != std::string::npos &&
           d.find("dots 84-90") != std::string::npos && "y = 4*8 + 5");
    std::printf("  screen rows / lines / columns from an address, holes included OK\n");
}

}  // namespace

int main()
{
    acceptance();
    banksApart();
    who();
    inverses();
    lifetime();
    view();
    hotSpots();
    options();
    monitor();
    pooling();
    screenSpots();
    std::printf("memory_heatmap: OK\n");
    return 0;
}
