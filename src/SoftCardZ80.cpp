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

// SoftCardZ80 implementation. MAME reference:
// src/devices/bus/a2bus/a2softcard.cpp (master @ 2026-07, 176 lines).

#include "SoftCardZ80.h"

#include "M6502.h"
#include "Memory.h"

#include <cstring>

namespace {
// Snapshot blob layout (little-endian, packed by hand so the format is
// stable across compilers/platforms — no struct memcpy):
//   "SFZ2" magic, then flags byte (bit0 enabled, bit1 firstBoot,
//   bit2 tCarry), then the Z80 register file. Older/foreign magics are
//   ignored on load (the slot may hold a different card than at save).
constexpr char kMagic[4] = { 'S', 'F', 'Z', '2' };  // v2: + Z80 pendingPrefix
} // namespace

SoftCardZ80::SoftCardZ80() : z80_(*this)
{
    z80_.reset();
}

// MAME a2softcard.cpp:88-109 `write_cnxx` — any $CnXX write flips bus
// ownership. Grant side: release the Z80's WAIT line + raise slot DMA
// (here: enabled_=true and halt the 6502 at its next instruction
// boundary); the FIRST grant after a reset also resets the Z80 so CP/M's
// loader finds it at PC=$0000. Release side: assert WAIT + lower DMA —
// the Z80 keeps its full state and resumes in place on the next grant.
void SoftCardZ80::slotRomWrite(uint8_t /*low8*/, uint8_t /*v*/)
{
    if (!enabled_) {
        if (firstZ80Boot_) {
            firstZ80Boot_ = false;
            z80_.reset();
        }
        enabled_ = true;
        // Halt the 6502 after the instruction performing this write —
        // M6502::run re-arms `running` on its next call, so this only
        // ends the in-flight chunk (same trick as WAI/STP).
        if (cpu_)
            cpu_->stop();
    } else {
        enabled_ = false;   // WAIT line: Z80 freezes, state preserved
    }
}

// MAME a2softcard.cpp:82-86 `reset_from_bus`: z80 reset + device_reset
// (disarm, re-latch FirstZ80Boot, assert WAIT).
void SoftCardZ80::onReset()
{
    z80_.reset();
    enabled_ = false;
    firstZ80Boot_ = true;
    tCarry_ = 0;
}

// MAME a2softcard.cpp:111-176 dma_r/dma_w — six windows, identical math
// for reads and writes. Every window is a whole 4 KB page, so the mapping is
// one base per Z80 page; this table is that if-chain, flattened (ARCH-007):
//   Z80 $0000-$AFFF → 6502 $1000-$BFFF   (+$1000)
//   Z80 $B000-$DFFF → 6502 $D000-$FFFF   (LC $D000/$E000/$F000, or ROM)
//   Z80 $E000-$EFFF → 6502 $C000-$CFFF   (I/O space)
//   Z80 $F000-$FFFF → 6502 $0000-$0FFF   (zero page and up)
uint16_t SoftCardZ80::xlate(uint16_t a)
{
    static constexpr uint16_t kPageBase[16] = {
        0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x7000, 0x8000,
        0x9000, 0xA000, 0xB000, 0xD000, 0xE000, 0xF000, 0xC000, 0x0000,
    };
    return uint16_t(kPageBase[a >> 12] | (a & 0x0FFF));
}

uint8_t SoftCardZ80::z80MemRead(uint16_t addr)
{
    // MAME gates on m_bEnabled (dma_r returns $FF when WAITed); we only
    // step the Z80 while enabled, but keep the guard for parity — the
    // toggle can land mid-instruction and the tail accesses of that
    // instruction still hit the bus on real hardware... where the WAIT
    // line stops the clock first. Returning open-bus matches MAME.
    if (!enabled_ || !mem_)
        return 0xFF;
    return mem_->memRead(xlate(addr));
}

void SoftCardZ80::z80MemWrite(uint16_t addr, uint8_t v)
{
    if (!enabled_ || !mem_)
        return;
    mem_->memWrite(xlate(addr), v);
}

// Run the Z80 for (up to) a 6502-cycle budget slice. 2 Z80 T-states =
// 1 six-502 cycle (2× clock); the converted count feeds
// Memory::advanceCycles per instruction so cycle-stamped consumers
// (video event log, Disk II LSS, speaker queue) see the same emuCycles
// stream the 6502 would have produced. Returns the 6502 cycles actually
// consumed (may overshoot by half an instruction, same contract as
// M6502::run). Exits early when the Z80 writes the toggle window and
// hands the bus back.
int SoftCardZ80::dmaRun(int cycles6502)
{
    // The 6502 is parked mid-instruction (the STA that granted the bus), and
    // its in-flight cycle count is what getCycleCountNow() /
    // getCurrentInstructionCycles() add to every bus timestamp. Left at that
    // STA's 4, every Z80 access to a VIA, the Disk II or the floating bus
    // was stamped up to 7 cycles off the Z80's own position. MAME's
    // dma_r/dma_w run on the Z80 timeline (a2softcard.cpp:111-176).
    if (cpu_) cpu_->clearInFlightCycles();
    int spent = 0;
    while (spent < cycles6502 && enabled_) {
        int t = z80_.step() + tCarry_;
        tCarry_ = t & 1;
        const int c = t >> 1;
        if (c > 0) {
            if (mem_)
                mem_->advanceCycles(c);
            spent += c;
        }
    }
    return spent;
}

void SoftCardZ80::appendSnapshotState(std::vector<uint8_t>& out) const
{
    const pom2::Z80::State& s = z80_.getState();
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(uint8_t((enabled_ ? 1 : 0) | (firstZ80Boot_ ? 2 : 0)
                          | (tCarry_ ? 4 : 0)));
    const uint8_t regs8[] = {
        s.a, s.f, s.b, s.c, s.d, s.e, s.h, s.l,
        s.a2, s.f2, s.b2, s.c2, s.d2, s.e2, s.h2, s.l2,
        s.i, s.r, s.im,
        uint8_t((s.iff1 ? 1 : 0) | (s.iff2 ? 2 : 0) | (s.halted ? 4 : 0)
                | (s.afterEi ? 8 : 0) | (s.nmiPending ? 16 : 0)
                | (s.irqLine ? 32 : 0)),
        s.irqData, s.pendingPrefix,
    };
    out.insert(out.end(), regs8, regs8 + sizeof(regs8));
    for (uint16_t w : { s.ix, s.iy, s.sp, s.pc, s.wz }) {
        out.push_back(uint8_t(w));
        out.push_back(uint8_t(w >> 8));
    }
}

void SoftCardZ80::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // 4 magic + 1 flags + 22 regs/flag bytes + 5×2 words = 37 bytes.
    constexpr std::size_t kLen = 4 + 1 + 22 + 10;
    if (len < kLen || std::memcmp(data, kMagic, 4) != 0)
        return;   // foreign or older blob — keep current state
    const uint8_t* p = data + 4;
    const uint8_t flags = *p++;
    enabled_      = (flags & 1) != 0;
    firstZ80Boot_ = (flags & 2) != 0;
    tCarry_       = (flags & 4) ? 1 : 0;

    pom2::Z80::State s;
    s.a = p[0];  s.f = p[1];  s.b = p[2];  s.c = p[3];
    s.d = p[4];  s.e = p[5];  s.h = p[6];  s.l = p[7];
    s.a2 = p[8]; s.f2 = p[9]; s.b2 = p[10]; s.c2 = p[11];
    s.d2 = p[12]; s.e2 = p[13]; s.h2 = p[14]; s.l2 = p[15];
    s.i = p[16]; s.r = p[17]; s.im = p[18];
    const uint8_t misc = p[19];
    s.iff1       = (misc & 1) != 0;
    s.iff2       = (misc & 2) != 0;
    s.halted     = (misc & 4) != 0;
    s.afterEi    = (misc & 8) != 0;
    s.nmiPending = (misc & 16) != 0;
    s.irqLine    = (misc & 32) != 0;
    s.irqData = p[20];
    s.pendingPrefix = p[21];
    p += 22;
    uint16_t* words[] = { &s.ix, &s.iy, &s.sp, &s.pc, &s.wz };
    for (uint16_t* w : words) {
        *w = uint16_t(p[0] | (p[1] << 8));
        p += 2;
    }
    z80_.setState(s);
}
