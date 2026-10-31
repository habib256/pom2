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

// PremiumSoftCard implementation — every rule and where it was read is in
// the header.

#include "PremiumSoftCard.h"

#include "Z80StateBlob.h"

#include <cstring>

namespace {
// Snapshot blob: "PSC1", the register, flags (bit0 call pending, bit1 I
// high), the Z80 register file, the 1 KB under the text page. The rest of
// the 64 KB is Memory's aux RAM.
constexpr char kMagic[4] = { 'P', 'S', 'C', '1' };
} // namespace

PremiumSoftCard::PremiumSoftCard() : z80_(*this)
{
    z80_.reset();
}

void PremiumSoftCard::onReset()
{
    reg_ = 0;
    z80_.reset();
    callPending_ = false;
    iHigh_ = false;
    tOwed_ = 0.0;
    publish();
}

void PremiumSoftCard::c07eWrite(uint8_t value)
{
    reg_ = value;
    if (!(value & kRunOutOfReset)) {          // held in RESET
        z80_.reset();
        callPending_ = false;
        iHigh_ = false;
    } else if (value & kRun) {
        callPending_ = false;                 // a pending call is released
    }
    publish();
}

bool PremiumSoftCard::answersStatus(uint8_t& value) const
{
    if (!(reg_ & kRun)) return false;
    value = z80_.getState().i;
    return true;
}

void PremiumSoftCard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    if (!z80Running()) { tOwed_ = 0.0; return; }
    tOwed_ += cycles * (kZ80Hz / cpuHz_);
    while (tOwed_ > 0.0) {
        tOwed_ -= z80_.step();
        // The next M1's refresh puts I on A8-A15: bit 7 newly up is a call.
        const bool high = (z80_.getState().i & 0x80) != 0;
        if (high && !iHigh_) {
            iHigh_ = true;
            callPending_ = true;
            tOwed_ = 0.0;
            return;
        }
        iHigh_ = high;
    }
}

void PremiumSoftCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(reg_);
    out.push_back(uint8_t((callPending_ ? 1 : 0) | (iHigh_ ? 2 : 0)));
    pom2::appendZ80State(out, z80_.getState());
    out.insert(out.end(), dram0400_.begin(), dram0400_.end());
}

void PremiumSoftCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (len < 4 + 2 + pom2::kZ80StateBlobBytes + dram0400_.size()
        || std::memcmp(data, kMagic, 4) != 0)
        return;
    reg_ = data[4];
    callPending_ = (data[5] & 1) != 0;
    iHigh_       = (data[5] & 2) != 0;
    z80_.setState(pom2::readZ80State(data + 6));
    std::memcpy(dram0400_.data(), data + 6 + pom2::kZ80StateBlobBytes, dram0400_.size());
    publish();
}
