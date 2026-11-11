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

// UthernetIICard — see the header. Bus glue over W5100Device; reference
// is AppleWin `source/Uthernet2.cpp:1411-1472` (GPL-2.0+).

#include "UthernetIICard.h"

namespace pom2 {
namespace {

/// 'UTH2' — snapshot tag (SlotPeripheral.h contract: a slot that holds a
/// different card after a rewind must ignore this blob).
constexpr uint32_t kSnapMagic   = 0x32485455;
constexpr uint16_t kSnapVersion = 1;

} // namespace

UthernetIICard::UthernetIICard(int slot)
    : slot_(slot)
{
    setMacAddress(kDefaultMac);
}

void UthernetIICard::setMacAddress(const std::array<uint8_t, 6>& mac)
{
    // SHAR sits in the common register block; writing it through the
    // normal path also flushes the ARP cache, which is what we want.
    for (int i = 0; i < 6; ++i) {
        chip_.writeValueAt(static_cast<uint16_t>(kW5100Shar0 + i),
                           mac[static_cast<size_t>(i)]);
    }
}

void UthernetIICard::setBackend(std::unique_ptr<NetworkBackend> backend)
{
    backend_ = std::move(backend);
    chip_.setBackend(backend_.get());
}

// `Uthernet2.cpp:1439-1456` — read path.
uint8_t UthernetIICard::deviceSelectRead(uint8_t low4)
{
    switch (low4 & kRegisterMask) {
    case kRegMode:   return chip_.modeRegister();
    case kRegAddrHi: return static_cast<uint8_t>((chip_.dataAddress() >> 8) & 0xFF);
    case kRegAddrLo: return static_cast<uint8_t>(chip_.dataAddress() & 0xFF);
    case kRegData:   return chip_.readData();
    default:         return 0xFF;   // unreachable: the mask is 2 bits
    }
}

// `Uthernet2.cpp:1421-1437` — write path.
void UthernetIICard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4 & kRegisterMask) {
    case kRegMode:   chip_.setModeRegister(v);      break;
    case kRegAddrHi: chip_.setDataAddressHigh(v);   break;
    case kRegAddrLo: chip_.setDataAddressLow(v);    break;
    case kRegData:   chip_.writeData(v);            break;
    default: break;
    }
}

void UthernetIICard::onReset()
{
    // Ctrl-Reset pulls the slot RESET line, and on this card that line goes
    // straight to the chip's /RESET pin. [B] Uthernet II manual, schematic
    // pp. 50-51: the connector's RST pin is on the RESET net, which reaches
    // the W5100's pin 59 /RESET. So Ctrl-Reset is a HARDWARE reset of the
    // chip, and manual p. 10 is explicit about what that means: "After a
    // hardware reset the address pointer is initialized to 0", as against
    // "a software reset does not reset the address pointer".
    //
    // POM2 did keep the pointer from 2026-10-27 to 2026-11-01, citing that
    // same p. 10 — but reading the software-reset half of it, and matching
    // [D] AppleWin, whose CtrlReset calls `Uthernet2::Reset(false)`. A
    // lower grade does not overrule a higher one: the manual wins.
    // `docs/audits/parite-uthernet2-2026-11-01.md`, constat 1.
    //
    // It matters because manual p. 12 lets a driver rewrite only the LOW
    // byte of the address: after a reset it assumes the high byte is $00,
    // and with a stale $04 up there it would write socket-0 registers
    // instead of the common ones.
    resetChip(true);
}

void UthernetIICard::onPowerCycle()
{
    resetChip(true);
}

void UthernetIICard::resetChip(bool powerCycle)
{
    chip_.reset(powerCycle);
    if (backend_) backend_->nicReset();   // MACRAW / IPRAW traffic went through it
    setMacAddress(kDefaultMac);
    cyclesSincePoll_ = 0;
}

void UthernetIICard::advanceCycles(int cycles)
{
    cyclesSincePoll_ += cycles;
    if (cyclesSincePoll_ < kPollIntervalCycles) return;
    cyclesSincePoll_ = 0;

    // Promotes in-flight connects, drains late DNS answers and pumps the
    // backend. Received *data* is pulled lazily instead, when the guest
    // reads SN_RX_RSR — that is how the real chip is polled.
    chip_.poll();
}

void UthernetIICard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back(static_cast<uint8_t>(kSnapMagic & 0xFF));
    out.push_back(static_cast<uint8_t>((kSnapMagic >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((kSnapMagic >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((kSnapMagic >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>(kSnapVersion & 0xFF));
    out.push_back(static_cast<uint8_t>((kSnapVersion >> 8) & 0xFF));

    chip_.appendSnapshotState(out);
}

void UthernetIICard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (len < 6) return;
    const uint32_t magic = static_cast<uint32_t>(data[0]) |
                           (static_cast<uint32_t>(data[1]) << 8) |
                           (static_cast<uint32_t>(data[2]) << 16) |
                           (static_cast<uint32_t>(data[3]) << 24);
    const uint16_t version = static_cast<uint16_t>(data[4] | (data[5] << 8));
    if (magic != kSnapMagic || version != kSnapVersion) return;

    chip_.loadSnapshotState(data + 6, len - 6);
    chip_.setBackend(backend_.get());
}

} // namespace pom2
