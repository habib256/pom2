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

// SlinkyCard — the Apple II Memory Expansion Card (670-0024, "Slinky").
// Port of MAME `src/devices/bus/a2bus/a2memexp.cpp` (R. Belmont,
// BSD-3-Clause), the `a2bus_memexpapple_device` half.
//
// ─── The hardware ────────────────────────────────────────────────────────
//
// Up to 1 MB of RAM that the 6502 cannot address directly: it is reached
// through a 24-bit address latch and a data port in the card's $C0nX window
// (MAME `read_c0nx` / `write_c0nx`, a2memexp.cpp:166-232):
//
//   $C0n0  address bits 0-7    write: set the latch byte, live pointer = latch
//   $C0n1  address bits 8-15   write: idem
//   $C0n2  address bits 16-23  write: idem. READS back with the high nibble
//                              forced to 1 (MAME `m_bankhior = 0xF0` for the
//                              Apple card, :129) — what firmware sizes it by.
//   $C0n3  data port           read or write RAM[ptr & $FFFFF], then ptr++,
//                              and $C0n0-$C0n2 follow the incremented pointer
//   $C0n4-$C0nF               plain read/write latches (MAME `m_regs`)
//
// The address mask is $FFFFF (MAME `m_addrmask = 0xfffff`, :130): 1 MB, which
// is the only size MAME models. The 256 KB / 512 KB populations the card was
// also sold in are NOT offered: how a partly populated board aliases is not
// in any source POM2 has, so it is not invented.
//
// ─── Firmware ────────────────────────────────────────────────────────────
//
// Apple's 341-0344A, 4 KB, the file MAME's ROM_LOAD names (a2memexp.cpp:25,
// CRC32 1e994e17), shipped as `roms/slinky_341-0344a.bin`. Layout, per MAME
// `read_cnxx` (:238-247) and `read_c800` (:253-260):
//
//   $0000-$00FF  unused by the bus (an easter-egg page)
//   $0n00-$0nFF  the $Cn00 page for slot n — seven per-slot copies that
//                differ in the bytes encoding the slot
//   $0800-$0FFF  the $C800-$CFFF expansion ROM (the card takes $C800,
//                MAME `take_c800() { return true; }`, :63)
//
// That firmware is a SmartPort/ProDOS block driver ($Cn07 = $00, $CnFF =
// driver entry) that serves the RAM as a RAM disk and formats it itself, so
// ProDOS 8 mounts it with no driver install. ROM-GATED: without the dump the
// slot stays empty (SlotCardFactory). A hand-written fallback was dropped on
// purpose: the real ROM ships in roms/, and a synthetic firmware would have
// had to invent the on-card volume layout.
//
// ─── RamFactor ───────────────────────────────────────────────────────────
//
// The Applied Engineering RamFactor is the SAME register model in MAME (the
// `a2bus_ramfactor_device` subclass): 8 MB mask, no forced high nibble on
// $C0n2, and an 8 KB bank-switched ROM selected by $C0nF bit 0. Software
// written for the Slinky (ProDOS, AppleWorks' RAM-disk support) therefore
// runs on either. POM2 models only the Apple card: no RamFactor ROM is
// shipped.
//
// ─── Persistence ─────────────────────────────────────────────────────────
//
// The RAM is volatile, as on the real board (no battery): it is never
// written to a file. It is ordinary machine state, so it rides in snapshots
// and the rewind ring, written sparsely (only 256-byte pages that differ from
// the $FF power-on fill, SlinkyCard.cpp). The CLAUDE.md "never cross an irreversible write" rule does not
// apply — nothing irreversible happens here.

#ifndef POM2_SLINKY_CARD_H
#define POM2_SLINKY_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class SlinkyCard : public SlotPeripheral
{
public:
    static constexpr std::size_t kRamBytes = 1024 * 1024;     // MAME m_addrmask + 1
    static constexpr uint32_t    kAddrMask = 0xFFFFF;         // a2memexp.cpp:130
    static constexpr uint8_t     kBankHiOr = 0xF0;            // a2memexp.cpp:129
    static constexpr std::size_t kRomBytes = 4096;            // a2memexp.cpp:24-25
    static constexpr const char* kRomPath  = "roms/slinky_341-0344a.bin";

    explicit SlinkyCard(int slot);

    std::string_view name() const override { return "Memory Expansion (Slinky)"; }

    /// Adopt the 4 KB 341-0344A image. Refuses any other size.
    bool loadRom(const std::vector<uint8_t>& bytes);
    bool romLoaded() const { return romLoaded_; }

    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    uint8_t expansionRomRead(uint16_t offset) override;
    bool    peekSlotRom(uint8_t low8, uint8_t& out) const override;
    bool    peekExpansionRom(uint16_t offset, uint8_t& out) const override;
    bool    takesC800() const override { return romLoaded_; }

    void onReset() override;

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // Diagnostics / tests.
    uint32_t livePointer() const { return liveptr_; }
    uint8_t  ramAt(uint32_t addr) const { return ram_[addr & kAddrMask]; }

private:
    void publishPointer(uint32_t p);

    int slot_;
    std::array<uint8_t, 16> regs_{};
    uint32_t wptr_    = 0;
    uint32_t liveptr_ = 0;
    std::vector<uint8_t> ram_;
    std::array<uint8_t, kRomBytes> rom_{};
    bool romLoaded_ = false;
};

} // namespace pom2

#endif // POM2_SLINKY_CARD_H
