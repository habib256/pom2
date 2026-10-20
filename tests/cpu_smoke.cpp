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

// POM2 — quick CPU + Memory + soft-switch smoke test.
// Run from POM2 root:
//   cd build && cmake .. && make cpu_smoke && ./tests/cpu_smoke
// Diagnostic dump, not a test: it prints and asserts nothing, so it is
// EXCLUDE_FROM_ALL and outside ctest. The gating equivalents are
// klaus_6502_functional and tomharte_6502.

#include "M6502.h"
#include "Memory.h"
#include <cassert>
#include <cstdio>

int main()
{
    Memory mem;
    M6502  cpu(&mem);

    // Stuff a tiny program at $0300 that writes 'A' to the top-left of the
    // text screen, toggles the speaker once, then infinite-loops:
    //   $0300 LDA #$C1     A9 C1     ; 'A' in the firmware's screen encoding
    //   $0302 STA $0400    8D 00 04
    //   $0305 LDA $C030    AD 30 C0  ; speaker click
    //   $0308 JMP $0308    4C 08 03
    const uint8_t program[] = {
        0xA9, 0xC1,             // $0300 LDA #$C1
        0x8D, 0x00, 0x04,       // $0302 STA $0400
        0xAD, 0x30, 0xC0,       // $0305 LDA $C030
        0x4C, 0x08, 0x03,       // $0308 JMP $0308
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(0x300 + i), program[i]);

    // Force PC straight at $0300 (skip the boot-time reset vector).
    cpu.setProgramCounter(0x0300);

    for (int i = 0; i < 100; ++i) {
        cpu.step();
    }

    const uint8_t cellByte = mem.peekMainRam(0x400);
    const auto    speaker  = mem.getSpeakerToggleCount();

    std::printf("text[$0400] = $%02X (expected $C1)\n", cellByte);
    std::printf("speaker toggles = %llu (expected >= 1)\n",
                (unsigned long long)speaker);
    std::printf("PC after %d steps = $%04X\n", 100, cpu.getProgramCounter());

    assert(cellByte == 0xC1 && "text screen write failed");
    assert(speaker >= 1     && "speaker click missed");
    assert(cpu.getProgramCounter() == 0x0308 && "should be parked in JMP loop");

    std::printf("OK\n");
    return 0;
}
