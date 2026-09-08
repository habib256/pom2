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

// POM2 — W5100 host-socket seam.
//
// Pins that W5100Device drives its host socket through the W5100Socket
// interface, so device behaviour can be exercised with NO host socket opened:
// no bind, no connect, no port, nothing for a sandboxed or parallel CI run to
// collide with.
//
// The two cases below are the ones that were previously unreachable without a
// real peer on the far end, and both are behaviours the chip is expected to
// get right rather than incidental plumbing.

#include "W5100Device.h"
#include "W5100Resolver.h"

#include "fakes/FakeW5100Socket.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// The LISTEN case below counts LOG LINES, which means capturing stderr. POSIX
// only: on the platforms without dup2 the behavioural half of the case still
// runs and the count is simply not asserted.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define POM2_TEST_CAN_CAPTURE_STDERR 1
#include <unistd.h>
#else
#define POM2_TEST_CAN_CAPTURE_STDERR 0
#endif

namespace {

using namespace pom2;

constexpr uint16_t socketReg(int index, uint8_t offset)
{
    return static_cast<uint16_t>(kW5100S0Base + (index << 8) + offset);
}

// Drive the guest-visible register sequence for "open a TCP socket".
void openTcpSocket(W5100Device& device, int index)
{
    device.writeValueAt(socketReg(index, kW5100SnMr), kW5100SnMrTcp);
    device.writeValueAt(socketReg(index, kW5100SnCr), kW5100SnCrOpen);
}

void connectTo(W5100Device& device, int index, uint32_t ipBigEndian,
               uint16_t port)
{
    device.writeValueAt(socketReg(index, kW5100SnDipr0),
                        static_cast<uint8_t>(ipBigEndian >> 24));
    device.writeValueAt(socketReg(index, kW5100SnDipr0 + 1),
                        static_cast<uint8_t>(ipBigEndian >> 16));
    device.writeValueAt(socketReg(index, kW5100SnDipr0 + 2),
                        static_cast<uint8_t>(ipBigEndian >> 8));
    device.writeValueAt(socketReg(index, kW5100SnDipr0 + 3),
                        static_cast<uint8_t>(ipBigEndian));
    device.writeValueAt(socketReg(index, kW5100SnDport0),
                        static_cast<uint8_t>(port >> 8));
    device.writeValueAt(socketReg(index, kW5100SnDport1),
                        static_cast<uint8_t>(port));
    device.writeValueAt(socketReg(index, kW5100SnCr), kW5100SnCrConnect);
}

// ── Case 1: OPEN goes through the factory, and the kind follows SnMR ──────
void testOpenUsesInjectedFactory()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    assert(fake->openCount == 0);
    assert(!device.socketInfo(0).hasHostSocket);

    openTcpSocket(device, 0);

    // One host socket, of the kind the guest asked for in SnMR. Nothing was
    // opened on the real stack: with the inline ::socket() call this test
    // could not have existed without binding something.
    assert(fake->openCount == 1);
    assert(fake->lastKind == W5100SocketKind::Tcp);
    assert(device.socketInfo(0).hasHostSocket);

    // CLOSE releases the handle, which is what closes the host socket now —
    // the owning handle replaced a raw fd plus a manual closeHostSocket().
    device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrClose);
    assert(!device.socketInfo(0).hasHostSocket);

    std::printf("  open routes through the factory, kind follows SnMR: OK\n");
}

// ── Case 2: a non-blocking connect parks in SYN_SENT until it completes ───
//
// This is the case the inline implementation could only reach against a real
// unreachable host, timing-dependent. The guest polls SN_SR for $13 (SYN_SENT)
// and must see $17 (ESTABLISHED) only once the connection actually completes.
void testConnectInProgressParksInSynSent()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    fake->nextConnectResult = W5100ConnectResult::InProgress;
    fake->nextPollConnectResult = W5100ConnectResult::InProgress;
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    openTcpSocket(device, 0);
    connectTo(device, 0, 0x0A000001u, 8080);

    assert(fake->lastSocket != nullptr);
    assert(fake->lastSocket->connectCount == 1);
    // The destination reached the socket as the guest wrote it.
    assert(fake->lastSocket->lastConnectPort != 0);
    assert(device.socketInfo(0).status == kW5100SnSrSynSent);

    // Still pending: poll() must not promote it.
    device.poll();
    assert(device.socketInfo(0).status == kW5100SnSrSynSent);

    // Completed: the next poll promotes to ESTABLISHED.
    fake->lastSocket->pollConnectResult = W5100ConnectResult::Connected;
    device.poll();
    assert(device.socketInfo(0).status == kW5100SnSrEstablished);

    std::printf("  connect InProgress parks in SYN_SENT, then promotes: OK\n");
}

// ── Case 3: a refused connection returns the socket to CLOSED ─────────────
void testConnectRefusedClosesSocket()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    fake->nextConnectResult = W5100ConnectResult::InProgress;
    fake->nextPollConnectResult = W5100ConnectResult::Failed;
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    openTcpSocket(device, 0);
    connectTo(device, 0, 0x0A000001u, 9);

    assert(device.socketInfo(0).status == kW5100SnSrSynSent);
    device.poll();
    // Refused / unreachable is CLOSED, which is what the guest polls SN_SR
    // for. Leaving it in SYN_SENT hangs every driver that waits on it.
    assert(device.socketInfo(0).status == kW5100SnSrClosed);
    assert(!device.socketInfo(0).hasHostSocket);

    std::printf("  refused connection returns to CLOSED: OK\n");
}

// ── Case 4: a driver that MASKS Sn_TX_WR still sends exactly what it staged ─
//
// Sn_TX_RD/WR are free-running counters in the datasheet, but a driver that
// writes them back reduced to the ring (`wr = (wr + len) & (size - 1)`) is
// legal and common. The first wrap then makes the unmasked difference
// meaningless — rd = $07C0, wr = $0064 differ by $F8A4 — and clamping that to
// the ring size sent the WHOLE 2 KB buffer: 164 staged bytes followed by 1884
// bytes of stale ring, after which Sn_TX_FSR read 0 and the guest stalled.
void testMaskedTxPointersSendOnlyTheStagedBytes()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    openTcpSocket(device, 0);
    // 10.0.0.1 — RFC 1918, which the destination policy allows.
    connectTo(device, 0, 0x0A000001u, 8080);
    assert(device.socketInfo(0).status == kW5100SnSrEstablished);

    // Default TMSR ($55) gives socket 0 a 2 KB ring based at $4000. Sn_TX_RD
    // is read-only, as on the chip, so the read pointer is moved the only way
    // a guest can move it: by sending. 1984 bytes leaves rd == wr == $07C0.
    constexpr uint16_t kSize    = 2048;
    constexpr uint16_t kTxBase  = kW5100TxBase;
    constexpr uint16_t kReadPtr = 0x07C0;          // 1984
    constexpr uint16_t kStaged  = 164;             // wraps past the end

    auto setWritePtr = [&](uint16_t wr) {
        device.writeValueAt(socketReg(0, kW5100SnTxWr0),
                            static_cast<uint8_t>(wr >> 8));
        device.writeValueAt(socketReg(0, kW5100SnTxWr1),
                            static_cast<uint8_t>(wr));
    };

    for (uint16_t off = 0; off < kReadPtr; ++off)
        device.writeValueAt(static_cast<uint16_t>(kTxBase + off), 0x11);
    setWritePtr(kReadPtr);
    device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrSend);
    assert(fake->lastSocket != nullptr);
    assert(fake->lastSocket->sentBytes.size() == kReadPtr);
    fake->lastSocket->sentBytes.clear();

    // The guest now stages 164 bytes across the wrap and writes the pointer
    // back MASKED: (1984 + 164) & 2047 == $0064.
    for (uint16_t k = 0; k < kStaged; ++k) {
        const uint16_t offset = static_cast<uint16_t>((kReadPtr + k) & (kSize - 1));
        device.writeValueAt(static_cast<uint16_t>(kTxBase + offset),
                            static_cast<uint8_t>(0x40 + (k & 0x1F)));
    }
    // …and the rest of the ring holds something else entirely, so a send that
    // over-reads is visible rather than plausible.
    for (uint16_t off = 0; off < kSize; ++off) {
        const uint16_t staged =
            static_cast<uint16_t>((off - kReadPtr) & (kSize - 1));
        if (staged < kStaged) continue;
        device.writeValueAt(static_cast<uint16_t>(kTxBase + off), 0xEE);
    }
    setWritePtr(static_cast<uint16_t>((kReadPtr + kStaged) & (kSize - 1)));

    // Sn_TX_FSR must agree BEFORE the send, or the guest never writes at all.
    const uint16_t free =
        static_cast<uint16_t>((device.readValueAt(socketReg(0, kW5100SnTxFsr0)) << 8) |
                               device.readValueAt(socketReg(0, kW5100SnTxFsr1)));
    assert(free == kSize - kStaged);

    device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrSend);

    assert(fake->lastSocket->sentBytes.size() == kStaged);
    for (uint16_t k = 0; k < kStaged; ++k)
        assert(fake->lastSocket->sentBytes[k] ==
               static_cast<uint8_t>(0x40 + (k & 0x1F)));

    std::printf("  masked TX pointers send exactly the staged bytes: OK\n");
}

// ── Case 5: the guest may not reach the host's own loopback services ───────
//
// TCP/UDP run on HOST sockets, so 127.0.0.1 was a CONNECT away — and POM2's AI
// control server sits there answering a loopback client with no Origin as if
// it were native. The chip's own failure for an unreachable destination is
// SOCK_CLOSED + TIMEOUT, which is what a driver is written to see.
void testLoopbackDestinationIsRefused()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    openTcpSocket(device, 0);
    connectTo(device, 0, 0x7F000001u, 6503);      // 127.0.0.1, the AI server

    // NOT fake->lastSocket->connectCount: the refusal path closes the socket,
    // so that pointer dangles by the time we get here (ASan caught it on the
    // nightly). The factory's tally outlives every socket it made.
    assert(fake->connectAttempts == 0);           // never even attempted
    assert(device.socketInfo(0).status == kW5100SnSrClosed);
    assert(!device.socketInfo(0).hasHostSocket);
    assert(device.readValueAt(socketReg(0, kW5100SnIr)) & kW5100SnIrTimeout);

    // The same address with the escape hatch open connects normally: the
    // policy is a default, not a wall.
    device.setAllowLoopback(true);
    openTcpSocket(device, 0);
    connectTo(device, 0, 0x7F000001u, 6503);
    assert(device.socketInfo(0).status == kW5100SnSrEstablished);

    // …and 224/4, 169.254/16 and 0/8 are refused whatever that setting says.
    for (const uint32_t bad : { 0xE0000001u, 0xA9FE0001u, 0x00000001u,
                                0xFFFFFFFFu }) {
        openTcpSocket(device, 0);
        connectTo(device, 0, bad, 80);
        assert(device.socketInfo(0).status == kW5100SnSrClosed);
    }

    std::printf("  loopback / link-local / multicast destinations refused: OK\n");
}

// The UDP path is the OTHER half of the same escape: a datagram needs no
// CONNECT to reach 127.0.0.1:6503.
void testLoopbackDatagramIsRefused()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    device.writeValueAt(socketReg(0, kW5100SnMr), kW5100SnMrUdp);
    device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrOpen);
    assert(device.socketInfo(0).status == kW5100SnSrUdp);

    // Destination 127.0.0.1:6503, one byte staged, SEND.
    for (int b = 0; b < 4; ++b)
        device.writeValueAt(socketReg(0, static_cast<uint8_t>(kW5100SnDipr0 + b)),
                            b == 0 ? 0x7F : (b == 3 ? 0x01 : 0x00));
    device.writeValueAt(socketReg(0, kW5100SnDport0), 0x19);
    device.writeValueAt(socketReg(0, kW5100SnDport1), 0x67);   // 6503
    device.writeValueAt(kW5100TxBase, 0x2F);
    device.writeValueAt(socketReg(0, kW5100SnTxWr0), 0x00);
    device.writeValueAt(socketReg(0, kW5100SnTxWr1), 0x01);
    device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrSend);

    assert(fake->lastSocket->sentBytes.empty());
    assert(device.socketInfo(0).status == kW5100SnSrClosed);
    assert(device.readValueAt(socketReg(0, kW5100SnIr)) & kW5100SnIrTimeout);

    std::printf("  loopback datagram refused: OK\n");
}

// ── Case 5b: a discarded datagram is one datagram, not the socket ─────────
//
// Bug hunt #8. sendto() reports ENETUNREACH / EHOSTUNREACH / ECONNREFUSED
// (host off the network, an ICMP report from an earlier datagram) through the
// same channel as a genuine fault, and the device answered every non-EAGAIN
// error with clearSocket: Sn_SR to CLOSED, fd gone. A real W5100 answers an
// undeliverable datagram with nothing at all, so a period UDP client that
// loops SEND/RECV never re-OPENs and goes silent for good. The receive path
// has classified this as Discarded since SocketCompat.h trap 7; the send path
// does now too. A genuine fault still tears the socket down.
void testDiscardedDatagramKeepsTheSocket()
{
    for (const bool genuine : { false, true }) {
        W5100Device device;
        auto factory = std::make_unique<test::FakeW5100SocketFactory>();
        auto* fake = factory.get();
        device.setSocketFactory(std::move(factory));
        device.reset(true);

        device.writeValueAt(socketReg(0, kW5100SnMr), kW5100SnMrUdp);
        device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrOpen);
        assert(device.socketInfo(0).status == kW5100SnSrUdp);
        fake->lastSocket->sendResult =
            W5100SendResult{genuine ? W5100IoStatus::Failed : W5100IoStatus::Discarded, 0};

        // Destination 10.0.2.2:123, one byte staged, SEND.
        const uint8_t ip[4] = { 10, 0, 2, 2 };
        for (int b = 0; b < 4; ++b)
            device.writeValueAt(socketReg(0, static_cast<uint8_t>(kW5100SnDipr0 + b)), ip[b]);
        device.writeValueAt(socketReg(0, kW5100SnDport0), 0x00);
        device.writeValueAt(socketReg(0, kW5100SnDport1), 0x7B);
        device.writeValueAt(kW5100TxBase, 0x2F);
        device.writeValueAt(socketReg(0, kW5100SnTxWr0), 0x00);
        device.writeValueAt(socketReg(0, kW5100SnTxWr1), 0x01);
        device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrSend);

        if (genuine) {
            assert(device.socketInfo(0).status == kW5100SnSrClosed &&
                   "a genuine socket fault must still close the socket");
        } else {
            assert(device.socketInfo(0).status == kW5100SnSrUdp &&
                   "one undeliverable datagram destroyed the guest's UDP socket");
            assert(device.socketInfo(0).hasHostSocket);
        }
    }
    std::printf("  discarded datagram keeps the socket, a fault closes it: OK\n");
}

// ── Case 6: the guest may not claim any local port it likes ───────────────
//
// Sn_PORT is claimed on the HOST, on every interface. A privileged port would
// shadow a real service; POM2's own listeners would let the guest stand in
// front of the AI server, the SSC's telnet bridge or the FujiNet relay. Both
// refusals fall back to an ephemeral port, which is every request/response
// exchange an Apple II program actually attempts.
void testLocalPortPolicy()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    auto openUdpOnPort = [&](uint16_t port) {
        device.writeValueAt(socketReg(0, kW5100SnPort0),
                            static_cast<uint8_t>(port >> 8));
        device.writeValueAt(socketReg(0, kW5100SnPort1),
                            static_cast<uint8_t>(port));
        device.writeValueAt(socketReg(0, kW5100SnMr), kW5100SnMrUdp);
        device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrOpen);
    };

    // 68 (DHCP client) is exactly what Sn_PORT exists for on real silicon —
    // but claiming it on the host means shadowing the host's own DHCP client.
    openUdpOnPort(68);
    assert(fake->lastSocket->bindCount == 0);
    assert(device.socketInfo(0).status == kW5100SnSrUdp);   // still usable

    openUdpOnPort(6503);                                    // the AI server
    assert(fake->lastSocket->bindCount == 0);

    // An ordinary high port is claimed as the datasheet says.
    openUdpOnPort(14000);
    assert(fake->lastSocket->bindCount == 1);
    assert(fake->lastSocket->lastBindPort == 14000);

    std::printf("  local-port policy refuses privileged + POM2 ports: OK\n");
}

#if POM2_TEST_CAN_CAPTURE_STDERR
/// Run `fn` with stderr redirected into a pipe and return what it wrote.
/// The pipe buffer is far larger than the handful of lines expected here.
std::string captureStderr(void (*fn)(W5100Device&), W5100Device& device)
{
    int pipeFds[2] = { -1, -1 };
    if (pipe(pipeFds) != 0) return {};
    std::fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    dup2(pipeFds[1], STDERR_FILENO);
    close(pipeFds[1]);

    fn(device);

    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    std::string out;
    char buf[4096];
    ssize_t got = 0;
    while ((got = read(pipeFds[0], buf, sizeof buf)) > 0)
        out.append(buf, static_cast<std::size_t>(got));
    close(pipeFds[0]);
    return out;
}

void listenLoop(W5100Device& device)
{
    // The canonical W5100 server loop, retried the way a driver retries it.
    for (int i = 0; i < 16; ++i) {
        openTcpSocket(device, 0);
        device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrListen);
    }
}
#endif

// ── Case 7: LISTEN's refusal is said ONCE, not once per lap ───────────────
//
// LISTEN cannot be honoured (there is no inbound path), and the honest answer
// is the chip's own SOCK_CLOSED + TIMEOUT — which makes the driver's server
// loop go round again. Every lap used to cost an unbuffered stderr write on
// the CPU thread with the emulator's state mutex held.
void testListenWarnsOncePerSocket()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    device.setSocketFactory(std::move(factory));
    device.reset(true);

#if POM2_TEST_CAN_CAPTURE_STDERR
    const std::string logged = captureStderr(&listenLoop, device);
    std::size_t lines = 0;
    for (std::size_t at = logged.find("LISTEN is not supported");
         at != std::string::npos;
         at = logged.find("LISTEN is not supported", at + 1))
        ++lines;
    assert(lines == 1);
#else
    listenLoop(device);
#endif

    // The ANSWER is unchanged — every lap still ends CLOSED with TIMEOUT set,
    // which is what the driver polls.
    assert(device.socketInfo(0).status == kW5100SnSrClosed);
    assert(!device.socketInfo(0).hasHostSocket);
    assert(device.readValueAt(socketReg(0, kW5100SnIr)) & kW5100SnIrTimeout);

    // A chip reset re-arms the warning: a fresh session deserves the reason.
    device.reset(true);
    openTcpSocket(device, 0);
    device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrListen);
    assert(device.socketInfo(0).status == kW5100SnSrClosed);

    std::printf("  LISTEN refusal logged once per socket: OK\n");
}

// ── Case 8: raw guest bytes never reach the host resolver ─────────────────
//
// The virtual-DNS name lives at socket offset $2A-$FF and is whatever the
// guest wrote there. It used to go straight to getaddrinfo() — every lookup is
// a query the host resolver puts on the wire, so an arbitrary name is an
// exfiltration channel — and, on failure, straight into a log line a human
// reads in a terminal, control bytes and all.
class RecordingResolver final : public W5100Resolver
{
public:
    Result resolve(const std::string& name, int) override
    {
        seen.push_back(name);
        Result r;
        r.status  = Status::Resolved;
        r.address = 0x0100000Au;      // 10.0.0.1, network byte order
        return r;
    }
    void poll() override {}
    void clearCache() override {}

    std::vector<std::string> seen;
};

void testVirtualDnsNamesAreValidated()
{
    W5100Device device;
    device.setSocketFactory(std::make_unique<test::FakeW5100SocketFactory>());
    auto resolver = std::make_unique<RecordingResolver>();
    auto* rec = resolver.get();
    device.setNameResolver(std::move(resolver));
    device.reset(true);

    auto openWithName = [&](const std::string& name) {
        device.writeValueAt(socketReg(0, kW5100SnDnsNameLen),
                            static_cast<uint8_t>(name.size()));
        for (std::size_t k = 0; k < name.size(); ++k)
            device.writeValueAt(
                socketReg(0, static_cast<uint8_t>(kW5100SnDnsNameBegin + k)),
                static_cast<uint8_t>(name[k]));
        device.writeValueAt(socketReg(0, kW5100SnMr),
                            kW5100SnMrTcp | kW5100SnVirtualDns);
        device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrOpen);
    };

    // An ordinary hostname goes through, and its answer lands in DIPR.
    openWithName("irc.libera.chat");
    assert(rec->seen.size() == 1 && rec->seen[0] == "irc.libera.chat");
    assert(device.readValueAt(socketReg(0, kW5100SnDipr0)) == 0x0A);

    // These do not: a control byte, a space, a label past 63 octets, an empty
    // label. None of them can be a name that would have resolved, so refusing
    // them costs the guest nothing.
    const std::string tooLongLabel(64, 'a');
    for (const std::string& bad : { std::string("a\x07\x1b[2Jb.test"),
                                    std::string("has space.test"),
                                    tooLongLabel + ".test",
                                    std::string(".leading.test"),
                                    std::string("double..dot.test"),
                                    std::string("under_score.test") }) {
        openWithName(bad);
        // Nothing new reached the resolver…
        assert(rec->seen.size() == 1);
        // …and DIPR is left at the extension's "resolution failed" marker.
        assert(device.readValueAt(socketReg(0, kW5100SnDipr0)) == 0x00);
    }

    std::printf("  virtual-DNS names are validated before the lookup: OK\n");
}

} // namespace

int main()
{
    std::printf("W5100 host-socket seam\n");
    testOpenUsesInjectedFactory();
    testConnectInProgressParksInSynSent();
    testConnectRefusedClosesSocket();
    testMaskedTxPointersSendOnlyTheStagedBytes();
    testLoopbackDestinationIsRefused();
    testLoopbackDatagramIsRefused();
    testDiscardedDatagramKeepsTheSocket();
    testLocalPortPolicy();
    testListenWarnsOncePerSocket();
    testVirtualDnsNamesAreValidated();
    std::printf("OK\n");
    return 0;
}
