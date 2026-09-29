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

// Pascal 1.1 firmware protocol for POM2's hand-assembled PRINTER pages
// (PrinterCard, the Grappler+ stub without its dump).
//
// A page that publishes the Pascal 1.1 signature ($Cn05=$38, $Cn07=$18,
// $Cn0B=$01) promises the four entry offsets at $Cn0D-$Cn10: a caller that
// trusts the signature forms $Cn00|offset and jumps. Both pages used to
// publish the signature with the NOP fill there, so PINIT ran a sled at
// $CnEA off the end of the page, and $Cn0C said class 0 ("reserved")
// where a printer is class 1 (docs/printer-detection.md; the real Grappler
// reads $14). Bug hunt 2026-09-29.
//
// Calling convention (Apple II Pascal 1.1 firmware protocol, same as the
// SSC page's): X = error code on return (0 = OK, 3 = illegal operation);
// PWRITE sends A; PSTATUS takes A=0 "ready for output?" / A=1 "input
// available?" and answers in carry (set = ready).

#ifndef POM2_PASCAL_PRINTER_ROM_H
#define POM2_PASCAL_PRINTER_ROM_H

#include "SlotRomAsm.h"

#include <cstdint>

namespace pom2 {

/// Class byte for $Cn0C: class 1 (printer) in the high nibble.
constexpr uint8_t kPascalPrinterClass = 0x10;

/// Assemble the $Cn0D-$Cn10 entry table and the four routines, placed in
/// [start, limit). `dataLo` is the low byte of the card's $C0nX data port.
inline void assemblePascalPrinterEntries(SlotRomAsm& a, uint8_t dataLo,
                                         unsigned start, unsigned limit)
{
    a.region("pascalTable", 0x0D, 0x11)
     .byteOf("pinit").byteOf("pread").byteOf("pwrite").byteOf("pstatus");

    a.region("pascalEntries", start, limit)
     .label("pinit")
     .emit({ 0xA2, 0x00,              // LDX #$00    (no error)
             0x60 })                  // RTS
     .label("pread")                  // a printer has no input
     .emit({ 0xA9, 0x00,              // LDA #$00
             0xA2, 0x03,              // LDX #$03    (illegal operation)
             0x60 })
     .label("pwrite")
     .emit({ 0x8D, dataLo, 0xC0,      // STA $C0nX   (data port)
             0xA2, 0x00,              // LDX #$00
             0x60 })
     .label("pstatus")
     .emit({ 0xA2, 0x00,              // LDX #$00
             0xC9, 0x01 })            // CMP #$01    (input status?)
     .branch(0xF0, "psNoInput")       // BEQ psNoInput
     .emit({ 0x38, 0x60 })            // SEC / RTS   (output always ready)
     .label("psNoInput")
     .emit({ 0x18, 0x60 });           // CLC / RTS   (never any input)
}

}  // namespace pom2

#endif  // POM2_PASCAL_PRINTER_ROM_H
