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

// MC6840 PTM — see Mc6840Ptm.h for the sources and what is left out.

#include "Mc6840Ptm.h"

#include <cstring>
#include <limits>

namespace pom2 {

namespace {
constexpr uint8_t kCrDual8   = 0x04;   // CRX2
constexpr uint8_t kCrMeasure = 0x08;   // CRX3: frequency / pulse-width comparison
constexpr uint8_t kCrNoWInit = 0x10;   // CRX4: a latch write does not initialise
constexpr uint8_t kCrIrqEn   = 0x40;   // CRX6
constexpr uint8_t kCr10Hold  = 0x01;   // CR1 bit 0: every timer held preset
constexpr uint8_t kCr20Sel1  = 0x01;   // CR2 bit 0: RS0 writes CR1 (else CR3)
constexpr uint8_t kCr30Div8  = 0x01;   // CR3 bit 0: timer 3 ÷8
constexpr char kMagic[4] = { 'P', 'T', 'M', '1' };
} // namespace

void Mc6840Ptm::reset()
{
    for (auto& t : t_) t = Timer{};
    t_[0].cr = kCr10Hold;
    flags_ = 0;
    readWithFlag_ = 0;
    msbBuffer_ = lsbBuffer_ = 0;
}

uint8_t Mc6840Ptm::status() const
{
    uint8_t s = flags_ & 0x07;
    for (int i = 0; i < 3; ++i)
        if ((flags_ & (1u << i)) && (t_[i].cr & kCrIrqEn)) s |= 0x80;
    return s;
}

// Continuous and single-shot count with the gate held low; the comparison
// modes wait for a gate fall that never comes (Mc6840Ptm.h).
bool Mc6840Ptm::counting(int t) const
{
    return !(t_[0].cr & kCr10Hold) && !(t_[t].cr & kCrMeasure);
}

void Mc6840Ptm::initializeCounter(int t)
{
    t_[t].counter = t_[t].latch;
    t_[t].prescale = 0;
    flags_ &= uint8_t(~(1u << t));
    readWithFlag_ &= uint8_t(~(1u << t));
}

// Counter clocks to the next Time Out, counting the Time Out clock itself.
// 16-bit: counter + 1. Dual 8-bit with LSB l and MSB m: the LSB runs out in
// l + 1 clocks, then each of the m MSB steps takes L + 1 — so l may exceed
// the latch's L (a latch written with CRX4 = 1 is not loaded yet) and the
// sum still holds.
uint64_t Mc6840Ptm::clocksToTimeOut(int t) const
{
    const Timer& x = t_[t];
    if (!(x.cr & kCrDual8)) return uint64_t(x.counter) + 1;
    const uint64_t l = x.counter & 0xFF, m = x.counter >> 8;
    const uint64_t big = uint64_t(x.latch & 0xFF) + 1;
    return l + 1 + m * big;
}

// Step a counter down by `clocks` that stop short of its Time Out.
static uint16_t stepDown(uint16_t counter, uint16_t latch, bool dual, uint64_t clocks)
{
    if (!dual) return uint16_t(counter - clocks);
    uint64_t l = counter & 0xFF, m = counter >> 8;
    const uint64_t big = uint64_t(latch & 0xFF) + 1;
    if (clocks <= l) l -= clocks;
    else {
        const uint64_t past = clocks - (l + 1);   // clocks after the first LSB run-out
        m -= 1 + past / big;
        l = (big - 1) - past % big;
    }
    return uint16_t((m << 8) | l);
}

void Mc6840Ptm::tickTimer(int t, uint64_t clocks)
{
    Timer& x = t_[t];
    if (t == 2 && (x.cr & kCr30Div8)) {
        const uint64_t total = x.prescale + clocks;
        x.prescale = uint8_t(total % 8);
        clocks = total / 8;
    }
    if (clocks == 0) return;
    const bool dual = (x.cr & kCrDual8) != 0;
    const uint64_t toOut = clocksToTimeOut(t);
    if (clocks < toOut) { x.counter = stepDown(x.counter, x.latch, dual, clocks); return; }
    // At least one Time Out: the flag sets and the counter reloads from the
    // latch; the remaining clocks run on the reloaded value, through as many
    // further Time Outs as they hold (the flag is already set).
    flags_ |= uint8_t(1u << t);
    readWithFlag_ &= uint8_t(~(1u << t));
    x.counter = x.latch;
    const uint64_t rest = (clocks - toOut) % clocksToTimeOut(t);
    if (rest) x.counter = stepDown(x.latch, x.latch, dual, rest);
}

void Mc6840Ptm::tick(uint64_t eClocks)
{
    if (eClocks == 0) return;
    for (int t = 0; t < 3; ++t)
        if (counting(t)) tickTimer(t, eClocks);
}

uint64_t Mc6840Ptm::clocksToNextTimeOut() const
{
    uint64_t best = std::numeric_limits<uint64_t>::max();
    for (int t = 0; t < 3; ++t) {
        if (!counting(t)) continue;
        uint64_t c = clocksToTimeOut(t);
        if (t == 2 && (t_[2].cr & kCr30Div8)) c = c * 8 - t_[2].prescale;
        if (c < best) best = c;
    }
    return best;
}

uint8_t Mc6840Ptm::read(uint8_t rs)
{
    rs &= 7;
    if (rs == 0) return 0;                       // no operation ([F], as MAME)
    if (rs == 1) {
        const uint8_t s = status();
        readWithFlag_ |= flags_ & 0x07;
        return s;
    }
    const int t = (rs - 2) / 2;
    if (rs & 1) return lsbBuffer_;               // RS3/5/7: the LSB buffer
    // RS2/4/6: the counter's MSB now, its LSB into the buffer; a flag the
    // status read saw is cleared (the RS-RT sequence).
    const uint16_t c = t_[t].counter;
    lsbBuffer_ = uint8_t(c & 0xFF);
    if (readWithFlag_ & (1u << t)) {
        flags_ &= uint8_t(~(1u << t));
        readWithFlag_ &= uint8_t(~(1u << t));
    }
    return uint8_t(c >> 8);
}

void Mc6840Ptm::write(uint8_t rs, uint8_t v)
{
    rs &= 7;
    if (rs <= 1) {
        const int t = rs == 1 ? 1 : (t_[1].cr & kCr20Sel1) ? 0 : 2;
        const bool wasHeld = (t_[0].cr & kCr10Hold) != 0;
        t_[t].cr = v;
        if (t == 0 && (v & kCr10Hold) && !wasHeld) {
            // CR10 set: every counter preset from its latch, flags cleared.
            for (int i = 0; i < 3; ++i) initializeCounter(i);
        }
        return;
    }
    if (!(rs & 1)) { msbBuffer_ = v; return; }   // RS2/4/6: the MSB buffer
    const int t = (rs - 3) / 2;                  // RS3/5/7: Write Timer Latches
    t_[t].latch = uint16_t(msbBuffer_ << 8 | v);
    flags_ &= uint8_t(~(1u << t));
    readWithFlag_ &= uint8_t(~(1u << t));
    if (!(t_[t].cr & kCrNoWInit) || (t_[0].cr & kCr10Hold)) initializeCounter(t);
}

void Mc6840Ptm::appendState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    for (const auto& t : t_) {
        out.push_back(uint8_t(t.latch));   out.push_back(uint8_t(t.latch >> 8));
        out.push_back(uint8_t(t.counter)); out.push_back(uint8_t(t.counter >> 8));
        out.push_back(t.cr);
    }
    out.push_back(uint8_t(flags_ | readWithFlag_ << 3));
    out.push_back(t_[2].prescale);
    out.push_back(msbBuffer_);
    out.push_back(lsbBuffer_);
}

bool Mc6840Ptm::loadState(const uint8_t* data, std::size_t len)
{
    if (!data || len < 4 + kStateBytes || std::memcmp(data, kMagic, 4) != 0) return false;
    const uint8_t* p = data + 4;
    for (auto& t : t_) {
        t.latch   = uint16_t(p[0] | p[1] << 8);
        t.counter = uint16_t(p[2] | p[3] << 8);
        t.cr      = p[4];
        t.prescale = 0;
        p += 5;
    }
    flags_ = p[0] & 0x07;
    readWithFlag_ = (p[0] >> 3) & 0x07;
    t_[2].prescale = p[1] & 7;
    msbBuffer_ = p[2];
    lsbBuffer_ = p[3];
    return true;
}

} // namespace pom2
