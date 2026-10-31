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

// applicard_card — the PCPI Appli-Card (ApplicardCard.h).
//
// Unit part, synthetic ROM: the latches and their status bits on both sides,
// the ROM overlay over $0000-$7FFF (and the RAM under it, writable only once
// the ROM is off), $C0n5 reset keeping the RAM, $C0n7 NMI, the snapshot.
//
// Boot part: PCPI's CP/M 2.2 (disks_5.4/dsk/applicard_cpm22.dsk, the boot
// disk izapple2 ships) with the real v9 ROM (roms/applicard_v9.bin) on an
// enhanced //e, card in slot 4: "PCPI Ver 2.0" and `A>` on the 80-column
// screen, then DIR lists the disk.

#include "ApplicardCard.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kSecond = 1'022'727;

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

std::vector<uint8_t> slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

// A ROM that sends $5A to the 6502, waits for a byte, stores it at $9000,
// turns itself off, then loops at $8000 (planted in RAM by the test):
//   0000 3E 5A     LD A,$5A        0002 D3 00     OUT ($00),A
//   0004 DB 40     IN A,($40)      0006 07        RLCA
//   0007 30 FB     JR NC,$0004     0009 DB 20     IN A,($20)
//   000B 32 00 90  LD ($9000),A    000E 32 00 10  LD ($1000),A  (ROM on: lost)
//   0011 AF        XOR A           0012 D3 60     OUT ($60),A   (ROM off)
//   0014 C3 00 80  JP $8000
std::vector<uint8_t> testRom()
{
    std::vector<uint8_t> r(ApplicardCard::kRomBytes, 0x00);
    const uint8_t code[] = {
        0x3E, 0x5A, 0xD3, 0x00, 0xDB, 0x40, 0x07, 0x30, 0xFB, 0xDB, 0x20,
        0x32, 0x00, 0x90, 0x32, 0x00, 0x10, 0xAF, 0xD3, 0x60, 0xC3, 0x00, 0x80,
    };
    std::copy(std::begin(code), std::end(code), r.begin());
    return r;
}

void testLatches()
{
    ApplicardCard c;
    CHECK(!c.loadRom(std::vector<uint8_t>(100)));
    CHECK(c.loadRom(testRom()));
    CHECK(c.romOn());
    c.advanceCycles(20);
    CHECK(c.deviceSelectRead(3) == 0x80);         // $5A waits
    CHECK(c.deviceSelectRead(0) == 0x5A);
    CHECK(c.deviceSelectRead(3) == 0x00);
    c.advanceCycles(50);                           // polling $40
    CHECK(c.deviceSelectRead(2) == 0x00);
    c.deviceSelectWrite(1, 0xA7);
    CHECK(c.deviceSelectRead(2) == 0x80);          // not taken yet
    CHECK(c.deviceSelectRead(1) == 0xA7);          // reads back
    c.advanceCycles(50);
    CHECK(c.deviceSelectRead(2) == 0x00);          // taken
    CHECK(!c.romOn());
    // $C0n5: reset — ROM back on, PC 0, RAM kept (the ROM's warm start).
    c.deviceSelectRead(5);
    CHECK(c.romOn() && c.z80().getState().pc == 0x0000);
    std::vector<uint8_t> blob;
    c.appendSnapshotState(blob);
    CHECK(blob[4 + 3 + 32 + 0x9000] == 0xA7);      // the $9000 store landed,
    CHECK(blob[4 + 3 + 32 + 0x1000] == 0x00);      // the $1000 one met the ROM

    // $C0n8-$C0nF are not decoded.
    CHECK(c.deviceSelectRead(0xD) == 0xFF && c.romOn() && c.z80().getState().pc == 0);

    // $C0n7: NMI — the Z80 goes to $0066.
    ApplicardCard n;
    n.loadRom(std::vector<uint8_t>(ApplicardCard::kRomBytes, 0x00));   // NOPs
    n.advanceCycles(10);
    n.deviceSelectWrite(7, 0);
    n.advanceCycles(1);
    CHECK(n.z80().getState().pc >= 0x0066 && n.z80().getState().pc < 0x0070);
}

void testSnapshot()
{
    ApplicardCard a;
    a.loadRom(testRom());
    a.advanceCycles(20);
    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    ApplicardCard b;
    b.loadRom(testRom());
    b.loadSnapshotState(blob.data(), blob.size());
    CHECK(b.z80().getState().pc == a.z80().getState().pc);
    CHECK(b.deviceSelectRead(0) == 0x5A);
}

uint16_t textRowBase(int row) { return uint16_t(0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28); }

std::vector<std::string> screen80(Memory& mem)
{
    std::vector<std::string> rows;
    const uint8_t* aux = mem.auxData();
    for (int r = 0; r < 24; ++r) {
        std::string s;
        for (int c = 0; c < 80; ++c) {
            const uint16_t a = uint16_t(textRowBase(r) + c / 2);
            uint8_t ch = uint8_t(((c & 1) ? mem.memRead(a) : aux[a]) & 0x7F);
            if (ch < 0x20) ch = uint8_t(ch + 0x40);
            s.push_back(ch < 0x7F ? char(ch) : '.');
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        rows.push_back(s);
    }
    return rows;
}

bool onScreen80(Memory& mem, const std::string& text)
{
    for (const auto& r : screen80(mem)) if (r.find(text) != std::string::npos) return true;
    return false;
}

void dump80(Memory& mem)
{
    for (const auto& r : screen80(mem)) std::fprintf(stderr, "|%s\n", r.c_str());
}

std::string findFirst(std::initializer_list<const char*> c)
{
    for (const char* p : c) if (fs::is_regular_file(p)) return p;
    return {};
}

// 0 = ran (failures counted in g_fail), 77 = media missing.
int testBoot()
{
    const std::string rom    = findFirst({ "roms/apple2e.rom" });
    const std::string prom   = findFirst({ "roms/disk2.rom" });
    const std::string z80rom = findFirst({ ApplicardCard::kRomPath });
    const std::string master = findFirst({ "disks_5.4/dsk/applicard_cpm22.dsk" });
    if (rom.empty() || prom.empty() || z80rom.empty() || master.empty())
        return 77;
    const fs::path tmp = fs::temp_directory_path() / "pom2_applicard.dsk";
    std::error_code ec;
    fs::copy_file(master, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "FAIL: copy %s\n", master.c_str()); ++g_fail; return 0; }

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    auto d2 = std::make_unique<DiskIICard>(6);
    auto card = std::make_unique<ApplicardCard>();
    if (!mem.loadAppleIIRom(rom.c_str()) || !d2->loadBootRom(prom)
        || !d2->insertDisk(tmp.string()) || !card->loadRom(slurp(z80rom))) {
        std::fprintf(stderr, "FAIL: machine setup\n");
        ++g_fail;
        return 0;
    }
    mem.slotBus().plug(6, std::move(d2));
    mem.slotBus().plug(4, std::move(card));
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) {
        for (int64_t t = 0; t < cycles;) t += cpu.run(4096);
    };

    run(30 * kSecond);
    const bool booted = onScreen80(mem, "PCPI Ver 2.0") && onScreen80(mem, "A>");
    CHECK(booted);
    if (!booted) { dump80(mem); return 0; }
    mem.pasteText("DIR\r");
    run(5 * kSecond);
    CHECK(onScreen80(mem, ": PIP      COM : XSUB     COM"));
    if (g_fail || std::getenv("POM2_DUMP")) dump80(mem);
    return 0;
}

} // namespace

int main()
{
    testLatches();
    testSnapshot();
    const int boot = testBoot();
    std::error_code ec;
    fs::remove(fs::temp_directory_path() / "pom2_applicard.dsk", ec);
    if (g_fail) {
        std::fprintf(stderr, "applicard_card: %d failure(s)\n", g_fail);
        return 1;
    }
    if (boot == 77) {
        std::printf("applicard_card: SKIP boot part — needs roms/apple2e.rom, "
                    "roms/disk2.rom, roms/applicard_v9.bin, "
                    "disks_5.4/dsk/applicard_cpm22.dsk\n");
        return 77;
    }
    std::printf("applicard_card: PASS\n");
    return 0;
}
