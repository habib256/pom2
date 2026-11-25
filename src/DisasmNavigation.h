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

// DisasmNavigation — moving through a 6502 listing one instruction at a time.
//
// Forward is exact: an instruction's length is in its opcode. Backward is not:
// 6502 code is variable length (1-3 bytes) and the bytes before an address do
// not say where their instruction began. So the step back is a VOTE, the way
// disassemblers have resynchronised since the 1980s: decode forward from every
// start in a short window before the target, keep the chains that land exactly
// on it, and take the predecessor most of them agree on. Real code
// resynchronises within a few instructions, so the vote is right on code; on
// data it is a guess, and it says nothing more than that.
//
// Pure and lock-free: it reads a 64 KiB copy (the debugger's snapshot), never
// the live machine. Pinned by `disasm_navigation`.

#ifndef POM2_DISASM_NAVIGATION_H
#define POM2_DISASM_NAVIGATION_H

#include "Opcode6502.h"

#include <cstdint>

namespace pom2 {

/// Address of the instruction after the one at `addr`.
uint16_t disasmNext(const uint8_t* mem64k, uint16_t addr, Cpu6502Set set);

/// Most likely start of the instruction that ENDS at `addr` (the vote above).
/// Falls back to `addr - 1` when no chain lands on `addr`.
uint16_t disasmPrevious(const uint8_t* mem64k, uint16_t addr, Cpu6502Set set);

/// `count` instructions forward (count > 0) or backward (count < 0).
uint16_t disasmStep(const uint8_t* mem64k, uint16_t addr, int count,
                    Cpu6502Set set);

} // namespace pom2

#endif // POM2_DISASM_NAVIGATION_H
