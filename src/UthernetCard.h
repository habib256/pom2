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

// UthernetCard — a2RetroSystems **Uthernet I** (catalog key `uthernet`).
// A CS8900A Ethernet MAC on a card, wired so its 16 I/O registers land
// directly in the slot's $C0nX device-select window.
//
// Port of MAME `src/devices/bus/a2bus/uthernet.cpp` (BSD-3, R. Belmont),
// which is itself a ~60-line shim over `machine/cs8900a.cpp`. POM2 keeps
// the same split: Cs8900aDevice is the chip, this file is the card.
//
// Address decode (`uthernet.cpp:47-52`)
// -------------------------------------
//   $C0nX (16 bytes) → cs8900a read/write with the low nibble verbatim.
//   No slot ROM. No expansion ROM. Nothing else on the card.
//
// The Uthernet I really is that simple: the CS8900A has an on-chip boot
// PROM interface but a2RetroSystems left it unpopulated, so the card is
// invisible to ProDOS and every driver has to be loaded from disk. That
// is why `slotRomRead` stays at the SlotPeripheral default ($FF open bus)
// — presenting a signature here would be wrong, and would make ProDOS
// probe a device that does not exist.
//
// Software that drives it
// -----------------------
//   * **IP65** — the 6502 TCP/IP stack (telnet65, wget65, IRC65).
//   * **Contiki 2.x** for the Apple II — web browser, telnet, IRC client.
//   * **ADTPro** over Ethernet.
// All of these carry their own TCP/IP stack and speak raw frames, so the
// card needs a `NetworkBackend` that can move Ethernet — in practice
// `SlirpNetworkBackend`. With no backend the card still probes and
// initialises correctly, it just never sees traffic.
//
// MAC address: the real card holds one in a serial EEPROM the CS8900A
// reads at power-on. POM2 has no EEPROM image, so the card seeds a
// locally-administered default (`kDefaultMac`) that the UI can override;
// IP65 and Contiki both program their own over it anyway.

#ifndef POM2_UTHERNET_CARD_H
#define POM2_UTHERNET_CARD_H

#include "Cs8900aDevice.h"
#include "NetworkBackend.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace pom2 {

class UthernetCard final : public SlotPeripheral
{
public:
    bool sessionReplacementReady() const override { return false; }
    /// Locally-administered default MAC (the 0x02 in the first octet says
    /// "not globally unique"), with 'U1' baked in so it is recognisable in
    /// a packet capture.
    static constexpr std::array<uint8_t, 6> kDefaultMac =
        { 0x02, 0x55, 0x31, 0x00, 0x00, 0x01 };

    /// How often the card services the host transport, in CPU cycles.
    /// ~2048 cycles ≈ 500 Hz at 1.023 MHz — far below any Ethernet
    /// deadline, and cheap enough to sit in advanceCycles unconditionally.
    static constexpr int kPollIntervalCycles = 2048;

    explicit UthernetCard(int slot);

    std::string_view name() const override { return "Uthernet I"; }

    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;

    void onReset() override;
    void advanceCycles(int cycles) override;

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    int getSlot() const { return slot_; }

    /// Adopt a host transport. Passing nullptr detaches (card keeps
    /// working, sees no traffic).
    void setBackend(std::unique_ptr<NetworkBackend> backend);
    NetworkBackend* backend() const { return backend_.get(); }

    /// Chip access for the status panel and the pinned tests.
    Cs8900aDevice&       chip()       { return chip_; }
    const Cs8900aDevice& chip() const { return chip_; }

    void setMacAddress(const std::array<uint8_t, 6>& mac) { chip_.setMacAddress(mac); }

private:
    int                             slot_;
    Cs8900aDevice                   chip_;
    std::unique_ptr<NetworkBackend> backend_;
    int                             cyclesSincePoll_ = 0;
};

} // namespace pom2

#endif // POM2_UTHERNET_CARD_H
