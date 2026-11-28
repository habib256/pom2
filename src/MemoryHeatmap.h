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

// MemoryHeatmap — the recording behind Tools ▸ Memory Heatmap (UI-022).
//
// For every PHYSICAL byte of the machine — main RAM, the motherboard language
// card, the ROM and I/O space, the //e's aux 64 KB and every RamWorks bank —
// when it was last read, written, executed and reached by a card's DMA, how
// many times, and by which instruction. Asked for by GROUiK (French Touch) on
// 2026-11-27; the tool is fenarinarsa's (Cyril Lambin), written into French
// Touch's AppleWin fork in 2020: github.com/Fr3nchT0uch/AppleWin, branch
// `extended_debugger` (v1.29.15v100FT), source/Debugger/Debugger_Heatmap.cpp
// and source/CPU/cpu_heatmap.inl. Upstream AppleWin carries only the empty
// stubs of cpu_heatmap.inl ("// todo"), never the tool.
//
// ── Planes ─────────────────────────────────────────────────────────────────
// A plane is one 64 KB grid, indexed by PHYSICAL offset, in the layout French
// Touch's map uses and POM2's own Memory::lcPhysical / auxPhysical already
// keep (the //e MMU's language-card layout, Memory.h):
//
//   kMain      $0000-$BFFF main RAM · $C000-$CFFF the language card's $D000
//              BANK 1 · $D000-$DFFF its $D000 bank 2 · $E000-$FFFF its $E000
//   kAux0 + n  the same for aux bank n (n = the RamWorks backing slot; 0 is
//              the //e's own aux 64 KB, the bank the video reads)
//   kRom       the firmware and I/O space by CPU address, $C000-$FFFF only:
//              soft switches, slot I/O, slot / internal / expansion ROM, the
//              BASIC and Monitor ROM (and a card holding INH, a Saturn)
//
// So the two $D000 banks are apart, as in AppleWin FT (which keeps bank 1 at
// $C000 too), and nothing the 6502 can reach goes unrecorded.
//
// ── Feeding and cost ───────────────────────────────────────────────────────
// Memory reports every read and write (`Memory::setHeatSink`), the Debugger
// every instruction before it runs, every retirement and interrupt entry (the
// interrupt-load strip), and the span a card's processor holds the bus — all
// on the CPU thread under stateMutex. The window copies what it draws under
// the same lock. Not attached, it costs nothing (Memory.h § setHeatSink).
// RamWorks planes are allocated on their first access.
//
// A write to $C066-$C069 is also handed to the CycleMonitor: French Touch's
// "Monitoring Card" markers, timed at the write's own cycle (CycleMonitor.h).
//
// Time is the CPU's cycle counter in units of 256 cycles, so a fade reads the
// same at 0.1 % and at x4 (AppleWin FT fades by instructions executed: a
// 0x1FFFF-instruction horizon). A clock that goes BACK (rewind, snapshot)
// clears the map: stamps from the abandoned future would read as negative.

#ifndef POM2_MEMORY_HEATMAP_H
#define POM2_MEMORY_HEATMAP_H

#include "CycleMonitor.h"
#include "DebugAddress.h"
#include "InterruptLoad.h"
#include "MemoryWatchSink.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class Memory;
class M6502;

namespace pom2 {

/// What MemoryHeatmap records, without the sink around it: copyable, so the
/// window can freeze a whole picture of it.
class HeatData {
public:
    enum Plane : int { kMain = 0, kRom = 1, kAux0 = 2 };
    static constexpr int kMaxAuxBanks = 128;              // RamWorks III, 8 MB
    static constexpr int kPlanes = kAux0 + kMaxAuxBanks;
    static int auxPlane(int slot) { return kAux0 + slot; }
    static bool isAux(int plane) { return plane >= kAux0; }

    enum Kind : int { Read = 0, Write = 1, Exec = 2, Dma = 3 };
    static constexpr int kKinds = 4;
    static constexpr int kStampShift = 8;   // a stamp = (cycles >> 8) + 1; 0 = never

    /// Where an access lands: a plane and a physical offset in it.
    struct Cell { int plane = -1; uint16_t phys = 0; };
    /// The access to `addr`, in `bank` (Memory::debugBank's answer).
    static Cell locate(uint16_t addr, DebugBank bank);
    /// The other way: the CPU address and bank a physical byte answers at —
    /// for a breakpoint or the listing. The ROM plane's bank is "any" (a //c
    /// ROM bank, a slot ROM, I/O: the address alone is what a program uses).
    static DebugAddress cpuAddress(int plane, uint16_t phys);

    struct PlaneData {
        std::array<std::vector<uint32_t>, kKinds> stamp;   // 0 = never
        std::array<std::vector<uint32_t>, kKinds> count;   // saturating
        // Who made the last read / write: the instruction's PC and bank, and
        // the bank the access went to (packBank).
        std::vector<uint16_t> readPc, writePc, readPcBank, writePcBank, readBank, writeBank;
        PlaneData();
    };

    HeatData() = default;
    HeatData(const HeatData& o);
    HeatData& operator=(const HeatData& o);
    HeatData(HeatData&&) noexcept = default;
    HeatData& operator=(HeatData&&) noexcept = default;

    /// Forget everything recorded (keeps the allocated planes).
    void clear();
    bool hasPlane(int plane) const {
        return plane >= 0 && plane < kPlanes && planes_[static_cast<std::size_t>(plane)] != nullptr;
    }
    const PlaneData* plane(int plane) const {
        return hasPlane(plane) ? planes_[static_cast<std::size_t>(plane)].get() : nullptr;
    }
    PlaneData& planeForWrite(int plane);

    /// Everything recorded about one byte: the hover / the inspector.
    struct ByteInfo {
        std::array<uint32_t, kKinds> stamp{};
        std::array<uint32_t, kKinds> count{};
        uint16_t readPc = 0, writePc = 0;
        DebugBank readPcBank{}, writePcBank{}, readBank{}, writeBank{};
    };
    ByteInfo byteInfo(int plane, uint16_t phys) const;

    /// One plane's stamps or counts over [lo, hi], for drawing. `out` is
    /// resized to 64 K entries per kind; entries outside the span are 0.
    struct PlaneCopy {
        std::array<std::vector<uint32_t>, kKinds> v;
        bool counts = false;
    };
    void copyPlane(int plane, bool counts, uint32_t lo, uint32_t hi, PlaneCopy& out) const;

    /// The interrupt load of the frames recorded, measured by POM2 from each
    /// handler's entry to its RTI (InterruptLoad.h) — no marker needed.
    const InterruptLoad& interruptLoad() const { return irq_; }
    /// French Touch's monitoring marks ($C066-$C069) seen while recording:
    /// AppleWin FT's CPU and IRQ bars (CycleMonitor.h).
    const CycleMonitor& cycleMonitor() const { return monitor_; }

    static uint16_t packBank(DebugBank b) {
        return static_cast<uint16_t>((b.kind & 0x0F) | ((b.segment & 0x0F) << 4) |
                                     (b.index << 8));
    }
    static DebugBank unpackBank(uint16_t v) {
        DebugBank b;
        b.kind = static_cast<DebugBank::Kind>(v & 0x0F);
        b.segment = static_cast<uint8_t>((v >> 4) & 0x0F);
        b.index = static_cast<uint8_t>(v >> 8);
        return b;
    }

protected:
    std::array<std::unique_ptr<PlaneData>, kPlanes> planes_{};
    InterruptLoad irq_;
    CycleMonitor monitor_;
};

class MemoryHeatmap final : public HeatData, public MemoryHeatSink {
public:
    // Kept for the callers of phase 1 (tests, bench).
    static constexpr int Main = kMain;
    static constexpr int Aux = kAux0;

    MemoryHeatmap(const Memory& mem, const M6502& cpu);

    // MemoryHeatSink — CPU thread, under stateMutex.
    void noteHeatRead (uint16_t addr, DebugBank bank) override;
    void noteHeatWrite(uint16_t addr, DebugBank bank) override;
    void noteHeatExec (uint16_t pc, DebugBank bank) override;
    void noteHeatBusMaster(bool card) override { busMaster_ = card; codeLen_ = 0; }
    void noteHeatRetired(uint8_t spAfter, int cycles, uint64_t frame) override;
    void noteHeatInterrupt(uint8_t spAfter, int cycles, uint64_t frame) override;

    /// Forget everything recorded (under stateMutex).
    void clear();
    /// Forget the monitoring marks' figures only (under stateMutex).
    void resetCycleMonitor() { monitor_.reset(); }
    /// The stamp of "now" (0 is "never").
    uint32_t now() const;
    /// A copy of everything recorded (Freeze).
    HeatData freeze() const { return HeatData(*this); }

private:
    void note(Kind kind, uint16_t addr, DebugBank bank);
    const Memory& mem_;
    const M6502&  cpu_;
    uint32_t lastNow_ = 0;
    uint32_t stamp();
    // The instruction running: its PC and bank (who made the access) and
    // its bytes, whose fetches are execution, not data reads.
    uint16_t pc_ = 0;
    uint16_t pcBank_ = 0;
    uint16_t codeLo_ = 0;
    uint8_t  codeLen_ = 0;
    bool     busMaster_ = false;
};

}  // namespace pom2

#endif  // POM2_MEMORY_HEATMAP_H
