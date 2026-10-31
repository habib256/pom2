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

// premium_softcard_probe — boot a Premium SoftCard //e disk on an enhanced
// //e and log every access the 6502 makes to $C000-$CFFF, by PC. Manual tool
// (probes target): the card sits in the auxiliary slot and no document gives
// its registers, so this is how they are read off Microsoft's software. No
// assertion; untracked media.
//
//   premium_softcard_probe DISK [SECONDS=10] [--out DIR]
//
// Prints the collapsed access log, the 80-column screen, and writes main and
// aux RAM and the CPU's view to DIR/psc_{main,aux,cpu}.bin.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryWatchSink.h"
#include "ProbeOutDir.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct Logger : pom2::MemoryWatchSink {
    M6502* cpu = nullptr;
    struct Hit { uint16_t pc; bool w; uint16_t addr; uint8_t v; };
    std::vector<Hit> log;
    void noteAccess(uint16_t addr, uint8_t v, bool write) override
    {
        if (addr == 0xC000 || addr == 0xC010) return;   // the keyboard poll
        if (addr >= 0xC0E0 && addr <= 0xC0EF) return;    // the Disk II
        if (log.size() < 400000) log.push_back({ cpu->getProgramCounter(), write, addr, v });
    }
};

uint16_t textRowBase(int row) { return uint16_t(0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28); }

} // namespace

int main(int argc, char** argv)
{
    std::string outArg;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outArg = argv[++i];
        else pos.push_back(a);
    }
    if (pos.empty()) {
        std::fprintf(stderr, "usage: premium_softcard_probe DISK [SECONDS] [--out DIR]\n");
        return 2;
    }
    const int seconds = pos.size() > 1 ? std::atoi(pos[1].c_str()) : 10;

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    if (!mem.loadAppleIIRom("roms/apple2e.rom")) return 1;
    auto disk = std::make_unique<DiskIICard>(6);
    if (!disk->loadBootRom("roms/disk2.rom") || !disk->insertDisk(pos[0])) return 1;
    mem.slotBus().plug(6, std::move(disk));
    Logger lg;
    lg.cpu = &cpu;
    mem.setWatchSink(&lg);
    for (int a = 0xC001; a < 0xD000; ++a) {
        mem.setReadWatch(uint16_t(a), true);
        mem.setWriteWatch(uint16_t(a), true);
    }
    cpu.hardReset();
    mem.slotBus().reset();
    for (int64_t t = 0; t < int64_t(seconds) * 1'022'727;) {
        const int n = cpu.run(4096);
        t += n > 0 ? n : 4096;
    }

    std::map<std::tuple<uint16_t, bool, uint16_t, uint8_t>, int> seen;
    std::vector<std::tuple<uint16_t, bool, uint16_t, uint8_t>> order;
    for (const auto& h : lg.log) {
        // Slot ROM fetches of the boot PROM and the Monitor's own I/O are
        // noise; keep everything else.
        if (h.addr >= 0xC600 && h.addr < 0xC700) continue;
        const auto k = std::make_tuple(h.pc, h.w, h.addr, h.v);
        if (seen[k]++ == 0) order.push_back(k);
    }
    for (const auto& k : order)
        std::printf("pc=%04X %c %04X %02X  x%d\n", std::get<0>(k), std::get<1>(k) ? 'W' : 'R',
                    std::get<2>(k), std::get<3>(k), seen[k]);
    std::printf("PC now %04X\n", cpu.getProgramCounter());
    const uint8_t* aux = mem.auxData();
    for (int r = 0; r < 24; ++r) {
        std::string line;
        for (int c = 0; c < 80; ++c) {
            const uint16_t a = uint16_t(textRowBase(r) + c / 2);
            const char ch = char(((c & 1) ? mem.peekMainRam(a) : aux[a]) & 0x7F);
            line.push_back(ch >= 0x20 && ch < 0x7F ? ch : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        std::printf("|%s\n", line.c_str());
    }
    const std::string dir = pom2test::probeOutDir(outArg);
    if (FILE* f = std::fopen((dir + "/psc_main.bin").c_str(), "wb")) {
        for (int a = 0; a < 0x10000; ++a) {
            const uint8_t b = mem.peekMainRam(uint16_t(a));
            std::fwrite(&b, 1, 1, f);
        }
        std::fclose(f);
    }
    if (FILE* f = std::fopen((dir + "/psc_aux.bin").c_str(), "wb")) {
        std::fwrite(aux, 1, 0x10000, f);
        std::fclose(f);
    }
    std::vector<uint8_t> view(0x10000);
    mem.snapshotCpuView(view.data());   // with the language card as mapped now
    if (FILE* f = std::fopen((dir + "/psc_cpu.bin").c_str(), "wb")) {
        std::fwrite(view.data(), 1, view.size(), f);
        std::fclose(f);
    }
    std::printf("RAM -> %s/psc_{main,aux,cpu}.bin\n", dir.c_str());
    return 0;
}
