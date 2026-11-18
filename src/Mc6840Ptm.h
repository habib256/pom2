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

// Mc6840Ptm — Motorola MC6840 Programmable Timer Module: three 16-bit
// down-counters, their latches, three control registers and a status
// register. Used by the Passport MIDI card (PassportMidiCard), where its
// timers pace the music.
//
// [B] Motorola MC6840 data sheet (MC6840/D, "Device Operation", pp. 6-12),
// the source of everything here:
//   * Table 1 — register select: RS0 write CR1 (CR20=1) / CR3 (CR20=0),
//     read: no operation; RS1 write CR2 / read status; RS2/4/6 write the
//     MSB buffer / read timer 1/2/3's counter, which latches its LSB into
//     the LSB buffer; RS3/5/7 write timer 1/2/3's latches (MSB buffer +
//     the byte) / read the LSB buffer.
//   * Table 2 — CRX1 clock (1 = E), CRX2 dual 8-bit, CRX3-5 mode, CRX6
//     interrupt enable, CRX7 output enable; CR10 holds every timer preset,
//     CR30 prescales timer 3 by 8.
//   * Status: bits 0-2 the individual flags, bit 7 = I1·CR16 + I2·CR26 +
//     I3·CR36. A flag is cleared by a timer reset, by a Write Timer Latches
//     or a Counter Initialization of its timer, and by reading its counter
//     after the status was read with the flag set (the RS-RT sequence).
//   * Counter Initialization (latches -> counter, flag cleared) on RESET,
//     CR10 = 1, a Write Timer Latches when CRX4 = 0, or a gate fall.
//   * A Time Out is the first clock after the counter reached zero: the
//     flag sets and the counter reloads. 16-bit: every N+1 clocks. Dual
//     8-bit: every (L+1)(M+1) — the LSB counts L..0, then reloads as the
//     MSB steps down. Single-shot counts the same way (only its output and
//     its gate independence differ).
//   * RESET: latches $FFFF, every control bit clear except CR10, counters
//     preset, flags clear.
// [D] MAME 6840ptm.cpp @ mame0287 (bda3b2e5): same register map; it reads
// RS0 as 0, as POM2 does ([F] the chip leaves its bus undriven there).
//
// Not modelled, deliberately:
//   * the outputs O1-O3 — nothing a Passport program can read depends on
//     them ([F] where the card routes them is unknown; settled by its
//     schematic);
//   * gate edges — the card's gates are taken as held low ([F], same
//     settlement). So continuous and single-shot modes count, and the two
//     measurement modes (CRX3 = 1), which only count after a gate fall,
//     stay still and never time out;
//   * the three-E-cycle synchronisation of an external clock. MAME feeds
//     the Passport's C1-C3 from the bus 1 MHz, the E clock itself; a
//     constant three-cycle start offset is below what the card exposes.
// The clock is the bus phase-2 E clock, counted by the owner in bus ticks.

#ifndef POM2_MC6840_PTM_H
#define POM2_MC6840_PTM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

class Mc6840Ptm
{
public:
    Mc6840Ptm() { reset(); }

    /// The RESET input: latches $FFFF, CR10 set, everything else cleared.
    void reset();

    /// Let `eClocks` E cycles go by.
    void tick(uint64_t eClocks);

    uint8_t read(uint8_t rs);           ///< rs = RS2..RS0, 0-7
    void    write(uint8_t rs, uint8_t v);
    /// The status register without the read's side effect (debugger, tests).
    uint8_t peekStatus() const { return status(); }

    /// The /IRQ line (true = asserted).
    bool irq() const { return (status() & 0x80) != 0; }

    /// E cycles until the next flag can set, or UINT64_MAX when no timer
    /// runs. Lets an owner know how far it may let time go unobserved.
    uint64_t clocksToNextTimeOut() const;

    uint16_t latch(int t)   const { return t_[t].latch; }
    uint16_t counter(int t) const { return t_[t].counter; }
    uint8_t  control(int t) const { return t_[t].cr; }

    void appendState(std::vector<uint8_t>& out) const;
    bool loadState(const uint8_t* data, std::size_t len);
    static constexpr std::size_t kStateBytes = 3 * 5 + 4;

private:
    struct Timer {
        uint16_t latch   = 0xFFFF;
        uint16_t counter = 0xFFFF;
        uint8_t  cr      = 0;
        uint8_t  prescale = 0;    ///< timer 3's ÷8 phase
    };

    uint8_t status() const;
    bool    counting(int t) const;
    void    initializeCounter(int t);
    void    tickTimer(int t, uint64_t clocks);
    uint64_t clocksToTimeOut(int t) const;   ///< in counter clocks

    std::array<Timer, 3> t_{};
    uint8_t flags_ = 0;          ///< bits 0-2
    uint8_t readWithFlag_ = 0;   ///< flags seen by a status read (RS-RT)
    uint8_t msbBuffer_ = 0;
    uint8_t lsbBuffer_ = 0;
};

} // namespace pom2

#endif
