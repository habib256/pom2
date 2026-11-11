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

// UthernetCard — see the header. Port of MAME
// `src/devices/bus/a2bus/uthernet.cpp` (BSD-3, R. Belmont).

#include "UthernetCard.h"

#include <cstring>

namespace pom2 {
namespace {

/// 'UTH1' — every card must tag its snapshot blob so a slot that holds a
/// different card after a rewind ignores it (SlotPeripheral.h contract).
constexpr uint32_t kSnapMagic   = 0x31485455;
constexpr uint16_t kSnapVersion = 1;

} // namespace

UthernetCard::UthernetCard(int slot)
    : slot_(slot)
{
    chip_.setMacAddress(kDefaultMac);
}

void UthernetCard::setBackend(std::unique_ptr<NetworkBackend> backend)
{
    backend_ = std::move(backend);
    chip_.setBackend(backend_.get());
}

// `uthernet.cpp:47-52` — read_c0nx / write_c0nx forward the offset
// straight through; the CS8900A does all the decoding.

uint8_t UthernetCard::deviceSelectRead(uint8_t low4)
{
    return chip_.read(low4);
}

void UthernetCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    chip_.write(low4, v);
}

// `uthernet.cpp:54` reset_from_bus → cs8900a reset. Ctrl-Reset on a real
// Apple II does pull the slot RESET line, and the CS8900A honours it, so
// a warm reset really does drop the card's link state.
void UthernetCard::onReset()
{
    // The chip keeps its programmed IA across a bus reset (MAME
    // cs8900a.cpp device_reset does not touch it, and neither does the
    // uthernet.cpp shim) — re-stamping kDefaultMac here silently reverted
    // any guest- or UI-programmed address on every Ctrl-Reset. The
    // default is stamped once at construction.
    chip_.reset();
    if (backend_) backend_->nicReset();
    cyclesSincePoll_ = 0;
}

void UthernetCard::advanceCycles(int cycles)
{
    if (!backend_) return;

    cyclesSincePoll_ += cycles;
    if (cyclesSincePoll_ < kPollIntervalCycles) return;
    cyclesSincePoll_ = 0;

    // Order matters: pump the transport first so anything that arrived
    // since the last tick is available, then move it into the chip's
    // inbound queue where a RxEvent read can pick it up.
    backend_->poll();
    chip_.pumpBackend();
}

void UthernetCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back(static_cast<uint8_t>(kSnapMagic & 0xFF));
    out.push_back(static_cast<uint8_t>((kSnapMagic >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((kSnapMagic >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((kSnapMagic >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>(kSnapVersion & 0xFF));
    out.push_back(static_cast<uint8_t>((kSnapVersion >> 8) & 0xFF));

    // The host transport itself is NOT snapshotted — live sockets and
    // slirp's NAT table are host state that has moved on by the time a
    // rewind replays. Only the chip comes back.
    chip_.appendSnapshotState(out);
}

void UthernetCard::loadSnapshotState(const uint8_t* data, std::size_t len)
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
