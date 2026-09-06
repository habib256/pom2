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

// W5100Device — WIZnet W5100 "hardwired TCP/IP" Ethernet controller, the
// chip on the a2RetroSystems **Uthernet II**.
//
// MAME has no W5100 device (its Apple II Ethernet support stops at the
// Uthernet I's CS8900A), so the reference here is AppleWin's
// `source/Uthernet2.cpp` + `source/W5100.h` (GPL-2.0+, Andrea Odetti),
// cross-checked against the WIZnet W5100 datasheet v1.2.8 and the
// Uthernet II manual (2018-11-17). Line citations below are against
// AppleWin's `Uthernet2.cpp`.
//
// Why this is NOT a packet-level model
// ------------------------------------
// The W5100 is not a NIC — it is a TCP/IP *offload engine*. The guest
// does not build IP headers or run a retransmit timer; it writes a
// destination address and a port into registers, issues CONNECT, and then
// pushes payload bytes at a ring buffer. All the protocol work happens
// inside the chip. That maps one-for-one onto host BSD sockets, which is
// exactly what this class does and what AppleWin does: each of the four
// W5100 sockets in TCP or UDP mode owns a real non-blocking host socket.
//
// The consequence is the headline feature: **Uthernet II works with no
// Ethernet backend at all.** A period IRC, telnet or FTP client talks
// TCP, so it runs over plain host sockets on any machine, no privileges,
// no libslirp. Only the two raw modes need a `NetworkBackend`:
//
//   MACRAW — socket 0 only, hands the guest whole Ethernet frames.
//   IPRAW  — the guest supplies an IP payload and a protocol number and
//            the chip frames it (this is how W5100 software does ICMP).
//
// Memory map (`W5100.h`, datasheet §3)
// ------------------------------------
//   $0000-$002F  common registers (mode, gateway, subnet, MAC, our IP,
//                retry timing, RX/TX memory-size allocation)
//   $0400-$07FF  four 256-byte socket register banks (S0..S3)
//   $4000-$5FFF  8 KB TX buffer, carved between sockets by TMSR
//   $6000-$7FFF  8 KB RX buffer, carved between sockets by RMSR
//
// The Uthernet II reaches all 32 KB through a 4-register indirect window
// (datasheet "indirect bus mode"), which UthernetIICard maps onto $C0n4-7
// and this class implements as modeRegister/dataAddress/readData/writeData.
//
// Virtual DNS (`Uthernet2.cpp:32-37`)
// ----------------------------------
// An AppleWin extension the real card does not have: setting bit 3 of a
// socket's protocol nibble means "the destination is a hostname, not an
// IP". The length-prefixed name lives at socket-register offset $2A-$FF
// and OPEN resolves it into DIPR. Software detects the extension by
// reading PTIMER as 0. POM2 keeps it — it is what lets a guest reach
// `irc.libera.chat` without carrying its own resolver — but resolves off
// the CPU thread; see `resolveDns`.
//
// Threading: every entry point runs on the CPU thread under
// EmulationController's stateMutex. All host sockets are non-blocking and
// nothing here waits on the network. The one exception is the bounded DNS
// wait documented on `kDnsWaitMs`.

#ifndef POM2_W5100_DEVICE_H
#define POM2_W5100_DEVICE_H

#include "NetworkBackend.h"
#include "NetworkValues.h"
#include "W5100Socket.h"   // socket_t / kInvalidSocket for Socket::fd

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pom2 {

class W5100Resolver;

// ── W5100 memory map (AppleWin `W5100.h`) ─────────────────────────────
inline constexpr uint16_t kW5100Mr       = 0x0000;
inline constexpr uint16_t kW5100Gar0     = 0x0001;
inline constexpr uint16_t kW5100Gar3     = 0x0004;
inline constexpr uint16_t kW5100Subr0    = 0x0005;
inline constexpr uint16_t kW5100Subr3    = 0x0008;
inline constexpr uint16_t kW5100Shar0    = 0x0009;
inline constexpr uint16_t kW5100Shar5    = 0x000E;
inline constexpr uint16_t kW5100Sipr0    = 0x000F;
inline constexpr uint16_t kW5100Sipr3    = 0x0012;
inline constexpr uint16_t kW5100Ir       = 0x0015;
inline constexpr uint16_t kW5100Imr      = 0x0016;
inline constexpr uint16_t kW5100Rtr0     = 0x0017;
inline constexpr uint16_t kW5100Rtr1     = 0x0018;
inline constexpr uint16_t kW5100Rcr      = 0x0019;
inline constexpr uint16_t kW5100Rmsr     = 0x001A;
inline constexpr uint16_t kW5100Tmsr     = 0x001B;
inline constexpr uint16_t kW5100Ptimer   = 0x0028;
inline constexpr uint16_t kW5100Pmagic   = 0x0029;
inline constexpr uint16_t kW5100Uport1   = 0x002F;
inline constexpr uint16_t kW5100S0Base   = 0x0400;
inline constexpr uint16_t kW5100S3Max    = 0x07FF;
inline constexpr uint16_t kW5100TxBase   = 0x4000;
inline constexpr uint16_t kW5100RxBase   = 0x6000;
inline constexpr uint16_t kW5100MemMax   = 0x7FFF;
inline constexpr uint32_t kW5100MemSize  = 0x8000;

// Mode register bits.
inline constexpr uint8_t kW5100MrAi  = 0x02;   // address auto-increment
inline constexpr uint8_t kW5100MrRst = 0x80;   // soft reset

// Socket mode register — protocol nibble.
inline constexpr uint8_t kW5100SnMrProtoMask = 0x0F;
inline constexpr uint8_t kW5100SnMrMf        = 0x40;  // MACRAW: filter by MAC
inline constexpr uint8_t kW5100SnMrClosed    = 0x00;
inline constexpr uint8_t kW5100SnMrTcp       = 0x01;
inline constexpr uint8_t kW5100SnMrUdp       = 0x02;
inline constexpr uint8_t kW5100SnMrIpRaw     = 0x03;
inline constexpr uint8_t kW5100SnMrMacRaw    = 0x04;
/// Virtual-DNS flag — POM2/AppleWin extension, not on real silicon.
inline constexpr uint8_t kW5100SnVirtualDns  = 0x08;

// Socket command register.
inline constexpr uint8_t kW5100SnCrOpen    = 0x01;
inline constexpr uint8_t kW5100SnCrListen  = 0x02;
inline constexpr uint8_t kW5100SnCrConnect = 0x04;
inline constexpr uint8_t kW5100SnCrDiscon  = 0x08;
inline constexpr uint8_t kW5100SnCrClose   = 0x10;
inline constexpr uint8_t kW5100SnCrSend    = 0x20;
/// SEND_MAC: UDP only — send the staged data to Sn_DIPR/Sn_DPORT using the
/// MAC already in Sn_DHAR instead of running ARP first (datasheet §5.1
/// "Sn_CR", value $21). POM2 has no ARP on the socket path at all, so it is
/// the same transmission SEND performs.
inline constexpr uint8_t kW5100SnCrSendMac  = 0x21;
/// SEND_KEEP: send a 1-byte TCP keep-alive probe with no payload
/// (datasheet §5.1, value $22). Host TCP owns keep-alives, so the chip-level
/// answer is "accepted, nothing staged" — but it must still be ACCEPTED, or
/// a driver polling Sn_IR for SEND_OK after one spins forever.
inline constexpr uint8_t kW5100SnCrSendKeep = 0x22;
inline constexpr uint8_t kW5100SnCrRecv    = 0x40;

// Socket interrupt register bits (datasheet §5.2.3 "Sn_IR"). Write-1-to-clear.
inline constexpr uint8_t kW5100SnIrCon     = 0x01;  // connection established
inline constexpr uint8_t kW5100SnIrDiscon  = 0x02;  // FIN / FIN-ACK received
inline constexpr uint8_t kW5100SnIrRecv    = 0x04;  // data staged in the RX ring
inline constexpr uint8_t kW5100SnIrTimeout = 0x08;  // ARP / TCP timeout
inline constexpr uint8_t kW5100SnIrSendOk  = 0x10;  // SEND completed

// Socket register offsets within a 256-byte bank.
inline constexpr uint8_t kW5100SnMr      = 0x00;
inline constexpr uint8_t kW5100SnCr      = 0x01;
inline constexpr uint8_t kW5100SnIr      = 0x02;
inline constexpr uint8_t kW5100SnSr      = 0x03;
inline constexpr uint8_t kW5100SnPort0   = 0x04;
inline constexpr uint8_t kW5100SnPort1   = 0x05;
inline constexpr uint8_t kW5100SnDhar0   = 0x06;
inline constexpr uint8_t kW5100SnDhar5   = 0x0B;
inline constexpr uint8_t kW5100SnDipr0   = 0x0C;
inline constexpr uint8_t kW5100SnDipr3   = 0x0F;
inline constexpr uint8_t kW5100SnDport0  = 0x10;
inline constexpr uint8_t kW5100SnDport1  = 0x11;
inline constexpr uint8_t kW5100SnProto   = 0x14;
inline constexpr uint8_t kW5100SnTos     = 0x15;
inline constexpr uint8_t kW5100SnTtl     = 0x16;
inline constexpr uint8_t kW5100SnTxFsr0  = 0x20;
inline constexpr uint8_t kW5100SnTxFsr1  = 0x21;
inline constexpr uint8_t kW5100SnTxRd0   = 0x22;
inline constexpr uint8_t kW5100SnTxRd1   = 0x23;
inline constexpr uint8_t kW5100SnTxWr0   = 0x24;
inline constexpr uint8_t kW5100SnTxWr1   = 0x25;
inline constexpr uint8_t kW5100SnRxRsr0  = 0x26;
inline constexpr uint8_t kW5100SnRxRsr1  = 0x27;
inline constexpr uint8_t kW5100SnRxRd0   = 0x28;
inline constexpr uint8_t kW5100SnRxRd1   = 0x29;
/// Virtual-DNS hostname area (POM2/AppleWin extension).
inline constexpr uint8_t kW5100SnDnsNameLen   = 0x2A;
inline constexpr uint8_t kW5100SnDnsNameBegin = 0x2B;
inline constexpr uint8_t kW5100SnDnsNameEnd   = 0xFF;
inline constexpr uint8_t kW5100SnDnsNameCpty  =
    static_cast<uint8_t>(kW5100SnDnsNameEnd - kW5100SnDnsNameBegin);

// Socket status register values.
inline constexpr uint8_t kW5100SnSrClosed      = 0x00;
inline constexpr uint8_t kW5100SnSrInit        = 0x13;
/// The rest of the datasheet's TCP status ladder (§5.2.2 "Sn_SR"). POM2 does
/// not reach LISTEN / the four closing states — a host socket does that
/// bookkeeping — but the values are named so no code invents its own and so a
/// reader can tell "not modelled" from "not known".
inline constexpr uint8_t kW5100SnSrListen      = 0x14;
inline constexpr uint8_t kW5100SnSrSynSent     = 0x15;
inline constexpr uint8_t kW5100SnSrSynRecv     = 0x16;
inline constexpr uint8_t kW5100SnSrEstablished = 0x17;
inline constexpr uint8_t kW5100SnSrFinWait     = 0x18;
inline constexpr uint8_t kW5100SnSrClosing     = 0x1A;
inline constexpr uint8_t kW5100SnSrTimeWait    = 0x1B;
inline constexpr uint8_t kW5100SnSrLastAck     = 0x1D;
/// Peer sent FIN: the guest may still SEND its remaining data and must
/// answer with DISCON/CLOSE (W5100 datasheet §5.2.1 "SOCK_CLOSE_WAIT").
inline constexpr uint8_t kW5100SnSrCloseWait   = 0x1C;
inline constexpr uint8_t kW5100SnSrUdp         = 0x22;
inline constexpr uint8_t kW5100SnSrIpRaw       = 0x32;
inline constexpr uint8_t kW5100SnSrMacRaw      = 0x42;

class W5100Device
{
public:
    static constexpr size_t kSocketCount = 4;

    /// How long `openSocket` will wait for an in-flight virtual-DNS
    /// lookup before giving up and leaving DIPR at 0.0.0.0. The lookup
    /// itself runs on a detached thread and its answer is cached, so a
    /// guest that retries after the (expected) failed connect gets the
    /// address instantly. Bounded because this runs on the CPU thread:
    /// a plain blocking getaddrinfo() could stall emulation for seconds.
    static constexpr int kDnsWaitMs = 120;

    /// Swap the socket factory. Nothing injected → the production host
    /// factory, installed lazily on first use so existing construction sites
    /// are unchanged. Tests inject before any socket is opened.
    void setSocketFactory(std::unique_ptr<W5100SocketFactory> factory);
    /// Injected for the same reason as the socket factory: the resolver
    /// owns worker threads, which a device may not acquire itself.
    void setNameResolver(std::unique_ptr<W5100Resolver> resolver);

    W5100Device();
    ~W5100Device();

    W5100Device(const W5100Device&) = delete;
    W5100Device& operator=(const W5100Device&) = delete;

    /// `powerCycle` false = the MR RST soft reset (registers and buffers
    /// clear, the indirect data address survives — Uthernet II manual
    /// p.10). True additionally drops caches and the data address.
    void reset(bool powerCycle);

    // ── Indirect bus interface (Uthernet II $C0n4-$C0n7) ──────────────
    uint8_t  modeRegister() const { return modeRegister_; }
    void     setModeRegister(uint8_t value);
    uint16_t dataAddress() const { return dataAddress_; }
    void     setDataAddressHigh(uint8_t v)
    {
        dataAddress_ = static_cast<uint16_t>((v << 8) | (dataAddress_ & 0x00FF));
    }
    void     setDataAddressLow(uint8_t v)
    {
        dataAddress_ = static_cast<uint16_t>(v | (dataAddress_ & 0xFF00));
    }
    /// Read/write through the data port, honouring MR's auto-increment.
    uint8_t readData();
    void    writeData(uint8_t value);

    // ── Direct addressing (used by the tests and the debug panel) ─────
    uint8_t readValueAt(uint16_t address);
    void    writeValueAt(uint16_t address, uint8_t value);
    /// Side-effect-free: no packet pull, no command dispatch.
    uint8_t peekValueAt(uint16_t address) const;

    /// Service in-flight non-blocking connects. Called from the card's
    /// cycle hook.
    void poll();

    /// Host transport for MACRAW / IPRAW. Not owned; may be null.
    void setBackend(NetworkBackend* backend) { backend_ = backend; }
    NetworkBackend* backend() const { return backend_; }

    /// Virtual DNS on/off. Off makes PTIMER read back its hardware
    /// default ($28) so software detects a stock W5100.
    void setVirtualDnsEnabled(bool enabled);
    bool virtualDnsEnabled() const { return virtualDns_; }

    // ── Introspection for the status panel ────────────────────────────
    struct SocketInfo {
        uint8_t  mode          = 0;
        uint8_t  status        = 0;
        uint16_t localPort     = 0;
        uint16_t remotePort    = 0;
        uint32_t remoteIp      = 0;   // network byte order
        uint16_t rxPending     = 0;
        uint16_t txPending     = 0;
        uint16_t rxCapacity    = 0;
        uint16_t txCapacity    = 0;
        bool     hasHostSocket = false;
    };
    SocketInfo socketInfo(size_t i) const;
    std::array<uint8_t, 6> macAddress() const;
    uint32_t localIp() const;   // network byte order

    uint64_t bytesSent()     const { return bytesSent_; }
    uint64_t bytesReceived() const { return bytesReceived_; }

    // ── Snapshot / rewind ─────────────────────────────────────────────
    void appendSnapshotState(std::vector<uint8_t>& out) const;
    void loadSnapshotState(const uint8_t* data, std::size_t len);

private:
    /// Lazily initialised to the production host factory.
    std::unique_ptr<W5100SocketFactory> socketFactory_;
    /// One of the chip's four sockets. `fd` is a host socket in TCP/UDP
    /// mode and `kInvalidSocket` otherwise; the raw modes need no host
    /// socket because they go out through the NetworkBackend.
    ///
    /// `pom2::socket_t`, not `int`: Winsock's SOCKET is an unsigned handle
    /// whose failure value is INVALID_SOCKET, not -1, so the `fd >= 0`
    /// test this struct used to carry was always true on Windows. See
    /// SocketCompat.h.
    struct Socket {
        uint16_t transmitBase    = 0;
        uint16_t transmitSize    = 0;
        uint16_t receiveBase     = 0;
        uint16_t receiveSize     = 0;
        uint16_t registerAddress = 0;

        /// Write cursor into the RX ring. The chip owns this one; the
        /// guest only moves the matching read cursor (SN_RX_RD).
        uint16_t rxWrite = 0;
        /// Bytes currently staged in the RX ring (SN_RX_RSR).
        uint16_t rxSize  = 0;

        /// The host socket, or null when this W5100 socket is closed. An
        /// owning handle rather than a raw fd: the interface is what a test
        /// substitutes to drive device behaviour without opening anything.
        std::unique_ptr<W5100HostSocket> host;
        uint8_t status     = kW5100SnSrClosed;
        /// Sn_IR, the socket's interrupt register (datasheet §5.2.3). Backed
        /// by a real byte rather than by `memory_`: the register is
        /// write-1-to-clear, which the plain register file cannot express.
        /// Host-side only, never snapshotted — every restored socket is
        /// demoted to CLOSED, so a pending CON/RECV would describe a
        /// connection that no longer exists.
        uint8_t interrupt  = 0;
        /// Per-protocol header the chip prepends to received data in the
        /// RX ring (`Uthernet2.cpp:212-234`): none for TCP, IP+port+len
        /// for UDP, IP+len for IPRAW, len for MACRAW.
        uint8_t headerSize = 0;

        /// TCP bytes accepted from the guest (SEND already completed and
        /// freed the TX ring) but not yet taken by the host socket — a
        /// slow peer makes sendto() return short or EAGAIN, and dropping
        /// the tail silently corrupted the stream. Host-side only, never
        /// snapshotted (a restored connection is demoted to CLOSED anyway).
        std::vector<uint8_t> pendingTx;

        bool isOpen() const
        {
            return host != nullptr &&
                   (status == kW5100SnSrEstablished ||
                    status == kW5100SnSrCloseWait ||
                    status == kW5100SnSrUdp);
        }
    };

    // Memory helpers.
    uint8_t  mem(uint16_t a) const { return memory_[a & kW5100MemMax]; }
    void     setMem(uint16_t a, uint8_t v) { memory_[a & kW5100MemMax] = v; }
    uint16_t readNetworkWord(uint16_t a) const;
    uint32_t readAddress(uint16_t a) const;

    // Socket lifecycle (`Uthernet2.cpp:910-1102`).
    void setSocketStatus(size_t i, uint8_t status);
    /// Set bits in Sn_IR. Datasheet §5.2.3: the bit stays set until the host
    /// writes a 1 to it, and the matching IR bit follows it.
    void raiseSocketIrq(size_t i, uint8_t bits);
    void clearSocket(size_t i);
    void openSystemSocket(size_t i, W5100SocketKind kind, uint8_t status);
    void openSocket(size_t i);
    void closeSocket(size_t i);
    void connectSocket(size_t i);
    void listenSocket(size_t i);
    void setCommandRegister(size_t i, uint8_t value);

    // Buffer geometry (`Uthernet2.cpp:441-547`).
    void     setTxSizes(uint8_t value);
    void     setRxSizes(uint8_t value);
    /// Re-fit rxWrite/rxSize to the socket's (possibly just rebuilt)
    /// ring geometry so no later ring arithmetic can underflow — a guest
    /// shrinking RMSR under staged data, or a crafted snapshot, used to
    /// make ringFreeRoom() wrap to ~64 K.
    void     clampRingState(size_t i);
    uint16_t txDataSize(size_t i) const;
    uint8_t  txFreeSizeRegister(size_t i, unsigned shift) const;
    uint8_t  rxDataSizeRegister(size_t i, unsigned shift) const;
    void     resetRxTxBuffers(size_t i);
    void     updateRsr(size_t i);

    // RX ring writers (`Uthernet2.cpp:119-181`).
    void ringWrite8(size_t i, uint8_t v);
    void ringWrite16(size_t i, uint16_t v);
    void ringWriteData(size_t i, const uint8_t* data, size_t len);
    bool ringHasRoomFor(size_t i, size_t len) const;
    uint16_t ringFreeRoom(size_t i) const;

    // Receive paths (`Uthernet2.cpp:549-770`).
    void receiveOnePacket(size_t i);
    void receiveOnePacketFromSocket(size_t i);
    void receiveOnePacketRaw();
    void receiveOnePacketMacRaw(size_t i, const uint8_t* data, int size);
    void receiveOnePacketIpRaw(size_t i, const uint8_t* payload, size_t len,
                               uint32_t source);

    // Transmit paths (`Uthernet2.cpp:772-895`).
    void sendData(size_t i);
    void sendDataToSocket(size_t i, const std::vector<uint8_t>& data);
    /// Push a TCP socket's pendingTx tail into the host socket; called on
    /// every poll() until the queue drains. Kills the connection if the
    /// backlog passes 1 MiB (the peer has stalled for good).
    void flushPendingTx(size_t i);
    void sendDataMacRaw(const std::vector<uint8_t>& data);
    void sendDataIpRaw(size_t i, const std::vector<uint8_t>& payload);

    // Register decode (`Uthernet2.cpp:1104-1354`).
    uint8_t readSocketRegister(uint16_t address);
    /// The common IR register — DERIVED from the four Sn_IR bytes, never
    /// stored (datasheet §5.1 "IR": IR(0)-IR(3) clear when Sn_IR clears).
    uint8_t interruptRegister() const;
    void    writeSocketRegister(uint16_t address, uint8_t value);
    void    writeCommonRegister(uint16_t address, uint8_t value);
    void    autoIncrement();

    // Virtual DNS + ARP (`Uthernet2.cpp:1012-1037`, `:1487-1525`).
    void resolveDns(size_t i);
    void macForAddress(uint32_t ipv4, MacAddress& out);

    /// Virtual DNS. Created lazily so a card that never resolves a name
    /// never starts a resolver.
    std::unique_ptr<W5100Resolver> resolver_;
    void drainPendingDns();

    std::vector<uint8_t>              memory_;
    std::array<Socket, kSocketCount>  sockets_{};
    uint8_t                           modeRegister_ = 0;
    uint16_t                          dataAddress_  = 0;
    bool                              virtualDns_   = true;
    NetworkBackend*                   backend_      = nullptr;

    /// The real card has no ARP cache — this one exists purely so an
    /// IPRAW send does not re-resolve on every packet.
    std::map<uint32_t, MacAddress> arpCache_;


    uint64_t bytesSent_     = 0;
    uint64_t bytesReceived_ = 0;
};

} // namespace pom2

#endif // POM2_W5100_DEVICE_H
