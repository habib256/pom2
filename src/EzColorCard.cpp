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
    vdp_.setIrqStrapped(true);   // /INT leaves the chip; JP2 decides where (EzColorCard.h)
    vdp_.setSiliconStrictMode(true);   // the access window, as the chip (header)
    vdp_.reset();
    clk_.setBusClock(static_cast<double>(POM2_CPU_CLOCK_HZ));   // the VDP's tables' unit
}

int EzColorCard::inFlight() const
{
    if (!cpu_) return 0;
    const int c = cpu_->getCurrentInstructionCycles();
    return c > 0 ? static_cast<int>(clk_.peekBusTicks(static_cast<uint64_t>(c))) : 0;
}

// The whole $C0n0-$C0nF answers, A0 choosing the port: even = data (VRAM),
// odd = register (status / address and register writes).
// [B] Ciarcia, "E-Z Color Graphics Interface", BYTE August 1982, Fig. 4
//     p. 65 and text p. 66: /DS (bus pin 41) and R/W (18) form CSR/CSW
//     through the 74LS00, A0 (pin 2) goes straight to MODE — "one address
//     line (typically A0)". No other address line reaches the card.
// [D] MAME a2bus/ezcgi.cpp:234-260 @ mame0287 decodes offsets 0 and 1 only
//     and reads $FF elsewhere; overruled by the schematic above (POM2 did
//     the same until 2026-11-16).
uint8_t EzColorCard::deviceSelectRead(uint8_t low4)
{
    uint8_t v;
    if ((low4 & 1) == 0) {
        v = vdp_.readData();
    } else {
        vdp_.syncSpriteScanToBeam(inFlight());
        v = vdp_.readControl();   // clears the frame flag: /INT drops
    }
    updateIrq();
    return v;
}

void EzColorCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (cpu_) vdp_.setLastAccessPc(cpu_->getProgramCounter());   // the drop histogram's site
    vdp_.renderBeamCatchUp(inFlight());
    if ((low4 & 1) == 0) vdp_.writeData(v);
    else                 vdp_.writeControl(v);
    updateIrq();
}

void EzColorCard::updateIrq(bool restoring)
{
    const bool low = vdp_.irqAsserted();
    assertIrq(wiring_ == InterruptWiring::Irq && low);
    const bool nmi = wiring_ == InterruptWiring::Nmi && low;
    if (restoring) restoreNmiLevel(nmi);
    else           assertNmi(nmi);
}

void EzColorCard::setInterruptWiring(InterruptWiring w)
{
    if (w == wiring_) return;
    wiring_ = w;
    updateIrq();
}

EzColorCard::InterruptWiring EzColorCard::parseInterruptWiring(std::string_view s)
{
    if (s == "irq")  return InterruptWiring::Irq;
    if (s == "open") return InterruptWiring::Open;
    return InterruptWiring::Nmi;
}

void EzColorCard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    const int ticks = static_cast<int>(clk_.busTicks(static_cast<uint64_t>(cycles)));
    if (ticks > 0) vdp_.advanceCycles(ticks);
    updateIrq();
}

// MAME reset_from_bus: m_tms->reset(), whose device_reset leaves VRAM alone.
void EzColorCard::onReset()
{
    vdp_.resetPin();
    updateIrq();
}

// Slot power off and on: the DRAM forgets.
void EzColorCard::onPowerCycle()
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
    if (vdp_.loadState(data + 4, len - 4)) updateIrq(/*restoring=*/true);
}
