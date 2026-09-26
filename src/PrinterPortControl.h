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

// PrinterPortControl — the printer side of a slot, read and set from outside
// the guest: the Super Serial Card's DIP banks, cable and printer tap, and
// the Grappler+'s printer lines (SELECT, PAPER EMPTY, BUSY, a printer at all).
//
// One implementation behind three doors, so they cannot drift: the CLI
// (`--printer-port SLOT:k=v,…`), the AI control server (`/printer-port`) and
// any program linking the core library (A2 File Cmd's bench). Every entry
// point takes the `SlotBus&` — the caller holds the machine lock and proves it
// by handing the bus over. Nothing here touches a card register the way the
// guest does: reading a state never clears an IRQ or a latch.
//
// The option language, one `key=value` list, comma-separated:
//
//   Super Serial Card
//     mode=comm|printer|sicp8|sicp8a   SW1:5-6 (also rewrites DSW2, see
//                                      SuperSerialCard::setMode)
//     dsw1=0xNN  dsw2=0xNN             the raw banks, applied after `mode`
//     cable=auto|none|printer|printer-offline|modem|null-modem
//     tap=on|off                       feed the host ImageWriter
//   Grappler+, Grappler (1981), Apple Parallel Interface (any card whose
//   printer is a CentronicsPrinter)
//     online=on|off                    SELECT
//     paper=ok|out                     PAPER EMPTY (+ BUSY, no ACK)
//     printer=connected|none           a printer on the cable at all
//     busy=on|off                      BUSY (buffer full; delays the ACK)
//     type=0..7                        S1 printer-type switches (bits 6-4;
//                                      Grappler+ only)
//
// An option list is validated whole before anything changes: one bad key and
// the slot is left exactly as it was.

#ifndef POM2_PRINTER_PORT_CONTROL_H
#define POM2_PRINTER_PORT_CONTROL_H

#include <cstdint>
#include <string>

class SlotBus;

namespace pom2 {

struct PrinterPortState {
    int         slot = 0;
    /// "ssc" | "grappler" | "grappler1" | "pic" | "none" | the card's display
    /// name for any other.
    std::string card;

    // Super Serial Card.
    bool        firmware = false;      ///< Apple's EPROM loaded
    bool        builtInPort = false;   ///< a //c port: no EPROM, no switches
    uint8_t     dsw1 = 0xFF;
    uint8_t     dsw2 = 0xFF;
    std::string mode;                  ///< SuperSerialCard::modeKey
    std::string cable;                 ///< SuperSerialCard::cableKey
    bool        tap = false;
    bool        dcd = false, dsr = false, cts = false;   ///< true = active

    // Grappler+.
    bool        romLoaded = false;
    bool        online = true;
    bool        paperOut = false;
    bool        printerConnected = true;
    bool        busy = false;
    uint8_t     printerType = 0;
    /// BUSY | PAPER EMPTY | SELECT as the status byte carries them.
    uint8_t     lineBits = 0;
};

/// What `slot` (1-7) holds, printer-wise. Never fails: an empty slot or an
/// unrelated card comes back with `card` naming it and nothing else set.
PrinterPortState describePrinterPort(SlotBus& bus, int slot);

/// One JSON object, the shape `/printer-port` answers with.
std::string printerPortJson(const PrinterPortState& state);

/// Apply an option list (see the header comment). Returns false with `error`
/// set, and changes nothing, on an unknown key, a bad value or a key the card
/// in `slot` does not have.
bool applyPrinterPortOptions(SlotBus& bus, int slot, const std::string& options,
                             std::string& error);

/// `SLOT:options` — the CLI form. Splits and range-checks the slot.
bool parsePrinterPortSpec(const std::string& spec, int& slot,
                          std::string& options, std::string& error);

}  // namespace pom2

#endif  // POM2_PRINTER_PORT_CONTROL_H
