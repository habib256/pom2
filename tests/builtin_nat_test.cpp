// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// builtin_nat — POM2's own virtual network (BuiltinNatBackend.h): the
// transport that gives the Uthernet I a network on a build with no libslirp.
//
// The test plays BOTH ends. It is the guest — it writes Ethernet frames the
// way an Apple II's IP stack would and reads the ones that come back — and,
// through the NatHost.h seam, it is the remote peer too: a fake host network
// records what the backend opens and sends, and hands back what a server
// would answer. The clock is the test's as well, so "one second later" is
// exact. A last part swaps in the host's REAL sockets on 127.0.0.1.
//
// What is pinned:
//   1. the codec: checksums, padding, options accepted, fragments refused;
//   2. THE FENCE, as a table — the gateway is not the host's loopback, and
//      127/8, 0/8, 169.254/16, 224/4, 240/4 are refused; one opt-in opens
//      the loopback and nothing else. Without the opt-in case the test
//      would pass on a backend that refused everything;
//   3. ARP (proxy for everything but the guest), ping, DHCP;
//   4. DNS answered from the host resolver: an answer, a late answer, a
//      failure, a timeout, and — the point of `peek` — ONE host lookup
//      however often the guest repeats its question;
//   5. UDP: out, back from the address written to, NOT back from another;
//   6. TCP: the handshake held until the host connects, data both ways
//      within the guest's window and MSS, retransmission, both closes,
//      refusal as a reset, and a fenced destination that opens no socket;
//   7. the same over real sockets, where a listener on 127.0.0.1 is reached
//      with the opt-in and never without it.

#include "BuiltinNatBackend.h"
#include "NatDhcp.h"
#include "NatDns.h"
#include "NatHost.h"
#include "NatPacket.h"
#include "NetworkBackend.h"
#include "SocketCompat.h"
#include "W5100Resolver.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pom2;
using namespace pom2::nat;

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

constexpr Mac      kGuestMac = { 0x00, 0x08, 0xDC, 0x12, 0x34, 0x56 };
constexpr uint32_t kPublicIp = 0x5DB8D822;      // 93.184.216.34
constexpr uint32_t kOtherIp  = 0x08080808;      // 8.8.8.8

// ── The remote peer: a fake host network ─────────────────────────────────

struct World;

struct FakeSocket final : HostSocket {
    World&       world;
    bool         tcp;
    Endpoint     remote;
    bool         connectCalled = false;
    ConnectState connectState;
    Bytes        fromGuest;                  ///< TCP bytes the relay wrote
    std::deque<uint8_t> toGuest;             ///< TCP bytes the "server" sends
    bool         eof = false, broken = false, shutdownSent = false;
    std::size_t  sendCap = SIZE_MAX;         ///< bytes one send() accepts
    std::vector<std::pair<Endpoint, Bytes>> datagramsOut;
    std::deque<std::pair<Endpoint, Bytes>>  datagramsIn;

    FakeSocket(World& w, bool isTcp);
    ~FakeSocket() override;

    bool connect(const Endpoint& r) override { connectCalled = true; remote = r; return true; }
    ConnectState pollConnect() override { return connectState; }
    IoStatus send(const uint8_t* data, std::size_t size, std::size_t& sent) override
    {
        sent = 0;
        if (broken) return IoStatus::Failed;
        if (sendCap == 0) return IoStatus::WouldBlock;
        sent = size < sendCap ? size : sendCap;
        fromGuest.insert(fromGuest.end(), data, data + sent);
        return IoStatus::Ok;
    }
    IoStatus receive(uint8_t* buffer, std::size_t capacity, std::size_t& received) override
    {
        received = 0;
        if (broken) return IoStatus::Failed;
        if (toGuest.empty()) return eof ? IoStatus::Closed : IoStatus::WouldBlock;
        while (received < capacity && !toGuest.empty()) {
            buffer[received++] = toGuest.front();
            toGuest.pop_front();
        }
        return IoStatus::Ok;
    }
    void shutdownSend() override { shutdownSent = true; }
    bool sendTo(const Endpoint& r, const uint8_t* data, std::size_t size) override
    {
        datagramsOut.emplace_back(r, Bytes(data, data + size));
        return true;
    }
    bool receiveFrom(uint8_t* buffer, std::size_t capacity, std::size_t& received,
                     Endpoint& from) override
    {
        if (datagramsIn.empty()) return false;
        from = datagramsIn.front().first;
        const Bytes& d = datagramsIn.front().second;
        received = d.size() < capacity ? d.size() : capacity;
        std::memcpy(buffer, d.data(), received);
        datagramsIn.pop_front();
        return true;
    }
};

struct World {
    std::vector<FakeSocket*> live;
    int          opened = 0, closed = 0;
    ConnectState nextConnect = ConnectState::Pending;
    FakeSocket*  last() const { return live.empty() ? nullptr : live.back(); }
};

FakeSocket::FakeSocket(World& w, bool isTcp)
    : world(w), tcp(isTcp), connectState(w.nextConnect)
{
    world.live.push_back(this);
    ++world.opened;
}

FakeSocket::~FakeSocket()
{
    for (std::size_t i = 0; i < world.live.size(); ++i)
        if (world.live[i] == this) { world.live.erase(world.live.begin() + i); break; }
    ++world.closed;
}

struct FakeNetwork final : HostNetwork {
    World& world;
    explicit FakeNetwork(World& w) : world(w) {}
    std::unique_ptr<HostSocket> openTcp() override { return std::make_unique<FakeSocket>(world, true); }
    std::unique_ptr<HostSocket> openUdp() override { return std::make_unique<FakeSocket>(world, false); }
};

struct Names {
    std::map<std::string, W5100Resolver::Result> answers;   ///< absent = pending
    int resolves = 0, peeks = 0;
};

struct FakeResolver final : W5100Resolver {
    Names& names;
    explicit FakeResolver(Names& n) : names(n) {}
    Result resolve(const std::string& name, int) override
    {
        ++names.resolves;
        return lookup(name);
    }
    Result peek(const std::string& name) override
    {
        ++names.peeks;
        return lookup(name);
    }
    void poll() override {}
    void clearCache() override {}
    Result lookup(const std::string& name) const
    {
        const auto it = names.answers.find(name);
        return it == names.answers.end() ? Result{ Status::Pending, 0 } : it->second;
    }
};

// ── The guest ────────────────────────────────────────────────────────────

struct Rig {
    World    world;
    Names    names;
    uint64_t clock = 1000;
    std::unique_ptr<NetworkBackend> net;

    explicit Rig(bool allowLoopback = false, bool restricted = false)
    {
        BuiltinNatOptions o;
        o.allowHostLoopback = allowLoopback;
        o.restricted        = restricted;
        o.nowMs             = [this] { return clock; };
        net = makeBuiltinNatBackend(o, std::make_unique<FakeNetwork>(world),
                                    std::make_unique<FakeResolver>(names));
    }

    void sendIp(uint32_t destination, uint8_t protocol, const Bytes& payload,
                uint32_t source = kGuestIp)
    {
        const Bytes frame = buildEthernet({ 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 }, kGuestMac,
                                          kEtherTypeIpv4,
                                          buildIpv4(source, destination, protocol, 7, payload));
        net->transmit(frame.data(), static_cast<int>(frame.size()));
    }
    void sendUdp(uint32_t destination, uint16_t sourcePort, uint16_t destinationPort,
                 const Bytes& payload, uint32_t source = kGuestIp)
    {
        sendIp(destination, kProtoUdp,
               buildUdp(source, destination, sourcePort, destinationPort, payload.data(),
                        payload.size()),
               source);
    }
    void sendTcp(uint32_t destination, uint16_t sourcePort, uint16_t destinationPort,
                 uint8_t flags, uint32_t sequence, uint32_t acknowledgment,
                 const Bytes& payload = {}, uint16_t window = 2048, uint16_t mss = 0)
    {
        TcpSegment s;
        s.sourceIp = kGuestIp;       s.destinationIp = destination;
        s.sourcePort = sourcePort;   s.destinationPort = destinationPort;
        s.sequence = sequence;       s.acknowledgment = acknowledgment;
        s.flags = flags;             s.window = window;
        s.mss = mss;                 s.payload = payload;
        sendIp(destination, kProtoTcp, buildTcp(s));
    }

    /// Everything the backend has for the guest, raw.
    std::vector<Bytes> frames()
    {
        std::vector<Bytes> out;
        uint8_t buf[kMaxEthFrame];
        for (int n; (n = net->receive(buf, sizeof buf)) > 0;) out.emplace_back(buf, buf + n);
        return out;
    }
    void pump(uint64_t advanceMs = 0)
    {
        clock += advanceMs;
        net->poll();
    }
    /// The TCP segments waiting for the guest (and nothing else expected).
    std::vector<TcpSegment> segments()
    {
        std::vector<TcpSegment> out;
        for (const Bytes& f : frames()) {
            EthernetFrame e;
            Ipv4Packet    ip;
            TcpSegment    s;
            if (parseEthernet(f.data(), f.size(), e) && e.etherType == kEtherTypeIpv4 &&
                parseIpv4(e.payload, ip) && ip.protocol == kProtoTcp && parseTcp(ip, s))
                out.push_back(s);
            else {
                std::printf("FAIL an unexpected or malformed frame reached the guest\n");
                ++failures;
            }
        }
        return out;
    }
    struct Datagram { uint32_t sourceIp; uint32_t destinationIp; uint16_t sourcePort, destinationPort; Bytes payload; Mac ethDst; };
    std::vector<Datagram> datagrams()
    {
        std::vector<Datagram> out;
        for (const Bytes& f : frames()) {
            EthernetFrame e;
            Ipv4Packet    ip;
            UdpDatagram   u;
            if (parseEthernet(f.data(), f.size(), e) && e.etherType == kEtherTypeIpv4 &&
                parseIpv4(e.payload, ip) && ip.protocol == kProtoUdp && parseUdp(ip, u))
                out.push_back({ ip.source, ip.destination, u.sourcePort, u.destinationPort,
                                Bytes(u.payload.data, u.payload.data + u.payload.size),
                                e.destination });
            else {
                std::printf("FAIL an unexpected or malformed frame reached the guest\n");
                ++failures;
            }
        }
        return out;
    }
};

Bytes text(const char* s) { return Bytes(s, s + std::strlen(s)); }

// ── 1. The codec ─────────────────────────────────────────────────────────

void testCodec()
{
    std::printf("[1] codec\n");
    // RFC 1071 § 3: the worked example sums to 0xddf2, checksum 0x220d.
    const uint8_t rfc[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    CHECK(checksum(rfc, sizeof rfc) == 0x220d);
    const uint8_t odd[] = { 0x01, 0x02, 0x03 };          // odd length: padded with zero
    CHECK(checksum(odd, 3) == static_cast<uint16_t>(~(0x0102 + 0x0300)));

    // A small frame is padded to 60; the IP total length trims it again.
    const Bytes udp   = buildUdp(kGuestIp, kPublicIp, 1234, 7, rfc, 3);
    const Bytes ip    = buildIpv4(kGuestIp, kPublicIp, kProtoUdp, 1, udp);
    const Bytes frame = buildEthernet(kBroadcastMac, kGuestMac, kEtherTypeIpv4, ip);
    CHECK(frame.size() == kMinimumFrame);
    EthernetFrame e;
    Ipv4Packet    p;
    UdpDatagram   u;
    CHECK(parseEthernet(frame.data(), frame.size(), e));
    CHECK(e.source == kGuestMac && e.etherType == kEtherTypeIpv4);
    CHECK(parseIpv4(e.payload, p) && p.source == kGuestIp && p.destination == kPublicIp);
    CHECK(parseUdp(p, u) && u.sourcePort == 1234 && u.destinationPort == 7 &&
          u.payload.size == 3);

    // One flipped payload bit fails the UDP checksum; a zero checksum means
    // "none" and passes.
    Bytes bad = ip;
    bad[kIpv4Header + kUdpHeader] ^= 0x01;
    CHECK(parseIpv4({ bad.data(), bad.size() }, p) && !parseUdp(p, u));
    bad[kIpv4Header + 6] = bad[kIpv4Header + 7] = 0;
    CHECK(parseIpv4({ bad.data(), bad.size() }, p) && parseUdp(p, u));
    // A corrupt IP header is refused outright.
    bad = ip;
    bad[8] ^= 0x10;
    CHECK(!parseIpv4({ bad.data(), bad.size() }, p));

    // A fragment is refused (more-fragments, then a non-zero offset).
    Bytes frag = ip;
    wr16(frag.data() + 6, 0x2000);
    wr16(frag.data() + 10, 0);
    wr16(frag.data() + 10, checksum(frag.data(), kIpv4Header));
    CHECK(!parseIpv4({ frag.data(), frag.size() }, p));
    wr16(frag.data() + 6, 0x0010);
    wr16(frag.data() + 10, 0);
    wr16(frag.data() + 10, checksum(frag.data(), kIpv4Header));
    CHECK(!parseIpv4({ frag.data(), frag.size() }, p));

    // An IPv4 header with options is legal: skipped, payload intact.
    Bytes opt(24 + udp.size(), 0);
    opt[0] = 0x46;
    wr16(opt.data() + 2, static_cast<uint16_t>(opt.size()));
    opt[8] = 64;
    opt[9] = kProtoUdp;
    wr32(opt.data() + 12, kGuestIp);
    wr32(opt.data() + 16, kPublicIp);
    opt[20] = 1; opt[21] = 1; opt[22] = 1; opt[23] = 0;      // NOP NOP NOP EOL
    wr16(opt.data() + 10, checksum(opt.data(), 24));
    std::memcpy(opt.data() + 24, udp.data(), udp.size());
    CHECK(parseIpv4({ opt.data(), opt.size() }, p) && parseUdp(p, u) && u.destinationPort == 7);

    // TCP: the MSS option survives a round trip, and so does a bad checksum's refusal.
    TcpSegment s;
    s.sourceIp = kGuestIp; s.destinationIp = kPublicIp; s.sourcePort = 4000;
    s.destinationPort = 80; s.sequence = 0x11223344; s.acknowledgment = 0x55667788;
    s.flags = kTcpSyn | kTcpAck; s.window = 1024; s.mss = 536; s.payload = text("hello");
    Bytes tcp = buildTcp(s);
    Ipv4Packet tp{ kGuestIp, kPublicIp, kProtoTcp, { tcp.data(), tcp.size() } };
    TcpSegment back;
    CHECK(parseTcp(tp, back));
    CHECK(back.sequence == s.sequence && back.acknowledgment == s.acknowledgment &&
          back.flags == s.flags && back.window == 1024 && back.mss == 536 &&
          back.payload == s.payload);
    tcp.back() ^= 0x80;
    tp.payload = { tcp.data(), tcp.size() };
    CHECK(!parseTcp(tp, back));
}

// ── 2. The fence ─────────────────────────────────────────────────────────

void testFence()
{
    std::printf("[2] the loopback fence, as a table\n");
    Endpoint h;
    const RoutePolicy closed{}, open{ true, false }, restricted{ false, true },
                      restrictedOpen{ true, true };
    for (bool tcp : { true, false }) {
        // The gateway is NOT the host's loopback…
        CHECK(routeDestination(kGatewayIp, 6503, tcp, closed, h) == Route::Refused);
        // …and neither is any address that reaches it by another name.
        CHECK(routeDestination(0x7F000001, 6503, tcp, closed, h) == Route::Refused);
        CHECK(routeDestination(0x7F123456, 80, tcp, closed, h) == Route::Refused);
        CHECK(routeDestination(0x00000000, 6503, tcp, closed, h) == Route::Refused);
        CHECK(routeDestination(0xA9FEA9FE, 80, tcp, closed, h) == Route::Refused);   // metadata
        CHECK(routeDestination(0xE0000001, 5353, tcp, closed, h) == Route::Refused); // multicast
        CHECK(routeDestination(0xFFFFFFFF, 9, tcp, closed, h) == Route::Refused);    // broadcast
        // Nobody else lives on the virtual subnet.
        CHECK(routeDestination(kGuestIp, 80, tcp, closed, h) == Route::Refused);
        CHECK(routeDestination(0x0A000263, 80, tcp, closed, h) == Route::Refused);
        CHECK(routeDestination(0x0A0002FF, 80, tcp, closed, h) == Route::Refused);
        // The DNS address is DNS and nothing else: not a second door.
        CHECK(routeDestination(kDnsIp, 6503, tcp, closed, h) == Route::Refused);
        CHECK(routeDestination(kDnsIp, 6503, tcp, open, h) == Route::Refused);
        // The world is open.
        CHECK(routeDestination(kPublicIp, 80, tcp, closed, h) == Route::Host);
        CHECK(h.ip == kPublicIp && h.port == 80);
        CHECK(routeDestination(0xC0A80117, 6502, tcp, closed, h) == Route::Host);   // the LAN

        // The opt-in opens the loopback, both ways of naming it — and
        // nothing else.
        CHECK(routeDestination(kGatewayIp, 6503, tcp, open, h) == Route::Host);
        CHECK(h.ip == kLoopbackIp && h.port == 6503);
        CHECK(routeDestination(0x7F000001, 6503, tcp, open, h) == Route::Host);
        CHECK(h.ip == 0x7F000001);
        CHECK(routeDestination(0x00000000, 6503, tcp, open, h) == Route::Refused);
        CHECK(routeDestination(0xA9FEA9FE, 80, tcp, open, h) == Route::Refused);
        CHECK(routeDestination(0xE0000001, 5353, tcp, open, h) == Route::Refused);

        // Restricted: the virtual services and nothing else, opt-in or not.
        CHECK(routeDestination(kPublicIp, 80, tcp, restricted, h) == Route::Refused);
        CHECK(routeDestination(kGatewayIp, 80, tcp, restrictedOpen, h) == Route::Refused);
        CHECK(routeDestination(0x7F000001, 80, tcp, restrictedOpen, h) == Route::Refused);
    }
    CHECK(routeDestination(kDnsIp, kDnsPort, false, closed, h) == Route::Dns);
    CHECK(routeDestination(kDnsIp, kDnsPort, false, restricted, h) == Route::Dns);
    CHECK(routeDestination(kDnsIp, kDnsPort, true, closed, h) == Route::Refused);

    // The setting: "slirp" is "the NAT", whichever one the build has.
    CHECK(std::string(resolveEthernetBackend("slirp", true, true)) == "slirp");
    CHECK(std::string(resolveEthernetBackend("slirp", false, true)) == "builtin");
    CHECK(std::string(resolveEthernetBackend("slirp", false, false)) == "none");
    CHECK(std::string(resolveEthernetBackend("builtin", true, true)) == "builtin");
    CHECK(std::string(resolveEthernetBackend("builtin", true, false)) == "none");
    CHECK(std::string(resolveEthernetBackend("loopback", true, true)) == "loopback");
    CHECK(std::string(resolveEthernetBackend("none", true, true)) == "none");
    CHECK(std::string(resolveEthernetBackend("typo", false, true)) == "builtin");
}

// ── 3. ARP, ping, DHCP ───────────────────────────────────────────────────

void sendArp(Rig& rig, uint32_t senderIp, uint32_t targetIp)
{
    ArpPacket a;
    a.operation = kArpRequest;
    a.senderMac = kGuestMac;
    a.senderIp  = senderIp;
    a.targetIp  = targetIp;
    const Bytes f = buildEthernet(kBroadcastMac, kGuestMac, kEtherTypeArp, buildArp(a));
    rig.net->transmit(f.data(), static_cast<int>(f.size()));
}

Bytes dhcpMessage(uint8_t type)
{
    Bytes m(240, 0);
    m[0] = 1; m[1] = 1; m[2] = 6;
    wr32(m.data() + 4, 0xCAFEBABE);                // xid
    wr16(m.data() + 10, 0x8000);                   // broadcast flag
    std::memcpy(m.data() + 28, kGuestMac.data(), 6);
    wr32(m.data() + 236, 0x63825363);
    m.insert(m.end(), { 53, 1, type, 255 });
    return m;
}

void testLocalServices()
{
    std::printf("[3] ARP, ping, DHCP\n");
    Rig rig;
    CHECK(rig.net && rig.net->isValid());
    CHECK(rig.net->name().find("built-in") != std::string_view::npos);

    // ARP: the gateway answers for itself and for the whole world.
    for (uint32_t target : { kGatewayIp, kDnsIp, kPublicIp }) {
        sendArp(rig, kGuestIp, target);
        const auto f = rig.frames();
        CHECK(f.size() == 1);
        if (f.size() != 1) continue;
        EthernetFrame e;
        ArpPacket     a;
        CHECK(parseEthernet(f[0].data(), f[0].size(), e) && e.destination == kGuestMac);
        CHECK(parseArp(e.payload, a) && a.operation == kArpReply);
        CHECK(a.senderIp == target && a.targetIp == kGuestIp && a.targetMac == kGuestMac);
        CHECK(a.senderMac[0] == 0x52 && a.senderMac[1] == 0x55);
    }
    // No reply to a probe, an announcement, or a question about the guest
    // itself: any of them would read as an address conflict.
    sendArp(rig, 0, kGuestIp);
    sendArp(rig, kGuestIp, kGuestIp);
    sendArp(rig, 0, kGatewayIp);
    CHECK(rig.frames().empty());

    // Ping the gateway.
    Bytes echo = { kIcmpEchoRequest, 0, 0, 0, 0x12, 0x34, 0x00, 0x01, 'p', 'o', 'm', '2' };
    wr16(echo.data() + 2, checksum(echo.data(), echo.size()));
    rig.sendIp(kGatewayIp, kProtoIcmp, echo);
    {
        const auto f = rig.frames();
        CHECK(f.size() == 1);
        EthernetFrame e;
        Ipv4Packet    p;
        if (f.size() == 1 && parseEthernet(f[0].data(), f[0].size(), e) &&
            parseIpv4(e.payload, p)) {
            CHECK(p.source == kGatewayIp && p.destination == kGuestIp);
            CHECK(p.payload.size == echo.size() && p.payload.data[0] == kIcmpEchoReply);
            CHECK(checksum(p.payload.data, p.payload.size) == 0);
            CHECK(std::memcmp(p.payload.data + 4, echo.data() + 4, echo.size() - 4) == 0);
        } else CHECK(false);
    }
    // A ping to the world is not relayed (it would need a raw socket).
    rig.sendIp(kPublicIp, kProtoIcmp, echo);
    CHECK(rig.frames().empty());

    // DHCP: DISCOVER -> OFFER, REQUEST -> ACK, from 0.0.0.0 to broadcast.
    for (auto step : { std::pair<uint8_t, uint8_t>{ 1, 2 }, { 3, 5 } }) {
        rig.sendUdp(0xFFFFFFFF, kDhcpClientPort, kDhcpServerPort, dhcpMessage(step.first), 0);
        const auto d = rig.datagrams();
        CHECK(d.size() == 1);
        if (d.size() != 1) continue;
        CHECK(d[0].ethDst == kBroadcastMac && d[0].destinationIp == 0xFFFFFFFF);
        CHECK(d[0].sourceIp == kGatewayIp && d[0].sourcePort == kDhcpServerPort &&
              d[0].destinationPort == kDhcpClientPort);
        const Bytes& r = d[0].payload;
        CHECK(r.size() >= 300 && r[0] == 2);
        CHECK(rd32(r.data() + 4) == 0xCAFEBABE);
        CHECK(rd32(r.data() + 16) == kGuestIp);                       // yiaddr
        CHECK(std::memcmp(r.data() + 28, kGuestMac.data(), 6) == 0);
        // Walk the options: type, mask, router, DNS.
        std::map<uint8_t, uint32_t> opts;
        for (std::size_t i = 240; i + 1 < r.size() && r[i] != 255;) {
            if (r[i] == 0) { ++i; continue; }
            opts[r[i]] = r[i + 1] == 4 ? rd32(r.data() + i + 2) : r[i + 2];
            i += 2 + r[i + 1];
        }
        CHECK(opts[53] == step.second);
        CHECK(opts[1] == kSubnetMask && opts[3] == kGatewayIp && opts[6] == kDnsIp);
        CHECK(opts[54] == kGatewayIp && opts[51] == 86400);
    }
    // The DHCP exchange opened no host socket: it never left the machine.
    CHECK(rig.world.opened == 0);
}

// ── 4. DNS ───────────────────────────────────────────────────────────────

Bytes dnsQuery(uint16_t id, const char* name, uint16_t type = 1)
{
    Bytes q(12, 0);
    wr16(q.data(), id);
    wr16(q.data() + 2, 0x0100);                     // RD
    wr16(q.data() + 4, 1);
    for (const char* p = name; *p;) {
        const char* dot = std::strchr(p, '.');
        const std::size_t n = dot ? static_cast<std::size_t>(dot - p) : std::strlen(p);
        q.push_back(static_cast<uint8_t>(n));
        q.insert(q.end(), p, p + n);
        p += n + (dot ? 1 : 0);
    }
    q.push_back(0);
    q.insert(q.end(), { static_cast<uint8_t>(type >> 8), static_cast<uint8_t>(type), 0, 1 });
    return q;
}

struct DnsAnswer { uint16_t id = 0; int rcode = -1; int answers = -1; uint32_t address = 0; };

DnsAnswer oneDnsAnswer(Rig& rig, uint16_t guestPort)
{
    DnsAnswer a;
    const auto d = rig.datagrams();
    CHECK(d.size() == 1);
    if (d.size() != 1) return a;
    CHECK(d[0].sourceIp == kDnsIp && d[0].sourcePort == kDnsPort &&
          d[0].destinationPort == guestPort && d[0].destinationIp == kGuestIp);
    const Bytes& r = d[0].payload;
    if (r.size() < 12) { CHECK(false); return a; }
    a.id      = rd16(r.data());
    a.rcode   = r[3] & 0x0F;
    a.answers = rd16(r.data() + 6);
    CHECK((r[2] & 0x80) != 0);                      // QR: a response
    if (a.answers == 1 && r.size() >= 16) {
        const uint8_t* rr = r.data() + r.size() - 16;
        CHECK(rd16(rr) == 0xC00C && rd16(rr + 2) == 1 && rd16(rr + 4) == 1 && rd16(rr + 10) == 4);
        a.address = rd32(rr + 12);
    }
    return a;
}

void testDns()
{
    std::printf("[4] DNS from the host resolver\n");
    Rig rig;
    uint32_t wire = 0;
    const uint8_t quad[4] = { 93, 184, 216, 34 };
    std::memcpy(&wire, quad, 4);                    // network order, as the resolver gives it

    // An answer already in hand.
    rig.names.answers["example.com"] = { W5100Resolver::Status::Resolved, wire };
    rig.sendUdp(kDnsIp, 3000, kDnsPort, dnsQuery(0x1111, "example.com"));
    DnsAnswer a = oneDnsAnswer(rig, 3000);
    CHECK(a.id == 0x1111 && a.rcode == 0 && a.answers == 1 && a.address == kPublicIp);

    // The resolver says no: NXDOMAIN.
    rig.names.answers["nope.invalid"] = { W5100Resolver::Status::Failed, 0 };
    rig.sendUdp(kDnsIp, 3000, kDnsPort, dnsQuery(0x2222, "nope.invalid"));
    a = oneDnsAnswer(rig, 3000);
    CHECK(a.id == 0x2222 && a.rcode == 3 && a.answers == 0);

    // The rate limit says no: SERVFAIL (the guest's retry will come back).
    rig.names.answers["flood.example"] = { W5100Resolver::Status::Refused, 0 };
    rig.sendUdp(kDnsIp, 3000, kDnsPort, dnsQuery(0x2323, "flood.example"));
    CHECK(oneDnsAnswer(rig, 3000).rcode == 2);

    // A type nobody serves: "exists, no such record" — and no host lookup.
    int before = rig.names.resolves;
    rig.sendUdp(kDnsIp, 3000, kDnsPort, dnsQuery(0x3333, "example.com", 28));   // AAAA
    a = oneDnsAnswer(rig, 3000);
    CHECK(a.rcode == 0 && a.answers == 0 && rig.names.resolves == before);

    // Bytes that are not a host name never reach the host resolver.
    rig.sendUdp(kDnsIp, 3000, kDnsPort, dnsQuery(0x3434, "a b;c.example"));
    a = oneDnsAnswer(rig, 3000);
    CHECK(a.rcode == 3 && rig.names.resolves == before);

    // A slow answer: nothing yet; the guest repeats its question twice more;
    // ONE host lookup was started. Then the answer arrives.
    before = rig.names.resolves;
    rig.sendUdp(kDnsIp, 3001, kDnsPort, dnsQuery(0x4444, "slow.example"));
    rig.pump(500);
    rig.sendUdp(kDnsIp, 3001, kDnsPort, dnsQuery(0x4444, "slow.example"));
    rig.pump(500);
    rig.sendUdp(kDnsIp, 3001, kDnsPort, dnsQuery(0x4444, "slow.example"));
    rig.pump(500);
    CHECK(rig.frames().empty());
    CHECK(rig.names.resolves == before + 1);
    rig.names.answers["slow.example"] = { W5100Resolver::Status::Resolved, wire };
    rig.pump(10);
    a = oneDnsAnswer(rig, 3001);
    CHECK(a.id == 0x4444 && a.rcode == 0 && a.address == kPublicIp);
    CHECK(rig.names.resolves == before + 1);
    rig.pump(10);
    CHECK(rig.frames().empty());                    // answered once

    // An answer that never comes: SERVFAIL at the deadline, not before.
    rig.sendUdp(kDnsIp, 3002, kDnsPort, dnsQuery(0x5555, "dead.example"));
    rig.pump(DnsProxy::kTimeoutMs - 100);
    CHECK(rig.frames().empty());
    rig.pump(200);
    a = oneDnsAnswer(rig, 3002);
    CHECK(a.id == 0x5555 && a.rcode == 2);

    // Garbage: a short datagram is dropped, a malformed question is FORMERR.
    rig.sendUdp(kDnsIp, 3003, kDnsPort, Bytes{ 1, 2, 3 });
    CHECK(rig.frames().empty());
    Bytes broken = dnsQuery(0x6666, "example.com");
    broken.resize(15);
    rig.sendUdp(kDnsIp, 3003, kDnsPort, broken);
    CHECK(oneDnsAnswer(rig, 3003).rcode == 1);

    // None of it opened a host socket: DNS is answered, not relayed.
    CHECK(rig.world.opened == 0);
}

// ── 5. UDP ───────────────────────────────────────────────────────────────

void testUdp()
{
    std::printf("[5] UDP relay\n");
    Rig rig;
    rig.sendUdp(kPublicIp, 5000, 69, text("RRQ"));
    CHECK(rig.world.opened == 1);
    FakeSocket* s = rig.world.last();
    if (!s) { CHECK(false); return; }
    CHECK(!s->tcp && s->datagramsOut.size() == 1);
    CHECK(s->datagramsOut[0].first.ip == kPublicIp && s->datagramsOut[0].first.port == 69);
    CHECK(s->datagramsOut[0].second == text("RRQ"));

    // The same guest port reuses the flow; another opens a second.
    rig.sendUdp(kPublicIp, 5000, 69, text("again"));
    CHECK(rig.world.opened == 1 && s->datagramsOut.size() == 2);

    // The server answers from ANOTHER port (TFTP does): delivered.
    s->datagramsIn.push_back({ { kPublicIp, 40123 }, text("DATA") });
    // A stranger writes to the same host port: dropped.
    s->datagramsIn.push_back({ { kOtherIp, 69 }, text("EVIL") });
    // Too long for one frame: dropped, not truncated.
    s->datagramsIn.push_back({ { kPublicIp, 40123 }, Bytes(1473, 'x') });
    // Exactly the largest that fits: delivered whole.
    s->datagramsIn.push_back({ { kPublicIp, 40123 }, Bytes(1472, 'y') });
    rig.pump(1);
    const auto d = rig.datagrams();
    CHECK(d.size() == 2);
    if (d.size() == 2) {
        CHECK(d[0].sourceIp == kPublicIp && d[0].sourcePort == 40123 &&
              d[0].destinationIp == kGuestIp && d[0].destinationPort == 5000 &&
              d[0].payload == text("DATA") && d[0].ethDst == kGuestMac);
        CHECK(d[1].payload.size() == 1472);
    }

    // A fenced destination opens nothing and sends nothing.
    for (uint32_t refused : { kGatewayIp, uint32_t{ 0x7F000001 }, uint32_t{ 0xA9FEA9FE },
                              uint32_t{ 0xFFFFFFFF }, uint32_t{ 0x0A000263 } })
        rig.sendUdp(refused, 5001, 6503, text("x"));
    CHECK(rig.world.opened == 1);
    CHECK(rig.frames().empty());

    // An idle flow closes; a busy one would not.
    rig.pump(119000);
    CHECK(rig.world.closed == 0);
    rig.pump(2000);
    CHECK(rig.world.closed == 1 && rig.world.live.empty());

    // With the opt-in, the gateway IS the host — and its replies come back
    // from the gateway's address, not from 127.0.0.1.
    Rig open(true);
    open.sendUdp(kGatewayIp, 5002, 6503, text("hi"));
    FakeSocket* g = open.world.last();
    if (!g) { CHECK(false); return; }
    CHECK(g->datagramsOut.size() == 1 && g->datagramsOut[0].first.ip == kLoopbackIp &&
          g->datagramsOut[0].first.port == 6503);
    g->datagramsIn.push_back({ { kLoopbackIp, 6503 }, text("yo") });
    open.pump(1);
    const auto back = open.datagrams();
    CHECK(back.size() == 1 && back[0].sourceIp == kGatewayIp && back[0].payload == text("yo"));
}

// ── 6. TCP ───────────────────────────────────────────────────────────────

constexpr uint32_t kGuestIsn = 0x00001000;

/// SYN, host connects, SYN-ACK, ACK. Returns the relay's ISN + 1.
uint32_t handshake(Rig& rig, uint16_t guestPort, uint32_t destination, uint16_t port,
                   uint16_t window = 2048, uint16_t mss = 536)
{
    rig.sendTcp(destination, guestPort, port, kTcpSyn, kGuestIsn, 0, {}, window, mss);
    FakeSocket* s = rig.world.last();
    if (!s) { CHECK(false); return 0; }
    // Nothing is said to the guest while the host connect is pending.
    rig.pump(10);
    CHECK(rig.frames().empty());
    s->connectState = ConnectState::Connected;
    rig.pump(10);
    const auto seg = rig.segments();
    CHECK(seg.size() == 1);
    if (seg.size() != 1) return 0;
    CHECK(seg[0].flags == (kTcpSyn | kTcpAck));
    CHECK(seg[0].acknowledgment == kGuestIsn + 1);
    CHECK(seg[0].mss == 1460);
    CHECK(seg[0].sourceIp == destination && seg[0].sourcePort == port &&
          seg[0].destinationIp == kGuestIp && seg[0].destinationPort == guestPort);
    const uint32_t ours = seg[0].sequence + 1;
    rig.sendTcp(destination, guestPort, port, kTcpAck, kGuestIsn + 1, ours, {}, window);
    CHECK(rig.frames().empty());
    return ours;
}

void testTcp()
{
    std::printf("[6] TCP relay\n");
    {
        Rig rig;
        const uint32_t ours = handshake(rig, 4000, kPublicIp, 80);
        FakeSocket* s = rig.world.last();
        if (!s || !ours) { CHECK(false); return; }
        CHECK(s->tcp && s->connectCalled && s->remote.ip == kPublicIp && s->remote.port == 80);

        // Guest -> host.
        rig.sendTcp(kPublicIp, 4000, 80, kTcpAck | kTcpPsh, kGuestIsn + 1, ours, text("GET /\r\n"));
        auto seg = rig.segments();
        CHECK(seg.size() == 1 && seg[0].flags == kTcpAck &&
              seg[0].acknowledgment == kGuestIsn + 1 + 7);
        rig.pump(1);
        CHECK(s->fromGuest == text("GET /\r\n"));

        // The same segment again (the guest missed our ACK): acknowledged
        // again, NOT delivered twice.
        rig.sendTcp(kPublicIp, 4000, 80, kTcpAck | kTcpPsh, kGuestIsn + 1, ours, text("GET /\r\n"));
        seg = rig.segments();
        CHECK(seg.size() == 1 && seg[0].acknowledgment == kGuestIsn + 8);
        rig.pump(1);
        CHECK(s->fromGuest.size() == 7);

        // Host -> guest.
        for (char c : std::string("HTTP/1.0 200 OK\r\n")) s->toGuest.push_back(static_cast<uint8_t>(c));
        rig.pump(1);
        seg = rig.segments();
        CHECK(seg.size() == 1);
        if (seg.size() == 1) {
            CHECK(seg[0].sequence == ours && (seg[0].flags & kTcpAck) &&
                  seg[0].payload == text("HTTP/1.0 200 OK\r\n"));
        }
        // Unacknowledged for a second: the same bytes, the same sequence.
        rig.pump(999);
        CHECK(rig.frames().empty());
        rig.pump(2);
        seg = rig.segments();
        CHECK(seg.size() == 1 && seg[0].sequence == ours && seg[0].payload.size() == 17);
        rig.sendTcp(kPublicIp, 4000, 80, kTcpAck, kGuestIsn + 8, ours + 17);
        rig.pump(2000);
        CHECK(rig.frames().empty());                  // acknowledged: no more resends

        // The server closes: FIN to the guest; the guest ACKs it and closes
        // too; its FIN is ACKed, the host is told, the connection is gone.
        s->eof = true;
        rig.pump(1);
        seg = rig.segments();
        CHECK(seg.size() == 1 && (seg[0].flags & kTcpFin) && seg[0].sequence == ours + 17);
        rig.sendTcp(kPublicIp, 4000, 80, kTcpFin | kTcpAck, kGuestIsn + 8, ours + 18);
        seg = rig.segments();
        CHECK(seg.size() == 1 && seg[0].flags == kTcpAck &&
              seg[0].acknowledgment == kGuestIsn + 9);
        CHECK(rig.world.closed == 0);
        rig.pump(1);
        CHECK(rig.world.closed == 1 && rig.world.live.empty());

        // A late segment for the connection that is gone: reset.
        rig.sendTcp(kPublicIp, 4000, 80, kTcpAck, kGuestIsn + 9, ours + 18);
        seg = rig.segments();
        CHECK(seg.size() == 1 && seg[0].flags == kTcpRst && seg[0].sequence == ours + 18);
    }
    {
        // The guest closes first: the host is half-closed and may still talk.
        Rig rig;
        const uint32_t ours = handshake(rig, 4001, kPublicIp, 23);
        FakeSocket* s = rig.world.last();
        if (!s) { CHECK(false); return; }
        rig.sendTcp(kPublicIp, 4001, 23, kTcpFin | kTcpAck, kGuestIsn + 1, ours);
        rig.segments();
        rig.pump(1);
        CHECK(s->shutdownSent && rig.world.closed == 0);
        s->toGuest.push_back('!');
        s->eof = true;
        rig.pump(1);
        rig.pump(1);
        auto seg = rig.segments();
        CHECK(seg.size() == 2 && seg[0].payload == text("!") && (seg[1].flags & kTcpFin));
        rig.sendTcp(kPublicIp, 4001, 23, kTcpAck, kGuestIsn + 2, ours + 2);
        rig.pump(1);
        CHECK(rig.world.closed == 1);
    }
    {
        // The guest's window and MSS are honoured: 400 bytes for a guest
        // that takes 100 at a time and holds 150.
        Rig rig;
        const uint32_t ours = handshake(rig, 4002, kPublicIp, 80, 150, 100);
        FakeSocket* s = rig.world.last();
        if (!s) { CHECK(false); return; }
        for (int i = 0; i < 400; ++i) s->toGuest.push_back(static_cast<uint8_t>(i));
        rig.pump(1);
        auto seg = rig.segments();
        CHECK(seg.size() == 2 && seg[0].payload.size() == 100 && seg[1].payload.size() == 50);
        CHECK(seg.size() == 2 && seg[1].sequence == ours + 100);
        rig.pump(1);
        CHECK(rig.frames().empty());                  // the window is full
        rig.sendTcp(kPublicIp, 4002, 80, kTcpAck, kGuestIsn + 1, ours + 150, {}, 150);
        rig.pump(1);
        seg = rig.segments();
        CHECK(seg.size() == 2 && seg[0].sequence == ours + 150 &&
              seg[0].payload.size() == 100 && seg[0].payload[0] == static_cast<uint8_t>(150));

        // Never acknowledged again: eight resends a second apart, then a
        // reset and the host socket closed.
        int resends = 0;
        bool reset = false;
        for (int i = 0; i < 12 && !reset; ++i) {
            rig.pump(1001);
            for (const TcpSegment& t : rig.segments()) {
                if (t.flags & kTcpRst) reset = true;
                else if (t.sequence == ours + 150) ++resends;
            }
        }
        CHECK(resends == 8 && reset && rig.world.closed == 1);
    }
    {
        // A lost SYN-ACK: the guest's repeated SYN gets it again.
        Rig rig;
        rig.sendTcp(kPublicIp, 4003, 80, kTcpSyn, kGuestIsn, 0, {}, 2048, 536);
        FakeSocket* s = rig.world.last();
        if (!s) { CHECK(false); return; }
        rig.sendTcp(kPublicIp, 4003, 80, kTcpSyn, kGuestIsn, 0, {}, 2048, 536);   // still connecting
        CHECK(rig.frames().empty() && rig.world.opened == 1);
        s->connectState = ConnectState::Connected;
        rig.pump(1);
        const auto first = rig.segments();
        rig.sendTcp(kPublicIp, 4003, 80, kTcpSyn, kGuestIsn, 0, {}, 2048, 536);
        const auto again = rig.segments();
        CHECK(first.size() == 1 && again.size() == 1 &&
              again[0].flags == (kTcpSyn | kTcpAck) && again[0].sequence == first[0].sequence);
        CHECK(rig.world.opened == 1);
    }
    {
        // Nobody listening: the guest learns it as a reset of its SYN.
        Rig rig;
        rig.sendTcp(kPublicIp, 4004, 81, kTcpSyn, kGuestIsn, 0);
        FakeSocket* s = rig.world.last();
        if (!s) { CHECK(false); return; }
        s->connectState = ConnectState::Failed;
        rig.pump(1);
        const auto seg = rig.segments();
        CHECK(seg.size() == 1 && (seg[0].flags & kTcpRst) && (seg[0].flags & kTcpAck) &&
              seg[0].acknowledgment == kGuestIsn + 1);
        CHECK(rig.world.closed == 1);

        // A connect that never answers is given up after 30 s.
        rig.sendTcp(kPublicIp, 4005, 81, kTcpSyn, kGuestIsn, 0);
        rig.pump(29000);
        CHECK(rig.frames().empty());
        rig.pump(2000);
        const auto late = rig.segments();
        CHECK(late.size() == 1 && (late[0].flags & kTcpRst) && rig.world.closed == 2);
    }
    {
        // The host connection breaks mid-stream: reset to the guest.
        Rig rig;
        handshake(rig, 4006, kPublicIp, 80);
        FakeSocket* s = rig.world.last();
        if (!s) { CHECK(false); return; }
        s->broken = true;
        rig.pump(1);
        const auto seg = rig.segments();
        CHECK(seg.size() == 1 && (seg[0].flags & kTcpRst) && rig.world.closed == 1);
    }
    {
        // A guest reset closes the host side without a word back.
        Rig rig;
        const uint32_t ours = handshake(rig, 4007, kPublicIp, 80);
        rig.sendTcp(kPublicIp, 4007, 80, kTcpRst, kGuestIsn + 1, ours);
        rig.pump(1);
        CHECK(rig.frames().empty() && rig.world.closed == 1);
    }
    {
        // THE FENCE, on the wire: a SYN to the gateway, to 127.0.0.1, to
        // 0.0.0.0, to link-local and to the DNS address is reset — and no
        // host socket is ever opened for it.
        Rig rig;
        uint16_t port = 4100;
        for (uint32_t refused : { kGatewayIp, uint32_t{ 0x7F000001 }, uint32_t{ 0 },
                                  uint32_t{ 0xA9FEA9FE }, kDnsIp, uint32_t{ 0x0A000263 } }) {
            rig.sendTcp(refused, port, 6503, kTcpSyn, kGuestIsn, 0);
            const auto seg = rig.segments();
            CHECK(seg.size() == 1 && (seg[0].flags & kTcpRst) &&
                  seg[0].acknowledgment == kGuestIsn + 1 && seg[0].sourceIp == refused);
            ++port;
        }
        CHECK(rig.world.opened == 0);

        // The opt-in: the gateway is the host's loopback, shown as the gateway.
        Rig open(true);
        handshake(open, 4200, kGatewayIp, 6503);
        FakeSocket* s = open.world.last();
        CHECK(s && s->remote.ip == kLoopbackIp && s->remote.port == 6503);
    }
    {
        // The table is bounded: connection 33 is reset, and frees no slot.
        Rig rig;
        for (int i = 0; i < 32; ++i)
            rig.sendTcp(kPublicIp, static_cast<uint16_t>(5000 + i), 80, kTcpSyn, kGuestIsn, 0);
        CHECK(rig.world.opened == 32 && rig.frames().empty());
        rig.sendTcp(kPublicIp, 5999, 80, kTcpSyn, kGuestIsn, 0);
        const auto seg = rig.segments();
        CHECK(seg.size() == 1 && (seg[0].flags & kTcpRst) && rig.world.opened == 32);
    }
}

// ── 7. Real sockets, on 127.0.0.1 ────────────────────────────────────────

#if POM2_HAS_SOCKETS

struct Listener {
    socket_t fd = kInvalidSocket;
    uint16_t port = 0;
    Listener()
    {
        ensureSocketStack();
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_c n = sizeof(a);
        if (!isValidSocket(fd) || ::bind(fd, reinterpret_cast<sockaddr*>(&a), n) != 0 ||
            ::listen(fd, 4) != 0 ||
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n) != 0) {
            closeHostSocket(fd);
            return;
        }
        setNonBlocking(fd);
        port = netToHost16(a.sin_port);
    }
    ~Listener() { closeHostSocket(fd); }
    socket_t accept() const { return ::accept(fd, nullptr, nullptr); }
};

/// Pump the backend, real time, until `done` or two seconds pass.
template <class Fn> bool until(NetworkBackend& net, Fn done)
{
    for (int i = 0; i < 2000; ++i) {
        net.poll();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void testRealSockets()
{
    std::printf("[7] real sockets on 127.0.0.1\n");
    Listener listener;
    if (!listener.port) { std::printf("  SKIP: cannot listen on 127.0.0.1\n"); return; }

    // Without the opt-in the listener is never reached, by either name.
    {
        Rig probe;                                   // for the frame helpers only
        probe.net = makeBuiltinNatBackend({});
        CHECK(probe.net && probe.net->isValid());
        probe.sendTcp(kGatewayIp, 4300, listener.port, kTcpSyn, kGuestIsn, 0);
        probe.sendTcp(0x7F000001, 4301, listener.port, kTcpSyn, kGuestIsn, 0);
        for (int i = 0; i < 50; ++i) {
            probe.net->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        socket_t got = listener.accept();
        CHECK(!isValidSocket(got));
        closeHostSocket(got);
        const auto seg = probe.segments();
        CHECK(seg.size() == 2 && (seg[0].flags & kTcpRst) && (seg[1].flags & kTcpRst));
    }

    // With it: a full exchange through the kernel.
    BuiltinNatOptions o;
    o.allowHostLoopback = true;
    Rig rig;
    rig.net = makeBuiltinNatBackend(o);
    rig.sendTcp(kGatewayIp, 4400, listener.port, kTcpSyn, kGuestIsn, 0, {}, 2048, 536);
    std::vector<TcpSegment> seg;
    CHECK(until(*rig.net, [&] { seg = rig.segments(); return !seg.empty(); }));
    if (seg.empty()) return;
    CHECK(seg[0].flags == (kTcpSyn | kTcpAck) && seg[0].sourceIp == kGatewayIp);
    const uint32_t ours = seg[0].sequence + 1;
    rig.sendTcp(kGatewayIp, 4400, listener.port, kTcpAck | kTcpPsh, kGuestIsn + 1, ours,
                text("ping"));
    socket_t server = kInvalidSocket;
    CHECK(until(*rig.net, [&] { server = listener.accept(); return isValidSocket(server); }));
    if (!isValidSocket(server)) return;
    setNonBlocking(server);
    char buf[16] = {};
    iolen_t n = 0;
    CHECK(until(*rig.net, [&] { n = recvSocket(server, buf, sizeof buf); return n > 0; }));
    CHECK(n == 4 && std::memcmp(buf, "ping", 4) == 0);
    rig.segments();                                   // the ACK of "ping"

    CHECK(sendSocket(server, "pong", 4) == 4);
    seg.clear();
    CHECK(until(*rig.net, [&] {
        for (const TcpSegment& s : rig.segments()) if (!s.payload.empty()) seg.push_back(s);
        return !seg.empty();
    }));
    CHECK(!seg.empty() && seg[0].payload == text("pong") && seg[0].sequence == ours);
    // The server hangs up: the guest is told with a FIN.
    closeHostSocket(server);
    seg.clear();
    CHECK(until(*rig.net, [&] {
        for (const TcpSegment& s : rig.segments()) if (s.flags & kTcpFin) seg.push_back(s);
        return !seg.empty();
    }));
    CHECK(!seg.empty() && seg[0].sequence == ours + 4);

    // UDP through the kernel, to a socket of the test's on 127.0.0.1 — on a
    // network of its own, so no TCP segment of the exchange above is mixed in.
    rig.net = makeBuiltinNatBackend(o);
    socket_t echo = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_c len = sizeof(a);
    if (!isValidSocket(echo) || ::bind(echo, reinterpret_cast<sockaddr*>(&a), len) != 0 ||
        ::getsockname(echo, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        std::printf("  SKIP: cannot bind a UDP socket on 127.0.0.1\n");
        closeHostSocket(echo);
        return;
    }
    setNonBlocking(echo);
    rig.sendUdp(kGatewayIp, 5500, netToHost16(a.sin_port), text("marco"));
    sockaddr_in from{};
    socklen_c   fromLen = sizeof(from);
    CHECK(until(*rig.net, [&] {
        n = ::recvfrom(echo, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        return n > 0;
    }));
    CHECK(n == 5 && std::memcmp(buf, "marco", 5) == 0);
    CHECK(sendToSocket(echo, "polo", 4, reinterpret_cast<sockaddr*>(&from), fromLen) == 4);
    std::vector<Rig::Datagram> d;
    CHECK(until(*rig.net, [&] { d = rig.datagrams(); return !d.empty(); }));
    CHECK(!d.empty() && d[0].payload == text("polo") && d[0].sourceIp == kGatewayIp &&
          d[0].destinationPort == 5500);
    closeHostSocket(echo);
}

#else
void testRealSockets() { std::printf("[7] SKIP: no sockets in this build\n"); }
#endif

}  // namespace

int main()
{
    testCodec();
    testFence();
    testLocalServices();
    testDns();
    testUdp();
    testTcp();
    testRealSockets();
    if (failures) { std::printf("builtin_nat: %d check(s) failed\n", failures); return 1; }
    std::printf("builtin_nat: OK\n");
    return 0;
}
