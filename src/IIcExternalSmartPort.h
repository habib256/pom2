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
// IIcExternalSmartPort — the plain //c's external disk port, as far as an
// intelligent 3.5" drive is concerned.
//
// On a //c the same IWM drives the internal 5.25" and the rear connector; the
// enable line picks the drive. POM2 keeps the 5.25" on `DiskIICard` (its LSS
// is the write authority everywhere, and a second controller on those soft
// switches once sent DOS 3.3 into seek storms — `iic_diskii_no_iwm_conflict`
// pins that the machine's shared IWM stays out of it). So this port carries
// its OWN IWM, used as nothing more than a register tracker — Q6/Q7, enable,
// SEL, the phases — with no mechanism attached, and a `SmartPortBusDevice`
// that answers the bytes. It claims an access only while the firmware is
// addressing the bus (PH1 + LSTRB high with the port enabled) or a
// transaction is in flight, and only while a unit holds media; every other
// access falls through to the Disk II exactly as before.
//
// The units come from whatever card sits in the machine's slot 5 — the
// built-in SmartPort the //c profiles plug there — through
// `SlotPeripheral::smartPortBusUnit`, so the media panel is unchanged: what
// the user mounts on "slot 5" is what the firmware finds on the port.
//
// The //c+ has the same connector and probes it the same way (its bank-1
// scan at $F223, fifty SENSE polls with SEL on drive 2), but there the
// machine's shared IWM already owns the port for the MIG-routed Sony drives,
// so this port rides along on that chip instead of tracking registers of its
// own — the `shared*` half of the contract. Its firmware numbers the external
// chain from 2, its internal drive being device 1; the responder takes the
// numbers the host assigns. The 16 KB //c (ROM 255) has no SmartPort
// firmware at all — its $C500 is not a disk page — so on that machine the
// rear connector carries a second 5.25" (`DiskIICard` drive 2) and a 3.5"
// is reachable only through the host-served substitute.

#ifndef POM2_IIC_EXTERNAL_SMARTPORT_H
#define POM2_IIC_EXTERNAL_SMARTPORT_H

#include "IWMDevice.h"
#include "SmartPortBusDevice.h"
#include "SmartPortBusPort.h"

#include <cstdint>

class SlotBus;

namespace pom2 {

class IIcExternalSmartPort final : public SmartPortBusPort {
public:
    static constexpr int kDefaultSlot = 5;

    explicit IIcExternalSmartPort(SlotBus* slots, int slot = kDefaultSlot);

    bool live() override;
    bool read(uint8_t offset, uint64_t cycles, uint8_t& out) override;
    bool write(uint8_t offset, uint8_t value, uint64_t cycles) override;
    void reset() override;
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    std::size_t loadSnapshotState(const uint8_t* data, std::size_t n) override;

    // ── The //c+ path: the machine's own IWM carries the bus ─────────────
    // On the //c+ the shared IWM already owns the port (MIG-routed Sony
    // drives), so the port does not track registers of its own there: the
    // caller performs the access on that IWM and lets the port watch the
    // lines around it. Same claim rule, same answers.
    /// Before a write: true when the byte is the bus's — the caller then
    /// sets `setBusCapture(true)` on the IWM so it stays out of the shifter.
    bool sharedWantsWrite(const IWMDevice& iwm) override;
    /// After the IWM took the write.
    void sharedAfterWrite(const IWMDevice& iwm, uint8_t offset, uint8_t value,
                          bool forBus) override;
    /// After the IWM answered a read with `iwmValue`: true with the port's
    /// byte when the access was the bus's.
    bool sharedAfterRead(const IWMDevice& iwm, uint8_t iwmValue, uint8_t& out) override;

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    const SmartPortBusDevice& device() const { return bus_; }
    const IWMDevice&          registers() const { return regs_; }

private:
    SlotBus*  slots_;
    int       slot_;
    bool      enabled_ = true;
    IWMDevice regs_;
    SmartPortBusDevice bus_;

    /// Refresh the unit list from the slot card. True when a card offers any.
    bool bind();
    static bool addressed(uint8_t phases, uint8_t control, bool rearIsDrive2);
    /// PH0 is REQ; PH0 + PH2 together is the bus reset ($C9E5 in the Liron
    /// dump). Own-IWM mode gets this from the phases callback, shared mode
    /// from a look at the lines after each access.
    void syncLines(uint8_t phases);
    bool answer(uint8_t control, uint8_t iwmValue, uint8_t& out);
    void takeByte(const IWMDevice& iwm, uint8_t offset, uint8_t value);
    uint8_t lastPhases_ = 0;
    unsigned mediaMask_ = 0;   // which units held media at the last look
};

}  // namespace pom2

#endif  // POM2_IIC_EXTERNAL_SMARTPORT_H
