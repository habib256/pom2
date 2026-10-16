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

// slinky_card — the Apple II Memory Expansion Card (SlinkyCard.h).
//
// Part 1, registers (no ROM needed): MAME a2memexp.cpp:166-232 — the 24-bit
// latch, the forced $F0 high nibble on $C0n2, the data port's
// auto-increment carried back into $C0n0-$C0n2, the latch keeping the START
// of a transfer, the 1 MB wrap, and device_reset clearing the latches but
// not the RAM.
//
// Part 2, the REAL 341-0344A firmware on a 6502: STATUS / WRITE / READ
// through the ProDOS driver entry ($CnFF), exactly as ProDOS 8 calls it, and
// the written block must be found in the card's own RAM — proof that the
// transfer went through the $C0nX registers and not around them. Skips (77)
// when roms/slinky_341-0344a.bin is absent.

#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SlinkyCard.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

using pom2::SlinkyCard;

namespace {

constexpr int     kSlot  = 5;
constexpr uint8_t kRomHi = 0xC0 + kSlot;
int failures = 0;

void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

void registers()
{
    SlinkyCard c(kSlot);
    // Fresh RAM reads $FF (device_start, a2memexp.cpp:147).
    c.deviceSelectWrite(0, 0x00); c.deviceSelectWrite(1, 0x00); c.deviceSelectWrite(2, 0x00);
    expect(c.deviceSelectRead(3) == 0xFF, "fresh RAM reads $FF");

    c.deviceSelectWrite(0, 0xFE);
    c.deviceSelectWrite(1, 0xFF);
    c.deviceSelectWrite(2, 0x01);
    expect(c.deviceSelectRead(0) == 0xFE, "$C0n0 reads the latch");
    expect(c.deviceSelectRead(1) == 0xFF, "$C0n1 reads the latch");
    expect(c.deviceSelectRead(2) == 0xF1, "$C0n2 reads with the high nibble forced");

    c.deviceSelectWrite(3, 0x11);   // $01FFFE
    c.deviceSelectWrite(3, 0x22);   // $01FFFF
    c.deviceSelectWrite(3, 0x33);   // $020000 — carry ripples into bits 16-23
    expect(c.deviceSelectRead(0) == 0x01 && c.deviceSelectRead(1) == 0x00 &&
           c.deviceSelectRead(2) == 0xF2, "auto-increment carries through all three bytes");
    expect(c.ramAt(0x01FFFE) == 0x11 && c.ramAt(0x01FFFF) == 0x22 &&
           c.ramAt(0x020000) == 0x33, "data port wrote consecutive bytes");

    // Re-writing ONE address byte reloads the live pointer from the latch,
    // which still holds the transfer's start ($01FFFE) in its other bytes.
    c.deviceSelectWrite(0, 0xFE);
    expect(c.livePointer() == 0x01FFFE, "address write reloads the pointer from the latch");
    expect(c.deviceSelectRead(3) == 0x11 && c.deviceSelectRead(3) == 0x22 &&
           c.deviceSelectRead(3) == 0x33, "data port reads back with auto-increment");

    // 1 MB mask (a2memexp.cpp:130): $1xxxxx aliases $0xxxxx.
    c.deviceSelectWrite(0, 0xFE); c.deviceSelectWrite(1, 0xFF); c.deviceSelectWrite(2, 0x11);
    expect(c.deviceSelectRead(3) == 0x11, "address bits above 20 alias (1 MB)");

    // Plain latches $C0n4-$C0nF.
    c.deviceSelectWrite(0x8, 0x5A);
    expect(c.deviceSelectRead(0x8) == 0x5A, "$C0n8 is a plain latch");

    c.onReset();
    expect(c.livePointer() == 0 && c.deviceSelectRead(0x8) == 0x00,
           "reset clears the latches");
    expect(c.ramAt(0x01FFFE) == 0x11, "reset keeps the RAM");
}

void callDriver(M6502& cpu, Memory& mem, uint8_t cmd, uint16_t block, uint16_t buffer)
{
    mem.memWrite(0x42, cmd);
    mem.memWrite(0x43, static_cast<uint8_t>(kSlot << 4));
    mem.memWrite(0x44, static_cast<uint8_t>(buffer & 0xFF));
    mem.memWrite(0x45, static_cast<uint8_t>(buffer >> 8));
    mem.memWrite(0x46, static_cast<uint8_t>(block & 0xFF));
    mem.memWrite(0x47, static_cast<uint8_t>(block >> 8));
    const uint8_t entry = mem.memRead(static_cast<uint16_t>((kRomHi << 8) | 0xFF));
    const uint8_t code[] = { 0x20, entry, kRomHi, 0x4C, 0x03, 0x03 };   // JSR / park
    for (uint16_t i = 0; i < sizeof code; ++i) mem.memWrite(0x0300 + i, code[i]);
    cpu.setStackPointer(0xFF);
    cpu.setProgramCounter(0x0300);
    cpu.run(400000);
}

bool carry(M6502& cpu) { return (cpu.getStatusRegister() & 0x01) != 0; }

int firmware()
{
    const std::string path = pom2::findResource(SlinkyCard::kRomPath);
    if (path.empty()) {
        std::printf("SKIP: %s not found\n", SlinkyCard::kRomPath);
        return 77;
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), {});

    Memory mem;
    M6502  cpu(&mem);
    cpu.hardReset();
    auto card = std::make_unique<SlinkyCard>(kSlot);
    expect(card->loadRom(rom), "341-0344A loads");
    SlinkyCard* raw = card.get();
    mem.slotBus().plug(kSlot, std::move(card));

    expect(mem.memRead(0xC501) == 0x20 && mem.memRead(0xC503) == 0x00 &&
           mem.memRead(0xC505) == 0x03 && mem.memRead(0xC507) == 0x00,
           "SmartPort/ProDOS signature at $Cn01-$Cn07");

    callDriver(cpu, mem, 0x00, 0, 0x0800);
    const unsigned blocks = cpu.getXRegister() | (cpu.getYRegister() << 8);
    std::printf("STATUS: C=%d A=$%02X blocks=%u\n", carry(cpu), cpu.getAccumulator(), blocks);
    expect(!carry(cpu) && cpu.getAccumulator() == 0, "STATUS succeeds");
    expect(blocks > 0 && blocks <= 2048, "STATUS reports a 1 MB-class block count");

    for (int i = 0; i < 512; ++i)
        mem.memWrite(0x2000 + i, static_cast<uint8_t>((i * 7 + 3) & 0xFF));
    callDriver(cpu, mem, 0x02, 9, 0x2000);
    expect(!carry(cpu), "WRITE block 9 succeeds");

    for (int i = 0; i < 512; ++i) mem.memWrite(0x4000 + i, 0x00);
    callDriver(cpu, mem, 0x01, 9, 0x4000);
    expect(!carry(cpu), "READ block 9 succeeds");
    bool same = true;
    for (int i = 0; i < 512; ++i)
        same &= mem.memRead(0x4000 + i) == static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    expect(same, "READ returns the block WRITE stored");

    // The block went through the card's RAM: find its first 16 bytes there.
    bool found = false;
    for (uint32_t a = 0; a + 16 <= SlinkyCard::kRamBytes && !found; a += 256) {
        bool m = true;
        for (int i = 0; i < 16 && m; ++i)
            m = raw->ramAt(a + i) == static_cast<uint8_t>((i * 7 + 3) & 0xFF);
        found = m;
    }
    expect(found, "the written block lives in the card's RAM");

    // A second block must not disturb the first.
    for (int i = 0; i < 512; ++i) mem.memWrite(0x2000 + i, static_cast<uint8_t>(0xA5 ^ i));
    callDriver(cpu, mem, 0x02, 10, 0x2000);
    callDriver(cpu, mem, 0x01, 9, 0x4000);
    expect(mem.memRead(0x4000) == 3 && mem.memRead(0x4001) == 10, "block 9 survives a write to block 10");
    return 0;
}

} // namespace

int main()
{
    registers();
    const int rc = firmware();
    std::printf("slinky_card: %d failure(s)\n", failures);
    if (failures) return 1;
    return rc;
}
