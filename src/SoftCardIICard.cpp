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

// SoftCardIICard implementation — see the header; izapple2
// cardZ80Softcard2.go is the reference for every rule.

#include "SoftCardIICard.h"

#include "Z80StateBlob.h"

#include <cstring>

namespace {
// Snapshot blob: "SC2a", flags (bit0 booting, bit1 toZ80Full, bit2
// fromZ80Full), toZ80, fromZ80, the Z80 register file in SoftCardZ80's
// layout, the replay buffer (count + bytes, at most a few), then the 64 KB.
constexpr char kMagic[4] = { 'S', 'C', '2', 'a' };
constexpr std::size_t kMaxReplay = 8;   // the longest Z80 instruction is 4 bytes
} // namespace

SoftCardIICard::SoftCardIICard() : z80_(*this)
{
    resetZ80();
}

void SoftCardIICard::resetZ80()
{
    z80_.reset();
    booting_ = true;
    toZ80Full_ = false;
    fromZ80Full_ = false;
    stalled_ = false;
    fetched_.clear();
    fetchPos_ = 0;
}

// izapple2: soft switches 0 (R/W), 1 (R) and 3 (W).
uint8_t SoftCardIICard::deviceSelectRead(uint8_t low4)
{
    switch (low4 & 3) {
    case 0: fromZ80Full_ = false; return fromZ80_;
    case 1: return status(fromZ80Full_, toZ80Full_);
    default: return 0xFF;
    }
}

void SoftCardIICard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4 & 3) {
    case 0: toZ80_ = v; toZ80Full_ = true; break;
    case 3: resetZ80(); break;
    default: break;
    }
}

uint8_t SoftCardIICard::z80MemRead(uint16_t addr)
{
    if (!booting_ || addr >= 0x8000)
        return ram_[addr];
    if (fetchPos_ < fetched_.size())
        return fetched_[fetchPos_++];          // taken by the undone attempt
    if (stalled_ || !toZ80Full_) {
        stalled_ = true;
        return 0;                              // the instruction is undone
    }
    toZ80Full_ = false;
    fetched_.push_back(toZ80_);
    ++fetchPos_;
    return toZ80_;
}

void SoftCardIICard::z80MemWrite(uint16_t addr, uint8_t v)
{
    if (stalled_)
        return;
    if (booting_ && addr < 0x8000) {
        fromZ80_ = v;
        fromZ80Full_ = true;
    }
    ram_[addr] = v;
}

uint8_t SoftCardIICard::z80IoRead(uint16_t port)
{
    if (stalled_)
        return 0;
    if (port & 1)
        return status(toZ80Full_, fromZ80Full_);
    toZ80Full_ = false;
    return toZ80_;
}

void SoftCardIICard::z80IoWrite(uint16_t /*port*/, uint8_t v)
{
    if (stalled_)
        return;
    if (booting_) {
        booting_ = false;
        return;
    }
    fromZ80_ = v;
    fromZ80Full_ = true;
}

bool SoftCardIICard::stepZ80(int& tStates)
{
    const pom2::Z80::State saved = z80_.getState();
    stalled_ = false;
    fetchPos_ = 0;
    tStates = z80_.step();
    if (stalled_) {
        z80_.setState(saved);
        return false;
    }
    fetched_.clear();
    return true;
}

void SoftCardIICard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    tOwed_ += cycles * (kZ80Hz / cpuHz_);
    while (tOwed_ > 0.0) {
        int t = 0;
        if (!stepZ80(t)) {
            tOwed_ = 0.0;   // held for the 6502: the time is gone
            return;
        }
        tOwed_ -= t;
    }
}

void SoftCardIICard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    const pom2::Z80::State& s = z80_.getState();
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(uint8_t((booting_ ? 1 : 0) | (toZ80Full_ ? 2 : 0) | (fromZ80Full_ ? 4 : 0)));
    out.push_back(toZ80_);
    out.push_back(fromZ80_);
    pom2::appendZ80State(out, s);
    const std::size_t n = fetched_.size() < kMaxReplay ? fetched_.size() : kMaxReplay;
    out.push_back(uint8_t(n));
    out.insert(out.end(), fetched_.begin(), fetched_.begin() + std::ptrdiff_t(n));
    out.insert(out.end(), ram_.begin(), ram_.end());
}

void SoftCardIICard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    constexpr std::size_t kFixed = 4 + 3 + pom2::kZ80StateBlobBytes + 1;
    if (len < kFixed + 0x10000 || std::memcmp(data, kMagic, 4) != 0)
        return;
    const uint8_t* p = data + 4;
    const std::size_t n = p[3 + pom2::kZ80StateBlobBytes];
    if (n > kMaxReplay || len < kFixed + n + 0x10000)
        return;
    const uint8_t flags = *p++;
    booting_     = (flags & 1) != 0;
    toZ80Full_   = (flags & 2) != 0;
    fromZ80Full_ = (flags & 4) != 0;
    toZ80_   = *p++;
    fromZ80_ = *p++;

    z80_.setState(pom2::readZ80State(p));
    p += pom2::kZ80StateBlobBytes;
    ++p;                                       // the replay count, read above
    fetched_.assign(p, p + n);
    fetchPos_ = 0;
    stalled_ = false;
    p += n;
    std::memcpy(ram_.data(), p, ram_.size());
}
