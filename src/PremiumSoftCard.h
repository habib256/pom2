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

// PremiumSoftCard — Microsoft Premium SoftCard //e (1983, designed by
// Burtronix): the //e's extended 80-column card (64 KB aux RAM, the 80
// columns) with a Z80B on the same RAM, in the AUXILIARY slot. No emulator
// had it, no schematic or programmer's manual is online; the model is read
// off Microsoft's own software — CP/M 2.25 (`Softcard //e CP/M 64K Version
// 2.25`), its 6502 BIOS and its Z80 BIOS — and the Installation and
// Operation Manual (§ "Differences Between the Premium SoftCard IIe and
// Previous SoftCards", pp. 72-74):
//
//  - "64K bytes of RAM which can be accessed by either the Z80 or the 6502":
//    the Z80's 64 KB IS the aux RAM, by physical address
//    (Memory::auxPhysical — $0000-$BFFF the aux bank, $C000-$FFFF the aux
//    language card's cells). Both CPUs run at once: the 6502 is the I/O
//    processor ("type-ahead buffer while CP/M performs other operations"),
//    the Z80 runs CP/M.
//  - "A Z80B ... three times as fast as" the SoftCard's 2.04 MHz, "not
//    synchronized or phase-locked to the Apple IIe internal clocks": 6 MHz,
//    its own crystal, timed here in T-states against the CPU clock. Memory
//    contention with the 6502 and the video is not modelled.
//  - **The Z80 calls the 6502** — "storing information in a 7-byte area
//    located at 45-4B, and then performing a Z80 CALL instruction to memory
//    location 40" (p. 74). $0040 is `LD A,$80 / LD I,A / XOR A / LD I,A /
//    RET` (Z80 BIOS $FBB5), and a console character goes the same way with
//    I = $FF ($FB89). The card watches the refresh address — the Z80 puts I
//    on A8-A15 there — and holds the Z80 the moment I's bit 7 comes up.
//  - **$C07E** (write; the card watches the bus — the address is IOUDIS on
//    a //c, nothing on a //e), the bits the software uses:
//        bit 0  1 = the Z80 out of RESET (0 = held in reset)
//        bit 2  1 = the 6502's aux accesses see A15 inverted, so the 6502
//               reaches the Z80's $C000-$FFFF (its $4000-$7FFF) and $0000-
//               $1FFF (its $8000-$9FFF) — the 6502 BIOS loads the Z80 BIOS
//               at 6502 $6300 = Z80 $E300 and writes `JP $FA00` at 6502
//               $8000 = Z80 $0000 this way (6502 BIOS $D6A5, $D913, $D37C)
//        bit 6  1 = run: the Z80 may go, a pending call is released, and
//               RAMRD reads of aux $2000 return the status byte below
//    The 6502 BIOS writes $05/$01 while loading, $04 then $00 to reset the
//    Z80 onto its fresh vector, $41 to start it, $01 when it takes a call
//    and $41 to return from one.
//  - **Status** (6502 BIOS $D97D: `STA $C003 / LDA $2000 / STA $C002`):
//    the Z80's I register as the refresh cycles show it — bit 7 = a call is
//    waiting, bits 3-5 set ($FF) = the console-output fast path ($10D5).
//    That the byte answers at aux $2000 only while bit 6 is set is the
//    reading that lets the 6502 BIOS also move CP/M data through Z80 $2000
//    with the card stopped ($01).
//  - **The 80-column text page is not the Z80's.** Two AM9114 (1K × 4)
//    SRAMs on the board hold aux $0400-$07FF: the 6502's plain aux accesses
//    there and the video see them, while the Z80's $0400-$07FF — CP/M's TPA,
//    where GBASIC's code sits — is DRAM, which the 6502 reaches only through
//    the A15 flip (6502 $8400-$87FF). That is why the 6502 BIOS flips for
//    every Z80 page below $20 ($D6A5), and modelled as `dram0400_`: with the
//    page shared, GBASIC's own strings scrolled onto the screen and its
//    arithmetic went wrong (6 * 7 = 6).
//  - No Z80 I/O ports, no interrupt either way ("Z80 interrupts to the
//    6502 are not allowed"). A bus RESET clears the register.
//
// The card sits on SlotBus SLOT 0: the //e has no slot 0 (the language card
// is built in, and Memory decodes $C080-$C08F itself), so the bus's slot 0
// is free to stand for the auxiliary slot — the card gets the bus's
// per-instruction pacing, reset and snapshot fan-out, at no cost to anyone.

#ifndef POM2_PREMIUM_SOFTCARD_H
#define POM2_PREMIUM_SOFTCARD_H

#include "Memory.h"
#include "SlotPeripheral.h"
#include "Z80.h"

#include <array>
#include <cstdint>
#include <vector>

class PremiumSoftCard : public SlotPeripheral, public Memory::AuxSlotPort,
                        private pom2::Z80Bus
{
public:
    static constexpr double kZ80Hz = 6'000'000.0;
    /// The SlotBus slot that stands for the //e auxiliary slot.
    static constexpr int kAuxSlot = 0;

    static constexpr uint8_t kRunOutOfReset = 0x01;
    static constexpr uint8_t kFlipA15       = 0x04;
    static constexpr uint8_t kRun           = 0x40;

    PremiumSoftCard();

    /// The //e whose aux RAM the card is. Wire before plugging.
    void setMemory(Memory* mem) { mem_ = mem; }

    std::string_view name() const override { return "Premium SoftCard //e"; }

    void onPlug() override   { if (mem_) mem_->setAuxSlotPort(this); }
    void onUnplug() override { if (mem_ && mem_->auxSlotPort() == this) mem_->setAuxSlotPort(nullptr); }
    void onReset() override;
    void advanceCycles(int cycles) override;
    void setCpuClock(double hz) override { if (hz > 0.0) { cpuHz_ = hz; } }

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // Memory::AuxSlotPort
    void c07eWrite(uint8_t value) override;
    bool flipsA15() const override { return (reg_ & kFlipA15) != 0; }
    bool answersStatus(uint8_t& value) const override;
    bool divertsAux() const override { return (reg_ & (kFlipA15 | kRun)) != 0; }
    uint8_t* ownCell(uint16_t phys) override
    {
        return (phys >= 0x0400 && phys < 0x0800) ? &dram0400_[phys - 0x0400] : nullptr;
    }

    // Test/debug hooks.
    const pom2::Z80& z80() const { return z80_; }
    pom2::Z80&       z80()       { return z80_; }
    uint8_t reg() const { return reg_; }
    bool callPending() const { return callPending_; }
    bool z80Running() const
    {
        return (reg_ & kRunOutOfReset) && (reg_ & kRun) && !callPending_;
    }

private:
    uint8_t z80MemRead(uint16_t addr) override { return mem_ ? mem_->auxPhysical(addr) : 0xFF; }
    void    z80MemWrite(uint16_t addr, uint8_t v) override { if (mem_) mem_->auxPhysical(addr) = v; }

    void publish() { if (mem_ && mem_->auxSlotPort() == this) mem_->refreshAuxSlotPort(); }

    Memory* mem_ = nullptr;
    pom2::Z80 z80_;
    /// The DRAM under the text-page SRAM (see the header note).
    std::array<uint8_t, 0x400> dram0400_{};
    uint8_t reg_ = 0;
    bool callPending_ = false;   // held on a refresh with I bit 7 newly set
    bool iHigh_ = false;         // I bit 7 as of the last instruction
    double cpuHz_ = 1'022'727.0;
    double tOwed_ = 0.0;
};

#endif // POM2_PREMIUM_SOFTCARD_H
