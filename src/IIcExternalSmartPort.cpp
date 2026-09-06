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
#include "IIcExternalSmartPort.h"

#include "SlotBus.h"
#include "SlotPeripheral.h"

#include <cstring>

namespace pom2 {

IIcExternalSmartPort::IIcExternalSmartPort(SlotBus* slots, int slot)
    : slots_(slots), slot_(slot)
{
    // The phase lines are the bus's control lines here: PH0 is REQ, and
    // PH0 + PH2 together is the bus reset the firmware issues before an
    // INIT scan ($C9E5 in the Liron dump; the //c's bank 1 is the same
    // code). Nothing mechanical hangs off this IWM, so nothing else needs
    // the callback.
    regs_.setPhasesCallback([this](uint8_t phases) { syncLines(phases); });
    regs_.setDevselCallback([](uint8_t) {});
    regs_.setSel35Callback([](bool) {});
}

bool IIcExternalSmartPort::bind()
{
    SlotPeripheral* card = slots_ ? slots_->peripheral(slot_) : nullptr;
    const int n = card ? card->smartPortBusUnitCount() : 0;
    for (int i = 0; i < SmartPortBusDevice::kMaxUnits; ++i)
        bus_.setUnit(i, (card && i < n) ? card->smartPortBusUnit(i) : nullptr);
    bus_.setUnitCount(n);
    return n > 0;
}

bool IIcExternalSmartPort::live()
{
    const bool bound = bind();
    // Media changing under a transaction (eject mid-WRITE, a mount right
    // after) must not leave half a frame in the responder to be spliced
    // with the next one: any change in which units hold media starts the
    // protocol over.
    unsigned mask = 0;
    for (int i = 0; i < SmartPortBusDevice::kMaxUnits && i < bus_.unitCount(); ++i)
        if (bus_.unitHasMedia(i)) mask |= 1u << i;
    if (mask != mediaMask_) {
        mediaMask_ = mask;
        bus_.busReset();
    }
    return enabled_ && bound && mask != 0;
}

bool IIcExternalSmartPort::addressed(uint8_t phases, uint8_t control)
{
    // PH1 (CA1) and PH3 (LSTRB) both high with the port enabled: what the
    // firmware's scan asserts before it polls SENSE, and something no disk
    // transaction ever does.
    return (phases & 0x02) && (phases & 0x08) && (control & 0x10);
}

void IIcExternalSmartPort::syncLines(uint8_t phases)
{
    if (phases == lastPhases_) return;
    lastPhases_ = phases;
    if ((phases & 0x05) == 0x05) bus_.busReset();
    bus_.reqChanged((phases & 0x01) != 0);
}

bool IIcExternalSmartPort::answer(uint8_t control, uint8_t iwmValue, uint8_t& out)
{
    switch (control & 0xC0) {
    case 0x00: {
        // Data register, read mode: the device's next byte, or $00 for
        // "nothing yet". Every SmartPort byte on the wire carries bit 7,
        // which is what the firmware's BPL loops are waiting for.
        uint8_t b = 0;
        out = bus_.hostReads(b) ? b : uint8_t{0x00};
        return true;
    }
    case 0x80:
        // Write handshake: latch free (bit 7), nothing draining (bit 6).
        out = 0x80;
        return true;
    case 0x40:
        // Status: the mode bits from the chip, and SENSE — the device's
        // ACK — in bit 7.
        out = static_cast<uint8_t>((iwmValue & 0x7F) | (bus_.sense() ? 0x80 : 0x00));
        return true;
    default:
        return false;
    }
}

void IIcExternalSmartPort::takeByte(const IWMDevice& iwm, uint8_t offset,
                                    uint8_t value)
{
    // The IWM's own test for a DATA write: Q6 + Q7 set, odd offset, drive
    // enabled — the mode register otherwise.
    if (!iwm.isIdle() && (iwm.control() & 0xC0) == 0xC0 && (offset & 1))
        bus_.hostWrote(value);
}

bool IIcExternalSmartPort::read(uint8_t offset, uint64_t cycles, uint8_t& out)
{
    // Track unconditionally — the control state must be right the moment a
    // device appears — answer only while live.
    regs_.tick(cycles);
    const uint8_t v = regs_.read(static_cast<uint8_t>(offset & 0xF));
    if (!live()) return false;
    if (!addressed(regs_.phases(), regs_.control()) && !bus_.active()) return false;
    return answer(regs_.control(), v, out);
}

bool IIcExternalSmartPort::write(uint8_t offset, uint8_t value, uint64_t cycles)
{
    regs_.tick(cycles);
    // Decided BEFORE the access: the byte that establishes write mode
    // (`STA $C0EF,X` with the first sync byte) is itself the first byte
    // of the packet, and the state after it is what the IWM uses to tell a
    // data write from a mode write.
    const bool forBus = live() &&
        (addressed(regs_.phases(), regs_.control()) || bus_.active());
    regs_.setBusCapture(forBus);
    regs_.write(static_cast<uint8_t>(offset & 0xF), value);
    if (forBus) takeByte(regs_, offset, value);
    return forBus;
}

bool IIcExternalSmartPort::sharedWantsWrite(const IWMDevice& iwm)
{
    return live() && (addressed(iwm.phases(), iwm.control()) || bus_.active());
}

void IIcExternalSmartPort::sharedAfterWrite(const IWMDevice& iwm, uint8_t offset,
                                            uint8_t value, bool forBus)
{
    syncLines(iwm.phases());
    if (forBus) takeByte(iwm, offset, value);
}

bool IIcExternalSmartPort::sharedAfterRead(const IWMDevice& iwm, uint8_t iwmValue,
                                           uint8_t& out)
{
    syncLines(iwm.phases());
    if (!live()) return false;
    if (!addressed(iwm.phases(), iwm.control()) && !bus_.active()) return false;
    return answer(iwm.control(), iwmValue, out);
}

namespace {
constexpr uint8_t kPortBlobMagic[4] = { 'X', 'S', 'P', '1' };
void put32(std::vector<uint8_t>& o, std::size_t v)
{
    for (int k = 0; k < 4; ++k) o.push_back(static_cast<uint8_t>(v >> (8 * k)));
}
std::size_t get32(const uint8_t* p)
{
    std::size_t v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<std::size_t>(p[k]) << (8 * k);
    return v;
}
}  // namespace

void IIcExternalSmartPort::appendSnapshotState(std::vector<uint8_t>& out) const
{
    // The private IWM's blob is opaque and variable in size, so it travels
    // behind its own length; the bus blob is self-delimiting; then the two
    // line-state bytes the port keeps for itself.
    out.insert(out.end(), kPortBlobMagic, kPortBlobMagic + 4);
    std::vector<uint8_t> iwm;
    regs_.appendSnapshotState(iwm);
    put32(out, iwm.size());
    out.insert(out.end(), iwm.begin(), iwm.end());
    bus_.appendSnapshotState(out);
    out.push_back(lastPhases_);
    out.push_back(static_cast<uint8_t>(mediaMask_));
}

std::size_t IIcExternalSmartPort::loadSnapshotState(const uint8_t* data, std::size_t n)
{
    // Magic first, reset second: a blob that is not ours must leave the
    // live port exactly as it was (see LironCard::loadSnapshotState).
    if (!data || n < 8 || std::memcmp(data, kPortBlobMagic, 4) != 0) return 0;
    reset();
    std::size_t i = 4;
    const std::size_t iwmLen = get32(data + i); i += 4;
    if (iwmLen > n - i) return 0;
    if (iwmLen && !regs_.loadSnapshotState(data + i, iwmLen)) { reset(); return 0; }
    i += iwmLen;
    const std::size_t busLen = bus_.loadSnapshotState(data + i, n - i);
    if (busLen == 0) { reset(); return 0; }
    i += busLen;
    if (i + 2 > n) { reset(); return 0; }
    lastPhases_ = data[i++];
    mediaMask_  = data[i++];
    return i;
}

void IIcExternalSmartPort::reset()
{
    regs_.reset();
    bus_.reset();
    lastPhases_ = 0;
    mediaMask_  = 0;
}

}  // namespace pom2
