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

// SaturnCard implementation — MAME ramcard128k.cpp do_io (:70-120) for the
// decode, POM2's language card for the write-enable rule; see the header.

#include "SaturnCard.h"

#include <cstring>

namespace {
// Snapshot blob: "SAT1", flags (bit0 readRam, bit1 writeEnable, bit2
// prewrite, bit3 bank2), block, then the 128 KB.
constexpr char kMagic[4] = { 'S', 'A', 'T', '1' };
} // namespace

void SaturnCard::onPlug()
{
    if (mem_) mem_->setLcInhibitor(this);
}

void SaturnCard::onUnplug()
{
    if (mem_ && mem_->lcInhibitor() == this) mem_->setLcInhibitor(nullptr);
}

void SaturnCard::onReset()
{
    readRam_ = false;
    writeEnable_ = false;
    prewrite_ = false;
    bank2_ = true;
    block_ = 0;
    publish();
}

void SaturnCard::access(uint8_t low4, bool isWrite)
{
    if (low4 & 4) {
        block_ = (low4 & 3) | ((low4 & 8) ? 4 : 0);
        return;
    }
    if ((low4 & 1) == 0) {
        prewrite_ = false;
        writeEnable_ = false;
    }
    if (isWrite) {
        prewrite_ = false;
    } else if (low4 & 1) {
        if (!prewrite_) prewrite_ = true;
        else            writeEnable_ = true;
    }
    const uint8_t mode = low4 & 3;
    readRam_ = mode == 0 || mode == 3;
    bank2_ = (low4 & 8) == 0;
    publish();
}

void SaturnCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(uint8_t((readRam_ ? 1 : 0) | (writeEnable_ ? 2 : 0)
                          | (prewrite_ ? 4 : 0) | (bank2_ ? 8 : 0)));
    out.push_back(uint8_t(block_));
    out.insert(out.end(), ram_.begin(), ram_.end());
}

void SaturnCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (len < 6 + ram_.size() || std::memcmp(data, kMagic, 4) != 0 || data[5] >= kBlocks)
        return;
    const uint8_t flags = data[4];
    readRam_     = (flags & 1) != 0;
    writeEnable_ = (flags & 2) != 0;
    prewrite_    = (flags & 4) != 0;
    bank2_       = (flags & 8) != 0;
    block_       = data[5];
    std::memcpy(ram_.data(), data + 6, ram_.size());
    publish();
}
