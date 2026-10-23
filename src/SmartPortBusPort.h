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
// SmartPortBusPort — the contract between a machine's disk-port decode and
// an intelligent SmartPort device answering on it.
//
// The 32 KB //c's own firmware talks to its external 3.5" drive as a
// UniDisk: a byte protocol over the IWM's registers at $C0E0-$C0EF. The
// memory map (MACHINE layer) decodes those addresses; what answers them is
// a device (DEVICES layer). This header is the seam between the two, kept
// free of both so `MemoryProfile` can name it without knowing the IWM
// register tracker or the packet decoder behind it — `IIcExternalSmartPort`
// is the implementation, `SmartPortBusDevice` the protocol.

#ifndef POM2_SMARTPORT_BUS_PORT_H
#define POM2_SMARTPORT_BUS_PORT_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

class IWMDevice;

class SmartPortBusPort {
public:
    virtual ~SmartPortBusPort() = default;

    /// A device is on the port: the responder is enabled and a unit behind
    /// it holds media. Without one the firmware's presence poll times out,
    /// it reports $28, and the machine boots from its internal drive — so
    /// an empty port must read as silence, not as an absent-media error.
    virtual bool live() = 0;

    /// A read of $C0E0 + offset. Returns true with the byte when the port
    /// answers it — the caller then owns nothing else about that access
    /// except the slot bus's side effects. False means "not mine": the
    /// machine's other controller on those addresses answers.
    virtual bool read(uint8_t offset, uint64_t cycles, uint8_t& out) = 0;

    /// A write of $C0E0 + offset. Always observed (the tracker must see
    /// every phase and control change); acted on only while live. Returns
    /// true when the byte was the bus's — the caller then keeps it from the
    /// machine's other controller on those addresses.
    virtual bool write(uint8_t offset, uint8_t value, uint64_t cycles) = 0;

    /// Machine reset, or a state restore the port's own state was not part
    /// of: abandon any transaction in flight.
    virtual void reset() = 0;

    /// Snapshot / rewind: the port's own registers and the transaction in
    /// flight. Absent from a blob → the port starts clean (the pre-snapshot
    /// behaviour); `loadSnapshotState` returns the bytes consumed, 0 if none.
    virtual void appendSnapshotState(std::vector<uint8_t>& /*out*/) const {}
    virtual std::size_t validateSnapshotState(const uint8_t*, std::size_t) const
    { return 0; }
    virtual std::size_t loadSnapshotState(const uint8_t* /*data*/, std::size_t /*n*/)
    { return 0; }

    // ── When the machine's own IWM carries the bus (the //c+) ────────────
    // The caller performs the access on that IWM and lets the port watch the
    // lines around it: same claim rule, same answers, no second tracker.
    /// Before a write: true when the byte is the bus's — the caller then
    /// sets `setBusCapture(true)` on the IWM so it stays out of the shifter.
    /// //c+ shared IWM: a Sony 3.5" drive is enabled ahead of this port on
    /// the chain. An enabled 3.5" drive forces every PHASE line low for the
    /// devices after it, so nothing downstream is reset or enabled by its
    /// register selects (CA0-CA2 / LSTRB are PH0-PH3). Set before each
    /// shared call below.
    virtual void setUpstreamSonyEnabled(bool /*enabled*/) {}
    virtual bool sharedWantsWrite(const IWMDevice& /*iwm*/) { return false; }
    /// After the IWM took the write.
    virtual void sharedAfterWrite(const IWMDevice& /*iwm*/, uint8_t /*offset*/,
                                  uint8_t /*value*/, bool /*forBus*/) {}
    /// After the IWM answered a read with `iwmValue`: true with the port's
    /// byte when the access was the bus's.
    virtual bool sharedAfterRead(const IWMDevice& /*iwm*/, uint8_t /*iwmValue*/,
                                 uint8_t& /*out*/) { return false; }
};

}  // namespace pom2

#endif  // POM2_SMARTPORT_BUS_PORT_H
