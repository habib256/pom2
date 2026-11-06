// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NatPacket — see the header.

#include "NatPacket.h"

#include <algorithm>
#include <cstring>

namespace pom2 {
namespace nat {

uint16_t rd16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t rd32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
}

void wr16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void wr32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

namespace {

uint32_t sumWords(const uint8_t* data, std::size_t size, uint32_t sum)
{
    std::size_t i = 0;
    for (; i + 1 < size; i += 2) sum += rd16(data + i);
    if (i < size) sum += static_cast<uint32_t>(data[i]) << 8;   // odd tail, zero-padded
    return sum;
}

/// The TCP/UDP pseudo-header (RFC 793 § 3.1, RFC 768).
uint32_t sumPseudoHeader(uint32_t source, uint32_t destination, uint8_t protocol,
                         std::size_t length)
{
    uint32_t sum = 0;
    sum += source >> 16;      sum += source & 0xFFFF;
    sum += destination >> 16; sum += destination & 0xFFFF;
    sum += protocol;
    sum += static_cast<uint32_t>(length);
    return sum;
}

uint16_t fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

}  // namespace

uint16_t checksum(const uint8_t* data, std::size_t size, uint32_t sum)
{
    return fold(sumWords(data, size, sum));
}

bool parseEthernet(const uint8_t* frame, std::size_t size, EthernetFrame& out)
{
    if (!frame || size < kEthernetHeader) return false;
    std::memcpy(out.destination.data(), frame, 6);
    std::memcpy(out.source.data(), frame + 6, 6);
    out.etherType = rd16(frame + 12);
    out.payload   = { frame + kEthernetHeader, size - kEthernetHeader };
    return true;
}

bool parseArp(View p, ArpPacket& out)
{
    if (p.size < kArpLength) return false;
    // Ethernet (1) carrying IPv4 (0x0800), 6-byte and 4-byte addresses.
    if (rd16(p.data) != 1 || rd16(p.data + 2) != kEtherTypeIpv4 ||
        p.data[4] != 6 || p.data[5] != 4)
        return false;
    out.operation = rd16(p.data + 6);
    std::memcpy(out.senderMac.data(), p.data + 8, 6);
    out.senderIp = rd32(p.data + 14);
    std::memcpy(out.targetMac.data(), p.data + 18, 6);
    out.targetIp = rd32(p.data + 24);
    return true;
}

bool parseIpv4(View p, Ipv4Packet& out)
{
    if (p.size < kIpv4Header) return false;
    if ((p.data[0] >> 4) != 4) return false;
    const std::size_t headerLength = static_cast<std::size_t>(p.data[0] & 0x0F) * 4;
    if (headerLength < kIpv4Header || headerLength > p.size) return false;
    const std::size_t totalLength = rd16(p.data + 2);
    // A frame is padded to 60 bytes, so the buffer may be LONGER than the
    // datagram; it may never be shorter.
    if (totalLength < headerLength || totalLength > p.size) return false;
    // More-fragments set, or a non-zero offset: a fragment.
    if ((rd16(p.data + 6) & 0x3FFF) != 0) return false;
    if (checksum(p.data, headerLength) != 0) return false;
    out.protocol    = p.data[9];
    out.source      = rd32(p.data + 12);
    out.destination = rd32(p.data + 16);
    out.payload     = { p.data + headerLength, totalLength - headerLength };
    return true;
}

bool parseUdp(const Ipv4Packet& packet, UdpDatagram& out)
{
    const View p = packet.payload;
    if (p.size < kUdpHeader) return false;
    const std::size_t length = rd16(p.data + 4);
    if (length < kUdpHeader || length > p.size) return false;
    // A zero checksum means the sender computed none (RFC 768).
    if (rd16(p.data + 6) != 0 &&
        checksum(p.data, length,
                 sumPseudoHeader(packet.source, packet.destination, kProtoUdp, length)) != 0)
        return false;
    out.sourcePort      = rd16(p.data);
    out.destinationPort = rd16(p.data + 2);
    out.payload         = { p.data + kUdpHeader, length - kUdpHeader };
    return true;
}

bool parseTcp(const Ipv4Packet& packet, TcpSegment& out)
{
    const View p = packet.payload;
    if (p.size < kTcpHeader) return false;
    const std::size_t headerLength = static_cast<std::size_t>(p.data[12] >> 4) * 4;
    if (headerLength < kTcpHeader || headerLength > p.size) return false;
    if (checksum(p.data, p.size,
                 sumPseudoHeader(packet.source, packet.destination, kProtoTcp, p.size)) != 0)
        return false;

    out = TcpSegment{};
    out.sourceIp        = packet.source;
    out.destinationIp   = packet.destination;
    out.sourcePort      = rd16(p.data);
    out.destinationPort = rd16(p.data + 2);
    out.sequence        = rd32(p.data + 4);
    out.acknowledgment  = rd32(p.data + 8);
    out.flags           = p.data[13];
    out.window          = rd16(p.data + 14);

    // Options: kind 0 ends the list, 1 is a one-byte pad, anything else
    // carries its own length. Only MSS (kind 2, length 4) is read.
    for (std::size_t i = kTcpHeader; i < headerLength;) {
        const uint8_t kind = p.data[i];
        if (kind == 0) break;
        if (kind == 1) { ++i; continue; }
        if (i + 1 >= headerLength) break;
        const std::size_t optionLength = p.data[i + 1];
        if (optionLength < 2 || i + optionLength > headerLength) break;
        if (kind == 2 && optionLength == 4) out.mss = rd16(p.data + i + 2);
        i += optionLength;
    }
    out.payload.assign(p.data + headerLength, p.data + p.size);
    return true;
}

Bytes buildEthernet(const Mac& destination, const Mac& source, uint16_t etherType,
                    const Bytes& payload)
{
    Bytes frame(std::max(kEthernetHeader + payload.size(), kMinimumFrame), 0);
    std::memcpy(frame.data(), destination.data(), 6);
    std::memcpy(frame.data() + 6, source.data(), 6);
    wr16(frame.data() + 12, etherType);
    if (!payload.empty())
        std::memcpy(frame.data() + kEthernetHeader, payload.data(), payload.size());
    return frame;
}

Bytes buildArp(const ArpPacket& packet)
{
    Bytes p(kArpLength, 0);
    wr16(p.data(), 1);
    wr16(p.data() + 2, kEtherTypeIpv4);
    p[4] = 6;
    p[5] = 4;
    wr16(p.data() + 6, packet.operation);
    std::memcpy(p.data() + 8, packet.senderMac.data(), 6);
    wr32(p.data() + 14, packet.senderIp);
    std::memcpy(p.data() + 18, packet.targetMac.data(), 6);
    wr32(p.data() + 24, packet.targetIp);
    return p;
}

Bytes buildIpv4(uint32_t source, uint32_t destination, uint8_t protocol,
                uint16_t identification, const Bytes& payload)
{
    Bytes p(kIpv4Header + payload.size(), 0);
    p[0] = 0x45;                                            // v4, 20-byte header
    wr16(p.data() + 2, static_cast<uint16_t>(p.size()));
    wr16(p.data() + 4, identification);
    wr16(p.data() + 6, 0x4000);                             // don't fragment
    p[8] = kDefaultTtl;
    p[9] = protocol;
    wr32(p.data() + 12, source);
    wr32(p.data() + 16, destination);
    wr16(p.data() + 10, checksum(p.data(), kIpv4Header));
    if (!payload.empty())
        std::memcpy(p.data() + kIpv4Header, payload.data(), payload.size());
    return p;
}

Bytes buildUdp(uint32_t sourceIp, uint32_t destinationIp, uint16_t sourcePort,
               uint16_t destinationPort, const uint8_t* data, std::size_t size)
{
    Bytes p(kUdpHeader + size, 0);
    wr16(p.data(), sourcePort);
    wr16(p.data() + 2, destinationPort);
    wr16(p.data() + 4, static_cast<uint16_t>(p.size()));
    if (size) std::memcpy(p.data() + kUdpHeader, data, size);
    uint16_t sum = checksum(p.data(), p.size(),
                            sumPseudoHeader(sourceIp, destinationIp, kProtoUdp, p.size()));
    if (sum == 0) sum = 0xFFFF;                             // 0 would mean "none"
    wr16(p.data() + 6, sum);
    return p;
}

Bytes buildTcp(const TcpSegment& s)
{
    const std::size_t headerLength = kTcpHeader + (s.mss != 0 ? 4 : 0);
    Bytes p(headerLength + s.payload.size(), 0);
    wr16(p.data(), s.sourcePort);
    wr16(p.data() + 2, s.destinationPort);
    wr32(p.data() + 4, s.sequence);
    wr32(p.data() + 8, s.acknowledgment);
    p[12] = static_cast<uint8_t>((headerLength / 4) << 4);
    p[13] = s.flags;
    wr16(p.data() + 14, s.window);
    if (s.mss != 0) {
        p[kTcpHeader]     = 2;
        p[kTcpHeader + 1] = 4;
        wr16(p.data() + kTcpHeader + 2, s.mss);
    }
    if (!s.payload.empty())
        std::memcpy(p.data() + headerLength, s.payload.data(), s.payload.size());
    wr16(p.data() + 16,
         checksum(p.data(), p.size(),
                  sumPseudoHeader(s.sourceIp, s.destinationIp, kProtoTcp, p.size())));
    return p;
}

Bytes buildIcmpEchoReply(View request)
{
    Bytes p(request.data, request.data + request.size);
    if (p.size() < kIcmpHeader) return {};
    p[0] = kIcmpEchoReply;
    p[2] = p[3] = 0;
    wr16(p.data() + 2, checksum(p.data(), p.size()));
    return p;
}

}  // namespace nat
}  // namespace pom2
