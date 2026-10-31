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

// SaturnCard — Saturn Systems 128K extended language card (later Titan):
// eight 16 KB language cards on one board, in any slot. Ported from MAME
// `src/devices/bus/a2bus/ramcard128k.cpp` (R. Belmont, BSD-3-Clause) and
// izapple2's `cardSaturn.go` (GPL-3.0), after the Saturn 128K manual.
//
//  - **DEVSEL $C0n0-$C0nF**, read or write, A2 picks the meaning:
//        A2 = 0 ($C0n0-3, $C0n8-B): a language-card switch, the Apple
//          card's rules — A0-A1 = 0/3 read RAM, 1/2 read ROM; A3 = 0 the
//          $D000 bank 2, 1 bank 1; write-enable needs two odd reads in a
//          row, an even access or any write drops the pre-write latch and
//          an even one write-enable too (POM2's own LC,
//          `Memory::languageCardSwitchAccess`, MAME apple2e lc_update).
//          MAME's ramcard128k sets write-enable on the first odd access;
//          the manual says the card behaves as Apple's.
//        A2 = 1: the 16 KB block — $C0n4-7 blocks 0-3, $C0nC-F blocks 4-7.
//  - **INH**: while it reads RAM the card takes $D000-$FFFF reads from
//    the ROM and the motherboard language card, while writes are enabled
//    it takes the writes (`Memory::LcInhibitor`); with neither, it is
//    invisible and the machine's own language card answers.
//  - **Reset** (MAME reset_from_bus): ROM read, writes off, block 0, $D000
//    bank 2. The 128 KB survive.

#ifndef POM2_SATURN_CARD_H
#define POM2_SATURN_CARD_H

#include "Memory.h"
#include "SlotPeripheral.h"

#include <cstdint>
#include <vector>

class SaturnCard : public SlotPeripheral, public Memory::LcInhibitor
{
public:
    static constexpr int kBlocks = 8;

    /// The bus whose INH line the card drives. Wire before plugging.
    void setMemory(Memory* mem) { mem_ = mem; }

    std::string_view name() const override { return "Saturn 128K"; }

    uint8_t deviceSelectRead(uint8_t low4) override { access(low4, false); return 0xFF; }
    void    deviceSelectWrite(uint8_t low4, uint8_t) override { access(low4, true); }
    /// Seats / unseats the card on Memory's INH hook (SlotBus calls both on
    /// every plug, unplug and clear).
    void onPlug() override;
    void onUnplug() override;
    void onReset() override;

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // Memory::LcInhibitor
    bool    inhibitsRead() const override { return readRam_; }
    bool    inhibitsWrite() const override { return writeEnable_; }
    uint8_t inhRead(uint16_t addr) const override { return ram_[offset(addr)]; }
    void    inhWrite(uint16_t addr, uint8_t v) override { ram_[offset(addr)] = v; }

    // Test/debug hooks.
    int  block() const { return block_; }
    bool bank2() const { return bank2_; }

private:
    void access(uint8_t low4, bool isWrite);
    std::size_t offset(uint16_t addr) const
    {
        const std::size_t base = std::size_t(block_) * 0x4000;
        if (addr >= 0xE000) return base + 0x2000 + (addr - 0xE000);
        return base + (bank2_ ? 0x1000 : 0) + (addr - 0xD000);
    }
    void publish() { if (mem_ && mem_->lcInhibitor() == this) mem_->refreshLcInhibit(); }

    Memory* mem_ = nullptr;
    std::vector<uint8_t> ram_ = std::vector<uint8_t>(kBlocks * 0x4000, 0);
    bool readRam_     = false;
    bool writeEnable_ = false;
    bool prewrite_    = false;
    bool bank2_       = true;
    int  block_       = 0;
};

#endif // POM2_SATURN_CARD_H
