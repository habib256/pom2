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

// The Apple Parallel Interface Card (`pic`) and the original Grappler
// (`grappler1`), with their real ROMs:
//
//   * the identification bytes a detection routine reads, per MAME's address
//     mapping (docs/printer-detection.md § signatures): the PIC has no Pascal
//     signature at all; the 1981 Grappler has one only in slot 1;
//   * the status bits for ready / offline / paper out / no printer;
//   * end to end on a //e: PR#1 + PRINT reaches the printer through each
//     card's own firmware, an offline printer holds the job, and it resumes.
// ROM-gated: SKIPs when roms/apple2e.rom or a card ROM is absent.

#include "AppleParallelCard.h"
#include "GrapplerClassicCard.h"
#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const std::string& what)
{
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

std::string firstExisting(const std::string& p)
{
    for (const std::string prefix : { "", "../", "../../" })
        if (std::filesystem::exists(prefix + p)) return prefix + p;
    return {};
}

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

std::string spooled(pom2::CentronicsPrinter& p)
{
    std::vector<uint8_t> b;
    p.drainSpoolFrom(0, b);
    std::string s;
    for (uint8_t c : b) s.push_back(static_cast<char>(c & 0x7F));
    return s;
}

void testSignatures(const std::vector<uint8_t>& pic, const std::vector<uint8_t>& gp1)
{
    AppleParallelCard card(1);
    expect(card.loadProm(pic), "PIC PROM loads");
    card.onReset();
    // No Pascal 1.1 signature: $Cn05/$Cn07 = $48 $48, $Cn0B = $58.
    expect(card.slotRomRead(0x00) == 0x18 && card.slotRomRead(0x01) == 0xB0 &&
           card.slotRomRead(0x05) == 0x48 && card.slotRomRead(0x07) == 0x48 &&
           card.slotRomRead(0x0B) == 0x58,
           "PIC: $Cn00-01 = 18 B0, $Cn05/07/0B = 48 48 58");

    GrapplerClassicCard one(1), two(2);
    expect(one.loadRom(gp1) && two.loadRom(gp1), "Grappler ROM loads");
    expect(one.slotRomRead(0x05) == 0x38 && one.slotRomRead(0x07) == 0x18 &&
           one.slotRomRead(0x0B) == 0x01 && one.slotRomRead(0x0C) == 0x14,
           "Grappler in slot 1: Pascal 1.1, class $14 (printer)");
    expect(two.slotRomRead(0x05) == 0x04 && two.slotRomRead(0x07) == 0x48,
           "Grappler in slot 2: its own page, no Pascal signature");
    expect(one.slotRomRead(0x00) == 0x18 && two.slotRomRead(0x00) == 0x18,
           "every Grappler page starts 18");
}

void testStatus()
{
    AppleParallelCard pic(1);
    // $C0n3: $97 | PE<<5 | SELECT<<6 | /FAULT<<3.
    expect(pic.deviceSelectRead(3) == 0xDF, "PIC ready: $DF");
    pic.printer().setOnline(false);
    expect(pic.deviceSelectRead(3) == 0x97, "PIC offline: SELECT low, /FAULT low");
    pic.printer().setOnline(true);
    pic.printer().setPaperOut(true);
    expect(pic.deviceSelectRead(3) == 0xF7, "PIC paper out: PE high, /FAULT low");
    pic.printer().setPaperOut(false);
    pic.printer().setConnected(false);
    expect(pic.deviceSelectRead(3) == 0xFF, "PIC no printer: all pulled up");

    GrapplerClassicCard gp(1);
    expect((gp.deviceSelectRead(1) & 0x0F) == 0x03,
           "Grappler ready: SELECT, latch set after reset");
    gp.printer().setOnline(false);
    expect((gp.deviceSelectRead(1) & 0x0E) == 0x08, "Grappler offline: BUSY, no SELECT");
}

struct Rig {
    Memory mem;
    M6502  cpu{&mem};
    pom2::CentronicsPrinter* printer = nullptr;

    bool boot(const std::string& rom, std::unique_ptr<SlotPeripheral> card)
    {
        mem.setCpu(&cpu);
        mem.clearRam();
        mem.resetSoftSwitches();
        mem.setIIEMode(true);
        if (!mem.loadAppleIIRom(rom.c_str(), false)) return false;
        printer = card->centronicsPrinter();
        mem.slotBus().plug(1, std::move(card));
        mem.slotBus().reset();
        cpu.setCpuMode(M6502::CpuMode::CMOS);
        cpu.hardReset();
        run(3'000'000);
        return true;
    }
    void run(int n) { for (int i = 0; i < n; ++i) cpu.step(); }
    void type(const char* s)
    {
        for (const char* p = s; *p; ++p) { mem.pasteKeyStream(p, 1); run(100'000); }
        run(1'000'000);
    }
};

std::string screenRow(const uint8_t* ram, int row)
{
    const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
    std::string line;
    for (int c = 0; c < 40; ++c) {
        const uint8_t ch = ram[base + c] & 0x3F;
        line.push_back(static_cast<char>(ch < 0x20 ? ch + 0x40 : ch));
    }
    return line;
}

// The PIC's firmware waits on its ACK latch: an offline printer holds the
// byte, and the job resumes by itself when the printer comes back.
void testPicFirmware(const std::string& rom, std::unique_ptr<SlotPeripheral> card)
{
    Rig rig;
    expect(rig.boot(rom, std::move(card)), "PIC: boot");
    rig.type("PR#1\r");
    rig.type("PRINT \"HELLO\"\r");
    expect(spooled(*rig.printer).find("HELLO") != std::string::npos,
           "PIC: PR#1 + PRINT prints through the card's firmware");
    rig.printer->setOnline(false);
    rig.type("PRINT \"WORLD\"\r");
    expect(spooled(*rig.printer).find("WORLD") == std::string::npos,
           "PIC: offline holds the job");
    rig.printer->setOnline(true);
    rig.run(3'000'000);
    expect(spooled(*rig.printer).find("WORLD") != std::string::npos,
           "PIC: back online, the job resumes by itself");
}

// The 1981 Grappler checks SELECT BEFORE strobing ($CBD2): low, it writes
// PRINTER NOT READY / PRESS <CR> TO CONTINUE on rows 10-11, beeps three times
// and waits for RETURN ($CC3E-$CC4A), then checks again. It never reads its
// ACK latch — it waits for BUSY to drop ($CBD9).
void testGrappler1981Firmware(const std::string& rom,
                              std::unique_ptr<SlotPeripheral> card)
{
    Rig rig;
    expect(rig.boot(rom, std::move(card)), "Grappler 1981: boot");
    rig.type("PR#1\r");
    rig.type("PRINT \"HELLO\"\r");
    expect(spooled(*rig.printer).find("HELLO") != std::string::npos,
           "Grappler 1981: PR#1 + PRINT prints through the card's firmware");
    const std::size_t before = spooled(*rig.printer).size();
    rig.printer->setOnline(false);
    rig.type("P");
    expect(screenRow(rig.mem.data(), 10).find("PRINTER NOT READY") != std::string::npos,
           "Grappler 1981: offline shows PRINTER NOT READY (row 10: \"" +
           screenRow(rig.mem.data(), 10) + "\")");
    expect(screenRow(rig.mem.data(), 11).find("PRESS <CR> TO CONTINUE") != std::string::npos,
           "Grappler 1981: and asks for RETURN on row 11");
    expect(spooled(*rig.printer).size() == before,
           "Grappler 1981: nothing is strobed while SELECT is low");
    rig.type("\r");                     // RETURN, printer still off line
    expect(spooled(*rig.printer).size() == before,
           "Grappler 1981: RETURN while still offline just asks again");
    rig.printer->setOnline(true);
    rig.run(1'000'000);
    expect(spooled(*rig.printer).size() == before,
           "Grappler 1981: SELECT back alone does not resume — it waits for RETURN");
    rig.type("\r");
    expect(spooled(*rig.printer).size() > before,
           "Grappler 1981: RETURN with the printer on line resumes");
}

}  // namespace

int main()
{
    std::printf("Parallel printer cards test\n");
    const std::string picRom = firstExisting("roms/341-0057.bin");
    const std::string gpRom = firstExisting("roms/grappler_eps-1.bin");
    const std::string iie = firstExisting("roms/apple2e.rom");
    testStatus();
    if (picRom.empty() || gpRom.empty() || iie.empty()) {
        std::printf("  SKIP: card ROMs or roms/apple2e.rom absent\n");
    } else {
        const auto pic = readFile(picRom), gp1 = readFile(gpRom);
        testSignatures(pic, gp1);
        auto a = std::make_unique<AppleParallelCard>(1);
        a->loadProm(pic);
        testPicFirmware(iie, std::move(a));
        auto g = std::make_unique<GrapplerClassicCard>(1);
        g->loadRom(gp1);
        testGrappler1981Firmware(iie, std::move(g));
    }
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("PASS\n");
    return 0;
}
