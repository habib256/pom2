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

// CFFA boot dump — diagnostic: boots //e from a user-supplied HDV/2mg through
// the MAME-faithful CffaCard (real firmware over emulated ATA), runs N seconds,
// dumps the text screen. Set POM2_TRACE_CFFA=1 to log every ATA command.
//
// Usage:
//   POM2_TRACE_CFFA=1 cffa_boot_dump --image "floppyemu/Total Replay v6.1.hdv" [--slot 6] [--seconds 6]
//
// Not a CTest target — debug aid only.

#include "Apple2Display.h"
#include "CffaCard.h"
#include "M6502.h"
#include "Memory.h"
#include "ProbeOutDir.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace {

bool fileExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}
std::string findFirst(std::initializer_list<const char*> c) {
    for (const char* x : c) if (fileExists(x)) return x;
    return {};
}
void dumpScreen(Memory& mem, const char* label) {
    std::printf("=== SCREEN %s ===\n", label);
    for (int row = 0; row < 24; ++row) {
        const int base = 0x400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            uint8_t b = mem.data()[base + col] & 0x7F;
            if (b < 0x20) b = ' ';
            std::putchar(static_cast<char>(b));
        }
        std::putchar('\n');
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string image;
    int slot = 6, budgetSec = 6;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--image"   && i + 1 < argc) image = argv[++i];
        else if (a == "--slot"    && i + 1 < argc) slot = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) budgetSec = std::atoi(argv[++i]);
    }
    if (image.empty()) {
        std::fprintf(stderr, "Usage: %s --image <path> [--slot 6] [--seconds 6]\n", argv[0]);
        return 1;
    }

    const std::string romPath = findFirst({
        "roms/apple2e.rom", "../roms/apple2e.rom", "../../roms/apple2e.rom" });
    const std::string cffaRom = findFirst({
        "roms/cffa20eec02.bin", "../roms/cffa20eec02.bin", "../../roms/cffa20eec02.bin",
        "roms/cffa20ee02.bin",  "../roms/cffa20ee02.bin",  "../../roms/cffa20ee02.bin" });
    if (romPath.empty() || cffaRom.empty()) {
        std::fprintf(stderr, "missing apple2e.rom or cffa firmware under roms/\n");
        return 1;
    }
    std::printf("rom=%s\ncffa=%s\nimage=%s\nslot=%d\n",
                romPath.c_str(), cffaRom.c_str(), image.c_str(), slot);

    Memory mem;
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n");
        return 1;
    }

    auto cffa = std::make_unique<pom2::CffaCard>(slot);
    if (!cffa->loadRom(cffaRom)) {
        std::fprintf(stderr, "CFFA loadRom failed: %s\n", cffa->getLastError().c_str());
        return 1;
    }
    if (!cffa->loadImage(image)) {
        std::fprintf(stderr, "CFFA loadImage failed: %s\n", cffa->getLastError().c_str());
        return 1;
    }
    std::printf("blocks=%zu\n", cffa->getBlockCount());
    mem.slotBus().plug(slot, std::move(cffa));

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());

    M6502 cpu(&mem);
    cpu.hardReset();
    mem.slotBus().reset();

    // Cold boot: the //e autostart firmware scans slots; CFFA ($Cn07=$3C) is
    // bootable, but to be deterministic we PR#<slot> by jumping $Cn00.
    cpu.setProgramCounter(static_cast<uint16_t>(0xC000 + slot * 0x100));

    const int kBudget = budgetSec * 1'022'727;
    int total = 0, lastSec = -1;
    while (total < kBudget) {
        total += cpu.run(64);
        const int sec = total / 1'022'727;
        if (sec != lastSec) {
            std::printf("[t=%2ds] PC=$%04X A=%02X X=%02X Y=%02X SP=%02X\n",
                        sec, cpu.getProgramCounter(), cpu.getAccumulator(),
                        cpu.getXRegister(), cpu.getYRegister(),
                        cpu.getStackPointer());
            lastSec = sec;
        }
    }
    dumpScreen(mem, "final");

    // Final framebuffer → PPM (visual confirmation of the graphical boot).
    disp.render(mem);
    if (const uint32_t* px = disp.pixels()) {
        const int w = disp.width(), h = disp.height();
        // Under pom2test::probeOutDir(), not a fixed /tmp (tools/check_probe_outputs.sh).
        const std::string ppm = pom2test::probeOutDir() + "/cffa_boot.ppm";
        if (FILE* f = std::fopen(ppm.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; ++i) {
                const uint32_t p = px[i];
                const unsigned char rgb[3] = {
                    static_cast<unsigned char>( p        & 0xFF),
                    static_cast<unsigned char>((p >>  8) & 0xFF),
                    static_cast<unsigned char>((p >> 16) & 0xFF) };
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
            std::printf("wrote %s\n", ppm.c_str());
        }
    }
    return 0;
}
