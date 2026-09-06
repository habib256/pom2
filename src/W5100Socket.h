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

// Device-side host-I/O seam for the W5100 offload engine. It carries only
// protocol-neutral values and byte buffers: no BSD/Winsock handles, sockaddr,
// resolver threads or platform headers may cross into the device layer.

#ifndef POM2_W5100_SOCKET_H
#define POM2_W5100_SOCKET_H

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pom2 {

enum class W5100SocketKind { Tcp, Udp };

/// Whether a send should address the datagram (UDP) or write into an already
/// connected stream (TCP). The W5100's TX path calls both against one socket:
/// `sendDataToSocket` addresses, `flushPendingTx` continues an accepted-but-
/// unsent tail, and a plain `send()` is what the second one must use.
enum class W5100SendMode { Addressed, Stream };
enum class W5100ConnectResult { Connected, InProgress, Failed };
enum class W5100IoStatus { Ok, WouldBlock, Closed, Discarded, Failed };

struct W5100ReceiveResult {
    W5100IoStatus status = W5100IoStatus::Failed;
    std::size_t   bytes = 0;
    uint32_t      sourceAddress = 0;  // network byte order
    uint16_t      sourcePort = 0;     // network byte order
};

struct W5100SendResult {
    W5100IoStatus status = W5100IoStatus::Failed;
    std::size_t   bytes = 0;
};

class W5100HostSocket
{
public:
    virtual ~W5100HostSocket() = default;

    virtual W5100ConnectResult connect(uint32_t address,
                                       uint16_t port) = 0;
    virtual W5100ConnectResult pollConnect() = 0;
    virtual W5100ReceiveResult receive(uint8_t* data,
                                       std::size_t capacity) = 0;
    virtual W5100SendResult send(const uint8_t* data, std::size_t size,
                                 uint32_t address, uint16_t port,
                                 W5100SendMode mode) = 0;

    /// Poll for writability + SO_ERROR, used by the SYN_SENT walker.
    virtual bool writable() const = 0;

    /// Claim `port` as the socket's LOCAL port, the way the chip's Sn_PORT
    /// register does. UDP needs it or nothing unsolicited can ever arrive: a
    /// guest that opens a socket on port 68 to hear from a DHCP server, or on
    /// 123 for NTP, is told by the chip that it owns that port, and a host
    /// socket left unbound listens on an ephemeral one instead — the reply
    /// goes to a port nobody is reading.
    ///
    /// Not pure: a fake in a device test has no port to claim, and answering
    /// "done" is the right answer for it. Returns false only when the host
    /// refused the port (already in use), which is not fatal — the socket
    /// still works outbound.
    virtual bool bind(uint16_t /*port*/) { return true; }
};

class W5100SocketFactory
{
public:
    virtual ~W5100SocketFactory() = default;

    virtual std::unique_ptr<W5100HostSocket> open(W5100SocketKind kind) = 0;

    // Name resolution is deliberately NOT here yet. W5100Device's resolver is
    // an async mailbox with an in-flight cap, a bounded wait and its own
    // cache, wired to register reads; lifting it needs its own pass. The
    // socket half is separable, and it is the half a device test needs.
};

} // namespace pom2

#endif // POM2_W5100_SOCKET_H
