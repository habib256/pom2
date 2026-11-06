// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NatDns — the built-in virtual network's DNS server. It ANSWERS the guest's
// queries itself, from the host's resolver; it does not relay them to a DNS
// server.
//
// Why not relay, which is what libslirp and Casso both do
// --------------------------------------------------------
// A relay has to know the address of the host's DNS server, and there is no
// portable way to ask: /etc/resolv.conf on POSIX (stale under a VPN on
// macOS, 127.0.0.53 under systemd-resolved), GetNetworkParams on Windows.
// Two code paths, one of which this project cannot run before shipping it.
// It also forwards whatever the guest wrote, unmetered, to a server outside
// the machine.
//
// Answering through the resolver POM2 already has (W5100Resolver, the
// Uthernet II's virtual DNS) has neither problem: getaddrinfo is the same
// call everywhere and honours whatever the host is configured with (hosts
// file, VPN, split DNS), and the resolver is already CAPPED and RATE-LIMITED
// — a guest looping queries over `<payload>.example` gets four new names a
// second, not a free channel out.
//
// The cost, stated: only A queries are answered with data. Any other type
// gets an empty NOERROR answer ("that name exists, no record of this type"),
// which is what a resolver without that record says, and no period Apple II
// client asks for anything else. Settled by: a guest program found asking
// for MX, TXT or PTR and needing the answer.
//
// [B] RFC 1035 § 4 (message format), § 4.1.1 (header, RCODE values).

#ifndef POM2_NAT_DNS_H
#define POM2_NAT_DNS_H

#include "NatPacket.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pom2 {

class W5100Resolver;

namespace nat {

inline constexpr uint16_t kDnsPort = 53;

class DnsProxy
{
public:
    /// A reply datagram for the guest's `guestPort`.
    using Emit = std::function<void(uint16_t guestPort, const Bytes& reply)>;

    /// Queries waiting for the host resolver at once. A full table answers
    /// SERVFAIL, and the guest's own retry comes back when there is room.
    static constexpr std::size_t kMaxPending = 8;
    /// How long a query may wait before it is answered SERVFAIL.
    static constexpr uint64_t    kTimeoutMs  = 8000;

    explicit DnsProxy(W5100Resolver* resolver) : resolver_(resolver) {}

    void query(uint16_t guestPort, View payload, uint64_t nowMs, const Emit& emit);
    void pump(uint64_t nowMs, const Emit& emit);
    void clear() { pending_.clear(); }
    std::size_t pending() const { return pending_.size(); }

private:
    struct Pending {
        uint16_t    guestPort = 0;
        Bytes       query;        ///< header + question, as received
        std::string name;
        uint64_t    deadlineMs = 0;
    };

    W5100Resolver*       resolver_;
    std::vector<Pending> pending_;
};

}  // namespace nat
}  // namespace pom2

#endif  // POM2_NAT_DNS_H
