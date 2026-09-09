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

// AI control server — the `/mem` bank parameter (bug hunt #16).
//
// Two defects, one parameter, both silent behind a 200:
//
//   1. Anything that was not exactly "aux" fell through to the main bank.
//      `bank=lc`, `bank=AUX`, a typo — all answered 200 with another bank's
//      bytes. Every sibling endpoint rejects an unknown value (`/speed`'s
//      preset, `/reset`'s kind); this one guessed.
//
//   2. `main` is the RAW 64 KiB array, so $D000-$FFFF is always the ROM
//      image and Language-Card RAM was UNREACHABLE — no value of `bank`
//      could read it. A //e spends most of its life executing out of LC RAM
//      (ProDOS, Pascal, Applesoft extensions), and the Debugger panel and
//      the Memory viewer both show LC RAM at that address
//      (`snapshotCpuView`). So the agent's view of $D000 and the GUI's view
//      of the same address disagreed, with nothing anywhere to say so.
//
// The fix adds `bank=cpu` — `Memory::peekCpuView`, the side-effect-free
// resolution of ALTZP / RAMRD / 80STORE / the two Language-Card latches that
// the two panels already list from — and turns an unknown bank into a 400.
// `main` and `aux` are unchanged, which is what the assertions below on the
// ROM byte and the aux byte are for.
//
// Writes stay bank-EXPLICIT (bug hunt #7): `POST /mem?bank=cpu` is refused
// rather than routed through whatever paging the guest happens to hold.

#include "AiControlServer.h"
#include "Apple2Display.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

namespace {

uint16_t kPort = 0;

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

struct Rsp { int status = 0; std::string body; };

Rsp request(const std::string& raw)
{
    int fd = -1;
    for (int attempt = 0; attempt < 40; ++attempt) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons(kPort);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            break;
        ::close(fd);
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(fd >= 0 && "loopback connect failed");
    size_t sent = 0;
    while (sent < raw.size()) {
        const ssize_t n = ::send(fd, raw.data() + sent, raw.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    std::string in;
    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        in.append(buf, buf + n);
    }
    ::close(fd);
    Rsp r;
    const size_t sp = in.find(' ');
    if (sp != std::string::npos) r.status = std::atoi(in.c_str() + sp + 1);
    const size_t he = in.find("\r\n\r\n");
    if (he != std::string::npos) r.body = in.substr(he + 4);
    return r;
}

Rsp get(const std::string& path)
{
    return request("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
}

Rsp post(const std::string& path, const std::string& body)
{
    char head[512];
    std::snprintf(head, sizeof(head),
                  "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Content-Length: %zu\r\n\r\n",
                  path.c_str(), body.size());
    return request(std::string(head) + body);
}

bool contains(const std::string& h, const std::string& n)
{
    return h.find(n) != std::string::npos;
}

}  // namespace

int main()
{
    EmulationController ctrl;
    Apple2Display display;
    ctrl.setMode(EmulationController::Mode::Stopped);

    pom2::AiControlServer srv;
    srv.attach(&ctrl, &display, nullptr, nullptr);
    kPort = pickFreePort();
    assert(kPort != 0 && "could not obtain an ephemeral loopback port");
    assert(srv.start(kPort) && "AiControlServer failed to bind its test port");

    // ── The machine: a //e with a known ROM byte at $D000 and a DIFFERENT
    // byte written into Language-Card RAM underneath it, plus a main/aux
    // pair at $0400 that differ.
    {
        auto st = ctrl.lockState();
        Memory& mem = st.memory();
        mem.setIIEMode(true);
        const uint8_t rom[1] = { 0xC9 };
        mem.loadRomBytes(rom, 1, 0xD000);
        // $C08B read twice: LC bank 1, read RAM + write enable.
        mem.memRead(0xC08B);
        mem.memRead(0xC08B);
        mem.memWrite(0xD000, 0x55);          // lands in LC RAM, not in mem[]
        assert(mem.peekCpuView(0xD000) == 0x55 &&
               "harness: the CPU view must show LC RAM here");
        assert(mem.data()[0xD000] == 0xC9 &&
               "harness: the raw main array must still show the ROM byte");

        mem.writeRamUnchecked(0x0400, 0x11); // main
        mem.memWrite(0xC005, 0);             // RAMWRT on
        mem.memWrite(0x0400, 0x22);          // aux
        mem.memWrite(0xC004, 0);             // RAMWRT off
    }

    // ── 1. `bank=main` (and the default) are UNCHANGED: the raw array.
    Rsp r = get("/mem?addr=0xD000&len=1");
    assert(r.status == 200);
    assert(contains(r.body, "\"bank\":\"main\""));
    assert(contains(r.body, "\"data\":\"C9\"") &&
           "the default bank must still be the raw main array");

    r = get("/mem?addr=0x400&len=1&bank=main");
    assert(r.status == 200 && contains(r.body, "\"data\":\"11\""));

    // ── 2. `bank=aux` is UNCHANGED.
    r = get("/mem?addr=0x400&len=1&bank=aux");
    assert(r.status == 200);
    assert(contains(r.body, "\"bank\":\"aux\""));
    assert(contains(r.body, "\"data\":\"22\""));

    // ── 3. THE case: `bank=cpu` reaches Language-Card RAM — the byte the
    // Debugger panel and the Memory viewer show at that address.
    r = get("/mem?addr=0xD000&len=1&bank=cpu");
    assert(r.status == 200 && "bank=cpu must be accepted");
    assert(contains(r.body, "\"bank\":\"cpu\""));
    assert(contains(r.body, "\"data\":\"55\"") &&
           "bank=cpu must read Language-Card RAM, not the ROM image");

    // …and it resolves the aux paging the same way: with RAMRD set the CPU
    // reads $0400 out of aux, so `cpu` must say $22 where `main` says $11.
    {
        auto st = ctrl.lockState();
        st.memory().memWrite(0xC003, 0);     // RAMRD on
    }
    r = get("/mem?addr=0x400&len=1&bank=cpu");
    assert(r.status == 200 && contains(r.body, "\"data\":\"22\"") &&
           "bank=cpu must follow RAMRD");
    {
        auto st = ctrl.lockState();
        st.memory().memWrite(0xC002, 0);     // RAMRD off
    }

    // ── 4. An unknown bank is a 400, never a silent "main". This is the half
    // that made the endpoint lie: `bank=lc` used to answer 200 with the ROM
    // byte and a body that named a bank the caller had not asked for.
    for (const char* bad : { "lc", "AUX", "Main", "0", "cpu2" }) {
        r = get(std::string("/mem?addr=0xD000&len=1&bank=") + bad);
        assert(r.status == 400 && "an unknown GET bank must be refused");
        assert(contains(r.body, "bank must be main|aux|cpu"));
    }

    // ── 5. Writes stay bank-EXPLICIT: main and aux only, `cpu` refused, an
    // unknown value refused (it used to be written to main behind a 200 that
    // named main while the caller had asked for something else).
    r = post("/mem?addr=0x300&bank=aux", "{\"data\":\"7E\"}");
    assert(r.status == 200 && contains(r.body, "\"bank\":\"aux\""));
    r = post("/mem?addr=0x300&bank=main", "{\"data\":\"7F\"}");
    assert(r.status == 200 && contains(r.body, "\"bank\":\"main\""));
    {
        auto st = ctrl.lockState();
        assert(st.memory().data()[0x0300] == 0x7F);
        assert(st.memory().auxData()[0x0300] == 0x7E);
    }
    r = post("/mem?addr=0x300&bank=cpu", "{\"data\":\"01\"}");
    assert(r.status == 400 && "a write must not be routed through guest paging");
    r = post("/mem?addr=0x300&bank=lc", "{\"data\":\"01\"}");
    assert(r.status == 400 && "an unknown POST bank must be refused");
    {
        auto st = ctrl.lockState();
        assert(st.memory().data()[0x0300] == 0x7F &&
               "a refused write must not have landed anywhere");
    }

    srv.stop();
    std::puts("ai_control_mem_bank_test: OK");
    return 0;
}
