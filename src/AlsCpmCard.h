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

// AlsCpmCard — Advanced Logic Systems "The CP/M Card" (1982): a 6 MHz Z80B
// with 64 KB of its own, running ALS's CP/M Plus. No emulator had it; there
// is no schematic or register list in print. This model is read off ALS's
// own software (CP/M 3.01C, CPM3.SYSTEM / ZBOOT / CPM3.SYS — disassembly
// notes in docs/backlog/cards.md, CARDS-011) and its User's Guide § 5.
//
//  - **Bus mastery** works as on the SoftCard: any write to the card's
//    $Cn00-$CnFF page, by either CPU, toggles the Z80 on or off; while it
//    runs the 6502 is halted (the manual's "DMA device"). The first grant
//    after a reset starts the Z80 at $0000.
//  - **Two banks** chosen by `OUT`, data bit 0 (BIOS SELMEM, $F944: $F0
//    for bank 0, $FF for bank 1, written to port $00 — the manual's
//    "0F0H" is the value). The port number is not decoded here.
//  - **Bank 0 is the Apple**, through the SoftCard's map with Z80 A15
//    inverted, below $E000:
//        Z80 $0000-$2FFF → Apple $9000-$BFFF
//        Z80 $3000-$5FFF → Apple $D000-$FFFF   (language card / ROM)
//        Z80 $6000-$6FFF → Apple $C000-$CFFF   (I/O, slot ROMs)
//        Z80 $7000-$7FFF → Apple $0000-$0FFF
//        Z80 $8000-$DFFF → Apple $1000-$6FFF
//    Evidence: CPM3.SYSTEM loads ZBOOT at Apple $9000 and CPMLDR at $9100
//    and the Z80 runs them at $0000 and $0100; the 6502/Z80 call page
//    lives at Apple $0300 and is entered at Z80 $7300; ZBOOT tells the ALS
//    from a SoftCard by finding no RAM at Z80 $5000 (Apple $F000, ROM);
//    CPM3.SYS's banked BDOS/BIOS sit at $2B00-$5FFF, i.e. Apple $B000 and
//    the language card.
//  - **Bank 1** is card RAM $0000-$DFFF (56 KB of TPA); **$E000-$FFFF**
//    is card RAM common to both banks (CP/M's resident BDOS/BIOS at $F200).
//    56 + 8 = the card's 64 KB. Apple $7000-$8FFF is not reachable from the
//    Z80 (ALS keeps its 6502 RWTS at $8800).
//  - **Real-time clock**: a 60 Hz interrupt (the BIOS handler at $FB94
//    counts 60 ticks to the second), RST 30 in IM 0, acknowledged by the
//    interrupt cycle — the BIOS reads no port. The BIOS only enables
//    interrupts in bank 1, where it puts its vectors at $0030 and $0038.
//  - **Speed**: 6 MHz on its own RAM. An access to the Apple is a DMA cycle
//    on the Apple's bus: it waits for the next bus-cycle edge and then takes
//    that whole cycle (~0.98 µs) where its own 3 T-states would take 0.5 —
//    the manual's "2 MHz for the instant it directly controls" the Apple.
//    Timing is kept in Apple bus cycles, with the fraction of the cycle the
//    Z80 has reached carried from one instruction to the next, so the wait
//    depends on where in the bus cycle each access falls.

#ifndef POM2_ALS_CPM_CARD_H
#define POM2_ALS_CPM_CARD_H

#include "SlotPeripheral.h"
#include "Z80.h"

#include <array>
#include <cstdint>

class Memory;
class M6502;

class AlsCpmCard : public SlotPeripheral, private pom2::Z80Bus
{
public:
    static constexpr double kZ80Hz = 6'000'000.0;
    static constexpr int    kTickHz = 60;
    /// The opcode the card drives in the interrupt acknowledge (IM 0).
    static constexpr uint8_t kRst30 = 0xF7;

    AlsCpmCard();

    void setMemory(Memory* mem) { mem_ = mem; }
    void setCpu(M6502* cpu) { cpu_ = cpu; }

    std::string_view name() const override { return "ALS CP/M Card"; }

    /// $CnXX write = bus-ownership toggle.
    void slotRomWrite(uint8_t low8, uint8_t v) override;
    void onReset() override;

    bool dmaActive() const override { return enabled_; }
    int  dmaRun(int cycles6502) override;
    void setCpuClock(double hz) override { if (hz > 0.0) { cpuHz_ = hz; } }
    void setStandardClock(double hz) override { if (hz > 0.0) { stockHz_ = hz; } }

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    /// Apple address behind a bank-0 Z80 address below $E000.
    static uint16_t xlateBank0(uint16_t z80Addr);

    // Test/debug hooks.
    const pom2::Z80& z80() const { return z80_; }
    pom2::Z80&       z80()       { return z80_; }
    int  bank() const { return bank_; }
    uint8_t cardRam(uint16_t a) const { return ram_[a]; }
    void pokeCardRam(uint16_t a, uint8_t v) { ram_[a] = v; }

private:
    uint8_t z80MemRead(uint16_t addr) override;
    void    z80MemWrite(uint16_t addr, uint8_t v) override;
    void    z80IoWrite(uint16_t port, uint8_t v) override;

    bool appleWindow(uint16_t addr) const { return bank_ == 0 && addr < 0xE000; }
    void clockTicks();

    Memory* mem_ = nullptr;
    M6502*  cpu_ = nullptr;
    pom2::Z80 z80_;
    /// $0000-$DFFF bank 1, $E000-$FFFF common.
    std::array<uint8_t, 0x10000> ram_{};

    bool enabled_      = false;
    bool firstZ80Boot_ = true;
    int  bank_         = 0;
    bool tickPending_  = false;
    uint64_t nextTick_ = 0;       // Memory cycle of the next 60 Hz tick
    double busPos_     = 0.0;     // where in the current bus cycle the Z80 is (0..1)
    double syncExtra_  = 0.0;     // bus cycles the Apple accesses added, this instruction
    double busCarry_   = 0.0;     // fractional CPU cycles owed (accelerator)
    double cpuHz_      = 0.0;
    double stockHz_    = 0.0;
    double busPerT_    = 0.0;     // Apple bus cycles per Z80 T-state
    double ratio_      = 1.0;     // CPU cycles per bus cycle (accelerator)
    /// Bus-cycle position of the access in progress, synced to an edge.
    double appleAccess();
};

#endif // POM2_ALS_CPM_CARD_H
