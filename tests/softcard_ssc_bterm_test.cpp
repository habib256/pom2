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

// softcard_ssc_bterm — how the Microsoft SoftCard CP/M 2.20 BIOS routes the
// console and the reader through the IOBYTE, with a Super Serial Card in
// slot 2 and BTERM (tests/data/cpm/BTERM.COM, ~/src/a2terminal) on the disk.
//
// BTERM is a CP/M terminal that uses only BIOS calls: it points the IOBYTE's
// CON: field at each device in turn around a CONST to find the one wired to
// the serial card. That needs a BIOS whose CONST honours the IOBYTE. The
// 44K 2.20 BIOS does not, read off its code once booted (Z80 addresses):
//
//   CONST  $AB08  LD HL,($F380) / JP (HL)     — no IOBYTE test at all;
//          $F34A  slot-3 card type 3 ? $C0BE status : $C000 bit 7
//   CONIN  $AB50  CON: field 2 (BAT:) → ($F38A) = the READER driver,
//                 3 (UC1:) → ($F384), 0/1 → ($F382)
//   READER $AB87  RDR: field 1 (PTR:) → ($F38A) = slot 2, the SSC
//
// So this pins four things, each the BIOS's and none BTERM's to fix:
//   1. PIP CON:=RDR: copies serial input to the screen (READER → SSC);
//   2. BTERM P shows 00 in every column and never touches slot 2 while a
//      byte waits in the SSC — CONST is the keyboard whatever the IOBYTE;
//   3. a key on the Apple ends BTERM P (CONST/CONIN on the keyboard);
//   4. after STAT CON:=BAT:, the CCP's command line comes from the SSC —
//      CONIN does follow the IOBYTE.
//
// Gated like softcard_cpm_boot on roms/apple2p.rom + roms/disk2.rom +
// disks_5.4/dsk/cpm22.dsk (tracked) and the SSC EPROM roms/ssc_341-0065-a.bin.
// The master is never written: BTERM goes onto a temp copy (CpmDiskFs).

#include "CpmDiskFs.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "SoftCardZ80.h"
#include "SuperSerialCard.h"

#include <cstdint>
#include <cstdio>
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

uint16_t textRowBase(int row) { return uint16_t(0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28); }

// The 40-column page as 24 lines; the inverse/flashing cursor shows as `.`.
std::vector<std::string> screen(Memory& mem)
{
    std::vector<std::string> rows;
    for (int r = 0; r < 24; ++r) {
        std::string s;
        for (int c = 0; c < 40; ++c) {
            const char ch = char(mem.memRead(uint16_t(textRowBase(r) + c)) & 0x7F);
            s.push_back(ch >= 0x20 && ch < 0x7F ? ch : '.');
        }
        rows.push_back(s);
    }
    return rows;
}

bool onScreen(Memory& mem, const std::string& text)
{
    for (const auto& r : screen(mem)) if (r.find(text) != std::string::npos) return true;
    return false;
}

// The last line holding `text`, so a re-printed prompt is told apart.
int lastRowWith(Memory& mem, const std::string& text)
{
    const auto rows = screen(mem);
    for (int r = 23; r >= 0; --r) if (rows[size_t(r)].find(text) != std::string::npos) return r;
    return -1;
}

void dump(Memory& mem)
{
    for (const auto& r : screen(mem)) std::fprintf(stderr, "|%s|\n", r.c_str());
}

struct Rig {
    Memory mem;
    M6502 cpu{&mem};
    SoftCardZ80* card = nullptr;
    SuperSerialCard* ssc = nullptr;

    // EmulationController::runCpuSlice's arbitration, without the pacing.
    void run(int64_t cycles)
    {
        for (int64_t total = 0; total < cycles;) {
            const int spent = card->dmaActive() ? card->dmaRun(4096) : cpu.run(4096);
            total += spent > 0 ? spent : 4096;
        }
    }
    // Type a command and give CP/M time to load and run it.
    void type(const char* s, int64_t seconds)
    {
        mem.pasteText(s);
        run(seconds * kSecond);
    }
    void serial(const std::string& s)
    {
        ssc->deliverRxBytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
};

std::string findFirst(std::initializer_list<const char*> c)
{
    for (const char* p : c) if (fs::is_regular_file(p)) return p;
    return {};
}

} // namespace

int main()
{
    const std::string rom    = findFirst({ "roms/apple2p.rom" });
    const std::string prom   = findFirst({ "roms/disk2.rom" });
    const std::string sscRom = findFirst({ "roms/ssc_341-0065-a.bin" });
    const std::string master = findFirst({ "disks_5.4/dsk/cpm22.dsk" });
    const std::string bterm  = findFirst({ "tests/data/cpm/BTERM.COM" });
    if (rom.empty() || prom.empty() || sscRom.empty() || master.empty() || bterm.empty()) {
        std::printf("softcard_ssc_bterm: SKIP (needs roms/apple2p.rom, roms/disk2.rom, "
                    "roms/ssc_341-0065-a.bin, disks_5.4/dsk/cpm22.dsk, tests/data/cpm/BTERM.COM)\n");
        return 77;
    }

    pom2::cpmfs::CpmDisk disk;
    std::string err;
    if (!disk.open(slurp(master), pom2::cpmfs::SectorOrder::Dos, err)
        || !disk.writeFile(0, "BTERM.COM", slurp(bterm), err)) {
        std::fprintf(stderr, "FAIL: preparing the disk: %s\n", err.c_str());
        return 1;
    }
    const fs::path tmp = fs::temp_directory_path() / "pom2_softcard_ssc_bterm.dsk";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(disk.image().data()),
                std::streamsize(disk.image().size()));
    }

    Rig rig;
    rig.mem.setCpu(&rig.cpu);
    if (!rig.mem.loadAppleIIRom(rom.c_str())) return 1;
    auto d2 = std::make_unique<DiskIICard>(6);
    if (!d2->loadBootRom(prom) || !d2->insertDisk(tmp.string())) return 1;
    rig.mem.slotBus().plug(6, std::move(d2));
    auto ssc = std::make_unique<SuperSerialCard>(2);
    if (!ssc->loadFirmware(slurp(sscRom))) return 1;
    rig.ssc = ssc.get();
    rig.mem.slotBus().plug(2, std::move(ssc));
    auto sc = std::make_unique<SoftCardZ80>();
    sc->setMemory(&rig.mem);
    sc->setCpu(&rig.cpu);
    rig.card = sc.get();
    rig.mem.slotBus().plug(4, std::move(sc));
    rig.cpu.hardReset();
    rig.mem.slotBus().reset();
    rig.cpu.setProgramCounter(0xC600);

    rig.run(15 * kSecond);
    if (!onScreen(rig.mem, "A>")) {
        std::fprintf(stderr, "FAIL: CP/M did not boot\n");
        dump(rig.mem);
        return 1;
    }

    // 1. READER → SSC: PIP CON:=RDR: echoes serial input until ^Z.
    rig.type("PIP CON:=RDR:\r", 3);
    rig.serial("HELLO\x1A");
    rig.run(3 * kSecond);
    CHECK(onScreen(rig.mem, "HELLO"));
    CHECK(rig.ssc->rxQueueDepth() == 0);
    CHECK(lastRowWith(rig.mem, "A>") > lastRowWith(rig.mem, "HELLO"));

    // 2. BTERM P: CONST under every CON: mapping, while a byte waits on the
    //    SSC. All four read the keyboard, so all stay 00 and slot 2 is idle.
    rig.type("BTERM P\r", 4);
    CHECK(onScreen(rig.mem, "BIOS CONST PER CON: MAPPING"));
    const std::string line = " TTY:00  CRT:00* BAT:00  UC1:00";
    CHECK(onScreen(rig.mem, line));
    rig.mem.slotBus().enableAccessLog(2);
    rig.serial("X");
    rig.run(2 * kSecond);
    const auto slot2 = rig.mem.slotBus().takeAccessLog(2);
    rig.mem.slotBus().disableAccessLog(2);
    CHECK(onScreen(rig.mem, line));
    CHECK(rig.ssc->rxQueueDepth() == 1);
    CHECK(slot2.empty());

    // 3. A key on the Apple ends it.
    rig.type(" ", 2);
    CHECK(lastRowWith(rig.mem, "A>") > lastRowWith(rig.mem, line));

    // 4. CON:=BAT: — the console's input is now the reader, i.e. the SSC
    //    (the X still queued from step 2 is the first byte the CCP reads,
    //    so a backspace takes it off the line before the command). Its
    //    output is LST:, so nothing typed there echoes on the screen: the
    //    proof is that a keyboard STAT afterwards finds CON: back on CRT:.
    rig.type("STAT CON:=BAT:\r", 4);
    rig.serial("\x08" "STAT CON:=CRT:\r");
    rig.run(5 * kSecond);
    CHECK(rig.ssc->rxQueueDepth() == 0);
    rig.type("STAT DEV:\r", 4);
    CHECK(onScreen(rig.mem, "CON: IS CRT:"));
    CHECK(lastRowWith(rig.mem, "CON: IS CRT:") > lastRowWith(rig.mem, "STAT CON:=BAT:"));

    std::error_code ec;
    fs::remove(tmp, ec);
    if (g_fail) {
        dump(rig.mem);
        std::fprintf(stderr, "softcard_ssc_bterm: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("softcard_ssc_bterm: PASS\n");
    return 0;
}
