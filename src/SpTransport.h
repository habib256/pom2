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

// SpTransport — the byte pipe underneath SP-over-SLIP, and its two concrete
// forms.
//
// The FujiNet spec says SP-over-SLIP "can be used on any medium providing a
// transparent, duplex, lossless byte stream. Examples are a TCP connection or
// a USB CDC-ACM connection." POM2 implements both, and this interface is the
// seam that keeps the session layer (SpOverSlipLink) from caring which one it
// holds:
//
//   SpTcpTransport     POM2 listens on 127.0.0.1:1985; a FujiNet *desktop
//                      build* connects in. The everyday case.
//   SpSerialTransport  POM2 opens a serial device; a *physical FujiNet board*
//                      answers over its USB CDC-ACM port.
//
// This mirrors the split the FujiNet AppleWin fork uses (`devrelay/service/
// Connection.h` + `TCPConnection` / `COMConnection`), for the same reason:
// the protocol code should be written once.
//
// ── Threading contract ────────────────────────────────────────────────────
//
// Two threads touch a transport, and they do different jobs:
//
//   * The LINK WORKER calls `pollForPeer()` — accept a TCP connection, or
//     open the serial device once it appears. Worker thread only.
//   * The CPU THREAD calls `writeAll()` / `readSome()` from inside a
//     SmartPort call, under EmulationController's stateMutex.
//
// Implementations serialise those with an internal mutex held for the whole
// of `readSome`/`writeAll` — safe because both are bounded by the caller's
// timeout (250 ms by default), so the worker never waits long to tear down a
// dead peer.
//
// ── …and a THIRD thread that must never wait on it ────────────────────────
//
// The UI thread polls `isOpen()` / `describe()` / `lastError()` every frame to
// draw the FujiNet panel, and it does so WHILE HOLDING EmulationController's
// stateMutex. If those status reads took the I/O mutex they would block for a
// whole read timeout whenever a peer went silent — and because the CPU worker
// needs stateMutex for every budget slice, the emulated machine would drop to
// a fraction of speed with audible underruns for as long as the dead peer sat
// there. So status is published SEPARATELY from I/O: atomics where one value
// suffices, and a `statusMtx_` that is never held across a syscall.
//
// `shutdown()` is the ONE method that must be callable from any thread: it
// wakes a `pollForPeer()` parked in a wait. It therefore may not take the I/O
// mutex either — a read in flight is exactly when it is called. Destroying the
// transport (and closing the listening socket / device) is only legal once the
// worker has been joined — the same rule SuperSerialCard documents at length,
// for the same reason (close + recv on one fd from two threads is a
// use-after-free).

#ifndef POM2_SP_TRANSPORT_H
#define POM2_SP_TRANSPORT_H

#include "Pom2Build.h"
#include "SerialPort.h"
#include "SocketCompat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace pom2 {

class SpTransport
{
public:
    virtual ~SpTransport() = default;

    /// True when a peer is attached and I/O can be attempted.
    virtual bool isOpen() const = 0;

    /// Try to acquire a peer, waiting at most `timeoutMs`. Returns true when
    /// a peer was JUST acquired (the session layer takes that as its cue to
    /// enumerate devices). Worker thread only.
    virtual bool pollForPeer(int timeoutMs) = 0;

    /// Write the whole buffer. false = the peer died; the caller should drop
    /// the link. Blocking, but only ever called with small buffers.
    virtual bool writeAll(const uint8_t* p, std::size_t n) = 0;

    /// Whole-call budget for the next `writeAll`, in milliseconds.
    ///
    /// The header above promises every transport call is bounded by the
    /// caller's timeout, and the read half keeps that promise. The write half
    /// did not: it carried its own fixed 2000 ms deadline, so one SmartPort
    /// call against a peer that had stopped reading cost 2 s of write plus up
    /// to `timeoutMs()` of read — with the panel's 5 s maximum, seven seconds
    /// of the CPU thread holding the emulator's stateMutex, from a single
    /// `$C0xx` access. The session layer sets this from its own budget before
    /// every write so the two halves share one bound.
    ///
    /// Every transport honours it: a tty output queue drains at the line
    /// rate, so a board that stops taking bytes parks the writer exactly the
    /// way a TCP peer that stops reading does (bug hunt #11 — the serial
    /// transport used to call this a no-op).
    virtual void setWriteDeadlineMs(int /*ms*/) {}

    /// Read up to `n` bytes, waiting at most `timeoutMs` for the first one.
    ///   > 0  bytes read
    ///   = 0  nothing arrived within the timeout (not an error)
    ///   < 0  the peer died
    virtual int readSome(uint8_t* p, std::size_t n, int timeoutMs) = 0;

    /// Cheap "is the peer still there?" check for the worker to run while the
    /// guest is idle. Must NOT consume anything the CPU thread is about to
    /// read, and must not block.
    ///
    /// Without it a peer that closes between two SmartPort calls is invisible:
    /// nothing probes the socket, so `isOpen()` stays true forever, the panel
    /// keeps naming a dead peer, and a replacement sits unaccepted in the
    /// listen backlog until the guest happens to issue a call.
    virtual bool checkPeerAlive() { return isOpen(); }

    /// Drop the current peer but stay ready to acquire another one.
    virtual void dropPeer() = 0;

    /// Wake a `pollForPeer()` in flight so the worker can exit. Any thread.
    virtual void shutdown() = 0;

    /// One line for the panel and the log ("listening on :1985",
    /// "/dev/ttyACM0 @ 115200", "connected 127.0.0.1:54xxx").
    virtual std::string describe() const = 0;
};

// ── TCP ───────────────────────────────────────────────────────────────────

/// Listens on loopback and accepts ONE peer at a time. A second connection
/// while one is live is closed immediately rather than queued: two peers
/// would mean two SmartPort device-number spaces to merge, which no real
/// configuration needs.
///
/// Default port 1985 — the port `fujinet-go-apple2-desktop` uses, so an
/// existing FujiNet configuration works against POM2 untouched.
class SpTcpTransport final : public SpTransport
{
public:
    static constexpr uint16_t kDefaultPort = 1985;

    explicit SpTcpTransport(uint16_t port = kDefaultPort);
    ~SpTcpTransport() override;

    /// Bind + listen. Must be called before the worker starts polling.
    /// Returns false with a human-readable reason in `errOut`.
    bool startListening(std::string& errOut);
    /// Close the listening socket. Illegal while a worker may still be inside
    /// `pollForPeer()` — join it first.
    void stopListening();
    bool isListening() const;

    uint16_t port() const { return port_; }

    bool        isOpen() const override;
    bool        pollForPeer(int timeoutMs) override;
    bool        writeAll(const uint8_t* p, std::size_t n) override;
    int         readSome(uint8_t* p, std::size_t n, int timeoutMs) override;
    bool        checkPeerAlive() override;
    void        dropPeer() override;
    void        shutdown() override;
    std::string describe() const override;
    void        setWriteDeadlineMs(int ms) override
    { if (ms > 0) writeDeadlineMs_.store(ms); }

private:
    uint16_t            port_;
    /// See SpTransport::setWriteDeadlineMs. The default stands in only for a
    /// caller that never sets one.
    std::atomic<int>    writeDeadlineMs_{2000};
    /// Serialises I/O against peer teardown: held for the whole of
    /// readSome/writeAll, so dropPeer() cannot close a descriptor another
    /// thread is inside recv() on (the use-after-close hazard
    /// SuperSerialCard documents). Both are bounded by the caller's timeout,
    /// so the wait is short.
    mutable std::mutex  mtx_;
    /// Guards the descriptor's LIFETIME, and unlike mtx_ is never held across
    /// a wait. `shutdown()` cannot take mtx_ (a read in flight is precisely
    /// when it runs), so this is what stops it from ::shutdown()ing a number
    /// dropPeer() has already closed and another thread's socket() has already
    /// recycled.
    mutable std::mutex  fdLifeMtx_;
    /// Guards the panel-facing strings only. Short-held by construction, so
    /// describe() never waits on an I/O timeout. See the header comment.
    mutable std::mutex  statusMtx_;
#if POM2_HAS_SOCKETS
    /// Atomic so `shutdown()` can wake a blocked wait from any thread
    /// WITHOUT closing the descriptor under it — same split as the SSC.
    std::atomic<socket_t> listenFd_{kInvalidSocket};
    std::atomic<socket_t> clientFd_{kInvalidSocket};
#endif
    std::string         peerText_;        ///< guarded by statusMtx_
};

// ── Serial (USB CDC-ACM) ──────────────────────────────────────────────────

/// Opens a serial device and treats THE OPEN as the connect event — a serial
/// line has no connection establishment to wait on. While disconnected the
/// worker retries the open every poll, which doubles as hot-plug detection.
class SpSerialTransport final : public SpTransport
{
public:
    /// `path` empty = "auto": take the single candidate from
    /// SerialPort::enumerate(), or stay idle if there is not exactly one.
    explicit SpSerialTransport(std::string path = std::string{},
                               int baud = SerialPort::kDefaultBaud);
    ~SpSerialTransport() override;

    std::string path() const
    { std::lock_guard<std::mutex> lk(statusMtx_); return path_; }
    void setPath(std::string path);
    int  baud() const { return baud_.load(); }
    void setBaud(int b) { baud_.store(b); }

    /// Why the last open() failed, in words the panel can show — including
    /// the permission case, which is the most likely first-contact failure
    /// on Linux ("add your user to the dialout group").
    ///
    /// BY VALUE, under the lock. Handing out a `const std::string&` raced the
    /// worker thread, which rewrites this string every poll: when the text
    /// changed length the reader's copy-construct could follow a pointer into
    /// a block the writer had just freed.
    std::string lastError() const
    { std::lock_guard<std::mutex> lk(statusMtx_); return lastError_; }

    bool        isOpen() const override;
    bool        pollForPeer(int timeoutMs) override;
    bool        writeAll(const uint8_t* p, std::size_t n) override;
    int         readSome(uint8_t* p, std::size_t n, int timeoutMs) override;
    bool        checkPeerAlive() override;
    void        dropPeer() override;
    void        shutdown() override;
    std::string describe() const override;
    /// NOT a no-op: a tty output queue drains at the line rate, so a board
    /// that stops taking bytes parks the writer exactly the way a TCP peer
    /// that stops reading does. Bug hunt #11.
    void        setWriteDeadlineMs(int ms) override
    { if (ms > 0) writeDeadlineMs_.store(ms); }

private:
    std::atomic<int>   writeDeadlineMs_{1000};
    std::atomic<int>   baud_;
    mutable std::mutex mtx_;          ///< guards port_ — HELD ACROSS I/O
    /// Guards the panel-facing strings. Short-held by construction: the UI
    /// thread reads describe()/lastError()/path() every frame with stateMutex
    /// held, and must never be parked behind a read timeout. Lock order is
    /// always mtx_ → statusMtx_.
    mutable std::mutex statusMtx_;
    /// Mirrors port_.isOpen() so isOpen() needs no lock at all.
    std::atomic<bool>  open_{false};
    /// Atomic so shutdown() — callable from any thread, including while a
    /// read is in flight — never waits on mtx_.
    std::atomic<bool>  stopping_{false};
    SerialPort         port_;         ///< guarded by mtx_
    std::string        path_;         ///< guarded by statusMtx_
    std::string        openPath_;     ///< the device actually opened
    std::string        lastError_;    ///< guarded by statusMtx_
};

} // namespace pom2

#endif // POM2_SP_TRANSPORT_H
