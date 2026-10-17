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

// SuperSerialTransport — the host side of the Super Serial Card's bridge.
//
// The card owns the ACIA registers, the RX/TX rings, the telnet parser and
// the emulated line rate. A transport owns whatever carries bytes to and from
// a peer — a loopback TCP listener and its worker thread in the production
// case — and reaches the card only through the hooks on SuperSerialCard
// marked "Transport hooks".
//
// The split exists so the card can be driven with no socket and no thread:
// a fake transport calls the same hooks directly.

#ifndef POM2_SUPER_SERIAL_TRANSPORT_H
#define POM2_SUPER_SERIAL_TRANSPORT_H

#include <cstdint>
#include <string>

namespace pom2 {

class SuperSerialTransport
{
public:
    virtual ~SuperSerialTransport() = default;

    /// Begin carrying bytes on `port`. Returns false when the endpoint could
    /// not be acquired (a bind failure, say); the card stays usable, it just
    /// has nothing on the far end.
    virtual bool start(uint16_t port) = 0;

    /// Stop, and do not return until the transport's own threads are joined.
    /// Must be safe to call when nothing is running.
    virtual void stop() = 0;

    virtual bool isListening() const = 0;
    virtual uint16_t port() const = 0;
    virtual bool isVsDrive() const { return false; }
    virtual std::string vsDrivePath(int) const { return {}; }
    virtual bool vsDriveReadOnly(int) const { return false; }
};

} // namespace pom2

#endif // POM2_SUPER_SERIAL_TRANSPORT_H
