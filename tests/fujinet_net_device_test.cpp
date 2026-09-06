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

// POM2's built-in `N:` against a stub HTTP server, in-process.
//
// No network dependency: the stub binds 127.0.0.1 on an ephemeral port, so
// this runs in CI. What it pins is what actually cost debugging time when the
// device was written against a real guest:
//
//   * The devicespec is NOT the whole control list. The guest sends
//     aux1 (open mode), aux2 (translation), THEN the spec — measured off the
//     wire from the FujiNet Contiki browser as
//     `04 00 4E 3A 68 74 74 70 ...`. Taking the list verbatim put two binary
//     bytes in front of the URL and every single open failed with an empty
//     host. That offset is the card's job, so what this file pins is the
//     parsing either side of it: "N:" prefix, scheme, host, port, path.
//   * Only the BODY reaches the guest. Guest-side browsers parse HTML, not
//     response headers.
//   * STATUS says "connected" while bytes remain and drops when they run
//     out — guest read loops end on that flag, so getting it wrong either
//     truncates the page or spins forever.
//   * The byte count is capped at 512, because guest code sizes its buffer
//     from it.
//   * THE FETCH IS NOT ON THE CALLER'S THREAD. `open()` starts it and
//     returns; the guest learns the verdict by polling STATUS, which is how
//     it drives `N:` anyway. `open()` is reached from a SmartPort call with
//     the emulator's state mutex held, so doing DNS + connect + a whole HTTP
//     body there froze the machine and the window together for up to 12 s.
//     Tests that want the verdict use `openAndSettle` below.

#include "FujiNetNetDevice.h"
#include "FujiNetNetwork.h"   // the kNetErr* bytes are contract, not detail

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;
void check(bool cond, const char* what)
{
    if (cond) std::printf("[ OK ] %s\n", what);
    else    { std::printf("FAIL: %s\n", what); ++failures; }
}

/// Serves one canned response, then closes — which is what HTTP/1.0 with
/// `Connection: close` asks for and what the device relies on to find EOF.
struct HttpStub {
    std::string  body;
    /// Answer with headers and HALF the body, then hold the connection open
    /// and say nothing more — a server that stalls mid-page, which is the
    /// case a per-recv timeout cannot bound.
    bool         stall = false;
    /// Send `body` VERBATIM, with no canned status line or headers. For the
    /// cases that are about what the device does with bytes it did not
    /// expect: a reply with no CRLFCRLF, and one over the size cap.
    bool         raw = false;
    int          listenFd = -1;
    uint16_t     port     = 0;
    std::thread  th;
    std::string  seenRequest;
    std::atomic<bool> done{false};
    std::atomic<bool> stopping{false};

    bool start()
    {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) return false;
        int one = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        socklen_t al = sizeof a;
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&a), &al) != 0) return false;
        port = ntohs(a.sin_port);
        if (::listen(listenFd, 2) != 0) return false;

        // `lfd` by value, not the member: stop() writes `listenFd = -1`
        // BEFORE joining (it has to — closing the listening socket is what
        // unblocks this accept()), so reading the member here is a plain
        // data race on an int, which the nightly TSan leg reported. The
        // thread gets its own copy; stop()'s close() still wakes it.
        const int lfd = listenFd;
        th = std::thread([this, lfd] {
            const int c = ::accept(lfd, nullptr, nullptr);
            if (c < 0) return;
            char buf[2048];
            const ssize_t r = ::recv(c, buf, sizeof buf - 1, 0);
            if (r > 0) { buf[r] = '\0'; seenRequest = buf; }
            std::string resp;
            if (!raw) {
                resp = "HTTP/1.0 200 OK\r\n"
                       "Content-Type: text/html\r\n"
                       "Server: stub\r\n\r\n";
            }
            resp += stall ? body.substr(0, body.size() / 2) : body;
            // Written in slices: a 600 KB reply will not fit one send() on
            // any platform, and a short write would silently shorten the
            // very case that is about length.
            for (std::size_t off = 0; off < resp.size(); ) {
                const ssize_t w = ::send(c, resp.data() + off, resp.size() - off, 0);
                if (w <= 0) break;
                off += static_cast<std::size_t>(w);
            }
            if (stall) {
                // Outlive the device's deadline without ever sending EOF.
                while (!stopping.load()) {
                    struct timespec ts{0, 20 * 1000 * 1000};
                    ::nanosleep(&ts, nullptr);
                }
            }
            ::close(c);
            done.store(true);
        });
        return true;
    }

    void stop()
    {
        stopping.store(true);
        if (listenFd >= 0) { ::shutdown(listenFd, SHUT_RDWR); ::close(listenFd); listenFd = -1; }
        if (th.joinable()) th.join();
    }
};

/// `open()` STARTS the fetch and returns — it runs on the CPU thread with the
/// emulator's state mutex held, so it may not do DNS, a connect and an HTTP
/// body there (see the async case at the end of this file). A test that wants
/// the VERDICT therefore waits for the fetch to settle first, which is the
/// same thing the guest does by polling STATUS.
bool openAndSettle(pom2::FujiNetNetDevice& net, const std::string& spec)
{
    if (!net.open(spec)) return false;        // refused outright: no fetch
    while (net.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return net.isOpen();
}

}  // namespace

int main()
{
    // ── A fetch, end to end ───────────────────────────────────────────────
    {
        HttpStub stub;
        stub.body = "<html><body>Hello Apple II</body></html>";
        if (!stub.start()) { std::printf("FAIL: cannot start the HTTP stub\n"); return 1; }

        pom2::FujiNetNetDevice net;
        const std::string spec = "N:HTTP://127.0.0.1:" + std::to_string(stub.port) + "/index.html";
        check(openAndSettle(net, spec), "opens N:HTTP://host:port/path");
        check(net.isOpen(), "reports itself open");

        // Only the body, never the headers.
        std::vector<uint8_t> got(net.available());
        const std::size_t n = net.read(got.data(), got.size());
        const std::string text(reinterpret_cast<const char*>(got.data()), n);
        check(text == stub.body, "hands the guest the BODY, headers stripped");

        // The request must carry a Host: header and the path as typed —
        // lower-casing the path would break case-sensitive servers.
        check(stub.seenRequest.find("GET /index.html ") != std::string::npos,
              "requests the path exactly as given");
        check(stub.seenRequest.find("Host: 127.0.0.1") != std::string::npos,
              "sends a Host: header");

        // Drained: connected must now be false, or a guest read loop spins.
        uint8_t st[4];
        net.status(st);
        check(st[0] == 0 && st[1] == 0, "no bytes waiting once drained");
        check(st[2] == 0, "connected clears when the body runs out");
        net.close();
        stub.stop();
    }

    // ── The 512-byte cap, and reading across it ───────────────────────────
    {
        HttpStub stub;
        stub.body.assign(1300, 'x');
        if (!stub.start()) { std::printf("FAIL: cannot start the HTTP stub\n"); return 1; }

        pom2::FujiNetNetDevice net;
        check(openAndSettle(net, "N:HTTP://127.0.0.1:" + std::to_string(stub.port) + "/"),
              "opens a larger document");
        check(net.available() == 1300, "buffers the whole body");

        uint8_t st[4];
        net.status(st);
        const uint16_t avail = static_cast<uint16_t>(st[0] | (st[1] << 8));
        check(avail == pom2::FujiNetNetDevice::kMaxStatusAvail,
              "status caps the announced count at 512");
        check(st[2] == 1, "connected while bytes remain");

        std::vector<uint8_t> all;
        uint8_t chunk[512];
        for (;;) {
            const std::size_t k = net.read(chunk, sizeof chunk);
            if (!k) break;
            all.insert(all.end(), chunk, chunk + k);
        }
        check(all.size() == 1300, "reads reassemble across the cap");
        check(std::string(all.begin(), all.end()) == stub.body, "content survives");
        stub.stop();
    }

    // ── Specs it must refuse rather than mangle ───────────────────────────
    {
        pom2::FujiNetNetDevice net;
        check(!net.open("N:HTTPS://example.invalid/"),
              "refuses https (no TLS here) instead of pretending");
        check(!net.open("N:FTP://example.invalid/"), "refuses an unknown scheme");
        check(!net.open("N:"), "refuses an empty spec");
    }

    // ── A stalled server must not become a half page ──────────────────────
    //
    // The fetch carries ONE deadline for the whole exchange. Two things are
    // pinned here: the deadline is honoured at all (a per-recv timeout never
    // bounds a server that drip-feeds just inside it), and what comes out is
    // an ERROR — never the bytes that did arrive. A truncated document the
    // guest cannot tell from a whole one is the failure nobody can diagnose
    // from the Apple II side.
    {
        HttpStub stub;
        stub.stall = true;
        stub.body.assign(4000, 'y');
        if (!stub.start()) { std::printf("FAIL: cannot start the HTTP stub\n"); return 1; }

        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(600);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = openAndSettle(
            net, "N:HTTP://127.0.0.1:" + std::to_string(stub.port) + "/");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();

        check(!ok, "a stalled server fails the open instead of succeeding short");
        check(ms < 5000, "the whole fetch is bounded by its deadline");
        check(net.available() == 0, "no partial body is left reachable after a failure");
        check(!net.isOpen(), "and the device does not report itself open");
        stub.stop();
    }

    // ── A blackholed host must not freeze the machine ─────────────────────
    //
    // 192.0.2.1 is TEST-NET-1: it swallows SYNs rather than refusing them.
    // SO_SNDTIMEO does NOT bound connect() — measured 2026-08-21 on macOS, a
    // connect asking for 8 s returned after 75. With the state mutex held that
    // is 75 s of frozen emulator, so the connect now gets an explicit wait.
    {
        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(800);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = openAndSettle(net, "N:HTTP://192.0.2.1/");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        check(!ok, "an unreachable host fails rather than hanging");
        // Generous on purpose: a host that RSTs instead of dropping fails
        // instantly and passes too. What must never happen is the OS default.
        check(ms < 30000, "a blackholed host is bounded by the deadline, not by the OS");
        if (ms >= 30000) std::printf("      (took %lld ms)\n", (long long)ms);
    }

    // ── open() ITSELF must not do the fetch ───────────────────────────────
    //
    // THE POINT OF THE ASYNC PATH. `open()` is reached from a SmartPort call,
    // on the CPU thread, with the emulator's state mutex held — the mutex the
    // CPU worker takes every 4096-cycle chunk and the UI thread takes to paint
    // every frame. A fetch there is up to `deadlineMs` of machine AND window
    // frozen together, with the FujiNet panel's own controls unreachable
    // because drawing them needs that same mutex. Against a blackholed host
    // that was the full connect budget, every time.
    {
        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(4000);
        const auto t0 = std::chrono::steady_clock::now();
        const bool started = net.open("N:HTTP://192.0.2.1/");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        check(started, "open() accepts a well-formed spec");
        // One PAL frame is 20 ms. 100 is the slack for a loaded CI box; what
        // must never happen again is the 4 s connect being paid here.
        check(ms < 100, "open() returns inside a frame, not after the connect");
        if (ms >= 100) std::printf("      (took %lld ms)\n", (long long)ms);
        check(net.busy(), "the fetch is in flight, not finished on this thread");
        check(net.isOpen(), "and the session counts as open while it runs");

        // What the guest sees meanwhile: connected, nothing waiting. That is
        // what a slow server looks like anyway, and it keeps a STATUS poll
        // loop polling instead of concluding the device is dead.
        uint8_t st[4];
        net.status(st);
        check(st[0] == 0 && st[1] == 0, "no bytes announced while fetching");
        check(st[2] == 1, "STATUS says connected while the fetch runs");
        check(net.read(st, 1) == 0, "and nothing is readable yet");

        while (net.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(!net.isOpen(), "the blackholed host settles as a failure");
        check(net.lastError() == pom2::kNetErrGeneral, "reported as GENERAL");
    }

    // ── Destroying the device mid-fetch must not wait for it ──────────────
    //
    // ~FujiNetCard runs inside SlotBus::plug(), with the machine lock held, so
    // a destructor that joined the fetch would be the freeze this whole change
    // exists to remove — just moved.
    {
        const auto t0 = std::chrono::steady_clock::now();
        {
            pom2::FujiNetNetDevice net;
            net.setFetchDeadlineMs(1000);
            check(net.open("N:HTTP://192.0.2.1/"), "start a fetch, then destroy it");
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        check(ms < 100, "the destructor abandons the fetch instead of joining it");
        if (ms >= 100) std::printf("      (took %lld ms)\n", (long long)ms);
        // Let the abandoned worker finish before main() returns: it holds only
        // its own shared block, but tidying up keeps the process exit quiet.
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    }

    // ── Specs that must be refused for their NUMBERS, not their scheme ────
    // The scheme cases above are refused by a string compare. These are the
    // ones the port parser has to get right, and a wrong answer here is a
    // socket opened against something the guest did not ask for.
    {
        pom2::FujiNetNetDevice net;
        check(!net.open("N:HTTP://host:0/"),      "refuses port 0");
        check(!net.open("N:HTTP://host:99999/"),  "refuses a port above 65535");
        check(!net.open("N:HTTP:///path"),        "refuses a spec with no host");
        check(net.lastError() == pom2::kNetErrGeneral,
              "a spec it cannot parse reports GENERAL");
        check(net.available() == 0, "and leaves nothing readable");
    }

    // ── A reply with no header terminator is passed through whole ─────────
    //
    // Deliberate, and pinned so it is not rediscovered as a bug. The split
    // looks for CRLFCRLF — what HTTP specifies and what the FujiNet
    // firmware's own N: looks for. A server that separates with bare LF
    // therefore hands the guest its headers too, which is preferable to
    // guessing at a boundary and silently eating part of a document.
    {
        HttpStub stub;
        stub.raw  = true;
        stub.body = "HTTP/1.0 200 OK\nContent-Type: text/plain\n\nhello";
        if (!stub.start()) { std::printf("FAIL: stub bind\n"); return 2; }
        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(4000);
        check(openAndSettle(net, "N:HTTP://127.0.0.1:" + std::to_string(stub.port) + "/"),
              "an LF-only reply still opens");
        std::vector<uint8_t> got(net.available());
        if (!got.empty()) net.read(got.data(), got.size());
        check(std::string(got.begin(), got.end()) == stub.body,
              "with no CRLFCRLF the whole reply becomes the body");
        stub.stop();
    }

    // ── A reply over the size cap is refused, not truncated ───────────────
    //
    // The guest has 128 KB of RAM and STATUS announces 512 bytes at a time,
    // so a runaway response must not grow POM2's heap without bound. Refused
    // for the same reason as the stalled server: half a document the guest
    // cannot tell from a whole one is the failure nobody can diagnose from
    // the Apple II side.
    {
        HttpStub stub;
        stub.raw  = true;
        stub.body = "HTTP/1.0 200 OK\r\n\r\n" + std::string(600u * 1024u, 'x');
        if (!stub.start()) { std::printf("FAIL: stub bind\n"); return 2; }
        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(10000);
        check(!openAndSettle(net,
                  "N:HTTP://127.0.0.1:" + std::to_string(stub.port) + "/"),
              "a reply over the 512 KB cap fails the open");
        check(net.lastError() == pom2::kNetErrGeneral, "and reports GENERAL");
        check(net.available() == 0, "and leaves no partial body reachable");
        stub.stop();
    }

    // ── Nothing listening: refused, and told apart from "no such host" ────
    //
    // The two error bytes are a contract with the guest, not an
    // implementation detail: FILE NOT FOUND is how the firmware's table
    // spells "no such host", and a guest that cannot tell a typo'd hostname
    // from a dead server has nothing to show the user.
    {
        HttpStub probe;              // bind, learn the port, then let it go
        if (!probe.start()) { std::printf("FAIL: stub bind\n"); return 2; }
        const uint16_t deadPort = probe.port;
        probe.stop();

        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(2000);
        check(!openAndSettle(net,
                  "N:HTTP://127.0.0.1:" + std::to_string(deadPort) + "/"),
              "a refused connection fails the open");
        check(net.lastError() == pom2::kNetErrGeneral,
              "a reachable host with nothing listening is GENERAL");
    }
    {
        // RFC 2606 reserves `.invalid` so it can never resolve.
        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(2000);
        const auto t0 = std::chrono::steady_clock::now();
        check(!openAndSettle(net, "N:HTTP://pom2-no-such-host.invalid/x"),
              "an unresolvable name fails the open");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        check(net.lastError() == pom2::kNetErrFileNotFound,
              "a name that will not resolve is FILE NOT FOUND, not GENERAL");
        check(ms < 8000, "and the lookup is bounded, not the resolver's business");
        if (ms >= 8000) std::printf("      (took %lld ms)\n", (long long)ms);
    }

    // ── close() puts a fetched body out of reach ──────────────────────────
    // available() and read() are public and do not consult open_, so this is
    // the only thing standing between a closed device and a stale page.
    {
        HttpStub stub;
        stub.body = "abcdef";
        if (!stub.start()) { std::printf("FAIL: stub bind\n"); return 2; }
        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(4000);
        check(openAndSettle(net, "N:HTTP://127.0.0.1:" + std::to_string(stub.port) + "/"),
              "fetch for the close() case");
        check(net.available() == stub.body.size(), "body is there before close");
        net.close();
        check(!net.isOpen(),        "close() clears open");
        check(net.available() == 0, "close() drops the body");
        uint8_t st[4];
        net.status(st);
        check(st[2] == 0, "and STATUS stops claiming a connection");
        stub.stop();
    }

    if (failures) { std::printf("fujinet_net_device: %d failure(s)\n", failures); return 2; }
    std::printf("fujinet_net_device OK\n");
    return 0;
}
