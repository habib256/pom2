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

// RamWorks III smoke test — pins the Applied Engineering aux-slot RAM
// expansion port. Verbatim against MAME `src/devices/bus/a2bus/
// a2eramworks3.cpp`:
//   - bank select on writes to $C0n1/3/5/7 (predicate
//     `(offset & 0x9) == 1`, line 111),
//   - bank index = `data & 0x7F` (line 113), max 128 banks (8 MB),
//   - reset clears bank to 0 (line 67),
//   - non-matching low-nibble writes ($C070/2/4/6/8/9/A/B/C/D/E/F)
//     leave the bank unchanged,
//   - IIe-only (no effect when iieMode is off).
//
// Headless: no ImGui, no OpenGL.

#include "Memory.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace {

constexpr uint16_t IIE_RAMRD_OFF  = 0xC002;
constexpr uint16_t IIE_RAMRD_ON   = 0xC003;
constexpr uint16_t IIE_RAMWRT_OFF = 0xC004;
constexpr uint16_t IIE_RAMWRT_ON  = 0xC005;

// Selects RAM aux for reads + writes — every $1000 byte goes through
// the iieMem* dispatchers into the current RamWorks bank's `aux`.
void enableAuxRW(Memory& mem)
{
    mem.memWrite(IIE_RAMRD_ON,  0);
    mem.memWrite(IIE_RAMWRT_ON, 0);
}

void disableAuxRW(Memory& mem)
{
    mem.memWrite(IIE_RAMRD_OFF,  0);
    mem.memWrite(IIE_RAMWRT_OFF, 0);
}

// Plant a sentinel byte at $1234 in the current bank.
void plantSentinel(Memory& mem, uint8_t value)
{
    enableAuxRW(mem);
    mem.memWrite(0x1234, value);
    disableAuxRW(mem);
}

uint8_t readSentinel(Memory& mem)
{
    enableAuxRW(mem);
    const uint8_t v = mem.memRead(0x1234);
    disableAuxRW(mem);
    return v;
}

void selectBank(Memory& mem, uint8_t bank)
{
    // MAME predicate `(offset & 0x9) == 1` matches $C071/3/5/7.
    // Use $C073 — the canonical address that the AE driver writes.
    mem.memWrite(0xC073, bank);
}

void testStockIIeRegression()
{
    // banks == 1 (default) = stock 64 KB aux. Writes to $C073 must NOT
    // alter aux contents (no swap path).
    Memory mem;
    mem.setIIEMode(true);
    assert(mem.ramWorksBanks() == 1);
    assert(mem.ramWorksBank()  == 0);

    plantSentinel(mem, 0xAB);
    mem.memWrite(0xC073, 0x05);   // would select bank 5 if RamWorks active
    assert(mem.ramWorksBank() == 0);
    assert(readSentinel(mem) == 0xAB);
}

void testBankIsolation()
{
    // 16 banks (1 MB). Write a distinct sentinel into each bank, then
    // rotate through and read back — every bank must remember its own
    // value independently.
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(16);
    assert(mem.ramWorksBanks() == 16);
    assert(mem.ramWorksBank()  == 0);

    for (uint8_t b = 0; b < 16; ++b) {
        selectBank(mem, b);
        assert(mem.ramWorksBank() == b);
        plantSentinel(mem, static_cast<uint8_t>(0x10 + b));
    }
    // Cross-check in reverse to exercise the bank-N → bank-0 path too.
    for (int b = 15; b >= 0; --b) {
        selectBank(mem, static_cast<uint8_t>(b));
        assert(readSentinel(mem) == static_cast<uint8_t>(0x10 + b));
    }
}

void testPredicateMaskC0n1_3_5_7()
{
    // MAME `a2eramworks3.cpp:111 (offset & 0x9) == 1` — writes to
    // $C071, $C073, $C075, $C077 select the bank; writes to the other
    // 12 addresses in $C070-$C07F do NOT.
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(8);

    // Start on bank 3.
    mem.memWrite(0xC073, 3);
    assert(mem.ramWorksBank() == 3);

    // The four addresses that DO select:
    for (uint16_t addr : { 0xC071, 0xC073, 0xC075, 0xC077 }) {
        // First, get away from the target so the swap actually runs.
        mem.memWrite(0xC073, 3);
        mem.memWrite(addr, 5);
        assert(mem.ramWorksBank() == 5);
    }

    // The twelve addresses that must NOT select. Pre-set bank to 2,
    // then poke each non-selecting address with data 7; bank should
    // stay at 2.
    for (uint16_t addr : { 0xC070, 0xC072, 0xC074, 0xC076,
                           0xC078, 0xC079, 0xC07A, 0xC07B,
                           0xC07C, 0xC07D, 0xC07E, 0xC07F }) {
        mem.memWrite(0xC073, 2);
        assert(mem.ramWorksBank() == 2);
        mem.memWrite(addr, 7);
        assert(mem.ramWorksBank() == 2);
    }
}

void testHighBitMaskedOff()
{
    // MAME line 113: `m_bank = 0x10000 * (data & 0x7f)`. The top bit
    // of the data byte is masked off — writing 0x83 with 8 banks
    // selects bank (0x83 & 0x7F) % 8 = 3 % 8 = 3.
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(8);

    mem.memWrite(0xC073, 0x83);
    assert(mem.ramWorksBank() == 3);

    // 0xFF & 0x7F = $7F: not one of the eight banks installed. The register
    // names nothing; the last installed bank is still the one on record.
    mem.memWrite(0xC073, 0xFF);
    assert(mem.ramWorksBankAbsent() && mem.ramWorksBank() == 3);
}

// A bank number with no chips behind it is ABSENT — not an alias of an
// installed one (POM2 wrapped modulo the count until 2026-11-18).
// [B] RamWorks III User's Manual v1.4.1, Table 6-1 and ch. 7 p. 47.
void testUninstalledBankIsAbsent()
{
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(4);

    for (uint8_t b = 0; b < 4; ++b) {
        selectBank(mem, b);
        plantSentinel(mem, static_cast<uint8_t>(0xA0 + b));
    }
    selectBank(mem, 7);
    assert(mem.ramWorksBankAbsent());
    assert(readSentinel(mem) == 0xFF);            // nothing drives the bus
    plantSentinel(mem, 0x5A);                     // written nowhere
    selectBank(mem, 9);                           // another absent bank
    selectBank(mem, 3);
    assert(!mem.ramWorksBankAbsent() && mem.ramWorksBank() == 3);
    for (uint8_t b = 0; b < 4; ++b) {
        selectBank(mem, b);
        assert(readSentinel(mem) == static_cast<uint8_t>(0xA0 + b));
    }

    // The scanner's bank 0 is not the scratch of an absent bank.
    selectBank(mem, 0);
    const uint8_t bank0Byte = mem.videoAuxData()[0x0400];
    selectBank(mem, 0x40);
    assert(mem.videoAuxData() != mem.auxDataMutable());
    assert(mem.videoAuxData()[0x0400] == bank0Byte);

    // A snapshot taken with an absent bank selected restores as one, and
    // does not write the scratch into an installed bank.
    plantSentinel(mem, 0x77);
    std::vector<uint8_t> blob;
    mem.appendSnapshotState(blob);
    Memory other;
    other.setIIEMode(true);
    other.setRamWorksBanks(4);
    assert(other.loadSnapshotState(blob.data(), blob.size()));
    assert(other.ramWorksBankAbsent());
    for (uint8_t b = 0; b < 4; ++b) {
        selectBank(other, b);
        assert(readSentinel(other) == static_cast<uint8_t>(0xA0 + b));
    }
}

// Applied Engineering's own FindBanks (manual p. 47): N and ~N at $00/$01 of
// every bank from $7F DOWN, read back going up. It must count exactly the
// banks installed — and so must the same routine writing UPWARD, which the
// modulo aliasing got wrong (each installed bank ended up holding the number
// of its highest alias).
void testFindBanksCountsTheInstalledBanks()
{
    for (const bool descending : { true, false }) {
        for (const uint32_t banks : { 4u, 8u, 16u, 48u, 128u }) {
            Memory mem;
            mem.setIIEMode(true);
            mem.setRamWorksBanks(banks);
            mem.memWrite(0xC009, 0);                       // ALTZP: $00/$01 in aux
            for (int k = 0; k < 128; ++k) {
                const uint8_t y = static_cast<uint8_t>(descending ? 127 - k : k);
                mem.memWrite(0xC073, y);
                mem.memWrite(0x0000, y);
                mem.memWrite(0x0001, static_cast<uint8_t>(~y));
            }
            uint32_t found = 0;
            for (int y = 0; y < 128; ++y) {
                mem.memWrite(0xC073, static_cast<uint8_t>(y));
                if (mem.memRead(0x0000) == y &&
                    mem.memRead(0x0001) == static_cast<uint8_t>(~y)) {
                    // Table 6-1: the 3 MB card is the 1 MB board ($00-$0F)
                    // plus the 2 Meg expander at $10-$17, $30-$37, $50-$57,
                    // $70-$77 — the manual's "48 valid banks".
                    const bool expected = banks == 48
                        ? (y < 0x18 || ((y & 0x18) == 0x10 && y >= 0x30))
                        : static_cast<uint32_t>(y) < banks;
                    assert(expected);
                    assert(mem.ramWorksBankNumber(mem.ramWorksBank()) == y);
                    ++found;
                }
            }
            assert(found == banks);
            mem.memWrite(0xC073, 0);
            mem.memWrite(0xC008, 0);
        }
    }
}

void testResetWipesAndBankZero()
{
    // `clearRam()` on a RamWorks-enabled IIe must wipe every bank and
    // snap the current bank back to 0. Loose pin on the "cold boot"
    // semantic — applyProfile() relies on this so a profile switch
    // doesn't leak RamWorks contents across machines.
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(8);

    selectBank(mem, 5);
    plantSentinel(mem, 0xC5);
    selectBank(mem, 1);
    plantSentinel(mem, 0xC1);
    mem.clearRam();
    assert(mem.ramWorksBank() == 0);

    for (uint8_t b = 0; b < 8; ++b) {
        selectBank(mem, b);
        assert(readSentinel(mem) == 0x00);
    }

    // Hidden banks get the same 00/FF pattern, not zeros. Slot Config
    // Apply used to call setRamWorksBanks AFTER coldBoot; assign(0)
    // then left banks 1+ as a zero fill `$C073` could reach.
    selectBank(mem, 7);
    enableAuxRW(mem);
    assert(mem.memRead(0x1234) == 0x00);
    assert(mem.memRead(0x1235) == 0xFF);
    disableAuxRW(mem);
}

void testIIPlusNotAffected()
{
    // Plain II/II+ has no aux RAM — RamWorks writes to $C073 must not
    // crash or alter state. iieMode is off, so the swap path is gated
    // and the access falls through to the paddle-reset mirror.
    Memory mem;
    // do NOT call setIIEMode(true)
    assert(!mem.isIIE());

    // Even if a user mistakenly called setRamWorksBanks() before iie
    // mode flipped on, the setter doesn't gate on iieMode — but the
    // dispatch in softSwitchAccess does. So banks state may exist but
    // the bank never moves off 0.
    mem.setRamWorksBanks(8);
    mem.memWrite(0xC073, 5);
    assert(mem.ramWorksBank() == 0);
}

void testSetIIEModeFalseDropsBacking()
{
    // Leaving IIe mode must release the RamWorks backing — saves
    // memory and pins the invariant that aux state is IIe-only.
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(48);   // 3 MB
    assert(mem.ramWorksBanks() == 48);

    selectBank(mem, 10);
    assert(mem.ramWorksBank() == 10);

    mem.setIIEMode(false);
    assert(mem.ramWorksBanks() == 1);
    assert(mem.ramWorksBank()  == 0);
}

// The DHGR paint host edits the PICTURE: its writes go to the bank the
// screen shows (bank 0), whatever bank the CPU has selected. It used
// auxDataMutable(), the CPU's bank, so with bank 1 selected every stroke
// landed in the program's private bank and nothing changed on screen.
void testVideoAuxMutableIsTheVisibleBank()
{
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(4);
    selectBank(mem, 1);
    mem.videoAuxDataMutable()[0x2000] = 0x5A;
    assert(mem.videoAuxData()[0x2000] == 0x5A);
    assert(mem.auxData()[0x2000] != 0x5A && "wrote the CPU's bank, not the screen's");
    selectBank(mem, 0);
    assert(mem.auxData()[0x2000] == 0x5A && "the byte is in bank 0");
}

void testResetSnapsBankToZero()
{
    // MAME `a2eramworks3.cpp:65-68 device_reset` snaps `m_bank = 0` on a
    // machine reset (not the reset key). Pin that POM2's cold
    // `resetSoftSwitches()` does the same when RamWorks is active. Data in all banks must SURVIVE the snap
    // — reset clears the selector only, not the DRAM.
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(4);

    // Plant distinct sentinels in banks 0..3.
    for (uint8_t b = 0; b < 4; ++b) {
        selectBank(mem, b);
        plantSentinel(mem, static_cast<uint8_t>(0xE0 + b));
    }

    // Park on bank 2 then reset. Bank must snap to 0; bank 2's
    // sentinel must still be readable after rotating back.
    selectBank(mem, 2);
    assert(mem.ramWorksBank() == 2);
    // The reset KEY (Ctrl-Reset / F12 → resetSoftSwitchesWarm) keeps the
    // bank: MAME's reset_w never resets the aux slot, AppleWin's
    // MemResetPaging keeps g_uActiveBank.
    mem.resetSoftSwitchesWarm();
    assert(mem.ramWorksBank() == 2);
    assert(readSentinel(mem) == 0xE2);
    // The machine reset (power cycle) snaps it to 0.
    mem.resetSoftSwitches();
    assert(mem.ramWorksBank() == 0);
    assert(readSentinel(mem) == 0xE0);   // bank 0's sentinel still there

    selectBank(mem, 2);
    assert(readSentinel(mem) == 0xE2);   // bank 2 preserved through reset

    // No-op path: with banks == 1 (stock IIe), resetSoftSwitches must
    // not touch ramWorksBank_ (it's always 0 anyway) and must not
    // crash when ramWorksBacking_ is empty.
    Memory stock;
    stock.setIIEMode(true);
    stock.resetSoftSwitches();
    assert(stock.ramWorksBank() == 0);
}

void testMaxBanksCap()
{
    // 128 banks cap (MAME line 99: "RW3 supports 00-7F for 8 MB").
    // Passing 256 must clamp to 128.
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(256);
    assert(mem.ramWorksBanks() == 128);

    // Bank 127 reachable.
    selectBank(mem, 127);
    assert(mem.ramWorksBank() == 127);
    plantSentinel(mem, 0x7F);
    selectBank(mem, 0);
    selectBank(mem, 127);
    assert(readSentinel(mem) == 0x7F);
}

} // namespace

// The video scanner fetches from the FIRST 64 KB aux bank, whatever bank the
// CPU has selected (MAME a2eramworks3.cpp get_vram_ptr → m_ram[0]; AppleWin
// "Video scanner … always fetches from 1st 64K aux bank"). POM2 painted the
// CPU-selected bank: a RamWorks program sitting in bank N at frame time
// turned every aux column of the 80-column screen to garbage (bug hunt
// 2026-09-29).
void testVideoReadsBankZero()
{
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(4);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.memWrite(0xC005, 0);          // RAMWRT on: $0200-$BFFF writes → aux
    mem.memWrite(0x0400, 0xC1);       // bank 0 text page
    mem.memWrite(0xC073, 0x01);       // select bank 1
    mem.memWrite(0x0400, 0xC2);       // bank 1 text page
    mem.memWrite(0xC004, 0);
    assert(mem.ramWorksBank() == 1);
    assert(mem.auxData()[0x400] == 0xC2);          // the CPU's view: bank 1
    assert(mem.videoAuxData()[0x400] == 0xC1 &&
           "the video path read the CPU-selected RamWorks bank");
    mem.memWrite(0xC073, 0x00);
    assert(mem.videoAuxData()[0x400] == 0xC1 && mem.videoAuxData() == mem.auxData());
    std::printf("  video reads RamWorks bank 0 whatever the CPU selected: OK\n");
}

int main()
{
    testStockIIeRegression();
    testBankIsolation();
    testPredicateMaskC0n1_3_5_7();
    testHighBitMaskedOff();
    testUninstalledBankIsAbsent();
    testFindBanksCountsTheInstalledBanks();
    testResetWipesAndBankZero();
    testIIPlusNotAffected();
    testSetIIEModeFalseDropsBacking();
    testVideoAuxMutableIsTheVisibleBank();
    testResetSnapsBankToZero();
    testMaxBanksCap();

    std::printf("RamWorks III smoke: OK\n");
    testVideoReadsBankZero();
    return 0;
}
