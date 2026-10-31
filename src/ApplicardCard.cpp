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

// ApplicardCard implementation — MAME a2applicard.cpp read_c0nx/write_c0nx
// (:185-250) and z80_io_r/z80_io_w (:252-300); see the header.

#include "ApplicardCard.h"

#include "Z80StateBlob.h"

#include <algorithm>
#include <cstring>

namespace {
// Snapshot blob: "APC1", flags (bit0 romOn, bit1 toZ80Full, bit2
// fromZ80Full), toZ80, fromZ80, the Z80 register file, then the 64 KB.
// The ROM is not saved: it is the card's, reloaded at plug time.
constexpr char kMagic[4] = { 'A', 'P', 'C', '1' };
} // namespace

ApplicardCard::ApplicardCard() : z80_(*this)
{
    resetZ80();
}

bool ApplicardCard::loadRom(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != kRomBytes) return false;
    std::copy(bytes.begin(), bytes.end(), rom_.begin());
    return true;
}

// izapple2 reset(): RAM kept, ROM on, both latches empty.
void ApplicardCard::resetZ80()
{
    z80_.reset();
    romOn_ = true;
    toZ80Full_ = false;
    fromZ80Full_ = false;
}

void ApplicardCard::control(uint8_t low4)
{
    switch (low4) {
    case 5: resetZ80(); break;
    case 7: z80_.triggerNmi(); break;
    default: break;                            // 6: the CTC is not fitted
    }
}

uint8_t ApplicardCard::deviceSelectRead(uint8_t low4)
{
    switch (low4) {
    case 0: fromZ80Full_ = false; return fromZ80_;
    case 1: return toZ80_;
    case 2: return toZ80Full_ ? 0x80 : 0x00;
    case 3: return fromZ80Full_ ? 0x80 : 0x00;
    case 5: case 6: case 7: control(low4); return 0xFF;
    default: return 0xFF;
    }
}

void ApplicardCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4) {
    case 1: toZ80_ = v; toZ80Full_ = true; break;
    case 5: case 6: case 7: control(low4); break;
    default: break;
    }
}

uint8_t ApplicardCard::z80MemRead(uint16_t addr)
{
    if (romOn_ && addr < 0x8000)
        return rom_[addr % kRomBytes];
    return ram_[addr];
}

void ApplicardCard::z80MemWrite(uint16_t addr, uint8_t v)
{
    if (romOn_ && addr < 0x8000)
        return;
    ram_[addr] = v;
}

uint8_t ApplicardCard::z80IoRead(uint16_t port)
{
    switch (port & 0xE0) {
    case 0x00: return fromZ80_;
    case 0x20: toZ80Full_ = false; return toZ80_;
    case 0x40: return uint8_t((toZ80Full_ ? 0x80 : 0) | (fromZ80Full_ ? 0x01 : 0));
    default:   return 0xFF;
    }
}

void ApplicardCard::z80IoWrite(uint16_t port, uint8_t v)
{
    switch (port & 0xE0) {
    case 0x00: fromZ80_ = v; fromZ80Full_ = true; break;
    case 0x60: romOn_ = (v & 1) != 0; break;
    default: break;
    }
}

void ApplicardCard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    tOwed_ += cycles * (kZ80Hz / cpuHz_);
    while (tOwed_ > 0.0)
        tOwed_ -= z80_.step();
}

void ApplicardCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(uint8_t((romOn_ ? 1 : 0) | (toZ80Full_ ? 2 : 0) | (fromZ80Full_ ? 4 : 0)));
    out.push_back(toZ80_);
    out.push_back(fromZ80_);
    pom2::appendZ80State(out, z80_.getState());
    out.insert(out.end(), ram_.begin(), ram_.end());
}

void ApplicardCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (len < 4 + 3 + pom2::kZ80StateBlobBytes + ram_.size()
        || std::memcmp(data, kMagic, 4) != 0)
        return;
    const uint8_t* p = data + 4;
    const uint8_t flags = *p++;
    romOn_       = (flags & 1) != 0;
    toZ80Full_   = (flags & 2) != 0;
    fromZ80Full_ = (flags & 4) != 0;
    toZ80_   = *p++;
    fromZ80_ = *p++;
    z80_.setState(pom2::readZ80State(p));
    p += pom2::kZ80StateBlobBytes;
    std::memcpy(ram_.data(), p, ram_.size());
}
