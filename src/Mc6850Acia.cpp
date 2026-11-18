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

// MC6850 ACIA with a timed transmitter — see Mc6850Acia.h.

#include "Mc6850Acia.h"

#include <algorithm>
#include <cstring>

namespace pom2 {

namespace {
constexpr uint8_t kRdrf = 0x01, kTdre = 0x02, kOvrn = 0x20, kIrq = 0x80;
constexpr char kMagic[4] = { 'A', 'C', 'I', '1' };
// CR4:2 -> data bits, parity (0 none, 1 even, 2 odd), stop bits. [B] the
// data sheet's word-select table.
struct Word { int data, parity, stop; };
constexpr Word kWords[8] = {
    {7, 1, 2}, {7, 2, 2}, {7, 1, 1}, {7, 2, 1},
    {8, 0, 2}, {8, 0, 1}, {8, 1, 1}, {8, 2, 1},
};
} // namespace

void Mc6850Acia::setTiming(double busTicksPerTxClock)
{
    if (busTicksPerTxClock > 0.0) txClock_ = busTicksPerTxClock;
}

void Mc6850Acia::powerOn()
{
    control_ = 0x03;
    tdrFull_ = shifting_ = false;
    rdrf_ = overrun_ = false;
    rdr_ = tdr_ = 0;
    rx_.clear();
}

double Mc6850Acia::bitTicks() const
{
    static constexpr int kDivide[3] = { 1, 16, 64 };
    return txClock_ * kDivide[std::min(control_ & 0x03, 2)];
}

int Mc6850Acia::frameBits() const
{
    const Word& w = kWords[(control_ >> 2) & 7];
    return 1 + w.data + (w.parity ? 1 : 0) + w.stop;
}

double Mc6850Acia::frameTicks() const { return bitTicks() * frameBits(); }

// The character as the far end's 8N1 receiver assembles it: the 7-bit
// formats put the parity bit where that receiver expects data bit 7; the
// 8-bit-plus-parity formats put it where it expects the stop bit, so a
// parity bit of 0 is a framing error there and the byte is lost.
Mc6850Acia::Sent Mc6850Acia::wireByte(uint8_t v, double at) const
{
    const Word& w = kWords[(control_ >> 2) & 7];
    const uint8_t data = w.data == 7 ? uint8_t(v & 0x7F) : v;
    int ones = 0;
    for (uint8_t d = data; d; d &= uint8_t(d - 1)) ++ones;
    const int parityBit = w.parity == 1 ? (ones & 1) : w.parity == 2 ? !(ones & 1) : 1;
    if (w.data == 7) return { at, uint8_t(data | (parityBit << 7)), true };
    return { at, data, w.parity == 0 || parityBit == 1 };
}

uint8_t Mc6850Acia::status() const
{
    if (inReset()) return 0;                    // master reset clears the status
    uint8_t st = 0;
    if (!tdrFull_) st |= kTdre;
    if (rdrf_) st |= kRdrf;
    if (overrun_) st |= kOvrn;
    const bool txIrq = (control_ & 0x60) == 0x20 && !tdrFull_;
    const bool rxIrq = (control_ & 0x80) && (rdrf_ || overrun_);
    if (txIrq || rxIrq) st |= kIrq;
    return st;
}

uint8_t Mc6850Acia::readData()
{
    const uint8_t v = rdr_;
    rdrf_ = false;
    overrun_ = false;
    if (!rx_.empty() && !inReset()) { rdr_ = rx_.front(); rx_.pop_front(); rdrf_ = true; }
    return v;
}

void Mc6850Acia::receive(uint8_t v)
{
    if (inReset()) return;
    if (!rdrf_) { rdr_ = v; rdrf_ = true; return; }
    rx_.push_back(v);
}

void Mc6850Acia::writeControl(uint8_t v)
{
    const bool wasReset = inReset();
    control_ = v;
    if (inReset()) {
        // Master reset: status cleared, transmitter and receiver initialised.
        tdrFull_ = shifting_ = false;
        rdrf_ = overrun_ = false;
        rx_.clear();
    } else if (wasReset) {
        tdrFull_ = false;   // out of reset: the TDR is empty
    }
}

void Mc6850Acia::startShift(double at)
{
    shiftByte_ = tdr_;
    tdrFull_ = false;
    shifting_ = true;
    shiftEnd_ = at + frameTicks();
}

void Mc6850Acia::writeData(uint8_t v)
{
    if (inReset()) return;
    tdr_ = v;
    tdrFull_ = true;
    // A break level holds the line: nothing is transferred until it ends.
    if (!shifting_ && (control_ & 0x60) != 0x60) startShift(now_);
}

void Mc6850Acia::tick(double busTicks, std::vector<Sent>& out)
{
    if (busTicks > 0.0) now_ += busTicks;
    while (shifting_ && shiftEnd_ <= now_) {
        out.push_back(wireByte(shiftByte_, shiftEnd_));
        shifting_ = false;
        if (tdrFull_ && (control_ & 0x60) != 0x60) startShift(shiftEnd_);
    }
    if (!shifting_ && tdrFull_ && !inReset() && (control_ & 0x60) != 0x60)
        startShift(now_);   // a break just ended with a character waiting
}

// Times are saved relative to `now_`, which restarts at 0 on load.
void Mc6850Acia::appendState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(control_);
    out.push_back(tdr_);
    out.push_back(shiftByte_);
    out.push_back(rdr_);
    out.push_back(uint8_t((tdrFull_ ? 1 : 0) | (shifting_ ? 2 : 0) | (rdrf_ ? 4 : 0) | (overrun_ ? 8 : 0)));
    const double left = shifting_ ? std::max(0.0, shiftEnd_ - now_) : 0.0;
    const uint32_t q = uint32_t(std::min(left * 16.0, 4.0e9));   // 1/16 bus tick
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(q >> (8 * i)));
}

bool Mc6850Acia::loadState(const uint8_t* data, std::size_t len)
{
    if (!data || len < 13 || std::memcmp(data, kMagic, 4) != 0) return false;
    control_ = data[4];
    tdr_ = data[5];
    shiftByte_ = data[6];
    rdr_ = data[7];
    tdrFull_ = data[8] & 1;
    shifting_ = (data[8] & 2) != 0;
    rdrf_ = (data[8] & 4) != 0;
    overrun_ = (data[8] & 8) != 0;
    uint32_t q = 0;
    for (int i = 0; i < 4; ++i) q |= uint32_t(data[9 + i]) << (8 * i);
    now_ = 0.0;
    shiftEnd_ = q / 16.0;
    rx_.clear();
    return true;
}

} // namespace pom2
