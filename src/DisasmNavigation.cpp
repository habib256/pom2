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

#include "DisasmNavigation.h"

#include "Disassembler6502.h"

#include <array>

namespace pom2 {

namespace {

int lengthAt(const uint8_t* mem, uint16_t addr, Cpu6502Set set)
{
    int len = 1;
    (void)disassemble6502(mem, addr, len, set);
    return len < 1 ? 1 : len;
}

// How far back a chain may start. Sixteen bytes is five to sixteen
// instructions: enough for the chains to converge on real code, short enough
// that a scroll stays cheap (one decode per byte per start, per notch).
constexpr int kWindow = 16;

} // namespace

uint16_t disasmNext(const uint8_t* mem, uint16_t addr, Cpu6502Set set)
{
    return static_cast<uint16_t>(addr + lengthAt(mem, addr, set));
}

uint16_t disasmPrevious(const uint8_t* mem, uint16_t addr, Cpu6502Set set)
{
    // votes[k] counts the chains whose last instruction before `addr` starts
    // at addr - k (k = 1..3, the only lengths an instruction can have).
    std::array<int, 4> votes{};
    for (int back = kWindow; back >= 1; --back) {
        uint16_t cur  = static_cast<uint16_t>(addr - back);
        int      left = back;              // bytes between cur and addr
        int      prev = 0;                 // distance of the last start
        while (left > 0) {
            const int len = lengthAt(mem, cur, set);
            prev = left;
            left -= len;
            cur   = static_cast<uint16_t>(cur + len);
        }
        if (left == 0 && prev >= 1 && prev <= 3) ++votes[static_cast<std::size_t>(prev)];
    }
    int best = 0;
    for (int k = 1; k <= 3; ++k)
        if (votes[static_cast<std::size_t>(k)] > votes[static_cast<std::size_t>(best)]) best = k;
    return static_cast<uint16_t>(addr - (best == 0 ? 1 : best));
}

uint16_t disasmStep(const uint8_t* mem, uint16_t addr, int count, Cpu6502Set set)
{
    for (; count > 0; --count) addr = disasmNext(mem, addr, set);
    for (; count < 0; ++count) addr = disasmPrevious(mem, addr, set);
    return addr;
}

} // namespace pom2
