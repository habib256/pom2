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

// SpTcpTransport — SP-over-SLIP across a loopback TCP connection.
//
// POM2 LISTENS and the FujiNet connects in, which is the direction both
// reference implementations use: the FujiNet AppleWin fork's
// `devrelay/service/Listener.cpp` ("Creates a Connection object, which is how
// SP device(s) will register itself with our listener") and
// `fujinet-go-apple2-desktop`, which joins its embedded firmware to the
// emulator over loopback TCP 1985. Keeping that direction and that port means
// an existing FujiNet configuration works against POM2 with nothing changed.
//
// Binds 127.0.0.1 only. The peer is a program on the same machine; exposing
// a SmartPort bus — which can read and write the guest's disks — to the LAN
// is not something a user should be able to do by accident.
//
// ── Who may connect, and why there is no authentication ───────────────────
//
// The peer IS the SmartPort device: whatever connects here can serve blocks
// the guest will boot from and read blocks the guest writes. The first
// connector is accepted unauthenticated, and that is not an oversight —
// SP-over-SLIP has no authentication to speak. It is a framed byte stream
// defined by fujinet-pc / the FujiNet AppleWin fork, and inventing a POM2
// handshake would mean no stock FujiNet build could connect at all, which is
// the entire purpose of matching their port and their direction. So the
// exposure is bounded structurally instead, in four ways:
//
//   * 127.0.0.1 ONLY (the bind above) — never a LAN address, no setting.
//   * The listener is armed only when a FujiNet CARD IS PLUGGED. The
//     `fujinet_enabled<slot>` default is true, but it is read inside
//     `plugFujiNet`, and `startDeferredFujiNetLinks` re-checks that the slot
//     really holds a FujiNetCard before calling start(). A machine with no
//     FujiNet in any slot opens no socket.
//   * ONE peer at a time; a second connection is closed immediately
//     (pollForPeer below), so nothing can displace a live link.
//   * A CONNECTOR THAT DOES NOT SPEAK THE PROTOCOL IS DROPPED. The worker
//     enumerates the moment a peer appears, and `enumerateDevices()` calls
//     `handlePeerLost()` when NOTHING answers INIT — three attempts at the
//     call budget apiece, so about a second of grace and then the socket is
//     closed and the port is listening again. Squatting on it costs a
//     reconnect per second and yields nothing.
//
// The port and the peer's address are both logged (startListening /
// pollForPeer), so an unexpected connector is visible rather than silent.

#include "SpTransport.h"

#include "Logger.h"

#include <algorithm>
#include <chrono>
#include "SocketUtil.h"

#include <cstring>

namespace pom2 {

SpTcpTransport::SpTcpTransport(uint16_t port) : port_(port) {}

SpTcpTransport::~SpTcpTransport()
{
    dropPeer();
    stopListening();
}

#if !POM2_HAS_SOCKETS

// Emscripten: no host sockets at all. The card still constructs and plugs so
// the rest of the machine is unaffected; it simply never finds a peer.
bool SpTcpTransport::startListening(std::string& errOut)
{ errOut = "host sockets are not available in this build"; return false; }
void SpTcpTransport::stopListening() {}
bool SpTcpTransport::isListening() const { return false; }
bool SpTcpTransport::isOpen() const { return false; }
bool SpTcpTransport::pollForPeer(int) { return false; }
bool SpTcpTransport::writeAll(const uint8_t*, std::size_t) { return false; }
int  SpTcpTransport::readSome(uint8_t*, std::size_t, int) { return -1; }
bool SpTcpTransport::checkPeerAlive() { return false; }
void SpTcpTransport::dropPeer() {}
void SpTcpTransport::shutdown() {}
std::string SpTcpTransport::describe() const { return "TCP unavailable in this build"; }

#else

bool SpTcpTransport::startListening(std::string& errOut)
{
    if (isListening()) return true;
    ensureSocketStack();

    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!isValidSocket(fd)) {
        errOut = "socket(): " + lastSocketErrorText();
        return false;
    }
    // NOT raw SO_REUSEADDR — on Winsock that would let any local process
    // take the port from under us while we are listening. See SocketCompat.h
    // trap 6.
    setListenerBindPolicy(fd);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = hostToNet16(port_);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback ONLY

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        errOut = "bind(127.0.0.1:" + std::to_string(port_) + "): " +
                 lastSocketErrorText();
        socket_t tmp = fd;
        closeHostSocket(tmp);
        return false;
    }
    if (::listen(fd, 2) != 0) {
        errOut = "listen(): " + lastSocketErrorText();
        socket_t tmp = fd;
        closeHostSocket(tmp);
        return false;
    }

    listenFd_.store(fd);
    log().info("FujiNet", "listening for SP-over-SLIP on 127.0.0.1:" +
                              std::to_string(port_));
    return true;
}

void SpTcpTransport::stopListening()
{
    // Contract: the worker must already be joined. Closing a listening
    // descriptor a thread is parked in accept() on is the use-after-close
    // race SocketUtil.h rule 1 exists to prevent.
    //
    // The lifetime lock covers the exchange AND the close, so a shutdown()
    // running concurrently on another thread cannot read the number and then
    // ::shutdown() it after we have handed it back to the kernel.
    std::lock_guard<std::mutex> life(fdLifeMtx_);
    socket_t fd = listenFd_.exchange(kInvalidSocket);
    closeHostSocketValue(fd);
}

bool SpTcpTransport::isListening() const
{ return isValidSocket(listenFd_.load()); }

bool SpTcpTransport::isOpen() const
{ return isValidSocket(clientFd_.load()); }

bool SpTcpTransport::pollForPeer(int timeoutMs)
{
    const socket_t lfd = listenFd_.load();
    if (!isValidSocket(lfd)) return false;

    socket_t    fd   = kInvalidSocket;
    sockaddr_in peer{};
    // poll-then-accept, never a bare blocking accept: shutdown() must be able
    // to wake this on macOS/BSD too, where shutdown() on a listener does not
    // unblock accept().
    switch (pollAcceptOnce(lfd, timeoutMs, fd, peer)) {
    case PollAccept::Retry:    return false;
    case PollAccept::Shutdown: return false;
    case PollAccept::Accepted: break;
    }

    if (isOpen()) {
        // One peer at a time — two would mean two SmartPort device-number
        // spaces to merge, which no real configuration needs. Refuse the
        // newcomer rather than silently swapping the live link out from
        // under an in-flight call.
        log().warn("FujiNet", "second SP-over-SLIP peer refused (already "
                              "connected to " + describe() + ")");
        closeHostSocketValue(fd);
        return false;
    }

    disableSigpipe(fd);
    // SmartPort traffic is small request/response packets and the emulated
    // 6502 is stalled waiting for each one, so Nagle's delayed-ACK pairing
    // would add up to 40 ms to every single disk block.
    setSockOptInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    // NON-BLOCKING, and this is load-bearing rather than tidy. writeAll()
    // runs on the CPU thread holding both callMtx_ and the emulator's state
    // mutex; on a blocking socket a peer that stops draining (suspended,
    // deadlocked, under a debugger) parks send() for ever once the send and
    // receive buffers fill — a ~131 KB SLIP-escaped write is enough. The UI
    // thread then blocks on the state mutex the moment it paints the FujiNet
    // panel and can never reach the Stop button, so the only way out was to
    // kill POM2. readSome() already treats EWOULDBLOCK as "nothing yet".
    setNonBlocking(fd);

    {
        std::lock_guard<std::mutex> life(fdLifeMtx_);
        std::lock_guard<std::mutex> st(statusMtx_);
        clientFd_.store(fd);
        peerText_ = peerAddressText(peer) + ":" +
                    std::to_string(netToHost16(peer.sin_port));
    }
    log().info("FujiNet", "SP-over-SLIP peer connected from " + describe());
    return true;
}

bool SpTcpTransport::writeAll(const uint8_t* p, std::size_t n)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const socket_t fd = clientFd_.load();
    if (!isValidSocket(fd)) return false;

    // ONE deadline for the whole write, not one per wait. A peer that accepts
    // a trickle of bytes just often enough keeps a per-wait timeout alive for
    // ever, and this runs on the CPU thread under the emulator's state mutex:
    // "slow" and "hung" look identical from the Apple II, and both freeze the
    // window. SpOverSlipLink's header promises every call is bounded by
    // timeoutMs(); this is where TCP keeps that promise.
    //
    // THE CALLER'S budget, capped at 2 s. It used to be a flat 2000 ms, which
    // meant one SmartPort call could cost that PLUS the read timeout — up to
    // 7 s of the CPU thread holding the emulator's state mutex, from a single
    // `$C0xx` access, with the FujiNet panel's own Stop button unreachable
    // behind the same mutex. `setWriteDeadlineMs` (SpTransport.h) is how the
    // session layer hands its budget down; the 2 s cap keeps a user who winds
    // the panel's timeout up from paying more than the old worst case.
    const int kWriteDeadlineMs = std::min(2000, writeDeadlineMs_.load());
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kWriteDeadlineMs);

    std::size_t sent = 0;
    while (sent < n) {
        const iolen_t w = sendNoSignal(fd, p + sent, n - sent);
        if (w > 0) { sent += static_cast<std::size_t>(w); continue; }
        const int e = lastSocketError();
        if (!errInterrupted(e) && !errWouldBlock(e)) return false;

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            log().warn("FujiNet", "peer stopped accepting data — " +
                                  std::to_string(sent) + " of " +
                                  std::to_string(n) + " bytes written before the "
                                  "write deadline; dropping the connection");
            return false;
        }
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (waitSocket(fd, SocketWait::Write, left) != WaitResult::Ready) {
            // Timeout or error: either way this peer is not taking the packet.
            return false;
        }
    }
    return true;
}

int SpTcpTransport::readSome(uint8_t* p, std::size_t n, int timeoutMs)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const socket_t fd = clientFd_.load();
    if (!isValidSocket(fd)) return -1;

    switch (waitSocket(fd, SocketWait::Read, timeoutMs)) {
    case WaitResult::Timeout: return 0;
    case WaitResult::Failed:  return -1;
    case WaitResult::Ready:   break;
    }

    const iolen_t got = recvSocket(fd, p, n);
    if (got > 0) return static_cast<int>(got);
    if (got == 0) return -1;                 // orderly close = peer gone
    const int e = lastSocketError();
    if (errInterrupted(e) || errWouldBlock(e)) return 0;
    return -1;
}

bool SpTcpTransport::checkPeerAlive()
{
    // Under the I/O lock, so this cannot run concurrently with a real
    // readSome() on the CPU thread — MSG_PEEK plus a concurrent recv() would
    // otherwise be a coin toss over who sees the byte.
    std::lock_guard<std::mutex> io(mtx_);
    const socket_t fd = clientFd_.load();
    if (!isValidSocket(fd)) return false;

    // Zero timeout: a probe, not a wait. Not-readable tells us nothing bad —
    // an idle-but-live peer looks exactly like that.
    if (waitSocket(fd, SocketWait::Read, 0) != WaitResult::Ready) return true;

    uint8_t        b   = 0;
    const iolen_t  got = recvPeekSocket(fd, &b, 1);
    if (got > 0) return true;      // real data, left in the queue for the CPU
    if (got == 0) return false;    // orderly close: the peer is gone
    const int e = lastSocketError();
    return errInterrupted(e) || errWouldBlock(e);
}

void SpTcpTransport::dropPeer()
{
    socket_t fd = kInvalidSocket;
    {
        // I/O lock first: readSome/writeAll hold it across their syscall, so
        // taking it here is what guarantees nobody is inside recv() on the
        // descriptor we are about to close.
        std::lock_guard<std::mutex> io(mtx_);
        // Lifetime lock second, and held ACROSS the close: shutdown() runs
        // without the I/O lock (it must — a read in flight is when it is
        // called), so this is the only thing that stops it from ::shutdown()ing
        // a number we have already closed and another socket() has recycled.
        std::lock_guard<std::mutex> life(fdLifeMtx_);
        fd = clientFd_.exchange(kInvalidSocket);
        if (isValidSocket(fd)) closeHostSocketValue(fd);
    }
    {
        std::lock_guard<std::mutex> st(statusMtx_);
        peerText_.clear();
    }
    if (isValidSocket(fd))
        log().info("FujiNet", "SP-over-SLIP peer disconnected");
}

void SpTcpTransport::shutdown()
{
    // Wake, do not close: a descriptor closed under a thread parked in
    // recv()/accept() can be recycled by the next socket() on another thread.
    //
    // Under the LIFETIME lock, not the I/O lock: waiting on mtx_ here would
    // defeat the whole point (this is what wakes a parked reader), but without
    // any lock the close inside dropPeer()/stopListening() can land between
    // our load and the syscall.
    std::lock_guard<std::mutex> life(fdLifeMtx_);
    shutdownBoth(listenFd_.load());
    shutdownBoth(clientFd_.load());
}

std::string SpTcpTransport::describe() const
{
    // statusMtx_ only — see the header. Taking the I/O mutex here parked the
    // UI thread (and, through stateMutex, the CPU worker) behind every read
    // timeout of a peer that had connected but stopped answering.
    std::lock_guard<std::mutex> st(statusMtx_);
    if (!peerText_.empty()) return peerText_;
    if (isValidSocket(listenFd_.load()))
        return "listening on 127.0.0.1:" + std::to_string(port_);
    return "idle";
}

#endif // POM2_HAS_SOCKETS

} // namespace pom2
