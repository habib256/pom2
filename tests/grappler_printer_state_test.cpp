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

// Grappler+: the printer's status lines, and what the real firmware does
// with them.
//
// What this pins:
//   * the status byte at $C0n0 for every printer state — ready, busy,
//     offline, paper out, no printer — bit for bit (MAME grappler.cpp:699-707,
//     empty port pulled up per ctronics.cpp:56-73);
//   * a byte strobed while the printer cannot take it gets no ACK and is
//     delivered, with its ACK, when the printer is ready again;
//   * end to end, firmware 3.1 on a //e: SELECT low mid-job flashes
//     "NOT SELECTED" on row 10 and waits; SELECT back resumes the job and the
//     screen row is restored. Paper out with SELECT high waits silently in the
//     ACK loop. ROM-gated: SKIPs without roms/apple2e.rom or the Grappler dump.

#include "GrapplerCard.h"
#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const std::string& what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        for (const std::string prefix : { "", "../", "../../" }) {
            if (fs::exists(prefix + p)) return prefix + p;
        }
    }
    return {};
}

std::string spooled(GrapplerCard& card)
{
    std::vector<uint8_t> bytes;
    card.drainSpoolFrom(0, bytes);
    std::string out;
    for (uint8_t b : bytes) out.push_back(static_cast<char>(b & 0x7F));
    return out;
}

void testStatusBits()
{
    // Bits 3-0 of $C0n0: BUSY, PE, SELECT, ACK. DIP bits 6-4 masked out.
    struct Row { const char* what; bool busy, online, paperOut, connected;
                 uint8_t low4; };
    const Row rows[] = {
        { "ready",       false, true,  false, true,  0x03 },
        { "busy",        true,  true,  false, true,  0x0A },
        { "offline",     false, false, false, true,  0x01 },
        { "paper out",   false, true,  true,  true,  0x0F },
        { "paper out + offline", false, false, true, true, 0x0D },
        { "no printer",  false, true,  false, false, 0x0F },
    };
    for (const Row& r : rows) {
        GrapplerCard card(1);
        card.setPrinterBusy(r.busy);
        card.setOnline(r.online);
        card.setPaperOut(r.paperOut);
        card.setPrinterConnected(r.connected);
        const uint8_t st = card.deviceSelectRead(0);
        // A busy printer also reads "not acknowledged" (ackEffective).
        expect((st & 0x0F) == r.low4,
               std::string("status bits, ") + r.what);
    }
}

void testHeldByte()
{
    GrapplerCard card(1);
    card.setOnline(false);
    card.deviceSelectWrite(0, 'X');
    expect(spooled(card).empty(), "offline: the byte is not printed");
    expect((card.deviceSelectRead(0) & 0x01) == 0, "offline: no ACK");
    card.setOnline(true);
    expect(spooled(card) == "X", "online again: the held byte prints");
    expect((card.deviceSelectRead(0) & 0x01) != 0, "online again: ACK");

    GrapplerCard gone(1);
    gone.setPrinterConnected(false);
    gone.deviceSelectWrite(0, 'Y');
    expect(spooled(gone).empty() && (gone.deviceSelectRead(0) & 0x01) == 0,
           "no printer: nothing printed, no ACK");
}

std::string scrapeRow(const uint8_t* ram, int row)
{
    const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
    std::string out;
    for (int col = 0; col < 40; ++col) {
        const uint8_t c = ram[base + col] & 0x3F;          // any char set
        out.push_back(static_cast<char>(c < 0x20 ? c + 0x40 : c));
    }
    return out;
}

struct Rig {
    Memory mem;
    M6502  cpu{&mem};
    GrapplerCard* card = nullptr;

    bool boot(const std::string& rom, const std::string& grappler)
    {
        mem.setCpu(&cpu);
        mem.clearRam();
        mem.resetSoftSwitches();
        mem.setIIEMode(true);
        if (!mem.loadAppleIIRom(rom.c_str(), false)) return false;
        auto g = std::make_unique<GrapplerCard>(1);
        card = g.get();
        if (!card->loadRom(grappler)) return false;
        mem.slotBus().plug(1, std::move(g));
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

void testFirmware()
{
    const std::string rom = firstExisting({ "roms/apple2e.rom" });
    const std::string gp = firstExisting({ "roms/grappler_plus.bin" });
    if (rom.empty() || gp.empty()) {
        std::printf("  SKIP: roms/apple2e.rom or roms/grappler_plus.bin absent\n");
        return;
    }
    {   // Offline mid-job, then back.
        Rig rig;
        expect(rig.boot(rom, gp), "boot //e + Grappler+");
        rig.type("PR#1\r");
        rig.card->setOnline(false);
        rig.type("PRINT \"HELLO\"\r");
        const std::string row10 = scrapeRow(rig.mem.data(), 10);
        expect(row10.find("NOT SELECTED") != std::string::npos,
               "offline: firmware flashes NOT SELECTED on row 10 (got \"" +
               row10 + "\")");
        expect(spooled(*rig.card).find("HELLO") == std::string::npos,
               "offline: HELLO has not printed");
        const uint16_t pc = rig.cpu.getProgramCounter();
        expect(pc >= 0xC800 && pc < 0xD000,
               "offline: the guest waits in the Grappler firmware");
        rig.card->setOnline(true);
        rig.run(3'000'000);
        expect(spooled(*rig.card).find("HELLO") != std::string::npos,
               "online again: the job resumes and HELLO prints");
        expect(scrapeRow(rig.mem.data(), 10).find("NOT SELECTED") ==
                   std::string::npos,
               "online again: the firmware restores row 10");
    }
    {   // Paper out, SELECT high: silent wait in the ACK loop.
        Rig rig;
        expect(rig.boot(rom, gp), "boot //e + Grappler+");
        rig.type("PR#1\r");
        rig.card->setPaperOut(true);
        rig.type("PRINT \"HELLO\"\r");
        expect(spooled(*rig.card).find("HELLO") == std::string::npos,
               "paper out: HELLO has not printed");
        expect(scrapeRow(rig.mem.data(), 10).find("NOT SELECTED") ==
                   std::string::npos,
               "paper out with SELECT high: no warning (PE is never read)");
        rig.card->setPaperOut(false);
        rig.run(3'000'000);
        expect(spooled(*rig.card).find("HELLO") != std::string::npos,
               "paper back: HELLO prints");
    }
}

}  // namespace

int main()
{
    std::printf("Grappler+ printer state test\n");
    testStatusBits();
    testHeldByte();
    testFirmware();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("PASS\n");
    return 0;
}
