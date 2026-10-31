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

// saturn_card — the Saturn 128K (SaturnCard.h) in slot 5 of a ][+, driven
// through the real bus (Memory::memRead / memWrite, so the INH hook and the
// ROM fast path it folds into are what is under test). No media: the ROM is
// a $5A pattern laid with loadRomBytes.
//
// Pins: RAM read + write at $C0nB; the motherboard language card untouched
// and answering again when the card lets go; $D000 bank 1 vs 2 with
// $E000-$FFFF shared; the eight blocks isolated; write-only mode (ROM read,
// RAM write); the two-access write-enable, where a write counts (manual p. 70);
// reset → ROM with the 128 KB kept; unplug frees INH; snapshot round trip;
// Saturn's own RAMTEST128K passing on the card (media-gated, SKIP 77).

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "SaturnCard.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <memory>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

constexpr uint16_t kSw = 0xC0D0;   // slot 5 DEVSEL
constexpr uint8_t  kRom = 0x5A;

void touch(Memory& m, uint8_t low4, int times = 1)
{
    for (int i = 0; i < times; ++i) m.memRead(uint16_t(kSw + low4));
}

void testBus()
{
    Memory mem;
    std::vector<uint8_t> rom(0x3000, kRom);
    CHECK(mem.loadRomBytes(rom.data(), rom.size(), 0xD000));
    auto owned = std::make_unique<SaturnCard>();
    owned->setMemory(&mem);
    SaturnCard* card = owned.get();
    mem.slotBus().plug(5, std::move(owned));
    CHECK(mem.lcInhibitor() == card);
    CHECK(mem.memRead(0xD000) == kRom && mem.memRead(0xFFF0) == kRom);

    // $C0nB twice: bank 1, RAM read, writes on.
    touch(mem, 0xB, 2);
    mem.memWrite(0xD000, 0x11);
    mem.memWrite(0xE000, 0x22);
    CHECK(mem.memRead(0xD000) == 0x11 && mem.memRead(0xE000) == 0x22);

    // Let go ($C0nA: ROM, writes off): the motherboard answers again, and
    // its language card never saw the writes.
    touch(mem, 0xA);
    CHECK(mem.memRead(0xD000) == kRom);
    mem.memRead(0xC08B); mem.memRead(0xC08B);
    CHECK(mem.memRead(0xD000) == 0x00 && mem.memRead(0xE000) == 0x00);
    mem.memRead(0xC082);                            // motherboard LC back to ROM

    // Bank 2 vs bank 1; $E000 shared.
    touch(mem, 0x3, 2);
    CHECK(mem.memRead(0xD000) == 0x00 && mem.memRead(0xE000) == 0x22);
    mem.memWrite(0xD000, 0x33);
    touch(mem, 0xB, 2);
    CHECK(mem.memRead(0xD000) == 0x11);

    // Blocks: $C0n5 = block 1, empty; $C0nC = block 4; back to 0.
    touch(mem, 0x5);
    CHECK(card->block() == 1 && mem.memRead(0xE000) == 0x00);
    mem.memWrite(0xE000, 0x99);
    touch(mem, 0xC);
    CHECK(card->block() == 4 && mem.memRead(0xE000) == 0x00);
    touch(mem, 0x4);
    CHECK(card->block() == 0 && mem.memRead(0xE000) == 0x22);
    touch(mem, 0x5);
    CHECK(mem.memRead(0xE000) == 0x99);
    touch(mem, 0x4);

    // Write-only: $C0n9 twice = bank 1, ROM read, RAM write.
    touch(mem, 0x9, 2);
    CHECK(mem.memRead(0xD000) == kRom);
    mem.memWrite(0xD000, 0x77);
    touch(mem, 0x8);                                 // bank 1 RAM read, writes off
    CHECK(mem.memRead(0xD000) == 0x77);
    mem.memWrite(0xD000, 0x00);                      // dropped
    CHECK(mem.memRead(0xD000) == 0x77);

    // One odd access is not enough...
    touch(mem, 0x9);
    mem.memWrite(0xD000, 0x88);
    touch(mem, 0x8);
    CHECK(mem.memRead(0xD000) == 0x77);
    // ...but two are, writes included: "either reading or writing to the
    // location" (Saturn manual p. 70). Apple's card counts reads only.
    mem.memWrite(uint16_t(kSw + 0x9), 0);
    mem.memWrite(uint16_t(kSw + 0x9), 0);
    mem.memWrite(0xD000, 0x88);
    touch(mem, 0x8);
    CHECK(mem.memRead(0xD000) == 0x88);
    touch(mem, 0x9);                                 // and a read then a write
    mem.memWrite(uint16_t(kSw + 0x9), 0);
    mem.memWrite(0xD000, 0x77);
    touch(mem, 0x8);
    CHECK(mem.memRead(0xD000) == 0x77);

    // Snapshot round trip into a second card.
    std::vector<uint8_t> blob;
    card->appendSnapshotState(blob);
    SaturnCard copy;
    copy.loadSnapshotState(blob.data(), blob.size());
    CHECK(copy.inhibitsRead() && !copy.inhibitsWrite() && copy.inhRead(0xD000) == 0x77);

    // Reset: ROM, writes off, block 0, bank 2; the RAM survives.
    touch(mem, 0x5);
    mem.slotBus().reset();
    CHECK(card->block() == 0 && card->bank2());
    CHECK(mem.memRead(0xD000) == kRom);
    touch(mem, 0x3);
    CHECK(mem.memRead(0xD000) == 0x33);

    // Unplug: INH free, the ROM is back whatever the card's last state.
    mem.slotBus().unplug(5);
    CHECK(mem.lcInhibitor() == nullptr);
    CHECK(mem.memRead(0xD000) == kRom);
}

uint16_t textRowBase(int row) { return uint16_t(0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28); }

std::string screen(Memory& mem)
{
    std::string out;
    for (int r = 0; r < 24; ++r) {
        std::string line;
        for (int c = 0; c < 40; ++c) {
            uint8_t ch = uint8_t(mem.peekMainRam(uint16_t(textRowBase(r) + c)) & 0x7F);
            if (ch < 0x20) ch = uint8_t(ch + 0x40);
            line.push_back(ch < 0x7F ? char(ch) : '.');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
    }
    return out;
}

// Saturn's own 1982 software disk on a ][+ with the card in slot 5: its
// RAMTEST128K (Natural Language Systems, rev. Kenneth Roe, April 1982)
// checks ROM read, RAM read/write, write protect, ROM read + RAM write and
// write-through, then a static and a dynamic test of all 8 × 2 banks, and
// asks for the next slot. With Apple's reads-only write-enable rule it stops
// at once with "?NOT RAM" — measured, it is the negative control for the fix
// (Saturn manual p. 70: "either reading or writing"). ~10 emulated
// minutes, ~5 s here.
// 0 = ran, 77 = media missing.
int testRamTest()
{
    namespace fs = std::filesystem;
    const char* disk = "disks_5.4/dsk/saturn128k_software.dsk";
    if (!fs::is_regular_file(disk) || !fs::is_regular_file("roms/apple2p.rom")
        || !fs::is_regular_file("roms/disk2.rom"))
        return 77;
    const fs::path tmp = fs::temp_directory_path() / "pom2_saturn.dsk";
    std::error_code ec;
    fs::copy_file(disk, tmp, fs::copy_options::overwrite_existing, ec);
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    auto d2 = std::make_unique<DiskIICard>(6);
    if (!mem.loadAppleIIRom("roms/apple2p.rom") || !d2->loadBootRom("roms/disk2.rom")
        || !d2->insertDisk(tmp.string())) {
        std::fprintf(stderr, "FAIL: RAMTEST machine setup\n");
        ++g_fail;
        return 0;
    }
    mem.slotBus().plug(6, std::move(d2));
    auto sat = std::make_unique<SaturnCard>();
    sat->setMemory(&mem);
    mem.slotBus().plug(5, std::move(sat));
    cpu.hardReset();
    mem.slotBus().reset();
    constexpr int64_t kSecond = 1'022'727;
    auto run = [&](int64_t cycles) { for (int64_t t = 0; t < cycles;) t += cpu.run(4096); };
    auto until = [&](const char* text, int seconds) {
        for (int i = 0; i < seconds * 2; ++i) {
            if (screen(mem).find(text) != std::string::npos) return true;
            run(kSecond / 2);
        }
        return false;
    };

    bool started = false, finished = false;
    std::set<std::string> seen;   // every line shown, the progress lines aside
    // The HELLO menu takes one key and leaves to BASIC.
    if (until("PSEUDO-DISK", 15)) {
        mem.pasteKeyStream("X", 1);
        run(kSecond);
        mem.pasteText("BRUN RAMTEST128K\r");
        if (until("ENTER SLOT NUMBERS TO TEST", 20)) {
            mem.pasteText("5\r");
            started = true;
            for (int i = 0; i < 1500 && !finished; ++i) {   // ~10 emulated minutes
                run(kSecond);
                const std::string sc = screen(mem);
                for (std::size_t p = 0, n; p < sc.size(); p = n + 1) {
                    n = sc.find('\n', p);
                    const std::string line = sc.substr(p, n - p);
                    if (line.find("FINISHED PAGE") == std::string::npos) seen.insert(line);
                }
                // Back at the slot question, after the last bank.
                finished = sc.find("BANK 8-2") != std::string::npos
                        && sc.find("ENTER SLOT NUMBERS TO TEST") != std::string::npos;
                if (sc.find("NOT RAM") != std::string::npos || sc.find("C)ONTINUE") != std::string::npos)
                    break;
            }
        }
    }
    auto saw = [&](const char* t) {
        for (const auto& l : seen) if (l.find(t) != std::string::npos) return true;
        return false;
    };
    CHECK(started);
    CHECK(finished);
    CHECK(saw("TESTING SLOT 5") && saw("CHECK FOR WRITE-THROUGH") && saw("DYNAMIC RAM TEST"));
    CHECK(!saw("NOT RAM") && !saw("ERROR") && !saw("C)ONTINUE"));
    if (!finished || g_fail || std::getenv("POM2_DUMP")) {
        for (const auto& l : seen) std::fprintf(stderr, "  | %s\n", l.c_str());
        std::fprintf(stderr, "---- screen ----\n%s", screen(mem).c_str());
    }
    fs::remove(tmp, ec);
    return 0;
}

} // namespace

int main()
{
    testBus();
    const int ramTest = testRamTest();
    if (g_fail) {
        std::fprintf(stderr, "saturn_card: %d failure(s)\n", g_fail);
        return 1;
    }
    if (ramTest == 77) {
        std::printf("saturn_card: SKIP RAMTEST128K — needs disks_5.4/dsk/saturn128k_software.dsk, "
                    "roms/apple2p.rom, roms/disk2.rom\n");
        return 77;
    }
    std::printf("saturn_card: PASS\n");
    return 0;
}
