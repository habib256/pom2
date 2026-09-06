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

#include "FujiNetNetDevice.h"

#include "Logger.h"
#include "ThreadGuard.h"
#include "SocketUtil.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <cstring>

#ifndef _WIN32
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

// No host sockets under Emscripten: the browser build has no TCP to offer, so
// the whole fetch path compiles out and the device politely refuses. The
// guest sees an open that fails, which is the truth there.
#if !POM2_HAS_SOCKETS

namespace pom2 {

FujiNetNetDevice::~FujiNetNetDevice() = default;

void FujiNetNetDevice::fetchInto(Fetch& out, std::string, uint16_t,
                                 std::string, int)
{
    std::lock_guard<std::mutex> lk(out.mtx);
    out.error = kNetErrGeneral;
}

void FujiNetNetDevice::harvest() {}

bool FujiNetNetDevice::open(const std::string& devicespec)
{
    close();
    error_       = kNetErrGeneral;
    description_ = devicespec + " — no host network in this build";
    log().warn("FujiNet", "built-in N: unavailable — this build has no host sockets");
    return false;
}

void FujiNetNetDevice::close()
{
    open_ = false;
    body_.clear();
    cursor_ = 0;
}

void FujiNetNetDevice::status(uint8_t out[4]) const
{
    out[0] = out[1] = out[2] = 0;
    out[3] = error_;
}

std::size_t FujiNetNetDevice::read(uint8_t*, std::size_t) { return 0; }

}  // namespace pom2

#else   // POM2_HAS_SOCKETS

namespace pom2 {

namespace {

/// Error codes in the firmware's numbering, which guest code compares
/// against directly (network_data.h NDEV_STATUS).
// The error bytes are part of the `N:` contract (FujiNetNetwork.h), not of
// this implementation — a fake N: has to report the same ones.
constexpr uint8_t kErrSuccess      = kNetErrSuccess;
constexpr uint8_t kErrGeneral      = kNetErrGeneral;
constexpr uint8_t kErrFileNotFound = kNetErrFileNotFound;

std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/// Split "N:HTTP://host:port/path" into its parts. The guest usually shouts
/// the whole thing in upper case, and the HOST half of a URL is
/// case-insensitive — but the PATH is not, so only the scheme and host get
/// folded and the path is passed through exactly as typed.
bool parseSpec(const std::string& spec, std::string& host, uint16_t& port,
               std::string& path)
{
    std::string s = spec;
    // Strip the "N:" (or "N1:".."N8:") device prefix.
    const std::string u = upper(s);
    if (u.size() >= 2 && u[0] == 'N') {
        const std::size_t colon = s.find(':');
        if (colon != std::string::npos && colon <= 2) s = s.substr(colon + 1);
    }
    const std::string us = upper(s);
    if (us.rfind("HTTP://", 0) == 0)       s = s.substr(7);
    else if (us.rfind("TCP://", 0) == 0)   s = s.substr(6);
    else if (us.rfind("HTTPS://", 0) == 0) return false;   // no TLS, see header
    else if (us.find("://") != std::string::npos) return false;

    const std::size_t slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : s.substr(slash);
    if (path.empty()) path = "/";

    port = 80;
    const std::size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        const long p = std::strtol(hostport.c_str() + colon + 1, nullptr, 10);
        if (p <= 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
        hostport = hostport.substr(0, colon);
    }
    // Host names are case-insensitive; lower-casing keeps the Host: header
    // conventional rather than SHOUTING at the server.
    for (char& c : hostport) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    host = hostport;
    return !host.empty();
}


// ── Bounded host I/O ──────────────────────────────────────────────────────
//
// This used to run on the CPU thread INSIDE a SmartPort call, with the
// emulation worker holding the state mutex for the whole slice
// (EmulationController.cpp, `runCpuSlice` under `stateMtx`) — so a fetch was
// up to 12 s of frozen machine, frozen window and an unreachable FujiNet
// panel. It runs on its own worker now (see `open`), and the deadline stayed:
// a fetch that cannot end is a socket and a thread that never go away, and a
// guest polling STATUS deserves an answer either way.
//
// Every wait is ALSO sliced, so `cancel` — set when the device is destroyed
// under the machine lock — is noticed within a slice rather than at the end
// of the budget.
//
// SO_SNDTIMEO/SO_RCVTIMEO are not enough for the deadline, twice over:
//   * They do not bound connect(). Measured 2026-08-21 on macOS against
//     192.0.2.1 (TEST-NET-1, swallows SYNs): connect() returned after 75 s
//     with the option asking for 8. That is 75 s of frozen emulator.
//   * A per-recv timeout bounds each call, never the transfer. A server
//     drip-feeding one byte just inside the timeout keeps the loop alive
//     forever — an unbounded freeze, not a slow page.
constexpr int kConnectTimeoutMs = 5000;

/// How long a single wait may last before the cancel flag is re-read. Short
/// enough that a card unplugged mid-fetch stops touching the network almost
/// at once, long enough to cost nothing (one extra poll() per slice).
constexpr int kWaitSliceMs = 100;

/// A guard, not a policy: the guest has 128 KB of RAM and STATUS can only
/// announce 512 bytes at a time, so a runaway response must not grow POM2's
/// heap without bound.
constexpr std::size_t kMaxBody = 512u * 1024u;

using SteadyPoint = std::chrono::steady_clock::time_point;

/// Milliseconds left before `deadline`, never negative.
int msLeft(SteadyPoint deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - now).count());
}

}  // namespace

FujiNetNetDevice::~FujiNetNetDevice() { close(); }

void FujiNetNetDevice::fetchInto(Fetch& out, std::string host, uint16_t port,
                                 std::string path, int deadlineMs)
{
    /// Publish a verdict into the shared block. `done` is set by the caller
    /// of fetchInto, AFTER this has run, so a harvest can never see half a
    /// result.
    const auto finishFetch = [&out](bool ok, uint8_t error,
                                    std::vector<uint8_t> body = {}) {
        std::lock_guard<std::mutex> lk(out.mtx);
        out.ok    = ok;
        out.error = error;
        out.body  = std::move(body);
    };

    const SteadyPoint deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);

    addrinfo* res = nullptr;
    if (!resolveBounded(host, std::to_string(port), msLeft(deadline), &res)) {
        // The guest reads FILE NOT FOUND as "no such host".
        finishFetch(false, kErrFileNotFound);
        log().warn("FujiNet", "built-in N: cannot resolve \"" + host + "\"");
        return;
    }

    socket_t fd = kInvalidSocket;
    for (addrinfo* a = res; a; a = a->ai_next) {
        if (out.cancel.load()) break;
        const int budget = std::min(kConnectTimeoutMs, msLeft(deadline));
        if (connectBounded(a, budget, fd)) break;
    }
    ::freeaddrinfo(res);
    if (!isValidSocket(fd)) { finishFetch(false, kErrGeneral); return; }

    // HTTP/1.0 on purpose: it ends the body at EOF, so there is no chunked
    // transfer-encoding to unpick. `Connection: close` says the same thing to
    // a server that answers 1.1 anyway.
    const std::string req = "GET " + path + " HTTP/1.0\r\n"
                            "Host: " + host + "\r\n"
                            "User-Agent: POM2-FujiNet/1.0\r\n"
                            "Connection: close\r\n\r\n";

    // The socket is non-blocking, so a send can be short or refuse outright.
    std::size_t sent = 0;
    while (sent < req.size()) {
        const int left = msLeft(deadline);
        if (left <= 0 || out.cancel.load()) {
            closeHostSocketValue(fd);
            finishFetch(false, kErrGeneral);
            return;
        }
        const WaitResult ww = waitSocket(fd, SocketWait::Write,
                                         std::min(left, kWaitSliceMs));
        if (ww == WaitResult::Timeout) continue;      // deadline re-checked
        if (ww != WaitResult::Ready) {
            closeHostSocketValue(fd);
            finishFetch(false, kErrGeneral);
            return;
        }
        const iolen_t w = sendNoSignal(fd, req.data() + sent, req.size() - sent);
        if (w > 0) { sent += static_cast<std::size_t>(w); continue; }
        const int e = lastSocketError();
        if (!errWouldBlock(e) && !errInterrupted(e)) {
            closeHostSocketValue(fd);
            finishFetch(false, kErrGeneral);
            return;
        }
    }

    std::vector<uint8_t> raw;
    uint8_t buf[4096];
    bool truncated = false;
    for (;;) {
        const int left = msLeft(deadline);
        if (left <= 0) { truncated = true; break; }
        if (out.cancel.load()) { truncated = true; break; }
        const WaitResult wr = waitSocket(fd, SocketWait::Read,
                                         std::min(left, kWaitSliceMs));
        if (wr == WaitResult::Timeout) continue;      // deadline re-checked
        if (wr != WaitResult::Ready) { truncated = true; break; }

        const iolen_t r = ::recv(fd, reinterpret_cast<char*>(buf), sizeof buf, 0);
        if (r == 0) break;                  // clean EOF — the body is complete
        if (r < 0) {
            const int e = lastSocketError();
            if (errWouldBlock(e) || errInterrupted(e)) continue;
            truncated = true;
            break;
        }
        raw.insert(raw.end(), buf, buf + static_cast<std::size_t>(r));
        if (raw.size() > kMaxBody) { truncated = true; break; }
    }
    closeHostSocketValue(fd);

    // A short read is NOT a short page. Handing the guest half a document it
    // cannot tell from a whole one is the one failure nobody can diagnose from
    // the Apple II side, so say so instead.
    if (truncated) {
        finishFetch(false, kErrGeneral);
        log().warn("FujiNet", "built-in N: incomplete response from " + host + path +
                              " — " + std::to_string(raw.size()) +
                              " bytes before the deadline or the size cap; refusing to"
                              " hand the guest a half page");
        return;
    }

    // Hand the guest the BODY only. Guest-side browsers parse HTML, not
    // response headers, and the FujiNet's own N: does the same split.
    static const uint8_t kCrLfCrLf[4] = { '\r', '\n', '\r', '\n' };
    auto it = std::search(raw.begin(), raw.end(), kCrLfCrLf, kCrLfCrLf + 4);
    std::vector<uint8_t> body;
    if (it != raw.end()) body.assign(it + 4, raw.end());
    else                 body = std::move(raw);   // headerless: pass it through

    finishFetch(true, kErrSuccess, std::move(body));
}

void FujiNetNetDevice::harvest()
{
    if (!fetch_) return;

    bool                 ok  = false;
    uint8_t              err = kErrGeneral;
    std::vector<uint8_t> body;
    {
        std::lock_guard<std::mutex> lk(fetch_->mtx);
        if (!fetch_->done) return;             // still in flight
        ok  = fetch_->ok;
        err = fetch_->error;
        body.swap(fetch_->body);
    }
    fetch_.reset();

    open_   = ok;
    error_  = err;
    cursor_ = 0;
    // available()/read() are public and do not consult open_; a failed fetch
    // must not leave partial bytes reachable.
    if (ok) body_ = std::move(body);
    else    body_.clear();

    description_ = spec_ + (ok ? " — " + std::to_string(body_.size()) + " B"
                               : " — failed");
    log().info("FujiNet", std::string("built-in N: ") +
                          (ok ? "fetched " : "failed ") + spec_ +
                          (ok ? " (" + std::to_string(body_.size()) + " bytes)"
                              : ""));
}

bool FujiNetNetDevice::open(const std::string& devicespec)
{
    close();

    std::string host, path;
    uint16_t port = 80;
    if (!parseSpec(devicespec, host, port, path)) {
        error_ = kErrGeneral;
        description_ = devicespec + " — unsupported (HTTP over TCP only)";
        log().warn("FujiNet", "built-in N: cannot open \"" + devicespec +
                              "\" — only HTTP over plain TCP is served here");
        return false;
    }

    // Off the CPU thread — see the header. The worker is DETACHED and holds
    // its own reference to the shared block, so nothing here ever waits for
    // it: the device can be destroyed (which it is, under the machine lock,
    // whenever the card is unplugged) while the fetch is still running.
    spec_        = devicespec;
    error_       = kErrSuccess;   // nothing has gone wrong yet
    description_ = devicespec + " — fetching";
    auto f = std::make_shared<Fetch>();
    fetch_ = f;
    const int budget = deadlineMs_;
    std::thread([f, host, port, path, budget] {
        pom2::runGuarded("FujiNetN", [&] {
            fetchInto(*f, host, port, path, budget);
        });
        // Outside runGuarded on purpose: an exception that escaped the fetch
        // must still end it, or a guest polls STATUS for ever.
        std::lock_guard<std::mutex> lk(f->mtx);
        f->done = true;
    }).detach();
    return true;
}

void FujiNetNetDevice::close()
{
    // Abandon anything in flight rather than waiting for it: the worker sees
    // the flag at its next slice and tidies up after itself.
    if (fetch_) { fetch_->cancel.store(true); fetch_.reset(); }
    open_ = false;
    body_.clear();
    cursor_ = 0;
}

void FujiNetNetDevice::status(uint8_t out[4]) const
{
    pump();
    if (fetch_) {
        // The fetch is still running. "Connected, nothing waiting" is what a
        // slow server looks like anyway, and it is what keeps a guest's poll
        // loop polling instead of concluding the device is dead.
        out[0] = out[1] = 0;
        out[2] = 1;
        out[3] = kErrSuccess;
        return;
    }
    const std::size_t avail = std::min<std::size_t>(available(), kMaxStatusAvail);
    out[0] = static_cast<uint8_t>(avail & 0xFF);
    out[1] = static_cast<uint8_t>((avail >> 8) & 0xFF);
    // "Connected" stays true while there is still body to hand over: guest
    // loops read until this clears, and dropping it early truncates the page.
    out[2] = (open_ && available() > 0) ? 1 : 0;
    out[3] = error_;
}

std::size_t FujiNetNetDevice::read(uint8_t* dst, std::size_t n)
{
    harvest();
    const std::size_t take = std::min(n, available());
    if (take) {
        std::memcpy(dst, body_.data() + cursor_, take);
        cursor_ += take;
    }
    return take;
}

}  // namespace pom2

#endif  // POM2_HAS_SOCKETS
