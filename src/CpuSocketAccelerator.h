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

// CpuSocketAccelerator — the seam between Memory and an accelerator that
// sits in the 6502 SOCKET rather than in a slot (the Zip Chip). A slot
// accelerator (TransWarp) is a SlotPeripheral and reaches the bus through
// SlotBus's snoop; a socket one has no slot, so Memory calls it directly.
//
// Header-only and abstract on purpose: Memory.cpp is linked by dozens of
// test targets, and calling through this interface keeps every one of them
// free of a link dependency on the implementation (same reason the clock
// fan-out in EmulationController goes through SlotPeripheral virtuals).
//
// Every hook is on the $C000-$C0FF I/O path, which is already the slow
// path (memReadSlowBody / memWrite above $C000); nothing here touches the
// RAM fast path (docs/PERFORMANCE.md §§ 8.2/8.5).

#ifndef POM2_CPU_SOCKET_ACCELERATOR_H
#define POM2_CPU_SOCKET_ACCELERATOR_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

class CpuSocketAccelerator
{
public:
    virtual ~CpuSocketAccelerator() = default;

    /// What a $C0xx READ should do.
    struct ReadResult {
        bool    answer = false;          ///< the chip drives the data bus
        bool    skipMotherboard = false; ///< and the motherboard switch does NOT act
        uint8_t value = 0;
    };

    /// A read of $C000+`low` at CPU cycle `now`. `stateByte` is the
    /// STATEREG-like summary of the //e paging switches, only meaningful
    /// for low == $5E (Memory computes it only then).
    virtual ReadResult ioRead(uint8_t low, uint64_t now, uint8_t stateByte) = 0;

    /// A write of `value` to $C000+`low`. Writes always reach the
    /// motherboard as well (MAME calls `do_io` on every one).
    virtual void ioWrite(uint8_t low, uint8_t value, uint64_t now) = 0;

    /// The 6502 clock multiplier at `now` (1.0 = stock).
    virtual double cpuSpeedMultiplier(uint64_t now) const = 0;

    /// The machine's reset line.
    virtual void onReset(uint64_t now) = 0;

    virtual void appendSnapshotState(std::vector<uint8_t>& out) const = 0;
    virtual bool loadSnapshotState(const uint8_t* data, std::size_t len) = 0;
};

} // namespace pom2

#endif // POM2_CPU_SOCKET_ACCELERATOR_H
