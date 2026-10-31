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

// SoftCardIICard — Microsoft Z-80 SoftCard II: a 6 MHz Z80 with 64 KB of its
// own, Microsoft's answer to the PCPI Applicard. Unlike the SoftCard
// (SoftCardZ80.h) it is NOT a bus master: both CPUs run all the time, each
// on its own memory, and they talk through an AMD Am2950 8-bit
// bidirectional latch. MAME has no model; this is a port of izapple2's
// `cardZ80Softcard2.go` (Iván Izaguirre, GPL-3.0), checked by booting
// Microsoft's CP/M 2.28B for the card to its `A>` prompt.
//
//  - **6502 side**, the card's DEVSEL $C0n0-$C0nF (only A0-A1 decoded):
//        read  $C0n0  the byte from the Z80 (empties that latch)
//        read  $C0n1  status: bit 7 = a byte from the Z80 waits,
//                             bit 0 = the byte for the Z80 is not taken yet
//        write $C0n0  a byte for the Z80
//        write $C0n3  resets the Z80 (and the latches, back to boot mode)
//  - **Z80 side**, any port, A0 decoded:
//        IN  even     the byte from the 6502 (empties that latch)
//        IN  odd      status, mirrored: bit 7 = a byte from the 6502 waits,
//                     bit 0 = the byte for the 6502 is not taken yet
//        OUT even     a byte for the 6502
//  - **Boot mode**, from reset to the first OUT: the card's RAM holds
//    nothing, so every Z80 read below $8000 — opcode fetches included —
//    comes from the 6502's latch, and a write below $8000 goes to the
//    latch back to the 6502 only — the simplest decode, sbabcoc's
//    SoftCard2 reconstruction; izapple2 also stores it in RAM, which no
//    known code can tell apart. The 6502 thereby feeds the Z80 a
//    loader one byte at a time. A read with the latch empty holds the Z80
//    (on the card, its WAIT line) until the 6502 writes; here the
//    instruction is undone and re-run on the next step, the bytes it had
//    already taken replayed so none is consumed twice. The first OUT ends
//    boot mode and is not itself delivered.
//  - **Clock**: the card's own 6 MHz crystal, timed in T-states against the
//    elapsed CPU cycles (the live CPU clock, so an accelerator on the 6502
//    does not speed the Z80 up). A held Z80 keeps losing its time.

#ifndef POM2_SOFTCARD_II_CARD_H
#define POM2_SOFTCARD_II_CARD_H

#include "SlotPeripheral.h"
#include "SoftCard2Protocol.h"
#include "Z80.h"

#include <array>
#include <cstdint>
#include <vector>

class SoftCardIICard : public SlotPeripheral, private pom2::Z80Bus
{
public:
    static constexpr double kZ80Hz = 6'000'000.0;

    SoftCardIICard();

    std::string_view name() const override { return "SoftCard II"; }
    pom2::Z80* z80Core() override { return &z80_; }

    uint8_t deviceSelectRead(uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    /// The bus RESET line resets the Z80 as $C0n3 does.
    void onReset() override { resetZ80(); }
    void advanceCycles(int cycles) override;
    void setCpuClock(double hz) override { if (hz > 0.0) { cpuHz_ = hz; } }

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    /// Am2950 status byte: bit 7 = `readable`, bit 0 = `writePending`.
    static uint8_t status(bool readable, bool writePending)
    {
        return uint8_t((readable ? 0x80 : 0) | (writePending ? 0x01 : 0));
    }

    // Test/debug hooks.
    const pom2::Z80& z80() const { return z80_; }
    pom2::Z80&       z80()       { return z80_; }
    bool booting() const { return booting_; }
    uint8_t cardRam(uint16_t a) const { return ram_[a]; }

private:
    uint8_t z80MemRead(uint16_t addr) override;
    uint8_t z80Peek(uint16_t addr) override;
    void    z80MemWrite(uint16_t addr, uint8_t v) override;
    uint8_t z80IoRead(uint16_t port) override;
    void    z80IoWrite(uint16_t port, uint8_t v) override;

    void resetZ80();
    /// One instruction; false when it was held for the 6502 and undone.
    bool stepZ80(int& tStates);

    pom2::Z80 z80_;
    /// The 6502 BIOS calls on the mailbox, decoded for the OS-call trace —
    /// fed only while the trace hangs an observer on the Z80.
    pom2::SoftCard2Protocol proto_{ [this](const std::string& l) {
        if (auto* o = z80_.observer()) o->onCardLine(l.c_str()); } };
    std::array<uint8_t, 0x10000> ram_{};
    bool booting_ = true;

    // The Am2950's two latches.
    uint8_t toZ80_       = 0;
    bool    toZ80Full_   = false;
    uint8_t fromZ80_     = 0;
    bool    fromZ80Full_ = false;

    // Boot-mode hold: bytes the undone instruction took, replayed on retry.
    bool stalled_ = false;
    std::vector<uint8_t> fetched_;
    std::size_t fetchPos_ = 0;

    double cpuHz_ = 1'022'727.0;
    double tOwed_ = 0.0;   // Z80 T-states the elapsed CPU time pays for, not yet run
};

#endif // POM2_SOFTCARD_II_CARD_H
