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

// Diagnostic probe: boot Copy II Plus v8.3.do and report how far it gets.
// Not a CTest target — run by hand via build/tests/c2plus_boot_probe.

#include "DiskIICard.h"
#include "Disassembler6502.h"
#include "M6502.h"
#include "Memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
std::string findFirst(std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) if (fileExists(c)) return c;
    return {};
}
bool copyFile(const std::string& src, const fs::path& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return static_cast<bool>(out);
}
std::string scrapeTextPage(const uint8_t* ram) {
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(ram[base + col] & 0x7F);
            out.push_back((c >= 0x20 && c < 0x7F) ? c : ' ');
        }
        out.push_back('\n');
    }
    return out;
}

// 80-column scrape: aux RAM provides even cells (0,2,…,78), main RAM
// provides odd cells (1,3,…,79). For each row, interleave them.
std::string scrape80Col(const uint8_t* main, const uint8_t* aux) {
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            // even col: aux byte; odd col: main byte. Re-ordered so the
            // displayed sequence is aux[0] main[0] aux[1] main[1] ...
            const char ca = static_cast<char>(aux[base + col] & 0x7F);
            const char cm = static_cast<char>(main[base + col] & 0x7F);
            out.push_back((ca >= 0x20 && ca < 0x7F) ? ca : ' ');
            out.push_back((cm >= 0x20 && cm < 0x7F) ? cm : ' ');
        }
        out.push_back('\n');
    }
    return out;
}
}

int main(int argc, char** argv) {
    // Match the GUI's ROM precedence: prefer apple2e.rom (IIe, 128K, 80-col)
    // if present, fall back to apple2.rom (II+).
    const std::string romIIePath = findFirst({"../roms/apple2e.rom", "roms/apple2e.rom"});
    const std::string romIIPath  = findFirst({"../roms/apple2.rom",  "roms/apple2.rom"});
    const std::string promPath   = findFirst({"../roms/disk2.rom",   "roms/disk2.rom"});
    const std::string imgPath    = (argc > 1) ? argv[1] : findFirst({
        "../disks_5.4/Copy II Plus v8.3.do", "disks_5.4/Copy II Plus v8.3.do"});

    bool useIIe = !romIIePath.empty();
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--ii+") useIIe = false;
    }
    const std::string romPath = useIIe ? romIIePath : romIIPath;

    if (romPath.empty() || promPath.empty() || imgPath.empty()) {
        std::fprintf(stderr, "missing files\n"); return 1;
    }
    std::fprintf(stderr, "[probe] mode=%s rom=%s\n",
                 useIIe ? "IIe" : "II+", romPath.c_str());

    fs::path scratch = fs::temp_directory_path() / "pom2_c2plus_boot.do";
    if (!copyFile(imgPath, scratch)) { std::fprintf(stderr, "copy failed\n"); return 1; }

    Memory mem;
    if (useIIe) mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(romPath.c_str())) { std::fprintf(stderr, "rom\n"); return 1; }

    auto card = std::make_unique<DiskIICard>();
    card->loadBootRom(promPath);
    card->insertDisk(scratch.string());
    DiskIICard* cardRaw = card.get();
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setDebugBrkTrace(true);
    cpu.setProgramCounter(0xC600);

    // Snapshot LC bank contents at $D080-$D08F every 1M cycles so we can
    // catch which value transitions to $00 and roughly when.
    auto snapshotD084 = [&]() {
        // Probe both banks of main LC + both banks of aux LC, plus current
        // routing flags. Use direct reads via memRead to follow current state.
        std::printf("[D084-probe] iieMode=%d iieMemMode=$%04X "
                    "lcReadRam=? mainD084=$%02X (current routing)\n",
                    static_cast<int>(mem.isIIE()),
                    static_cast<unsigned>(mem.iieModeFlags()),
                    mem.memRead(0xD084));
    };
    (void)snapshotD084;

    const uint8_t* ram = mem.data();
    auto runUntilStable = [&](int cycles) {
        int t = 0;
        while (t < cycles) t += cpu.run(8192);
    };

    auto sample = [&](const char* tag) {
        const auto ds = mem.getDisplayState();
        const uint16_t pc = cpu.getProgramCounter();
        std::printf("[%s] PC=$%04X qt=%d trk=%d motor=%d wf=%llu "
                    "text=%d 80col=%d 80store=%d page2=%d hires=%d altchar=%d\n",
                    tag, pc,
                    cardRaw->getQuarterTrack(), cardRaw->getCurrentTrack(),
                    cardRaw->isMotorOn() ? 1 : 0,
                    static_cast<unsigned long long>(cardRaw->getWriteFlushCount()),
                    ds.textMode, ds.eightyCol, ds.eightyStore,
                    ds.page2, ds.hiRes, ds.altChar);
        // Dump 16 bytes at PC and the stack top to see what's executing.
        std::printf("    bytes@PC: ");
        for (int i = -4; i < 12; ++i) {
            std::printf("%02X ", mem.memRead(static_cast<uint16_t>(pc + i)));
        }
        std::printf("\n    SP=$%02X stack[$01F0..$01FF]: ",
                    cpu.getStackPointer());
        for (int i = 0xF0; i <= 0xFF; ++i) std::printf("%02X ", mem.memRead(0x0100 + i));
        std::printf("\n");
    };

    auto dumpScreen = [&](const char* tag) {
        std::printf("---- %s (40-col main) ----\n%s",
                    tag, scrapeTextPage(ram).c_str());
        if (useIIe) {
            std::printf("---- %s (aux text page, view as 40-col) ----\n%s",
                        tag, scrapeTextPage(mem.auxData()).c_str());
            std::printf("---- %s (80-col interleaved aux/main) ----\n%s",
                        tag, scrape80Col(ram, mem.auxData()).c_str());
        }
        std::printf("----\n");
    };

    // Watch counters at $D36F/$D370 (ProDOS spin loop) every 5M cycles.
    int total = 0;
    while (total < 130'000'000) {
        const int slice = cpu.run(1'000'000);
        total += slice;
        if (total % 5'000'000 == 0) {
            const uint16_t pc = cpu.getProgramCounter();
            std::printf("t=%dM PC=$%04X qt=%d motor=%d\n",
                        total/1'000'000, pc, cardRaw->getQuarterTrack(),
                        cardRaw->isMotorOn() ? 1 : 0);
        }
    }
    sample("final");
    dumpScreen("final");

    // ── Crash-area dump: where does PC park, and what's there? ───────────
    {
        const uint16_t pc = cpu.getProgramCounter();
        std::printf("\n[crash] final PC=$%04X SP=$%02X\n", pc, cpu.getStackPointer());

        // Print the raw memory window around PC (cap at 0xFFFF).
        const uint32_t lo = pc >= 0x40 ? pc - 0x40 : 0;
        const uint32_t hi = std::min<uint32_t>(0x10000u, lo + 0x100);
        std::printf("[crash] memory $%04X-$%04X:\n", lo, hi - 1);
        for (uint32_t row = lo & ~0xFu; row < hi; row += 16) {
            std::printf("$%04X:", row);
            for (uint32_t c = 0; c < 16 && (row + c) < 0x10000; ++c) {
                std::printf(" %02X", mem.memRead(static_cast<uint16_t>(row + c)));
            }
            std::printf("\n");
        }

        // Disassemble a window around PC (32 bytes back, 32 forward).
        std::vector<uint8_t> snap(0x10000);
        for (int i = 0; i < 0x10000; ++i) snap[i] = mem.memRead(static_cast<uint16_t>(i));
        const uint16_t start = static_cast<uint16_t>(pc >= 0x20 ? pc - 0x20 : 0);
        const uint16_t end   = static_cast<uint16_t>(std::min<uint32_t>(0x10000u, pc + 0x40));
        std::printf("[crash] disassembly $%04X-$%04X:\n", start, end - 1);
        uint16_t cur = start;
        while (cur < end) {
            int len = 0;
            std::string mn = pom2::disassemble6502(snap.data(), cur, len);
            std::printf("  $%04X:", cur);
            for (int j = 0; j < 3; ++j) {
                if (j < len) std::printf(" %02X", snap[cur + j]);
                else         std::printf("   ");
            }
            std::printf("  %s%s\n", mn.c_str(), cur == pc ? "   ; <-- PC" : "");
            cur += len > 0 ? len : 1;
        }
    }

    cpu.dumpPcTrace("final-trace");
    return 0;
}
