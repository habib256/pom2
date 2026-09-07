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

// ── What the CPU would see, without touching anything ────────────────────
//
// One concern, one translation unit (CLAUDE.md). These three functions answer
// "which byte does the 6502 fetch at this address, right now?" for INSPECTORS
// — the Debugger panel's disassembly, the Memory Viewer's grid and its undo
// record, the Mouse Inspector's screen holes. They resolve //e aux routing
// (including the live RamWorks bank), the language card's bank/read/ALTZP
// state, INTCXROM / SLOTC3ROM / INTC8ROM and the //c's bank-1 alt firmware —
// and they do it with NO side effect, which is the whole point: reading a
// soft switch through the emulated bus clocks the Disk II, clears the
// keyboard strobe and moves the video beam, so an inspector must not.
//
// $C000-$C0FF therefore stays the flat mirror on purpose, and so does a SLOT
// window at $C100-$CFFF: several cards decode MMIO in their $Cn00 page (the
// Mockingboard's VIA, the CFFA's task file) and the //c+ MIG windows have
// read side effects of their own.
//
// They live here rather than in Memory.cpp because that file is one of the
// god-objects TODO.md's size ratchet exists to hold still, and because this
// is a self-contained question about the paging model rather than part of the
// bus itself.

#include "Memory.h"

uint8_t Memory::peekCpuView(uint16_t addr) const
{
    // WHY this exists: the Debugger panel and the Memory Viewer both read
    // `data()` — the flat MAIN-bank mirror — so on a //e they disassembled
    // and displayed main RAM while the CPU was fetching from aux or from the
    // language card, and the viewer's Undo recorded a main byte for a write
    // that had gone to aux. This resolves the same paging state memRead()
    // resolves, but every branch below is a pure load: nothing here toggles a
    // soft switch, latches INTC8ROM, claims the $C800 window or steps a
    // stateful intercept (NoSlotClock, //c+ MIG).
    if (flatBus_) return mem[addr];        // Klaus harness / a card CPU's bus

    if (addr < 0xC000)
        return (iieMode && iieReadFromAux(addr)) ? aux[addr] : mem[addr];

    // $C000-$C0FF: reading a soft switch is an ACTION. Never perform it for a
    // repaint — hand back the mirror instead (documented in Memory.h).
    if (addr <= 0xC0FF) return mem[addr];

    if (addr >= 0xD000) return languageCardRead(addr);

    // $C100-$CFFF. Mirrors memReadSlowBody's decode without its latches.
    if (iieMode) {
        const bool forcedInt = iicProfile_ && iicProfile_->forcesIntCxRom();
        const bool c3        = (addr & 0xFF00u) == 0xC300u;
        const bool slotC3    = (iieMemMode & MF_SLOTC3ROM) != 0;
        bool internal = (iieMemMode & MF_INTCXROM) != 0 || forcedInt;
        if (!internal && c3 && !slotC3)               internal = true;
        if (!internal && intC8Rom && addr >= 0xC800)  internal = true;
        if (internal) {
            // //c-class bank-1 alt firmware. The //c+ MIG windows
            // ($CC00-$CCFF / $CE00-$CEFF) are excluded on purpose: migRead
            // walks a RAM page cursor and flips 3.5" head select, so it is
            // emphatically not a peek.
            if (iicProfile_ && !(addr >= 0xCC00 && addr <= 0xCCFF) &&
                !(addr >= 0xCE00 && addr <= 0xCEFF)) {
                uint8_t out = 0;
                if (iicProfile_->internalRomRead(addr, 0xFF, out)) return out;
            }
            return internalIORom[addr - 0xC000];
        }
    }
    // Slot-ROM window: falls back to the mirror. See Memory.h — several
    // cards decode MMIO inside their $Cn00 page.
    return mem[addr];
}

uint8_t Memory::peekCpuWriteTarget(uint16_t addr) const
{
    if (flatBus_ || addr >= 0xC000) return peekCpuView(addr);
    return (iieMode && iieWriteToAux(addr)) ? aux[addr] : mem[addr];
}

void Memory::snapshotCpuView(uint8_t* out) const
{
    if (!out) return;
    // The callers (Debugger panel, Memory Viewer) run this UNDER stateMutex,
    // once per repaint, so it replaces a 64 KiB memcpy and must stay in that
    // league — CLAUDE.md's lock rule. RAM below $C000 routes in whole ranges
    // (the aux table is range-based), so copy those in blocks and only walk
    // the top 16 KiB byte by byte.
    if (flatBus_ || !iieMode) {
        std::memcpy(out, mem.data(), 0xC000);
    } else {
        struct Range { uint32_t lo, hi; };
        static constexpr Range kRanges[] = {
            { 0x0000, 0x0200 }, { 0x0200, 0x0400 }, { 0x0400, 0x0800 },
            { 0x0800, 0x2000 }, { 0x2000, 0x4000 }, { 0x4000, 0xC000 },
        };
        for (const Range& r : kRanges) {
            const bool fromAux = iieReadFromAux(static_cast<uint16_t>(r.lo));
            const uint8_t* src = fromAux ? aux.data() : mem.data();
            std::memcpy(out + r.lo, src + r.lo, r.hi - r.lo);
        }
    }
    for (uint32_t a = 0xC000; a < 0x10000u; ++a)
        out[a] = peekCpuView(static_cast<uint16_t>(a));
}
