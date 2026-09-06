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

// AI Control HTTP server smoke test — pins the request/response cycle for
// the AiControlServer that lets external agents drive POM2.
//
// What this gates:
//   * The TCP listener accepts a loopback connection and answers a basic
//     `GET /status` with HTTP 200 + JSON containing `"ok":true`, `"cpu"`,
//     and `"profile"` keys.
//   * Auth: when a token is set, an unauthenticated request gets 401 and
//     a request carrying the right `X-POM2-Token` header gets 200.
//   * Memory bridge round-trip: `POST /mem` writes a byte at $0300 and
//     `GET /mem` reads it back.
//   * Reset: `POST /reset {"kind":"hard"}` clears the PC's high byte to a
//     plausible reset-vector destination ($F800 fallback when no ROM).
//   * 404 on unknown endpoints; 405 on wrong methods.
//
// Why this matters: the AI control endpoint is the single integration
// point an external agent (Claude Code MCP bridge, a curl-driven CI step,
// etc.) leans on. Regressions in the HTTP shape break every agent at
// once, so the contract gets pinned line-by-line.

#include "AiControlServer.h"
#include "Apple2Display.h"
#include "EmulationController.h"
#include "Memory.h"
#include "MouseCard.h"
#include "SlotBus.h"
#include "SnapshotIO.h"

#include <memory>

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

// The port is chosen at run time, never hard-coded: a fixed 36502 collided
// with the second ctest job on a shared runner (and with anything a dev box
// happened to have on that port), turning a busy machine into a red suite.
// Bind :0, read back what the kernel handed out, close, and hand that number
// to the server. The window between close and re-bind is theoretical on a
// loopback-only test and the alternative — teaching AiControlServer to
// report an ephemeral port — is a src/ change this fix does not need.
uint16_t kTestPort = 0;

uint16_t pickFreePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    uint16_t port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
            port = ntohs(addr.sin_port);
    }
    ::close(fd);
    return port;
}

// Connect to 127.0.0.1:port; returns the socket fd or -1 on failure.
int connectLoopback(uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
    // Retry the connect a handful of times — the server's worker thread
    // may not have called listen() yet on a fast first request after
    // start().
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fd);
    return -1;
}

bool sendAll(int fd, const std::string& s)
{
    size_t sent = 0;
    while (sent < s.size()) {
        const ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Read until the peer closes. The server sets Connection: close on every
// response, so this is the correct termination condition.
std::string drainAll(int fd)
{
    std::string out;
    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
    }
    return out;
}

struct HttpResponse {
    int status;
    std::string body;
};

HttpResponse parseResponse(const std::string& raw)
{
    HttpResponse r{0, {}};
    const size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) return r;
    // "HTTP/1.1 200 OK"
    const size_t sp1 = raw.find(' ');
    if (sp1 == std::string::npos || sp1 + 1 >= raw.size()) return r;
    r.status = std::atoi(raw.c_str() + sp1 + 1);
    const size_t headersEnd = raw.find("\r\n\r\n");
    if (headersEnd != std::string::npos) {
        r.body = raw.substr(headersEnd + 4);
    }
    return r;
}

HttpResponse oneShot(uint16_t port, const std::string& request)
{
    const int fd = connectLoopback(port);
    assert(fd >= 0 && "loopback connect failed");
    const bool sent = sendAll(fd, request);
    assert(sent && "send failed");
    const std::string raw = drainAll(fd);
    ::close(fd);
    return parseResponse(raw);
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

void testStatusEndpoint(EmulationController& ctrl, pom2::AiControlServer& srv)
{
    srv.setAuthToken("");   // open
    srv.setProfileLabel("Test Profile");
    (void)ctrl;
    const HttpResponse r = oneShot(kTestPort,
        "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 200);
    assert(contains(r.body, "\"ok\":true"));
    assert(contains(r.body, "\"cpu\""));
    assert(contains(r.body, "\"profile\":\"Test Profile\""));
    std::puts("  status: OK");
}

void testAuth(EmulationController& /*ctrl*/, pom2::AiControlServer& srv)
{
    // Token-less access remains available to native localhost tools, but a
    // cross-origin browser page must not inherit that trust implicitly.
    srv.setAuthToken("");
    HttpResponse r = oneShot(kTestPort,
        "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Origin: https://attacker.invalid\r\n\r\n");
    assert(r.status == 401);

    srv.setAuthToken("s3cret");

    // No header → 401.
    r = oneShot(kTestPort,
        "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 401);
    assert(contains(r.body, "\"ok\":false"));

    // Wrong token → 401.
    r = oneShot(kTestPort,
        "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nX-POM2-Token: nope\r\n\r\n");
    assert(r.status == 401);

    // Right token → 200.
    r = oneShot(kTestPort,
        "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nX-POM2-Token: s3cret\r\n\r\n");
    assert(r.status == 200);
    assert(contains(r.body, "\"ok\":true"));

    srv.setAuthToken("");
    std::puts("  auth: OK");
}

void testMemoryRoundtrip(EmulationController& ctrl, pom2::AiControlServer& /*srv*/)
{
    // POST /mem?addr=0x0300 {"data":"AB"} → /mem?addr=0x0300&len=1 reads "AB".
    const std::string writeBody = "{\"data\":\"AB\"}";
    char writeReq[512];
    std::snprintf(writeReq, sizeof(writeReq),
        "POST /mem?addr=0x0300 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        writeBody.size(), writeBody.c_str());
    HttpResponse r = oneShot(kTestPort, writeReq);
    assert(r.status == 200);
    assert(contains(r.body, "\"written\":1"));

    // Confirm via direct memory inspection that the byte actually landed
    // through Memory::memWrite (RAM at $0300 is unprotected user space).
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        assert(ctrl.memory().data()[0x0300] == 0xAB);
    }

    r = oneShot(kTestPort,
        "GET /mem?addr=0x0300&len=1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 200);
    assert(contains(r.body, "\"data\":\"AB\""));

    // The endpoint is a RAM editor.  It must not claim success for writes
    // which Memory::memWrite rejects because $D000-$FFFF is ROM by default.
    const std::string romBody = "{\"data\":\"00\"}";
    char romReq[512];
    std::snprintf(romReq, sizeof(romReq),
        "POST /mem?addr=0xD000 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        romBody.size(), romBody.c_str());
    r = oneShot(kTestPort, romReq);
    assert(r.status == 400);

    std::puts("  memory roundtrip: OK");
}

void testReset(EmulationController& /*ctrl*/, pom2::AiControlServer& /*srv*/)
{
    const std::string body = "{\"kind\":\"hard\"}";
    char req[512];
    std::snprintf(req, sizeof(req),
        "POST /reset HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    const HttpResponse r = oneShot(kTestPort, req);
    assert(r.status == 200);
    assert(contains(r.body, "\"kind\":\"hard\""));
    std::puts("  reset: OK");
}

// ─── /snapshot path-safety regression gate ───────────────────────────────
// AiControlServer canonicalises caller-supplied paths and requires them
// to stay under the emulator's working directory before opening / writing
// the file. This pins the rejection branch: a compromised agent (or a
// browser hijacked via DNS rebinding into the localhost listener) must
// not be able to read or overwrite arbitrary host files via /snapshot.
// The /disk path uses the same helper but requires a plugged DiskIICard,
// which the test fixture intentionally leaves out — exercising the
// snapshot path covers the shared code.
void testSnapshotPathSafety(EmulationController& /*ctrl*/, pom2::AiControlServer& /*srv*/)
{
    auto post = [](const std::string& target, const std::string& body) {
        char req[1024];
        std::snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n\r\n%s",
            target.c_str(), body.size(), body.c_str());
        return oneShot(kTestPort, req);
    };

    // /snapshot/save now requires a `.pom2snap` extension so an agent can't
    // overwrite unrelated files (ROMs, configs, disk images) inside cwd. A
    // path with a wrong extension → 400 before path-safety even runs.
    HttpResponse rExt = post("/snapshot/save",
        "{\"path\":\"roms/apple2e.rom\"}");
    assert(rExt.status == 400);
    assert(contains(rExt.body, ".pom2snap"));

    // /snapshot/save with an absolute path outside cwd → 403 rejected.
    // /etc exists on every Linux host so weakly_canonical resolves cleanly;
    // the cwd prefix check is what must fire (after the extension gate).
    HttpResponse r = post("/snapshot/save",
        "{\"path\":\"/etc/pom2_should_never_write_here.pom2snap\"}");
    assert(r.status == 403);
    assert(contains(r.body, "path rejected"));

    // /snapshot/load on the same kind of path → 403. Load keeps the
    // permissive any-extension policy (it can't damage anything — only
    // reads, and the magic-byte check rejects non-snapshots).
    r = post("/snapshot/load", "{\"path\":\"/etc/passwd\"}");
    assert(r.status == 403);

    // /snapshot/load on a non-existent file under cwd → 403 (mustExist
    // branch — weakly_canonical succeeds but is_regular_file fails).
    r = post("/snapshot/load",
        "{\"path\":\"this_file_does_not_exist_pom2.pom2snap\"}");
    assert(r.status == 403);

    // Happy path: a relative `.pom2snap` path under cwd is accepted. Save,
    // observe the file landed, clean up. Verifies the path-safety guard
    // isn't over-rejecting and that the canonical path is what the
    // response reports.
    namespace fs = std::filesystem;
    const std::string relName = "test_path_safety_snapshot.pom2snap";
    const fs::path expected = fs::weakly_canonical(fs::current_path() / relName);
    fs::remove(expected);   // best-effort cleanup from a prior aborted run
    r = post("/snapshot/save", "{\"path\":\"" + relName + "\"}");
    assert(r.status == 200);
    assert(fs::exists(expected));
    assert(contains(r.body, expected.string()));
    fs::remove(expected);

    // R4-#3: a symlink under cwd whose target is OUTSIDE cwd must be
    // rejected for save — weakly_canonical returns it lexically (inside
    // cwd) but ofstream would follow it out of the jail. Use `.pom2snap`
    // so the extension gate doesn't pre-empt the symlink check.
    {
        const fs::path outside =
            fs::temp_directory_path() / "pom2_symlink_escape_target.pom2snap";
        const std::string linkName = "pom2_evil_link.pom2snap";
        const fs::path link = fs::current_path() / linkName;
        std::error_code ec;
        fs::remove(outside, ec);
        fs::remove(link, ec);
        fs::create_symlink(outside, link, ec);
        if (!ec) {                       // skip if the platform lacks symlinks
            HttpResponse rs = post("/snapshot/save",
                "{\"path\":\"" + linkName + "\"}");
            assert(rs.status == 403);
            assert(!fs::exists(outside)); // nothing written through the link
            fs::remove(link, ec);
            fs::remove(outside, ec);
        }
    }

    // An embedded NUL splits the check from the write: every guard here runs
    // on a std::string (which holds the NUL) while the write underneath goes
    // through c_str() (which stops at it). So
    // `"roms/apple2e.rom\0.pom2snap"` passed the ".pom2snap" extension gate
    // and the file that got the snapshot bytes was the ROM.
    // (Bug hunt 2026-09-06 #H3.)
    {
        std::string body = "{\"path\":\"roms/apple2e.rom";
        body.push_back('\0');
        body += ".pom2snap\"}";
        const std::string req =
            "POST /snapshot/save HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        const HttpResponse rn = oneShot(kTestPort, req);
        assert(rn.status == 400);
        assert(contains(rn.body, "control byte"));
    }

    std::puts("  snapshot path-safety: OK");
}

void testNotFoundAndMethod(EmulationController& /*ctrl*/, pom2::AiControlServer& /*srv*/)
{
    HttpResponse r = oneShot(kTestPort,
        "GET /nope HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 404);

    // /reset rejects GET.
    r = oneShot(kTestPort,
        "GET /reset HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 405);
    std::puts("  not-found/method: OK");
}

void testSpeed(EmulationController& ctrl, pom2::AiControlServer& /*srv*/)
{
    const std::string body = "{\"preset\":\"2x\"}";
    char req[512];
    std::snprintf(req, sizeof(req),
        "POST /speed HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    const HttpResponse r = oneShot(kTestPort, req);
    assert(r.status == 200);
    assert(ctrl.getCyclesPerFrame() == 34090);

    // `payload`, not `body`: the enclosing scope already has a `body` and
    // shadowing it here made the lambda read as if it reused that one.
    auto postSpeed = [](const std::string& payload) {
        char rq[512];
        std::snprintf(rq, sizeof(rq),
            "POST /speed HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: %zu\r\n\r\n%s",
            payload.size(), payload.c_str());
        return oneShot(kTestPort, rq);
    };

    // R4-#1: out-of-range cycles_per_frame is REJECTED, not cast to a
    // garbage int that freezes/pauses the emulator. State stays unchanged.
    HttpResponse rr = postSpeed("{\"cycles_per_frame\":9999999999}");
    assert(rr.status == 400);
    assert(ctrl.getCyclesPerFrame() == 34090);
    rr = postSpeed("{\"cycles_per_frame\":0}");
    assert(rr.status == 400);
    // A valid in-range value is accepted.
    rr = postSpeed("{\"cycles_per_frame\":50000}");
    assert(rr.status == 200);
    assert(ctrl.getCyclesPerFrame() == 50000);

    // R4-#2: jsonGetString must match the key at an object-key position, not
    // inside another field's VALUE. "label"'s value is the quoted string
    // "cycles_per_frame"; the real numeric field must still be found.
    rr = postSpeed("{\"label\":\"cycles_per_frame\",\"cycles_per_frame\":17045}");
    assert(rr.status == 200);
    assert(ctrl.getCyclesPerFrame() == 17045);

    std::puts("  speed: OK");
}

// Regression: applyProfile() / restartEmulationFromSettings() rebuild the
// machine with stop()…tear down cards…start() while the CPU worker thread
// is ALREADY running. start() then can't re-spawn the thread, so it must
// re-arm the run mode itself — otherwise the machine stays Stopped after a
// profile or slot-config switch and freezes on an uncleared ("@"-tile
// garbage) text page. Because a saved non-default profile auto-applies on
// startup, that surfaced as "the emulator doesn't boot on launch". Pin that
// start() resumes Running on BOTH the cold path (no worker yet) and the hot
// path (worker already live) that the profile switch actually hits.
void testStartResumesMode(EmulationController& ctrl)
{
    ctrl.stop();
    assert(ctrl.getMode() == EmulationController::Mode::Stopped);
    ctrl.start();   // cold path: spawns the worker AND arms Running
    assert(ctrl.getMode() == EmulationController::Mode::Running &&
           "start() must resume Running after stop()");
    ctrl.stop();
    assert(ctrl.getMode() == EmulationController::Mode::Stopped);
    ctrl.start();   // hot path: worker already joinable — must STILL arm Running
    assert(ctrl.getMode() == EmulationController::Mode::Running &&
           "start() must resume Running even when the worker is already "
           "live (the applyProfile/restart frozen-on-switch regression)");
    ctrl.stop();    // park again so the worker idles quietly until teardown
    std::puts("  start-resumes-mode: OK");
}

void testStopInterruptsBlockedResponse(pom2::AiControlServer& srv)
{
    const int fd = connectLoopback(kTestPort);
    assert(fd >= 0);
    int tiny = 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &tiny, sizeof(tiny));
    assert(sendAll(fd,
        "GET /screen.ppm HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    // Deliberately never read the ~600 KiB response. stop() must shutdown
    // the accepted client, not merely the listener, before joining.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto before = std::chrono::steady_clock::now();
    srv.stop();
    const auto elapsed = std::chrono::steady_clock::now() - before;
    assert(elapsed < std::chrono::seconds(1));
    ::close(fd);
    std::puts("  stop interrupts blocked response: OK");
}

// Round 10 #3: the CPU section is a fixed 16 bytes (PC2 + 6 regs + cycles8).
// The reader consumes 16 unconditionally, so a CPU section declaring fewer
// bytes must be REJECTED — the old `len>=9` gate processed a short section,
// reading up to 7 bytes past it (garbage cycle counter / CPU mode) from a
// crafted/truncated snapshot reachable over the localhost API.
void testSnapshotCpuSectionGate(EmulationController& /*ctrl*/, pom2::AiControlServer& /*srv*/)
{
    namespace fs = std::filesystem;
    auto post = [](const std::string& target, const std::string& body) {
        char req[1024];
        std::snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            target.c_str(), body.size(), body.c_str());
        return oneShot(kTestPort, req);
    };
    auto getReq = [](const std::string& target) {
        char req[256];
        std::snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", target.c_str());
        return oneShot(kTestPort, req);
    };

    // Craft a snapshot whose CPU section is only 12 bytes (< 16) and encodes a
    // sentinel PC = $1234 in its first two bytes.
    const std::string relName = "test_cpu_section_gate.snap";
    const fs::path path = fs::weakly_canonical(fs::current_path() / relName);
    fs::remove(path);
    {
        pom2::SnapshotWriter w(path.string());
        assert(w.good());
        const uint8_t cpu12[12] = {0x34, 0x12};   // PC lo/hi = $1234; rest zero
        w.writeSection("CPU", cpu12, sizeof(cpu12));
    }

    // Set a known live PC ($0300), then load the crafted file.
    HttpResponse r = post("/cpu", "{\"pc\":768}");
    assert(r.status == 200);
    r = post("/snapshot/load", "{\"path\":\"" + relName + "\"}");
    // 400, not 200: the short CPU section is skipped, which leaves this
    // file with NOTHING restorable — reporting success there was the
    // silent-half-restore bug (fixed 2026-07-30). The endpoint now says
    // so; the important part is still that the machine is untouched.
    assert(r.status == 400);

    // The short CPU section must be SKIPPED → PC stays $0300 (768), NOT the
    // sentinel $1234 (4660). The old gate set PC from the under-length section.
    r = getReq("/cpu");
    assert(r.status == 200);
    assert(contains(r.body, "\"pc\":768") &&
           "short CPU section must be skipped (PC unchanged)");
    assert(!contains(r.body, "\"pc\":4660") &&
           "short CPU section must NOT set PC past its declared length");

    fs::remove(path);
    std::puts("  snapshot CPU-section gate: OK");
}

// POST /cpu must honour a/x/y/p/sp as well as pc — the M6502 setters exist
// (M6502.h:133-137, added for snapshot restore) but the endpoint used to
// silently ignore everything except pc.
void testCpuRegisterSet(EmulationController& /*ctrl*/, pom2::AiControlServer& /*srv*/)
{
    auto post = [](const std::string& target, const std::string& body) {
        char req[1024];
        std::snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            target.c_str(), body.size(), body.c_str());
        return oneShot(kTestPort, req);
    };

    HttpResponse r = post("/cpu",
        "{\"pc\":4096,\"a\":17,\"x\":34,\"y\":51,\"p\":52,\"sp\":253}");
    assert(r.status == 200);

    r = oneShot(kTestPort, "GET /cpu HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 200);
    assert(contains(r.body, "\"pc\":4096"));
    assert(contains(r.body, "\"a\":17"));
    assert(contains(r.body, "\"x\":34"));
    assert(contains(r.body, "\"y\":51"));
    assert(contains(r.body, "\"p\":52"));
    assert(contains(r.body, "\"sp\":253"));

    // Partial update: only the supplied keys change.
    r = post("/cpu", "{\"a\":255}");
    assert(r.status == 200);
    r = oneShot(kTestPort, "GET /cpu HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(contains(r.body, "\"a\":255"));
    assert(contains(r.body, "\"x\":34"));
    assert(contains(r.body, "\"sp\":253"));

    std::puts("  cpu register set: OK");
}

// Pins the POST /mouse endpoint: card lookup via the SlotBus, the 8-bit
// running counter (mirrors MainWindow::onMouseMove), ±127 per-call clamp,
// absolute set, reset, and the 503/405 error shapes. This is the headless
// driver used to verify mouse apps end-to-end (it closed the "X stuck" item).
void testMouseEndpoint(EmulationController& ctrl, pom2::AiControlServer& /*srv*/)
{
    auto postMouse = [](const std::string& body) {
        char req[512];
        std::snprintf(req, sizeof(req),
            "POST /mouse HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body.size(), body.c_str());
        return oneShot(kTestPort, req);
    };

    // No Mouse Card plugged yet → 503.
    HttpResponse r = postMouse("{\"dx\":1}");
    assert(r.status == 503 && contains(r.body, "no Mouse Card"));

    // Plug a Mouse Card in slot 4. No ROMs needed: the endpoint finds it via
    // the SlotBus and calls setHostMouse, which just stores the host position.
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ctrl.memory().slotBus().plug(4, std::make_unique<MouseCard>(4));
    }

    // GET is rejected (POST only).
    r = oneShot(kTestPort, "GET /mouse HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 405);

    // Relative deltas accumulate into an 8-bit running counter; the response
    // echoes the counter and the slot the card was found in.
    r = postMouse("{\"dx\":40,\"dy\":10}");
    assert(r.status == 200);
    assert(contains(r.body, "\"x\":40") && contains(r.body, "\"y\":10"));
    assert(contains(r.body, "\"slot\":4"));
    r = postMouse("{\"dx\":40,\"dy\":10}");
    assert(contains(r.body, "\"x\":80") && contains(r.body, "\"y\":20"));

    // Per-call delta clamps to +127 (MCU 8-bit signed wrap window): 80+127=207.
    r = postMouse("{\"dx\":999}");
    assert(contains(r.body, "\"x\":207"));

    // Absolute set overrides the accumulator; button flag echoes.
    r = postMouse("{\"x\":12,\"y\":34,\"btn\":1}");
    assert(contains(r.body, "\"x\":12") && contains(r.body, "\"y\":34"));
    assert(contains(r.body, "\"btn\":1"));

    // reset zeroes the running counter.
    r = postMouse("{\"reset\":1}");
    assert(contains(r.body, "\"x\":0") && contains(r.body, "\"y\":0"));

    std::puts("  mouse: OK");
}

} // namespace

int main()
{
    EmulationController ctrl;
    Apple2Display display;
    // Don't actually start the CPU worker — these tests prod state in
    // place and don't need cycles being consumed underneath the assertions.
    ctrl.setMode(EmulationController::Mode::Stopped);

    pom2::AiControlServer srv;
    srv.attach(&ctrl, &display, nullptr, nullptr);
    kTestPort = pickFreePort();
    assert(kTestPort != 0 && "could not obtain an ephemeral loopback port");
    const bool started = srv.start(kTestPort);
    assert(started && "AiControlServer failed to bind its ephemeral test port");

    testStatusEndpoint   (ctrl, srv);
    testAuth             (ctrl, srv);
    testMemoryRoundtrip  (ctrl, srv);
    testReset            (ctrl, srv);
    testSpeed            (ctrl, srv);
    testSnapshotPathSafety(ctrl, srv);
    testSnapshotCpuSectionGate(ctrl, srv);
    testCpuRegisterSet   (ctrl, srv);
    testNotFoundAndMethod(ctrl, srv);
    testMouseEndpoint    (ctrl, srv);
    testStartResumesMode (ctrl);   // last: it spawns the CPU worker thread

    testStopInterruptsBlockedResponse(srv);
    std::puts("ai_control_server_smoke_test: OK");
    return 0;
}
