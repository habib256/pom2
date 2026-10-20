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

// Moved out of SuperSerialCard, not rewritten. Two disciplines in here are
// load-bearing and were each fixed after a real failure — the join-before-
// reassign in start() and the shutdown()-without-close() in stop(). Both are
// commented at their site; neither is incidental.

#include "SuperSerialTcpTransport.h"

#include "Logger.h"
#include "SocketCompat.h"
#include "SocketUtil.h"
#include "SuperSerialCard.h"
#include "SuperSerialTransport.h"
#include "ThreadGuard.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pom2 {
namespace {

class TcpTransport final : public SuperSerialTransport
{
public:
#if POM2_HAS_SOCKETS
    TcpTransport(SuperSerialCard& card, int slot) : card_(card), slot_(slot) {}
#else
    TcpTransport(SuperSerialCard&, int) {}
#endif

    ~TcpTransport() override { stop(); }

    bool start(uint16_t newPort) override
    {
#if !POM2_HAS_SOCKETS
        // No BSD sockets in the browser — the telnet bridge is unavailable.
        port_ = newPort;
        log().info("SSC", "telnet listener disabled in WASM build");
        return false;
#else
        if (listening_ && newPort == port_ && worker_.joinable()) return true;

        // Tear down any previous listener. This also covers a worker that
        // exited on its own (listen-socket error): it clears `listening_` on
        // its way out, but the dead std::thread must still be join()ed here —
        // assigning a new thread over a joinable member calls std::terminate —
        // and the stale listen fd must be closed before we bind a fresh one.
        // stop() is a no-op when nothing is live.
        stop();

        port_ = newPort;
        ensureSocketStack();     // Winsock needs WSAStartup; no-op elsewhere

        const socket_t lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        listenFd_ = lfd;
        if (!isValidSocket(lfd)) {
            log().warn("SSC", "socket() failed: " + lastSocketErrorText());
            return false;
        }
        // Re-bind after our own TIME_WAIT, but never let a second live process
        // steal the telnet port — the two are the SAME option on POSIX and
        // opposite ones on Winsock. SocketCompat.h, trap 6.
        setListenerBindPolicy(lfd);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons(port_);
        if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            log().warn("SSC", "bind 127.0.0.1:" + std::to_string(port_) +
                                  " failed: " + lastSocketErrorText());
            closeHostSocketValue(lfd);
            listenFd_ = kInvalidSocket;
            return false;
        }
        if (::listen(lfd, 1) != 0) {
            log().warn("SSC", "listen() failed: " + lastSocketErrorText());
            closeHostSocketValue(lfd);
            listenFd_ = kInvalidSocket;
            return false;
        }

        stopRequested_ = false;
        listening_     = true;
        // guardedThread, not std::thread: an exception escaping a thread
        // callable calls std::terminate with no log line — a crash nobody can
        // report or diagnose (CLAUDE.md, exception barrier).
        worker_ = guardedThread("SSC", [this] { runWorker(); });
        log().info("SSC", "listening on 127.0.0.1:" + std::to_string(port_) +
                              " (telnet to connect to slot " +
                              std::to_string(slot_) + ")");
        return true;
#endif
    }

    void stop() override
    {
#if !POM2_HAS_SOCKETS
        listening_ = false;
        return;
#else
        if (!listening_ && !worker_.joinable()) return;
        stopRequested_ = true;
        // Wake the worker out of recv()/accept() WITHOUT close()-ing the fds
        // under it: close + recv on the same fd from two threads is a
        // use-after-close / fd-recycle hazard. shutdown() only half-closes.
        // The actual close() of the client fd is the worker's job, on its exit
        // path; the listen fd is closed here only AFTER join(), so nothing can
        // recv()/accept() a recycled descriptor.
        {
            std::lock_guard<std::mutex> life(fdLifeMtx_);
            shutdownBoth(clientFd_.load());
            shutdownBoth(listenFd_.load());
        }
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard<std::mutex> life(fdLifeMtx_);
            closeHostSocketValue(listenFd_.exchange(kInvalidSocket));
            // The worker closed the client fd on exit; exchange makes this a
            // no-op rather than a double close.
            closeHostSocketValue(clientFd_.exchange(kInvalidSocket));
        }
        listening_ = false;
#endif
    }

    bool isListening() const override { return listening_; }
    uint16_t port() const override { return port_; }

private:
#if POM2_HAS_SOCKETS
    void closeClient()
    {
        // exchange() guarantees exactly one close even from two paths.
        std::lock_guard<std::mutex> life(fdLifeMtx_);
        const socket_t fd = clientFd_.exchange(kInvalidSocket);
        if (isValidSocket(fd)) {
            shutdownBoth(fd);
            closeHostSocketValue(fd);
        }
    }

    void runWorker()
    {
        while (!stopRequested_) {
            sockaddr_in peer{};
            socket_t fd = kInvalidSocket;
            const auto pa = pollAcceptOnce(listenFd_.load(), 200, fd, peer);
            if (pa == PollAccept::Retry)    continue;
            if (pa == PollAccept::Shutdown) break;

            disableSigpipe(fd);
            setSockOptInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
            setNonBlocking(fd);

            {
                std::lock_guard<std::mutex> life(fdLifeMtx_);
                if (stopRequested_) {
                    closeHostSocketValue(fd);
                    break;
                }
                clientFd_ = fd;
            }
            card_.onTransportConnected();
            log().info("SSC", std::string("client connected from ") +
                                  peerAddressText(peer));

            std::vector<uint8_t> pendingOut;
            uint8_t scratch[256];
            while (!stopRequested_ && isValidSocket(clientFd_.load())) {
                const bool raw = card_.rawMode();

                const iolen_t got =
                    recvSocket(clientFd_, scratch, sizeof(scratch));
                if (got > 0) {
                    size_t n = static_cast<size_t>(got);
                    // Raw mode skips filtering entirely — that IS raw mode.
                    if (!raw) n = card_.processTransportTextRx(scratch, n);
                    card_.deliverTransportBytes(scratch, n, !raw);
                } else if (got == 0) {
                    log().info("SSC", "client disconnected");
                    break;
                } else if (!errWouldBlock(lastSocketError())) {
                    log().info("SSC", "recv error: " + lastSocketErrorText());
                    break;
                }

                // Only refill once the previous batch is on the wire: the
                // pacing budget accrues against wall time, and taking more
                // before the last write drained would burst past the rate.
                if (pendingOut.empty()) (void)card_.drainTransportTx(pendingOut);

                if (!pendingOut.empty() && isValidSocket(clientFd_.load())) {
                    const iolen_t sent = sendNoSignal(
                        clientFd_, pendingOut.data(), pendingOut.size());
                    const int sendErr = (sent < 0) ? lastSocketError() : 0;
                    if (sent > 0) {
                        pendingOut.erase(pendingOut.begin(),
                                         pendingOut.begin() + sent);
                    } else if (sent < 0 && (errWouldBlock(sendErr) ||
                                            errInterrupted(sendErr))) {
                        // Retry next tick with the batch intact.
                    } else {
                        log().info("SSC",
                                   "send error: " + socketErrorText(sendErr));
                        break;
                    }
                }

                std::this_thread::sleep_for(std::chrono::microseconds(2000));
            }
            closeClient();
            card_.onTransportDisconnected();
        }
        listening_ = false;
    }
#endif // POM2_HAS_SOCKETS

    uint16_t         port_ = 0;
    std::atomic<bool> listening_{ false };
#if POM2_HAS_SOCKETS
    SuperSerialCard& card_;
    int              slot_ = 0;
    std::atomic<bool> stopRequested_{ false };
    // Atomic so stop() can shutdown() these to wake the worker, while the
    // worker remains the sole closer of the client fd.
    std::atomic<socket_t> listenFd_{ kInvalidSocket };
    std::atomic<socket_t> clientFd_{ kInvalidSocket };
    std::mutex            fdLifeMtx_;
    std::thread           worker_;
#endif
};

} // namespace

std::unique_ptr<SuperSerialTransport>
makeSuperSerialTcpTransport(SuperSerialCard& card, int slot)
{
    return std::make_unique<TcpTransport>(card, slot);
}

} // namespace pom2
