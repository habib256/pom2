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

// als_cpm_card — the ALS CP/M Card (AlsCpmCard.h) booting ALS's own CP/M Plus
// 3.01C (disks_5.4/dsk/als_cpm3_disk1.dsk), then BTERM driving a Super
// Serial Card in slot 2 through nothing but BIOS calls.
//
// Unit part, no media: the bank-0 window (the SoftCard map with Z80 A15
// inverted), the snapshot round trip and the speed (6 MHz on card RAM, a
// synced Apple bus cycle per access to the Apple).
//
// Hard-disk part (testProdosHardDisk): CP/M Plus started from ProDOS on a
// slot-5 hard disk, its RAM disk built by INITRAM in ProDOS's /RAM.
//
// Boot part: II+ Autostart ROM — not a poke at $C600: ALS's console goes
// through the 6502's COUT, which ANDs INVFLG, and only the ROM's reset sets
// it — ALS card in slot 4, the master with BTERM.COM added on a temp copy.
// Pins, in order:
//   1. the card is found (CPM3.SYSTEM/ZBOOT's $08FE handshake) and CP/M
//      Plus prints its banner and runs HELLO to `A>`;
//   2. BTERM P: with a byte waiting on the SSC, the BAT: column — and only
//      it — goes 00 → FF, the byte left in the ACIA. The ALS BIOS's CONST
//      follows the IOBYTE's CON: field (banked BIOS $5BA2, slot table at
//      Apple $08F5-$08F8), which the SoftCard's does not
//      (softcard_ssc_bterm);
//   3. BTERM 2 (serial = BAT:): what the remote sends is on the screen,
//      what is typed goes out through PUNCH, ^A Q quits to `A>`.
// The SSC is cabled null-modem: a terminal session has a peer, and with no
// cable the card reports DCD/DSR off and the 6502's PUNCH waits forever.

#include "AlsCpmCard.h"
#include "CpmDiskFs.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"
#include "SuperSerialCard.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

int lastRowWith(Memory& mem, const std::string& text)
{
    const auto rows = screen(mem);
    for (int r = 23; r >= 0; --r) if (rows[size_t(r)].find(text) != std::string::npos) return r;
    return -1;
}

// The //e's 80 columns: even display columns in AUX, odd in main.
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

void dump(Memory& mem)
{
    for (const auto& r : screen(mem)) std::fprintf(stderr, "|%s|\n", r.c_str());
}

void testWindow()
{
    CHECK(AlsCpmCard::xlateBank0(0x0000) == 0x9000);   // ZBOOT
    CHECK(AlsCpmCard::xlateBank0(0x0100) == 0x9100);   // CPMLDR
    CHECK(AlsCpmCard::xlateBank0(0x2B00) == 0xBB00);   // banked BDOS
    CHECK(AlsCpmCard::xlateBank0(0x3000) == 0xD000);
    CHECK(AlsCpmCard::xlateBank0(0x5000) == 0xF000);   // ROM: ZBOOT's ALS test
    CHECK(AlsCpmCard::xlateBank0(0x5900) == 0xF900);   // banked BIOS
    CHECK(AlsCpmCard::xlateBank0(0x6400) == 0xC400);   // the toggle, slot 4
    CHECK(AlsCpmCard::xlateBank0(0x7300) == 0x0300);   // the call page
    CHECK(AlsCpmCard::xlateBank0(0x78FE) == 0x08FE);   // "card found"
    CHECK(AlsCpmCard::xlateBank0(0x8000) == 0x1000);
    CHECK(AlsCpmCard::xlateBank0(0xDFFF) == 0x6FFF);
}

void testSnapshot()
{
    AlsCpmCard a;
    pom2::Z80::State s = a.z80().getState();
    s.pc = 0x1234;
    s.a = 0x5A;
    a.z80().setState(s);
    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    AlsCpmCard b;
    b.loadSnapshotState(blob.data(), blob.size());
    CHECK(b.z80().getState().pc == 0x1234 && b.z80().getState().a == 0x5A);
    CHECK(blob.size() > 0x10000);
}

// Speed: 6 MHz on the card's RAM; an Apple access waits for a bus-cycle
// edge and takes the whole cycle. In 1200 Apple cycles, NOPs (4 T) from
// common card RAM run 1200 / (4 × 1.0227/6) ≈ 1760 times; from Apple memory
// every fetch is a DMA cycle and the refresh T-state then misses the next
// edge, 2 cycles each: 600.
void testTiming()
{
    auto nopsIn = [](bool fromApple, int budget) {
        Memory mem;
        AlsCpmCard card;
        card.setMemory(&mem);
        card.slotRomWrite(0, 0);            // first grant: Z80 reset, bank 0
        for (int i = 0; i < 0x1000; ++i) {
            mem.memWrite(uint16_t(0x9000 + i), 0x00);   // Z80 $0000: Apple $9000
            card.pokeCardRam(uint16_t(0xE000 + i), 0x00);
        }
        pom2::Z80::State st = card.z80().getState();
        const uint16_t start = fromApple ? 0x0000 : 0xE000;
        st.pc = start;
        card.z80().setState(st);
        card.dmaRun(budget);
        return card.z80().getState().pc - start;
    };
    const int fast = nopsIn(false, 1200);
    const int slow = nopsIn(true, 1200);
    CHECK(fast >= 1755 && fast <= 1765);
    CHECK(slow >= 599 && slow <= 601);
}

struct Rig {
    Memory mem;
    M6502 cpu{&mem};
    AlsCpmCard* card = nullptr;
    SuperSerialCard* ssc = nullptr;

    void run(int64_t cycles)
    {
        for (int64_t total = 0; total < cycles;) {
            const int spent = card->dmaActive() ? card->dmaRun(4096) : cpu.run(4096);
            total += spent > 0 ? spent : 4096;
        }
    }
    void type(const char* s, int64_t seconds) { mem.pasteText(s); run(seconds * kSecond); }
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

// 0 = ran (failures counted in g_fail), 77 = media missing.
int testBoot()
{
    const std::string rom    = findFirst({ "roms/apple2p.rom" });
    const std::string prom   = findFirst({ "roms/disk2.rom" });
    const std::string sscRom = findFirst({ "roms/ssc_341-0065-a.bin" });
    const std::string master = findFirst({ "disks_5.4/dsk/als_cpm3_disk1.dsk" });
    const std::string bterm  = findFirst({ "tests/data/cpm/BTERM.COM" });
    if (rom.empty() || prom.empty() || sscRom.empty() || master.empty() || bterm.empty())
        return 77;

    pom2::cpmfs::CpmDisk disk;
    std::string err;
    if (!disk.open(slurp(master), pom2::cpmfs::SectorOrder::Dos, err)
        || !disk.writeFile(0, "BTERM.COM", slurp(bterm), err)) {
        std::fprintf(stderr, "FAIL: preparing the disk: %s\n", err.c_str());
        ++g_fail;
        return 0;
    }
    const fs::path tmp = fs::temp_directory_path() / "pom2_als_cpm_card.dsk";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(disk.image().data()),
                std::streamsize(disk.image().size()));
    }

    Rig rig;
    rig.mem.setCpu(&rig.cpu);
    auto d2 = std::make_unique<DiskIICard>(6);
    auto ssc = std::make_unique<SuperSerialCard>(2);
    if (!rig.mem.loadAppleIIRom(rom.c_str()) || !d2->loadBootRom(prom)
        || !d2->insertDisk(tmp.string()) || !ssc->loadFirmware(slurp(sscRom))) {
        std::fprintf(stderr, "FAIL: machine setup\n");
        ++g_fail;
        return 0;
    }
    rig.mem.slotBus().plug(6, std::move(d2));
    ssc->setCable(SuperSerialCard::Cable::NullModem);
    rig.ssc = ssc.get();
    rig.mem.slotBus().plug(2, std::move(ssc));
    auto als = std::make_unique<AlsCpmCard>();
    als->setMemory(&rig.mem);
    als->setCpu(&rig.cpu);
    rig.card = als.get();
    rig.mem.slotBus().plug(4, std::move(als));
    rig.cpu.hardReset();
    rig.mem.slotBus().reset();

    // 1. Boot.
    rig.run(20 * kSecond);
    if (!onScreen(rig.mem, "CP/M 3.01C") || !onScreen(rig.mem, "A>")) {
        std::fprintf(stderr, "FAIL: CP/M Plus did not boot\n");
        dump(rig.mem);
        ++g_fail;
        return 0;
    }
    CHECK(onScreen(rig.mem, "ADVANCED LOGIC SYSTEMS"));
    CHECK(rig.mem.memRead(0x08FE) == 0xFF);

    // 2. BTERM P, then a byte from the remote side.
    rig.type("BTERM P\r", 6);
    CHECK(onScreen(rig.mem, " TTY:00* CRT:00  BAT:00  UC1:00"));
    rig.serial("X");
    rig.run(2 * kSecond);
    CHECK(onScreen(rig.mem, " TTY:00* CRT:00  BAT:FF  UC1:00"));
    CHECK(rig.ssc->rxQueueDepth() == 1);
    rig.type(" ", 2);
    CHECK(lastRowWith(rig.mem, "A>") > lastRowWith(rig.mem, "BAT:FF"));

    // 3. The terminal on BAT:.
    rig.type("BTERM 2\r", 6);
    CHECK(onScreen(rig.mem, "SERIAL ON CON:=BAT:"));
    rig.serial("HELLO FROM REMOTE\r\n");
    rig.run(3 * kSecond);
    CHECK(onScreen(rig.mem, "HELLO FROM REMOTE"));
    CHECK(rig.ssc->rxQueueDepth() == 0);
    rig.type("DIR", 3);
    CHECK(rig.ssc->recentTxText().find("DIR") != std::string::npos);
    rig.mem.pasteKeyStream("\x01Q", 2);   // pasteText would drop the ^A
    rig.run(4 * kSecond);
    CHECK(onScreen(rig.mem, "BYE"));
    CHECK(lastRowWith(rig.mem, "A>") > lastRowWith(rig.mem, "BYE"));

    if (g_fail) dump(rig.mem);
    return 0;
}

// ProDOS boots the first *.SYSTEM file of the root; ALS's 800K install keeps
// CPM3.SYSTEM in /CPM, to be run by hand from BASIC.SYSTEM. Give the root an
// entry that shares that file's blocks, so the volume boots straight into it.
bool aliasCpm3SystemInRoot(std::vector<uint8_t>& img)
{
    auto blk = [&](int n) { return img.data() + size_t(n) * 512; };
    auto findEntry = [&](int key, const char* name, uint8_t*& out) {
        for (int b = key; b; b = blk(b)[2] | blk(b)[3] << 8) {
            for (int i = 0; i < 13; ++i) {
                uint8_t* e = blk(b) + 4 + i * 39;
                const int len = e[0] & 15;
                if ((e[0] >> 4) && std::string(reinterpret_cast<char*>(e + 1), size_t(len)) == name) {
                    out = e;
                    return true;
                }
            }
        }
        return false;
    };
    uint8_t* dir = nullptr;
    uint8_t* sys = nullptr;
    if (!findEntry(2, "CPM", dir)) return false;
    if (!findEntry(dir[17] | dir[18] << 8, "CPM3.SYSTEM", sys)) return false;
    for (int i = 1; i < 13; ++i) {                 // root key block, entry 0 = header
        uint8_t* e = blk(2) + 4 + i * 39;
        if (e[0] >> 4) continue;
        std::memcpy(e, sys, 39);
        e[37] = 2;                                  // header pointer: the root
        e[38] = 0;
        uint8_t* hdr = blk(2) + 4;
        const int count = (hdr[0x21] | hdr[0x22] << 8) + 1;
        hdr[0x21] = uint8_t(count);
        hdr[0x22] = uint8_t(count >> 8);
        return true;
    }
    return false;
}

// A temp copy of `src` in DOS order (a .cpm image is converted), for the
// Disk II, which writes back to the file it was given.
std::string tempDosCopy(const std::string& src, const char* name)
{
    pom2::cpmfs::CpmDisk d;
    std::string err;
    if (src.empty() || !d.open(slurp(src), pom2::cpmfs::orderForPath(src), err)) return {};
    const auto dos = d.imageInOrder(pom2::cpmfs::SectorOrder::Dos);
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(dos.data()), std::streamsize(dos.size()));
    return o ? p.string() : std::string();
}

// CP/M Plus 3.01C started from ProDOS on a slot-5 hard disk (ALS's 800K
// install, disks_3.5/als_cpm3_prodos_800k.po), on a 128 KB //e:
//   - A: is /CPM/SYSTEM, read by the BIOS through ProDOS MLI calls — HELLO
//     runs from it and lists the volume;
//   - D: is the RAM disk, absent until 3.01C's INITRAM builds /RAM/SYSTEM in
//     ProDOS's aux-memory /RAM, then a PIP onto it is listed back.
// INITRAM comes from the 3.01C ProDOS utilities disk (B:, CP/M-order .cpm),
// PIP from Disk 1 (C:): the 800K volume's own copies of DIR, PIP, INITRAM…
// were added by an archiving tool with 8-bit block numbers on a disk whose
// format has 16-bit ones, and load as garbage — on the real card too.
int testProdosHardDisk()
{
    const std::string rom    = findFirst({ "roms/apple2e_unenh.rom" });
    const std::string prom   = findFirst({ "roms/disk2.rom" });
    const std::string volume = findFirst({ "disks_3.5/als_cpm3_prodos_800k.po" });
    const std::string util   = tempDosCopy(
        findFirst({ "disks_5.4/dsk/als_cpm3_prodos_disk_301c.cpm" }), "pom2_als_util.dsk");
    const std::string disk1  = tempDosCopy(
        findFirst({ "disks_5.4/dsk/als_cpm3_disk1.dsk" }), "pom2_als_disk1.dsk");
    if (rom.empty() || prom.empty() || volume.empty() || util.empty() || disk1.empty())
        return 77;
    auto img = slurp(volume);
    if (!aliasCpm3SystemInRoot(img)) {
        std::fprintf(stderr, "FAIL: no /CPM/CPM3.SYSTEM on the volume\n");
        ++g_fail;
        return 0;
    }
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    auto d2 = std::make_unique<DiskIICard>(6);
    auto hdv = std::make_unique<ProDOSHardDiskCard>(5);
    if (!mem.loadAppleIIRom(rom.c_str()) || !d2->loadBootRom(prom)
        || !d2->insertDisk(0, util) || !d2->insertDisk(1, disk1)
        || !hdv->loadImageFromBytes(std::move(img), "als_cpm3_prodos_800k.po")) {
        std::fprintf(stderr, "FAIL: hard-disk machine setup\n");
        ++g_fail;
        return 0;
    }
    mem.slotBus().plug(6, std::move(d2));
    mem.slotBus().plug(5, std::move(hdv));
    auto als = std::make_unique<AlsCpmCard>();
    als->setMemory(&mem);
    als->setCpu(&cpu);
    AlsCpmCard* card = als.get();
    mem.slotBus().plug(4, std::move(als));
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) {
        for (int64_t total = 0; total < cycles;) {
            const int spent = card->dmaActive() ? card->dmaRun(4096) : cpu.run(4096);
            total += spent > 0 ? spent : 4096;
        }
    };
    auto type = [&](const char* k, int64_t seconds) {
        mem.pasteText(k);
        run(seconds * kSecond);
    };
    // The Autostart ROM scans down from slot 7 and would boot the floppy in
    // slot 6 first: let it initialise the machine, then enter slot 5.
    for (int64_t t = 0; t < 5 * kSecond; t += cpu.run(64))
        if ((cpu.getProgramCounter() & 0xFF00) == 0xC600) break;
    cpu.setProgramCounter(0xC500);
    run(20 * kSecond);
    const bool booted = onScreen80(mem, "CP/M PLUS 3.01C")
                     && onScreen80(mem, "A: ADUMP    CONVERT  DIR");
    CHECK(booted);
    if (!booted) { dump80(mem); return 0; }

    type("4\r", 6);                       // HELLO's menu: exit to CP/M
    type("DIR D:\r", 6);
    CHECK(onScreen80(mem, "CP/M Error On D: Invalid Drive"));
    type("B:INITRAM\r", 10);
    CHECK(onScreen80(mem, "RAM DISK INITIALIZED AND ASSIGNED"));
    type("C:PIP D:=B:QUIT.COM\r", 10);
    type("DIR D:\r", 6);
    CHECK(onScreen80(mem, "D: QUIT     COM"));
    if (g_fail) dump80(mem);
    return 0;
}

} // namespace

int main()
{
    testWindow();
    testSnapshot();
    testTiming();
    const int hard = testProdosHardDisk();
    const int boot = testBoot();
    // After the machines are gone: the Disk II commits its image on destruction.
    std::error_code ec;
    for (const char* t : { "pom2_als_cpm_card.dsk", "pom2_als_util.dsk", "pom2_als_disk1.dsk" })
        fs::remove(fs::temp_directory_path() / t, ec);
    if (g_fail) {
        std::fprintf(stderr, "als_cpm_card: %d failure(s)\n", g_fail);
        return 1;
    }
    if (boot == 77 || hard == 77) {
        std::printf("als_cpm_card: SKIP part (floppy boot %s, hard disk %s) — needs "
                    "roms/apple2p.rom, roms/apple2e_unenh.rom, roms/disk2.rom, "
                    "roms/ssc_341-0065-a.bin, disks_5.4/dsk/als_cpm3_*, "
                    "disks_3.5/als_cpm3_prodos_800k.po, tests/data/cpm/BTERM.COM\n",
                    boot == 77 ? "skipped" : "ran", hard == 77 ? "skipped" : "ran");
        return 77;
    }
    std::printf("als_cpm_card: PASS\n");
    return 0;
}
