// NMOS indexed-access dummy READ — bug hunt #8.
//
// A 6502 adds the index to the low byte first and performs a real bus READ
// at that un-fixed address before correcting the high byte: on every
// STA abs,X / STA abs,Y / STA (zp),Y / RMW abs,X, and on a page-crossing
// LDA abs,X / abs,Y / (zp),Y (SingleStepTests/65x02 `6502/v1` bus traces).
// On an Apple II that address is often a soft switch, so `STA $C030,X`
// clicks the speaker twice on a ][ / ][+ / unenhanced //e, and a
// page-crossing `LDA $C0F0,X` (un-fixed $C030, fixed $C130) clicks it once
// on its way past. The 65C02 re-reads the last operand byte instead and
// never touches the data address. Cycle counts were already right, so the
// speaker toggle counter is the bus witness here, and the cycle totals are
// asserted unchanged so the pin guards the timing too.

#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

struct Run { uint64_t toggles; int cycles; };

Run run(M6502::CpuMode mode, std::initializer_list<uint8_t> program, int instrs)
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    cpu.setCpuMode(mode);
    mem.memWrite(0x10, 0x30);          // ($10) -> $C030
    mem.memWrite(0x11, 0xC0);
    uint16_t a = 0x0200;
    for (uint8_t b : program) mem.memWrite(a++, b);
    cpu.setProgramCounter(0x0200);
    const uint64_t before = mem.getSpeakerToggleCount();
    int cycles = 0;
    for (int i = 0; i < instrs; ++i) cycles += cpu.run(1);
    return { mem.getSpeakerToggleCount() - before, cycles };
}

void check(const char* what, M6502::CpuMode mode, std::initializer_list<uint8_t> program,
           int instrs, uint64_t wantToggles, int wantCycles)
{
    const Run r = run(mode, program, instrs);
    std::printf("  %-28s %-5s toggles=%llu (want %llu) cycles=%d (want %d)\n", what,
                mode == M6502::CpuMode::NMOS ? "NMOS" : "65C02",
                static_cast<unsigned long long>(r.toggles),
                static_cast<unsigned long long>(wantToggles), r.cycles, wantCycles);
    assert(r.toggles == wantToggles && "the indexed dummy read is missing or extra");
    assert(r.cycles == wantCycles && "the dummy read must not change the cycle count");
}

}  // namespace

int main()
{
    const auto NMOS = M6502::CpuMode::NMOS;
    const auto CMOS = M6502::CpuMode::CMOS;
    // LDX #0 : STA $C030,X — dummy read at the target itself on NMOS.
    check("LDX #0 : STA $C030,X", NMOS, {0xA2, 0x00, 0x9D, 0x30, 0xC0}, 2, 2, 2 + 5);
    check("LDX #0 : STA $C030,X", CMOS, {0xA2, 0x00, 0x9D, 0x30, 0xC0}, 2, 1, 2 + 5);
    // LDY #0 : STA ($10),Y — same, through the zero-page pointer.
    check("LDY #0 : STA ($10),Y", NMOS, {0xA0, 0x00, 0x91, 0x10}, 2, 2, 2 + 6);
    check("LDY #0 : STA ($10),Y", CMOS, {0xA0, 0x00, 0x91, 0x10}, 2, 1, 2 + 6);
    // LDX #$40 : LDA $C0F0,X — page cross: the un-fixed address is $C030,
    // the fixed one $C130 (slot ROM, no side effect).
    check("LDX #$40 : LDA $C0F0,X", NMOS, {0xA2, 0x40, 0xBD, 0xF0, 0xC0}, 2, 1, 2 + 5);
    check("LDX #$40 : LDA $C0F0,X", CMOS, {0xA2, 0x40, 0xBD, 0xF0, 0xC0}, 2, 0, 2 + 5);
    // LDX #0 : LDA $C030,X — no page cross: no dummy read on either core.
    check("LDX #0 : LDA $C030,X", NMOS, {0xA2, 0x00, 0xBD, 0x30, 0xC0}, 2, 1, 2 + 4);
    // Plain LDA $C030 — control, one access on both.
    check("LDA $C030", NMOS, {0xAD, 0x30, 0xC0}, 1, 1, 4);
    check("LDA $C030", CMOS, {0xAD, 0x30, 0xC0}, 1, 1, 4);
    std::printf("cpu_nmos_index_dummy_read OK\n");
    return 0;
}
