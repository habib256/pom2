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
// RAM write); the two-odd-reads write-enable and that writes do not count;
// reset → ROM with the 128 KB kept; unplug frees INH; snapshot round trip.

#include "Memory.h"
#include "SaturnCard.h"

#include <cstdint>
#include <cstdio>
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

    // One odd read is not enough, and writes do not count.
    touch(mem, 0x9);
    mem.memWrite(0xD000, 0x88);
    mem.memWrite(uint16_t(kSw + 0x9), 0);
    mem.memWrite(uint16_t(kSw + 0x9), 0);
    mem.memWrite(0xD000, 0x88);
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

} // namespace

int main()
{
    testBus();
    if (g_fail) {
        std::fprintf(stderr, "saturn_card: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("saturn_card: PASS\n");
    return 0;
}
