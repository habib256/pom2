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

// SlirpNetworkBackend — user-mode NAT for POM2's Ethernet cards, built on
// libslirp (the TCP/IP stack QEMU's `-net user` uses).
//
// Why slirp and not TAP/pcap
// --------------------------
// The CS8900A (Uthernet I) is a raw NIC: the Apple-side software carries
// its own stack, so the host has to bridge whole Ethernet frames. The two
// classic ways to do that — a TAP device or libpcap on a real interface —
// both need root (CAP_NET_ADMIN / CAP_NET_RAW). libslirp instead
// terminates the guest's IP inside our process and re-opens ordinary
// user-space sockets to the outside world: no privileges, no host
// configuration, works identically in CI. The cost is slirp's known
// limits — no inbound connections unless port-forwarded, no ICMP unless
// the host allows unprivileged ping sockets, and the guest cannot be
// reached from the LAN.
//
// Virtual network (libslirp defaults, same as QEMU)
// ------------------------------------------------
//   10.0.2.0/24     the virtual network
//   10.0.2.2        the virtual router / gateway (the host)
//   10.0.2.3        the virtual DNS server
//   10.0.2.15       what the DHCP server hands out — configure IP65 /
//                   Contiki with this if you skip DHCP
//
// Build gate: this file only compiles to something functional when
// POM2_HAVE_SLIRP is defined (CMake sets it when pkg-config finds
// `slirp`). Otherwise `create()` returns nullptr and callers fall back to
// NullNetworkBackend, so the cards stay pluggable on a slirp-less build.

#ifndef POM2_SLIRP_NETWORK_BACKEND_H
#define POM2_SLIRP_NETWORK_BACKEND_H

#include "NetworkBackend.h"

#include <memory>
#include <string>

namespace pom2 {

/// Build-time availability of the libslirp backend. Prefer this over
/// `#ifdef POM2_HAVE_SLIRP` in callers — the UI wants to *say* why
/// networking is unavailable, not silently hide the option.
bool slirpAvailable();

/// Policy knobs for the virtual network. Both defaults are the SAFE answer,
/// and the wiring point for overriding them is `makeEthernetBackend` in
/// MainWindow_SlotConfig.cpp:
///     opts.allowHostLoopback = settings->getBool("slirp_allow_host_loopback", false);
///     opts.restricted        = settings->getBool("slirp_restricted", false);
struct SlirpOptions {
    /// Let the guest reach the HOST's loopback interface through the virtual
    /// router at 10.0.2.2.
    ///
    /// FALSE by default, and that is a change: libslirp's
    /// `disable_host_loopback` was left at 0, so a CS8900A guest carrying its
    /// own IP stack (IP65, Contiki — the whole reason the Uthernet I exists)
    /// could open 10.0.2.2:<port> and slirp would re-open it as 127.0.0.1:<port>
    /// on the host. POM2's own AI control server listens there and treats a
    /// loopback client with no Origin as native, so /mem, /disk and /reset were
    /// reachable from inside the emulated machine. This is the same boundary
    /// W5100Device::setAllowLoopback draws for the Uthernet II, and the two
    /// have to be drawn together or the escape simply moves to the other card.
    ///
    /// Nothing else changes: the LAN, the internet and the virtual DHCP/DNS
    /// servers at 10.0.2.2-3 all keep working. Only the host's own private
    /// services stop being addressable.
    bool allowHostLoopback = false;

    /// libslirp's `restricted` mode: the guest may talk to the virtual
    /// services (DHCP, DNS, TFTP) and to nothing else. FALSE, which is the
    /// behaviour POM2 has always had — a period network client that cannot
    /// reach the internet is not much of a network client. Exposed so a user
    /// who wants the card present but inert has an answer short of unplugging.
    bool restricted = false;
};

/// Construct a libslirp-backed backend, or nullptr when unavailable
/// (not compiled in, or slirp_new failed). `hostname` is what the
/// virtual DHCP server reports; empty picks libslirp's default.
std::unique_ptr<NetworkBackend> makeSlirpBackend(const std::string& hostname = {},
                                                 const SlirpOptions& options = {});

} // namespace pom2

#endif // POM2_SLIRP_NETWORK_BACKEND_H
