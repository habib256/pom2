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
            // A card the //c lets through its forced INTCXROM, in memRead's
            // order (Memory.cpp, the internal branch of memReadSlowBody): an
            // awake Mockingboard 4c, then the bank-1 alt firmware, then the
            // bank-0 punch for a card that exposesIicOnboardRom (the
            // AppleWin-HLE mouse at $C400, the ClockCard, the ARMED $C500
            // stub), then that stub's $C800 bank while its window is open.
            // The view used to stop at the internal ROM, so the debugger
            // disassembled the //c's own mouse / SmartPort firmware while
            // the CPU ran the card's (bug hunt 2026-10-16). Each card is
            // asked through peekSlotRom / peekExpansionRom, which latch
            // nothing; a card that cannot answer shows the flat mirror.
            const auto cardPeek = [&](const SlotPeripheral* p, bool expansion) {
                uint8_t out = 0;
                const bool ok = expansion
                    ? p->peekExpansionRom(static_cast<uint16_t>(addr - 0xC800), out)
                    : p->peekSlotRom(static_cast<uint8_t>(addr & 0xFF), out);
                return ok ? out : mem[addr];
            };
            const bool slotPage = addr >= 0xC100 && addr <= 0xC7FF;
            if (iicProfile_ && iicExpansionAwake_ && slotPage)
                if (const SlotPeripheral* p = slots.peripheralForIicRomPage((addr >> 8) & 7))
                    return cardPeek(p, false);
            // //c-class bank-1 alt firmware. The //c+ MIG windows
            // ($CC00-$CCFF / $CE00-$CEFF) are excluded on purpose: migRead
            // walks a RAM page cursor and flips 3.5" head select, so it is
            // emphatically not a peek.
            if (iicProfile_ && !(addr >= 0xCC00 && addr <= 0xCCFF) &&
                !(addr >= 0xCE00 && addr <= 0xCEFF)) {
                uint8_t out = 0;
                if (iicProfile_->internalRomRead(addr, 0xFF, out)) return out;
            }
            if (iicProfile_ && slotPage) {
                const int slot = (addr >> 8) & 0x07;
                const bool armOk = (slot != 5) ||
                    (iicSmartPortArmed_ && !iicProfile_->peekServesExternalSmartPort());
                if (armOk)
                    if (const SlotPeripheral* p = slots.peripheral(slot);
                        p && p->exposesIicOnboardRom())
                        return cardPeek(p, false);
            }
            if (iicProfile_ && addr >= 0xC800 && addr <= 0xCFFE &&
                !intC8Rom && iicSmartPortArmed_ && iicCardWindow_ &&
                slots.getActiveExpansionSlot() == 5)
                if (const SlotPeripheral* p = slots.peripheral(5);
                    p && p->exposesIicOnboardRom())
                    return cardPeek(p, true);
            return internalIORom[addr - 0xC000];
        }
    }
    // Slot-ROM window: the card's own side-effect-free peek, when it has
    // one. The flat mirror is never filled here on a //e (its internal ROM
    // lives in internalIORom), so a card's firmware read as $00: the
    // debugger disassembled BRKs, and Step Over / Step Out never saw the
    // card's RTS — F8 on a `JSR $C5xx` SmartPort call became an unbounded
    // Run (bug hunt 2026-10-23). A card that cannot peek (several decode
    // MMIO inside their $Cn00 page; see Memory.h) still shows the mirror.
    uint8_t out = 0;
    if (addr <= 0xC7FF) {
        if (const SlotPeripheral* p = slots.peripheral((addr >> 8) & 0x07);
            p && p->peekSlotRom(static_cast<uint8_t>(addr & 0xFF), out))
            return out;
    } else if (addr <= 0xCFFE) {
        const int active = slots.getActiveExpansionSlot();
        if (active >= 1 && active <= 7)
            if (const SlotPeripheral* p = slots.peripheral(active);
                p && p->peekExpansionRom(static_cast<uint16_t>(addr - 0xC800), out))
                return out;
    }
    return mem[addr];
}

uint8_t Memory::peekCpuWriteTarget(uint16_t addr) const
{
    if (flatBus_) return peekCpuView(addr);
    if (addr < 0xC000)
        return (iieMode && iieWriteToAux(addr)) ? aux[addr] : mem[addr];
    // $D000-$FFFF is NOT "the language card writes where it reads". The card
    // has two independent latches, and their power-on combination — every
    // reset leaves `lcWriteEnable` set with ROM still mapped for reads
    // (Sather fig. 5.13) — is exactly the case where they disagree: the read
    // view is Monitor/Applesoft ROM while the write lands in LC RAM. Handing
    // the ROM byte back as "the byte this write replaces" made the Memory
    // Viewer's UNDO push a ROM opcode into Language-Card RAM. Resolve the RAM
    // cell whenever writes are enabled; when they are not, the write is
    // dropped and so is its undo, so the read view is the right answer.
    // A card holding INH for writes owns the cell (its inhRead is a plain
    // RAM index, whatever its read mode).
    if (addr >= 0xD000 && lcInhibitor_ && lcInhibitor_->inhibitsWrite())
        return lcInhibitor_->inhRead(addr);
    if (addr >= 0xD000 && lcWriteEnable) return languageCardRamPeek(addr);
    return peekCpuView(addr);
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

// Physical identity, resolved before an access can change the mapping.
// LC segment 0 is the shared $E000-$FFFF region; 1/2 are $D000 banks.
pom2::DebugBank Memory::debugBank(uint16_t addr, bool write) const
{
    using B = pom2::DebugBank;
    if (flatBus_) return {B::Flat};
    if (addr < 0xC000) {
        const bool auxiliary = iieMode && (write ? iieWriteToAux(addr) : iieReadFromAux(addr));
        return auxiliary ? B{B::Aux, ramWorksBank_} : B{B::Main};
    }
    if (addr >= 0xD000) {
        if (lcInhibitor_ && (write ? lcInhibitor_->inhibitsWrite() : inhRead_)) {
            for (int slot = 1; slot <= 7; ++slot)
                if (dynamic_cast<const LcInhibitor*>(slots.peripheral(slot)) == lcInhibitor_)
                    return {B::Slot, static_cast<uint8_t>(slot)};
        }
        if (write ? lcWriteEnable : lcReadRam) {
            const uint8_t segment = addr >= 0xE000 ? 0 : lcBank2Active ? 2 : 1;
            const bool auxiliary = iieMode && (iieMemMode & MF_ALTZP);
            return auxiliary ? B{B::AuxLanguage, ramWorksBank_, segment} : B{B::Language,0,segment};
        }
        if (write) return {B::Ignored};
        if (addr >= 0xF000 && (!iicProfile_ || !iicProfile_->debugRomBank())) {
            uint8_t displaced = 0;
            for (int slot = 1; slot <= 7; ++slot)
                if (const auto* card = slots.peripheral(slot); card && card->peekDisplacedRom(addr,displaced))
                    return {B::Slot,static_cast<uint8_t>(slot)};
        }
        return {B::Rom, iicProfile_ ? iicProfile_->debugRomBank() : uint8_t{0}};
    }
    if (addr < 0xC100) return {B::Io};
    const bool c3 = (addr & 0xFF00) == 0xC300;
    const bool internal = iieMode && ((iicProfile_ && iicProfile_->forcesIntCxRom()) ||
        (iieMemMode & MF_INTCXROM) || (c3 && !(iieMemMode & MF_SLOTC3ROM)) ||
        (intC8Rom && addr >= 0xC800));
    if (internal) {
        if (iicProfile_ && addr < 0xC800) {
            const int slot = (addr >> 8) & 7;
            if (iicExpansionAwake_ && slots.peripheralForIicRomPage(slot))
                return {B::Slot,static_cast<uint8_t>(slot)};
            if (!iicProfile_->debugRomBank() && (slot != 5 ||
                (iicSmartPortArmed_ && !iicProfile_->peekServesExternalSmartPort())))
                if (const auto* p = slots.peripheral(slot); p && p->exposesIicOnboardRom())
                    return {B::Slot,static_cast<uint8_t>(slot)};
        }
        if (iicProfile_ && addr >= 0xC800 && addr <= 0xCFFE && !intC8Rom &&
            iicSmartPortArmed_ && iicCardWindow_ && !iicProfile_->debugRomBank() &&
            slots.getActiveExpansionSlot() == 5)
            return {B::Slot,5};
        if (iicProfile_ && iicProfile_->isIIcPlus() && iicProfile_->debugRomBank() &&
            ((addr >= 0xCC00 && addr <= 0xCCFF) || (addr >= 0xCE00 && addr <= 0xCEFF)))
            return {B::Io};
        return {B::Rom, iicProfile_ ? iicProfile_->debugRomBank() : uint8_t{0}};
    }
    const int slot = addr < 0xC800 ? (addr >> 8) & 7 : slots.getActiveExpansionSlot();
    return slot >= 1 && slot <= 7 ? B{B::Slot,static_cast<uint8_t>(slot)} : B{B::Io};
}
