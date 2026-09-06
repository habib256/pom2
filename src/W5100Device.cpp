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

// W5100Device — see the header for the chip overview and why the TCP/UDP
// paths map onto host sockets. Reference: AppleWin `source/Uthernet2.cpp`
// (GPL-2.0+, Andrea Odetti) + WIZnet W5100 datasheet v1.2.8; the citations
// in comments are AppleWin line numbers.

#include "W5100Device.h"

#include "W5100Resolver.h"
#include "Pom2Build.h"

#include "Logger.h"

#include <algorithm>
#include <cstring>

// Byte-order helpers only. The host socket API itself is reached through the
// injected W5100SocketFactory — this device does not include it, and the
// configure-time layer guard enforces that.
#include "NetworkValues.h"

#if POM2_HAS_SOCKETS
// Under Emscripten there is no usable BSD-socket API, so the TCP/UDP paths
// compile out and those socket modes stay CLOSED — the register model, the
// RX/TX rings and MACRAW/IPRAW (which go through the NetworkBackend, not
// through sockets) are unaffected. Same treatment SuperSerialCard gives its
// telnet listener.
#include <chrono>
#endif

namespace pom2 {
namespace {

constexpr uint32_t kSnapMagic   = 0x30303135;  // '5100'
constexpr uint16_t kSnapVersion = 1;

/// Dest MAC + source MAC + EtherType.
constexpr int kEthMinimumSize = 6 + 6 + 2;

/// Scratch size for one recvfrom on a TCP/UDP socket. Deliberately the
/// whole 8 KB RX region rather than an MTU: a single socket may own all of
/// it (RMSR $FF), and a datagram is read WHOLE or not at all. Sizing it
/// this way also makes truncation unstageable by construction — a datagram
/// long enough to fill this buffer cannot pass ringHasRoomFor(), whose
/// ceiling is the same 8 KB minus the in-band header, so a truncated read
/// is always dropped instead of being stamped with a wrong length.
constexpr size_t kMaxDatagram = kW5100MemSize - kW5100RxBase;   // 8 KB

uint8_t indexByte(uint16_t value, unsigned shift)
{
    return static_cast<uint8_t>((value >> shift) & 0xFF);
}

// ── IPv4 / Ethernet framing for IPRAW ─────────────────────────────────
// AppleWin keeps these in `source/Tfe/IPRaw.cpp`; they are only needed by
// the IPRAW path so POM2 keeps them file-local to the chip that uses them.

#pragma pack(push, 1)
struct Ip4Header {
    uint8_t  ihlVersion;          // low nibble = IHL, high nibble = version
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t flagsFragment;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t sourceAddress;
    uint32_t destinationAddress;
};
struct Eth2Frame {
    uint8_t  destinationMac[6];
    uint8_t  sourceMac[6];
    uint16_t type;
};
#pragma pack(pop)

static_assert(sizeof(Ip4Header) == 20, "IPv4 header must be 20 bytes");
static_assert(sizeof(Eth2Frame) == 14, "Ethernet II header must be 14 bytes");

/// RFC 1071 Internet checksum.
uint16_t internetChecksum(const void* addr, int count)
{
    uint32_t sum = 0;
    const auto* p = static_cast<const uint8_t*>(addr);
    while (count > 1) {
        uint16_t word;
        std::memcpy(&word, p, 2);
        sum += word;
        p += 2;
        count -= 2;
    }
    if (count > 0) sum += *p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

/// Wrap an IP payload in IPv4 + Ethernet II headers. Mirrors AppleWin's
/// `createETH2Frame` (`IPRaw.cpp:104-132`).
std::vector<uint8_t> createEth2Frame(const std::vector<uint8_t>& data,
                                     const MacAddress& sourceMac,
                                     const MacAddress& destinationMac,
                                     uint8_t ttl, uint8_t tos, uint8_t protocol,
                                     uint32_t sourceAddress,
                                     uint32_t destinationAddress)
{
    const size_t total = sizeof(Eth2Frame) + sizeof(Ip4Header) + data.size();
    std::vector<uint8_t> frame(total, 0);

    Eth2Frame eth{};
    std::memcpy(eth.destinationMac, destinationMac.b, 6);
    std::memcpy(eth.sourceMac,      sourceMac.b,      6);
    eth.type = pom2::hostToNet16(0x0800);
    std::memcpy(frame.data(), &eth, sizeof(eth));

    Ip4Header ip{};
    ip.ihlVersion = 0x45;   // version 4, IHL 5 (20 bytes, no options)
    ip.tos        = tos;
    ip.len = pom2::hostToNet16(
        static_cast<uint16_t>(sizeof(Ip4Header) + data.size()));
    ip.id            = 0;
    ip.flagsFragment = 0;
    ip.ttl           = ttl;
    ip.proto         = protocol;
    ip.checksum      = 0;
    ip.sourceAddress      = sourceAddress;
    ip.destinationAddress = destinationAddress;
    ip.checksum = internetChecksum(&ip, sizeof(ip));
    std::memcpy(frame.data() + sizeof(Eth2Frame), &ip, sizeof(ip));

    if (!data.empty()) {
        std::memcpy(frame.data() + sizeof(Eth2Frame) + sizeof(Ip4Header),
                    data.data(), data.size());
    }
    return frame;
}

/// Pull the IP payload out of an Ethernet frame. Sets protocol to 0xFF
/// (reserved) and payload to null when the frame is not usable IPv4.
/// Mirrors `IPRaw.cpp:134-162`.
void getIpPayload(const uint8_t* frame, int lengthOfFrame,
                  const uint8_t*& payload, size_t& lengthOfPayload,
                  uint32_t& source, uint8_t& protocol)
{
    protocol = 0xFF;
    payload  = nullptr;
    lengthOfPayload = 0;
    source = 0;

    const int minimumSize = static_cast<int>(sizeof(Eth2Frame) + sizeof(Ip4Header));
    if (lengthOfFrame <= minimumSize) return;

    Eth2Frame eth{};
    Ip4Header ip{};
    std::memcpy(&eth, frame, sizeof(eth));
    std::memcpy(&ip, frame + sizeof(Eth2Frame), sizeof(ip));

    if (eth.type != pom2::hostToNet16(0x0800)) return;
    if ((ip.ihlVersion >> 4) != 4) return;

    const uint16_t ipv4HeaderSize = static_cast<uint16_t>((ip.ihlVersion & 0x0F) * 4);
    const uint16_t ipPacketSize   = pom2::netToHost16(ip.len);
    const int      expectedSize   = static_cast<int>(sizeof(Eth2Frame)) + ipPacketSize;

    if (ipPacketSize <= ipv4HeaderSize) return;
    if (lengthOfFrame < expectedSize) return;
    if (ipv4HeaderSize < sizeof(Ip4Header)) return;

    protocol        = ip.proto;
    payload         = frame + sizeof(Eth2Frame) + ipv4HeaderSize;
    lengthOfPayload = static_cast<size_t>(ipPacketSize - ipv4HeaderSize);
    source          = ip.sourceAddress;
}


void putU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void putU64(std::vector<uint8_t>& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t getU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t getU64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────

W5100Device::W5100Device()
    : memory_(kW5100MemSize, 0)
{
    reset(true);
}

W5100Device::~W5100Device()
{
    for (size_t i = 0; i < kSocketCount; ++i) clearSocket(i);
}

// `Uthernet2.cpp:1362-1409`
void W5100Device::reset(bool powerCycle)
{
    modeRegister_ = 0;

    if (powerCycle) {
        // The indirect data address is deliberately NOT cleared by a soft
        // reset — Uthernet II manual p.10.
        dataAddress_ = 0;
        arpCache_.clear();
        if (resolver_) resolver_->clearCache();
    }

    for (size_t i = 0; i < kSocketCount; ++i) clearSocket(i);

    std::fill(memory_.begin(), memory_.end(), static_cast<uint8_t>(0));

    for (size_t i = 0; i < kSocketCount; ++i) {
        Socket& s = sockets_[i];
        s = Socket{};
        s.registerAddress = static_cast<uint16_t>(kW5100S0Base + (i << 8));
        resetRxTxBuffers(i);

        // Destination hardware address defaults to broadcast, TTL to 128.
        for (uint8_t r = kW5100SnDhar0; r <= kW5100SnDhar5; ++r)
            setMem(static_cast<uint16_t>(s.registerAddress + r), 0xFF);
        setMem(static_cast<uint16_t>(s.registerAddress + kW5100SnTtl), 0x80);
    }

    // Datasheet power-on values: retry time 0x07D0 (200 ms), retry count 8.
    setMem(kW5100Rtr0, 0x07);
    setMem(kW5100Rtr1, 0xD0);
    setMem(kW5100Rcr,  0x08);
    // 0x55 = 2 KB to each of the four sockets, for both directions.
    setRxSizes(0x55);
    setTxSizes(0x55);

    // PTIMER reads 0 when virtual DNS is available — that is the flag
    // guest software tests for (`Uthernet2.cpp:32-37`).
    setMem(kW5100Ptimer, virtualDns_ ? 0x00 : 0x28);
}

void W5100Device::setVirtualDnsEnabled(bool enabled)
{
    virtualDns_ = enabled;
    setMem(kW5100Ptimer, virtualDns_ ? 0x00 : 0x28);
}

uint16_t W5100Device::readNetworkWord(uint16_t a) const
{
    // W5100 16-bit registers are big-endian (high byte first).
    return static_cast<uint16_t>((static_cast<uint16_t>(mem(a)) << 8) |
                                 mem(static_cast<uint16_t>(a + 1)));
}

uint32_t W5100Device::readAddress(uint16_t a) const
{
    // Kept in network byte order, as the socket interface takes it.
    uint32_t v;
    const uint8_t bytes[4] = { mem(a),
                               mem(static_cast<uint16_t>(a + 1)),
                               mem(static_cast<uint16_t>(a + 2)),
                               mem(static_cast<uint16_t>(a + 3)) };
    std::memcpy(&v, bytes, 4);
    return v;
}

// ── Socket lifecycle ──────────────────────────────────────────────────

// `Uthernet2.cpp:212-234` — the RX header the chip prepends depends on
// the protocol the socket is running.
void W5100Device::setSocketStatus(size_t i, uint8_t status)
{
    Socket& s = sockets_[i];
    s.status = status;
    switch (status) {
    case kW5100SnSrEstablished: s.headerSize = 0;         break;  // TCP: raw stream
    case kW5100SnSrUdp:         s.headerSize = 4 + 2 + 2; break;  // IP + port + len
    case kW5100SnSrIpRaw:       s.headerSize = 4 + 2;     break;  // IP + len
    case kW5100SnSrMacRaw:      s.headerSize = 2;         break;  // len
    default:                    s.headerSize = 0;         break;
    }
}

void W5100Device::setNameResolver(std::unique_ptr<W5100Resolver> resolver)
{
    resolver_ = std::move(resolver);
}

void W5100Device::setSocketFactory(
    std::unique_ptr<W5100SocketFactory> factory)
{
    socketFactory_ = std::move(factory);
}

void W5100Device::clearSocket(size_t i)
{
    Socket& s = sockets_[i];
    s.host.reset();   // the handle's destructor closes the host socket
    s.pendingTx.clear();
    setSocketStatus(i, kW5100SnSrClosed);
}

// `Uthernet2.cpp:910-945`
void W5100Device::openSystemSocket(size_t i, W5100SocketKind kind,
                                   uint8_t status)
{
#if !POM2_HAS_SOCKETS
    // `type`/`protocol` until the socket-factory seam renamed them to `kind`.
    // Only the WASM leg compiles this branch, and the WASM leg could not get
    // this far — MainWindow.cpp died on a missing Version.h first — so the
    // rename left a reference to two parameters that no longer exist.
    (void)kind; (void)status;
    clearSocket(i);
    log().warn("W5100", "TCP/UDP sockets are unavailable in the WASM build");
#else
    clearSocket(i);

    // SIGPIPE suppression, non-blocking mode and the Windows UDP
    // connection-reset ioctl all live in the factory with the socket they
    // harden — W5100HostSockets.cpp records the failure each one prevents.
    //
    // The device does NOT construct one: that would be a device reaching into
    // the runtime for host sockets, which the configure-time layer guard
    // rejects. Whoever plugs the card injects it; with none, these socket
    // modes simply stay CLOSED, exactly as they do in the WASM build.
    if (!socketFactory_) return;
    auto host = socketFactory_->open(kind);
    if (!host) return;

    // Sn_PORT is the socket's LOCAL port, and the guest writes it BEFORE the
    // OPEN command (datasheet §5.2.1: "Sn_PORT should be set before OPEN").
    // Claiming it on the host socket is what the chip does, and without it
    // nothing unsolicited can reach the guest: a DHCP client opens UDP on 68
    // and waits for the server's broadcast, an NTP or TFTP exchange expects
    // its answer at the port it named, and an unbound host socket listens on
    // an ephemeral one instead — so those replies went to a port nobody was
    // reading and the guest simply timed out. Zero means "any", which is what
    // an unbound socket already is.
    const uint16_t localPort = readNetworkWord(
        static_cast<uint16_t>(sockets_[i].registerAddress + kW5100SnPort0));
    if (localPort != 0) host->bind(localPort);   // failure is logged, not fatal

    sockets_[i].host = std::move(host);
    setSocketStatus(i, status);
#endif
}

// `Uthernet2.cpp:947-1001`
void W5100Device::openSocket(size_t i)
{
    Socket& s = sockets_[i];
    clearSocket(i);

    const uint8_t mr = mem(static_cast<uint16_t>(s.registerAddress + kW5100SnMr));
    const uint8_t protocol = static_cast<uint8_t>(mr & kW5100SnMrProtoMask);
    const bool wantsDns = (protocol & kW5100SnVirtualDns) != 0;

    // A DNS-mode open on a build/config with virtual DNS off is a no-op:
    // the socket stays CLOSED rather than silently connecting somewhere.
    if (wantsDns && !virtualDns_) {
        log().warn("W5100", "socket opened in virtual-DNS mode but virtual DNS is disabled");
        return;
    }

    switch (protocol) {
    case kW5100SnMrIpRaw:
    case kW5100SnMrIpRaw | kW5100SnVirtualDns:
        setSocketStatus(i, kW5100SnSrIpRaw);
        break;
    case kW5100SnMrMacRaw:
        setSocketStatus(i, kW5100SnSrMacRaw);
        break;
    case kW5100SnMrTcp:
    case kW5100SnMrTcp | kW5100SnVirtualDns:
        openSystemSocket(i, W5100SocketKind::Tcp, kW5100SnSrInit);
        break;
    case kW5100SnMrUdp:
    case kW5100SnMrUdp | kW5100SnVirtualDns:
        openSystemSocket(i, W5100SocketKind::Udp, kW5100SnSrUdp);
        break;
    default:
        break;
    }

    if (wantsDns) resolveDns(i);

    resetRxTxBuffers(i);
}

void W5100Device::closeSocket(size_t i)
{
    clearSocket(i);
}

// `Uthernet2.cpp:1039-1075`
void W5100Device::connectSocket(size_t i)
{
#if !POM2_HAS_SOCKETS
    (void)i;
#else
    Socket& s = sockets_[i];
    if (!s.host) return;
    // CONNECT is only legal from SOCK_INIT (TCP, freshly opened) — the
    // real chip ignores it elsewhere. Accepting it on a UDP socket used
    // to connect() the datagram fd, flip the status to ESTABLISHED and
    // drop the 8-byte UDP RX header the driver still expects.
    if (s.status != kW5100SnSrInit) return;

    // Already in network byte order in the registers, and the interface
    // takes them that way — no sockaddr here, which is the point: this is a
    // device, and sockaddr belongs to the host socket layer.
    const uint32_t destinationAddress =
        readAddress(static_cast<uint16_t>(s.registerAddress + kW5100SnDipr0));
    // DIPR 0.0.0.0 is this card's "DNS resolution failed" marker (and no
    // valid destination either way): a real chip's ARP would time out and
    // close, whereas connect(INADDR_ANY) on Linux reaches 127.0.0.1 — the
    // guest would silently talk to a random host-local service.
    if (destinationAddress == 0) {
        log().warn("W5100", "CONNECT with destination 0.0.0.0 "
                            "(DNS failed or DIPR unset) — closing socket");
        clearSocket(i);
        return;
    }
    uint16_t port;
    const uint8_t portBytes[2] = {
        mem(static_cast<uint16_t>(s.registerAddress + kW5100SnDport0)),
        mem(static_cast<uint16_t>(s.registerAddress + kW5100SnDport1))
    };
    std::memcpy(&port, portBytes, 2);

    switch (s.host->connect(destinationAddress, port)) {
        case W5100ConnectResult::Connected:
            setSocketStatus(i, kW5100SnSrEstablished);
            return;
        case W5100ConnectResult::InProgress:
            setSocketStatus(i, kW5100SnSrSynSent);
            return;
        case W5100ConnectResult::Failed:
            // The reason is logged by the socket layer, which still has errno.
            log().warn("W5100", "connect() refused by the host");
            clearSocket(i);
            return;
    }
#endif
}

// LISTEN is in the W5100 command set but POM2 does not open a host
// listener for it: an inbound connection cannot reach the guest through
// either supported transport (libslirp is outbound-only without explicit
// port forwarding, and there is no host-side port to bind that the user
// asked for). Report the command as unhandled rather than pretending.
void W5100Device::listenSocket(size_t i)
{
    log().warn("W5100", "socket " + std::to_string(i) +
                        ": LISTEN is not supported (no inbound path)");
}

// `Uthernet2.cpp:1077-1102`
void W5100Device::setCommandRegister(size_t i, uint8_t value)
{
    switch (value) {
    case kW5100SnCrOpen:    openSocket(i);   break;
    case kW5100SnCrListen:  listenSocket(i); break;
    case kW5100SnCrConnect: connectSocket(i); break;
    case kW5100SnCrClose:
    case kW5100SnCrDiscon:  closeSocket(i);  break;
    case kW5100SnCrSend:    sendData(i);     break;
    case kW5100SnCrRecv:    updateRsr(i);    break;
    default: break;
    }
}

// ── Buffer geometry ───────────────────────────────────────────────────

// `Uthernet2.cpp:441-485` — RMSR/TMSR pack four 2-bit size codes, one per
// socket, each selecting 1/2/4/8 KB. Bases are assigned in order and
// clamped so a greedy allocation cannot run past the 8 KB region.
namespace {
// Every ring size must stay a power of two: all the pointer arithmetic
// below masks with `size - 1`. A greedy allocation clamped against the
// end of the region can come out non-pow2 (e.g. 1 KB + 8 KB requested in
// an 8 KB region leaves 7 KB) — round it down so masking stays exact.
inline uint16_t floorPow2(uint16_t v)
{
    while (v & (v - 1)) v = static_cast<uint16_t>(v & (v - 1));
    return v;
}
} // namespace

void W5100Device::setTxSizes(uint8_t value)
{
    setMem(kW5100Tmsr, value);
    uint16_t base = kW5100TxBase;
    const uint16_t end = kW5100RxBase;
    for (Socket& s : sockets_) {
        s.transmitBase = base;
        const uint8_t bits = static_cast<uint8_t>(value & 0x03);
        value = static_cast<uint8_t>(value >> 2);
        uint16_t size = static_cast<uint16_t>(1u << (10 + bits));
        if (static_cast<uint32_t>(base) + size > end)
            size = floorPow2(static_cast<uint16_t>(end - base));
        s.transmitSize = size;
        base = static_cast<uint16_t>(base + size);
    }
}

void W5100Device::setRxSizes(uint8_t value)
{
    setMem(kW5100Rmsr, value);
    uint16_t base = kW5100RxBase;
    const uint32_t end = kW5100MemSize;
    size_t i = 0;
    for (Socket& s : sockets_) {
        s.receiveBase = base;
        const uint8_t bits = static_cast<uint8_t>(value & 0x03);
        value = static_cast<uint8_t>(value >> 2);
        uint16_t size = static_cast<uint16_t>(1u << (10 + bits));
        if (static_cast<uint32_t>(base) + size > end)
            size = floorPow2(static_cast<uint16_t>(end - base));
        s.receiveSize = size;
        base = static_cast<uint16_t>(base + size);
        // The chip state may predate this geometry (guest rewrote RMSR
        // with data staged, or a snapshot is being restored).
        clampRingState(i++);
    }
}

void W5100Device::clampRingState(size_t i)
{
    Socket& s = sockets_[i];
    if (s.receiveSize == 0) {
        s.rxWrite = 0;
        s.rxSize  = 0;
        return;
    }
    s.rxWrite = static_cast<uint16_t>(s.rxWrite % s.receiveSize);
    if (s.rxSize > s.receiveSize) s.rxSize = s.receiveSize;
}

// `Uthernet2.cpp:487-502`
uint16_t W5100Device::txDataSize(size_t i) const
{
    const Socket& s = sockets_[i];
    const uint16_t size = s.transmitSize;
    if (size == 0) return 0;
    const uint16_t mask = static_cast<uint16_t>(size - 1);

    const int rd = readNetworkWord(
        static_cast<uint16_t>(s.registerAddress + kW5100SnTxRd0)) & mask;
    const int wr = readNetworkWord(
        static_cast<uint16_t>(s.registerAddress + kW5100SnTxWr0)) & mask;

    int present = wr - rd;
    if (present < 0) present += size;
    return static_cast<uint16_t>(present);
}

uint8_t W5100Device::txFreeSizeRegister(size_t i, unsigned shift) const
{
    const Socket& s = sockets_[i];
    const uint16_t size    = s.transmitSize;
    const uint16_t present = txDataSize(i);
    uint16_t free = static_cast<uint16_t>(size - present);

    // Sn_TX_FSR is the ONLY backpressure signal the chip gives the guest, and
    // the ring pointers alone do not tell the whole story: sendData advances
    // Sn_TX_RD to Sn_TX_WR BEFORE knowing whether the host socket accepted the
    // bytes, parking the remainder in pendingTx. So the ring read "empty" on
    // every poll while a host-side backlog piled up — a guest that polls FSR
    // exactly as the datasheet says, before every chunk, was told to keep
    // going, and lost up to a megabyte before flushPendingTx gave up and closed
    // the connection on it. Counting the backlog is what turns "the peer is
    // slower than the emulated CPU" from silent data loss into ordinary flow
    // control; poll() drains pendingTx, so the value recovers on its own.
    const std::size_t backlog = s.pendingTx.size();
    free = (backlog >= free) ? 0 : static_cast<uint16_t>(free - backlog);
    return indexByte(free, shift);
}

uint8_t W5100Device::rxDataSizeRegister(size_t i, unsigned shift) const
{
    return indexByte(sockets_[i].rxSize, shift);
}

// `Uthernet2.cpp:897-908`
void W5100Device::resetRxTxBuffers(size_t i)
{
    Socket& s = sockets_[i];
    s.rxWrite = 0;
    s.rxSize  = 0;
    for (uint8_t r : { kW5100SnTxRd0, kW5100SnTxRd1, kW5100SnTxWr0,
                       kW5100SnTxWr1, kW5100SnRxRd0, kW5100SnRxRd1 })
        setMem(static_cast<uint16_t>(s.registerAddress + r), 0x00);
}

// `Uthernet2.cpp:520-547` — RECV re-synchronises the staged size against
// the read pointer the guest just advanced.
void W5100Device::updateRsr(size_t i)
{
    Socket& s = sockets_[i];
    const uint16_t size = s.receiveSize;
    if (size == 0) { s.rxSize = 0; return; }
    const uint16_t mask = static_cast<uint16_t>(size - 1);

    const int rd = readNetworkWord(
        static_cast<uint16_t>(s.registerAddress + kW5100SnRxRd0)) & mask;
    const int wr = s.rxWrite & mask;

    int present = wr - rd;
    if (present < 0) present += size;
    s.rxSize = static_cast<uint16_t>(present);
}

// ── RX ring writers (`Uthernet2.cpp:119-181`) ─────────────────────────

void W5100Device::ringWrite8(size_t i, uint8_t v)
{
    Socket& s = sockets_[i];
    if (s.receiveSize == 0) return;
    setMem(static_cast<uint16_t>(s.receiveBase + s.rxWrite), v);
    s.rxWrite = static_cast<uint16_t>((s.rxWrite + 1) % s.receiveSize);
    ++s.rxSize;
}

void W5100Device::ringWrite16(size_t i, uint16_t v)
{
    // W5100 in-band lengths are big-endian, like its registers.
    ringWrite8(i, indexByte(v, 8));
    ringWrite8(i, indexByte(v, 0));
}

void W5100Device::ringWriteData(size_t i, const uint8_t* data, size_t len)
{
    for (size_t c = 0; c < len; ++c) ringWrite8(i, data[c]);
}

bool W5100Device::ringHasRoomFor(size_t i, size_t len) const
{
    const Socket& s = sockets_[i];
    // Strictly less than: a completely full ring is indistinguishable
    // from an empty one, since rxWrite would meet the read pointer.
    return s.rxSize + len + s.headerSize < s.receiveSize;
}

uint16_t W5100Device::ringFreeRoom(size_t i) const
{
    const Socket& s = sockets_[i];
    // Signed on purpose: clampRingState keeps rxSize <= receiveSize, but
    // an unsigned underflow here once meant "~64 K free" in a 1 KB ring.
    const int total = static_cast<int>(s.receiveSize) - static_cast<int>(s.rxSize);
    return total > s.headerSize ? static_cast<uint16_t>(total - s.headerSize) : 0;
}

// ── Receive ───────────────────────────────────────────────────────────

// `Uthernet2.cpp:747-770`
void W5100Device::receiveOnePacket(size_t i)
{
    switch (sockets_[i].status) {
    case kW5100SnSrMacRaw:
    case kW5100SnSrIpRaw:
        receiveOnePacketRaw();
        break;
    case kW5100SnSrEstablished:
    case kW5100SnSrUdp:
        receiveOnePacketFromSocket(i);
        break;
    default:
        break;
    }
}

// `Uthernet2.cpp:704-745`
void W5100Device::receiveOnePacketFromSocket(size_t i)
{
#if !POM2_HAS_SOCKETS
    (void)i;
#else
    Socket& s = sockets_[i];
    if (!s.isOpen()) return;

    // Only ESTABLISHED and UDP reach here (receiveOnePacket dispatches the
    // raw modes elsewhere and nothing else at all), and the two protocols
    // want opposite treatment on every branch below, so decide once.
    const bool isUdp = (s.status == kW5100SnSrUdp);

    const uint16_t freeRoom = ringFreeRoom(i);
    // Never fill the ring: rxWrite meeting the read pointer reads as empty
    // (see ringHasRoomFor). Below a couple of bytes the read is not worth
    // the syscall either.
    //
    // For UDP this gate does NOT keep a datagram safe, and an earlier comment
    // here claiming it did was wrong: the read below is sized at a whole
    // datagram, so any freeRoom above 32 consumes one, and a datagram too big
    // for the remaining room is then dropped by ringHasRoomFor — gone from the
    // host socket too.
    //
    // That is faithful, and deliberately kept. A real W5100 has nowhere to
    // put a frame that does not fit its ring either; it loses it on the wire,
    // and a guest streaming MTU-sized datagrams into the 2 KB power-on carve
    // loses them on real hardware for the same reason. Peeking first so the
    // host socket holds the datagram until the ring drains was tried
    // (2026-08-22) and rejected twice over: it emulates a buffer the chip does
    // not have, and leaving datagrams queued across socket teardown made the
    // receive-path tests flaky (20/20 -> 17/20). See TODO § [Network].
    if (freeRoom <= 32) return;

    uint8_t buffer[kMaxDatagram];
    // A DATAGRAM read must be sized against a whole datagram, NOT against
    // the ring's free room. recvfrom keeps no remainder: whatever does not
    // fit the supplied buffer is discarded, and the two families disagree
    // about how loudly. POSIX returns the truncated count with errno
    // untouched, so a short buffer stamps a wrong in-band length into the
    // ring and hands the guest a silently corrupt datagram; Winsock fails
    // the call with WSAEMSGSIZE, which the error arm below used to read as
    // "socket is dead". Reading whole datagrams removes both. A TCP read
    // is a stream and stays clamped to the ring, since bytes taken out of
    // the socket and then dropped would tear a hole in it.
    const size_t want = isUdp
        ? sizeof(buffer)
        : std::min<size_t>(static_cast<size_t>(freeRoom) - 1, sizeof(buffer));

    // The source address and port come back in the result. No sockaddr at
    // this layer: that belongs to the host socket implementation.
    const auto rx = s.host->receive(buffer, want);
    // Keep the signed-`got` shape the branches below are written against: they
    // distinguish a zero-length datagram from an orderly EOF from an error,
    // and that three-way split is the delicate part. The seam only changes
    // where the bytes come from.
    const long got = (rx.status == W5100IoStatus::Ok)
                         ? static_cast<long>(rx.bytes)
                         : -1L;
    if (got >= 0 && isUdp) {
        // got == 0 is a legitimate ZERO-LENGTH datagram — the empty
        // keepalive/probe several period stacks send — and never a peer
        // close, which is a concept datagram sockets do not have. The
        // real chip stages the 8-byte header with a length of 0 for it.
        // Treating it as a close destroyed the guest's socket.
        const size_t length = static_cast<size_t>(got);
        // Whole datagram or none: a message-oriented socket has no way to
        // hand the guest half of one, and the in-band length that follows
        // must describe what actually lands in the ring. The datagram is
        // dropped, which is exactly what a real network does when a
        // receiver cannot take it.
        if (!ringHasRoomFor(i, length)) return;

        // UDP prepends source IP + source port + length.
        const uint8_t* ip = reinterpret_cast<const uint8_t*>(&rx.sourceAddress);
        ringWriteData(i, ip, 4);
        const uint8_t* port = reinterpret_cast<const uint8_t*>(&rx.sourcePort);
        ringWriteData(i, port, 2);
        ringWrite16(i, static_cast<uint16_t>(length));
        ringWriteData(i, buffer, length);
        bytesReceived_ += static_cast<uint64_t>(length);
    } else if (got > 0) {
        // TCP is a raw stream with no header at all, and `want` above
        // already fitted it to the ring.
        ringWriteData(i, buffer, static_cast<size_t>(got));
        bytesReceived_ += static_cast<uint64_t>(got);
    } else if (got == 0) {
        // Orderly shutdown by the peer — half-close, not a dead socket.
        // The real chip parks in SOCK_CLOSE_WAIT: the guest may still
        // SEND its remaining data (the fd stays open for writing) and
        // ends the session with DISCON/CLOSE. Jumping straight to CLOSED
        // broke every driver that drains-then-disconnects on SR=$1C.
        setSocketStatus(i, kW5100SnSrCloseWait);
    } else {
        if (rx.status == W5100IoStatus::WouldBlock) return;   // nothing yet
        // On a datagram socket a failure can describe the packet rather
        // than the socket — Winsock reports an oversized datagram
        // (WSAEMSGSIZE) and somebody else's ICMP port-unreachable
        // (WSAECONNRESET, raised on the NEXT recvfrom of an unconnected
        // UDP socket) through the same channel as a genuine fault. Those
        // must cost one datagram, not the guest's socket. On TCP the very
        // same ECONNRESET IS the connection dying, so the tolerance is
        // strictly UDP-only. SocketCompat.h, trap 7.
        if (rx.status == W5100IoStatus::Discarded) return;
        clearSocket(i);
    }
#endif
}

// `Uthernet2.cpp:595-659` — one shared raw-frame path: whichever socket
// is in IPRAW gets first refusal (filtered by IP protocol number), and
// socket 0 in MACRAW is the fallback that takes everything else.
void W5100Device::receiveOnePacketRaw()
{
    if (!backend_) return;

    bool acceptAll = false;
    int  macRawSocket = -1;

    // MACRAW is socket 0 only on the W5100.
    if (sockets_[0].status == kW5100SnSrMacRaw) {
        macRawSocket = 0;
        const uint8_t mr = mem(static_cast<uint16_t>(
            sockets_[0].registerAddress + kW5100SnMr));
        // MF clear = do not filter, hand up every frame on the wire.
        acceptAll = (mr & kW5100SnMrMf) == 0;
    }

    uint8_t buffer[kMaxEthFrame];
    const std::array<uint8_t, 6> ourMac = macAddress();

    // Loop until a frame passes the MAC filter, or the backend runs dry.
    int len;
    bool isForUs = false;
    bool isBroadcast = false;
    while ((len = backend_->receive(buffer, sizeof(buffer))) > 0) {
        if (len < kEthMinimumSize) continue;

        isForUs      = std::memcmp(buffer, ourMac.data(), 6) == 0;
        static const uint8_t kBroadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        isBroadcast  = std::memcmp(buffer, kBroadcast, 6) == 0;
        if (isForUs || isBroadcast || acceptAll) break;
    }
    if (len <= 0) return;

    const uint8_t* payload = nullptr;
    size_t   lengthOfPayload = 0;
    uint32_t source = 0;
    uint8_t  packetProtocol = 0xFF;
    getIpPayload(buffer, len, payload, lengthOfPayload, source, packetProtocol);

    // IPRAW always filters by MAC, so a frame addressed to somebody else
    // (only here because MACRAW asked for everything) can never take it.
    int ipRawSocket = -1;
    if (isForUs || isBroadcast) {
        for (size_t i = 0; i < kSocketCount; ++i) {
            if (sockets_[i].status != kW5100SnSrIpRaw) continue;
            const uint8_t socketProtocol = mem(static_cast<uint16_t>(
                sockets_[i].registerAddress + kW5100SnProto));
            if (payload && packetProtocol == socketProtocol) {
                ipRawSocket = static_cast<int>(i);
                break;
            }
        }
    }

    if (ipRawSocket >= 0) {
        receiveOnePacketIpRaw(static_cast<size_t>(ipRawSocket), payload,
                              lengthOfPayload, source);
    } else if (macRawSocket >= 0) {
        receiveOnePacketMacRaw(static_cast<size_t>(macRawSocket), buffer, len);
    }
    // Otherwise the frame is dropped, exactly as the chip would.
}

// `Uthernet2.cpp:661-680`
void W5100Device::receiveOnePacketMacRaw(size_t i, const uint8_t* data, int size)
{
    if (!ringHasRoomFor(i, static_cast<size_t>(size))) return;   // drop
    // MACRAW's in-band length INCLUDES the two length bytes themselves.
    ringWrite16(i, static_cast<uint16_t>(size + 2));
    ringWriteData(i, data, static_cast<size_t>(size));
    bytesReceived_ += static_cast<uint64_t>(size);
}

// `Uthernet2.cpp:682-701`
void W5100Device::receiveOnePacketIpRaw(size_t i, const uint8_t* payload,
                                        size_t len, uint32_t source)
{
    if (!payload || !ringHasRoomFor(i, len)) return;   // drop
    // IPRAW header: source IP (network order, as-is) then payload length,
    // which does NOT include the header.
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(&source);
    ringWriteData(i, ip, 4);
    ringWrite16(i, static_cast<uint16_t>(len));
    ringWriteData(i, payload, len);
    bytesReceived_ += static_cast<uint64_t>(len);
}

// ── Transmit ──────────────────────────────────────────────────────────

// `Uthernet2.cpp:844-895` — SEND drains the TX ring between the read and
// write pointers, moves the read pointer to meet the write pointer, then
// dispatches by socket protocol.
void W5100Device::sendData(size_t i)
{
    Socket& s = sockets_[i];
    const uint16_t size = s.transmitSize;
    if (size == 0) return;
    const uint16_t mask = static_cast<uint16_t>(size - 1);

    const uint16_t rd = static_cast<uint16_t>(readNetworkWord(
        static_cast<uint16_t>(s.registerAddress + kW5100SnTxRd0)) & mask);
    const uint16_t wr = static_cast<uint16_t>(readNetworkWord(
        static_cast<uint16_t>(s.registerAddress + kW5100SnTxWr0)) & mask);

    const uint16_t base       = s.transmitBase;
    const uint16_t rdAddress  = static_cast<uint16_t>(base + rd);
    const uint16_t wrAddress  = static_cast<uint16_t>(base + wr);

    std::vector<uint8_t> data;
    if (rdAddress < wrAddress) {
        data.assign(memory_.begin() + rdAddress, memory_.begin() + wrAddress);
    } else if (rdAddress > wrAddress) {
        // Wrapped: tail of the ring, then head.
        const uint16_t end = static_cast<uint16_t>(base + size);
        data.assign(memory_.begin() + rdAddress, memory_.begin() + end);
        data.insert(data.end(), memory_.begin() + base, memory_.begin() + wrAddress);
    }
    // rd == wr means nothing staged; `data` stays empty and the
    // dispatch below is a harmless zero-length send.

    // The read pointer catches up with the write pointer.
    setMem(static_cast<uint16_t>(s.registerAddress + kW5100SnTxRd0), indexByte(wr, 8));
    setMem(static_cast<uint16_t>(s.registerAddress + kW5100SnTxRd1), indexByte(wr, 0));

    switch (s.status) {
    case kW5100SnSrMacRaw:      sendDataMacRaw(data);      break;
    case kW5100SnSrIpRaw:       sendDataIpRaw(i, data);    break;
    case kW5100SnSrEstablished:
    case kW5100SnSrCloseWait:   // half-closed: our direction still sends
    case kW5100SnSrUdp:         sendDataToSocket(i, data); break;
    default: break;
    }
}

// `Uthernet2.cpp:812-842`
void W5100Device::sendDataToSocket(size_t i, const std::vector<uint8_t>& data)
{
#if !POM2_HAS_SOCKETS
    (void)i; (void)data;
#else
    Socket& s = sockets_[i];
    if (!s.isOpen() || data.empty()) return;

    // Ignored for a connected TCP socket, so the same code serves both.
    const uint32_t destinationAddress =
        readAddress(static_cast<uint16_t>(s.registerAddress + kW5100SnDipr0));
    uint16_t port;
    const uint8_t portBytes[2] = {
        mem(static_cast<uint16_t>(s.registerAddress + kW5100SnDport0)),
        mem(static_cast<uint16_t>(s.registerAddress + kW5100SnDport1))
    };
    std::memcpy(&port, portBytes, 2);

    // TCP is a stream: whatever the non-blocking send does not take NOW
    // must be kept and retried (poll() flushes pendingTx), or the bytes
    // silently vanish mid-stream while the guest believes they were sent —
    // the TX ring was already freed when SEND completed. UDP keeps the
    // fire-and-forget path: dropping a datagram on EAGAIN is legal, and
    // queueing datagrams here would fuse their boundaries.
    const bool isTcp = (s.status != kW5100SnSrUdp);
    if (isTcp && !s.pendingTx.empty()) {
        s.pendingTx.insert(s.pendingTx.end(), data.begin(), data.end());
        flushPendingTx(i);
        return;
    }

    // sendToSocket, not ::sendto: MSG_NOSIGNAL where the platform has it,
    // paired with the SO_NOSIGPIPE armed at creation where it does not.
    const auto sent = s.host->send(data.data(), data.size(),
                                  destinationAddress, port,
                                  W5100SendMode::Addressed);
    if (sent.status == W5100IoStatus::Ok) {
        bytesSent_ += static_cast<uint64_t>(sent.bytes);
        // A short send is not an error on TCP: keep the tail so flushPendingTx
        // can finish it. Dropping it silently corrupted the stream.
        if (isTcp && sent.bytes < data.size())
            s.pendingTx.assign(data.begin() + static_cast<long>(sent.bytes),
                               data.end());
    } else if (sent.status == W5100IoStatus::WouldBlock) {
        if (isTcp) s.pendingTx = data;
    } else {
        clearSocket(i);
    }
#endif
}

void W5100Device::flushPendingTx(size_t i)
{
#if !POM2_HAS_SOCKETS
    (void)i;
#else
    Socket& s = sockets_[i];
    if (s.pendingTx.empty() || !s.host) return;

    // Stream mode: this is a connected socket and the tail was already
    // accepted from the guest, so it must NOT be re-addressed.
    const auto sent = s.host->send(s.pendingTx.data(), s.pendingTx.size(),
                                  0, 0, W5100SendMode::Stream);
    if (sent.status == W5100IoStatus::Ok && sent.bytes > 0) {
        bytesSent_ += static_cast<uint64_t>(sent.bytes);
        s.pendingTx.erase(
            s.pendingTx.begin(),
            s.pendingTx.begin() + static_cast<long>(sent.bytes));
    } else if (sent.status == W5100IoStatus::Failed) {
        clearSocket(i);
        return;
    }
    // A peer that has not taken a megabyte is not coming back; an honest
    // dead connection beats an unbounded host-side queue.
    constexpr size_t kMaxPendingTx = 1u << 20;
    if (s.pendingTx.size() > kMaxPendingTx) {
        log().warn("W5100", "socket " + std::to_string(i) +
                            ": peer stalled with >1 MiB unsent — closing");
        clearSocket(i);
    }
#endif
}

void W5100Device::sendDataMacRaw(const std::vector<uint8_t>& data)
{
    // MACRAW hands the backend a complete Ethernet frame verbatim.
    if (!backend_ || data.empty()) return;
    backend_->transmit(data.data(), static_cast<int>(data.size()));
    bytesSent_ += data.size();
}

// `Uthernet2.cpp:772-793`
void W5100Device::sendDataIpRaw(size_t i, const std::vector<uint8_t>& payload)
{
    if (!backend_ || payload.empty()) return;

    const Socket& s = sockets_[i];
    const uint8_t ttl = mem(static_cast<uint16_t>(s.registerAddress + kW5100SnTtl));
    const uint8_t tos = mem(static_cast<uint16_t>(s.registerAddress + kW5100SnTos));
    const uint8_t protocol = mem(static_cast<uint16_t>(s.registerAddress + kW5100SnProto));
    const uint32_t source = readAddress(kW5100Sipr0);
    const uint32_t dest = readAddress(
        static_cast<uint16_t>(s.registerAddress + kW5100SnDipr0));

    MacAddress sourceMac{};
    const std::array<uint8_t, 6> ourMac = macAddress();
    std::memcpy(sourceMac.b, ourMac.data(), 6);

    MacAddress destinationMac{};
    macForAddress(dest, destinationMac);

    const std::vector<uint8_t> frame = createEth2Frame(
        payload, sourceMac, destinationMac, ttl, tos, protocol, source, dest);

    backend_->transmit(frame.data(), static_cast<int>(frame.size()));
    bytesSent_ += payload.size();
}

// ── ARP + DNS ─────────────────────────────────────────────────────────

// `Uthernet2.cpp:1487-1525`
void W5100Device::macForAddress(uint32_t ipv4, MacAddress& out)
{
    const auto it = arpCache_.find(ipv4);
    if (it != arpCache_.end()) { out = it->second; return; }

    MacAddress mac{};
    const uint32_t source = readAddress(kW5100Sipr0);

    if (ipv4 == source) {
        const std::array<uint8_t, 6> ourMac = macAddress();
        std::memcpy(mac.b, ourMac.data(), 6);
    } else {
        // Broadcast is both the correct answer for 255.255.255.255 and
        // the right fallback while a real card's ARP is outstanding.
        std::memset(mac.b, 0xFF, sizeof(mac.b));
        if (ipv4 != 0xFFFFFFFFu && backend_) {
            const uint32_t subnet = readAddress(kW5100Subr0);
            if ((ipv4 & subnet) == (source & subnet)) {
                backend_->resolveMac(ipv4, mac);          // same subnet
            } else {
                const uint32_t gateway = readAddress(kW5100Gar0);
                backend_->resolveMac(gateway, mac);       // via the router
            }
        }
    }

    // Capped like dnsCache_ next door, and for the same reason: a guest doing
    // an IPRAW subnet or ICMP sweep — an ordinary Apple II network-utility
    // workload — inserts one node per destination address it ever touches, and
    // nothing here evicts. Clearing wholesale rather than evicting one entry
    // also lets a peer that only became resolvable later pick up a real
    // destination MAC, instead of keeping the broadcast fallback that a single
    // early miss cached for the rest of the session.
    if (arpCache_.size() >= 512) arpCache_.clear();
    arpCache_[ipv4] = mac;
    out = mac;
}

// `Uthernet2.cpp:1012-1037`, but async — see kDnsWaitMs in the header for
// why a plain blocking getaddrinfo() is not acceptable here.
void W5100Device::resolveDns(size_t i)
{
    Socket& s = sockets_[i];
    const uint16_t diprAddress =
        static_cast<uint16_t>(s.registerAddress + kW5100SnDipr0);

    // 0.0.0.0 is how this extension signals "resolution failed".
    for (int b = 0; b < 4; ++b)
        setMem(static_cast<uint16_t>(diprAddress + b), 0x00);

    const uint8_t length = mem(static_cast<uint16_t>(
        s.registerAddress + kW5100SnDnsNameLen));
    if (length == 0 || length > kW5100SnDnsNameCpty) return;

    std::string name;
    name.reserve(length);
    for (uint8_t k = 0; k < length; ++k) {
        name.push_back(static_cast<char>(mem(static_cast<uint16_t>(
            s.registerAddress + kW5100SnDnsNameBegin + k))));
    }

    // The lookup, its cache, the bounded wait and the in-flight cap all live
    // in W5100NameResolver now. What stays here is the part that IS the chip:
    // reading the name out of the socket's DNS registers above, and writing
    // the answer into DIPR below.
    // Injected, not constructed: the resolver owns worker threads, and a
    // device may not reach into the runtime for those. With none, virtual DNS
    // is simply unavailable — the same answer the WASM build gives.
    if (!resolver_) {
        log().warn("W5100", "virtual DNS is unavailable (no resolver)");
        return;
    }
    const auto answer = resolver_->resolve(name, kDnsWaitMs);
    if (answer.status != W5100Resolver::Status::Resolved) {
        if (answer.status == W5100Resolver::Status::Failed)
            log().warn("W5100", "could not resolve '" + name + "'");
        // Pending and Refused already logged their own reason.
        return;
    }
    const uint32_t resolved = answer.address;


    const uint8_t* ip = reinterpret_cast<const uint8_t*>(&resolved);
    for (int b = 0; b < 4; ++b)
        setMem(static_cast<uint16_t>(diprAddress + b), ip[b]);
}

// ── Register decode ───────────────────────────────────────────────────

// `Uthernet2.cpp:1104-1152`
uint8_t W5100Device::readSocketRegister(uint16_t address)
{
    const size_t i = static_cast<size_t>((address >> 8) - 0x04);
    if (i >= kSocketCount) return 0;
    const uint8_t loc = static_cast<uint8_t>(address & 0xFF);

    switch (loc) {
    case kW5100SnSr:
        return sockets_[i].status;
    case kW5100SnTxFsr0: return txFreeSizeRegister(i, 8);
    case kW5100SnTxFsr1: return txFreeSizeRegister(i, 0);
    // Reading the staged-size register is what pulls data in — the chip
    // is polled, it has no interrupt line on the Uthernet II.
    case kW5100SnRxRsr0: receiveOnePacket(i); return rxDataSizeRegister(i, 8);
    case kW5100SnRxRsr1: receiveOnePacket(i); return rxDataSizeRegister(i, 0);
    default:
        return mem(address);
    }
}

// `Uthernet2.cpp:1233-1291`
void W5100Device::writeSocketRegister(uint16_t address, uint8_t value)
{
    const size_t i = static_cast<size_t>((address >> 8) - 0x04);
    if (i >= kSocketCount) return;
    const uint8_t loc = static_cast<uint8_t>(address & 0xFF);

    switch (loc) {
    case kW5100SnMr:
        setMem(address, value);
        break;
    case kW5100SnCr:
        setCommandRegister(i, value);
        break;
    case kW5100SnPort0:  case kW5100SnPort1:
    case kW5100SnDport0: case kW5100SnDport1:
    case kW5100SnDipr0:  case kW5100SnDipr0 + 1:
    case kW5100SnDipr0 + 2: case kW5100SnDipr3:
    case kW5100SnProto:  case kW5100SnTos: case kW5100SnTtl:
    case kW5100SnTxWr0:  case kW5100SnTxWr1:
    case kW5100SnRxRd0:  case kW5100SnRxRd1:
        setMem(address, value);
        break;
    default:
        // The virtual-DNS hostname area is the only other writable range.
        if (virtualDns_ && loc >= kW5100SnDnsNameLen) setMem(address, value);
        break;
    }
}

// `Uthernet2.cpp:1293-1332`
void W5100Device::setModeRegister(uint8_t value)
{
    if (value & kW5100MrRst) {
        reset(false);
    } else {
        modeRegister_ = value;
    }
}

void W5100Device::writeCommonRegister(uint16_t address, uint8_t value)
{
    if (address == kW5100Mr) {
        setModeRegister(value);
    } else if ((address >= kW5100Gar0  && address <= kW5100Gar3)  ||
               (address >= kW5100Subr0 && address <= kW5100Subr3) ||
               (address >= kW5100Shar0 && address <= kW5100Shar5) ||
               (address >= kW5100Sipr0 && address <= kW5100Sipr3)) {
        setMem(address, value);
        // Our own IP or MAC changing invalidates every cached ARP answer.
        arpCache_.clear();
    } else if (address == kW5100Rmsr) {
        setRxSizes(value);
    } else if (address == kW5100Tmsr) {
        setTxSizes(value);
    }
    // Everything else in the common range is read-only or reserved.
}

// `Uthernet2.cpp:1154-1183`
uint8_t W5100Device::readValueAt(uint16_t address)
{
    // Fold the >= $8000 mirror FIRST, exactly as writeValueAt does
    // (Uthernet II manual p.13). Masking only inside mem() further down
    // wrapped plain memory but let every range test below see the raw
    // address, so $8403 read back memory_[$0403] — always 0 — instead of
    // S0's status register, and $8426 never pulled a packet in.
    address &= kW5100MemMax;
    if (address == kW5100Mr) return modeRegister_;
    if (address >= kW5100S0Base && address <= kW5100S3Max)
        return readSocketRegister(address);
    // Common registers, TX/RX buffers and everything else read straight
    // out of the array.
    return mem(address);
}

// `Uthernet2.cpp:1334-1354`
void W5100Device::writeValueAt(uint16_t address, uint8_t value)
{
    // Same >= $8000 mirror as readValueAt (Uthernet II manual p.13):
    // without the mask, writes up there fall past every range test below
    // and are silently dropped.
    address &= kW5100MemMax;
    if (address <= kW5100Uport1) {
        writeCommonRegister(address, value);
    } else if (address >= kW5100S0Base && address <= kW5100S3Max) {
        writeSocketRegister(address, value);
    } else if (address >= kW5100TxBase && address <= kW5100MemMax) {
        setMem(address, value);
    }
    // 0x0030-0x03FF and 0x0800-0x3FFF are reserved: writes are dropped.
}

uint8_t W5100Device::peekValueAt(uint16_t address) const
{
    address &= kW5100MemMax;        // same mirror as readValueAt
    if (address == kW5100Mr) return modeRegister_;
    if (address >= kW5100S0Base && address <= kW5100S3Max) {
        const size_t i = static_cast<size_t>((address >> 8) - 0x04);
        const uint8_t loc = static_cast<uint8_t>(address & 0xFF);
        if (i < kSocketCount) {
            // Same values readValueAt would give, minus the packet pull.
            switch (loc) {
            case kW5100SnSr:     return sockets_[i].status;
            case kW5100SnTxFsr0: return txFreeSizeRegister(i, 8);
            case kW5100SnTxFsr1: return txFreeSizeRegister(i, 0);
            case kW5100SnRxRsr0: return rxDataSizeRegister(i, 8);
            case kW5100SnRxRsr1: return rxDataSizeRegister(i, 0);
            default: break;
            }
        }
    }
    return mem(address);
}

// `Uthernet2.cpp:1185-1207`
void W5100Device::autoIncrement()
{
    if (!(modeRegister_ & kW5100MrAi)) return;
    ++dataAddress_;
    // The indirect window wraps within each 8 KB buffer rather than
    // running into the next region (Uthernet II manual p.12 bottom).
    if (dataAddress_ == kW5100RxBase || dataAddress_ == kW5100MemSize)
        dataAddress_ = static_cast<uint16_t>(dataAddress_ - 0x2000);
}

uint8_t W5100Device::readData()
{
    const uint8_t value = readValueAt(dataAddress_);
    autoIncrement();
    return value;
}

void W5100Device::writeData(uint8_t value)
{
    writeValueAt(dataAddress_, value);
    autoIncrement();
}

// ── Per-tick servicing ────────────────────────────────────────────────

// `Uthernet2.cpp:269-306` (Socket::process) + `:1527-1534`
void W5100Device::poll()
{
    // Drain any DNS answer that arrived after its bounded wait expired.
    drainPendingDns();

    if (backend_) backend_->poll();

#if POM2_HAS_SOCKETS
    for (size_t i = 0; i < kSocketCount; ++i) {
        // Retry TCP bytes the host socket refused earlier (short write /
        // EAGAIN) — see sendDataToSocket.
        if (!sockets_[i].pendingTx.empty()) flushPendingTx(i);

        Socket& s = sockets_[i];
        if (!s.host || s.status != kW5100SnSrSynSent) continue;

        // Zero timeout: this runs on the emulation thread, so it polls and
        // returns. A refused connection has to reach us too, not just a
        // successful one — on Winsock that arrives through select()'s
        // exception set, which is why waitSocket() is not WSAPoll. See
        // SocketCompat.h.
        const auto poll = s.host->pollConnect();
        if (poll == W5100ConnectResult::InProgress)
            continue;

        if (poll == W5100ConnectResult::Connected) {
            setSocketStatus(i, kW5100SnSrEstablished);
        } else {
            // Connection refused / unreachable — back to CLOSED, which is
            // what the guest polls SN_SR for.
            clearSocket(i);
        }
    }
#endif
}

void W5100Device::drainPendingDns()
{
    // CPU thread only, which is what keeps the resolver's cache lock-free.
    if (resolver_) resolver_->poll();
}

// ── Introspection ─────────────────────────────────────────────────────

W5100Device::SocketInfo W5100Device::socketInfo(size_t i) const
{
    SocketInfo info;
    if (i >= kSocketCount) return info;
    const Socket& s = sockets_[i];
    info.mode       = mem(static_cast<uint16_t>(s.registerAddress + kW5100SnMr));
    info.status     = s.status;
    info.localPort  = readNetworkWord(static_cast<uint16_t>(s.registerAddress + kW5100SnPort0));
    info.remotePort = readNetworkWord(static_cast<uint16_t>(s.registerAddress + kW5100SnDport0));
    info.remoteIp   = readAddress(static_cast<uint16_t>(s.registerAddress + kW5100SnDipr0));
    info.rxPending  = s.rxSize;
    info.txPending  = txDataSize(i);
    info.rxCapacity = s.receiveSize;
    info.txCapacity = s.transmitSize;
    info.hasHostSocket = s.host != nullptr;
    return info;
}

std::array<uint8_t, 6> W5100Device::macAddress() const
{
    std::array<uint8_t, 6> mac{};
    for (int i = 0; i < 6; ++i)
        mac[static_cast<size_t>(i)] = mem(static_cast<uint16_t>(kW5100Shar0 + i));
    return mac;
}

uint32_t W5100Device::localIp() const { return readAddress(kW5100Sipr0); }

// ── Snapshot / rewind ─────────────────────────────────────────────────

void W5100Device::appendSnapshotState(std::vector<uint8_t>& out) const
{
    putU32(out, kSnapMagic);
    putU16(out, kSnapVersion);

    out.push_back(modeRegister_);
    putU16(out, dataAddress_);
    out.push_back(virtualDns_ ? 1 : 0);

    // Only the regions the datasheet defines — the reserved holes carry
    // nothing and would just bloat the rewind delta (W5100 memory map
    // §2, mirrored by `Uthernet2.cpp:1563-1587`).
    out.insert(out.end(), memory_.begin(), memory_.begin() + (kW5100Uport1 + 1));
    out.insert(out.end(), memory_.begin() + kW5100S0Base,
                          memory_.begin() + (kW5100S3Max + 1));
    out.insert(out.end(), memory_.begin() + kW5100TxBase,
                          memory_.begin() + kW5100RxBase);
    out.insert(out.end(), memory_.begin() + kW5100RxBase,
                          memory_.begin() + kW5100MemSize);

    for (const Socket& s : sockets_) {
        putU16(out, s.rxWrite);
        putU16(out, s.rxSize);
        out.push_back(s.status);
    }

    putU64(out, bytesSent_);
    putU64(out, bytesReceived_);
}

void W5100Device::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    constexpr std::size_t kRegionCommon = kW5100Uport1 + 1;
    constexpr std::size_t kRegionSocket = (kW5100S3Max + 1) - kW5100S0Base;
    constexpr std::size_t kRegionTx     = kW5100RxBase - kW5100TxBase;
    constexpr std::size_t kRegionRx     = kW5100MemSize - kW5100RxBase;
    constexpr std::size_t kExpected =
        4 + 2 + 1 + 2 + 1 +
        kRegionCommon + kRegionSocket + kRegionTx + kRegionRx +
        kSocketCount * 5 + 16;

    if (len < kExpected) return;
    if (getU32(data) != kSnapMagic) return;
    if (getU16(data + 4) != kSnapVersion) return;

    // Live host sockets never survive a rewind — see the per-socket note
    // below — so drop them all before touching the register file.
    for (size_t i = 0; i < kSocketCount; ++i) clearSocket(i);

    std::size_t p = 6;
    modeRegister_ = data[p++];
    dataAddress_  = getU16(data + p); p += 2;
    virtualDns_   = data[p++] != 0;

    std::memcpy(memory_.data(), data + p, kRegionCommon);            p += kRegionCommon;
    std::memcpy(memory_.data() + kW5100S0Base, data + p, kRegionSocket); p += kRegionSocket;
    std::memcpy(memory_.data() + kW5100TxBase, data + p, kRegionTx);     p += kRegionTx;
    std::memcpy(memory_.data() + kW5100RxBase, data + p, kRegionRx);     p += kRegionRx;

    // Buffer geometry is derived, not stored — rebuild it from the
    // register values we just restored.
    setRxSizes(memory_[kW5100Rmsr]);
    setTxSizes(memory_[kW5100Tmsr]);

    for (size_t i = 0; i < kSocketCount; ++i) {
        Socket& s = sockets_[i];
        s.rxWrite = getU16(data + p); p += 2;
        s.rxSize  = getU16(data + p); p += 2;
        // Never trust a blob's ring cursors against the rebuilt geometry —
        // a stale/crafted snapshot could otherwise underflow the ring math.
        clampRingState(i);
        uint8_t status = data[p++];

        // A TCP connection or a UDP binding cannot be resurrected: the
        // peer moved on while the ring was rewound, and the fd is gone.
        // The raw modes carry no host state, so those do come back.
        if (status != kW5100SnSrMacRaw && status != kW5100SnSrIpRaw)
            status = kW5100SnSrClosed;
        setSocketStatus(i, status);
    }

    bytesSent_     = getU64(data + p); p += 8;
    bytesReceived_ = getU64(data + p);

    arpCache_.clear();
}

} // namespace pom2
