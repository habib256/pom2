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

// IIe memory smoke test — pins the auxiliary 64 KB bank, the IIe paging
// soft switches, and the internal I/O ROM dispatch. Gates any change to
// the IIe-specific code paths in Memory.cpp (iieMemRead, iieMemWrite,
// iieHandleSoftSwitch, iieReadStatus, the loadAppleIIRom split, and the
// $C100-$CFFF routing in memRead).
//
// Headless: no ImGui, no OpenGL.

#include "Memory.h"

#include <cassert>
#include <cstdio>

namespace {

constexpr uint16_t IIE_80STORE_OFF = 0xC000;
constexpr uint16_t IIE_80STORE_ON  = 0xC001;
constexpr uint16_t IIE_RAMRD_OFF   = 0xC002;
constexpr uint16_t IIE_RAMRD_ON    = 0xC003;
constexpr uint16_t IIE_RAMWRT_OFF  = 0xC004;
constexpr uint16_t IIE_RAMWRT_ON   = 0xC005;
constexpr uint16_t IIE_INTCXROM_OFF = 0xC006;
constexpr uint16_t IIE_INTCXROM_ON  = 0xC007;
constexpr uint16_t IIE_ALTZP_OFF   = 0xC008;
constexpr uint16_t IIE_ALTZP_ON    = 0xC009;
constexpr uint16_t IIE_SLOTC3_OFF  = 0xC00A;
constexpr uint16_t IIE_SLOTC3_ON   = 0xC00B;
constexpr uint16_t IIE_80COL_OFF   = 0xC00C;
constexpr uint16_t IIE_80COL_ON    = 0xC00D;
constexpr uint16_t IIE_ALTCHAR_OFF = 0xC00E;
constexpr uint16_t IIE_ALTCHAR_ON  = 0xC00F;

constexpr uint16_t IIE_RD_RAMRD     = 0xC013;
constexpr uint16_t IIE_RD_RAMWRT    = 0xC014;
constexpr uint16_t IIE_RD_INTCXROM  = 0xC015;
constexpr uint16_t IIE_RD_ALTZP     = 0xC016;
constexpr uint16_t IIE_RD_SLOTC3ROM = 0xC017;
constexpr uint16_t IIE_RD_80STORE   = 0xC018;
constexpr uint16_t IIE_RD_ALTCHAR   = 0xC01E;
constexpr uint16_t IIE_RD_80COL     = 0xC01F;

constexpr uint16_t SET_GR    = 0xC050;
constexpr uint16_t SET_TEXT  = 0xC051;
constexpr uint16_t SET_PAGE1 = 0xC054;
constexpr uint16_t SET_PAGE2 = 0xC055;
constexpr uint16_t SET_LORES = 0xC056;
constexpr uint16_t SET_HIRES = 0xC057;

void testRAMRDIndependence(Memory& mem)
{
    // Both banks must be writable independently. With RAMRD off, write
    // 0x42 to main $1000. Turn RAMRD on, write 0x99 — the write goes to
    // main, not aux (RAMRD only routes reads). To get the value into aux,
    // we need RAMWRT on too.
    mem.memWrite(IIE_RAMRD_OFF, 0);
    mem.memWrite(IIE_RAMWRT_OFF, 0);
    mem.memWrite(0x1000, 0x42);
    assert(mem.memRead(0x1000) == 0x42);

    // Switch writes to aux, write a different sentinel.
    mem.memWrite(IIE_RAMWRT_ON, 0);
    mem.memWrite(0x1000, 0x99);

    // Reads still come from main → still 0x42.
    assert(mem.memRead(0x1000) == 0x42);

    // Switch reads to aux → now 0x99.
    mem.memWrite(IIE_RAMRD_ON, 0);
    assert(mem.memRead(0x1000) == 0x99);

    // Switch back, main is still 0x42 (proves the two banks are
    // independent storage, not aliased).
    mem.memWrite(IIE_RAMRD_OFF, 0);
    mem.memWrite(IIE_RAMWRT_OFF, 0);
    assert(mem.memRead(0x1000) == 0x42);
}

void testALTZP(Memory& mem)
{
    // Zero page + stack switch under ALTZP.
    mem.memWrite(IIE_ALTZP_OFF, 0);
    mem.memWrite(0x00FF, 0x12);
    assert(mem.memRead(0x00FF) == 0x12);
    mem.memWrite(IIE_ALTZP_ON, 0);
    mem.memWrite(0x00FF, 0xAB);  // writes to aux
    assert(mem.memRead(0x00FF) == 0xAB);
    mem.memWrite(IIE_ALTZP_OFF, 0);
    assert(mem.memRead(0x00FF) == 0x12);  // main intact

    // Stack region $0100-$01FF too.
    mem.memWrite(IIE_ALTZP_ON, 0);
    mem.memWrite(0x01F0, 0x55);
    mem.memWrite(IIE_ALTZP_OFF, 0);
    mem.memWrite(0x01F0, 0x66);
    assert(mem.memRead(0x01F0) == 0x66);
    mem.memWrite(IIE_ALTZP_ON, 0);
    assert(mem.memRead(0x01F0) == 0x55);
    mem.memWrite(IIE_ALTZP_OFF, 0);
}

void test80STOREPAGE2(Memory& mem)
{
    // 80STORE makes PAGE2 swap text page 1 ($0400-$07FF) to aux RAM
    // *regardless* of RAMRD/RAMWRT. Verify by writing to $0400 with
    // 80STORE on and PAGE2 toggling.
    mem.memWrite(IIE_RAMRD_OFF, 0);
    mem.memWrite(IIE_RAMWRT_OFF, 0);
    mem.memRead(SET_PAGE1);
    mem.memWrite(IIE_80STORE_OFF, 0);  // $C000 read is kbLatch; only write toggles 80STORE
    mem.memWrite(0x0400, 0x77);  // → main
    assert(mem.memRead(0x0400) == 0x77);

    mem.memWrite(IIE_80STORE_ON, 0);
    mem.memRead(SET_PAGE2);
    mem.memWrite(0x0400, 0xEE);  // 80STORE+PAGE2 → aux
    assert(mem.memRead(0x0400) == 0xEE);

    mem.memRead(SET_PAGE1);      // 80STORE+PAGE1 → main
    assert(mem.memRead(0x0400) == 0x77);

    // Confirm aux really got the byte by inspecting auxData() directly.
    assert(mem.auxData()[0x0400] == 0xEE);

    mem.memWrite(IIE_80STORE_OFF, 0);  // $C000 read is kbLatch; only write toggles 80STORE
    mem.memRead(SET_PAGE1);
}

void testINTCXROMandSLOTC3ROM(Memory& mem)
{
    // Stuff a sentinel in the internal I/O ROM and one in main $C100
    // (which would be slot 1 ROM if any card were plugged). Without a
    // slot 1 card, slotRomRead returns the FLOATING BUS — MAME
    // `apple2e.cpp:2970-2987` `read_slot_rom` ends in
    // `return read_floatingbus();` when `m_slotdevice[slotnum]` is null.
    // (It used to be a hard $FF, which is only what SlotBus answers when
    // nobody installed a floating-bus source; Memory always does.)
    //
    // We can't directly poke internalIORom from outside; instead rely on
    // it being all-zero (constructor) and check that INTCXROM=on returns
    // 0x00 from $C100 (internal) vs the open bus when off. No CPU is
    // attached here, so `floatingBus()` and `peekFloatingBus()` sample the
    // same cycle.
    const uint8_t bus = mem.peekFloatingBus();
    mem.memWrite(IIE_INTCXROM_OFF, 0);
    mem.memWrite(IIE_SLOTC3_ON, 0);   // disable internal $C300 too
    const uint8_t off1 = mem.memRead(0xC100);
    const uint8_t off3 = mem.memRead(0xC300);
    assert(off1 == bus);  // open bus from empty slot 1
    assert(off3 == bus);  // open bus from empty slot 3

    mem.memWrite(IIE_INTCXROM_ON, 0);
    assert(mem.memRead(0xC100) == 0x00);  // internal ROM (zero-init)
    assert(mem.memRead(0xC300) == 0x00);
    mem.memWrite(IIE_INTCXROM_OFF, 0);

    // SLOTC3ROM=off forces $C300-$C3FF to internal even when INTCXROM=off.
    mem.memWrite(IIE_SLOTC3_OFF, 0);
    assert(mem.memRead(0xC300) == 0x00);  // internal
    assert(mem.memRead(0xC100) == bus);   // slot bus (still open)
    mem.memWrite(IIE_SLOTC3_ON, 0);
    assert(mem.memRead(0xC300) == bus);   // back to slot bus
}

void testStatusReads(Memory& mem)
{
    // Each switch read (high bit) must reflect the current mode.
    auto rdHi = [&](uint16_t a){ return (mem.memRead(a) & 0x80) != 0; };

    mem.memWrite(IIE_RAMRD_OFF, 0);   assert(!rdHi(IIE_RD_RAMRD));
    mem.memWrite(IIE_RAMRD_ON, 0);    assert( rdHi(IIE_RD_RAMRD));
    mem.memWrite(IIE_RAMRD_OFF, 0);

    mem.memWrite(IIE_RAMWRT_ON, 0);   assert( rdHi(IIE_RD_RAMWRT));
    mem.memWrite(IIE_RAMWRT_OFF, 0);  assert(!rdHi(IIE_RD_RAMWRT));

    mem.memWrite(IIE_INTCXROM_ON, 0); assert( rdHi(IIE_RD_INTCXROM));
    mem.memWrite(IIE_INTCXROM_OFF, 0);assert(!rdHi(IIE_RD_INTCXROM));

    mem.memWrite(IIE_ALTZP_ON, 0);    assert( rdHi(IIE_RD_ALTZP));
    mem.memWrite(IIE_ALTZP_OFF, 0);   assert(!rdHi(IIE_RD_ALTZP));

    mem.memWrite(IIE_SLOTC3_ON, 0);   assert( rdHi(IIE_RD_SLOTC3ROM));
    mem.memWrite(IIE_SLOTC3_OFF, 0);  assert(!rdHi(IIE_RD_SLOTC3ROM));

    mem.memWrite(IIE_80STORE_ON, 0);          assert( rdHi(IIE_RD_80STORE));
    mem.memWrite(IIE_80STORE_OFF, 0);     assert(!rdHi(IIE_RD_80STORE));

    mem.memWrite(IIE_80COL_ON, 0);    assert( rdHi(IIE_RD_80COL));
    mem.memWrite(IIE_80COL_OFF, 0);   assert(!rdHi(IIE_RD_80COL));

    mem.memWrite(IIE_ALTCHAR_ON, 0);  assert( rdHi(IIE_RD_ALTCHAR));
    mem.memWrite(IIE_ALTCHAR_OFF, 0); assert(!rdHi(IIE_RD_ALTCHAR));
}

void testIIPlusRegression()
{
    // Same Memory class with iieMode off must keep behaving like an Apple
    // II+. The IIe paths are gated, the aux bank is never consulted, and
    // the soft-switch dispatch ignores IIe-only writes.
    Memory mem;
    assert(!mem.isIIE());
    mem.memWrite(0x1000, 0xAA);
    assert(mem.memRead(0x1000) == 0xAA);

    // Toggle a IIe-only switch — must NOT change behaviour on II+.
    mem.memWrite(IIE_RAMRD_ON, 0);
    mem.memWrite(0x1100, 0xBB);
    assert(mem.memRead(0x1100) == 0xBB);
    mem.memWrite(IIE_RAMRD_OFF, 0);
    // The aux bank should still be all zeros (nobody ever wrote to it).
    assert(mem.auxData()[0x1100] == 0x00);
    assert(mem.auxData()[0x1000] == 0x00);

    // Status reads at $C013 should NOT short-circuit (II+ falls through to
    // the floating bus / 0). Just ensure it doesn't crash.
    (void)mem.memWrite(IIE_RD_RAMRD, 0);
}

} // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);
    assert(mem.isIIE());

    testRAMRDIndependence(mem);
    testALTZP(mem);
    test80STOREPAGE2(mem);
    testINTCXROMandSLOTC3ROM(mem);
    testStatusReads(mem);
    testIIPlusRegression();

    std::printf("IIe memory smoke: OK\n");
    return 0;
}
