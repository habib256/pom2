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

// The FujiNet USB-serial transport over a pseudo-terminal (TODO G5-13).
//
// `serial_port` pins the SerialPort underneath; `SpSerialTransport` — the
// SpTransport a FujiNet board is reached through — had no test at all. The
// pty's master plays the board:
//   1. an unopenable path is refused with a reason, and nothing is open;
//   2. pollForPeer opens the slave, and a second poll does not reopen it;
//   3. writeAll reaches the board, the board's bytes reach readSome;
//   4. readSome with nothing to read returns 0 after about its timeout;
//   5. shutdown() makes a parked readSome return -1 promptly (the serial
//      line has no socket to shut, so it must notice the flag itself);
//   6. dropPeer closes, and a new poll reopens.

#include "SpTransport.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>

namespace {

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    else       std::printf("  ok: %s\n", what);
}

long msSince(std::chrono::steady_clock::time_point t0)
{
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());
}

std::string masterReads(int fd, std::size_t want, int ms)
{
    std::string got;
    const auto t0 = std::chrono::steady_clock::now();
    char buf[64];
    while (got.size() < want && msSince(t0) < ms) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) got.append(buf, static_cast<std::size_t>(n));
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return got;
}

}  // namespace

int main()
{
    // 1. a path that is not there
    {
        pom2::SpSerialTransport t("/dev/pom2-no-such-tty", 115200);
        expect(!t.pollForPeer(10), "an absent device is not opened");
        expect(!t.isOpen() && !t.lastError().empty(), "…and the reason is kept");
    }

    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        std::printf("SKIP: no pseudo-terminal available\n");
        return 77;
    }
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    const std::string slave = ::ptsname(master);

    pom2::SpSerialTransport t(slave, 115200);
    // 2. open
    expect(t.pollForPeer(10) && t.isOpen(), "pollForPeer opens the tty");
    expect(!t.pollForPeer(10), "a second poll does not reopen it");
    expect(t.describe() == slave + " @ 115200",
           "describe() names the open device and its rate");

    // 3. both directions
    const uint8_t out[] = { 0xC0, 0x01, 0xDB, 0xDC, 0xC0 };   // SLIP-shaped
    expect(t.writeAll(out, sizeof(out)), "writeAll succeeds");
    expect(masterReads(master, sizeof(out), 2000) ==
           std::string(reinterpret_cast<const char*>(out), sizeof(out)),
           "the board receives the frame byte for byte");
    const char in[] = "\xC0\x42\xC0";
    ::write(master, in, 3);
    uint8_t buf[16];
    int got = 0;
    for (int tries = 0; tries < 20 && got < 3; ++tries) {
        const int r = t.readSome(buf + got, sizeof(buf) - static_cast<std::size_t>(got), 100);
        if (r > 0) got += r;
    }
    expect(got == 3 && std::memcmp(buf, in, 3) == 0,
           "the board's bytes reach readSome");

    // 4. timeout
    auto t0 = std::chrono::steady_clock::now();
    const int idle = t.readSome(buf, sizeof(buf), 150);
    const long waited = msSince(t0);
    expect(idle == 0 && waited >= 100 && waited < 1500,
           "readSome with nothing to read returns 0 after about its timeout");

    // 5. shutdown wakes a parked reader
    auto parked = std::async(std::launch::async, [&] {
        const auto s = std::chrono::steady_clock::now();
        const int r = t.readSome(buf, sizeof(buf), 5000);
        return std::make_pair(r, msSince(s));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    t.shutdown();
    const auto [r, ms] = parked.get();
    expect(r < 0 && ms < 1000, "shutdown() makes a parked readSome return -1 promptly");

    // 6. drop and reopen (a fresh transport: shutdown is terminal)
    pom2::SpSerialTransport t2(slave, 115200);
    expect(t2.pollForPeer(10), "a fresh transport opens the tty");
    t2.dropPeer();
    expect(!t2.isOpen(), "dropPeer closes it");
    expect(t2.describe() == "waiting for " + slave, "…and describe() says it waits");
    expect(t2.pollForPeer(10) && t2.isOpen(), "…and a new poll reopens it");
    t2.dropPeer();

    ::close(master);
    if (failures) { std::printf("sp_serial_transport: %d failure(s)\n", failures); return 1; }
    std::printf("sp_serial_transport OK\n");
    return 0;
}
