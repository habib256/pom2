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

// DIX menu raster probe — diagnostic, not a pinned test.
//
// TODO.md "Next up" §2: on the DIX menu (the TV set), the moving colour
// rasters are GR windows cut into the HIRES frame by Mockingboard-T2-synced
// code (MENU/main.a RASTER1/2: STA $C056 … WAIT55 … STA $C057, plus a
// $C054/$C055 page swap in RASTER1/3). The symptom is the horizontal
// placement of those mid-line switches: the hidden span widens by about one
// character cell on each side.
//
// This probe boots DIX (800K .po, //e PAL + Mockingboard slot 4 + SmartPort
// slot 5), sits on the menu, and for a handful of frames dumps (a) the
// published video-event log — kind, scanline, hpos, and the byteCol
// frameCycleToPos maps it to — and (b) the rendered RGBA frame as PPM, so
// the GR window's measured pixel edges can be compared against both the
// event columns and the HIRES TV-frame art around them.
//
// Usage: dix_menu_raster_probe [disk.po] [bootSecs] [frames]

#include "Apple2Display.h"
#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"
#include "DiskIICard.h"
#include "MemoryWatchSink.h"
#include "Mockingboard.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
        const uint8_t rgb[3] = { static_cast<uint8_t>(p & 0xFF),
                                 static_cast<uint8_t>((p >> 8) & 0xFF),
                                 static_cast<uint8_t>((p >> 16) & 0xFF) };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

}  // namespace

namespace {
struct ViaWriteLog : pom2::MemoryWatchSink {
    struct Rec { uint64_t cycle; uint16_t addr; uint8_t value; };
    std::vector<Rec> recs;
    M6502* cpu = nullptr;
    void noteAccess(uint16_t addr, uint8_t value, bool write) override
    {
        if (!write || (addr & 0xFFFE) != 0xC404) return;
        recs.push_back({ cpu ? cpu->getCycleCountNow() : 0, addr, value });
    }
};
}  // namespace

int main(int argc, char** argv)
{
    const std::string rom = firstExisting({"roms/apple2e.rom"});
    const std::string po  = (argc > 1) ? std::string(argv[1])
                                       : firstExisting({"disks_3.5/DIX-fix.po",
                                                        "disks_3.5/DIX.po"});
    const int bootSecs = (argc > 2) ? std::atoi(argv[2]) : 30;
    const int frames   = (argc > 3) ? std::atoi(argv[3]) : 6;
    if (rom.empty() || po.empty()) {
        std::printf("SKIP: missing apple2e.rom or DIX .po\n");
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

    // .po → SmartPort slot 5 (DIX); .dsk → Disk II slot 6 (TRIBU & friends).
    if (po.size() > 4 && po.compare(po.size() - 4, 4, ".dsk") == 0) {
        const std::string boot = firstExisting({"roms/disk2.rom"});
        auto d2 = std::make_unique<DiskIICard>();
        if (boot.empty() || !d2->loadBootRom(boot) || !d2->insertDisk(po)) {
            std::printf("FAIL: Disk II setup for %s\n", po.c_str());
            return 1;
        }
        mem.slotBus().plug(6, std::move(d2));
    } else {
        auto card = std::make_unique<pom2::SmartPortCard>(5);
        auto u0   = std::make_unique<pom2::SmartPort35Unit>();
        if (!u0->loadImage(po)) {
            std::printf("FAIL: %s: %s\n", po.c_str(), u0->lastError().c_str());
            return 1;
        }
        card->setUnit(0, std::move(u0));
        mem.slotBus().plug(5, std::move(card));
    }

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();
    mem.slotBus().reset();

    const VideoTiming& t = pom2VideoTiming(VideoStandard::PAL);
    const uint64_t lineCycles  = static_cast<uint64_t>(t.cyclesPerScanline);
    const uint64_t frameCycles = lineCycles * t.scanlinesPerFrame;

    std::printf("booting %s for %d s...\n", po.c_str(), bootSecs);
    for (int s = 0; s < bootSecs; ++s) cpu.run(POM2_TIMING_PAL.cpuClockHz);
    std::printf("PC=$%04X text=%d hires=%d page2=%d\n", cpu.getProgramCounter(),
                mem.getDisplayState().textMode ? 1 : 0,
                mem.getDisplayState().hiRes ? 1 : 0,
                mem.getDisplayState().page2 ? 1 : 0);

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());

    ViaWriteLog wlog;
    wlog.cpu = &cpu;
    mem.setWatchSink(&wlog);
    mem.setWriteWatch(0xC404, true);
    mem.setWriteWatch(0xC405, true);

    for (int f = 0; f < frames; ++f) {
        const uint64_t target = mem.getCycleCounter() + frameCycles;
        while (mem.getCycleCounter() < target) cpu.run(50);
        const auto evs = mem.takeVideoEvents();
        disp.render(mem);
        std::printf("frame %d: %dx%d, %zu events\n", f, disp.width(),
                    disp.height(), evs.size());
        for (const auto& e : evs) {
            const auto pos = Apple2Display::frameCycleToPos(e.emuCycle,
                                                            VideoStandard::PAL);
            std::printf("  ln %3u hpos %2d col %2d  %s=%d\n", e.scanline,
                        static_cast<int>(e.emuCycle % lineCycles), pos.byteCol,
                        kindName(e.kind), e.value ? 1 : 0);
        }
        // T1 re-arm trace: pair the $C404 (latch lo) / $C405 (arm) writes.
        for (size_t i = 0; i + 1 < wlog.recs.size(); ++i) {
            const auto& lo = wlog.recs[i];
            const auto& hi = wlog.recs[i + 1];
            if (lo.addr == 0xC404 && hi.addr == 0xC405) {
                const uint16_t v = static_cast<uint16_t>(lo.value | (hi.value << 8));
                std::printf("  arm @%llu ln %3d hpos %2d V=%u\n",
                            static_cast<unsigned long long>(hi.cycle),
                            static_cast<int>((hi.cycle / lineCycles) % 312),
                            static_cast<int>(hi.cycle % lineCycles), v);
                ++i;
            }
        }
        wlog.recs.clear();
        char path[64];
        std::snprintf(path, sizeof(path), "dix_menu_f%d.ppm", f);
        writePpm(path, disp.pixels(), disp.width(), disp.height());
        std::printf("  wrote %s\n", path);
    }
    return 0;
}
