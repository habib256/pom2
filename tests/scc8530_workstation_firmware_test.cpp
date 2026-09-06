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

// Scc8530Device against REAL firmware — the Apple II Workstation Card's
// 341-0358-A ROM, driven far enough to complete its power-on self-test and
// configure the chip for LocalTalk.
//
// `scc8530_smoke` checks the port against the datasheet and against MAME.
// This checks it against the only oracle that cannot be argued with: 8 KB of
// Apple's own driver code, written by people who had the silicon in front of
// them. The firmware's POST includes a **255-byte loopback ping-pong on both
// channels with a fixed 8000-poll budget**, so it does not merely touch the
// registers — it fails unless the transmit timing, the TBE/RxCA status bits
// and the one-slot transmit buffer all behave.
//
// WHY THE HARNESS LOOKS LIKE THIS. The dump is not a slot ROM: it is firmware
// for a 65C02 **on the card**, with its own bus (RAM `$0000-$6FFF`, I/O
// selects at `$7x00`, ROM `$8000-$FFFF`) — see `docs/printer_plan_2.md` § 5.2.
// POM2's `M6502` is bound to `Memory`, the Apple II memory model, so running
// that CPU properly needs a bus abstraction that is its own piece of work
// (TODO § Workstation Card). Rather than block this evidence behind that, the
// harness borrows `Memory` in **testMode** — flat 64 KB RAM, the Klaus
// Dormann configuration — and shims the I/O page by decoding each
// instruction's effective address around the step. That is good enough to
// drive firmware and cheap to throw away when the real card lands; it is
// deliberately NOT a card emulation.
//
// WHAT THE HARNESS ASSUMES, AND WHERE EACH ASSUMPTION COMES FROM. None of
// these are guesses about the SCC — they are facts the firmware itself
// asserts, which is what makes the test meaningful:
//
//   * SCC at `$7500-$7503`, A1 = A//B and A0 = D//C. From `$EE13`:
//     `LDA #$03 / STA $7502 / LDA $7502` polls RR3, which exists in channel A
//     only, so `$7502` must be channel A control.
//   * The other `$7x00` selects are write/read-back latches, and `$7A00` is
//     only **five bits** wide. From the POST's own register test at
//     `$F0F1-$F17D`: it writes `$FF` into `$7A00` with a `DEC` and then
//     compares against `#$1F`.
//   * /RTxC and PCLK are both 3.6864 MHz. Not assumed — *derived*: the
//     firmware's final configuration is WR12/WR13 = 6 with WR4 selecting the
//     x1 clock, and `3686400 / (6 + 2) / 2` is exactly **230400**, the
//     LocalTalk bit rate. No other plausible crystal produces it from the
//     divisor the firmware chose.
//   * The card CPU is at most about 2 MHz. This one is a CEILING, not a
//     measurement: the 255-byte ping-pong has to fit in 8000 polls, which
//     passes at every rate tried up to 2.0 MHz against a 3.6864 MHz SCC clock
//     and fails from 2.05 MHz up. It passes at 500 kHz too, so the POST says
//     nothing about how much slower the real card might be. 1.02 MHz is used
//     below because it is comfortably inside the passing range.

#include "M6502.h"
#include "Memory.h"
#include "Scc8530Device.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kPclk      = 3686400;   // PCLK and /RTxC, see header
constexpr uint64_t kCardCpuHz = 1020000;   // ~1 MHz card CPU
constexpr long     kBudget    = 2500000;   // instructions; the config lands ~1.3M

// The card's POST error accumulator, and the address it halts at when one of
// the bits survives the `AND #$EF` mask at $C14F.
constexpr uint16_t kPostErrors = 0x0100;
constexpr uint16_t kHaltLoop   = 0xC174;

std::string findRom()
{
    for (const char* p : { "roms/341-0358-A.bin", "../roms/341-0358-A.bin",
                           "../../roms/341-0358-A.bin", "roms/341-0358-a.bin",
                           "../roms/341-0358-a.bin", "../../roms/341-0358-a.bin" }) {
        std::ifstream f(p, std::ios::binary);
        if (f.good()) return p;
    }
    return {};
}

// Absolute-mode opcodes, and whether each reads, writes, or does both. Only
// the three-byte absolute forms can name an I/O select in this firmware; a
// mode this table misses would make the POST fail loudly rather than pass
// quietly, which is the safe direction for a harness.
struct AbsOp { bool rd, wr, x, y; };

bool absOp(uint8_t op, AbsOp& o)
{
    o = { false, false, false, false };
    switch (op) {
    // absolute, read
    case 0xAD: case 0xAE: case 0xAC: case 0x2C: case 0xCD: case 0xEC:
    case 0xCC: case 0x0D: case 0x2D: case 0x4D: case 0x6D: case 0xED:
        o.rd = true; return true;
    // absolute, write
    case 0x8D: case 0x8E: case 0x8C: case 0x9C:
        o.wr = true; return true;
    // absolute, read-modify-write (INC/DEC/shifts/TSB/TRB)
    case 0xEE: case 0xCE: case 0x0E: case 0x4E: case 0x2E: case 0x6E:
    case 0x0C: case 0x1C:
        o.rd = true; o.wr = true; return true;
    // absolute,X
    case 0xBD: case 0xBC: case 0x3C: case 0xDD: case 0x1D: case 0x3D:
    case 0x5D: case 0x7D: case 0xFD:
        o.rd = true; o.x = true; return true;
    case 0x9D: case 0x9E:
        o.wr = true; o.x = true; return true;
    case 0xFE: case 0xDE: case 0x1E: case 0x5E: case 0x3E: case 0x7E:
        o.rd = true; o.wr = true; o.x = true; return true;
    // absolute,Y
    case 0xB9: case 0xBE: case 0xD9: case 0x19: case 0x39: case 0x59:
    case 0x79: case 0xF9:
        o.rd = true; o.y = true; return true;
    case 0x99:
        o.wr = true; o.y = true; return true;
    default:
        return false;
    }
}

} // namespace

int main()
{
    const std::string romPath = findRom();
    if (romPath.empty()) {
        std::printf("scc8530_workstation_firmware: SKIP "
                    "(roms/341-0358-A.bin not found)\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    std::vector<uint8_t> rom(0x10000, 0);
    {
        std::ifstream f(romPath, std::ios::binary);
        f.read(reinterpret_cast<char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
        if (f.gcount() != static_cast<std::streamsize>(rom.size())) {
            std::printf("scc8530_workstation_firmware: SKIP "
                        "(%s is not a 64 KiB dump)\n", romPath.c_str());
            return 77;   // ctest SKIP_RETURN_CODE
        }
    }

    Memory mem;
    mem.setTestMode(true);
    std::vector<uint8_t> flat(0x10000, 0);
    // Upper 32 KiB of the dump is the image the card's CPU runs at $8000.
    std::memcpy(flat.data() + 0x8000, rom.data() + 0x8000, 0x8000);
    mem.loadFlatTestImage(flat.data(), flat.size());

    pom2::Scc8530Device scc;
    scc.setPclk(kPclk);
    scc.setRtxc(pom2::Scc8530Device::CHAN_A, kPclk);
    scc.setRtxc(pom2::Scc8530Device::CHAN_B, kPclk);

    M6502 cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    const uint16_t resetVec =
        static_cast<uint16_t>(rom[0xFFFC] | (rom[0xFFFD] << 8));
    assert(resetVec == 0xC000 && "the dump's RESET vector should be $C000");
    cpu.setProgramCounter(resetVec);
    cpu.setStackPointer(0xFF);

    uint8_t  latch[16] = { 0 };
    uint64_t sccAcc = 0;
    long     romWrites = 0;
    long     dataBytes = 0;
    bool     reachedHalt = false;

    for (long n = 0; n < kBudget && !reachedHalt; ++n) {
        const uint16_t pc = cpu.getProgramCounter();
        if (pc == kHaltLoop) { reachedHalt = true; break; }

        const uint8_t op = mem.peekMainRam(pc);
        AbsOp o = { false, false, false, false };
        uint16_t ea = 0;
        bool abs = absOp(op, o);
        bool io = false;
        if (abs) {
            ea = static_cast<uint16_t>(mem.peekMainRam(static_cast<uint16_t>(pc + 1)) |
                                       (mem.peekMainRam(static_cast<uint16_t>(pc + 2)) << 8));
            if (o.x) ea = static_cast<uint16_t>(ea + cpu.getXRegister());
            if (o.y) ea = static_cast<uint16_t>(ea + cpu.getYRegister());
            io = (ea >= 0x7000 && ea < 0x8000);
        }

        if (io && o.rd) {
            uint8_t v;
            if (ea >= 0x7500 && ea <= 0x7503) {
                v = scc.readAbDc(static_cast<uint8_t>(ea & 3));
                if (ea & 1) dataBytes++;
            } else {
                v = latch[(ea >> 8) & 0x0F];
                if ((ea & 0xFF00) == 0x7A00) v &= 0x1F;   // five bits wide
            }
            mem.writeRamUnchecked(ea, v);
        }

        const uint64_t cyBefore = mem.getCycleCounter();
        cpu.step();
        sccAcc += (mem.getCycleCounter() - cyBefore) * static_cast<uint64_t>(kPclk);
        scc.tick(sccAcc / kCardCpuHz);
        sccAcc %= kCardCpuHz;

        if (io && o.wr) {
            const uint8_t v = mem.peekMainRam(ea);
            if (ea >= 0x7500 && ea <= 0x7503)
                scc.writeAbDc(static_cast<uint8_t>(ea & 3), v);
            else
                latch[(ea >> 8) & 0x0F] = v;
        }

        // The card's ROM must never be written. Flat test RAM cannot enforce
        // that, so the harness watches for it instead — a firmware that
        // scribbles into $8000+ would mean the memory map is wrong.
        if (abs && o.wr && ea >= 0x8000) romWrites++;
    }

    const uint8_t post = mem.peekMainRam(kPostErrors);

    std::printf("  firmware ran to PC=$%04X, POST errors=$%02X, "
                "%ld data-register reads\n",
                cpu.getProgramCounter(), post, dataBytes);

    // 1. The power-on self-test passed. $0100 accumulates the failure bits
    //    the boot code tests at $C14A; a non-zero value that survives its
    //    `AND #$EF` mask sends the card to the halt loop.
    assert(post == 0x00 && "the card firmware reported a POST failure");
    assert(!reachedHalt && "the card firmware halted at its POST error loop");

    // 2. The loopback ping-pong really moved bytes through the chip. Each
    //    channel sends 255 and reads back 255, so anything near that count
    //    means the transmit timing and the status bits both worked.
    assert(dataBytes >= 500 && "the SCC loopback self-test moved too few bytes");

    // 3. The firmware never wrote into what this test claims is ROM.
    assert(romWrites == 0 && "firmware wrote into $8000+, so the map is wrong");

    // 4. It went on to configure the chip for LocalTalk. This is the payoff:
    //    every field below was chosen by Apple's own driver, and the rate is
    //    the one the physical network runs at.
    const uint8_t wr4  = scc.peekWr(pom2::Scc8530Device::CHAN_A, 4);
    const uint8_t wr3  = scc.peekWr(pom2::Scc8530Device::CHAN_A, 3);
    const uint8_t wr11 = scc.peekWr(pom2::Scc8530Device::CHAN_A, 11);
    const uint8_t wr14 = scc.peekWr(pom2::Scc8530Device::CHAN_A, 14);

    assert((wr4 & 0x0C) == 0x00 && "WR4 should select a synchronous mode");
    assert((wr4 & 0x30) == 0x20 && "WR4 should select SDLC");
    assert((wr4 & 0xC0) == 0x00 && "SDLC needs the x1 clock");
    assert((wr3 & 0x01) != 0    && "the receiver should be enabled");
    assert((wr3 & 0x04) != 0    && "SDLC address search should be on");
    assert((wr11 & 0x60) == 0x60 && "the receive clock should come from the DPLL");
    assert((wr14 & 0x01) != 0    && "the baud-rate generator should be running");
    assert(scc.txRate(pom2::Scc8530Device::CHAN_A) == 230400 &&
           "the card should end up at the LocalTalk bit rate");

    std::printf("  ok: POST passed; chip left in SDLC at %u bit/s "
                "(WR3=%02X WR4=%02X WR11=%02X WR14=%02X)\n",
                scc.txRate(pom2::Scc8530Device::CHAN_A), wr3, wr4, wr11, wr14);
    std::printf("OK scc8530_workstation_firmware\n");
    return 0;
}
