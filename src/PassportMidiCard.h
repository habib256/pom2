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

// PassportMidiCard — Passport Designs' MIDI Interface for the Apple II (and
// its Yamaha / clone cousins): an MC6840 timer and an MC6850 ACIA on the
// $C0nX window, no ROM. Catalog key `passportmidi`. Ultima V, Master Tracks
// and Passport's own sequencers find it in slot 2.
//
// [D] MAME bus/a2bus/a2midi.cpp @ mame0287 (bda3b2e5), which this follows:
//   $C0n0-$C0n7  the 6840 (RS2..RS0 = A2..A0)
//   $C0n8-$C0n9  the 6850 (RS = A0: control/status, then data)
//   $C0nA-$C0nF  nothing; reads 0 ([F] MAME's "drum sync (?)" at A-B;
//                Apple2TS also answers $C0nC/D, for the SD MIDI ][+ clone,
//                not modelled here).
//   the ACIA clocked at 31 250 × 16 = 500 kHz (÷16 -> MIDI's 31 250 baud);
//   the PTM on the bus 1 MHz, internal or "external" — MAME wires all three
//   C inputs to the same 1 MHz;
//   RESET reaches the PTM, not the ACIA ("ACIA has no reset input").
// Two divergences from that source, on purpose:
//   * the /IRQ is the OR of both chips. a2midi.cpp's ptm_irq_w stores the
//     PTM's line into m_acia_irq — a copy of acia_irq_w — so in MAME a
//     timer interrupt can be dropped by the ACIA's next transition and the
//     ACIA's held one forgotten. Both chips' IRQ pins are open-drain onto
//     the one slot line ([B] each data sheet).
//   * the ACIA's transmitter takes real time (Mc6850Acia.h): 320 µs per
//     8N1 byte at 31 250 baud, during which TDRE stays low for the next one.
//
// Output: every character the ACIA finishes sending is kept, stamped with
// the CPU cycle it ended at, for the host to drain (PassportMidiCard::
// drainMidiOut -> MidiOutputCoordinator). The card does not know what plays
// it — a host MIDI port, a .mid recording — the way a printer card does not
// know the ImageWriter.

#ifndef POM2_PASSPORT_MIDI_CARD_H
#define POM2_PASSPORT_MIDI_CARD_H

#include "Mc6840Ptm.h"
#include "Mc6850Acia.h"
#include "SlotBusClock.h"
#include "SlotPeripheral.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string_view>
#include <vector>

namespace pom2 {

struct MidiOutByte {
    uint64_t cycle;   ///< the card's CPU-cycle clock when the stop bit ended
    uint8_t  byte;
};

class PassportMidiCard : public SlotPeripheral
{
public:
    /// [D] MAME a2midi.cpp: CLOCK(config, "acia_clock", 31250*16).
    static constexpr double kAciaClockHz = 31250.0 * 16.0;
    /// Bytes kept for the host before the oldest are dropped (a frozen UI).
    static constexpr std::size_t kMaxPending = 65536;

    explicit PassportMidiCard(int slot);

    /// Everything sent since the last drain, oldest first. Under the
    /// machine lock, like the printer spools.
    void drainMidiOut(std::vector<MidiOutByte>& out);
    /// Bytes dropped because nobody drained (a counter for the panel).
    uint64_t droppedBytes() const { return dropped_; }
    /// The card's clock (CPU cycles it has been advanced through).
    uint64_t cycles() const { return cycles_; }

    /// A byte from MIDI IN (no host input is wired yet; tests use it).
    void receiveMidi(uint8_t b);

    Mc6840Ptm&  ptm()  { return ptm_; }
    Mc6850Acia& acia() { return acia_; }

    // ─── SlotPeripheral ─────────────────────────────────────────────────
    std::string_view name() const override { return "Passport MIDI"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    void    advanceCycles(int cycles) override;
    void    onReset() override;
    void    onPowerCycle() override;
    void    setCpuClock(double hz) override { busClock_.setCpuClock(hz); }
    void    setStandardClock(double hz) override;
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

private:
    void syncTo(uint64_t cpuCycle);
    void updateIrq();

    Mc6840Ptm  ptm_;
    Mc6850Acia acia_;
    SlotBusClock busClock_;
    double   busHz_ = 1022727.0;
    uint64_t cycles_ = 0;      ///< advanced through advanceCycles
    uint64_t synced_ = 0;      ///< the chips have been run up to this cycle
    double   busAtSync_ = 0.0; ///< the ACIA's bus time at `synced_`
    std::vector<Mc6850Acia::Sent> sent_;
    std::deque<MidiOutByte> pending_;
    uint64_t dropped_ = 0;
};

} // namespace pom2

#endif
