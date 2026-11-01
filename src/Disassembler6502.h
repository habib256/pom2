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

// Disassembler6502 — single-instruction MOS 6502 disassembler used by the
// debug console. Stateless free function: feed it a pointer to a 64 KB
// memory snapshot and a program counter, get back a printable mnemonic
// (e.g. "LDA #$42", "JMP $FF00", "BNE $0312") plus the instruction length.

#ifndef DISASSEMBLER6502_H
#define DISASSEMBLER6502_H

#include "Opcode6502.h"

#include <cstdint>
#include <string>

namespace pom2 {

/// Disassemble the 6502 instruction at `mem[pc]`. `mem` must point to a
/// contiguous 64 KB region (uses `(pc + N) & 0xFFFF` for operand fetches).
/// Writes the instruction byte length (1/2/3) into `instrLen` and returns
/// the formatted mnemonic. When `cmos` is true, decodes the 65C02
/// (Rockwell/WDC) extensions (STZ/BRA/PHX/…, the 3-byte BBR/BBS bit-branch
/// ops, etc.) instead of rendering them as "???" — pass the live CPU mode so
/// the Disasm panel doesn't desync on 65C02 code.
std::string disassemble6502(const uint8_t* mem, uint16_t pc, int& instrLen,
                            bool cmos = false);
/// Same, decoding the instruction set `set` — the debugger passes
/// `M6502::instructionSet()`, so an NCR65C02's $x7/$xF/$CB/$DB come out as
/// the one-byte NOPs the core executes, not Rockwell/WDC instructions.
std::string disassemble6502(const uint8_t* mem, uint16_t pc, int& instrLen,
                            Cpu6502Set set);

} // namespace pom2

#endif // DISASSEMBLER6502_H
