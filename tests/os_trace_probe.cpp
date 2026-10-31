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

// os_trace_probe — boot a //e enhanced on a disk with the OS-call traces on
// and print what they say (OsCallTrace.h). A manual tool (`probes` target):
// it is how each trace was checked against the system it decodes.
//
//   os_trace_probe [--disk a.dsk] [--35 b.po] [--trace mli,sp,pascal,mon,
//                  disk,acorn,cpm65] [--seconds N] [--type SEC:TEXT]...
//                  [--dump ADDR:LEN]... [--rom roms/apple2e.rom]
//
// --disk goes in the Disk II of slot 6, --35 in unit 1 of a SmartPort card
// in slot 5; with only --35 there is no Disk II (an empty one would hold the
// autostart scan). TEXT takes \r for
// Return. The screen is printed at the end, then each dump (CPU view).

#include "Debugger.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "OsCallTrace.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string unescape(const std::string& s)
{
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            o.push_back(n == 'r' ? '\r' : n == 'n' ? '\n' : n);
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
}

std::string screen(Memory& mem)
{
    static const int rowBase[24] = {
        0x400, 0x480, 0x500, 0x580, 0x600, 0x680, 0x700, 0x780,
        0x428, 0x4A8, 0x528, 0x5A8, 0x628, 0x6A8, 0x728, 0x7A8,
        0x450, 0x4D0, 0x550, 0x5D0, 0x650, 0x6D0, 0x750, 0x7D0 };
    const bool eighty = mem.getDisplayState().eightyCol;
    const uint8_t* aux = mem.auxData();
    std::string out;
    for (int r = 0; r < 24; ++r) {
        std::string line;
        for (int c = 0; c < (eighty ? 80 : 40); ++c) {
            const uint16_t a = uint16_t(rowBase[r] + (eighty ? c / 2 : c));
            uint8_t ch = uint8_t(((eighty && (c & 1) == 0) ? aux[a] : mem.peekMainRam(a)) & 0x7F);
            if (ch < 0x20) ch = uint8_t(ch + 0x40);
            line.push_back(ch == 0x7F ? ' ' : char(ch));
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += "|" + line + "\n";
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    std::string rom = "roms/apple2e.rom", disk, disk35;
    double seconds = 20;
    pom2::OsTraceKinds k;
    struct Typed { double at; std::string text; };
    std::vector<Typed> typed;
    std::vector<std::pair<unsigned, unsigned>> dumps;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--rom") rom = next();
        else if (a == "--disk") disk = next();
        else if (a == "--35") disk35 = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--type") {
            const std::string v = next();
            const auto colon = v.find(':');
            typed.push_back({ std::atof(v.substr(0, colon).c_str()), unescape(v.substr(colon + 1)) });
        } else if (a == "--dump") {
            const std::string v = next();
            const auto colon = v.find(':');
            dumps.push_back({ unsigned(std::strtoul(v.substr(0, colon).c_str(), nullptr, 16)),
                              unsigned(std::strtoul(v.substr(colon + 1).c_str(), nullptr, 0)) });
        } else if (a == "--trace") {
            const std::string v = "," + next() + ",";
            auto has = [&](const char* n) { return v.find(std::string(",") + n + ",") != std::string::npos; };
            k.mli = has("mli"); k.smartPort = has("sp"); k.pascalBios = has("pascal");
            k.monitor = has("mon"); k.disk = has("disk"); k.applecorn = has("acorn");
            k.cpm65 = has("cpm65");
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    if (!mem.loadAppleIIRom(rom.c_str())) { std::fprintf(stderr, "no ROM %s\n", rom.c_str()); return 1; }
    // An empty Disk II would hold the autostart scan for ever: plugged only
    // when it has a disk, or when nothing else does.
    if (!disk.empty() || disk35.empty()) {
        auto d2 = std::make_unique<DiskIICard>(6);
        if (!disk.empty() && !d2->insertDisk(disk)) { std::fprintf(stderr, "cannot mount %s\n", disk.c_str()); return 1; }
        mem.slotBus().plug(6, std::move(d2));
    }
    if (!disk35.empty()) {
        auto sp = std::make_unique<pom2::SmartPortCard>(5);
        sp->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
        std::string err;
        if (!sp->mountBay(0, disk35, err)) { std::fprintf(stderr, "cannot mount %s: %s\n", disk35.c_str(), err.c_str()); return 1; }
        mem.slotBus().plug(5, std::move(sp));
    }

    pom2::Debugger dbg;
    dbg.setMemory(&mem);
    pom2::OsCallTrace& t = dbg.osCalls();
    t.bind(&cpu, &mem);
    t.setMli(k.mli); t.setSmartPort(k.smartPort); t.setPascal(k.pascalBios);
    t.setMonitor(k.monitor); t.setApplecorn(k.applecorn); t.setCpm65(k.cpm65); t.setDisk(k.disk);
    t.setSink([](const std::string& line) { std::printf("%s\n", line.c_str()); });
    mem.slotBus().setCardTrace(k.disk ? &t : nullptr);
    cpu.setDebugHook(&dbg);

    cpu.hardReset();
    mem.slotBus().reset();
    constexpr double kHz = 1022727.0;
    double now = 0;
    for (std::size_t n = 0; now < seconds; ) {
        cpu.run(4096);
        now = double(cpu.getCycleCountNow()) / kHz;
        while (n < typed.size() && typed[n].at <= now) mem.pasteText(typed[n++].text);
    }
    std::printf("%s", screen(mem).c_str());
    for (const auto& d : dumps) {
        for (unsigned a = d.first; a < d.first + d.second; a += 16) {
            std::printf("%04X:", a);
            for (unsigned i = 0; i < 16 && a + i < d.first + d.second; ++i)
                std::printf(" %02X", mem.peekCpuView(uint16_t(a + i)));
            std::printf("\n");
        }
    }
    return 0;
}
