// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// BuiltinNatBackend — see the header.

#include "BuiltinNatBackend.h"

#include "Logger.h"
#include "NatDhcp.h"
#include "NatDns.h"
#include "NatGuestSink.h"
#include "NatPacket.h"
#include "NatTcp.h"
#include "NatUdp.h"
#include "Pom2Build.h"
#include "W5100NameResolver.h"
#include "W5100Resolver.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <string>
#include <utility>

namespace pom2 {
namespace nat {

Route routeDestination(uint32_t destination, uint16_t port, bool tcp,
                       const RoutePolicy& policy, Endpoint& host)
{
    host = { destination, port };
    const uint8_t top = static_cast<uint8_t>(destination >> 24);

    // The virtual DNS server: port 53 only, and only over UDP — it is
    // answered here, there is no server behind it to hold a TCP connection.
    // (libslirp re-dials this address on the HOST's resolver keeping the
    // guest's port, which is 127.0.0.1 under dnsmasq and several VPN
    // clients: 10.0.2.3:<port> reached host services. Nothing is re-dialled
    // here, so the hole has no counterpart.)
    if (destination == kDnsIp)
        return (!tcp && port == kDnsPort) ? Route::Dns : Route::Refused;

    if (destination == kGatewayIp) {
        if (!policy.allowHostLoopback || policy.restricted) return Route::Refused;
        host.ip = kLoopbackIp;
        return Route::Host;
    }
    // The rest of the virtual subnet — the guest's own address, its
    // broadcast, unassigned numbers: nobody is there.
    if ((destination & kSubnetMask) == kNetwork) return Route::Refused;

    if (policy.restricted) return Route::Refused;

    if (top == 127) return policy.allowHostLoopback ? Route::Host : Route::Refused;
    if (top == 0) return Route::Refused;                          // "this network"
    if ((destination >> 16) == 0xA9FE) return Route::Refused;     // 169.254/16 link-local
    if ((top & 0xF0) == 0xE0) return Route::Refused;              // 224/4 multicast
    if ((top & 0xF0) == 0xF0) return Route::Refused;              // 240/4, broadcast included
    return Route::Host;
}

}  // namespace nat

namespace {

using namespace nat;

// [D] libslirp's convention for its virtual hosts: 52:55 then the address.
constexpr Mac kGatewayMac = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };

std::string dotted(uint32_t ip)
{
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
           std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

class BuiltinNat final : public NetworkBackend, private GuestSink
{
public:
    /// Same backstop as the other backends: when the guest stops draining,
    /// frames are dropped rather than the heap grown. TCP resends; UDP
    /// tolerates the loss.
    static constexpr std::size_t kMaxQueued = 256;

    BuiltinNat(const BuiltinNatOptions& options, std::unique_ptr<HostNetwork> network,
               std::unique_ptr<W5100Resolver> resolver)
        : policy_{ options.allowHostLoopback, options.restricted },
          nowMs_(options.nowMs),
          network_(std::move(network)),
          resolver_(std::move(resolver)),
          dns_(resolver_.get()),
          udp_(*this, network_.get()),
          tcp_(*this, network_.get(),
               [this](uint32_t destination, uint16_t port, Endpoint& host) {
                   const Route r = routeDestination(destination, port, true, policy_, host);
                   if (r != Route::Host) noteRefused(destination, port, "TCP");
                   return r == Route::Host;
               })
    {
        name_ = std::string("built-in NAT (guest 10.0.2.15, gw 10.0.2.2, dns 10.0.2.3") +
                (policy_.restricted ? ", restricted" : "") +
                (policy_.allowHostLoopback ? ", host loopback ALLOWED" : "") + ")";
        log().info("Net", "built-in virtual network up — " + name_);
    }

    bool isValid() const override { return network_ != nullptr; }
    std::string_view name() const override { return name_; }

    void transmit(const uint8_t* frame, int len) override
    {
        if (!network_ || len < kMinEthFrame || len > kMaxEthFrame) {
            ++framesDropped_;
            return;
        }
        EthernetFrame ethernet;
        if (!parseEthernet(frame, static_cast<std::size_t>(len), ethernet)) {
            ++framesDropped_;
            return;
        }
        ++framesSent_;
        // The source of each frame the guest sends is where replies go.
        guestMac_ = ethernet.source;
        if (ethernet.etherType == kEtherTypeArp)       onArp(ethernet);
        else if (ethernet.etherType == kEtherTypeIpv4) onIpv4(ethernet);
    }

    int receive(uint8_t* buf, int cap) override
    {
        if (outbound_.empty()) return 0;
        const Bytes& f = outbound_.front();
        const int n = static_cast<int>(f.size());
        if (n > cap) { outbound_.pop_front(); ++framesDropped_; return 0; }
        std::memcpy(buf, f.data(), f.size());
        outbound_.pop_front();
        ++framesReceived_;
        return n;
    }

    void poll() override
    {
        if (!network_) return;
        const uint64_t t = now();
        dns_.pump(t, [this](uint16_t guestPort, const Bytes& reply) {
            udpToGuest(kDnsIp, kDnsPort, guestPort, reply.data(), reply.size());
        });
        udp_.pump(t);
        tcp_.pump(t);
    }

    /// Every address is behind the gateway (it answers ARP for all of them),
    /// so an IPRAW frame for any destination is addressed to it.
    bool resolveMac(uint32_t, MacAddress& out) override
    {
        std::memcpy(out.b, kGatewayMac.data(), 6);
        return true;
    }

private:
    uint64_t now() const
    {
        if (nowMs_) return nowMs_();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    /// Every address but the guest's own resolves to the gateway, so traffic
    /// to any destination, on the virtual subnet's numbering or not, comes
    /// to the NAT. A probe (sender 0.0.0.0) or an announcement (sender =
    /// target) gets no reply, or the guest would read it as an address
    /// conflict (RFC 5227 § 2.1.1).
    void onArp(const EthernetFrame& frame)
    {
        ArpPacket request;
        if (!parseArp(frame.payload, request) || request.operation != kArpRequest) return;
        if (request.senderIp == 0 || request.senderIp == request.targetIp ||
            request.targetIp == guestIp_)
            return;
        ArpPacket reply;
        reply.operation = kArpReply;
        reply.senderMac = kGatewayMac;
        reply.senderIp  = request.targetIp;
        reply.targetMac = request.senderMac;
        reply.targetIp  = request.senderIp;
        queue(buildEthernet(request.senderMac, kGatewayMac, kEtherTypeArp, buildArp(reply)));
    }

    void onIpv4(const EthernetFrame& frame)
    {
        Ipv4Packet packet;
        if (!parseIpv4(frame.payload, packet)) return;
        // The guest's address is whatever it uses — a static configuration
        // need not be the DHCP lease.
        if (packet.source != 0) guestIp_ = packet.source;

        switch (packet.protocol) {
            case kProtoIcmp: onIcmp(packet); break;
            case kProtoUdp:  onUdp(packet);  break;
            case kProtoTcp: {
                TcpSegment segment;
                if (parseTcp(packet, segment)) tcp_.onGuestSegment(segment, now());
                break;
            }
            default: break;
        }
    }

    /// Echo requests to the gateway or the DNS address get echo replies;
    /// nothing else is relayed (a raw ICMP socket needs privileges).
    void onIcmp(const Ipv4Packet& packet)
    {
        if ((packet.destination != kGatewayIp && packet.destination != kDnsIp) ||
            packet.payload.size < kIcmpHeader || packet.payload.data[0] != kIcmpEchoRequest ||
            checksum(packet.payload.data, packet.payload.size) != 0)
            return;
        queueIpv4(packet.destination, packet.source, kProtoIcmp,
                  buildIcmpEchoReply(packet.payload));
    }

    void onUdp(const Ipv4Packet& packet)
    {
        UdpDatagram datagram;
        if (!parseUdp(packet, datagram)) return;

        // DHCP goes to the server here whatever its destination address
        // (the client has no address and broadcasts); the reply is
        // broadcast, since there is nothing to unicast it to yet.
        if (datagram.destinationPort == kDhcpServerPort) {
            const DhcpLease lease{ kGuestIp, kSubnetMask, kGatewayIp, kDnsIp };
            const Bytes reply = dhcpReply(datagram.payload, lease);
            if (reply.empty()) return;
            const Bytes udp = buildUdp(kGatewayIp, 0xFFFFFFFF, kDhcpServerPort,
                                       kDhcpClientPort, reply.data(), reply.size());
            queue(buildEthernet(kBroadcastMac, kGatewayMac, kEtherTypeIpv4,
                                buildIpv4(kGatewayIp, 0xFFFFFFFF, kProtoUdp, nextIpId_++, udp)));
            return;
        }

        Endpoint host;
        switch (routeDestination(packet.destination, datagram.destinationPort, false,
                                 policy_, host)) {
            case Route::Dns:
                dns_.query(datagram.sourcePort, datagram.payload, now(),
                           [this](uint16_t guestPort, const Bytes& reply) {
                               udpToGuest(kDnsIp, kDnsPort, guestPort, reply.data(),
                                          reply.size());
                           });
                break;
            case Route::Host:
                udp_.sendFromGuest(datagram.sourcePort, host, packet.destination,
                                   datagram.payload, now());
                break;
            case Route::Refused:
                noteRefused(packet.destination, datagram.destinationPort, "UDP");
                break;
        }
    }

    /// Said once per session: a guest retrying a refused destination would
    /// otherwise fill the log, and the first line is the one that matters.
    void noteRefused(uint32_t destination, uint16_t port, const char* what)
    {
        ++framesDropped_;
        if (refusalWarned_) return;
        refusalWarned_ = true;
        const uint8_t top = static_cast<uint8_t>(destination >> 24);
        const bool loopback = destination == kGatewayIp || top == 127;
        log().warn("Net", std::string("built-in NAT: guest ") + what + " to " +
                              dotted(destination) + ":" + std::to_string(port) +
                              " refused" +
                              (loopback ? " — the host's loopback; set "
                                          "uthernet_allow_loopback to opt in"
                                        : "") +
                              " (said once)");
    }

    // ── GuestSink ─────────────────────────────────────────────────────────
    void udpToGuest(uint32_t sourceIp, uint16_t sourcePort, uint16_t guestPort,
                    const uint8_t* data, std::size_t size) override
    {
        queueIpv4(sourceIp, guestIp_, kProtoUdp,
                  buildUdp(sourceIp, guestIp_, sourcePort, guestPort, data, size));
    }

    void tcpToGuest(const TcpSegment& segment) override
    {
        queueIpv4(segment.sourceIp, segment.destinationIp, kProtoTcp, buildTcp(segment));
    }

    uint32_t guestIp() const override { return guestIp_; }

    void queueIpv4(uint32_t source, uint32_t destination, uint8_t protocol,
                   const Bytes& payload)
    {
        queue(buildEthernet(guestMac_, kGatewayMac, kEtherTypeIpv4,
                            buildIpv4(source, destination, protocol, nextIpId_++, payload)));
    }

    void queue(Bytes frame)
    {
        if (frame.size() > static_cast<std::size_t>(kMaxEthFrame) ||
            outbound_.size() >= kMaxQueued) {
            ++framesDropped_;
            return;
        }
        outbound_.push_back(std::move(frame));
    }

    RoutePolicy                    policy_;
    std::function<uint64_t()>      nowMs_;
    std::unique_ptr<HostNetwork>   network_;
    std::unique_ptr<W5100Resolver> resolver_;
    DnsProxy                       dns_;
    UdpRelay                       udp_;
    TcpRelay                       tcp_;
    std::deque<Bytes>              outbound_;
    Mac                            guestMac_ = kBroadcastMac;
    uint32_t                       guestIp_  = kGuestIp;
    uint16_t                       nextIpId_ = 0;
    bool                           refusalWarned_ = false;
    std::string                    name_;
};

}  // namespace

bool builtinNatAvailable()
{
    return POM2_HAS_SOCKETS != 0;
}

std::unique_ptr<NetworkBackend> makeBuiltinNatBackend(
    const BuiltinNatOptions& options, std::unique_ptr<nat::HostNetwork> network,
    std::unique_ptr<W5100Resolver> resolver)
{
    if (!network) network = nat::makeHostNetwork();
    if (!network) return nullptr;
    if (!resolver) resolver = std::make_unique<W5100NameResolver>();
    return std::make_unique<BuiltinNat>(options, std::move(network), std::move(resolver));
}

}  // namespace pom2
