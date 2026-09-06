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

// The production W5100 socket factory. Every line of hardening here was moved
// out of W5100Device.cpp rather than rewritten: each one exists because of a
// specific failure, recorded at its site.

#include "W5100HostSockets.h"

#include "Logger.h"
#include "SocketCompat.h"
#include "SocketUtil.h"

namespace pom2 {
namespace {

#if POM2_HAS_SOCKETS

class HostSocket final : public W5100HostSocket
{
public:
    HostSocket(socket_t fd, W5100SocketKind kind) : fd_(fd), kind_(kind) {}

    ~HostSocket() override
    {
        if (isValidSocket(fd_)) closeHostSocket(fd_);   // takes socket_t&
    }

    HostSocket(const HostSocket&) = delete;
    HostSocket& operator=(const HostSocket&) = delete;

    W5100ConnectResult connect(uint32_t address, uint16_t port) override
    {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = address;
        destination.sin_port = port;

        const int res = ::connect(
            fd_, reinterpret_cast<sockaddr*>(&destination),
            static_cast<socklen_c>(sizeof(destination)));
        if (res == 0) return W5100ConnectResult::Connected;

        // Non-blocking connect: EINPROGRESS / WSAEWOULDBLOCK is the normal
        // answer, not a failure. The SYN_SENT walker polls pollConnect().
        const int e = lastSocketError();
        if (errInProgress(e) || errWouldBlock(e))
            return W5100ConnectResult::InProgress;
        // Report WHY here: this is the only layer that still has the errno.
        // The device above it deliberately names no host type, so a bare
        // "connect() failed" was all the guest's owner could see.
        log().warn("W5100", "connect() failed: " + socketErrorText(e));
        return W5100ConnectResult::Failed;
    }

    W5100ConnectResult pollConnect() override
    {
        if (waitSocket(fd_, SocketWait::Write, 0) != WaitResult::Ready)
            return W5100ConnectResult::InProgress;
        return connectResult(fd_) == 0 ? W5100ConnectResult::Connected
                                       : W5100ConnectResult::Failed;
    }

    bool writable() const override
    {
        return waitSocket(fd_, SocketWait::Write, 0) == WaitResult::Ready;
    }

    bool bind(uint16_t port) override
    {
        // Bind policy first: without it a UDP port the guest used a moment ago
        // (a reset, a re-open, a previous POM2 session) can still be held by
        // the kernel and the bind fails for a socket nobody is using.
        //
        // Through setListenerBindPolicy(), NOT a raw SO_REUSEADDR — the option
        // does not mean the same thing on the two stacks. On Winsock it lets
        // ANY local process bind an address this socket already holds and
        // collect the traffic (SocketCompat.h, trap 6), so the POSIX idiom
        // spelled out here turned a guest's DHCP/NTP port into something a
        // local program could hijack. The helper picks SO_EXCLUSIVEADDRUSE
        // there and SO_REUSEADDR on POSIX.
        setListenerBindPolicy(fd_);

        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port        = hostToNet16(port);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&local),
                   static_cast<socklen_c>(sizeof(local))) == 0)
            return true;

        // Not fatal, and deliberately not a close: the guest can still talk
        // OUTBOUND from the ephemeral port the kernel will pick, which is
        // every request/response exchange it is likely to attempt. Only
        // unsolicited inbound traffic is lost, and that is what the log line
        // is for — the alternative, refusing the OPEN, would break working
        // configurations because some unrelated host program holds port 68.
        log().warn("W5100", "could not bind local port " +
                            std::to_string(port) + ": " +
                            lastSocketErrorText() +
                            " — outbound still works, unsolicited inbound "
                            "datagrams will not arrive");
        return false;
    }

    W5100ReceiveResult receive(uint8_t* data, std::size_t capacity) override
    {
        W5100ReceiveResult out;
        sockaddr_in source{};
        socklen_c sourceLen = sizeof(source);

        const iolen_t got = ::recvfrom(
            fd_, reinterpret_cast<char*>(data), static_cast<int>(capacity), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLen);
        if (got >= 0) {
            out.status = W5100IoStatus::Ok;
            out.bytes = static_cast<std::size_t>(got);
            out.sourceAddress = source.sin_addr.s_addr;
            out.sourcePort = source.sin_port;
            return out;
        }

        const int e = lastSocketError();
        if (errWouldBlock(e) || errInterrupted(e)) {
            out.status = W5100IoStatus::WouldBlock;
            return out;
        }
        // On a datagram socket a failure can describe the PACKET rather than
        // the socket: Winsock reports an oversized datagram (WSAEMSGSIZE) and
        // somebody else's ICMP port-unreachable (WSAECONNRESET, raised on the
        // NEXT recvfrom of an unconnected UDP socket) through the same channel
        // as a genuine fault. Those must cost one datagram, not the guest's
        // socket. On TCP that very same ECONNRESET IS the connection dying, so
        // the tolerance is strictly UDP-only. SocketCompat.h, trap 7.
        if (kind_ == W5100SocketKind::Udp && errDatagramDiscard(e)) {
            out.status = W5100IoStatus::Discarded;
            return out;
        }
        out.status = W5100IoStatus::Failed;
        return out;
    }

    W5100SendResult send(const uint8_t* data, std::size_t size,
                         uint32_t address, uint16_t port,
                         W5100SendMode mode) override
    {
        W5100SendResult out;
        iolen_t res = 0;
        if (mode == W5100SendMode::Stream) {
            // Continuing an already-accepted tail on a connected socket.
            res = sendSocket(fd_, data, size);
        } else {
            sockaddr_in destination{};
            destination.sin_family = AF_INET;
            destination.sin_addr.s_addr = address;
            destination.sin_port = port;
            // sendToSocket, not ::sendto: MSG_NOSIGNAL where the platform has
            // it, paired with the SO_NOSIGPIPE armed at creation where it does
            // not. SIGPIPE is fatal by default, and this socket writes to a
            // peer that can vanish at any moment — sendData deliberately keeps
            // sending in CLOSE_WAIT, so the ordinary HTTP/1.0 or IRC shape
            // (server answers, server closes, guest sends again) has the peer
            // answer RST and the next send kill the whole POM2 process: no log
            // line, no dialog, the emulated machine and any un-written-back
            // disk state simply gone.
            res = sendToSocket(fd_, data, size,
                               reinterpret_cast<const sockaddr*>(&destination),
                               static_cast<socklen_c>(sizeof(destination)));
        }

        if (res >= 0) {
            out.status = W5100IoStatus::Ok;
            out.bytes = static_cast<std::size_t>(res);
            return out;
        }
        const int e = lastSocketError();
        out.status = (errWouldBlock(e) || errInterrupted(e))
                         ? W5100IoStatus::WouldBlock
                         : W5100IoStatus::Failed;
        return out;
    }

private:
    socket_t fd_ = kInvalidSocket;
    W5100SocketKind kind_ = W5100SocketKind::Tcp;
};

#endif // POM2_HAS_SOCKETS

class HostFactory final : public W5100SocketFactory
{
public:
    std::unique_ptr<W5100HostSocket> open(W5100SocketKind kind) override
    {
#if !POM2_HAS_SOCKETS
        (void)kind;
        // Emscripten has no usable BSD-socket API, so these modes stay CLOSED.
        // The register model, the RX/TX rings and MACRAW/IPRAW (which go
        // through NetworkBackend, not through sockets) are unaffected.
        return nullptr;
#else
        ensureSocketStack();   // Winsock needs WSAStartup; no-op elsewhere

        const int type = (kind == W5100SocketKind::Udp) ? SOCK_DGRAM
                                                        : SOCK_STREAM;
        const int protocol = (kind == W5100SocketKind::Udp) ? IPPROTO_UDP
                                                            : IPPROTO_TCP;
        const socket_t fd = ::socket(AF_INET, type, protocol);
        if (!isValidSocket(fd)) {
            log().warn("W5100", "socket() failed: " + lastSocketErrorText());
            return nullptr;
        }
        disableSigpipe(fd);
        // Non-blocking is mandatory: every call runs on the CPU thread under
        // stateMutex and must not wait on the network.
        if (!setNonBlocking(fd)) {
            log().warn("W5100",
                       "non-blocking mode failed: " + lastSocketErrorText());
            closeHostSocketValue(fd);   // fd is const here
            return nullptr;
        }
        // See the receive() note: off, so one datagram to a closed port stays
        // one lost datagram (SocketCompat.h, trap 7). No-op on POSIX.
        if (kind == W5100SocketKind::Udp) disableUdpConnReset(fd);

        return std::make_unique<HostSocket>(fd, kind);
#endif
    }
};

} // namespace

std::unique_ptr<W5100SocketFactory> makeHostW5100SocketFactory()
{
    return std::make_unique<HostFactory>();
}

} // namespace pom2
