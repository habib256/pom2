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

// The language card: its $C080-$C08F switches, its RAM, and the two
// things that can stand in front of it — a slot card holding the bus INH
// line (the Saturn 128K) and an aux-slot card flipping A15 (the Premium
// SoftCard //e). Split out of Memory.cpp (R0, docs/decisions/project-scope.md).
// The aux slot's geometry sits here too: the card seated in it, and the
// RamWorks size it excludes (setAuxSlotPort / setRamWorksBanks).

#include "Memory.h"

#include "Logger.h"

uint8_t Memory::languageCardSwitchAccess(uint16_t addr, bool isWrite)
{
    const uint8_t low4 = static_cast<uint8_t>(addr & 0x0F);

    // $C080-$C087 select bank 2, $C088-$C08F select bank 1. Within each
    // half, the low two bits choose ROM/RAM read mode and whether the
    // prewrite latch is armed. $C084-$C087 mirror $C080-$C083.
    //
    // Write-enable is STICKY (MAME apple2e.cpp:1506-1564 `lc_update`):
    //   - any EVEN access ($C08{0,2,4,6,8,A,C,E}) clears prewrite AND
    //     write-enable;
    //   - any WRITE clears prewrite only — write-enable is left UNCHANGED
    //     (so flipping the bank with `STA $C08x` mid-write keeps writes on);
    //   - the first odd READ arms prewrite; a second consecutive odd READ
    //     commits write-enable.
    // The previous formula (`writeEnable = odd && prevPrewrite`, recomputed
    // every access) diverged from this — it dropped/re-armed write-enable on
    // repeated odd writes/reads, so a game that streams data into Language-
    // Card RAM while toggling banks (Nox Archaist's city decompressor, into
    // aux LC at $D000) had its LC writes silently dropped → corrupt $D000
    // code → crash. Pin: tests/iie_langcard_writeenable_smoke_test.cpp.
    if ((low4 & 1) == 0) {            // even access: disable prewrite + writing
        lcPrewrite    = false;
        lcWriteEnable = false;
    }
    if (isWrite) {                    // any write disables prewrite (WE unchanged)
        lcPrewrite = false;
    } else if ((low4 & 1) == 1) {     // odd read: arm, then commit
        if (!lcPrewrite) lcPrewrite = true;
        else             lcWriteEnable = true;
    }

    const uint8_t mode = low4 & 0x03;
    lcReadRam     = (mode == 0x00 || mode == 0x03);   // 0/3 = RAM, 1/2 = ROM
    lcBank2Active = (low4 & 0x08) == 0;                // bank2 when !(offset&8)

    // The card itself does not drive the data lines for $C08x — the byte
    // the CPU reads is whatever the video DMA last latched onto the bus.
    return floatingBus();
}

uint8_t Memory::languageCardRead(uint16_t addr) const
{
    if (inhRead_) return lcInhibitor_->inhRead(addr);   // a card holds INH
    if (!lcReadRam) {
        // //c ROMBANK alt firmware overrides motherboard ROM at $D000-$FFFF.
        // LC RAM path below is unaffected — banking only swaps the ROM side.
        uint8_t out;
        if (iicProfile_ && iicProfile_->languageCardRomRead(addr, out))
            return out;
        return mem[addr];
    }
    return languageCardRamPeek(addr);
}

// The RAM half of the language card's address decode, on its own so the read
// path and `peekCpuWriteTarget` cannot drift: which 4 KB bank at $D000, and
// main vs the ALTZP aux trio. It is what `languageCardWrite` writes into.
// The aux language card's cell at physical address p ^ $8000 under an
// aux-slot card's A15 flip: always below $C000, in the aux bank.
static uint16_t flippedLcCell(uint16_t addr, bool bank2)
{
    const uint16_t phys = addr >= 0xE000 ? addr
                        : static_cast<uint16_t>((bank2 ? 0xD000 : 0xC000) + (addr - 0xD000));
    return static_cast<uint16_t>(phys ^ 0x8000);
}

uint8_t Memory::languageCardRamPeek(uint16_t addr) const
{
    const bool useAux = iieMode && (iieMemMode & MF_ALTZP);
    if (useAux && auxFlip_) return aux[flippedLcCell(addr, lcBank2Active)];
    if (addr < 0xE000) {
        const uint16_t off = static_cast<uint16_t>(addr - 0xD000);
        if (useAux) return lcBank2Active ? auxLcBank2[off] : auxLcBank1[off];
        return lcBank2Active ? lcBank2[off] : lcBank1[off];
    }
    if (useAux) return auxLcHigh[addr - 0xE000];
    return lcHigh[addr - 0xE000];
}

void Memory::languageCardWrite(uint16_t addr, uint8_t value)
{
    // A card holding INH takes the WRITES whenever it holds either side, and
    // keeps them only when its own write side is on. MAME apple2.cpp:236-281
    // (a2bus_inh_w: INH_READ selects upperbank[1], whose write handler is
    // inh_w → the card; INH_WRITE alone keeps upperbank[0], also inh_w) and
    // apple2e.cpp:826-870 + inh_w :3321 (the //e's own lc_w never runs under
    // INH_READ). This used to let a write through to the machine's language
    // card when the card read RAM with writes off — a reset leaves the
    // motherboard LC write-enabled, so a Saturn in write-protect mode had the
    // //e LC (ProDOS's home) take every byte it dropped (bug hunt 2026-11-01).
    if (lcInhibitor_ && (inhRead_ || lcInhibitor_->inhibitsWrite())) {
        if (lcInhibitor_->inhibitsWrite()) lcInhibitor_->inhWrite(addr, value);
        return;
    }
    if (!lcWriteEnable) return;
    const bool useAux = iieMode && (iieMemMode & MF_ALTZP);
    if (useAux && auxFlip_) { aux[flippedLcCell(addr, lcBank2Active)] = value; return; }
    if (addr < 0xE000) {
        const uint16_t off = static_cast<uint16_t>(addr - 0xD000);
        if (useAux) {
            if (lcBank2Active) auxLcBank2[off] = value;
            else               auxLcBank1[off] = value;
        } else {
            if (lcBank2Active) lcBank2[off] = value;
            else               lcBank1[off] = value;
        }
        return;
    }
    if (useAux) auxLcHigh[addr - 0xE000] = value;
    else        lcHigh[addr - 0xE000] = value;
}

void Memory::setAuxSlotPort(AuxSlotPort* port)
{
    // One auxiliary slot: a card in it (the Premium SoftCard //e) IS the aux
    // memory, so RamWorks cannot be fitted beside it, and nothing decodes
    // $C071/3/5/7 as a bank register (MAME apple2e.cpp:2665 hands $C07x to
    // the aux card; only a2eramworks3 implements write_c07x). POM2 let both
    // coexist, and a 6502 `STA $C073` swapped 64 KB of other RAM in under
    // the running Z80. Bank 0 is brought back first: it is what the display
    // reads, and what the card then owns.
    if (port && ramWorksBanks_ > 1) {
        ramWorksSwapToBank(0);
        pom2::log().info("RamWorks", "an aux-slot card is seated: RamWorks off (one aux slot)");
        setRamWorksBanks(1);
    }
    auxPort_ = port;
    refreshAuxSlotPort();
}

void Memory::setRamWorksBanks(uint32_t banks)
{
    if (auxPort_) banks = 1;   // the aux slot is taken (setAuxSlotPort)
    if (banks < 1) banks = 1;
    if (banks > kRamWorksMaxBanks) banks = kRamWorksMaxBanks;
    ramWorksBanks_ = banks;
    ramWorksBank_  = 0;
    // Backing holds ONE slot per bank (including bank 0 — we snapshot
    // the visible buffers into it whenever leaving bank 0). When
    // banks == 1 the backing is empty — stock IIe path, no swap.
    if (banks > 1) {
        ramWorksBacking_.assign(
            static_cast<size_t>(banks) * kRamWorksBankStride, 0u);
    } else {
        ramWorksBacking_.clear();
        ramWorksBacking_.shrink_to_fit();
    }
}
