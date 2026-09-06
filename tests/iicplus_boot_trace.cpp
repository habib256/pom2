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

// Boot-trace smoke test for the //c+ profile. Loads the user's
// apple2cp.rom, applies the IIcPlus profile-equivalent memory state,
// hard-resets, runs the CPU for ~1.5 emulated seconds and reports:
//   * PC at the end (so we can see where the boot routine settles)
//   * a histogram of which $C0xx soft switches were touched
//   * a fingerprint of text page 1 ($0400-$07FF) so we can tell
//     whether the //c+ Monitor cleared the screen and printed
//     anything readable
//
// This is a diagnostic tool — it doesn't assert anything, it prints.
// The CMake target is `test_iicplus_boot_trace`; run from the build
// directory while we figure out why the //c+ refuses to boot a 5.25"
// disk via the library-click path.

#include "M6502.h"
#include "Memory.h"
#include "SystemProfile.h"
#include "SlotBus.h"
#include "DiskIICard.h"
#include "IWMDevice.h"
#include "Logger.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

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

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    // Wire the IWM (mirrors EmulationController's setup) so Memory's
    // $C0E0-$C0EF dispatch routes the //c+ data path through the
    // MAME-faithful state machine.
    pom2::IWMDevice iwm;
    mem.setIWM(&iwm);
    // Mirror EmulationController's runtime opt-in so this diagnostic
    // can A/B compare the two data paths without recompiling.
    if (const char* env = std::getenv("POM2_IWM_AUTHORITATIVE")) {
        mem.setIWMAuthoritative(env[0] != '0');
    }

    const auto& cfg = pom2::profileConfig(pom2::SystemProfile::AppleIIcPlus);

    // Mimic applyProfile: clear RAM, reset switches, enable IIe paging,
    // load ROM with pickLowerHalf=true for //c-style banks, set CPU
    // mode, hard reset.
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);

    const std::string romPath = firstExisting({
        "roms/apple2cp.rom", "roms/apple2c-plus.rom",
        "roms/apple2c-32Kv0.rom"
    });
    if (romPath.empty()) {
        std::printf("SKIP iicplus_boot_trace: no //c+ ROM found\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    if (!mem.loadAppleIIRom(romPath.c_str(), /*pickLowerHalf=*/true)) {
        std::printf("FAIL: could not load %s\n", romPath.c_str());
        return 1;
    }

    // Plug a Disk II in slot 6 (POM2's //c+ profile does the same).
    const std::string diskRom = firstExisting({"roms/disk2.rom"});
    auto card = std::make_unique<DiskIICard>(6);
    if (!diskRom.empty()) card->loadBootRom(diskRom);
    const std::string lssRom = firstExisting({"roms/diskii_p6.rom"});
    if (!lssRom.empty()) card->loadLssRom(lssRom);
    // Insert a real .dsk so the //c+ ROM's slot-6 boot scan sees a
    // mounted volume and (we hope) auto-boots into DOS / ProDOS
    // instead of dropping into the Monitor.
    const std::string disk = firstExisting({
        "disks_5.4/dsk/dos33_master.dsk",
        "dos33_master.dsk",
    });
    if (!disk.empty()) {
        card->insertDisk(disk);
        std::printf("Inserted: %s\n", disk.c_str());
    } else {
        std::printf("(no disk inserted)\n");
    }
    // Bind the IWM ↔ DiskIICard duo so the IWM's flux source mirrors
    // the card's active drive + head position.
    card->setIWM(&iwm);
    DiskIICard* cardRaw = card.get();
    mem.slotBus().plug(6, std::move(card));
    (void)cardRaw;

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    std::printf("Initial PC after reset: $%04X (expect $FA62 for //c+)\n",
                cpu.getProgramCounter());

    // Run ~1.5 emulated seconds at 4 MHz = ~6M cycles. Enough for any
    // banner display + slot scan.
    constexpr int kBudget = 6'000'000;
    int    cycles = 0;
    uint16_t pcHistory[8] = {};
    int pcHistoryIdx = 0;
    while (cycles < kBudget) {
        const uint16_t pc = cpu.getProgramCounter();
        pcHistory[pcHistoryIdx] = pc;
        pcHistoryIdx = (pcHistoryIdx + 1) & 7;
        cpu.step();
        cycles += cpu.getCurrentInstructionCycles();
    }

    std::printf("After ~6M cycles: PC=$%04X\n", cpu.getProgramCounter());
    std::printf("Last 8 PCs (newest at the right): ");
    for (int i = 0; i < 8; ++i) {
        std::printf("$%04X ", pcHistory[(pcHistoryIdx + i) & 7]);
    }
    std::printf("\n");

    // Fingerprint text page 1: count non-$00 bytes, max/min, sample first 80.
    int nonZero = 0;
    uint8_t firstChars[40] = {};
    for (uint16_t a = 0x0400; a <= 0x07FF; ++a) {
        const uint8_t b = mem.memRead(a);
        if (b != 0x00) ++nonZero;
    }
    std::printf("Text page 1: %d/1024 non-zero bytes\n", nonZero);
    // Apple II text page row addressing: 24 rows × 40 cols, interleaved
    // in groups of 8 (row 0 base = $400, row 1 = $480, row 2 = $500,
    // row 8 = $428, row 9 = $4A8, etc.). Reconstruct linearly here so
    // we can read the boot screen.
    static const uint16_t kRowBase[24] = {
        0x400,0x480,0x500,0x580,0x600,0x680,0x700,0x780,
        0x428,0x4A8,0x528,0x5A8,0x628,0x6A8,0x728,0x7A8,
        0x450,0x4D0,0x550,0x5D0,0x650,0x6D0,0x750,0x7D0,
    };
    for (int r = 0; r < 24; ++r) {
        std::printf("R%02d: '", r);
        for (int c = 0; c < 40; ++c) {
            const uint8_t b = mem.memRead(static_cast<uint16_t>(kRowBase[r] + c));
            const char ch = static_cast<char>(b & 0x7F);
            std::putchar(ch >= 0x20 && ch < 0x7F ? ch : '.');
        }
        std::printf("'\n");
    }

    std::printf("Done.\n");
    return 0;
}
