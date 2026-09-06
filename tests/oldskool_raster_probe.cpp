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

// OLDSKOOL FORT ET VERT raster probe — diagnostic, not a pinned test.
//
// User report (2026-09-02, screenshot): on the SHADOW-party intro screen
// (big SHADOW letters, TV set, PRESENT), the blue/white raster bands beside
// the TV sit "about 7 pixels" off — one HGR byte = one CPU cycle. The demo
// is French Touch's 8KB intro (GPLv3, github.com/Fr3nchT0uch/
// Oldskool_Fort_Et_Vert), REQUIRING a NON-enhanced IIe (NMOS 6502) + PAL;
// its bands are mid-scanline soft-switch windows, so their pixel edges test
// frameCycleToPos's hpos->byteCol mapping AND the CPU's cycle timing.
//
// This probe boots disks_5.4/demo/oldskool/oldskool.dsk on a //e PAL
// (Disk II slot 6, Mockingboard slot 4), types "BRUN OLDSKOOL", then scans
// frames for mid-scanline switch bursts, dumping the event table (ln, hpos,
// byteCol) and PPM frames for comparison against the art and the sources.
//
// Usage: oldskool_raster_probe [nmos|cmos] [brunAtSec] [maxRunSec]

#include "Apple2Display.h"
#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"
#include "DiskIICard.h"
#include "Mockingboard.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string firstExisting(std::initializer_list<const char*> cands)
{
    namespace fs = std::filesystem;
    for (const char* c : cands) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
        const std::string up = std::string("../") + c;
        if (fs::exists(up, ec)) return up;
    }
    return {};
}

const char* kindName(Memory::VideoEventKind k)
{
    using K = Memory::VideoEventKind;
    switch (k) {
        case K::TextMode:    return "TEXT";
        case K::MixedMode:   return "MIXED";
        case K::Page2:       return "PAGE2";
        case K::HiRes:       return "HIRES";
        case K::EightyCol:   return "80COL";
        case K::Dhgr:        return "DHGR";
        case K::An3:         return "AN3";
        case K::EightyStore: return "80STORE";
        case K::AltChar:     return "ALTCHAR";
    }
    return "?";
}

void writePpm(const char* path, const uint32_t* px, int w, int h)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        const uint32_t p = px[i];
        std::fputc((p >> 0)  & 0xFF, f);
        std::fputc((p >> 8)  & 0xFF, f);
        std::fputc((p >> 16) & 0xFF, f);
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string mode = (argc > 1) ? argv[1] : "nmos";
    const int brunAt   = (argc > 2) ? std::atoi(argv[2]) : 14;
    const int maxRun   = (argc > 3) ? std::atoi(argv[3]) : 90;

    const std::string rom  = firstExisting({"roms/apple2e.rom"});
    const std::string boot = firstExisting({"roms/disk2.rom"});
    const std::string dsk  =
        firstExisting({"disks_5.4/demo/oldskool/oldskool.dsk"});
    if (rom.empty() || boot.empty() || dsk.empty()) {
        std::printf("SKIP: missing rom/disk2.rom/oldskool.dsk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/false)) {
        std::printf("FAIL: load %s\n", rom.c_str());
        return 1;
    }

    M6502 cpu(&mem);
    mem.setCpu(&cpu);

    auto mb = std::make_unique<MockingboardCard>(4, MockingboardCard::Variant::AC);
    mb->setCpu(&cpu);
    mem.slotBus().plug(4, std::move(mb));

    auto d2 = std::make_unique<DiskIICard>();
    if (!d2->loadBootRom(boot) || !d2->insertDisk(dsk)) {
        std::printf("FAIL: Disk II setup for %s\n", dsk.c_str());
        return 1;
    }
    mem.slotBus().plug(6, std::move(d2));

    cpu.setCpuMode(mode == "cmos" ? M6502::CpuMode::CMOS
                                  : M6502::CpuMode::NMOS);
    cpu.hardReset();
    mem.slotBus().reset();

    const VideoTiming& t = pom2VideoTiming(VideoStandard::PAL);
    const uint64_t lineCycles  = static_cast<uint64_t>(t.cyclesPerScanline);
    const uint64_t frameCycles = lineCycles * t.scanlinesPerFrame;

    std::printf("cpu=%s booting %s for %d s, then BRUN OLDSKOOL...\n",
                mode.c_str(), dsk.c_str(), brunAt);
    for (int s = 0; s < brunAt; ++s) cpu.run(POM2_TIMING_PAL.cpuClockHz);

    // Type the launch command, one key per 0.1 s of machine time — the
    // latch is newest-wins, so each key gets time to be consumed by KEYIN.
    const char* cmd = "BRUN OLDSKOOL\r";
    for (const char* c = cmd; *c; ++c) {
        mem.queueKey(static_cast<uint8_t>(*c));
        cpu.run(POM2_TIMING_PAL.cpuClockHz / 10);
    }

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());

    int dumps = 0;
    const int totalFrames = maxRun * 50;
    for (int f = 0; f < totalFrames && dumps < 4; ++f) {
        const uint64_t target = mem.getCycleCounter() + frameCycles;
        while (mem.getCycleCounter() < target) cpu.run(50);
        auto evs = mem.takeVideoEvents();
        disp.render(mem);

        int midline = 0;
        for (const auto& e : evs) {
            const int hpos = static_cast<int>(e.emuCycle % lineCycles);
            if (e.scanline < 192 && hpos >= 25 && hpos < 65) ++midline;
        }
        if (f % 100 == 0) {
            std::printf("t=%5.1fs frame %5d: %3zu events, %3d midline PC=$%04X\n",
                        brunAt + f / 50.0, f, evs.size(), midline,
                        cpu.getProgramCounter());
            char path[64];
            std::snprintf(path, sizeof path, "oldskool_%s_scan_f%05d.ppm",
                          mode.c_str(), f);
            writePpm(path, disp.pixels(), disp.width(), disp.height());
        }
        if (midline > 30) {
            std::printf("== raster frame %d (t=%.1fs): %zu events, %d midline\n",
                        f, brunAt + f / 50.0, evs.size(), midline);
            int shown = 0;
            for (const auto& e : evs) {
                if (shown >= 100) { std::printf("  ...\n"); break; }
                const auto pos = Apple2Display::frameCycleToPos(
                    e.emuCycle, VideoStandard::PAL);
                std::printf("  ln %3u hpos %2d col %2d  %s=%d\n", e.scanline,
                            static_cast<int>(e.emuCycle % lineCycles),
                            pos.byteCol, kindName(e.kind), e.value ? 1 : 0);
                ++shown;
            }
            char path[64];
            std::snprintf(path, sizeof path, "oldskool_%s_raster_f%05d.ppm",
                          mode.c_str(), f);
            writePpm(path, disp.pixels(), disp.width(), disp.height());
            std::printf("  wrote %s\n", path);
            ++dumps;
        }
    }
    return 0;
}
