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
    // Ctrl-Reset pulls the slot RESET line: sockets close, buffers clear —
    // but it is not a power cycle. The indirect data address and the ARP /
    // virtual-DNS caches survive it (Uthernet II manual p.10; AppleWin's
    // CtrlReset → Uthernet2::Reset(false)); this used to power-cycle the
    // chip, and $C0n5/6 read back 0 after a Ctrl-Reset. The MAC is re-seeded
    // because SHAR is part of what gets cleared either way, and a card with
    // an all-zero source address is useless.
    resetChip(false);
}

void UthernetIICard::onPowerCycle()
{
    resetChip(true);
}

void UthernetIICard::resetChip(bool powerCycle)
{
    chip_.reset(powerCycle);
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
