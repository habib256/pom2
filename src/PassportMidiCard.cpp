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

// Passport MIDI card — see PassportMidiCard.h.

#include "PassportMidiCard.h"

#include <cmath>
#include <cstring>

namespace pom2 {

namespace {
constexpr char kMagic[4] = { 'P', 'M', 'D', '1' };
}

PassportMidiCard::PassportMidiCard(int /*slot*/)
{
    acia_.setTiming(busHz_ / kAciaClockHz);
    onPowerCycle();
}

void PassportMidiCard::setStandardClock(double hz)
{
    if (hz <= 0.0) return;
    busClock_.setBusClock(hz);
    busHz_ = hz;
    acia_.setTiming(busHz_ / kAciaClockHz);
}

// Run both chips up to `cpuCycle`. The PTM counts whole bus ticks (its E
// clock); the ACIA keeps a fractional bus time, and each character it
// finishes is stamped back on the CPU-cycle clock: the cycle `cpuCycle`
// minus how far before the end of this span its stop bit ended.
void PassportMidiCard::syncTo(uint64_t cpuCycle)
{
    if (cpuCycle <= synced_) return;
    const uint64_t span = cpuCycle - synced_;
    const uint64_t ticks = busClock_.busTicks(span);
    synced_ = cpuCycle;
    ptm_.tick(ticks);
    sent_.clear();
    acia_.tick(double(ticks), sent_);
    busAtSync_ = acia_.now();
    if (sent_.empty()) return;
    const double cpuPerBus = ticks ? double(span) / double(ticks) : 1.0;
    for (const auto& s : sent_) {
        if (!s.framed) continue;                 // the far end drops it
        const double behind = (busAtSync_ - s.at) * cpuPerBus;
        const uint64_t back = behind > 0.0 ? uint64_t(std::llround(behind)) : 0;
        if (pending_.size() == kMaxPending) { pending_.pop_front(); ++dropped_; }
        pending_.push_back({ cpuCycle > back ? cpuCycle - back : 0, s.byte });
    }
}

void PassportMidiCard::updateIrq()
{
    assertIrq(ptm_.irq() || acia_.irq());
}

void PassportMidiCard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    cycles_ += uint64_t(cycles);
    syncTo(cycles_);
    updateIrq();
}

uint8_t PassportMidiCard::deviceSelectRead(uint8_t low4)
{
    syncTo(cycles_ + busAccessOffset());
    uint8_t v = 0;
    if (low4 < 8) v = ptm_.read(low4);
    else if (low4 == 8) v = acia_.status();
    else if (low4 == 9) v = acia_.readData();
    updateIrq();
    return v;
}

void PassportMidiCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    syncTo(cycles_ + busAccessOffset());
    if (low4 < 8) ptm_.write(low4, v);
    else if (low4 == 8) acia_.writeControl(v);
    else if (low4 == 9) acia_.writeData(v);
    updateIrq();
}

void PassportMidiCard::receiveMidi(uint8_t b)
{
    acia_.receive(b);
    updateIrq();
}

void PassportMidiCard::drainMidiOut(std::vector<MidiOutByte>& out)
{
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

// Ctrl-Reset: the PTM's RESET pin is the bus's; the ACIA has none.
void PassportMidiCard::onReset()
{
    ptm_.reset();
    updateIrq();
}

void PassportMidiCard::onPowerCycle()
{
    ptm_.reset();
    acia_.powerOn();
    updateIrq();
}

void PassportMidiCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    ptm_.appendState(out);
    acia_.appendState(out);
}

// The card's own cycle clock is not restored: it only stamps output, and it
// must keep running forward for the host that schedules by it.
void PassportMidiCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (!data || len < 4 || std::memcmp(data, kMagic, 4) != 0) return;
    constexpr std::size_t ptmLen = 4 + Mc6840Ptm::kStateBytes;
    if (len < 4 + ptmLen) return;
    Mc6840Ptm ptm;
    Mc6850Acia acia;
    acia.setTiming(busHz_ / kAciaClockHz);
    if (!ptm.loadState(data + 4, len - 4)) return;
    if (!acia.loadState(data + 4 + ptmLen, len - 4 - ptmLen)) return;
    ptm_ = ptm;
    acia_ = acia;
    synced_ = cycles_;
    busAtSync_ = acia_.now();
    updateIrq();
}

} // namespace pom2
