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

// An IWM access is stamped at its own bus cycle, not the instruction start.
//
// Bug hunt 2026-11-07: the //c+ on-board IWM (`Memory` → `IIcClassProfile::
// ioReadIWM` / `ioWriteIWM`) and the Liron card's IWM (`LironCard::
// deviceSelectRead` / `deviceSelectWrite`) were ticked with the instruction
// START — 3 to 6 CPU cycles, 1.5 to 3 Sony cells, before the access reaches
// the bus. The fix hands them `Memory::accessCycle()` and
// `SlotPeripheral::busAccessOffset()`. Until now only the 3.5" boot tests
// guarded it, and a boot tolerates a window that is a cell or two off often
// enough that it is not a pin. This one is: it reads the cycle the IWM was
// last ticked at (`IWMDevice::emuCycles`) at the access itself.
//
// "At the access itself" is a read watchpoint on the IWM register: Memory
// reports a watched read AFTER performing it, so the sink sees the tick the
// read just made. That matters for the Liron, whose `advanceCycles` ticks
// its IWM again at the END of every instruction (to drain the motor-off
// timer) and would hide the access's own stamp. A watched write is reported
// BEFORE it is performed, so the write probe reads the IWM after the
// instruction instead — which only works on the //c+, whose IWM nothing
// re-ticks at the instruction end. The Liron's write path is the same
// expression as its read path (`cycles_ + busAccessOffset()`), one line apart.
//
// [B] WDC W65C02S datasheet, Table 5-7 — an absolute LDA/STA puts its data
// access on cycle 4 (opcode fetch = cycle 1), so 3 cycles after the start;
// `(zp),Y` without a page crossing puts it on cycle 5, 4 after the start.
// POM2's per-cycle bus stamps (same bug hunt) issue them there.
//
// Needs the //c+ firmware (roms/apple2cp.rom) and, for the Liron half, the
// //e firmware + the Liron EPROM; without them it SKIPs (77).

#include "IWMDevice.h"
#include "LironCard.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryWatchSink.h"
#include "ResourcePaths.h"
#include "SlotBus.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void check(bool ok, const char* what, uint64_t got, uint64_t want)
{
    if (ok) {
        std::printf("  ok: %s (+%llu)\n", what, static_cast<unsigned long long>(want));
    } else {
        std::printf("  FAIL: %s — IWM ticked at start+%llu, want start+%llu\n", what,
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(want));
        ++gFailures;
    }
}

// The probes: each is one instruction at $0300, its target an IWM register
// that changes nothing a later probe depends on (Q6L/Q7L reads, a phase-0
// OFF write), and the cycle offset its data access lands on.
struct Probe {
    const char*          name;
    std::vector<uint8_t> code;
    uint64_t             offset;
    bool                 write;
};

// Records the IWM's stamp the moment a watched read has been performed.
struct ReadStamp : pom2::MemoryWatchSink {
    std::function<uint64_t()> stampOf;
    uint64_t                  at   = 0;
    bool                      seen = false;
    void noteAccess(uint16_t, uint8_t, bool write) override
    {
        if (write) return;
        at   = stampOf();
        seen = true;
    }
};

std::vector<Probe> probesFor(uint16_t base)
{
    const auto lo = static_cast<uint8_t>(base & 0xFF);
    const auto hi = static_cast<uint8_t>(base >> 8);
    return {
        { "LDA abs (read, Q6L)",
          { 0xAD, static_cast<uint8_t>(lo | 0x0C), hi }, 3, false },
        { "STA abs (write, phase 0 off)", { 0x8D, lo, hi }, 3, true },
        // ($80),Y with Y = 0 and $80/$81 = base|$0E: no page crossing.
        { "LDA (zp),Y (read, Q7L)", { 0xB1, 0x80 }, 4, false },
    };
}

// Runs every probe on `mem`/`cpu` and compares the cycle `stampOf` reports
// with the instruction start + the probe's offset. `reTicksAtEnd`: the device
// ticks its IWM again once the instruction is over (the Liron), so a write's
// stamp cannot be read back after it.
void runProbes(const char* machine, Memory& mem, M6502& cpu, uint16_t base,
               std::function<uint64_t()> stampOf, bool reTicksAtEnd)
{
    std::printf("%s\n", machine);
    ReadStamp sink;
    sink.stampOf = stampOf;
    mem.setWatchSink(&sink);
    mem.setReadWatch(static_cast<uint16_t>(base | 0x0C), true);
    mem.setReadWatch(static_cast<uint16_t>(base | 0x0E), true);
    mem.memWrite(0x80, static_cast<uint8_t>((base & 0xFF) | 0x0E));
    mem.memWrite(0x81, static_cast<uint8_t>(base >> 8));
    for (const Probe& p : probesFor(base)) {
        if (p.write && reTicksAtEnd) {
            std::printf("  --: %s (same expression as the read path)\n", p.name);
            continue;
        }
        for (size_t i = 0; i < p.code.size(); ++i)
            mem.memWrite(static_cast<uint16_t>(0x0300 + i), p.code[i]);
        cpu.setYRegister(0);
        cpu.setProgramCounter(0x0300);
        sink.seen = false;
        const uint64_t start = mem.getCycleCounter();
        cpu.run(1);                          // one instruction, whatever its length
        if (!p.write && !sink.seen) {
            std::printf("  FAIL: %s — the read watch never fired\n", p.name);
            ++gFailures;
            continue;
        }
        const uint64_t at = p.write ? stampOf() : sink.at;
        check(at == start + p.offset, p.name, at - start, p.offset);
    }
    mem.setReadWatch(static_cast<uint16_t>(base | 0x0C), false);
    mem.setReadWatch(static_cast<uint16_t>(base | 0x0E), false);
    mem.setWatchSink(nullptr);
}

bool testIIcPlus()
{
    const std::string rom = pom2::findResource("roms/apple2cp.rom");
    if (rom.empty()) {
        std::printf("SKIP //c+ half: needs roms/apple2cp.rom\n");
        return false;
    }
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    pom2::IWMDevice iwm;
    mem.setIWM(&iwm);
    mem.setIIEMode(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLower16KFor32K=*/true)) {
        std::printf("FAIL: cannot load %s\n", rom.c_str());
        ++gFailures;
        return true;
    }
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    runProbes("//c+ on-board IWM ($C0E0-$C0EF)", mem, cpu, 0xC0E0,
              [&] { return iwm.emuCycles(); }, /*reTicksAtEnd=*/false);
    return true;
}

bool testLiron()
{
    const std::string rom = pom2::findResource("roms/apple2e.rom");
    if (rom.empty()) {
        std::printf("SKIP Liron half: needs roms/apple2e.rom\n");
        return false;
    }
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    if (!mem.loadAppleIIRom(rom.c_str())) {
        std::printf("FAIL: cannot load %s\n", rom.c_str());
        ++gFailures;
        return true;
    }
    auto card = std::make_unique<pom2::LironCard>(5);
    pom2::LironCard* liron = card.get();
    if (!liron->romLoaded()) {
        std::printf("SKIP Liron half: %s\n", liron->lastError().c_str());
        return false;
    }
    mem.slotBus().plug(5, std::move(card));
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    runProbes("Liron card IWM (slot 5, $C0D0-$C0DF)", mem, cpu, 0xC0D0,
              [&] { return liron->iwm().emuCycles(); }, /*reTicksAtEnd=*/true);
    return true;
}

} // namespace

int main()
{
    const bool ranIIcPlus = testIIcPlus();
    const bool ranLiron   = testLiron();
    if (gFailures) {
        std::printf("iwm_access_stamp: %d FAILURE(S)\n", gFailures);
        return 1;
    }
    if (!ranIIcPlus && !ranLiron) return 77;
    std::printf("iwm_access_stamp: OK\n");
    return 0;
}
