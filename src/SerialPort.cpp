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

// SerialPort implementation. See SerialPort.h for the three traps this file
// exists to remove (ESP32 auto-reset via DTR/RTS, raw mode, macOS cu.* vs
// tty.*).

#include "SerialPort.h"

#include <algorithm>

#if POM2_HAS_SERIAL

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <cstring>
#  include <dirent.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/stat.h>
#  include <termios.h>
#  include <unistd.h>
#endif

#endif // POM2_HAS_SERIAL

namespace pom2 {

// ─────────────────────────────────────────────────────────────────────────
// Socketless / device-less build (Emscripten)
// ─────────────────────────────────────────────────────────────────────────
#if !POM2_HAS_SERIAL

SerialPort::~SerialPort() = default;
std::vector<SerialPort::Info> SerialPort::enumerate() { return {}; }
bool SerialPort::open(const std::string&, int)
{ lastError_ = "serial ports are not available in this build"; return false; }
bool SerialPort::isOpen() const { return false; }
bool SerialPort::isHealthy() { return false; }
void SerialPort::close() {}
bool SerialPort::writeAll(const uint8_t*, std::size_t) { return false; }
int  SerialPort::readSome(uint8_t*, std::size_t, int) { return -1; }
bool SerialPort::setDtr(bool) { return false; }
bool SerialPort::setRts(bool) { return false; }
bool SerialPort::getModemLines(bool&, bool&) const { return false; }
void SerialPort::setError(const std::string& what) { lastError_ = what; }

#else // POM2_HAS_SERIAL

void SerialPort::setError(const std::string& what) { lastError_ = what; }

SerialPort::~SerialPort() { close(); }

// ═════════════════════════════════════════════════════════════════════════
// POSIX (Linux, macOS)
// ═════════════════════════════════════════════════════════════════════════
#ifndef _WIN32

namespace {

/// termios does not take a number, it takes a `speed_t` constant. Anything
/// outside this table is refused rather than silently becoming B9600.
speed_t baudConstant(int baud)
{
    switch (baud) {
    case 1200:    return B1200;
    case 2400:    return B2400;
    case 4800:    return B4800;
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
#ifdef B230400
    case 230400:  return B230400;
#endif
#ifdef B460800
    case 460800:  return B460800;
#endif
#ifdef B921600
    case 921600:  return B921600;
#endif
    default:      return 0;
    }
}

/// Directory scan helper — returns entries whose name starts with any of the
/// given prefixes, as full paths, sorted.
std::vector<std::string> scanDir(const char* dir,
                                 const std::vector<std::string>& prefixes)
{
    std::vector<std::string> out;
    DIR* d = ::opendir(dir);
    if (!d) return out;
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        for (const auto& p : prefixes) {
            if (name.compare(0, p.size(), p) == 0) {
                out.push_back(std::string(dir) + "/" + name);
                break;
            }
        }
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

std::vector<SerialPort::Info> SerialPort::enumerate()
{
    std::vector<Info> out;

#ifdef __APPLE__
    // cu.* ONLY — see trap 3. Opening the tty.* twin blocks on carrier
    // detect, which a USB CDC device never raises.
    for (const auto& p : scanDir("/dev", { "cu.usbmodem", "cu.usbserial",
                                           "cu.SLAB_USBtoUART", "cu.wchusbserial" }))
        out.push_back({ p, p });
#else
    // Linux. /dev/serial/by-id/* first: those names embed the USB vendor and
    // serial number, so they survive a replug — /dev/ttyACM0 does not, and a
    // user who saved "ttyACM0" in settings would silently target somebody
    // else's device after a reboot.
    for (const auto& link : scanDir("/dev/serial/by-id", { "" })) {
        const std::string base = link.substr(link.find_last_of('/') + 1);
        if (base == "." || base == "..") continue;
        out.push_back({ link, base });
    }
    if (out.empty()) {
        for (const auto& p : scanDir("/dev", { "ttyACM", "ttyUSB" }))
            out.push_back({ p, p });
    }
#endif

    // A FujiNet advertises itself in the by-id string; float it to the top so
    // the panel's default pick is the obvious one.
    std::stable_partition(out.begin(), out.end(), [](const Info& i) {
        std::string d = i.description;
        std::transform(d.begin(), d.end(), d.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        return d.find("fujinet") != std::string::npos ||
               d.find("fujiapple") != std::string::npos;
    });
    return out;
}

bool SerialPort::open(const std::string& path, int baud)
{
    close();

    const speed_t speed = baudConstant(baud);
    if (speed == 0) {
        setError("unsupported baud rate " + std::to_string(baud));
        return false;
    }

    // O_NONBLOCK on the open() itself matters even on a device that has no
    // carrier concept: without it the open can block indefinitely on a port
    // whose driver waits for DCD. The flag then STAYS SET for the life of the
    // descriptor — deliberately, and nothing below clears it. It is not what
    // paces a read (poll() does that, in readSome); it is what guarantees
    // read()/write() come back with EAGAIN instead of parking in the driver,
    // which is what lets writeAll() wait on POLLOUT rather than block. The
    // VMIN=0/VTIME=0 pair set below asks the tty layer for the same "return
    // what is there" behaviour, so the two agree either way.
    // O_NOCTTY: never let a serial device become POM2's controlling terminal,
    // or a hangup on the line would deliver SIGHUP to the emulator.
    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        const int e = errno;
        if (e == EACCES) {
            // The single most likely first-contact failure on Linux, and the
            // one with the least informative errno text.
            setError(path + ": permission denied — add your user to the "
                     "'dialout' group (sudo usermod -aG dialout $USER) and "
                     "log out and back in");
        } else {
            setError(path + ": " + std::strerror(e));
        }
        return false;
    }

    termios tio{};
    if (::tcgetattr(fd_, &tio) != 0) {
        setError(path + ": tcgetattr: " + std::strerror(errno));
        close();
        return false;
    }

    // Trap 2 — raw mode. cfmakeraw clears ICANON/ECHO/ISIG, IXON/ICRNL/INLCR,
    // OPOST and the character-size bits, which is exactly what binary SLIP
    // traffic needs. The explicit clears afterwards are the ones that have
    // bitten other projects on one libc or another; belt and braces.
    ::cfmakeraw(&tio);
    tio.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR));
    tio.c_oflag &= static_cast<tcflag_t>(~OPOST);
    tio.c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB | CSTOPB | CRTSCTS));
    tio.c_cflag |= static_cast<tcflag_t>(CS8 | CLOCAL | CREAD);

    // Trap 1, half two — HUPCL drops DTR when the last fd closes, which on an
    // ESP32 board is a reset. POM2 must be able to quit without rebooting the
    // FujiNet.
    tio.c_cflag &= static_cast<tcflag_t>(~HUPCL);

    // poll() does the waiting, so the driver should never block on its own:
    // return whatever is available immediately.
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    ::cfsetispeed(&tio, speed);
    ::cfsetospeed(&tio, speed);

    if (::tcsetattr(fd_, TCSANOW, &tio) != 0) {
        setError(path + ": tcsetattr: " + std::strerror(errno));
        close();
        return false;
    }

    // Trap 1, half one — de-assert DTR and RTS BEFORE any traffic. On an
    // ESP32 these are wired to EN (reset) and IO0 (boot select); leaving them
    // at the driver's default reboots the board, or strands it in the ROM
    // bootloader, every time POM2 opens the port.
    setDtr(false);
    setRts(false);

    // Discard anything the driver buffered before we owned the line — the
    // tail of some other program's session is not our first packet.
    ::tcflush(fd_, TCIOFLUSH);

    path_ = path;
    lastError_.clear();
    return true;
}

bool SerialPort::isOpen() const { return fd_ >= 0; }

bool SerialPort::isHealthy()
{
    if (fd_ < 0) return false;
    pollfd pfd{};
    pfd.fd = fd_;
    // Some BSD poll implementations only report a terminal hangup when an
    // input condition is requested, even though POLLHUP itself is returned
    // independently of the requested event mask.
    pfd.events = POLLIN;
    const int r = ::poll(&pfd, 1, 0);
    if (r < 0) {
        if (errno == EINTR) return true;
        setError(path_ + ": health poll: " + std::strerror(errno));
        return false;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        setError(path_ + ": device disconnected");
        return false;
    }
    return true;
}

void SerialPort::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    path_.clear();
}

bool SerialPort::writeAll(const uint8_t* p, std::size_t n)
{
    if (fd_ < 0) return false;
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t w = ::write(fd_, p + sent, n - sent);
        if (w > 0) { sent += static_cast<std::size_t>(w); continue; }
        if (w < 0 && (errno == EINTR)) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The device's output buffer is full. Wait for room rather than
            // spinning; a USB CDC endpoint drains in microseconds, so this
            // is rare and short.
            pollfd pfd{};
            pfd.fd     = fd_;
            pfd.events = POLLOUT;
            const int r = ::poll(&pfd, 1, 1000);
            if (r > 0) continue;
            if (r == 0) { setError(path_ + ": write timed out"); return false; }
            if (errno == EINTR) continue;
            setError(path_ + ": poll(out): " + std::strerror(errno));
            return false;
        }
        setError(path_ + ": write: " + std::strerror(errno));
        return false;
    }
    return true;
}

int SerialPort::readSome(uint8_t* p, std::size_t n, int timeoutMs)
{
    if (fd_ < 0) return -1;

    pollfd pfd{};
    pfd.fd     = fd_;
    pfd.events = POLLIN;
    const int r = ::poll(&pfd, 1, timeoutMs);
    if (r == 0) return 0;                       // timeout, device fine
    if (r < 0) {
        if (errno == EINTR) return 0;           // caller re-checks its deadline
        setError(path_ + ": poll: " + std::strerror(errno));
        return -1;
    }
    // POLLERR/POLLNVAL on a serial fd is an unplug; POLLHUP alone can also
    // mean the far end vanished. Either way the device is gone.
    if (pfd.revents & (POLLERR | POLLNVAL)) {
        setError(path_ + ": device disconnected");
        return -1;
    }

    const ssize_t got = ::read(fd_, p, n);
    if (got > 0) return static_cast<int>(got);
    if (got == 0) {
        // On a POLLHUP'd tty, read() returns 0 forever. Treat it as gone so
        // the link tears down instead of spinning.
        if (pfd.revents & POLLHUP) { setError(path_ + ": device disconnected"); return -1; }
        return 0;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    // ENXIO / EIO is what an unplugged USB CDC device reports.
    setError(path_ + ": read: " + std::strerror(errno));
    return -1;
}

bool SerialPort::setDtr(bool on)
{
    if (fd_ < 0) return false;
    int bits = TIOCM_DTR;
    return ::ioctl(fd_, on ? TIOCMBIS : TIOCMBIC, &bits) == 0;
}

bool SerialPort::setRts(bool on)
{
    if (fd_ < 0) return false;
    int bits = TIOCM_RTS;
    return ::ioctl(fd_, on ? TIOCMBIS : TIOCMBIC, &bits) == 0;
}

bool SerialPort::getModemLines(bool& dtrOut, bool& rtsOut) const
{
    if (fd_ < 0) return false;
    int bits = 0;
    // Unsupported on a pseudo-terminal (Linux ptys have no modem-control
    // lines and fail the ioctl), which is why the unit test can only pin the
    // termios half of trap 1 — see tests/serial_port_test.cpp.
    if (::ioctl(fd_, TIOCMGET, &bits) != 0) return false;
    dtrOut = (bits & TIOCM_DTR) != 0;
    rtsOut = (bits & TIOCM_RTS) != 0;
    return true;
}

bool SerialPort::modemControlSupported() const
{
    if (fd_ < 0) return false;
    int bits = 0;
    return ::ioctl(fd_, TIOCMGET, &bits) == 0;
}

// ═════════════════════════════════════════════════════════════════════════
// Win32
// ═════════════════════════════════════════════════════════════════════════
#else // _WIN32

namespace {
HANDLE H(void* h) { return static_cast<HANDLE>(h); }

std::string win32ErrorText(DWORD e)
{
    char* buf = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string msg = (n && buf) ? std::string(buf, n)
                                 : ("win32 error " + std::to_string(e));
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    return msg;
}

/// COM10 and above need the \\.\ prefix — "COM10" on its own resolves to
/// nothing, which is the classic Win32 serial trap. Harmless for COM1-9, so
/// it is applied unconditionally.
std::string win32DevicePath(const std::string& path)
{
    if (path.compare(0, 4, "\\\\.\\") == 0) return path;
    return "\\\\.\\" + path;
}
} // namespace

std::vector<SerialPort::Info> SerialPort::enumerate()
{
    std::vector<Info> out;
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return out;

    char  nameBuf[256];
    BYTE  dataBuf[256];
    for (DWORD i = 0;; ++i) {
        DWORD nameLen = sizeof(nameBuf);
        DWORD dataLen = sizeof(dataBuf);
        DWORD type    = 0;
        const LONG r = RegEnumValueA(key, i, nameBuf, &nameLen, nullptr,
                                     &type, dataBuf, &dataLen);
        if (r != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;
        const std::string com(reinterpret_cast<char*>(dataBuf),
                              dataLen ? dataLen - 1 : 0);
        const std::string driver(nameBuf, nameLen);
        if (!com.empty()) out.push_back({ com, com + " (" + driver + ")" });
    }
    RegCloseKey(key);

    // USB CDC devices show up under a usbser/ACM driver path; float them up.
    std::stable_partition(out.begin(), out.end(), [](const Info& i) {
        std::string d = i.description;
        std::transform(d.begin(), d.end(), d.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        return d.find("usbser") != std::string::npos ||
               d.find("acm") != std::string::npos;
    });
    return out;
}

bool SerialPort::open(const std::string& path, int baud)
{
    close();

    const std::string dev = win32DevicePath(path);
    HANDLE h = CreateFileA(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0,            // serial ports are never shared
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,   // synchronous I/O
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        setError(path + ": " + win32ErrorText(GetLastError()));
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        setError(path + ": GetCommState: " + win32ErrorText(GetLastError()));
        CloseHandle(h);
        return false;
    }
    dcb.BaudRate = static_cast<DWORD>(baud);
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    // Trap 2 — binary mode, no flow control of any kind. fOutX/fInX are
    // XON/XOFF, which would eat the $11/$13 bytes inside a SLIP frame.
    dcb.fBinary          = TRUE;
    dcb.fParity          = FALSE;
    dcb.fOutxCtsFlow     = FALSE;
    dcb.fOutxDsrFlow     = FALSE;
    dcb.fDsrSensitivity  = FALSE;
    dcb.fOutX            = FALSE;
    dcb.fInX             = FALSE;
    dcb.fErrorChar       = FALSE;
    dcb.fNull            = FALSE;
    dcb.fAbortOnError    = FALSE;
    // Trap 1 — DTR/RTS must be DISABLED, not "handshake". On an ESP32 board
    // these drive EN and IO0; letting Windows assert them reboots the device
    // on every open.
    dcb.fDtrControl      = DTR_CONTROL_DISABLE;
    dcb.fRtsControl      = RTS_CONTROL_DISABLE;
    if (!SetCommState(h, &dcb)) {
        setError(path + ": SetCommState: " + win32ErrorText(GetLastError()));
        CloseHandle(h);
        return false;
    }

    // Timeouts are set per read in readSome(); this is the safe baseline.
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;    // return immediately with what's there
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 1000;
    if (!SetCommTimeouts(h, &to)) {
        setError(path + ": SetCommTimeouts: " +
                 win32ErrorText(GetLastError()));
        CloseHandle(h);
        return false;
    }

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    EscapeCommFunction(h, CLRDTR);
    EscapeCommFunction(h, CLRRTS);

    handle_ = h;
    path_   = path;
    lastError_.clear();
    return true;
}

bool SerialPort::isOpen() const
{ return handle_ != nullptr && H(handle_) != INVALID_HANDLE_VALUE; }

bool SerialPort::isHealthy()
{
    if (!isOpen()) return false;
    DWORD errors = 0;
    COMSTAT status{};
    if (ClearCommError(H(handle_), &errors, &status)) return true;
    setError(path_ + ": device disconnected: " +
             win32ErrorText(GetLastError()));
    return false;
}

void SerialPort::close()
{
    if (isOpen()) CloseHandle(H(handle_));
    handle_ = nullptr;
    path_.clear();
}

bool SerialPort::writeAll(const uint8_t* p, std::size_t n)
{
    if (!isOpen()) return false;
    std::size_t sent = 0;
    while (sent < n) {
        DWORD wrote = 0;
        if (!WriteFile(H(handle_), p + sent,
                       static_cast<DWORD>(n - sent), &wrote, nullptr)) {
            setError(path_ + ": WriteFile: " + win32ErrorText(GetLastError()));
            return false;
        }
        if (wrote == 0) { setError(path_ + ": write timed out"); return false; }
        sent += wrote;
    }
    return true;
}

int SerialPort::readSome(uint8_t* p, std::size_t n, int timeoutMs)
{
    if (!isOpen()) return -1;

    // "Wait up to timeoutMs for the FIRST byte, then return what's there" is
    // exactly the ReadIntervalTimeout=MAXDWORD + ReadTotalTimeoutConstant
    // combination.
    //
    // The constant is clamped to at least 1 ms because that combination is
    // only defined for a constant BETWEEN 1 and MAXDWORD-1 (SetCommTimeouts,
    // "remarks"): a constant of ZERO alongside MAXDWORD/MAXDWORD is not
    // "return immediately" — that shape needs a multiplier of 0 as well — it
    // is an INFINITE wait for the first byte. So a caller passing
    // timeoutMs <= 0 (the session layer does, as `waitMs > 0 ? waitMs : 1`
    // shows it means to avoid) would have blocked the link until a byte
    // happened to arrive, with no way to interrupt it.
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = MAXDWORD;
    to.ReadTotalTimeoutConstant    = static_cast<DWORD>(timeoutMs < 1 ? 1 : timeoutMs);
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 1000;
    if (!SetCommTimeouts(H(handle_), &to)) {
        setError(path_ + ": SetCommTimeouts: " +
                 win32ErrorText(GetLastError()));
        return -1;
    }

    DWORD got = 0;
    if (!ReadFile(H(handle_), p, static_cast<DWORD>(n), &got, nullptr)) {
        const DWORD e = GetLastError();
        setError(path_ + ": ReadFile: " + win32ErrorText(e));
        return -1;                       // includes ERROR_DEVICE_REMOVED
    }
    return static_cast<int>(got);        // 0 = timeout
}

bool SerialPort::setDtr(bool on)
{
    if (!isOpen()) return false;
    return EscapeCommFunction(H(handle_), on ? SETDTR : CLRDTR) != 0;
}

bool SerialPort::setRts(bool on)
{
    if (!isOpen()) return false;
    return EscapeCommFunction(H(handle_), on ? SETRTS : CLRRTS) != 0;
}

bool SerialPort::getModemLines(bool&, bool&) const
{
    // GetCommModemStatus reports the INPUT lines (CTS/DSR/RI/DCD); Windows
    // gives no way to read back DTR/RTS, which are outputs. The DCB above
    // sets both to *_CONTROL_DISABLE, which is the enforceable half of the
    // contract; the test that reads them back is POSIX-only.
    return false;
}

bool SerialPort::modemControlSupported() const { return false; }

#endif // _WIN32
#endif // POM2_HAS_SERIAL

} // namespace pom2
