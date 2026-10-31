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

// ApplicardCard — PCPI Appli-Card: a 6 MHz Z80B with 64 KB of its own and a
// 2 KB boot ROM, running beside the 6502 (no bus mastery, no shared memory).
// Ported from MAME `src/devices/bus/a2bus/a2applicard.cpp` (R. Belmont,
// BSD-3-Clause), with the port decode of izapple2's `cardApplicard.go`
// (GPL-3.0), which follows the Applicard OEM manual chapter 4.
//
//  - **6502 side**, DEVSEL $C0n0-$C0n7:
//        read  $C0n0  the byte from the Z80 (empties that latch)
//        r/w   $C0n1  write: a byte for the Z80; read: that byte back
//        read  $C0n2  bit 7 = the byte for the Z80 is not taken yet
//        read  $C0n3  bit 7 = a byte from the Z80 waits
//        any   $C0n5  resets the Z80 (ROM back on, both latches empty)
//        any   $C0n6  Z80 interrupt through the CTC: no CTC is fitted, nothing
//        any   $C0n7  Z80 NMI
//  - **Z80 ports**, each repeated over its 32-port range (A5-A7 decoded):
//        OUT $00  a byte for the 6502        IN $20  the byte from the 6502
//        IN  $40  bit 7 = a byte from the 6502 waits, bit 0 = own byte not
//                 taken yet                  OUT $60  bit 0: ROM on / off
//        $80+     the CTC socket, empty: reads $FF
//    MAME decodes the exact ports $00/$20/$40/$60 only; the manual (and
//    izapple2) say each is repeated, which is what is modelled.
//  - **The ROM** is on at power-up and reset, repeated over $0000-$7FFF;
//    while it is on the low 32 KB of RAM is neither read nor written. The
//    RAM keeps its contents across a reset (the ROM skips its memory test
//    on a warm start).
//  - **Clock**: 12 MHz crystal / 2 = 6 MHz (MAME), timed in T-states
//    against the live CPU clock as the SoftCard II is.

#ifndef POM2_APPLICARD_CARD_H
#define POM2_APPLICARD_CARD_H

#include "SlotPeripheral.h"
#include "Z80.h"

#include <array>
#include <cstdint>
#include <vector>

class ApplicardCard : public SlotPeripheral, private pom2::Z80Bus
{
public:
    static constexpr double kZ80Hz = 6'000'000.0;
    static constexpr std::size_t kRomBytes = 2048;
    static constexpr const char* kRomPath = "roms/applicard_v9.bin";

    ApplicardCard();

    /// The 2 KB Z80 boot ROM (MAME `applicard-v9.bin`); false if not 2 KB.
    bool loadRom(const std::vector<uint8_t>& bytes);

    std::string_view name() const override { return "Applicard"; }
    pom2::Z80* z80Core() override { return &z80_; }

    uint8_t deviceSelectRead(uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    void onReset() override { resetZ80(); }
    void advanceCycles(int cycles) override;
    void setCpuClock(double hz) override { if (hz > 0.0) { cpuHz_ = hz; } }

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // Test/debug hooks.
    const pom2::Z80& z80() const { return z80_; }
    pom2::Z80&       z80()       { return z80_; }
    bool romOn() const { return romOn_; }

private:
    uint8_t z80MemRead(uint16_t addr) override;
    void    z80MemWrite(uint16_t addr, uint8_t v) override;
    uint8_t z80IoRead(uint16_t port) override;
    void    z80IoWrite(uint16_t port, uint8_t v) override;

    void resetZ80();
    /// $C0n5-$C0n7: the same on a read or a write.
    void control(uint8_t low4);

    pom2::Z80 z80_;
    std::array<uint8_t, kRomBytes> rom_{};
    std::array<uint8_t, 0x10000> ram_{};
    bool romOn_ = true;

    uint8_t toZ80_       = 0;
    bool    toZ80Full_   = false;
    uint8_t fromZ80_     = 0;
    bool    fromZ80Full_ = false;

    double cpuHz_ = 1'022'727.0;
    double tOwed_ = 0.0;
};

#endif // POM2_APPLICARD_CARD_H
