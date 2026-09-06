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

// Boot-trace diagnostic for Choplifter (1982 Brøderbund, II/II+/48K WOZ)
// on the //e Enhanced profile. Mirrors the iicplus_boot_trace.cpp
// scaffolding but boots the IIe ROM with Choplifter mounted in slot 6.
//
// Pair with POM2_TRACE_IIE_REBOOT=1 — the env var arms the $FA62 reset-
// entry trap in M6502::executeOpcode and the IIe paging / auto-INTCXROM
// log in Memory.cpp. The trace prints to stderr through pom2::log().
//
// Usage:
//   POM2_TRACE_IIE_REBOOT=1 ./tests/test_choplifter_iie_trace 2>&1 | head -200
//
// Prints periodic PC snapshots (every ~512K cycles ≈ 0.5 s emulated) so
// we can spot the moment Choplifter exits its image and bounces.

#include "M6502.h"
#include "Memory.h"
#include "SystemProfile.h"
#include "SlotBus.h"
#include "DiskIICard.h"
#include "Logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        const std::string up1 = "../" + p;
        if (fs::exists(up1)) return up1;
        const std::string up2 = "../../" + p;
        if (fs::exists(up2)) return up2;
    }
    return {};
}

}

int main(int argc, char** argv)
{
    // Force the env var on if the caller didn't — this binary is a pure
    // diagnostic, no reason to run it without the trace.
    if (!std::getenv("POM2_TRACE_IIE_REBOOT")) {
        setenv("POM2_TRACE_IIE_REBOOT", "1", 1);
    }

    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);  // back-ptr so $0400 write trap can log the PC

    // --profile ii+ runs the same test on the II+ baseline (NMOS + II+
    // ROM, iieMode=false) so we can A/B Choplifter behaviour between
    // working (II+) and broken (IIe) profiles.
    bool useIIPlus = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            const std::string p = argv[i + 1];
            if (p == "ii+" || p == "iiplus" || p == "ii") useIIPlus = true;
        }
    }

    // Mirror applyProfile: clear RAM, reset switches, enable IIe paging
    // logic in Memory (or not), then load the profile-appropriate ROM.
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(!useIIPlus);

    const std::string romPath = useIIPlus
        ? firstExisting({"roms/apple2p.rom", "roms/apple2.rom"})
        : firstExisting({"roms/apple2e.rom"});
    if (romPath.empty()) {
        std::printf("SKIP: roms/apple2e.rom not found\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::printf("FAIL: could not load %s\n", romPath.c_str());
        return 1;
    }

    // Plug Disk II in slot 6 with the standard P5/P6 PROMs.
    auto card = std::make_unique<DiskIICard>(6);
    const std::string p5 = firstExisting({"roms/disk2.rom"});
    if (!p5.empty()) card->loadBootRom(p5);
    const std::string p6 = firstExisting({"roms/diskii_p6.rom"});
    if (!p6.empty()) card->loadLssRom(p6);

    // CLI: optional --disk <path> overrides the default Choplifter WOZ.
    std::string diskArg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--disk") == 0 && i + 1 < argc) {
            diskArg = argv[++i];
        }
    }
    const std::string disk = !diskArg.empty() ? diskArg : firstExisting({
        "disks_5.4/woz/Choplifter (1982)(Br\xc3\xb8""derbund)(II-II+)[48K].woz",
    });
    if (disk.empty()) {
        std::printf("FAIL: Choplifter WOZ not found (pass --disk <path>)\n");
        return 1;
    }
    if (!card->insertDisk(disk)) {
        std::printf("FAIL: insertDisk: %s\n", card->getLastError().c_str());
        return 1;
    }
    std::printf("Inserted: %s\n", disk.c_str());

    mem.slotBus().plug(6, std::move(card));

    cpu.setCpuMode(useIIPlus ? M6502::CpuMode::NMOS : M6502::CpuMode::CMOS);
    cpu.hardReset();
    std::printf("Profile: %s\n", useIIPlus ? "Apple ][+ (NMOS)" : "Apple //e Enhanced (65C02)");

    std::printf("Initial PC after reset: $%04X\n",
                cpu.getProgramCounter());

    // Run ~20 emulated seconds at 1 MHz = 20M cycles. Choplifter title
    // takes a few seconds to load; we want to see the reboot event.
    constexpr int kBudget       = 20'000'000;
    constexpr int kSnapshotEvery = 1'000'000;  // ~1 s emulated
    int cycles = 0;
    int nextSnapshot = kSnapshotEvery;
    while (cycles < kBudget) {
        cpu.step();
        cycles += cpu.getCurrentInstructionCycles();
        if (cycles >= nextSnapshot) {
            std::printf("[t=%2ds] PC=$%04X  A=$%02X X=$%02X Y=$%02X SP=$%02X\n",
                cycles / 1'000'000,
                cpu.getProgramCounter(),
                cpu.getAccumulator(),
                cpu.getXRegister(),
                cpu.getYRegister(),
                cpu.getStackPointer());
            nextSnapshot += kSnapshotEvery;
        }
    }

    std::printf("Final PC=$%04X after %d cycles\n",
                cpu.getProgramCounter(), cycles);

    // Dump text page 1 (top-left of screen). This is where the "M"
    // showed up in the user's bug report.
    static const uint16_t kRowBase[24] = {
        0x400,0x480,0x500,0x580,0x600,0x680,0x700,0x780,
        0x428,0x4A8,0x528,0x5A8,0x628,0x6A8,0x728,0x7A8,
        0x450,0x4D0,0x550,0x5D0,0x650,0x6D0,0x750,0x7D0,
    };
    std::printf("--- Text page 1 final ---\n");
    for (int r = 0; r < 24; ++r) {
        std::printf("R%02d: '", r);
        for (int c = 0; c < 40; ++c) {
            const uint8_t b = mem.memRead(static_cast<uint16_t>(kRowBase[r] + c));
            const char ch = static_cast<char>(b & 0x7F);
            std::putchar(ch >= 0x20 && ch < 0x7F ? ch : '.');
        }
        std::printf("'\n");
    }
    // Dump a few hot code regions discovered by the $0400-write trap.
    // The 'M' is written by code at $034E in RAM; the cycle of writes
    // also originates at $0340, $0143, $01C2. Let's see those bytes.
    auto dumpRange = [&](uint16_t base, int len) {
        std::printf("--- $%04X..", base);
        std::printf("$%04X ---\n", base + len - 1);
        for (int i = 0; i < len; i += 16) {
            std::printf("$%04X:", base + i);
            for (int j = 0; j < 16 && (i + j) < len; ++j) {
                std::printf(" %02X", mem.memRead(static_cast<uint16_t>(base + i + j)));
            }
            std::printf("\n");
        }
    };
    dumpRange(0x0140, 64);
    dumpRange(0x01C0, 32);
    dumpRange(0x0330, 64);
    dumpRange(0x03F2, 8);

    std::printf("Done.\n");
    return 0;
}
