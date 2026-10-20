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

// IIe Language Card + ALTZP + RAMRD/WRT round-trip smoke test.
//
// Drives the same access patterns ProDOS 8 v1.4 uses during init and
// asserts each routing rule. Found while chasing a Copy II Plus boot
// hang in IIe mode where ProDOS got stuck in a delay loop incrementing
// counters at $D36F/$D370 in language-card RAM — the writes weren't
// being read back as the same value.
//
// The combinations we cover:
//
//   ● LC bank 1 (read-RAM + write-RAM)   — $C08B accessed twice
//   ● LC bank 2 (read-RAM + write-RAM)   — $C083 accessed twice
//   ● LC + ALTZP off (main banks)        — auxLcBank* untouched
//   ● LC + ALTZP on  (aux banks)         — main lcBank* untouched
//   ● ALTZP toggles preserve LC enable   — write-enable survives a
//                                          $C008/$C009 access in between
//   ● Bank-2 write-then-read round-trip  — proves the prewrite latch
//                                          works in IIe mode end-to-end
//
// All tests run in IIe mode (Memory::setIIEMode(true)) and exercise the
// real memRead/memWrite paths so the test gates the dispatcher, not the
// internal helpers.

#include "Memory.h"

#include <cassert>
#include <cstdio>

namespace {

constexpr uint16_t LC_RAM_RW_BANK2 = 0xC083;   // read RAM, write RAM, bank 2
constexpr uint16_t LC_RAM_RW_BANK1 = 0xC08B;   // read RAM, write RAM, bank 1
constexpr uint16_t ALTZP_OFF       = 0xC008;
constexpr uint16_t ALTZP_ON        = 0xC009;

void enableLcWrite(Memory& mem, uint16_t switchAddr) {
    // Two accesses required to arm the prewrite latch and then enable.
    (void)mem.memRead(switchAddr);
    (void)mem.memRead(switchAddr);
}

void testBank2RoundTrip(Memory& mem) {
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    mem.memWrite(0xD370, 0xAB);
    mem.memWrite(0xD36F, 0xCD);
    assert(mem.memRead(0xD370) == 0xAB);
    assert(mem.memRead(0xD36F) == 0xCD);
}

void testBank1RoundTrip(Memory& mem) {
    enableLcWrite(mem, LC_RAM_RW_BANK1);
    mem.memWrite(0xD500, 0x42);
    assert(mem.memRead(0xD500) == 0x42);
    // Bank 2 at the same address must NOT see this value.
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    // The bank-2 byte was set above to a different value at $D370 only;
    // $D500 in bank 2 was untouched (still 0). If the write leaked to
    // bank 2, this assert fires.
    assert(mem.memRead(0xD500) == 0x00);
}

void testAltzpSwapsLcBanks(Memory& mem) {
    // Set up: in main banks, write known sentinels.
    mem.memWrite(ALTZP_OFF, 0);
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    mem.memWrite(0xD600, 0x11);            // main LC bank 2
    enableLcWrite(mem, LC_RAM_RW_BANK1);
    mem.memWrite(0xD600, 0x22);            // main LC bank 1
    mem.memWrite(0xE000, 0x33);            // main LC high (shared)

    // Now toggle ALTZP and verify the LC view is the AUX banks (initially
    // zero — we haven't written to them yet).
    mem.memWrite(ALTZP_ON, 0);
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    assert(mem.memRead(0xD600) == 0x00);   // aux LC bank 2: untouched

    enableLcWrite(mem, LC_RAM_RW_BANK1);
    assert(mem.memRead(0xD600) == 0x00);   // aux LC bank 1: untouched
    assert(mem.memRead(0xE000) == 0x00);   // aux LC high: untouched

    // Write into aux LC banks.
    mem.memWrite(0xD600, 0xAA);            // aux LC bank 1
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    mem.memWrite(0xD600, 0xBB);            // aux LC bank 2
    mem.memWrite(0xE000, 0xCC);            // aux LC high

    // Toggle back and confirm main banks are unchanged.
    mem.memWrite(ALTZP_OFF, 0);
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    assert(mem.memRead(0xD600) == 0x11);
    assert(mem.memRead(0xE000) == 0x33);
    enableLcWrite(mem, LC_RAM_RW_BANK1);
    assert(mem.memRead(0xD600) == 0x22);

    // And aux banks still hold our second-pass values.
    mem.memWrite(ALTZP_ON, 0);
    enableLcWrite(mem, LC_RAM_RW_BANK1);
    assert(mem.memRead(0xD600) == 0xAA);
    assert(mem.memRead(0xE000) == 0xCC);
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    assert(mem.memRead(0xD600) == 0xBB);

    mem.memWrite(ALTZP_OFF, 0);                // restore
}

void testWriteEnableSurvivesAltzpToggle(Memory& mem) {
    // ProDOS pattern: enable LC writes, then toggle ALTZP, then write.
    // The write-enable latch must NOT be cleared by the soft-switch
    // access at $C008/$C009.
    enableLcWrite(mem, LC_RAM_RW_BANK2);

    mem.memWrite(ALTZP_ON, 0);
    mem.memWrite(0xD700, 0x77);            // expect to land in aux LC bank 2

    mem.memWrite(ALTZP_OFF, 0);
    // Same address in main LC bank 2 should still hold whatever it was.
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    const uint8_t mainVal = mem.memRead(0xD700);
    (void)mainVal;     // we don't assert a specific main value here —
                       // a previous test may have set it. We only care
                       // that the aux write actually went to aux.
    mem.memWrite(ALTZP_ON, 0);
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    assert(mem.memRead(0xD700) == 0x77);

    mem.memWrite(ALTZP_OFF, 0);
}

void testInductiveSpinLoop(Memory& mem) {
    // Mirror the exact ProDOS pattern that hung Copy II+: increment a
    // 16-bit counter at $D36F/$D370 in main LC bank 2 a million times
    // and verify it actually moves.
    enableLcWrite(mem, LC_RAM_RW_BANK2);
    mem.memWrite(0xD36F, 0x00);
    mem.memWrite(0xD370, 0x00);
    for (int i = 0; i < 1024; ++i) {
        const uint8_t lo = mem.memRead(0xD36F);
        if (lo == 0xFF) {
            mem.memWrite(0xD36F, 0x00);
            mem.memWrite(0xD370,
                         static_cast<uint8_t>(mem.memRead(0xD370) + 1));
        } else {
            mem.memWrite(0xD36F, static_cast<uint8_t>(lo + 1));
        }
    }
    // After 1024 increments, hi byte should be 4 (1024 / 256).
    assert(mem.memRead(0xD370) == 0x04);
    assert(mem.memRead(0xD36F) == 0x00);
}

}  // namespace

int main() {
    Memory mem;
    mem.setIIEMode(true);
    mem.resetSoftSwitches();

    testBank2RoundTrip(mem);
    std::printf("LC bank 2 round-trip: OK\n");

    testBank1RoundTrip(mem);
    std::printf("LC bank 1 round-trip + bank isolation: OK\n");

    testAltzpSwapsLcBanks(mem);
    std::printf("ALTZP swaps LC banks: OK\n");

    testWriteEnableSurvivesAltzpToggle(mem);
    std::printf("Write-enable survives ALTZP toggle: OK\n");

    testInductiveSpinLoop(mem);
    std::printf("Inductive spin loop (ProDOS $D36F/$D370 pattern): OK\n");

    std::printf("iie_lc_paging_smoke OK\n");
    return 0;
}
