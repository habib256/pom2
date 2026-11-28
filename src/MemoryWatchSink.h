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

// MemoryWatchSink — where Memory reports a watched access.
//
// An interface rather than a direct call into `pom2::Debugger` for one
// practical reason: `Memory.cpp` is linked into two dozen test binaries and a
// benchmark, and none of them should have to pull the debugger in behind it.
// Memory holds a pointer that is null in every one of those builds, and the
// only implementor is `Debugger` (see Memory.h § Write watchpoints).

#ifndef POM2_MEMORY_WATCH_SINK_H
#define POM2_MEMORY_WATCH_SINK_H

#include <cstdint>
#include "DebugAddress.h"

namespace pom2 {

struct MemoryWatchSink {
    virtual ~MemoryWatchSink() = default;
    /// One access to a watched address, including opcode fetches.
    virtual void noteAccess(uint16_t addr, uint8_t value, bool write) = 0;
    virtual void noteBankAccess(uint16_t addr, uint8_t value, bool write, DebugBank) {
        noteAccess(addr, value, write);
    }
};

/// Where Memory reports EVERY access while the memory heatmap is open
/// (`Memory::setHeatSink`). Same reason for an interface: Memory and the
/// Debugger link into builds that have no heatmap. The implementor is
/// `pom2::MemoryHeatmap`.
struct MemoryHeatSink {
    virtual ~MemoryHeatSink() = default;
    /// A bus read or write, after it happened, with the bank it went to.
    virtual void noteHeatRead (uint16_t addr, DebugBank bank) = 0;
    virtual void noteHeatWrite(uint16_t addr, DebugBank bank) = 0;
    /// Before the instruction at `pc` runs (the Debugger's per-instruction
    /// hook): its bytes are execution, not data reads. `bank` is where the
    /// fetch goes (the Debugger has just resolved it for its own use).
    virtual void noteHeatExec(uint16_t pc, DebugBank bank) = 0;
    /// A card's processor holds the bus (SoftCard Z80 DMA, between the two
    /// calls): its accesses are the card's, not the parked 6502's.
    virtual void noteHeatBusMaster(bool /*card*/) {}
    /// An instruction retired (`spAfter` = SP once it has) / an interrupt
    /// entered (SP after the pushes), in the video frame `frame` — the
    /// heatmap's interrupt-load strip (InterruptLoad.h).
    virtual void noteHeatRetired(uint8_t /*spAfter*/, int /*cycles*/, uint64_t /*frame*/) {}
    virtual void noteHeatInterrupt(uint8_t /*spAfter*/, int /*cycles*/, uint64_t /*frame*/) {}
};

}  // namespace pom2

#endif  // POM2_MEMORY_WATCH_SINK_H
