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

// dan2_card — the DAN ][ Controller (Dan2Card.h).
//
//  1. The protocol on the bus: the 8255's port C (IBF, the ROM page bit),
//     the $FF sync probe, STATUS on an empty SD slot, the snapshot.
//  2. The firmware, differentially: a //e enhanced boots ProDOS 2.4.3 off
//     SD slot A from slot 7, then a 6502 program calls the card's ProDOS
//     driver ($CnFF) to STATUS, WRITE, READ back and WRITE to a protected
//     image — once on the card's EPROM (roms/dan2_eprom.bin, the LLE
//     reference, when present) and once on POM2's page. Same results.

#include "Dan2Card.h"
#include "M6502.h"
#include "Memory.h"

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

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

constexpr int64_t kSecond = 1'022'727;

std::vector<uint8_t> slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

void send(Dan2Card& c, std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) c.deviceSelectWrite(0, b);
}

void testProtocol()
{
    Dan2Card c(7);
    CHECK(!c.romLayoutError());
    // The ID bytes and the ProDOS tail.
    CHECK(c.slotRomRead(0x01) == 0x20 && c.slotRomRead(0x03) == 0x00
          && c.slotRomRead(0x05) == 0x03 && c.slotRomRead(0x07) == 0x3C);
    CHECK(c.slotRomRead(0xFC) == 0 && c.slotRomRead(0xFD) == 0 && c.slotRomRead(0xFE) == 0xB7);
    CHECK(c.deviceSelectRead(2) == 0x80 && "/OBF high, nothing waiting");
    // The sync probe answers $27.
    send(c, { 0xAC, 0xFF });
    CHECK(c.deviceSelectRead(2) == 0xA0);
    CHECK(c.deviceSelectRead(0) == 0x27);
    // STATUS of an empty SD slot: no device.
    send(c, { 0xAC, 0x00, 0x70, 0x00, 0x08, 0x00, 0x00 });
    CHECK(c.deviceSelectRead(0) == 0x28);
    CHECK(c.deviceSelectRead(2) == 0x80);
    // Port C bit set/reset: bit 0 = the ROM page, and the mode word leaves it.
    c.deviceSelectWrite(3, 0x01);
    CHECK(c.romPage() == 1);
    c.deviceSelectWrite(3, 0xFA);
    CHECK(c.romPage() == 1);
    c.deviceSelectWrite(3, 0x00);
    CHECK(c.romPage() == 0);
    // A reply in flight survives a snapshot round-trip.
    send(c, { 0xAC, 0xFF });
    c.deviceSelectWrite(3, 0x01);
    std::vector<uint8_t> blob;
    c.appendSnapshotState(blob);
    Dan2Card d(7);
    d.loadSnapshotState(blob.data(), blob.size());
    CHECK(d.romPage() == 1);
    CHECK(d.deviceSelectRead(0) == 0x27);
    c.onReset();
    CHECK(c.romPage() == 0 && c.deviceSelectRead(2) == 0x80);
}

struct DriverResults {
    bool booted = false;
    uint8_t statusA = 0xEE, statusP = 0, writeA = 0xEE, writeP = 0, readA = 0xEE, readP = 0;
    uint8_t protA = 0xEE, protP = 0;
    bool readBack = false;
};

// One //e enhanced, the card in slot 7, `image` copied into SD slot A.
DriverResults runMachine(const std::string& rom, const std::string& image, bool eprom)
{
    DriverResults r;
    const fs::path tmp = fs::temp_directory_path() / (eprom ? "pom2_dan2_eprom.po" : "pom2_dan2_pom2.po");
    std::error_code ec;
    fs::copy_file(image, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "FAIL: copy %s\n", image.c_str()); ++g_fail; return r; }
    fs::permissions(tmp, fs::perms::owner_write, fs::perm_options::add, ec);

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    auto card = std::make_unique<Dan2Card>(7);
    if (eprom) CHECK(card->loadEprom(slurp(Dan2Card::kEpromPath)));
    std::string err;
    if (!mem.loadAppleIIRom(rom.c_str()) || !card->mountBay(0, tmp.string(), err)) {
        std::fprintf(stderr, "FAIL: machine setup %s\n", err.c_str());
        ++g_fail;
        return r;
    }
    Dan2Card* dan = card.get();
    mem.slotBus().plug(7, std::move(card));
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](int64_t cycles) { for (int64_t t = 0; t < cycles;) t += cpu.run(4096); };

    // ProDOS up: the MLI entry at $BF00 and slot 7 drive 1 in DEVLST.
    run(8 * kSecond);
    bool inList = false;
    for (int i = 0; i <= mem.memRead(0xBF31) && i < 14; ++i)
        if ((mem.memRead(uint16_t(0xBF32 + i)) & 0xF0) == 0x70) inList = true;
    r.booted = mem.memRead(0xBF00) == 0x4C && inList;
    if (!r.booted) return r;

    // A driver call at $0300: parameters $42-$47, JSR $C7xx, A and P kept.
    const uint8_t entry = dan->slotRomRead(0xFF);
    auto call = [&](uint8_t cmd, uint16_t buf, uint16_t block, uint8_t& a, uint8_t& p) {
        const uint8_t prog[] = {
            0xA9, cmd, 0x85, 0x42, 0xA9, 0x70, 0x85, 0x43,
            0xA9, uint8_t(buf), 0x85, 0x44, 0xA9, uint8_t(buf >> 8), 0x85, 0x45,
            0xA9, uint8_t(block), 0x85, 0x46, 0xA9, uint8_t(block >> 8), 0x85, 0x47,
            0x20, entry, 0xC7,                       // JSR $C7xx
            0x8D, 0xF0, 0x03, 0x08, 0x68, 0x8D, 0xF1, 0x03,
            0x4C, 0x23, 0x03 };                      // JMP * ($0323)
        for (std::size_t i = 0; i < sizeof prog; ++i) mem.memWrite(uint16_t(0x0300 + i), prog[i]);
        // Taken over at a keyboard wait inside the //e's 80-column firmware,
        // which runs with INTCXROM on: give $C700 back to the card first.
        mem.memWrite(0xC006, 0);
        cpu.setProgramCounter(0x0300);
        for (int64_t t = 0; t < kSecond && cpu.getProgramCounter() != 0x0323;) t += cpu.run(64);
        a = mem.memRead(0x03F0);
        p = uint8_t(mem.memRead(0x03F1) & 0x01);     // carry
    };
    const uint16_t block = uint16_t(dan->backing(0).blockCount() - 1);
    call(0, 0x2000, 0, r.statusA, r.statusP);
    for (int i = 0; i < 512; ++i) mem.memWrite(uint16_t(0x2000 + i), uint8_t(i * 7 + 3));
    call(2, 0x2000, block, r.writeA, r.writeP);
    for (int i = 0; i < 512; ++i) mem.memWrite(uint16_t(0x4000 + i), 0);
    call(1, 0x4000, block, r.readA, r.readP);
    std::vector<uint8_t> onCard(512);
    dan->backing(0).readBlock(block, onCard.data());
    r.readBack = true;
    for (int i = 0; i < 512; ++i)
        if (mem.memRead(uint16_t(0x4000 + i)) != uint8_t(i * 7 + 3) || onCard[i] != uint8_t(i * 7 + 3))
            r.readBack = false;
    dan->setBayHostWriteProtected(0, true);
    call(2, 0x2000, block, r.protA, r.protP);
    dan->ejectBay(0);
    fs::remove(tmp, ec);
    return r;
}

void report(const char* what, const DriverResults& r)
{
    std::printf("  %-6s booted=%d status=%02X/%d write=%02X/%d read=%02X/%d back=%d protected=%02X/%d\n",
                what, r.booted, r.statusA, r.statusP, r.writeA, r.writeP, r.readA, r.readP,
                r.readBack, r.protA, r.protP);
}

} // namespace

int main()
{
    if (!fs::is_regular_file("roms/apple2e.rom")) {
        std::printf("dan2_card: SKIP (roms/apple2e.rom not found)\n");
        return 77;
    }
    testProtocol();
    const std::string image = "disks_5.4/dsk/ProDOS_2_4_3.po";
    if (fs::is_regular_file(image)) {
        const DriverResults own = runMachine("roms/apple2e.rom", image, false);
        report("POM2", own);
        CHECK(own.booted);
        CHECK(own.statusA == 0 && own.statusP == 0);
        CHECK(own.writeA == 0 && own.writeP == 0);
        CHECK(own.readA == 0 && own.readP == 0 && own.readBack);
        CHECK(own.protA == 0x2B && own.protP == 1);
        if (fs::is_regular_file(Dan2Card::kEpromPath)) {
            const DriverResults ref = runMachine("roms/apple2e.rom", image, true);
            report("EPROM", ref);
            CHECK(ref.booted);
            CHECK(ref.statusP == own.statusP && ref.writeA == own.writeA && ref.writeP == own.writeP);
            CHECK(ref.readA == own.readA && ref.readP == own.readP && ref.readBack == own.readBack);
            CHECK(ref.protA == own.protA && ref.protP == own.protP);
        } else {
            std::printf("  (no %s: POM2's page only)\n", Dan2Card::kEpromPath);
        }
    } else {
        std::printf("  (no %s: protocol only)\n", image.c_str());
    }
    std::printf("dan2_card: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
