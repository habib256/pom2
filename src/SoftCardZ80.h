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

// SoftCardZ80 — Microsoft SoftCard (Z-80) slot card. Phase 2 of the
// CP/M plan; the CPU core itself is the standalone pom2::Z80 (see Z80.h).
//
// Ported from MAME src/devices/bus/a2bus/a2softcard.cpp (R. Belmont,
// BSD-3-Clause) — line references in the .cpp. Hardware model:
//
//  - The card is a **DMA bus master**: only one CPU owns the bus at a
//    time. Any WRITE to the card's $CnXX slot-ROM window toggles
//    ownership (MAME `write_cnxx`, a2softcard.cpp:88-109). There is no
//    slot ROM and no DEVSEL behaviour — reads of $CnXX float ($FF).
//  - While the Z80 owns the bus the 6502 is halted (MAME
//    `raise_slot_dma`); toggling back asserts the Z80's WAIT line, so
//    the Z80 *freezes in place* and resumes exactly there next time —
//    except the very first grant after reset, which resets the Z80 to
//    PC=$0000 (MAME `m_FirstZ80Boot`).
//  - Z80 memory map = the 6502 space re-arranged so CP/M gets RAM at
//    $0000 and its BIOS windows on the Language Card (MAME dma_r/dma_w,
//    a2softcard.cpp:111-176):
//        Z80 $0000-$AFFF → 6502 $1000-$BFFF   (+$1000)
//        Z80 $B000-$BFFF → 6502 $D000-$DFFF   (LC bank)
//        Z80 $C000-$CFFF → 6502 $E000-$EFFF   (LC)
//        Z80 $D000-$DFFF → 6502 $F000-$FFFF   (LC / ROM)
//        Z80 $E000-$EFFF → 6502 $C000-$CFFF   (I/O + slot ROM space)
//        Z80 $F000-$FFFF → 6502 $0000-$0FFF   (zero page / stack / text)
//    Every access goes through Memory::memRead/memWrite — the real bus —
//    so soft-switch side effects, LC paging and IIe aux paging behave
//    identically for both CPUs. (The Z80 reaches the toggle itself via
//    the $E000 window: 6502 $Cn00 = Z80 $En00.)
//  - Z80 clock = 2× the 6502 clock (MAME a2softcard.cpp:41,
//    `Z80(config, m_z80, 1021800*2)`), so 2 Z80 T-states consume one
//    6502 cycle of the frame budget; `dmaRun` feeds the converted count
//    to Memory::advanceCycles so emuCycles stays in the 6502 domain
//    (video beam / Disk II LSS / audio pacing unchanged).
//
// The Apple IRQ line is NOT wired to the Z80 (matches MAME: no
// set_input_line from the bus side). CP/M runs with the Z80's own
// interrupts disabled.

#ifndef POM2_SOFTCARD_Z80_H
#define POM2_SOFTCARD_Z80_H

#include "SlotPeripheral.h"
#include "Z80.h"

#include <cstdint>

class Memory;
class M6502;

class SoftCardZ80 : public SlotPeripheral, private pom2::Z80Bus
{
public:
    SoftCardZ80();

    /// The bus the Z80 drives. Required before the first bus-master
    /// grant; MainWindow wires it at plug time.
    void setMemory(Memory* mem) { mem_ = mem; }
    /// The 6502 to halt when the Z80 takes the bus. Optional but
    /// strongly recommended: without it the 6502 finishes its current
    /// run() chunk (up to 4096 cycles) before the controller notices
    /// the hand-over.
    void setCpu(M6502* cpu) { cpu_ = cpu; }

    std::string_view name() const override { return "SoftCard Z80"; }

    /// $CnXX write = bus-ownership toggle (MAME write_cnxx). Data and
    /// offset are ignored by the hardware.
    void slotRomWrite(uint8_t low8, uint8_t v) override;

    /// Apple II RESET: Z80 reset + card disarmed + first-boot latch
    /// re-set (MAME reset_from_bus, a2softcard.cpp:82-86).
    void onReset() override;

    // ── DMA bus mastery (consumed by EmulationController) ────────────
    bool dmaActive() const override { return enabled_; }
    int  dmaRun(int cycles6502) override;
    /// The Z80 is clocked from the Apple bus (MAME a2softcard.cpp:41,
    /// 1021800 x 2), not by the 6502 accelerator: under a Zip or TransWarp a
    /// bus cycle costs cpuHz / stockHz cycles of the accelerated budget.
    void setCpuClock(double hz) override { if (hz > 0.0) { cpuHz_ = hz; } }
    void setStandardClock(double hz) override { if (hz > 0.0) { stockHz_ = hz; } }

    // ── snapshot / rewind ────────────────────────────────────────────
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    /// Test/debug hooks.
    const pom2::Z80& z80() const { return z80_; }
    pom2::Z80&       z80()       { return z80_; }
    bool firstBootPending() const { return firstZ80Boot_; }

    /// MAME dma_r/dma_w window arithmetic — exposed for the smoke test.
    static uint16_t xlate(uint16_t z80Addr);

private:
    // pom2::Z80Bus — the Z80's view of the Apple II bus.
    uint8_t z80MemRead(uint16_t addr) override;
    void    z80MemWrite(uint16_t addr, uint8_t v) override;

    Memory* mem_ = nullptr;
    M6502*  cpu_ = nullptr;
    pom2::Z80 z80_;

    bool enabled_      = false;   // MAME m_bEnabled — Z80 owns the bus
    bool firstZ80Boot_ = true;    // MAME m_FirstZ80Boot
    int  tCarry_       = 0;       // odd Z80 T-state carried between dmaRun calls
    double cpuHz_      = 0.0;     // 0 = never given: 1 bus cycle = 1 CPU cycle
    double stockHz_    = 0.0;
    double busCarry_   = 0.0;     // fractional accelerated cycles owed
    double busRatio_   = 1.0;     // dmaRun's CPU cycles per bus cycle, for stampAccess
    void   stampAccess();         // put the Z80's access position in the 6502's in-flight count
};

#endif // POM2_SOFTCARD_Z80_H
