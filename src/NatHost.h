// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NatHost — the host's network stack as the built-in virtual network uses
// it: non-blocking TCP and UDP sockets, and nothing else.
//
// A seam, for the reason `W5100Socket.h` is one: the relays (NatTcp, NatUdp)
// are state machines worth testing exhaustively, and a test that needs a real
// peer on a real socket tests the kernel's timing as much as the relay. Here
// the test IS the remote peer. `makeHostNetwork()` (NatHostSockets.cpp) is
// the production implementation over SocketCompat.h — POSIX and Winsock.
//
// No call waits. The backend is pumped from the CPU thread under stateMutex
// (NetworkBackend.h), so an operation that would block reports that and
// returns.
//
// Addresses and ports are HOST byte order here, as everywhere in the NAT.
//
// [D] Shape after Casso `Seams/IHostNetwork.h` @ 2d899adf (MIT). Not taken:
//     its Listen/Accept (POM2's built-in network forwards no inbound port)
//     and its DNS-server query (POM2 answers DNS from the host RESOLVER
//     instead of relaying to a server — NatDns.h says why).

#ifndef POM2_NAT_HOST_H
#define POM2_NAT_HOST_H

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pom2 {
namespace nat {

struct Endpoint {
    uint32_t ip   = 0;
    uint16_t port = 0;
};

enum class ConnectState : uint8_t { Pending, Connected, Failed };

enum class IoStatus : uint8_t {
    Ok,          ///< bytes moved (possibly fewer than asked)
    WouldBlock,  ///< nothing moved; try again later
    Closed,      ///< receive only: the peer finished sending (orderly EOF)
    Failed,      ///< the connection is gone
};

class HostSocket
{
public:
    virtual ~HostSocket() = default;

    // ── TCP ───────────────────────────────────────────────────────────────
    /// Start a connect. False when it could not even be started.
    virtual bool         connect(const Endpoint& remote) = 0;
    virtual ConnectState pollConnect() = 0;
    virtual IoStatus     send(const uint8_t* data, std::size_t size, std::size_t& sent) = 0;
    virtual IoStatus     receive(uint8_t* buffer, std::size_t capacity, std::size_t& received) = 0;
    /// Half-close: this side has no more to send; receiving goes on.
    virtual void         shutdownSend() = 0;

    // ── UDP ───────────────────────────────────────────────────────────────
    /// False when the datagram could not be sent; UDP permits the loss.
    virtual bool sendTo(const Endpoint& remote, const uint8_t* data, std::size_t size) = 0;
    /// False when nothing is pending.
    virtual bool receiveFrom(uint8_t* buffer, std::size_t capacity,
                             std::size_t& received, Endpoint& from) = 0;
};

class HostNetwork
{
public:
    virtual ~HostNetwork() = default;
    /// nullptr when the host refused the socket (descriptor exhaustion, …).
    virtual std::unique_ptr<HostSocket> openTcp() = 0;
    /// Bound to an EPHEMERAL port on every interface. The guest does not get
    /// to choose a port on the host (W5100Device's `localPortAllowed` draws
    /// the same line for the Uthernet II).
    virtual std::unique_ptr<HostSocket> openUdp() = 0;
};

/// The host's real sockets, or nullptr on a build with none (the browser).
std::unique_ptr<HostNetwork> makeHostNetwork();

}  // namespace nat
}  // namespace pom2

#endif  // POM2_NAT_HOST_H
