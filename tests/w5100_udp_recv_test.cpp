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

// W5100 receive-path robustness test — pins the rules the UDP half of
// `W5100Device::receiveOnePacketFromSocket` has to obey, plus the $8000
// address mirror on the read side.
//
// Reference: AppleWin `source/Uthernet2.cpp:704-745` (the receive path this
// class is ported from), WIZnet W5100 datasheet v1.2.8 §5.2.2 (the UDP RX
// header: source IP, source port, then the payload LENGTH), and the
// Uthernet II manual p.13 (addresses >= $8000 mirror the 32 KB map).
//
// The guest is driven exactly as a 6502 driver drives the chip — register
// writes and an RX ring read — against a REAL loopback UDP peer this test
// opens itself. Everything host-side goes through SocketCompat.h, so the
// file builds and runs on Winsock too; that matters here because two of
// the failures being pinned only ever surface on Windows (recvfrom failing
// an oversized datagram with WSAEMSGSIZE, and an ICMP port-unreachable
// arriving as WSAECONNRESET).
//
// What is pinned, and why each one is worth a test:
//
//   1. A datagram that does not fit the RX ring is DROPPED WHOLE. It used
//      to be read into a buffer sized from the ring's remaining room,
//      which on POSIX returns a TRUNCATED datagram with no error at all
//      (recvfrom discards the rest), so the in-band length stamped into
//      the ring described a datagram the guest never received. On Winsock
//      the same call fails with WSAEMSGSIZE, which the error arm read as
//      "socket is dead". Either the whole datagram lands with a correct
//      length, or nothing does — and the socket survives either way.
//   2. A datagram that DOES fit still lands whole, with the 8-byte header
//      the datasheet specifies. (Regression guard for rule 1.)
//   3. A ZERO-LENGTH datagram is a datagram, not a peer close. recvfrom
//      returning 0 on a datagram socket used to take the "connection
//      closed" arm and destroy the guest's socket; an empty keepalive
//      from any peer was enough.
//   4. Reads through the $8000 mirror decode like the low copy. Writes
//      masked the address first and reads did not, so $8403 read plain
//      memory (always 0) instead of S0's status, and $8426 never pulled
//      a packet in.

#include "SocketCompat.h"
#include "W5100Device.h"
#include "W5100HostSockets.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if !POM2_HAS_SOCKETS
int main()
{
    std::puts("SKIP: built without host sockets (Emscripten)");
    return 77;   // ctest SKIP_RETURN_CODE
}
#else

#include <chrono>
#include <thread>

namespace {

using namespace pom2;

constexpr uint16_t kS0 = kW5100S0Base;
/// UDP RX header: source IP (4) + source port (2) + length (2).
constexpr uint16_t kUdpHeader = 8;

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void writeWord(W5100Device& dev, uint16_t address, uint16_t value)
{
    dev.writeValueAt(address, static_cast<uint8_t>((value >> 8) & 0xFF));
    dev.writeValueAt(static_cast<uint16_t>(address + 1),
                     static_cast<uint8_t>(value & 0xFF));
}

uint16_t peekWord(const W5100Device& dev, uint16_t address)
{
    return static_cast<uint16_t>(
        (dev.peekValueAt(address) << 8) |
        dev.peekValueAt(static_cast<uint16_t>(address + 1)));
}

/// A UDP socket on 127.0.0.1, port chosen by the kernel. Stands in for
/// whatever the guest is talking to.
class LocalPeer
{
public:
    LocalPeer()
    {
        ensureSocketStack();
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        assert(isValidSocket(fd_));
        // Non-blocking rather than MSG_DONTWAIT, which Winsock has no
        // equivalent of.
        assert(setNonBlocking(fd_));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        assert(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_c len = sizeof(addr);
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
    }
    ~LocalPeer() { closeHostSocket(fd_); }

    /// The peer's own socket, for a test that reads a raw datagram back.
    socket_t fd() const { return fd_; }

    uint16_t port() const { return port_; }

    /// Wait for the guest's first datagram, which is how this side learns
    /// the ephemeral port the guest's unbound socket picked.
    bool learnGuestAddress(int timeoutMs)
    {
        char buf[64];
        for (int waited = 0; waited < timeoutMs; waited += 5) {
            socklen_c len = sizeof(guest_);
            // The `char*` + `int` spelling is the one that compiles on
            // both families (SocketCompat.h, trap 2/3).
            const iolen_t n = ::recvfrom(fd_, buf, static_cast<int>(sizeof(buf)), 0,
                                         reinterpret_cast<sockaddr*>(&guest_), &len);
            if (n >= 0) return true;
            sleepMs(5);
        }
        return false;
    }

    /// Send to a port the guest CHOSE, with no prior traffic from it — the
    /// unsolicited case (a DHCP OFFER, an NTP or TFTP reply).
    void sendToPort(uint16_t port, size_t length)
    {
        sockaddr_in to{};
        to.sin_family      = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port        = htons(port);
        std::vector<uint8_t> payload(length);
        for (size_t i = 0; i < length; ++i)
            payload[i] = static_cast<uint8_t>(i & 0xFF);
        ::sendto(fd_, reinterpret_cast<const char*>(payload.data()),
                 static_cast<int>(payload.size()), 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    /// Send `length` bytes of a recognisable pattern to the guest.
    void sendToGuest(size_t length)
    {
        std::vector<uint8_t> payload(length);
        for (size_t i = 0; i < length; ++i)
            payload[i] = static_cast<uint8_t>(i & 0xFF);
        const iolen_t n = ::sendto(fd_, reinterpret_cast<const char*>(payload.data()),
                                   static_cast<int>(payload.size()), 0,
                                   reinterpret_cast<const sockaddr*>(&guest_),
                                   sizeof(guest_));
        assert(n == static_cast<iolen_t>(length));
    }

private:
    socket_t    fd_   = kInvalidSocket;
    uint16_t    port_ = 0;
    sockaddr_in guest_{};
};

/// Open socket 0 in UDP mode with an `rmsr`-sized RX ring, then make the
/// guest send one byte so the peer learns its ephemeral port.
void openUdpSocket(W5100Device& dev, LocalPeer& peer, uint8_t rmsr)
{
    dev.reset(true);
    dev.writeValueAt(kW5100Rmsr, rmsr);

    dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnMr), kW5100SnMrUdp);
    dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnCr), kW5100SnCrOpen);
    assert(dev.readValueAt(static_cast<uint16_t>(kS0 + kW5100SnSr)) == kW5100SnSrUdp);

    // Destination 127.0.0.1 : peer port.
    dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnDipr0), 127);
    dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnDipr0 + 1), 0);
    dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnDipr0 + 2), 0);
    dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnDipr3), 1);
    writeWord(dev, static_cast<uint16_t>(kS0 + kW5100SnDport0), peer.port());

    // Stage one byte in the TX ring, advance SN_TX_WR, SEND.
    dev.writeValueAt(kW5100TxBase, 0x2A);
    writeWord(dev, static_cast<uint16_t>(kS0 + kW5100SnTxWr0), 1);
    dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnCr), kW5100SnCrSend);

    assert(peer.learnGuestAddress(2000));
}

/// Poll SN_RX_RSR the way a driver does — reading it is what pulls a
/// packet off the host socket — until something is staged or time is up.
/// `regBase` lets a caller poll through the $8000 mirror instead.
///
/// SN_RX_RSR is a 16-bit register read one byte at a time, and on this chip
/// reading it is ALSO what pulls a datagram off the host socket (it is polled,
/// there is no RX interrupt). So the two byte reads are not a snapshot: a
/// datagram arriving between them yields a torn value — the old hi with the
/// new lo — which read a 1408-byte datagram back as 128 about once in 40 runs.
/// A real W5100 driver reads RSR until two consecutive reads agree (datasheet
/// §5.2.2); this does the same. The inner loop is bounded because after the
/// single datagram these tests send is staged, rxSize stops moving.
uint16_t readRxRsr(W5100Device& dev, uint16_t regBase)
{
    const uint8_t hi = dev.readValueAt(
        static_cast<uint16_t>(regBase + kW5100SnRxRsr0));
    const uint8_t lo = dev.readValueAt(
        static_cast<uint16_t>(regBase + kW5100SnRxRsr1));
    return static_cast<uint16_t>((hi << 8) | lo);
}

uint16_t pollForData(W5100Device& dev, int timeoutMs, uint16_t regBase = kS0)
{
    for (int waited = 0; waited < timeoutMs; waited += 5) {
        uint16_t a = readRxRsr(dev, regBase);
        for (int settle = 0; settle < 8; ++settle) {
            const uint16_t b = readRxRsr(dev, regBase);
            if (b == a) break;
            a = b;
        }
        if (a != 0) return a;
        sleepMs(5);
    }
    return 0;
}

/// Read `count` bytes out of socket 0's RX ring starting at SN_RX_RD.
std::vector<uint8_t> drainRing(W5100Device& dev, uint16_t count)
{
    const uint16_t ring = dev.socketInfo(0).rxCapacity;
    const uint16_t rd   = peekWord(dev, static_cast<uint16_t>(kS0 + kW5100SnRxRd0));
    std::vector<uint8_t> out;
    out.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        out.push_back(dev.peekValueAt(
            static_cast<uint16_t>(kW5100RxBase + ((rd + i) % ring))));
    }
    return out;
}

// ─── 1: an oversized datagram is dropped, never truncated ─────────────
//
// RMSR $00 gives socket 0 a 1 KB ring, a legal and common carve. The
// datagram is a plain 1472-byte one (an Ethernet MTU minus IP+UDP
// headers) — the size any real UDP reply arrives at.
void testOversizedDatagramIsDropped()
{
    LocalPeer peer;
    W5100Device dev;
    // The device no longer builds its own host sockets — whoever
    // plugs it injects them. Inject the production factory so this
    // test exercises the same path the emulator does.
    dev.setSocketFactory(pom2::makeHostW5100SocketFactory());
    openUdpSocket(dev, peer, 0x00);
    assert(dev.socketInfo(0).rxCapacity == 1024);

    constexpr size_t kDatagram = 1472;
    peer.sendToGuest(kDatagram);

    const uint16_t pending = pollForData(dev, 500);

    // The ring cannot hold 1472 + 8, so nothing may be staged. What must
    // NEVER happen is a partial datagram carrying a length that does not
    // describe it: that is what a ring-sized read buffer produced.
    if (pending != 0) {
        const auto staged = drainRing(dev, pending);
        const uint16_t stamped = static_cast<uint16_t>((staged[6] << 8) | staged[7]);
        std::printf("  FAIL: staged %u bytes stamped as a %u-byte datagram "
                    "(really %zu)\n", pending, stamped, kDatagram);
        assert(false && "truncated datagram staged in the RX ring");
    }
    // The socket also survives: a datagram too big for the ring costs one
    // packet, not the guest's socket. This is the half that fails on
    // Windows, where the same call comes back as WSAEMSGSIZE.
    assert(dev.readValueAt(static_cast<uint16_t>(kS0 + kW5100SnSr)) == kW5100SnSrUdp);
    assert(dev.socketInfo(0).hasHostSocket);

    std::puts("OK oversized_datagram_dropped_whole");
}

// ─── 2: a datagram that fits lands whole ──────────────────────────────
void testFittingDatagramLandsWhole()
{
    LocalPeer peer;
    W5100Device dev;
    // The device no longer builds its own host sockets — whoever
    // plugs it injects them. Inject the production factory so this
    // test exercises the same path the emulator does.
    dev.setSocketFactory(pom2::makeHostW5100SocketFactory());
    openUdpSocket(dev, peer, 0x02);           // 4 KB ring for socket 0
    assert(dev.socketInfo(0).rxCapacity == 4096);

    constexpr size_t kDatagram = 1472;
    peer.sendToGuest(kDatagram);

    const uint16_t pending = pollForData(dev, 2000);
    assert(pending == kUdpHeader + kDatagram);

    const auto staged = drainRing(dev, pending);
    // Source IP is 127.0.0.1 and the port is the peer's, both in network
    // order in the header (datasheet §5.2.2).
    assert(staged[0] == 127 && staged[1] == 0 && staged[2] == 0 && staged[3] == 1);
    assert(((staged[4] << 8) | staged[5]) == peer.port());
    assert(((staged[6] << 8) | staged[7]) == static_cast<int>(kDatagram));
    for (size_t i = 0; i < kDatagram; ++i)
        assert(staged[kUdpHeader + i] == static_cast<uint8_t>(i & 0xFF));

    std::puts("OK fitting_datagram_lands_whole");
}

// ─── 3: a zero-length datagram is not a close ─────────────────────────
void testZeroLengthDatagramKeepsSocket()
{
    LocalPeer peer;
    W5100Device dev;
    // The device no longer builds its own host sockets — whoever
    // plugs it injects them. Inject the production factory so this
    // test exercises the same path the emulator does.
    dev.setSocketFactory(pom2::makeHostW5100SocketFactory());
    openUdpSocket(dev, peer, 0x02);

    peer.sendToGuest(0);

    // The header alone is staged, with a length of 0.
    const uint16_t pending = pollForData(dev, 2000);
    assert(pending == kUdpHeader);
    const auto staged = drainRing(dev, pending);
    assert(((staged[6] << 8) | staged[7]) == 0);

    // And the socket is still a live UDP socket, not CLOSED.
    assert(dev.readValueAt(static_cast<uint16_t>(kS0 + kW5100SnSr)) == kW5100SnSrUdp);
    assert(dev.socketInfo(0).hasHostSocket);

    std::puts("OK zero_length_datagram_keeps_socket");
}

// ─── 5: a full ring loses the next datagram, exactly as the chip does ─
//
// This pins a DELIBERATE loss, which is unusual enough to be worth stating.
// A real W5100 has nowhere to put a frame that does not fit its RX ring: it
// is lost on the wire. A guest streaming MTU-sized datagrams into the 2 KB
// power-on carve therefore loses them on real hardware too, and POM2 matches.
//
// The alternative — peek at the host socket and leave the datagram there
// until the guest drains the ring — was tried on 2026-08-22 and reverted: it
// emulates a buffer the chip does not have, and holding datagrams across
// socket teardown made this very file flaky (20/20 -> 17/20 runs). The test
// exists so the next person to "fix" this reads that first.
void testFullRingLosesTheNextDatagram()
{
    LocalPeer peer;
    W5100Device dev;
    // The device no longer builds its own host sockets — whoever
    // plugs it injects them. Inject the production factory so this
    // test exercises the same path the emulator does.
    dev.setSocketFactory(pom2::makeHostW5100SocketFactory());
    openUdpSocket(dev, peer, 0x01);           // 2 KB ring: one datagram fits
    assert(dev.socketInfo(0).rxCapacity == 2048);

    constexpr size_t kDatagram = 1400;
    peer.sendToGuest(kDatagram);
    const uint16_t first = pollForData(dev, 2000);
    assert(first == kUdpHeader + kDatagram);

    // Whatever happens to the second one, the socket must survive it and the
    // FIRST one must still be intact and correctly described — a datagram the
    // ring cannot take may cost a packet, never the guest's socket or the
    // packet already staged.
    peer.sendToGuest(kDatagram);
    for (int i = 0; i < 20; ++i) {
        (void)dev.readValueAt(static_cast<uint16_t>(kS0 + kW5100SnRxRsr0));
        (void)dev.readValueAt(static_cast<uint16_t>(kS0 + kW5100SnRxRsr1));
        sleepMs(5);
    }
    const uint16_t stillPending = static_cast<uint16_t>(
        (dev.readValueAt(static_cast<uint16_t>(kS0 + kW5100SnRxRsr0)) << 8) |
         dev.readValueAt(static_cast<uint16_t>(kS0 + kW5100SnRxRsr1)));
    assert(stillPending == first && "the staged datagram must not be disturbed");

    const auto staged = drainRing(dev, first);
    assert(((staged[6] << 8) | staged[7]) == static_cast<int>(kDatagram));
    assert(dev.readValueAt(static_cast<uint16_t>(kS0 + kW5100SnSr)) == kW5100SnSrUdp);
    assert(dev.socketInfo(0).hasHostSocket);

    std::puts("OK full_ring_loses_the_next_datagram");
}

// ─── 4: the $8000 mirror decodes on the read side too ─────────────────
void testHighMirrorReads()
{
    LocalPeer peer;
    W5100Device dev;
    // The device no longer builds its own host sockets — whoever
    // plugs it injects them. Inject the production factory so this
    // test exercises the same path the emulator does.
    dev.setSocketFactory(pom2::makeHostW5100SocketFactory());
    openUdpSocket(dev, peer, 0x02);

    constexpr uint16_t kMirror = static_cast<uint16_t>(kS0 + 0x8000);

    // Status through the mirror is the socket's status, not the (always
    // zero) byte of plain memory the unmasked range test fell through to.
    assert(dev.readValueAt(static_cast<uint16_t>(kMirror + kW5100SnSr)) ==
           kW5100SnSrUdp);
    assert(dev.peekValueAt(static_cast<uint16_t>(kMirror + kW5100SnSr)) ==
           kW5100SnSrUdp);

    // And $8426 pulls a packet in exactly like $0426 does: this whole
    // receive runs through the mirror.
    peer.sendToGuest(64);
    const uint16_t pending = pollForData(dev, 2000, kMirror);
    assert(pending == kUdpHeader + 64);

    std::puts("OK high_mirror_reads_decode_registers");
}

// ─── 6: Sn_PORT is claimed on the host socket ─────────────────────────
//
// The guest writes Sn_PORT and then OPEN, and the chip listens on THAT port
// (datasheet §5.2.1: Sn_PORT is set before OPEN). The host socket was never
// bound, so the kernel gave it an ephemeral port instead and the guest's
// choice meant nothing: every UNSOLICITED datagram — a DHCP OFFER to port 68,
// an NTP or TFTP reply, anything a peer sends without being spoken to first —
// arrived at a port nobody was reading, and the guest timed out with no error
// anywhere to point at. Outbound request/response worked, which is exactly
// why this survived: the socket looked fine.
void testLocalPortIsBound()
{
    // Learn a port nothing is using by taking one and giving it straight
    // back. A race with some other process is possible but not likely; the
    // retry covers it rather than making the test flaky.
    auto freeUdpPort = []() -> uint16_t {
        socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
        assert(isValidSocket(s));
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = 0;
        assert(::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        socklen_c len = sizeof(a);
        assert(::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0);
        const uint16_t p = ntohs(a.sin_port);
        closeHostSocket(s);
        return p;
    };

    constexpr size_t kDatagram = 32;
    for (int attempt = 0; attempt < 4; ++attempt) {
        LocalPeer  peer;
        W5100Device dev;
        dev.setSocketFactory(pom2::makeHostW5100SocketFactory());
        dev.reset(true);
        dev.writeValueAt(kW5100Rmsr, 0x02);            // 4 KB ring for socket 0

        const uint16_t chosen = freeUdpPort();
        // Sn_PORT BEFORE the OPEN command, the order the datasheet gives.
        writeWord(dev, static_cast<uint16_t>(kS0 + kW5100SnPort0), chosen);
        dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnMr), kW5100SnMrUdp);
        dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnCr), kW5100SnCrOpen);
        assert(dev.readValueAt(static_cast<uint16_t>(kS0 + kW5100SnSr)) ==
               kW5100SnSrUdp);
        assert(dev.socketInfo(0).localPort == chosen);

        // The guest has sent NOTHING. This is the peer talking first.
        peer.sendToPort(chosen, kDatagram);

        const uint16_t pending = pollForData(dev, 1500);
        if (pending == 0) continue;              // port stolen: try another
        assert(pending == kUdpHeader + kDatagram);

        const auto staged = drainRing(dev, pending);
        assert(staged[0] == 127 && staged[3] == 1);
        assert(((staged[4] << 8) | staged[5]) == peer.port());
        assert(((staged[6] << 8) | staged[7]) == static_cast<int>(kDatagram));
        for (size_t i = 0; i < kDatagram; ++i)
            assert(staged[kUdpHeader + i] == static_cast<uint8_t>(i & 0xFF));

        std::puts("OK local_port_is_bound_for_unsolicited_udp");
        return;
    }
    assert(false && "an unsolicited datagram never reached the guest's Sn_PORT");
}

// ─── 7: SEND_MAC transmits, it is not an unknown command ──────────────
//
// SEND_MAC ($21) is SEND with the ARP step skipped — the chip is told to use
// the MAC already in Sn_DHAR instead of resolving one (datasheet §5.1
// "Sn_CR"). POM2 runs UDP over a host socket, which does its own address
// resolution, so it is the same transmission SEND performs. It used to fall
// to `default: break`, so a driver that uses SEND_MAC once it has cached the
// peer's MAC — the WIZnet UDP example does exactly that — lost EVERY datagram
// while its TX ring filled up behind it and Sn_TX_FSR went to zero for good.
void testSendMacTransmits()
{
    LocalPeer   peer;
    W5100Device dev;
    dev.setSocketFactory(pom2::makeHostW5100SocketFactory());
    openUdpSocket(dev, peer, 0x02);   // also teaches the peer our address

    // Stage a second datagram and dispatch it with SEND_MAC this time.
    const uint16_t rd = peekWord(dev, static_cast<uint16_t>(kS0 + kW5100SnTxRd0));
    const uint8_t payload[3] = { 'M', 'A', 'C' };
    const uint16_t ring = dev.socketInfo(0).txCapacity;
    for (uint16_t i = 0; i < 3; ++i)
        dev.writeValueAt(static_cast<uint16_t>(kW5100TxBase + ((rd + i) % ring)),
                         payload[i]);
    writeWord(dev, static_cast<uint16_t>(kS0 + kW5100SnTxWr0),
              static_cast<uint16_t>(rd + 3));
    dev.writeValueAt(static_cast<uint16_t>(kS0 + kW5100SnCr), kW5100SnCrSendMac);

    // It reached the peer.
    char buf[16];
    bool got = false;
    for (int waited = 0; waited < 2000 && !got; waited += 5) {
        const iolen_t n = ::recvfrom(peer.fd(), buf, static_cast<int>(sizeof(buf)),
                                     0, nullptr, nullptr);
        if (n == 3 && std::memcmp(buf, payload, 3) == 0) got = true;
        else sleepMs(5);
    }
    assert(got && "SEND_MAC transmitted nothing");
    // And the ring was consumed, not left full.
    assert(peekWord(dev, static_cast<uint16_t>(kS0 + kW5100SnTxRd0)) ==
           static_cast<uint16_t>(rd + 3));

    std::puts("OK send_mac_transmits");
}

}  // namespace

int main()
{
    std::puts("=== W5100 receive-path robustness test ===");
    ensureSocketStack();
    testOversizedDatagramIsDropped();
    testFittingDatagramLandsWhole();
    testZeroLengthDatagramKeepsSocket();
    testFullRingLosesTheNextDatagram();
    testHighMirrorReads();
    testLocalPortIsBound();
    testSendMacTransmits();
    std::puts("All W5100 receive-path tests passed.");
    return 0;
}

#endif // POM2_HAS_SOCKETS
