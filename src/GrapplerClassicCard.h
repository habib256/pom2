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

// GrapplerClassicCard — the ORIGINAL Orange Micro Grappler (1981), the
// Grappler+'s predecessor. A port of MAME `a2bus_grappler_device`
// (bus/a2bus/grappler.cpp:223-446), catalog key `grappler1`.
//
// 2 KB EPROM (eps-1, CRC 862773cb). The $Cn00 page is `rom[offset | slot<<8]`
// — each slot has its OWN page, and only slot 1's carries the Pascal 1.1
// signature ($Cn05=$38, $Cn07=$18, $Cn0B=$01, $Cn0C=$14); in slots 2-7 those
// bytes read $04/$48/$Cn/$AA (docs/printer-detection.md). $C800-$CFFF is the
// whole ROM (grappler.cpp:121-124); the card claims the window.
//
// Registers ($C0n0-$C0nF):
//   read  A0 set   status: open bus & $F0 | BUSY<<3 | PE<<2 | SELECT<<1 |
//                  ACK latch — and the latch here is 1 while a byte waits
//                  for its /ACK, 0 once acknowledged (grappler.cpp:397-434)
//   write A0 set   data latch
//   A1 set         assert /STROBE (read or write); else A2 set: release it
// The byte goes to the printer when /STROBE is released; a printer that takes
// it pulses /ACK at once, one that cannot leaves the latch at 1 — the printer
// is a `CentronicsPrinter`, with the same held-byte rule as the other cards.

#ifndef POM2_GRAPPLER_CLASSIC_CARD_H
#define POM2_GRAPPLER_CLASSIC_CARD_H

#include "CentronicsPrinter.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

class GrapplerClassicCard : public SlotPeripheral
{
public:
    static constexpr std::size_t kRomBytes = 2048;

    explicit GrapplerClassicCard(int slot);

    int getSlot() const { return slot_; }
    bool loadRom(const std::vector<uint8_t>& bytes);
    bool romLoaded() const { return romLoaded_; }

    pom2::CentronicsPrinter* centronicsPrinter() override { return &printer_; }
    pom2::CentronicsPrinter& printer() { return printer_; }

    // ─── SlotPeripheral ─────────────────────────────────────────────────
    std::string_view name() const override { return "Grappler (Orange Micro, 1981)"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead      (uint8_t low8) override;
    uint8_t expansionRomRead (uint16_t offset) override;
    bool    takesC800() const override { return romLoaded_; }
    void    onReset() override;
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

private:
    int slot_;
    std::array<uint8_t, kRomBytes> rom_{};
    bool romLoaded_ = false;

    uint8_t dataLatch_ = 0;
    bool strobe_ = true;      // /STROBE line, high = released
    bool ackLatch_ = true;    // 1 = waiting for /ACK (MAME m_ack_latch)

    pom2::CentronicsPrinter printer_;

    void setStrobe(bool level);
};

#endif  // POM2_GRAPPLER_CLASSIC_CARD_H
