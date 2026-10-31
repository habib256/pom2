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

// softcard2_card — the Microsoft SoftCard II (SoftCardIICard.h).
//
// Unit part, no media: the Am2950 latches and their two status bytes, boot
// mode (the 6502 feeds the Z80 its opcodes, the Z80 is held while the latch
// is empty and resumes without losing or doubling a byte, the first OUT ends
// it), $C0n3 reset, the 6 MHz pace and the snapshot round trip.
//
// Boot part: Microsoft's CP/M 2.28B for the card
// (disks_5.4/dsk/softcard2_cpm228b.dsk, the image izapple2 boots) on an
// enhanced //e with the card in slot 4: the banner and `A>` on the 80-column
// screen, then DIR lists the disk.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "Ccs7710Card.h"
#include "OsCallTrace.h"
#include "SoftCardIICard.h"
#include "SuperSerialCard.h"
#include "VidexVideotermCard.h"

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

// The 6502's view: DEVSEL offsets.
uint8_t rd(SoftCardIICard& c, uint8_t o) { return c.deviceSelectRead(o); }
void    wr(SoftCardIICard& c, uint8_t o, uint8_t v) { c.deviceSelectWrite(o, v); }

// Boot mode: the 6502 sends `LD A,$5A / LD ($1000),A / OUT ($00),A`. Each
// byte is fed only once the previous one is taken; the store below $8000
// comes back on the latch and does NOT reach RAM (A15 low is the mailbox);
// the OUT ends boot mode, after
// which the Z80 runs from its own RAM (zero from $0007: NOPs).
void testBootMode()
{
    SoftCardIICard c;
    CHECK(c.booting());
    CHECK(rd(c, 1) == 0x00);
    c.advanceCycles(50);                         // nothing to fetch: held at $0000
    CHECK(c.z80().getState().pc == 0x0000);

    const uint8_t prog[] = { 0x3E, 0x5A, 0x32, 0x00, 0x10, 0xD3, 0x00 };
    for (uint8_t b : prog) {
        wr(c, 0, b);
        CHECK(rd(c, 1) & 0x01);                  // not taken yet
        c.advanceCycles(20);
        CHECK(!(rd(c, 1) & 0x01));               // taken, exactly once
    }
    CHECK(c.z80().getState().a == 0x5A);
    CHECK(c.cardRam(0x1000) == 0x00);
    CHECK(!c.booting());
    CHECK(rd(c, 1) == 0x80);                      // the store came back
    CHECK(rd(c, 0) == 0x5A);
    CHECK(rd(c, 1) == 0x00);                      // and the latch is empty

    // Out of boot mode: OUT delivers, IN takes, status mirrors.
    SoftCardIICard d;
    wr(d, 0, 0xD3); d.advanceCycles(20);          // OUT ($00),A: leaves boot mode
    wr(d, 0, 0x00); d.advanceCycles(20);
    CHECK(!d.booting());
    CHECK(rd(d, 1) == 0x00);
    wr(d, 3, 0);                                   // $C0n3: back to boot mode
    CHECK(d.booting() && d.z80().getState().pc == 0x0000);
}

// The Z80 runs at 6 MHz whatever the 6502's clock: a NOP is 4 T, so in
// 1023 CPU cycles at 1.0227 MHz it runs 1500 of them, and at 4 MHz the same
// 1023 cycles are a quarter of the time.
void testPace()
{
    auto nops = [](double hz) {
        SoftCardIICard c;
        c.setCpuClock(hz);
        wr(c, 0, 0xD3); c.advanceCycles(20);
        wr(c, 0, 0x00); c.advanceCycles(20);       // RAM is zero: NOPs from $0002
        const uint16_t pc0 = c.z80().getState().pc;
        c.advanceCycles(1023);
        return int(c.z80().getState().pc - pc0);
    };
    const int stock = nops(1'022'727.0);
    const int fast  = nops(4 * 1'022'727.0);
    CHECK(stock >= 1499 && stock <= 1502);
    CHECK(fast >= 374 && fast <= 377);
}

void testSnapshot()
{
    SoftCardIICard a;
    wr(a, 0, 0x3E); a.advanceCycles(20);           // LD A,n: the opcode taken,
    a.advanceCycles(20);                            // held waiting for n
    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    SoftCardIICard b;
    b.loadSnapshotState(blob.data(), blob.size());
    wr(b, 0, 0x77); b.advanceCycles(20);
    CHECK(b.z80().getState().a == 0x77);            // resumed mid-instruction
    CHECK(b.booting());
    CHECK(blob.size() > 0x10000);
}

uint16_t textRowBase(int row) { return uint16_t(0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28); }

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
    const std::string master = findFirst({ "disks_5.4/dsk/softcard2_cpm228b.dsk" });
    if (rom.empty() || prom.empty() || master.empty())
        return 77;
    // A temp copy: the Disk II writes back to the file it was given.
    const fs::path tmp = fs::temp_directory_path() / "pom2_softcard2.dsk";
    std::error_code ec;
    fs::copy_file(master, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "FAIL: copy %s\n", master.c_str()); ++g_fail; return 0; }

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    auto d2 = std::make_unique<DiskIICard>(6);
    if (!mem.loadAppleIIRom(rom.c_str()) || !d2->loadBootRom(prom)
        || !d2->insertDisk(tmp.string())) {
        std::fprintf(stderr, "FAIL: machine setup\n");
        ++g_fail;
        return 0;
    }
    mem.slotBus().plug(6, std::move(d2));
    mem.slotBus().plug(4, std::make_unique<SoftCardIICard>());
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) {
        for (int64_t t = 0; t < cycles;) t += cpu.run(4096);
    };

    run(15 * kSecond);
    const bool booted = onScreen80(mem, "Softcard II CP/M") && onScreen80(mem, "A>");
    CHECK(booted);
    if (!booted) { dump80(mem); return 0; }
    mem.pasteText("DIR\r");
    run(5 * kSecond);
    CHECK(onScreen80(mem, "A: PIP      COM"));
    if (g_fail || std::getenv("POM2_DUMP")) dump80(mem);
    return 0;
}

std::vector<uint8_t> slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

// The Videoterm's 2 KB of VRAM as text. The screen starts wherever the
// firmware has scrolled it to, so a line can wrap the end of the buffer:
// searched twice over.
std::string videotermText(const VidexVideotermCard& v)
{
    std::string s;
    for (int i = 0; i < 2 * int(VidexVideotermCard::kVramBytes); ++i) {
        const char c = char(v.vram()[i % VidexVideotermCard::kVramBytes] & 0x7F);
        s.push_back(c >= 0x20 && c < 0x7F ? c : ' ');
    }
    return s;
}

// CP/M 2.28B on a ][+ whose console is a Videx Videoterm in slot 3. The
// server's built-in console assumes a //e's aux 80 columns; on a ][+ it
// loses every other column, and the Videoterm is the 80-column card the
// packaging asked for there: the loader types it as an Apple High-Speed
// Serial-class card ($Cn05/$Cn07 = $38/$18) and drives it through its $C800
// entry points. Found by Simon Babcock's SoftCard2 (HARDWARE-REFERENCE.md,
// "Built-in console").
int testBootIIPlusVideoterm()
{
    const std::string rom    = findFirst({ "roms/apple2p.rom" });
    const std::string prom   = findFirst({ "roms/disk2.rom" });
    const std::string master = findFirst({ "disks_5.4/dsk/softcard2_cpm228b.dsk" });
    const std::string fw     = findFirst({ "roms/videx_videoterm_v24_60hz.bin" });
    const std::string chars  = findFirst({ "roms/videx_videoterm_char_normal.bin" });
    if (rom.empty() || prom.empty() || master.empty() || fw.empty() || chars.empty())
        return 77;
    const fs::path tmp = fs::temp_directory_path() / "pom2_softcard2.dsk";
    std::error_code ec;
    fs::copy_file(master, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "FAIL: copy %s\n", master.c_str()); ++g_fail; return 0; }

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    auto d2 = std::make_unique<DiskIICard>(6);
    auto vt = std::make_unique<VidexVideotermCard>(3);
    if (!mem.loadAppleIIRom(rom.c_str()) || !d2->loadBootRom(prom)
        || !d2->insertDisk(tmp.string()) || !vt->loadFirmware(slurp(fw))
        || !vt->loadCharRoms(slurp(chars), {})) {
        std::fprintf(stderr, "FAIL: ][+ machine setup\n");
        ++g_fail;
        return 0;
    }
    const VidexVideotermCard* videx = vt.get();
    mem.slotBus().plug(6, std::move(d2));
    mem.slotBus().plug(3, std::move(vt));
    mem.slotBus().plug(4, std::make_unique<SoftCardIICard>());
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) {
        for (int64_t t = 0; t < cycles;) t += cpu.run(4096);
    };
    run(15 * kSecond);
    const std::string boot = videotermText(*videx);
    const bool booted = boot.find("Softcard II CP/M") != std::string::npos
                     && boot.find("A>") != std::string::npos;
    CHECK(booted);
    mem.pasteText("DIR\r");
    run(5 * kSecond);
    const std::string dir = videotermText(*videx);
    CHECK(dir.find("A: PIP      COM") != std::string::npos);
    if (!booted || g_fail || std::getenv("POM2_DUMP")) {
        const std::string t = dir.substr(0, VidexVideotermCard::kVramBytes);
        for (std::size_t i = 0; i < t.size(); i += 80)
            std::fprintf(stderr, "  |%s\n", t.substr(i, 80).c_str());
    }
    return 0;
}

// The loader's slot scan, the 6502 BIOS calls on the mailbox, and the
// printer. A //e with a CCS 7710 (6850, type 3) in slot 1, a Super Serial
// (Pascal 1.1 firmware, type 6) in 2, the SoftCard II in 4, the Disk II in
// 6. sbabcoc's SoftCard2 HARDWARE-REFERENCE.md: the slot types land at Z80
// $F3B9-$F3BF (slot 3 empty = the built-in console, type 1); the 6502 BIOS
// extension words at $F38E-$F395 read $0D54/$0D44/$1600/$DB00 in the
// running system; slot 1's type 3 card is CP/M's LST:, which ^P echoes the
// console to.
int testSlotsCallsAndPrinter()
{
    const std::string rom    = findFirst({ "roms/apple2e.rom" });
    const std::string prom   = findFirst({ "roms/disk2.rom" });
    const std::string master = findFirst({ "disks_5.4/dsk/softcard2_cpm228b.dsk" });
    const std::string sscRom = findFirst({ "roms/ssc_341-0065-a.bin" });
    if (rom.empty() || prom.empty() || master.empty() || sscRom.empty())
        return 77;
    const fs::path tmp = fs::temp_directory_path() / "pom2_softcard2.dsk";
    std::error_code ec;
    fs::copy_file(master, tmp, fs::copy_options::overwrite_existing, ec);

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    auto d2 = std::make_unique<DiskIICard>(6);
    auto ssc = std::make_unique<SuperSerialCard>(2);
    if (!mem.loadAppleIIRom(rom.c_str()) || !d2->loadBootRom(prom)
        || !d2->insertDisk(tmp.string()) || !ssc->loadFirmware(slurp(sscRom))) {
        std::fprintf(stderr, "FAIL: slots machine setup\n");
        ++g_fail;
        return 0;
    }
    auto ccs = std::make_unique<Ccs7710Card>(1);
    Ccs7710Card* printer = ccs.get();
    auto sc2 = std::make_unique<SoftCardIICard>();
    SoftCardIICard* card = sc2.get();
    mem.slotBus().plug(1, std::move(ccs));
    mem.slotBus().plug(2, std::move(ssc));
    mem.slotBus().plug(4, std::move(sc2));
    mem.slotBus().plug(6, std::move(d2));
    pom2::OsCallTrace trace;
    trace.setBdos(true);
    mem.slotBus().setZ80Observer(&trace);
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) { for (int64_t t = 0; t < cycles;) t += cpu.run(4096); };
    run(15 * kSecond);
    CHECK(onScreen80(mem, "A>"));

    // 2. The slot types, and 3. the BIOS extension words.
    const uint8_t types[7] = { card->cardRam(0xF3B9), card->cardRam(0xF3BA), card->cardRam(0xF3BB),
                               card->cardRam(0xF3BC), card->cardRam(0xF3BD), card->cardRam(0xF3BE),
                               card->cardRam(0xF3BF) };
    std::fprintf(stderr, "  slot types 1-7: %d %d %d %d %d %d %d\n", types[0], types[1], types[2],
                 types[3], types[4], types[5], types[6]);
    CHECK(types[0] == 3 && "slot 1: CCS 7710 = 6850 serial (type 3)");
    CHECK(types[1] == 6 && "slot 2: Super Serial = Pascal firmware (type 6)");
    CHECK(types[5] == 2 && "slot 6: Disk II (type 2)");
    auto word = [&](uint16_t a) { return card->cardRam(a) | card->cardRam(uint16_t(a + 1)) << 8; };
    CHECK(word(0xF38E) == 0x0D54 && word(0xF390) == 0x0D44);
    CHECK(word(0xF392) == 0x1600 && word(0xF394) == 0xDB00);

    // 1. The calls the boot made, decoded.
    auto saw = [&](const char* t) {
        for (const auto& l : trace.lines()) if (l.find(t) != std::string::npos) return true;
        return false;
    };
    CHECK(saw("SC2  READSEC S6,D1") && "disk reads through the 6502");
    CHECK(saw("SC2  WRITESLOT S3") && "console output through the 6502");

    // The printer: ^P echoes the console to LST: (slot 1, the 6850).
    mem.pasteKeyStream("\x10", 1);
    run(kSecond);
    mem.pasteText("DIR\r");
    run(5 * kSecond);
    mem.pasteKeyStream("\x10", 1);
    run(3 * kSecond);
    std::vector<uint8_t> out;
    printer->drainSpoolFrom(0, out);
    const std::string printed(out.begin(), out.end());
    CHECK(printed.find("PIP") != std::string::npos && "DIR reached the 6850");
    if (g_fail || std::getenv("POM2_DUMP")) {
        std::fprintf(stderr, "  printed %zu bytes: %.200s\n", out.size(), printed.c_str());
        int n = 0;
        for (const auto& l : trace.lines())
            if (l.rfind("SC2", 0) == 0 && n++ < 40) std::fprintf(stderr, "  | %s\n", l.c_str());
    }
    mem.slotBus().setZ80Observer(nullptr);
    return 0;
}

} // namespace

int main()
{
    testBootMode();
    testPace();
    testSnapshot();
    const int boot = testBoot();
    const int bootIIPlus = testBootIIPlusVideoterm();
    testSlotsCallsAndPrinter();
    std::error_code ec;
    fs::remove(fs::temp_directory_path() / "pom2_softcard2.dsk", ec);
    if (g_fail) {
        std::fprintf(stderr, "softcard2_card: %d failure(s)\n", g_fail);
        return 1;
    }
    if (bootIIPlus == 77)
        std::printf("softcard2_card: ][+ + Videoterm boot skipped — needs roms/apple2p.rom "
                    "and the Videoterm ROMs\n");
    if (boot == 77) {
        std::printf("softcard2_card: SKIP boot part — needs roms/apple2e.rom, "
                    "roms/disk2.rom, disks_5.4/dsk/softcard2_cpm228b.dsk\n");
        return 77;
    }
    std::printf("softcard2_card: PASS\n");
    return 0;
}
