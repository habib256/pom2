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

// SerialPort — raw host serial port, POSIX termios and Win32 DCB behind one
// API. The serial sibling of SocketCompat.h.
//
// POM2 had no serial code before this: SuperSerialCard emulates a 6551 and
// bridges it to *TCP*, it never opens a host serial device. This class exists
// for the FujiNet USB CDC-ACM transport (SpSerialTransport) but is written as
// a general primitive — a real modem on the SSC, ADTPro over a cable, or a
// hardware disk emulator would all want the same thing.
//
// ── The three traps this header exists to remove ──────────────────────────
//
//   1. THE ESP32 AUTO-RESET CIRCUIT. FujiNet is an ESP32 board, and every
//      ESP32 USB bridge wires DTR and RTS to EN (reset) and IO0 (boot mode)
//      through the standard two-transistor auto-reset circuit — that is
//      exactly how `esptool` drops the chip into its ROM bootloader with no
//      button press. An open() that lets the OS assert DTR/RTS at their
//      defaults therefore REBOOTS THE BOARD, or strands it in the
//      bootloader, every single time POM2 opens the port. `open()` here
//      de-asserts both before the first byte, and clears HUPCL so that
//      CLOSING the port does not drop DTR and reset the board on the way
//      out. Pinned by tests/serial_port_test.cpp.
//
//   2. RAW MODE IS NOT OPTIONAL. SLIP frames carry $11 and $13, which IXON
//      software flow control eats, and $0D/$0A, which ICRNL/ONLCR rewrite.
//      A cooked terminal silently corrupts binary traffic. cfmakeraw() plus
//      an explicit re-clear of the flags that matter.
//
//   3. macOS: /dev/cu.* NOT /dev/tty.*. Opening the tty.* form blocks
//      waiting for carrier detect, which a USB CDC device never asserts, so
//      the open never returns. enumerate() only ever reports cu.* devices.
//
// Two smaller ones worth knowing: on Win32 the `\\.\` prefix is required for
// COM10 and above (plain "COM10" resolves to nothing), and on Linux a user
// outside the `dialout` group gets EACCES with no explanation — `lastError()`
// says so in words rather than leaving the caller with "open failed".
//
// Baud rate is meaningless for a native CDC-ACM device (the USB pipe runs at
// USB speed regardless) but SOME FujiNet boards put a real FTDI/CP210x bridge
// in front of the ESP32, and those do care. The setting is therefore exposed
// and defaults to 115200 rather than being helpfully hidden.
//
// Not available under Emscripten (no devices in a browser): POM2_HAS_SERIAL
// is 0 there and every method fails cleanly.

#ifndef POM2_SERIAL_PORT_H
#define POM2_SERIAL_PORT_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// 1 when the host has serial devices POM2 can open.
#if defined(__EMSCRIPTEN__)
#  define POM2_HAS_SERIAL 0
#else
#  define POM2_HAS_SERIAL 1
#endif

namespace pom2 {

class SerialPort
{
public:
    /// Conventional default. See the note above on why this is exposed at
    /// all for a CDC-ACM device.
    static constexpr int kDefaultBaud = 115200;

    struct Info {
        std::string path;         ///< what to pass to open()
        std::string description;  ///< for the picker ("/dev/ttyACM0 (FujiNet)")
    };

    SerialPort() = default;
    ~SerialPort();

    SerialPort(const SerialPort&)            = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    /// Candidate serial devices, most-likely-first. Linux prefers the stable
    /// /dev/serial/by-id/* names (they survive a replug; ttyACM0 does not),
    /// macOS reports /dev/cu.usbmodem* / cu.usbserial*, Windows reads
    /// HKLM\HARDWARE\DEVICEMAP\SERIALCOMM. Never throws; an empty result just
    /// means nothing is plugged in.
    static std::vector<Info> enumerate();

    /// Open in raw binary mode with DTR/RTS de-asserted (trap 1). Returns
    /// false and fills lastError() on failure.
    bool open(const std::string& path, int baud = kDefaultBaud);
    bool isOpen() const;
    /// Non-blocking device-presence probe. Does not consume input bytes.
    bool isHealthy();
    void close();

    /// Write everything or fail. false = the device went away.
    /// Write everything or fail. false = the device went away, or the whole
    /// write did not fit in `timeoutMs`.
    ///
    /// `timeoutMs` bounds the CALL, not each stall: the poll used to be
    /// re-armed for a fresh 1000 ms every time the device buffer made room,
    /// so a peer draining a trickle held the caller — the CPU thread, under
    /// the emulator's stateMutex — for as long as it liked (measured: 44 s
    /// for one write against a peer taking 8 KB every 700 ms). `abort`, when
    /// given, is re-read on every slice so a stop lands inside a slice
    /// instead of at the end of the budget (readSome already works this way).
    /// Bug hunt #11.
    bool writeAll(const uint8_t* p, std::size_t n, int timeoutMs = 1000,
                  const std::atomic<bool>* abort = nullptr);

    /// Read up to `n` bytes, waiting at most `timeoutMs` for the first.
    ///   > 0  bytes read
    ///   = 0  timeout, device still healthy
    ///   < 0  device went away (unplugged)
    int readSome(uint8_t* p, std::size_t n, int timeoutMs);

    /// Modem control lines. Exposed because the ESP32 reset circuit hangs off
    /// them: a caller that WANTS to reset the board can, deliberately.
    bool setDtr(bool on);
    bool setRts(bool on);
    /// Current DTR/RTS state as the driver reports it. Returns false when the
    /// device has no modem-control lines to report — a pseudo-terminal (so the
    /// unit test cannot observe trap 1; only real hardware and the manual
    /// checklist can) or Windows, where DTR/RTS are outputs the API gives no
    /// way to read back. A false return means "unknown", never "low".
    bool getModemLines(bool& dtrOut, bool& rtsOut) const;

    /// Whether this device exposes modem-control lines at all. Lets a caller
    /// distinguish "DTR is low" from "there is no DTR here", which matters
    /// because the FujiNet reset circuit only exists on the real thing.
    bool modemControlSupported() const;

    const std::string& path() const { return path_; }
    const std::string& lastError() const { return lastError_; }

private:
    void setError(const std::string& what);

#if POM2_HAS_SERIAL
#  ifdef _WIN32
    void* handle_ = nullptr;      ///< HANDLE, kept void* so <windows.h> stays
                                  ///  out of this header
#  else
    int   fd_ = -1;
#  endif
#endif
    std::string path_;
    std::string lastError_;
};

} // namespace pom2

#endif // POM2_SERIAL_PORT_H
