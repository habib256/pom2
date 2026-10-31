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

// E-Z Color Graphics Interface — see EzColorCard.h.

#include "EzColorCard.h"

#include "M6502.h"

#include <cstring>

namespace {
// Snapshot blob: "EZC1" then the VDP's state (TMS9918::appendState).
constexpr char kMagic[4] = { 'E', 'Z', 'C', '1' };
} // namespace

EzColorCard::EzColorCard()
{
    vdp_.setIrqStrapped(true);   // the VDP's /INT drives the slot IRQ
    vdp_.reset();
}

int EzColorCard::inFlight() const
{
    return cpu_ ? cpu_->getCurrentInstructionCycles() : 0;
}

// MAME ezcgi.cpp read_c0nx: 0 = vram_read, 1 = register_read, else $FF.
uint8_t EzColorCard::deviceSelectRead(uint8_t low4)
{
    uint8_t v = 0xFF;
    if (low4 == 0) {
        v = vdp_.readData();
    } else if (low4 == 1) {
        vdp_.syncSpriteScanToBeam(inFlight());
        v = vdp_.readControl();   // clears the frame flag: /INT drops
    }
    updateIrq();
    return v;
}

// write_c0nx: 0 = vram_write, 1 = register_write.
void EzColorCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (low4 > 1) return;
    vdp_.renderBeamCatchUp(inFlight());
    if (low4 == 0) vdp_.writeData(v);
    else           vdp_.writeControl(v);
    updateIrq();
}

void EzColorCard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    vdp_.advanceCycles(cycles);
    updateIrq();
}

// MAME reset_from_bus: m_tms->reset().
void EzColorCard::onReset()
{
    vdp_.reset();
    updateIrq();
}

void EzColorCard::paintPicture(uint32_t* dst, uint32_t, uint32_t, uint64_t, double) const
{
    const uint32_t* src = vdp_.framebufferData();
    const std::size_t n = std::size_t(TMS9918::kFullWidth) * TMS9918::kFullHeight;
    for (std::size_t i = 0; i < n; ++i) dst[i] = src[i] | 0xFF000000u;   // colour 0 is see-through
}

void EzColorCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    vdp_.appendState(out);
}

void EzColorCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (!data || len < 4 + TMS9918::kStateBytes || std::memcmp(data, kMagic, 4) != 0) return;
    if (vdp_.loadState(data + 4, len - 4)) updateIrq();
}
