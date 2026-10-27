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

// UthernetIICard — a2RetroSystems **Uthernet II** (catalog key
// `uthernet2`). A WIZnet W5100 wired to the slot bus through the chip's
// indirect ("PPP over 4 registers") interface.
//
// MAME has no Uthernet II device, so the reference is AppleWin's
// `source/Uthernet2.cpp` + `source/W5100.h` (GPL-2.0+, Andrea Odetti),
// cross-checked against the Uthernet II manual (2018-11-17). The chip
// itself is W5100Device; this file is only the bus glue.
//
// Address decode (`Uthernet2.cpp:1411-1472`, `W5100.h:5-12`)
// ----------------------------------------------------------
// Only A0 and A1 reach the card, so the four registers repeat four times
// across the 16-byte $C0nX window. The canonical addresses are the
// $C0n4-$C0n7 group:
//
//   $C0n4  MODE     RW  W5100 mode register (bit 1 = address
//                       auto-increment, bit 7 = soft reset)
//   $C0n5  ADDR_HI  RW  high byte of the 15-bit indirect address
//   $C0n6  ADDR_LO  RW  low byte
//   $C0n7  DATA     RW  the byte at that address; auto-increments when
//                       MODE bit 1 is set
//
// Because only two address lines are decoded, $C0n0 aliases MODE, $C0n1
// aliases ADDR_HI and so on. That is real hardware behaviour and some
// drivers rely on it, so the mask is applied rather than the range.
//
// No slot ROM: like the Uthernet I, the card has no boot PROM, so
// `slotRomRead` keeps the SlotPeripheral default ($FF). Every driver is
// loaded from disk.
//
// What runs on it
// ---------------
// Anything written for the W5100's hardware TCP: **A2Stream**, **Contiki
// with the W5100 driver**, the Uthernet II builds of **IP65** (telnet65,
// irc65, wget65), **ADTPro**, and the various IRC/Gopher/HTTP clients in
// the modern Apple II scene. Because TCP and UDP run on host sockets,
// none of this needs libslirp or elevated privileges.

#ifndef POM2_UTHERNET_II_CARD_H
#define POM2_UTHERNET_II_CARD_H

#include "NetworkBackend.h"
#include "SlotPeripheral.h"
#include "W5100Device.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace pom2 {

class UthernetIICard final : public SlotPeripheral
{
public:
    bool sessionReplacementReady() const override { return false; }
    /// Only A0/A1 are decoded (`W5100.h:6`).
    static constexpr uint8_t kRegisterMask = 0x03;
    static constexpr uint8_t kRegMode    = 0x04 & kRegisterMask;   // 0
    static constexpr uint8_t kRegAddrHi  = 0x05 & kRegisterMask;   // 1
    static constexpr uint8_t kRegAddrLo  = 0x06 & kRegisterMask;   // 2
    static constexpr uint8_t kRegData    = 0x07 & kRegisterMask;   // 3

    /// Locally-administered default MAC, with 'U2' in the third octet so
    /// it is recognisable next to the Uthernet I's in a capture.
    static constexpr std::array<uint8_t, 6> kDefaultMac =
        { 0x02, 0x55, 0x32, 0x00, 0x00, 0x01 };

    /// Cycle budget between host-socket servicing passes. ~2048 cycles
    /// ≈ 500 Hz at 1.023 MHz: fast enough that a non-blocking connect is
    /// promoted to ESTABLISHED within ~2 ms of the handshake completing,
    /// cheap enough to leave unconditional in advanceCycles.
    static constexpr int kPollIntervalCycles = 2048;

    explicit UthernetIICard(int slot);

    std::string_view name() const override { return "Uthernet II"; }

    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;

    void onReset() override;
    void onPowerCycle() override;
    void advanceCycles(int cycles) override;

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    int getSlot() const { return slot_; }

    /// Adopt a host transport for the raw modes. TCP/UDP do not use it.
    void setBackend(std::unique_ptr<NetworkBackend> backend);
    NetworkBackend* backend() const { return backend_.get(); }

    W5100Device&       chip()       { return chip_; }
    const W5100Device& chip() const { return chip_; }

    /// Seed SHAR (the chip's source hardware address). Drivers normally
    /// program their own; this just means a freshly plugged card is not
    /// sitting at 00:00:00:00:00:00.
    void setMacAddress(const std::array<uint8_t, 6>& mac);

private:
    void resetChip(bool powerCycle);
    int                             slot_;
    W5100Device                     chip_;
    std::unique_ptr<NetworkBackend> backend_;
    int                             cyclesSincePoll_ = 0;
};

} // namespace pom2

#endif // POM2_UTHERNET_II_CARD_H
