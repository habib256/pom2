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

// AppleParallelCard — the Apple II Parallel Interface Card (1979, 670-0012),
// a port of MAME `bus/a2bus/a2pic.cpp` (catalog key `pic`).
//
// A 512-byte PROM (341-0057) holding the two firmwares the SW1:6 switch picks
// between: "Parallel Printer" (341-0005 behaviour, adds LF after CR; the
// default) and "Centronics" (341-0019, no LF). It has NO Pascal 1.1 signature
// — $Cn05/$Cn07 read $48/$48 — which is what makes it the card A2 File Cmd
// has to recognise by other bytes (docs/printer-detection.md). The Epson APL
// and the Fourth Dimension card carry byte-identical firmware.
//
// Registers ($C0n0-$C0n7, A3 ignored — a2pic.cpp:210-283):
//   read  n3  status: $97 | PE<<5 | SELECT<<6 | /FAULT<<3
//   read  n4  ACK latch<<7 | open bus & $7E | /ACK input
//   n6        enable the ACK interrupt (read or write)
//   n7        reset mode: autostrobe off, IRQ off, ACK latch set
//   write n0  latch data, strobe (if autostrobe is on)
//   write n2  strobe again with the latched data
// Any $CnXX access turns autostrobe on (a2pic.cpp:294-299). PROM addressing
// is MAME's default "Standard (X2)" jumper, which swaps A6 so that $Cn00 lands
// on the entry code and $CnC0-$CnFF follow the ACK latch (a2pic.cpp:286-293).
//
// The printer on the cable is a `CentronicsPrinter`: instant when ready,
// holding the byte (no ACK) while off line, out of paper or absent.
// Not modelled: the strobe-length timer and the strobe/ACK polarity switches
// (MAME defaults: 1 µs, negative), the 500 ns strobe at n5 (MAME logs it as
// unimplemented too), and the X3/X5 jumpers (data output on, 8-bit).

#ifndef POM2_APPLE_PARALLEL_CARD_H
#define POM2_APPLE_PARALLEL_CARD_H

#include "CentronicsPrinter.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class AppleParallelCard : public SlotPeripheral
{
public:
    static constexpr std::size_t kPromBytes = 512;

    explicit AppleParallelCard(int slot);

    int getSlot() const { return slot_; }

    /// The 341-0057 PROM, exactly 512 bytes. The caller reads the file.
    bool loadProm(const std::vector<uint8_t>& bytes);
    bool promLoaded() const { return promLoaded_; }

    /// SW1:6 — true = "Parallel Printer" firmware (PROM $100-$1FF, MAME's
    /// default), false = "Centronics" ($000-$0FF). Takes effect on reset, as
    /// on the card (a2pic.cpp:364-365).
    void setParallelPrinterFirmware(bool on) { sw6ParallelPrinter_ = on; }
    bool parallelPrinterFirmware() const { return sw6ParallelPrinter_; }

    pom2::CentronicsPrinter* centronicsPrinter() override { return &printer_; }
    pom2::CentronicsPrinter& printer() { return printer_; }

    // ─── SlotPeripheral ─────────────────────────────────────────────────
    std::string_view name() const override { return "Apple Parallel Interface"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead      (uint8_t low8) override;
    void    slotRomWrite     (uint8_t low8, uint8_t v) override;
    void    onReset() override;
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

private:
    int slot_;
    std::array<uint8_t, kPromBytes> prom_{};
    bool promLoaded_ = false;
    bool sw6ParallelPrinter_ = true;
    uint16_t firmwareBase_ = 0x100;

    uint8_t dataLatch_ = 0xFF;
    bool autostrobeDisable_ = true;
    bool ackLatch_ = true;
    bool irqEnable_ = false;

    pom2::CentronicsPrinter printer_;

    void resetMode();
    void setAckLatch();
    void clearAckLatch();
    void strobe(uint8_t byte);
    void updateIrq();
};

#endif  // POM2_APPLE_PARALLEL_CARD_H
