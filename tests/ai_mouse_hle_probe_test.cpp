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

// AI control /mouse endpoint vs the AppleWin-HLE mouse card.
//
// Regression pinned: AiControlServer::handleMouse probed the SlotBus with
// `dynamic_cast<MouseCard*>` only. MouseCardAppleWin is a SIBLING class
// (not derived from MouseCard) and is the default built-in mouse on every
// //c profile — so POST /mouse answered 503 "no Mouse Card plugged" on the
// machines that always have a mouse. The endpoint now also recognises the
// HLE card (by its kCardName tag — see the comment in AiControlServer.cpp
// for why not dynamic_cast) and drives its setHostMouse.
//
// Split out of ai_control_server_smoke_test.cpp because this binary must
// additionally link MouseCardAppleWin.cpp (+ its MC6821 PIA), which the
// main smoke test deliberately doesn't.

#include "AiControlServer.h"
#include "EmulationController.h"
#include "Memory.h"
#include "MouseCardAppleWin.h"
#include "SlotBus.h"

#include <memory>

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

// Chosen at run time (bind :0, read it back, close) rather than hard-coded:
// a fixed port collides with a second ctest job on the same runner. See the
// same helper in ai_control_server_smoke_test.cpp.
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

int connectLoopback(uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
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

HttpResponse postMouse(const std::string& body)
{
    char req[512];
    std::snprintf(req, sizeof(req),
        "POST /mouse HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        body.size(), body.c_str());
    return oneShot(kTestPort, req);
}

}  // namespace

int main()
{
    EmulationController ctrl;
    ctrl.setMode(EmulationController::Mode::Stopped);

    pom2::AiControlServer srv;
    srv.attach(&ctrl, nullptr, nullptr, nullptr);
    kTestPort = pickFreePort();
    assert(kTestPort != 0 && "could not obtain an ephemeral loopback port");
    const bool started = srv.start(kTestPort);
    assert(started && "AiControlServer failed to bind its ephemeral test port");

    // Empty bus → 503 (same shape as the LLE-card case).
    HttpResponse r = postMouse("{\"dx\":1}");
    assert(r.status == 503 && contains(r.body, "no Mouse Card"));

    // Plug the AppleWin-HLE variant at the //c's built-in mouse slot. No
    // ROM needed: the endpoint stores host position via setHostMouse only.
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ctrl.memory().slotBus().plug(4, std::make_unique<MouseCardAppleWin>(4));
    }

    // The endpoint must FIND the HLE card (used to 503 here) and run the
    // same accumulator protocol as for the LLE card.
    r = postMouse("{\"dx\":40,\"dy\":10}");
    assert(r.status == 200);
    assert(contains(r.body, "\"x\":40") && contains(r.body, "\"y\":10"));
    assert(contains(r.body, "\"slot\":4"));
    r = postMouse("{\"x\":12,\"y\":34,\"btn\":1}");
    assert(r.status == 200);
    assert(contains(r.body, "\"x\":12") && contains(r.body, "\"y\":34"));
    assert(contains(r.body, "\"btn\":1"));
    r = postMouse("{\"reset\":1}");
    assert(contains(r.body, "\"x\":0") && contains(r.body, "\"y\":0"));

    srv.stop();
    std::printf("OK ai_mouse_hle_probe\n");
    return 0;
}
