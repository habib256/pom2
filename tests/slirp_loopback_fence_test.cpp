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

// POM2 — the libslirp half of the loopback perimeter (CLAUDE.md, "The guest is
// inside the loopback perimeter").
//
// The rule says a guest may not reach 127.0.0.1 unless the user opts in with
// `uthernet_allow_loopback`, and names `SlirpOptions::allowHostLoopback` as the
// fence. That option sets libslirp's `cfg.disable_host_loopback`, and that flag
// is NOT the whole fence: it only refuses the ALIAS route, a connection
// addressed to the virtual gateway 10.0.2.2 that `sotranslate_out` would
// re-open on 127.0.0.1. A frame that names 127.0.0.1 DIRECTLY is an ordinary
// foreign destination to slirp and it dials it.
//
// Measured on libslirp 4.9.3, 2026-09-09, with allowHostLoopback=false:
//     TCP SYN      -> 127.0.0.1:<ephemeral>  reached a host listener
//     UDP datagram -> 127.0.0.1:<ephemeral>  reached a host listener
//     TCP SYN      -> 0.0.0.0:<ephemeral>    reached it too (connect(0.0.0.0)
//                                            resolves to loopback)
//
// Every raw guest path lands in NetworkBackend::transmit — the CS8900A's TX
// buffer, the W5100's MACRAW (a whole guest-built Ethernet frame) and its
// IPRAW (guest-chosen Sn_DIPR, which sendDataIpRaw does not run through
// checkDestination) — so the drop belongs there and closes all three at once.
//
// What is pinned:
//   1. With the default options, a guest-built SYN and datagram naming
//      127.0.0.1 never reach a host listener on 127.0.0.1.
//   2. Same for 0.0.0.0 (the "this network" hole checkDestination already
//      names on the socket path).
//   3. The opt-in still works: allowHostLoopback=true lets the SYN through.
//      Without this case the test would pass on a backend that dropped
//      everything.
//   4. A DHCP DISCOVER (source 0.0.0.0, destination 255.255.255.255) still
//      gets an OFFER from slirp's own server — the fence tests the
//      DESTINATION, so broadcast and multicast stay open, which is what the
//      guest's address configuration needs.
//
// Everything here is loopback-only: the sockets are bound to 127.0.0.1 on
// kernel-assigned ports, and slirp's DHCP server is internal to the library.

#include "NetworkBackend.h"
#include "SlirpNetworkBackend.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(POM2_HAVE_SLIRP) || defined(_WIN32) || defined(__EMSCRIPTEN__)
int main()
{
    std::puts("SKIP: built without libslirp (or on a platform this pin does not cover)");
    return 77;   // ctest SKIP_RETURN_CODE
}
#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

namespace {

using namespace pom2;

uint16_t internetChecksum(const uint8_t* p, size_t n)
{
    uint32_t s = 0;
    while (n > 1) { uint16_t w; std::memcpy(&w, p, 2); s += w; p += 2; n -= 2; }
    if (n) s += *p;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return static_cast<uint16_t>(~s);
}

/// A guest-built Ethernet II / IPv4 frame from 10.0.2.15 to `dstIpNet`, either
/// a bare TCP SYN or a UDP datagram. This is exactly the shape a MACRAW W5100
/// socket or a CS8900A TX buffer hands NetworkBackend::transmit().
std::vector<uint8_t> guestFrame(uint32_t dstIpNet, uint16_t dstPort, bool udp)
{
    const uint32_t srcIpNet = htonl(0x0A00020F);          // 10.0.2.15
    const size_t   l4       = udp ? 8 + 4 : 20;
    std::vector<uint8_t> f(14 + 20 + l4, 0);

    static const uint8_t dmac[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };
    static const uint8_t smac[6] = { 0x02, 0x00, 0x00, 0x11, 0x22, 0x33 };
    std::memcpy(f.data(),     dmac, 6);
    std::memcpy(f.data() + 6, smac, 6);
    f[12] = 0x08; f[13] = 0x00;

    uint8_t* ip = f.data() + 14;
    ip[0] = 0x45;
    const uint16_t total = htons(static_cast<uint16_t>(20 + l4));
    std::memcpy(ip + 2, &total, 2);
    ip[6] = 0x40;                                          // DF
    ip[8] = 64;
    ip[9] = udp ? 17 : 6;
    std::memcpy(ip + 12, &srcIpNet, 4);
    std::memcpy(ip + 16, &dstIpNet, 4);
    const uint16_t ick = internetChecksum(ip, 20);
    std::memcpy(ip + 10, &ick, 2);

    uint8_t* l = f.data() + 34;
    const uint16_t sp = htons(40000), dp = htons(dstPort);
    std::memcpy(l,     &sp, 2);
    std::memcpy(l + 2, &dp, 2);

    std::vector<uint8_t> pseudo(12 + l4, 0);
    std::memcpy(pseudo.data(),     &srcIpNet, 4);
    std::memcpy(pseudo.data() + 4, &dstIpNet, 4);
    pseudo[9]  = udp ? 17 : 6;
    pseudo[10] = static_cast<uint8_t>(l4 >> 8);
    pseudo[11] = static_cast<uint8_t>(l4);

    if (udp) {
        const uint16_t ul = htons(static_cast<uint16_t>(l4));
        std::memcpy(l + 4, &ul, 2);
        std::memcpy(l + 8, "POM2", 4);
    } else {
        const uint32_t seq = htonl(0x11223344);
        std::memcpy(l + 4, &seq, 4);
        l[12] = 0x50;                                      // data offset 5
        l[13] = 0x02;                                      // SYN
        const uint16_t win = htons(4096);
        std::memcpy(l + 14, &win, 2);
    }
    std::memcpy(pseudo.data() + 12, l, l4);
    const uint16_t ck = internetChecksum(pseudo.data(), pseudo.size());
    std::memcpy(l + (udp ? 6 : 16), &ck, 2);
    return f;
}

struct Listeners {
    int      tcp     = -1;
    int      udp     = -1;
    uint16_t tcpPort = 0;
    uint16_t udpPort = 0;

    Listeners()
    {
        tcp = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(tcp >= 0);
        int one = 1;
        ::setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::bind(tcp, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        socklen_t al = sizeof(a);
        assert(::getsockname(tcp, reinterpret_cast<sockaddr*>(&a), &al) == 0);
        tcpPort = ntohs(a.sin_port);
        assert(::listen(tcp, 4) == 0);
        ::fcntl(tcp, F_SETFL, O_NONBLOCK);

        udp = ::socket(AF_INET, SOCK_DGRAM, 0);
        assert(udp >= 0);
        sockaddr_in b{};
        b.sin_family = AF_INET;
        b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::bind(udp, reinterpret_cast<sockaddr*>(&b), sizeof(b)) == 0);
        socklen_t bl = sizeof(b);
        assert(::getsockname(udp, reinterpret_cast<sockaddr*>(&b), &bl) == 0);
        udpPort = ntohs(b.sin_port);
        ::fcntl(udp, F_SETFL, O_NONBLOCK);
    }
    ~Listeners() { if (tcp >= 0) ::close(tcp); if (udp >= 0) ::close(udp); }
};

/// Push `frame` at the backend for a bounded number of poll rounds and report
/// whether the matching host listener ever saw it.
struct Outcome { bool tcp = false; bool udp = false; };

Outcome attempt(bool allowHostLoopback, uint32_t dstIpNet)
{
    Listeners ln;
    SlirpOptions options;
    options.allowHostLoopback = allowHostLoopback;
    auto backend = makeSlirpBackend("pom2test", options);
    assert(backend);

    const std::vector<uint8_t> syn = guestFrame(dstIpNet, ln.tcpPort, false);
    const std::vector<uint8_t> dgm = guestFrame(dstIpNet, ln.udpPort, true);

    Outcome out;
    for (int i = 0; i < 200 && !(out.tcp && out.udp); ++i) {
        if (i % 20 == 0) {
            backend->transmit(syn.data(), static_cast<int>(syn.size()));
            backend->transmit(dgm.data(), static_cast<int>(dgm.size()));
        }
        backend->poll();
        uint8_t drain[2048];
        while (backend->receive(drain, sizeof(drain)) > 0) {}

        sockaddr_in pa{};
        socklen_t   pl = sizeof(pa);
        const int c = ::accept(ln.tcp, reinterpret_cast<sockaddr*>(&pa), &pl);
        if (c >= 0) { out.tcp = true; ::close(c); }
        char m[64];
        if (::recv(ln.udp, m, sizeof(m), 0) > 0) out.udp = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return out;
}

// 1 + 2 — the default configuration refuses both spellings of "the host".
void testGuestCannotReachHostLoopback()
{
    const Outcome lo = attempt(false, htonl(0x7F000001));    // 127.0.0.1
    assert(!lo.tcp && "SYN naming 127.0.0.1 reached a host listener");
    assert(!lo.udp && "datagram naming 127.0.0.1 reached a host listener");

    const Outcome any = attempt(false, htonl(0x00000000));   // 0.0.0.0
    assert(!any.tcp && "SYN naming 0.0.0.0 reached a host listener");
    assert(!any.udp && "datagram naming 0.0.0.0 reached a host listener");
    std::puts("  guest frames to 127.0.0.1 and 0.0.0.0 are refused by default");
}

// 3 — the opt-in still opts in, so case 1 is not passing vacuously.
void testOptInStillReachesLoopback()
{
    const Outcome lo = attempt(true, htonl(0x7F000001));
    assert(lo.tcp && "uthernet_allow_loopback no longer lets the guest connect");
    std::puts("  allowHostLoopback=true still reaches 127.0.0.1");
}

// 4 — the fence tests the DESTINATION only, so the broadcast the guest needs
//     for its own address configuration is untouched.
void testDhcpStillWorks()
{
    SlirpOptions options;                        // defaults: fence armed
    auto backend = makeSlirpBackend("pom2test", options);
    assert(backend);

    static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x11, 0x22, 0x33 };
    constexpr size_t kBootp = 244;
    std::vector<uint8_t> f(14 + 20 + 8 + kBootp, 0);
    std::memset(f.data(), 0xFF, 6);              // broadcast
    std::memcpy(f.data() + 6, mac, 6);
    f[12] = 0x08; f[13] = 0x00;

    uint8_t* ip = f.data() + 14;
    ip[0] = 0x45;
    const uint16_t total = htons(static_cast<uint16_t>(20 + 8 + kBootp));
    std::memcpy(ip + 2, &total, 2);
    ip[8] = 64; ip[9] = 17;
    const uint32_t src = 0, dst = 0xFFFFFFFFu;   // 0.0.0.0 -> 255.255.255.255
    std::memcpy(ip + 12, &src, 4);
    std::memcpy(ip + 16, &dst, 4);
    const uint16_t ick = internetChecksum(ip, 20);
    std::memcpy(ip + 10, &ick, 2);

    uint8_t* u = f.data() + 34;
    const uint16_t sp = htons(68), dp = htons(67);
    const uint16_t ul = htons(static_cast<uint16_t>(8 + kBootp));
    std::memcpy(u, &sp, 2); std::memcpy(u + 2, &dp, 2); std::memcpy(u + 4, &ul, 2);

    uint8_t* b = f.data() + 42;
    b[0] = 1; b[1] = 1; b[2] = 6;                // BOOTREQUEST / ethernet / 6
    b[4] = 0xDE; b[5] = 0xAD; b[6] = 0xBE; b[7] = 0xEF;
    std::memcpy(b + 28, mac, 6);
    b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;
    b[240] = 53; b[241] = 1; b[242] = 1;         // DHCPDISCOVER
    b[243] = 255;

    bool offered = false;
    for (int i = 0; i < 200 && !offered; ++i) {
        if (i % 25 == 0) backend->transmit(f.data(), static_cast<int>(f.size()));
        backend->poll();
        uint8_t r[2048];
        int n;
        while ((n = backend->receive(r, sizeof(r))) > 0) {
            if (n > 42 + 240 && r[12] == 0x08 && r[13] == 0x00 &&
                r[23] == 17 && r[42] == 2) {
                offered = true;
                std::printf("  DHCP OFFER yiaddr = %u.%u.%u.%u\n",
                            r[58], r[59], r[60], r[61]);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(offered && "the loopback fence swallowed the guest's DHCP DISCOVER");
}

}  // namespace

int main()
{
    std::puts("slirp_loopback_fence");
    testGuestCannotReachHostLoopback();
    testOptInStillReachesLoopback();
    testDhcpStillWorks();
    std::puts("slirp_loopback_fence: OK");
    return 0;
}

#endif
