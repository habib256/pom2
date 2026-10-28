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

// als_boot_probe — boot an ALS "The CP/M Card" disk against a STUB card and
// log what the boot code asks of it. Manual tool (probes target): the card's
// register map is documented nowhere, so this is how it is read off the
// software. No assertion; untracked media.
//
//   als_boot_probe ROM DISK [SLOT=4] [SECONDS=10] [--out DIR]
//
// Prints every access to the stub's slot — $C0n0-$C0nF, $Cn00-$CnFF, and
// $C800-$CFFF while it holds the window — with the 6502 PC, then the text
// page, and writes the 64 KB main RAM to DIR/als_ram.bin.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "ProbeOutDir.h"
#include "SlotPeripheral.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct StubCard : SlotPeripheral {
    M6502* cpu = nullptr;
    struct Hit { uint16_t pc; char kind; uint16_t addr; uint8_t v; };
    std::vector<Hit> log;
    int slot = 4;

    std::string_view name() const override { return "ALS stub"; }
    void note(char k, uint16_t a, uint8_t v)
    {
        if (log.size() < 200000) log.push_back({ cpu->getProgramCounter(), k, a, v });
    }
    uint8_t deviceSelectRead(uint8_t r) override
    { note('R', uint16_t(0xC080 + slot * 16 + r), 0xFF); return 0xFF; }
    void deviceSelectWrite(uint8_t r, uint8_t v) override
    { note('W', uint16_t(0xC080 + slot * 16 + r), v); }
    uint8_t slotRomRead(uint8_t a) override
    { note('r', uint16_t(0xC000 + slot * 256 + a), 0xFF); return 0xFF; }
    void slotRomWrite(uint8_t a, uint8_t v) override
    { note('w', uint16_t(0xC000 + slot * 256 + a), v); }
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
    if (pos.size() < 2) {
        std::fprintf(stderr, "usage: als_boot_probe ROM DISK [SLOT] [SECONDS] [--out DIR]\n");
        return 2;
    }
    const int slot = pos.size() > 2 ? std::atoi(pos[2].c_str()) : 4;
    const int seconds = pos.size() > 3 ? std::atoi(pos[3].c_str()) : 10;

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    if (!mem.loadAppleIIRom(pos[0].c_str())) return 1;
    auto disk = std::make_unique<DiskIICard>(6);
    if (!disk->loadBootRom("roms/disk2.rom") || !disk->insertDisk(pos[1])) return 1;
    mem.slotBus().plug(6, std::move(disk));
    auto stub = std::make_unique<StubCard>();
    StubCard* s = stub.get();
    s->cpu = &cpu;
    s->slot = slot;
    mem.slotBus().plug(slot, std::move(stub));
    cpu.hardReset();
    mem.slotBus().reset();
    cpu.setProgramCounter(0xC600);
    for (int64_t t = 0; t < int64_t(seconds) * 1'022'727;) t += cpu.run(4096);

    // Collapse repeats: one line per (pc, kind, addr, value) in first-seen order.
    std::map<std::tuple<uint16_t, char, uint16_t, uint8_t>, int> seen;
    std::vector<std::tuple<uint16_t, char, uint16_t, uint8_t>> order;
    for (const auto& h : s->log) {
        const auto k = std::make_tuple(h.pc, h.kind, h.addr, h.v);
        if (seen[k]++ == 0) order.push_back(k);
    }
    for (const auto& k : order)
        std::printf("pc=%04X %c %04X %02X  x%d\n", std::get<0>(k), std::get<1>(k),
                    std::get<2>(k), std::get<3>(k), seen[k]);
    std::printf("PC now %04X\n", cpu.getProgramCounter());
    for (int r = 0; r < 24; ++r) {
        std::string line;
        for (int c = 0; c < 40; ++c) {
            const char ch = char(mem.memRead(uint16_t(textRowBase(r) + c)) & 0x7F);
            line.push_back(ch >= 0x20 && ch < 0x7F ? ch : ' ');
        }
        std::printf("|%s|\n", line.c_str());
    }
    const std::string out = pom2test::probeOutDir(outArg) + "/als_ram.bin";
    if (FILE* f = std::fopen(out.c_str(), "wb")) {
        for (int a = 0; a < 0x10000; ++a) {
            const uint8_t b = (a >= 0xC000 && a < 0xD000) ? 0 : mem.memRead(uint16_t(a));
            std::fwrite(&b, 1, 1, f);
        }
        std::fclose(f);
        std::printf("RAM -> %s\n", out.c_str());
    }
    return 0;
}
