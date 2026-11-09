// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Independent 6502 software drives the production CPU, Memory, SlotBus,
// Uthernet II and built-in NAT. No Apple ROM or Internet service is needed.
// Fixtures/provenance: fixtures/uthernet2/README.md. The utility binaries
// were extracted from the upstream ProDOS disk and match its DOS 3.3 disk.

#include "BuiltinNatBackend.h"
#include "M6502.h"
#include "Memory.h"
#include "SocketCompat.h"
#include "TestTimeScale.h"
#include "UthernetIICard.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace pom2;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// A nonblocking loopback peer, serviced on the test thread. All loops have
// wall-clock bounds in Machine::until, including accept and short writes.
class Server {
public:
    Server()
    {
        ensureSocketStack();
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(isValidSocket(listener_), "socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(setListenerBindPolicy(listener_), "listener bind policy");
        require(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "bind loopback");
        require(::listen(listener_, 4) == 0, "listen");
        socklen_c length = sizeof(address);
        require(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0,
                "getsockname");
        port = ntohs(address.sin_port);
        require(setNonBlocking(listener_), "nonblocking listener");
    }
    ~Server()
    {
        if (isValidSocket(peer_)) closeHostSocket(peer_);
        if (isValidSocket(listener_)) closeHostSocket(listener_);
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void pump()
    {
        if (!isValidSocket(peer_)) {
            peer_ = ::accept(listener_, nullptr, nullptr);
            if (!isValidSocket(peer_)) return;
            require(setNonBlocking(peer_), "nonblocking peer");
            received.clear();
            ++connections;
            if (onAccept) onAccept();
        }
        char buffer[4096];
        const auto count = recvSocket(peer_, buffer, sizeof(buffer));
        if (count > 0) {
            const std::string chunk(buffer, static_cast<size_t>(count));
            received += chunk;
            if (echo) outgoing += chunk;
            if (onRead) onRead();
        } else if (count == 0) {
            closeHostSocket(peer_);
            peer_ = kInvalidSocket;
            return;
        } else {
            require(errWouldBlock(lastSocketError()) || errInterrupted(lastSocketError()),
                    "receive from guest");
        }
        if (!outgoing.empty()) {
            const auto sent = sendSocket(peer_, outgoing.data(), outgoing.size());
            if (sent > 0) outgoing.erase(0, static_cast<size_t>(sent));
            else require(sent < 0 && (errWouldBlock(lastSocketError()) ||
                                     errInterrupted(lastSocketError())), "send to guest");
        }
        if (finish && outgoing.empty()) {
            // Let the guest consume the response and observe a clean FIN.
#ifdef _WIN32
            ::shutdown(peer_, SD_SEND);
#else
            ::shutdown(peer_, SHUT_WR);
#endif
            finish = false;
        }
    }
    uint16_t port = 0;
    int connections = 0;
    bool echo = false, finish = false;
    std::string received, outgoing;
    std::function<void()> onAccept, onRead;
private:
    socket_t listener_ = kInvalidSocket, peer_ = kInvalidSocket;
};

class Machine {
public:
    Machine(const std::string& file, uint16_t entry, int slot) : cpu(&memory)
    {
        // The upstream standalone IP65 fixture has no C runtime/BSS clear;
        // its harness starts with zeroed RAM, unlike a POM2 power-on pattern.
        for (unsigned address = 0; address < 0xC000; ++address)
            memory.writeRamUnchecked(static_cast<uint16_t>(address), 0);
        // Same minimal Monitor service as upstream: RTS-filled ROM except
        // Apple's WAIT ($FCA8), which IP65 uses for its timer calibration.
        // Do not use Memory's flat test mode: it bypasses the slot bus.
        std::array<uint8_t, 0x3000> rom;
        rom.fill(0x60);
        constexpr uint8_t wait[] = {0x38,0x48,0xE9,0x01,0xD0,0xFC,
                                   0x68,0xE9,0x01,0xD0,0xF6,0x60};
        std::copy(std::begin(wait), std::end(wait), rom.begin() + 0x2CA8);
        require(memory.loadRomBytes(rom.data(), rom.size(), 0xD000), "Monitor stub");
        std::ifstream input(file, std::ios::binary);
        require(static_cast<bool>(input), "open fixture " + file);
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
        require(!bytes.empty() && entry + bytes.size() <= 0xC000, "fixture size");
        for (size_t i = 0; i < bytes.size(); ++i)
            memory.writeRamUnchecked(static_cast<uint16_t>(entry + i), bytes[i]);
        BuiltinNatOptions options;
        // Only this test's private backend opts into the local test server.
        options.allowHostLoopback = true;
        auto card = std::make_unique<UthernetIICard>(slot);
        card->setBackend(makeBuiltinNatBackend(options));
        memory.slotBus().plug(slot, std::move(card));
        memory.setCpu(&cpu);
        cpu.setCpuMode(M6502::CpuMode::NMOS);
        cpu.setProgramCounter(entry);
        cpu.setStackPointer(0xFF);
    }

    void until(const std::function<bool()>& done, Server& server, const char* what)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(20 * pom2test::timeScale());
        while (!done()) {
            require(cpu.getProgramCounter() != 0x03D0, std::string("unexpected exit: ") + what);
            if (Clock::now() >= deadline) {
                char state[100];
                std::snprintf(state, sizeof(state), " PC=$%04X stage=$%02X A=$%02X\n",
                              cpu.getProgramCounter(), memory.data()[0x0303], cpu.getAccumulator());
                throw std::runtime_error(std::string("timeout: ") + what + state + screen());
            }
            cpu.run(1000);
            server.pump();
            // Real host sockets and DNS run in wall time. Keep IP65's
            // instruction-counted timer near the II+'s 1.0205 MHz clock.
            const auto target = started_ + std::chrono::microseconds(
                static_cast<int64_t>(memory.getCycleCounter() * 1000000.0 / 1020500.0));
            std::this_thread::sleep_until(target);
        }
    }

    std::string screen() const
    {
        // Both utilities keep their 80x24 character backing store at $0800,
        // independently of the Soft-70 bitmap they draw in Hi-Res memory.
        std::string result;
        for (unsigned row = 0; row < 24; ++row) {
            for (unsigned col = 0; col < 80; ++col) {
                const auto c = memory.data()[0x0800 + row * 80 + col];
                result += c >= 32 && c < 127 ? static_cast<char>(c) : ' ';
            }
            result += '\n';
        }
        return result;
    }
    bool contains(const std::string& s) const { return screen().find(s) != std::string::npos; }
    void type(const std::string& text, Server& server)
    {
        for (unsigned char key : text) {
            until([&] { return (memory.keyLatchPeek() & 0x80) == 0; }, server, "keyboard ready");
            memory.queueKey(key);
            until([&] { return (memory.keyLatchPeek() & 0x80) == 0; }, server, "key consumed");
        }
    }
    Memory memory;
    M6502 cpu;
private:
    const Clock::time_point started_ = Clock::now();
};

std::string octets(std::initializer_list<unsigned> bytes)
{
    std::string s;
    for (unsigned b : bytes) s += static_cast<char>(b);
    return s;
}

void ip65(const std::string& fixture)
{
    Server server;
    server.echo = true;
    Machine m(fixture, 0x0800, 3);
    m.memory.writeRamUnchecked(0x0300, 3);
    m.memory.writeRamUnchecked(0x0301, static_cast<uint8_t>(server.port));
    m.memory.writeRamUnchecked(0x0302, static_cast<uint8_t>(server.port >> 8));
    unsigned lastStage = 0;
    m.until([&] {
        const unsigned stage = m.memory.data()[0x0303];
        if (stage != lastStage) {
            std::printf("IP65 stage %u\n", stage);
            lastStage = stage;
        }
        return stage == 0xFF;
    }, server, "IP65 DHCP/DNS/TCP echo (stage in $0303)");
    const auto* ram = m.memory.data();
    require(std::equal(ram + 0x0310, ram + 0x0314,
                      std::array<uint8_t,4>{10,0,2,15}.begin()), "DHCP lease");
    require(std::equal(ram + 0x0314, ram + 0x0318,
                      std::array<uint8_t,4>{127,0,0,1}.begin()), "localhost DNS answer");
    const std::string message = "HELLO FROM THE APPLE II+";
    require(std::equal(message.begin(), message.end(), ram + 0x0320) &&
            ram[0x0320 + message.size()] == 0, "TCP echo in guest RAM");
    require(server.received == message, "TCP bytes received by host");
    std::puts("IP65: DHCP 10.0.2.15, DNS localhost, TCP echo OK");
}

void telnet(const std::string& fixture)
{
    Server server;
    server.onAccept = [&] {
        server.outgoing = octets({255,253,24,255,253,31,255,251,1,255,251,3,
                                  255,250,24,1,255,240});
        server.outgoing += "\x1b[2J\x1b[HPOM2 TEST BBS\r\n\x1b[5;10Hhere\x1b[6n";
    };
    Machine m(fixture, 0x4000, 1);
    m.until([&] { return m.contains("Connect to host"); }, server, "Telnet DHCP and prompt");
    require(m.contains("10.0.2.15"), "Telnet DHCP lease on screen");
    m.type("10.0.2.2:" + std::to_string(server.port) + "\r", server);
    m.until([&] { return m.contains("POM2 TEST BBS") &&
                        server.received.find("\x1b[5;14R") != std::string::npos; },
            server, "Telnet BBS and ANSI cursor reply");
    require(server.received.find("VT100") != std::string::npos, "Telnet terminal type");
    require(server.received.find(octets({255,250,31,0,80,0,24,255,240})) != std::string::npos,
            "Telnet window size 80x24");
    require(m.screen().substr(4 * 81 + 9, 4) == "here", "ANSI cursor positioning");
    m.type("123\r", server);
    m.until([&] { return server.received.find("123\r") != std::string::npos; },
            server, "Telnet keyboard to server");
    server.finish = true;
    m.until([&] { return m.contains("Connection closed."); }, server, "Telnet remote FIN");
    std::puts("TELNET: DHCP, negotiation VT100/80x24, ANSI, keyboard, remote close OK");
}

void gopher(const std::string& fixture)
{
    Server server;
    bool answered = false;
    std::vector<std::string> selectors;
    server.onAccept = [&] { answered = false; };
    server.onRead = [&] {
        if (answered || server.received.find("\r\n") == std::string::npos) return;
        answered = true;
        const auto selector = server.received.substr(0, server.received.find("\r\n"));
        selectors.push_back(selector);
        if (selector.empty()) {
            server.outgoing = "iPOM2 GOPHER MENU\t\terror.host\t1\r\n"
                "0Read this document\t/about\t10.0.2.2\t" + std::to_string(server.port) + "\r\n.\r\n";
        } else {
            require(selector == "/about", "Gopher selector");
            for (int i = 1; i <= 30; ++i)
                server.outgoing += "POM2 document line " + std::to_string(i) + "\r\n";
            server.outgoing += ".\r\n";
        }
        server.finish = true;
    };
    Machine m(fixture, 0x4000, 1);
    m.until([&] { return m.contains("Go to host"); }, server, "Gopher DHCP and prompt");
    require(m.contains("10.0.2.15"), "Gopher DHCP lease on screen");
    m.type("10.0.2.2:" + std::to_string(server.port) + "\r", server);
    m.until([&] { return m.contains("POM2 GOPHER MENU") && m.contains("Item _"); },
            server, "Gopher menu");
    m.type("1\r", server);
    m.until([&] { return m.contains("POM2 document line 23") && m.contains("Page "); },
            server, "Gopher document first page");
    m.type(" ", server);
    m.until([&] { return m.contains("POM2 document line 30") && m.contains("[end]"); },
            server, "Gopher document second page");
    require(selectors == std::vector<std::string>{"", "/about"}, "Gopher requests on host");
    std::puts("GOPHER: DHCP, menu, selection, document, paging OK");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 3, "usage: test_uthernet2_ip65 <ip65|telnet|gopher> <fixture.bin>");
        const std::string mode = argv[1];
        if (mode == "ip65") ip65(argv[2]);
        else if (mode == "telnet") telnet(argv[2]);
        else if (mode == "gopher") gopher(argv[2]);
        else throw std::runtime_error("unknown test mode");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
