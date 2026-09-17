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

// The Super Serial Card's real TCP transport, over loopback (TODO G5-13).
//
// `ssc_transport_seam` drives the card through a fake transport, which is
// the point of the seam — and left `SuperSerialTcpTransport.cpp` at 0 %
// coverage: the listener, the accept loop, the non-blocking pump and the
// shutdown that must not hang were never executed by any test. This one
// opens a real socket on 127.0.0.1 and checks, in raw mode:
//   1. a client's bytes reach the ACIA receive register, in order;
//   2. bytes the guest transmits reach the client;
//   3. a disconnect is seen and a second client is served;
//   4. stop() returns promptly with a client still attached, and the port
//      is closed afterwards — and a restart on the same port works;
//   5. a port somebody else holds makes start() fail cleanly.

#include "SuperSerialCard.h"
#include "SuperSerialTcpTransport.h"
#include "SuperSerialTransport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr uint8_t kAciaData    = 0x8;
constexpr uint8_t kAciaStatus  = 0x9;
constexpr uint8_t kAciaCommand = 0xA;

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    else       std::printf("  ok: %s\n", what);
}

uint16_t freePort()
{
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(s);
    return ntohs(a.sin_port);
}

int connectTo(uint16_t port)
{
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(s);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return s;
}

/// Everything the guest can read from the ACIA within `ms`.
std::string guestReads(SuperSerialCard& card, std::size_t want, int ms)
{
    std::string got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (got.size() < want && std::chrono::steady_clock::now() < until) {
        if (card.deviceSelectRead(kAciaStatus) & 0x08)
            got.push_back(static_cast<char>(card.deviceSelectRead(kAciaData)));
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return got;
}

std::string clientReads(int fd, std::size_t want, int ms)
{
    timeval tv{0, 20000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    char buf[64];
    while (got.size() < want && std::chrono::steady_clock::now() < until) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) got.append(buf, static_cast<std::size_t>(n));
    }
    return got;
}

}  // namespace

int main()
{
    SuperSerialCard card(2);
    card.setRawMode(true);
    auto transport = pom2::makeSuperSerialTcpTransport(card, 2);
    const uint16_t port = freePort();
    expect(transport->start(port), "the listener starts on a free loopback port");
    expect(transport->isListening() && transport->port() == port, "…and reports it");

    // 1. client → guest
    int c = connectTo(port);
    expect(c >= 0, "a client connects");
    const std::string msg = "HELLO\x01\xFF";
    ::send(c, msg.data(), msg.size(), 0);
    expect(guestReads(card, msg.size(), 3000) == msg,
           "the client's bytes reach the ACIA receive register, raw and in order");

    // 2. guest → client
    card.deviceSelectWrite(kAciaCommand, 0x01);   // DTR: the port is open
    for (char ch : std::string("OK\r"))
        card.deviceSelectWrite(kAciaData, static_cast<uint8_t>(ch));
    expect(clientReads(c, 3, 3000) == "OK\r", "the guest's bytes reach the client");

    // 3. a disconnect, then a second client
    ::close(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c = connectTo(port);
    expect(c >= 0, "a second client connects after the first left");
    ::send(c, "2", 1, 0);
    expect(guestReads(card, 1, 3000) == "2", "…and is served");

    // 4. stop() with a client attached, then restart on the same port
    const auto t0 = std::chrono::steady_clock::now();
    transport->stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    expect(ms < 2000, "stop() returns promptly with a client attached");
    expect(!transport->isListening(), "…and the transport says so");
    ::close(c);
    expect(connectTo(port) < 0, "the port is closed after stop()");
    expect(transport->start(port), "a restart on the same port works");
    int c2 = connectTo(port);
    expect(c2 >= 0, "…and accepts a client");
    if (c2 >= 0) ::close(c2);
    transport->stop();

    // 5. a port somebody else is listening on
    const int holder = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(holder, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::listen(holder, 1);
    socklen_t len = sizeof(a);
    ::getsockname(holder, reinterpret_cast<sockaddr*>(&a), &len);
    auto other = pom2::makeSuperSerialTcpTransport(card, 2);
    expect(!other->start(ntohs(a.sin_port)), "a port in use makes start() fail");
    expect(!other->isListening(), "…and leaves the transport idle");
    other->stop();
    ::close(holder);

    if (failures) { std::printf("ssc_tcp_transport: %d failure(s)\n", failures); return 1; }
    std::printf("ssc_tcp_transport OK\n");
    return 0;
}
