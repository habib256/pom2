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

// InstructionTraceSink — what the Debugger tells an instruction tracer
// (UI-017). An interface, so Debugger.cpp links no tracer: every binary that
// carries the debugger keeps its link line, and only the ones that write a
// trace link InstructionTrace.cpp.
//
// Order, per instruction: `before` at the boundary (the PC, its physical
// bank, the registers going IN), then `after` once it executed (its
// cycles). An instruction a breakpoint stops is announced by `before` and
// never completed; an interrupt taken instead of it reports `interrupted`.

#ifndef POM2_INSTRUCTION_TRACE_SINK_H
#define POM2_INSTRUCTION_TRACE_SINK_H

#include "DebugAddress.h"

#include <cstdint>

class M6502;

namespace pom2 {

class InstructionTraceSink
{
public:
    virtual ~InstructionTraceSink() = default;
    virtual void before(uint16_t pc, DebugBank bank) = 0;
    virtual void after(const M6502& cpu, int cycles) = 0;
    virtual void interrupted(const M6502& cpu, int cycles) = 0;
};

} // namespace pom2

#endif // POM2_INSTRUCTION_TRACE_SINK_H
