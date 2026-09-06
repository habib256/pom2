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

// Host serial port test — pins src/SerialPort.cpp.
//
// Uses a POSIX pseudo-terminal pair: posix_openpt() gives a master, and
// ptsname() names a REAL slave device node that SerialPort::open() opens the
// same way it would open /dev/ttyACM0. That makes the whole termios path —
// raw mode, timeouts, modem-control lines — testable with no hardware.
// Windows has no pty equivalent, so the test compiles to a SKIP there; the
// Win32 DCB path is pinned by the manual checklist in docs/fujinet_plan.md.
//
// What is pinned, worst-consequence first:
//
//   1. THE ESP32 AUTO-RESET DEFENCE. FujiNet is an ESP32 board whose USB
//      bridge wires DTR and RTS to EN (reset) and IO0 (boot select) — the
//      auto-reset circuit esptool drives. If open() leaves them asserted,
//      POM2 REBOOTS THE USER'S FUJINET every time it opens the port, and the
//      symptom ("my FujiNet restarts when I launch the emulator") points
//      nowhere near this file.
//
//      HONEST LIMIT: a Linux pty has no modem-control lines — TIOCMGET fails
//      on both ends of the pair — so this harness CANNOT observe DTR/RTS.
//      What it does pin is the half that is observable and that regresses
//      just as easily: HUPCL cleared (with it set, merely QUITTING POM2 drops
//      DTR and resets the board) and CLOCAL set. The line-state half is
//      asserted only when the device really has lines, so the same test
//      tightens automatically against a real FujiNet, and it is on the manual
//      checklist in docs/fujinet_plan.md § 13.
//   2. RAW MODE SURVIVES BINARY DATA. $11/$13 are XON/XOFF and $0D/$0A are
//      what ICRNL/ONLCR rewrite. A SLIP frame contains all four routinely,
//      so a cooked terminal silently corrupts every disk block.
//   3. readSome() HONOURS ITS TIMEOUT. The session layer's 250 ms budget is
//      built on this: a read that returns instantly burns CPU, one that
//      blocks forever hangs the emulated 6502 mid-SmartPort-call.
//   4. AN UNPLUG IS REPORTED, NOT SPUN ON. Closing the master is what an
//      unplugged USB device looks like to the slave side.

#include "SerialPort.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !POM2_HAS_SERIAL || defined(_WIN32)

int main()
{
    std::puts("SKIP: serial pty harness is POSIX-only");
    return 77;   // ctest SKIP_RETURN_CODE
}

#else

#include <chrono>
#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

namespace {

using pom2::SerialPort;

/// Open a pty pair. Returns the master fd and fills `slavePath`.
int openPtyPair(std::string& slavePath)
{
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    assert(master >= 0);
    assert(::grantpt(master) == 0);
    assert(::unlockpt(master) == 0);
    const char* name = ::ptsname(master);
    assert(name != nullptr);
    slavePath = name;
    return master;
}

long elapsedMs(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

// ── 1. The ESP32 reset defence ───────────────────────────────────────────
void testModemLinesLowAfterOpen()
{
    std::string slave;
    const int master = openPtyPair(slave);

    SerialPort p;
    assert(p.open(slave, 115200));
    assert(p.isOpen());

    bool dtr = true, rts = true;
    if (p.modemControlSupported()) {
        // Real hardware (or any device with actual modem lines): THE
        // assertion of this file.
        assert(p.getModemLines(dtr, rts));
        assert(!dtr);
        assert(!rts);
    } else {
        // A pty. The contract is that getModemLines says "unknown" rather
        // than reporting a fabricated low — a caller must not be able to
        // conclude "DTR is safely down" from a device that has no DTR.
        assert(!p.getModemLines(dtr, rts));
        // And the setters must fail cleanly rather than pretending.
        assert(!p.setDtr(true));
        assert(!p.setRts(true));
    }

    p.close();
    ::close(master);
}

void testHupclClearedAndRawFlagsSet()
{
    std::string slave;
    const int master = openPtyPair(slave);

    SerialPort p;
    assert(p.open(slave, 115200));

    // Re-open the same node to inspect the line discipline open() left
    // behind. This is the observable half of trap 1: HUPCL set here means
    // "drop DTR when the last fd closes", i.e. reset the ESP32 when POM2
    // exits — a reboot the user would blame on anything but the emulator
    // shutting down.
    const int probe = ::open(slave.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    assert(probe >= 0);
    termios tio{};
    assert(::tcgetattr(probe, &tio) == 0);
    assert((tio.c_cflag & HUPCL) == 0);
    // CLOCAL: ignore modem status lines, so a device that never raises DCD
    // (every USB CDC one) does not wedge reads.
    assert((tio.c_cflag & CLOCAL) == CLOCAL);
    // Raw-mode flags — checked structurally here and behaviourally in
    // testBinaryRoundTrip, because either check alone can pass for the
    // wrong reason.
    assert((tio.c_iflag & (IXON | IXOFF | ICRNL | INLCR)) == 0);
    assert((tio.c_oflag & OPOST) == 0);
    assert((tio.c_cflag & CS8) == CS8);
    assert((tio.c_cflag & (PARENB | CSTOPB)) == 0);
    assert((tio.c_lflag & (ICANON | ECHO | ISIG)) == 0);
    ::close(probe);

    p.close();
    ::close(master);
}

// ── 2. Raw mode against the bytes that actually break ────────────────────
void testBinaryRoundTrip()
{
    std::string slave;
    const int master = openPtyPair(slave);

    SerialPort p;
    assert(p.open(slave, 115200));

    // The four bytes a cooked terminal mangles, plus the two SLIP specials
    // for good measure. If IXON is live, $11/$13 never arrive; if ICRNL or
    // ONLCR is live, $0D/$0A change value or multiply.
    const std::vector<uint8_t> payload{
        0x11, 0x13, 0x0D, 0x0A, 0xC0, 0xDB, 0x00, 0xFF, 0x1A, 0x04 };

    // Guest → host direction: write on the port, read on the master.
    assert(p.writeAll(payload.data(), payload.size()));
    std::vector<uint8_t> got;
    while (got.size() < payload.size()) {
        uint8_t buf[64];
        const ssize_t n = ::read(master, buf, sizeof(buf));
        if (n <= 0) break;
        got.insert(got.end(), buf, buf + n);
    }
    assert(got == payload);

    // Host → guest direction, through readSome().
    assert(::write(master, payload.data(), payload.size()) ==
           static_cast<ssize_t>(payload.size()));
    std::vector<uint8_t> back;
    while (back.size() < payload.size()) {
        uint8_t buf[64];
        const int n = p.readSome(buf, sizeof(buf), 500);
        assert(n >= 0);
        if (n == 0) break;
        back.insert(back.end(), buf, buf + n);
    }
    assert(back == payload);

    p.close();
    ::close(master);
}

// ── 3. The timeout the session layer's 250 ms budget rests on ────────────
void testReadTimeout()
{
    std::string slave;
    const int master = openPtyPair(slave);

    SerialPort p;
    assert(p.open(slave, 115200));

    uint8_t buf[16];
    const auto t0 = std::chrono::steady_clock::now();
    const int n = p.readSome(buf, sizeof(buf), 150);
    const long ms = elapsedMs(t0);

    assert(n == 0);                 // timeout, NOT an error
    assert(ms >= 130);              // it really waited...
    assert(ms < 600);               // ...and it really came back
    assert(p.isOpen());             // and the port is still usable

    p.close();
    ::close(master);
}

// ── 4. Unplug is an error, not an infinite loop ──────────────────────────
void testDisconnectReported()
{
    std::string slave;
    const int master = openPtyPair(slave);

    SerialPort p;
    assert(p.open(slave, 115200));

    // Closing the master is what a yanked USB cable looks like from here.
    ::close(master);

    // The very next read must report death rather than returning 0 forever.
    // Give it a handful of attempts: some platforms surface the hangup on
    // the second poll rather than the first.
    bool reportedDead = false;
    for (int i = 0; i < 20; ++i) {
        uint8_t buf[16];
        if (p.readSome(buf, sizeof(buf), 50) < 0) { reportedDead = true; break; }
    }
    assert(reportedDead);
    assert(!p.lastError().empty());

    p.close();
}

void testIdleHealthProbeDetectsDisconnect()
{
    std::string slave;
    const int master = openPtyPair(slave);
    SerialPort p;
    assert(p.open(slave, 115200));
    assert(p.isHealthy());
    ::close(master);

    bool dead = false;
    for (int i = 0; i < 20; ++i) {
        if (!p.isHealthy()) { dead = true; break; }
        ::usleep(1000);
    }
    assert(dead);
    assert(!p.lastError().empty());
    p.close();
}

// ── enumerate() must never throw, whatever the host looks like ───────────
void testEnumerateIsSafe()
{
    const auto list = SerialPort::enumerate();
    for (const auto& i : list) {
        assert(!i.path.empty());
        assert(!i.description.empty());
#ifdef __APPLE__
        // Trap 3: a tty.* device would block forever on open.
        assert(i.path.find("/dev/tty.") == std::string::npos);
#endif
    }
}

void testOpenFailureIsExplained()
{
    SerialPort p;
    assert(!p.open("/dev/definitely-not-a-serial-port-12345", 115200));
    assert(!p.isOpen());
    assert(!p.lastError().empty());

    // An unsupported baud must be refused outright rather than silently
    // becoming 9600 — a wrong rate on a real bridge chip is a link that
    // "connects" and then never decodes a byte.
    std::string slave;
    const int master = openPtyPair(slave);
    assert(!p.open(slave, 12345));
    assert(p.lastError().find("baud") != std::string::npos);
    ::close(master);
}

} // namespace

int main()
{
    testModemLinesLowAfterOpen();
    testHupclClearedAndRawFlagsSet();
    testBinaryRoundTrip();
    testReadTimeout();
    testDisconnectReported();
    testIdleHealthProbeDetectsDisconnect();
    testEnumerateIsSafe();
    testOpenFailureIsExplained();

    std::puts("serial_port: OK");
    return 0;
}

#endif // POSIX
