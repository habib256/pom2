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

// Pins the //e auto-INTCXROM for the $C800-$CFFF expansion-ROM window.
//
// Real Apple //e hardware has a hidden flip-flop ("m_intc8rom" in MAME)
// that is set whenever a CPU read hits $C300-$C3FF with SLOTC3ROM=off,
// and cleared whenever a read hits $CFFF. While set, the shared expansion
// window $C800-$CFFF returns the motherboard internal ROM instead of the
// active slot's expansion ROM — that is how the //e 80-col firmware
// reaches its 2 KB continuation in $C800+ from a `JSR $C300` entry, and
// it is the documented reason `JSR $CFFF` is the recommended way for
// firmware to release the window before returning to the caller.
//
// Verbatim port of MAME `apple/apple2e.cpp::apple2e_state::c300_int_r` /
// `c800_int_r`. Without this side effect, software that does `JSR $C300`
// (any //e 80-col-aware app, e.g. ScoSwamp or Copy II Plus from a `.2mg`
// HDV under stock ProDOS) crashes within microseconds: the firmware's
// internal JMP $C803 / JSR $CD5B reach the slot bus (= $FF empty), the
// CPU walks `$FF`-as-BBS7 through to $D000+ ROM data, eventually decodes
// a JMP indirect through a stale user-RAM vector, and BRKs in zero RAM.

#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

void poison(Memory& mem)
{
    // Stuff a tell-tale byte at $C8XY in the internal ROM via the public
    // accessor so we can prove our reads come from internalIORom, not
    // slot expansion.
    auto* iorom = const_cast<uint8_t*>(mem.internalIORomData());
    iorom[0x800] = 0x42;   // $C800
    iorom[0x9AB] = 0x77;   // $C9AB
    iorom[0xFFE] = 0x55;   // $CFFE — must still read from internal
    iorom[0xFFF] = 0x99;   // $CFFF — value to verify even on release
    iorom[0x300] = 0xAA;   // $C300 — first byte the firmware sees
    iorom[0x3FE] = 0xBB;   // $C3FE — last in-page byte
}

void testInitiallyOffSoSlotBusOwns()
{
    Memory mem;
    mem.setIIEMode(true);
    poison(mem);
    // Fresh power-on: intC8Rom is off, no slot has expansion, so $C8xx
    // reads return the floating-bus default ($FF) from the slot bus
    // shim, NOT our poisoned internal-ROM bytes.
    const uint8_t got = mem.memRead(0xC9AB);
    assert(got != 0x77 && "C8xx must NOT read internal ROM with intC8Rom=off");
}

void testC3xxAutoEnablesIntC8Rom()
{
    Memory mem;
    mem.setIIEMode(true);
    poison(mem);
    // Touch $C3xx (any byte in the page) — emulates the first instruction
    // fetch when control transfers into the 80-col firmware. SLOTC3ROM is
    // off by default after setIIEMode, so the read returns internal ROM
    // AND sets intC8Rom as a side effect.
    const uint8_t firstFw = mem.memRead(0xC300);
    assert(firstFw == 0xAA && "C300 must return internal ROM byte with SLOTC3ROM=off");

    // Now $C800-$CFFE must return internal ROM (the side effect happened).
    assert(mem.memRead(0xC800) == 0x42);
    assert(mem.memRead(0xC9AB) == 0x77);
    assert(mem.memRead(0xCFFE) == 0x55);
}

void testCfffClearsIntC8Rom()
{
    Memory mem;
    mem.setIIEMode(true);
    poison(mem);
    // Arm intC8Rom via $C3xx.
    (void)mem.memRead(0xC3FE);
    assert(mem.memRead(0xC800) == 0x42 && "intC8Rom should be armed");

    // $CFFF: the read itself returns the internal-ROM byte (per
    // MAME c800_int_r, the read happens THEN the flip-flop clears), but
    // subsequent $C8xx reads must drop back to the slot bus.
    const uint8_t atRelease = mem.memRead(0xCFFF);
    assert(atRelease == 0x99 && "CFFF read returns internal ROM on the same access");
    const uint8_t afterRelease = mem.memRead(0xC9AB);
    assert(afterRelease != 0x77 && "C8xx must drop back to slot bus after CFFF release");
}

void testSlotC3RomDisablesAutoEnable()
{
    Memory mem;
    mem.setIIEMode(true);
    poison(mem);
    // $C00B sets SLOTC3ROM (PR#3 uses the slot 3 card's ROM instead of
    // the internal 80-col firmware). On //e only WRITES to the $C00x
    // bank latch the softswitch — reads return the keyboard latch.
    // With SLOTC3ROM=on, reads of $C3xx must NOT auto-arm intC8Rom —
    // the slot 3 card owns that window.
    mem.memWrite(0xC00B, 0);
    (void)mem.memRead(0xC300);
    const uint8_t got = mem.memRead(0xC9AB);
    assert(got != 0x77 && "SLOTC3ROM=on must inhibit the auto-INTCXROM side effect");
}

void testIntcxromSoftSwitchUnchanged()
{
    Memory mem;
    mem.setIIEMode(true);
    poison(mem);
    // After hitting $C3xx, the auto-INTCXROM flag is on, BUT the
    // user-visible INTCXROM softswitch ($C015 status) must NOT report
    // as set — they are distinct on real //e.
    (void)mem.memRead(0xC300);
    const uint8_t rdcxrom = mem.memRead(0xC015);  // RDCXROM: bit 7 = state
    assert((rdcxrom & 0x80) == 0 &&
           "$C015 must reflect the software MF_INTCXROM, not the auto flip-flop");
}

void testIntcxromOnStillArmsIntC8Rom()
{
    Memory mem;
    mem.setIIEMode(true);
    poison(mem);
    // INTC8ROM latches on $C3xx access regardless of INTCXROM (UTAIIe
    // 5-28; MAME: the $C300 page of the INTCXROM-on view still runs
    // c300_int_r, whose only condition is !m_slotc3rom). Sequence that
    // used to break: read $C3xx with INTCXROM on, drop INTCXROM, then
    // read $C8xx — must still be internal ROM, not slot bus.
    mem.memWrite(0xC007, 0);          // INTCXROM on
    (void)mem.memRead(0xC300);
    mem.memWrite(0xC006, 0);          // INTCXROM off
    assert(mem.memRead(0xC9AB) == 0x77 &&
           "C3xx under INTCXROM=on must still arm INTC8ROM");
}

void testWritePathMirrorsReadSide()
{
    Memory mem;
    mem.setIIEMode(true);
    poison(mem);
    // A WRITE to $C3xx with SLOTC3ROM=off is claimed by the motherboard
    // (slot 3's I/O SELECT never asserts) and arms INTC8ROM exactly like
    // a read — MAME's write view runs the same c300 internal handler.
    mem.memWrite(0xC3C0, 0x00);
    assert(mem.memRead(0xC800) == 0x42 &&
           "C3xx WRITE must arm INTC8ROM (motherboard claims the page)");
    // While INTC8ROM owns $C800-$CFFF, writes there are absorbed by ROM
    // (must not crash / must not reach a slot card) and a $CFFF WRITE
    // releases the window like the read side.
    mem.memWrite(0xC9AB, 0x13);
    assert(mem.memRead(0xC9AB) == 0x77 && "C8xx write absorbed by ROM");
    mem.memWrite(0xCFFF, 0x00);
    assert(mem.memRead(0xC9AB) != 0x77 &&
           "CFFF WRITE must clear INTC8ROM (address decides, not direction)");
}

// Bug hunt #8: the WRITE path's INTCXROM branch returned before the $C3xx
// case, so `STA $C3xx` with INTCXROM on left INTC8ROM clear where the read
// path (and the flip-flop, UTAIIe 5-28) latch it. The only read/write
// asymmetry in the whole $C100-$CFFF window.
void testWriteToC3xxUnderIntcxromArmsIntC8Rom()
{
    Memory mem;
    mem.setIIEMode(true);
    poison(mem);
    mem.memWrite(0xC007, 0);          // INTCXROM on
    mem.memWrite(0xC3AB, 0x00);       // a WRITE, not a read
    mem.memWrite(0xC006, 0);          // INTCXROM off
    assert(mem.memRead(0xC800) == 0x42 &&
           "C3xx WRITE under INTCXROM=on must arm INTC8ROM like the read");
}

}  // namespace

int main()
{
    testWriteToC3xxUnderIntcxromArmsIntC8Rom();
    testInitiallyOffSoSlotBusOwns();
    testC3xxAutoEnablesIntC8Rom();
    testCfffClearsIntC8Rom();
    testSlotC3RomDisablesAutoEnable();
    testIntcxromSoftSwitchUnchanged();
    testIntcxromOnStillArmsIntC8Rom();
    testWritePathMirrorsReadSide();
    std::printf("iie_c8xx_smoke OK\n");
    return 0;
}
