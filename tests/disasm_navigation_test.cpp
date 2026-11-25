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

// Pins the two pure helpers behind the debugger's 2026-11-25 additions
// (French Touch's requests): the instruction-by-instruction walk the mouse
// wheel uses (DisasmNavigation.h) and the beam H/V readout (BeamPosition.h).

#include "BeamPosition.h"
#include "DisasmNavigation.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

void testWalk()
{
    using pom2::Cpu6502Set;
    std::vector<uint8_t> mem(0x10000, 0xEA);   // NOP everywhere: 1 byte each
    auto put = [&](uint16_t a, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) mem[a++] = b;
    };
    // $0300 LDA #$01 / $0302 STA $C000 / $0305 NOP / $0306 JMP $0300
    put(0x0300, {0xA9, 0x01, 0x8D, 0x00, 0xC0, 0xEA, 0x4C, 0x00, 0x03});
    const uint8_t* m = mem.data();
    const auto set = Cpu6502Set::Nmos6502;

    check(pom2::disasmNext(m, 0x0300, set) == 0x0302, "next over a 2-byte LDA #");
    check(pom2::disasmNext(m, 0x0302, set) == 0x0305, "next over a 3-byte STA abs");
    check(pom2::disasmStep(m, 0x0300, 4, set) == 0x0309, "four forward");

    check(pom2::disasmPrevious(m, 0x0306, set) == 0x0305, "back over a 1-byte NOP");
    check(pom2::disasmPrevious(m, 0x0305, set) == 0x0302, "back over a 3-byte STA");
    check(pom2::disasmPrevious(m, 0x0302, set) == 0x0300, "back over a 2-byte LDA");
    check(pom2::disasmPrevious(m, 0x0300, set) == 0x02FF, "back into the NOP run");
    check(pom2::disasmStep(m, 0x0309, -4, set) == 0x0300, "four back = four forward undone");

    // Wraps at the top of the address space like the CPU's PC does.
    check(pom2::disasmPrevious(m, 0x0000, set) == 0xFFFF, "back across $0000");
    check(pom2::disasmNext(m, 0xFFFF, set) == 0x0000, "forward across $FFFF");

    // The vote, not the first chain: a JSR before the code is three bytes,
    // and stepping back from the code must land on its opcode, not inside
    // its operand.
    put(0x02FD, {0x20, 0x00, 0x03});            // JSR $0300
    check(pom2::disasmPrevious(m, 0x0300, set) == 0x02FD, "back over a 3-byte JSR");
}

void testBeam()
{
    auto at = [](uint64_t c, VideoStandard s) { return pom2::beamPositionAt(c, s); };

    auto b = at(0, VideoStandard::NTSC);
    check(b.line == 0 && b.h == 0 && b.hbl && !b.vbl && b.column == -1, "top of frame is HBL");
    b = at(24, VideoStandard::NTSC);
    check(b.hbl && b.column == -1, "cycle 24 still HBL");
    b = at(25, VideoStandard::NTSC);
    check(!b.hbl && b.column == 0, "cycle 25 fetches byte 0");
    b = at(64, VideoStandard::NTSC);
    check(b.column == 39 && b.line == 0, "cycle 64 fetches byte 39");
    b = at(65, VideoStandard::NTSC);
    check(b.line == 1 && b.h == 0, "65 cycles a line");

    b = at(65ull * 191 + 64, VideoStandard::NTSC);
    check(b.line == 191 && !b.vbl && b.column == 39, "last visible byte");
    b = at(65ull * 192, VideoStandard::NTSC);
    check(b.line == 192 && b.vbl && b.column == -1, "line 192 is VBL");

    // NTSC wraps at 262 lines, PAL runs on to 312.
    b = at(65ull * 262, VideoStandard::NTSC);
    check(b.line == 0 && b.frameCycle == 0, "NTSC frame = 262 lines");
    b = at(65ull * 262, VideoStandard::PAL);
    check(b.line == 262 && b.vbl, "PAL line 262 is still VBL");
    b = at(65ull * 312 + 30, VideoStandard::PAL);
    check(b.line == 0 && b.h == 30 && b.column == 5, "PAL frame = 312 lines");
}

} // namespace

int main()
{
    testWalk();
    testBeam();
    if (g_failures) return 1;
    std::puts("disasm_navigation: OK");
    return 0;
}
