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

// Headless boot trace for Total Replay on //e via SmartPortCard +
// SmartPortHdvUnit in slot 5. Diagnostic — prints text page R00 + a
// few PC samples so we can confirm the disk image loads, ProDOS scans
// it, and Total Replay's text UI appears.
//
// Mirrors `iic_boot_trace.cpp`'s pattern: minimal Memory + M6502 +
// SmartPortCard, no GUI / EmulationController. Run from build/ or
// from the repo root (paths probe both prefixes).

#include "M6502.h"
#include "Memory.h"
#include "SystemProfile.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "Logger.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        const std::string up1 = "../" + p;    if (fs::exists(up1)) return up1;
        const std::string up2 = "../../" + p; if (fs::exists(up2)) return up2;
    }
    return {};
}

void dumpText(Memory& mem) {
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
}
}

int main()
{
    Memory mem; M6502 cpu(&mem);
    mem.clearRam(); mem.resetSoftSwitches(); mem.setIIEMode(true);

    // //e Enhanced (65C02). pickLowerHalf=false → //e ROM layout.
    const std::string romPath = firstExisting({"roms/apple2e.rom"});
    if (romPath.empty()) { std::printf("SKIP: no apple2e.rom\n"); return 0; }
    if (!mem.loadAppleIIRom(romPath.c_str(), /*pickLowerHalf=*/false)) {
        std::printf("FAIL: load %s\n", romPath.c_str()); return 1;
    }

    // SmartPort card in slot 5 with unit 0 = HDV Total Replay.
    const std::string hdvPath = firstExisting({
        "hdv/Total Replay II v1.0-alpha.4.hdv",
        "floppyemu/Total Replay v6.1.hdv",
    });
    if (hdvPath.empty()) {
        std::printf("SKIP: no Total Replay HDV in hdv/ or floppyemu/\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    auto card = std::make_unique<pom2::SmartPortCard>(5);
    auto u0   = std::make_unique<pom2::SmartPortHdvUnit>();
    if (!u0->loadImage(hdvPath)) {
        std::printf("FAIL: HDV load %s: %s\n",
                    hdvPath.c_str(), u0->lastError().c_str());
        return 1;
    }
    std::printf("Mounted: %s (%u blocks)\n",
                hdvPath.c_str(), u0->blockCount());
    card->setUnit(0, std::move(u0));
    pom2::SmartPortCard* cardRaw = card.get();
    mem.slotBus().plug(5, std::move(card));
    (void)cardRaw;

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    std::printf("Reset PC = $%04X (expect $FA62)\n", cpu.getProgramCounter());

    // Budget: ~20M instructions covers F8 self-test + slot-5 boot scan
    // + ProDOS load + Total Replay splash.
    constexpr int kInstrs = 20'000'000;
    bool sawC500 = false;
    int  firstC500 = -1;
    int  escapedRomAt = -1;
    for (int i = 0; i < kInstrs; ++i) {
        const uint16_t pc = cpu.getProgramCounter();
        if (pc >= 0xC500 && pc < 0xC600 && !sawC500) {
            sawC500 = true; firstC500 = i;
            std::printf("[%6d] >>> ENTERED $C5xx (pc=$%04X)\n", i, pc);
        }
        if (sawC500 && pc < 0xC000 && escapedRomAt < 0) {
            escapedRomAt = i;
            std::printf("[%6d] >>> ESCAPED ROM to RAM (pc=$%04X)\n", i, pc);
        }
        cpu.step();
    }

    std::printf("Final PC = $%04X (sawC500=%d firstC500=%d escapedAt=%d)\n",
                cpu.getProgramCounter(), sawC500, firstC500, escapedRomAt);
    std::printf("--- text page ---\n");
    dumpText(mem);

    // Pass conditions: boot routine reached slot 5 AND ProDOS+app
    // executed outside the ROM (i.e. running code in user RAM).
    if (!sawC500) {
        std::printf("FAIL: //e never entered slot-5 $C500 boot\n");
        return 1;
    }
    if (escapedRomAt < 0) {
        std::printf("FAIL: boot stayed inside ROM, never reached user RAM\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
