// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NatHostSockets — the host's real sockets behind NatHost.h, over
// SocketCompat.h (POSIX and Winsock from one source).
//
// Every trap this file steps around is recorded where it was first met:
// SocketCompat.h (traps 3, 5, 7) and W5100HostSockets.cpp. The two that
// matter most here: SIGPIPE is fatal by default and this code writes to peers
// that vanish (disableSigpipe + sendSocket), and on a datagram socket an
// error can describe one PACKET rather than the socket (errDatagramDiscard).

#include "NatHost.h"

#include "Logger.h"
#include "SocketCompat.h"
#include "SocketUtil.h"

namespace pom2 {
namespace nat {

#if POM2_HAS_SOCKETS

namespace {

sockaddr_in toSockaddr(const Endpoint& e)
{
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(e.ip);
    a.sin_port        = hostToNet16(e.port);
    return a;
}

class RealSocket final : public HostSocket
{
public:
    explicit RealSocket(socket_t fd) : fd_(fd) {}
    ~RealSocket() override
    {
        if (isValidSocket(fd_)) closeHostSocket(fd_);
    }
    RealSocket(const RealSocket&) = delete;
    RealSocket& operator=(const RealSocket&) = delete;

    bool connect(const Endpoint& remote) override
    {
        const sockaddr_in a = toSockaddr(remote);
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&a),
                      static_cast<socklen_c>(sizeof(a))) == 0)
            return true;
        const int e = lastSocketError();
        // Non-blocking: "in progress" is the normal answer, not a failure.
        return errInProgress(e) || errWouldBlock(e);
    }

    ConnectState pollConnect() override
    {
        if (waitSocket(fd_, SocketWait::Write, 0) != WaitResult::Ready)
            return ConnectState::Pending;
        return connectResult(fd_) == 0 ? ConnectState::Connected : ConnectState::Failed;
    }

    IoStatus send(const uint8_t* data, std::size_t size, std::size_t& sent) override
    {
        sent = 0;
        const iolen_t n = sendSocket(fd_, data, size);
        if (n >= 0) { sent = static_cast<std::size_t>(n); return IoStatus::Ok; }
        const int e = lastSocketError();
        return (errWouldBlock(e) || errInterrupted(e)) ? IoStatus::WouldBlock
                                                       : IoStatus::Failed;
    }

    IoStatus receive(uint8_t* buffer, std::size_t capacity, std::size_t& received) override
    {
        received = 0;
        const iolen_t n = recvSocket(fd_, buffer, capacity);
        if (n > 0) { received = static_cast<std::size_t>(n); return IoStatus::Ok; }
        if (n == 0) return IoStatus::Closed;
        const int e = lastSocketError();
        return (errWouldBlock(e) || errInterrupted(e)) ? IoStatus::WouldBlock
                                                       : IoStatus::Failed;
    }

    void shutdownSend() override
    {
#ifdef _WIN32
        ::shutdown(fd_, SD_SEND);
#else
        ::shutdown(fd_, SHUT_WR);
#endif
    }

    bool sendTo(const Endpoint& remote, const uint8_t* data, std::size_t size) override
    {
        const sockaddr_in a = toSockaddr(remote);
        return sendToSocket(fd_, data, size, reinterpret_cast<const sockaddr*>(&a),
                            static_cast<socklen_c>(sizeof(a))) >= 0;
    }

    bool receiveFrom(uint8_t* buffer, std::size_t capacity, std::size_t& received,
                     Endpoint& from) override
    {
        // A datagram that fails as a datagram (oversized; somebody else's
        // ICMP port-unreachable surfacing on the NEXT recvfrom under Winsock)
        // costs that datagram, not the flow: look at the next one. Bounded,
        // so a socket that only ever errors cannot hold the CPU thread.
        for (int attempt = 0; attempt < 4; ++attempt) {
            sockaddr_in source{};
            socklen_c   sourceLength = sizeof(source);
            const iolen_t n = ::recvfrom(fd_, reinterpret_cast<char*>(buffer),
                                         static_cast<int>(capacity), 0,
                                         reinterpret_cast<sockaddr*>(&source), &sourceLength);
            if (n >= 0) {
                received  = static_cast<std::size_t>(n);
                from.ip   = ntohl(source.sin_addr.s_addr);
                from.port = netToHost16(source.sin_port);
                return true;
            }
            if (!errDatagramDiscard(lastSocketError())) return false;
        }
        return false;
    }

private:
    socket_t fd_ = kInvalidSocket;
};

class RealNetwork final : public HostNetwork
{
public:
    std::unique_ptr<HostSocket> openTcp() override { return open(kSockStream, kIpProtoTcp); }

    std::unique_ptr<HostSocket> openUdp() override
    {
        return open(kSockDgram, kIpProtoUdp);
    }

private:
    static std::unique_ptr<HostSocket> open(int type, int protocol)
    {
        ensureSocketStack();
        socket_t fd = ::socket(AF_INET, type, protocol);
        if (!isValidSocket(fd)) return nullptr;
        disableSigpipe(fd);
        if (!setNonBlocking(fd)) { closeHostSocket(fd); return nullptr; }
        if (type == kSockDgram) {
            disableUdpConnReset(fd);
            // An explicit bind to port 0 so the socket has its ephemeral
            // port BEFORE the first send: Winsock refuses recvfrom on a
            // datagram socket that was never bound (WSAEINVAL).
            sockaddr_in local{};
            local.sin_family      = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port        = 0;
            if (::bind(fd, reinterpret_cast<const sockaddr*>(&local),
                       static_cast<socklen_c>(sizeof(local))) != 0) {
                closeHostSocket(fd);
                return nullptr;
            }
        }
        return std::make_unique<RealSocket>(fd);
    }
};

}  // namespace

std::unique_ptr<HostNetwork> makeHostNetwork()
{
    return std::make_unique<RealNetwork>();
}

#else   // no sockets in this build (the browser)

std::unique_ptr<HostNetwork> makeHostNetwork() { return nullptr; }

#endif

}  // namespace nat
}  // namespace pom2
