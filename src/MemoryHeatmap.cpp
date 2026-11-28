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

#include "MemoryHeatmap.h"

#include "M6502.h"
#include "Memory.h"
#include "Opcode6502.h"

#include <algorithm>
#include <limits>

namespace pom2 {

namespace {

int instructionLength(AddrMode m)
{
    switch (m) {
    case AM_IMP: case AM_ACC:
        return 1;
    case AM_ABS: case AM_ABX: case AM_ABY: case AM_IND: case AM_IAX: case AM_ZPR:
        return 3;
    default:
        return 2;
    }
}

/// A language-card access at `addr` ($D000-$FFFF) in `segment` (0 = the
/// $E000-$FFFF RAM, 1 / 2 = the $D000 bank), as a physical offset in the
/// lcPhysical layout: bank 1 of $D000 lives at $C000-$CFFF.
int lcPhysical(uint16_t addr, uint8_t segment)
{
    if (addr < 0xD000) return -1;
    if (addr >= 0xE000) return addr;
    return segment == 1 ? addr - 0x1000 : addr;
}

constexpr std::size_t kSize = 0x10000;

}  // namespace

// ── HeatData ───────────────────────────────────────────────────────────────

HeatData::PlaneData::PlaneData()
{
    for (auto& v : stamp) v.assign(kSize, 0);
    for (auto& v : count) v.assign(kSize, 0);
    for (auto* v : {&readPc, &writePc, &readPcBank, &writePcBank, &readBank, &writeBank})
        v->assign(kSize, 0);
}

HeatData::HeatData(const HeatData& o) : irq_(o.irq_), monitor_(o.monitor_)
{
    for (std::size_t i = 0; i < planes_.size(); ++i)
        if (o.planes_[i]) planes_[i] = std::make_unique<PlaneData>(*o.planes_[i]);
}

HeatData& HeatData::operator=(const HeatData& o)
{
    if (this == &o) return *this;
    for (std::size_t i = 0; i < planes_.size(); ++i)
        planes_[i] = o.planes_[i] ? std::make_unique<PlaneData>(*o.planes_[i]) : nullptr;
    irq_ = o.irq_;
    monitor_ = o.monitor_;
    return *this;
}

HeatData::Cell HeatData::locate(uint16_t addr, DebugBank bank)
{
    using B = DebugBank;
    switch (bank.kind) {
    case B::Main:
    case B::Flat:            // a flat bus (test harness): by address
        return addr < 0xC000 ? Cell{kMain, addr} : Cell{kRom, addr};
    case B::Language: {
        const int p = lcPhysical(addr, bank.segment);
        return p < 0 ? Cell{} : Cell{kMain, static_cast<uint16_t>(p)};
    }
    case B::Aux:
        if (addr >= 0xC000 || bank.index >= kMaxAuxBanks) return {};
        return {auxPlane(bank.index), addr};
    case B::AuxLanguage: {
        const int p = lcPhysical(addr, bank.segment);
        if (p < 0 || bank.index >= kMaxAuxBanks) return {};
        return {auxPlane(bank.index), static_cast<uint16_t>(p)};
    }
    case B::Rom:
    case B::Slot:
    case B::Io:
        return addr >= 0xC000 ? Cell{kRom, addr} : Cell{};
    case B::Any:
    case B::Ignored:
        return {};
    }
    return {};
}

DebugAddress HeatData::cpuAddress(int plane, uint16_t phys)
{
    using B = DebugBank;
    if (plane == kRom) return {phys, {}};
    const bool aux = isAux(plane);
    const auto index = static_cast<uint8_t>(aux ? plane - kAux0 : 0);
    if (phys < 0xC000) return {phys, aux ? B{B::Aux, index} : B{B::Main}};
    uint8_t segment = 0;
    uint16_t addr = phys;
    if (phys < 0xD000) { segment = 1; addr = static_cast<uint16_t>(phys + 0x1000); }
    else if (phys < 0xE000) segment = 2;
    return {addr, aux ? B{B::AuxLanguage, index, segment} : B{B::Language, 0, segment}};
}

void HeatData::clear()
{
    for (auto& p : planes_) {
        if (!p) continue;
        for (auto& v : p->stamp) std::fill(v.begin(), v.end(), 0u);
        for (auto& v : p->count) std::fill(v.begin(), v.end(), 0u);
        for (auto* v : {&p->readPc, &p->writePc, &p->readPcBank, &p->writePcBank,
                        &p->readBank, &p->writeBank})
            std::fill(v->begin(), v->end(), uint16_t{0});
    }
    irq_.reset();
    monitor_.reset();
}

HeatData::PlaneData& HeatData::planeForWrite(int plane)
{
    auto& p = planes_[static_cast<std::size_t>(plane)];
    if (!p) p = std::make_unique<PlaneData>();
    return *p;
}

HeatData::ByteInfo HeatData::byteInfo(int plane, uint16_t phys) const
{
    ByteInfo b;
    const PlaneData* d = this->plane(plane);
    if (!d) return b;
    for (int k = 0; k < kKinds; ++k) {
        b.stamp[static_cast<std::size_t>(k)] = d->stamp[static_cast<std::size_t>(k)][phys];
        b.count[static_cast<std::size_t>(k)] = d->count[static_cast<std::size_t>(k)][phys];
    }
    b.readPc = d->readPc[phys];
    b.writePc = d->writePc[phys];
    b.readPcBank = unpackBank(d->readPcBank[phys]);
    b.writePcBank = unpackBank(d->writePcBank[phys]);
    b.readBank = unpackBank(d->readBank[phys]);
    b.writeBank = unpackBank(d->writeBank[phys]);
    return b;
}

void HeatData::copyPlane(int plane, bool counts, uint32_t lo, uint32_t hi,
                         PlaneCopy& out) const
{
    out.counts = counts;
    hi = std::min<uint32_t>(hi, 0xFFFF);
    const PlaneData* d = this->plane(plane);
    for (int k = 0; k < kKinds; ++k) {
        auto& dst = out.v[static_cast<std::size_t>(k)];
        dst.resize(kSize);
        if (lo > hi) continue;
        if (!d) {
            std::fill(dst.begin() + lo, dst.begin() + hi + 1, 0u);
            continue;
        }
        const auto& src = counts ? d->count[static_cast<std::size_t>(k)]
                                 : d->stamp[static_cast<std::size_t>(k)];
        std::copy(src.begin() + lo, src.begin() + hi + 1, dst.begin() + lo);
    }
}

// ── MemoryHeatmap ──────────────────────────────────────────────────────────

MemoryHeatmap::MemoryHeatmap(const Memory& mem, const M6502& cpu)
    : mem_(mem), cpu_(cpu)
{
    // The three planes every machine draws, allocated now (the window builds
    // this with the lock released); RamWorks banks on their first access.
    planeForWrite(kMain);
    planeForWrite(kRom);
    planeForWrite(kAux0);
}

void MemoryHeatmap::clear()
{
    HeatData::clear();
    lastNow_ = 0;
}

uint32_t MemoryHeatmap::now() const
{
    return static_cast<uint32_t>(mem_.getCycleCounter() >> kStampShift) + 1u;
}

uint32_t MemoryHeatmap::stamp()
{
    const uint32_t t = now();
    if (t < lastNow_) clear();
    lastNow_ = t;
    return t;
}

void MemoryHeatmap::note(Kind kind, uint16_t addr, DebugBank bank)
{
    const Cell c = locate(addr, bank);
    if (c.plane < 0) return;
    const uint32_t t = stamp();
    PlaneData& d = planeForWrite(c.plane);
    const auto k = static_cast<std::size_t>(kind);
    d.stamp[k][c.phys] = t;
    uint32_t& n = d.count[k][c.phys];
    if (n != std::numeric_limits<uint32_t>::max()) ++n;
    if (kind == Read) {
        d.readPc[c.phys] = pc_;
        d.readPcBank[c.phys] = pcBank_;
        d.readBank[c.phys] = packBank(bank);
    } else if (kind == Write) {
        d.writePc[c.phys] = pc_;
        d.writePcBank[c.phys] = pcBank_;
        d.writeBank[c.phys] = packBank(bank);
    }
}

void MemoryHeatmap::noteHeatRead(uint16_t addr, DebugBank bank)
{
    if (busMaster_) { note(Dma, addr, bank); return; }
    // The running instruction's own opcode and operand fetches are its
    // execution (noteHeatExec), not data reads.
    if (static_cast<uint16_t>(addr - codeLo_) < codeLen_) return;
    note(Read, addr, bank);
}

void MemoryHeatmap::noteHeatWrite(uint16_t addr, DebugBank bank)
{
    // French Touch's monitoring marks: observed, never acted on (the machine
    // drops the write). Stamped at the write's own bus cycle — the in-flight
    // instruction's elapsed cycles included — so two marks made by the same
    // kind of store differ by exactly the cycles between them.
    // After note(): a clock that went back clears everything there first.
    note(busMaster_ ? Dma : Write, addr, bank);
    if (CycleMonitor::isMark(addr) && !busMaster_)
        monitor_.mark(addr, mem_.accessCycle(), mem_.videoAccessCycle());
}

void MemoryHeatmap::noteHeatExec(uint16_t pc, DebugBank bank)
{
    pc_ = pc;
    pcBank_ = packBank(bank);
    const uint8_t op = mem_.peekCpuView(pc);
    const int len = instructionLength(describeOpcode6502(op, cpu_.instructionSet()).mode);
    codeLo_ = pc;
    codeLen_ = static_cast<uint8_t>(len);
    for (int i = 0; i < len; ++i) {
        const Cell c = locate(static_cast<uint16_t>(pc + i), bank);
        if (c.plane < 0) continue;
        const uint32_t t = stamp();
        PlaneData& d = planeForWrite(c.plane);
        d.stamp[Exec][c.phys] = t;
        uint32_t& n = d.count[Exec][c.phys];
        if (n != std::numeric_limits<uint32_t>::max()) ++n;
    }
}

void MemoryHeatmap::noteHeatRetired(uint8_t spAfter, int cycles, uint64_t frame)
{
    irq_.stepped(spAfter, cycles, frame);
}

void MemoryHeatmap::noteHeatInterrupt(uint8_t spAfter, int cycles, uint64_t frame)
{
    irq_.entered(spAfter, cycles, frame);
}

}  // namespace pom2
