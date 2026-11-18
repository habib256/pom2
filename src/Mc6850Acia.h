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

// Mc6850Acia — Motorola MC6850 ACIA with a TIMED transmitter: a character
// takes its frame's bit times to leave, and the Transmit Data Register only
// empties when the shift register takes it. The Passport MIDI card's 31 250
// baud line (PassportMidiCard) is the reason: a MIDI program paces itself on
// TDRE, and a line that is "infinitely fast" (Ccs7710Card's model, right for
// a terminal) hands it a byte every poll.
//
// [B] Motorola MC6850 data sheet (May 1975), "Device Operation" and
// Table 1 (pp. 5-8):
//   * control CR1:0 — ÷1, ÷16, ÷64, 11 = master reset (clears the status
//     but CTS/DCD, initialises receiver and transmitter); CR4:2 word select
//     (7E2, 7O2, 7E1, 7O1, 8N2, 8N1, 8E1, 8O1); CR6:5 — 00 RTS low, 01 RTS
//     low + transmit interrupt, 10 RTS high, 11 RTS low + a break level;
//     CR7 receive interrupt.
//   * status b0 RDRF, b1 TDRE, b2 DCD, b3 CTS, b4 FE, b5 OVRN, b6 PE, b7 IRQ.
//   * TDR: "If the transmitter is idling … the transfer will take place
//     within one bit time of the trailing edge of the Write command. If a
//     character is being transmitted, the new data character will commence
//     as soon as the previous character is complete." TDRE goes high at the
//     transfer.
//   * 7-bit words: data bit 7 is "don't care"; the parity bit follows the
//     last data bit.
// [F] Where in that first bit time the transfer lands depends on the phase
// of the free-running bit clock; POM2 transfers at the write. Settled by: a
// scope on TxD against the write strobe.
//
// CTS and DCD are taken as wired active (low), as on the Passport card
// ([F] no schematic; MAME leaves both inputs at their defaults).
//
// Time is the owner's: `tick` takes bus ticks, `setTiming` says how many bus
// ticks one transmit clock cycle lasts. Characters that finish leaving are
// handed back with the bus time they ended at, as the byte an 8N1 receiver
// (a MIDI IN) assembles from the wire.

#ifndef POM2_MC6850_ACIA_H
#define POM2_MC6850_ACIA_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pom2 {

class Mc6850Acia
{
public:
    struct Sent {
        double  at;      ///< bus time (ticks since construction) the stop bit ended
        uint8_t byte;    ///< what an 8N1 receiver read off the wire
        bool    framed;  ///< false: the receiver saw a framing error (no stop bit)
    };

    /// Bus ticks per cycle of the transmit/receive clock input.
    void setTiming(double busTicksPerTxClock);

    /// Power on: held in master reset, as the data sheet's power-on logic.
    void powerOn();

    uint8_t status() const;
    uint8_t readData();
    void    writeControl(uint8_t v);
    void    writeData(uint8_t v);
    /// A character from the far end of the line.
    void    receive(uint8_t v);

    /// Advance `busTicks`; characters whose stop bit ends are appended.
    void tick(double busTicks, std::vector<Sent>& out);

    bool irq() const { return (status() & 0x80) != 0; }
    bool inReset() const { return (control_ & 0x03) == 0x03; }
    uint8_t control() const { return control_; }
    double now() const { return now_; }
    /// Bus ticks one character of the current word format takes.
    double frameTicks() const;

    void appendState(std::vector<uint8_t>& out) const;
    bool loadState(const uint8_t* data, std::size_t len);

private:
    void startShift(double at);
    double bitTicks() const;
    int frameBits() const;
    Sent wireByte(uint8_t v, double at) const;

    double  txClock_ = 1.0;     // bus ticks per transmit clock cycle
    double  now_ = 0.0;
    uint8_t control_ = 0x03;
    uint8_t tdr_ = 0;
    bool    tdrFull_ = false;
    bool    shifting_ = false;
    uint8_t shiftByte_ = 0;
    double  shiftEnd_ = 0.0;
    uint8_t rdr_ = 0;
    bool    rdrf_ = false;
    bool    overrun_ = false;
    std::deque<uint8_t> rx_;
};

} // namespace pom2

#endif
