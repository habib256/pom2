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

// SP-over-SLIP session-layer test — pins src/SpOverSlipLink.cpp.
//
// Drives the real link against a FAKE FUJINET running in-process on loopback:
// a thread that connects to POM2's listener, decodes SLIP frames with the
// same framer, and answers them the way the protocol says a FujiNet would.
// No FujiNet firmware, no hardware, no network beyond 127.0.0.1.
//
// What is pinned, worst-consequence first:
//
//   1. ENUMERATION STOPS AT THE RIGHT UNIT. The chain is discovered by
//      INIT-ing units until one answers non-zero. The FujiNet AppleWin fork
//      registers a device even for the unit whose INIT *failed* (it inserts
//      before testing its own `still_scanning` flag), so its count runs one
//      high; POM2 deliberately does not, and this test is what keeps that
//      divergence from being "fixed" back into a bug.
//   2. A BLOCK ROUND TRIP IS BYTE-EXACT. 512 bytes chosen to contain $C0 and
//      $DB in quantity, because those are what the framing escapes — a
//      broken escape corrupts disk data rather than failing loudly.
//   3. A STALE RESPONSE IS DISCARDED. This is the single reason the protocol
//      carries a request sequence number: after a guest reset, the response
//      to the pre-reset request is still sitting in the socket buffer, and
//      reading it as the answer to the NEXT call would hand ProDOS one
//      block's data labelled as another's.
//   4. A DEAD PEER TIMES OUT AND THE LINK SURVIVES. The emulated 6502 is
//      parked inside a JSR for the whole round trip, so "no answer" must
//      become a bounded stall and a clean I/O error, never a hang.

#include "SlipFramer.h"
#include "SpOverSlipLink.h"
#include "SocketCompat.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#if !POM2_HAS_SOCKETS

int main()
{
    std::puts("SKIP: built without host sockets");
    return 0;
}

#else

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32) && POM2_HAS_SERIAL
// A pseudo-terminal stands in for a USB CDC device: `ptsname` names a real
// device node that SerialPort::open() opens exactly as it would /dev/ttyACM0.
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

namespace {

using namespace pom2;

using Handler = std::function<void(const std::vector<uint8_t>& req,
                                   std::vector<uint8_t>&       wire)>;

/// A fake FujiNet: connects to POM2's listener and answers SLIP frames.
class FakePeer
{
public:
    FakePeer(uint16_t port, Handler h) : handler_(std::move(h))
    {
        ensureSocketStack();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(isValidSocket(fd_));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = hostToNet16(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int r = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr),
                                sizeof(addr));
        assert(r == 0);
        (void)r;
        th_ = std::thread(&FakePeer::run, this);
    }

    ~FakePeer() { stop(); }

    void stop()
    {
        if (stopped_.exchange(true)) return;
        shutdownBoth(fd_);
        if (th_.joinable()) th_.join();
        closeHostSocket(fd_);
    }

    /// Requests seen so far, decoded bodies.
    std::vector<std::vector<uint8_t>> seen()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return seen_;
    }

private:
    void run()
    {
        SlipFramer rx;
        uint8_t    buf[512];
        while (!stopped_.load()) {
            if (waitSocket(fd_, SocketWait::Read, 100) != WaitResult::Ready)
                continue;
            const iolen_t got = recvSocket(fd_, buf, sizeof(buf));
            if (got <= 0) break;
            for (iolen_t i = 0; i < got; ++i) {
                if (rx.feed(buf[i]) != SlipFramer::Feed::Frame) continue;
                const std::vector<uint8_t> req = rx.frame();
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    seen_.push_back(req);
                }
                std::vector<uint8_t> wire;
                handler_(req, wire);
                if (!wire.empty())
                    sendSocket(fd_, wire.data(), wire.size());
            }
        }
    }

    Handler                            handler_;
    socket_t                           fd_ = kInvalidSocket;
    std::thread                        th_;
    std::atomic<bool>                  stopped_{false};
    std::mutex                         mtx_;
    std::vector<std::vector<uint8_t>>  seen_;
};

// ── Response builders, straight from the spec's tables ───────────────────

void pushResponse(std::vector<uint8_t>& wire, uint8_t seq, uint8_t status,
                  const std::vector<uint8_t>& payload = {})
{
    std::vector<uint8_t> body{ seq, status };
    body.insert(body.end(), payload.begin(), payload.end());
    SlipFramer::encode(body, wire);
}

/// A SmartPort DIB: status byte, 3-byte block count, ID length, 16-byte ID,
/// type, subtype, 2-byte version.
std::vector<uint8_t> makeDib(const std::string& name, uint32_t blocks,
                             uint8_t type, uint8_t subtype)
{
    std::vector<uint8_t> d;
    d.push_back(0xF8);                                   // general status
    d.push_back(static_cast<uint8_t>(blocks & 0xFF));
    d.push_back(static_cast<uint8_t>((blocks >> 8) & 0xFF));
    d.push_back(static_cast<uint8_t>((blocks >> 16) & 0xFF));
    d.push_back(static_cast<uint8_t>(name.size()));
    for (size_t i = 0; i < 16; ++i)
        d.push_back(i < name.size() ? static_cast<uint8_t>(name[i]) : ' ');
    d.push_back(type);
    d.push_back(subtype);
    d.push_back(0x01);
    d.push_back(0x00);
    return d;
}

/// The canonical fake: two block devices, nothing at unit 3.
void standardHandler(const std::vector<uint8_t>& req, std::vector<uint8_t>& wire)
{
    assert(req.size() >= 11);
    const uint8_t seq  = req[0];
    const uint8_t cmd  = req[1];
    const uint8_t unit = req[3];

    switch (cmd) {
    case kSpInit:
        pushResponse(wire, seq, unit <= 2 ? 0x00 : 0x01);
        return;

    case kSpStatus: {
        const uint8_t code = req[6];
        if (code == 0x03) {
            pushResponse(wire, seq, 0x00,
                         makeDib(unit == 1 ? "FUJINET" : "SD", 0x1234, 0x02, 0x20));
        } else {
            pushResponse(wire, seq, 0x00, { 0xF8, 0x34, 0x12, 0x00 });
        }
        return;
    }

    case kSpReadBlock: {
        const uint32_t block = static_cast<uint32_t>(req[6]) |
                               (static_cast<uint32_t>(req[7]) << 8) |
                               (static_cast<uint32_t>(req[8]) << 16);
        std::vector<uint8_t> data(512);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>((block * 7 + i * 3) & 0xFF);
        // Make sure the framing escapes are exercised on every read.
        data[0] = 0xC0; data[1] = 0xDB; data[2] = 0xC0; data[511] = 0xDB;
        pushResponse(wire, seq, 0x00, data);
        return;
    }

    case kSpWriteBlock:
        pushResponse(wire, seq, 0x00);
        return;

    default:
        pushResponse(wire, seq, 0x00);
        return;
    }
}

// ── Harness ──────────────────────────────────────────────────────────────

/// Start a link on a free loopback port. CI runs tests in parallel, so a
/// fixed port would be flaky; walk a small range instead.
uint16_t startLink(SpOverSlipLink& link)
{
    std::string err;
    for (uint16_t p = 19850; p < 19890; ++p) {
        link.setTcpMode(p);
        if (link.start(err)) return p;
    }
    std::fprintf(stderr, "could not bind any test port: %s\n", err.c_str());
    assert(false);
    return 0;
}

/// Spin until `pred` or the deadline. Returns whether it came true.
template <typename F>
bool waitFor(F pred, int timeoutMs = 4000)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// ── 1. Enumeration ───────────────────────────────────────────────────────
void testEnumeration()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    FakePeer peer(port, standardHandler);
    assert(waitFor([&] { return link.isConnected(); }));
    assert(waitFor([&] { return link.deviceCount() == 2; }));

    const auto devs = link.devices();
    assert(devs.size() == 2);               // NOT 3 — see header comment
    assert(devs[0].unit == 1);
    assert(devs[0].name == "FUJINET");      // DIB decoded, padding trimmed
    assert(devs[0].blocks == 0x1234);
    assert(devs[0].type == 0x02);
    assert(devs[1].unit == 2);
    assert(devs[1].name == "SD");

    peer.stop();
    link.stop();
}

// ── 2. A block round trip ────────────────────────────────────────────────
void testReadWriteBlock()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    FakePeer peer(port, standardHandler);
    assert(waitFor([&] { return link.deviceCount() == 2; }));

    const auto r = link.readBlock(1, 0x000102);
    assert(r.ok());
    assert(r.data.size() == 512);
    // The bytes the framing has to escape must survive verbatim.
    assert(r.data[0] == 0xC0);
    assert(r.data[1] == 0xDB);
    assert(r.data[2] == 0xC0);
    assert(r.data[511] == 0xDB);
    for (size_t i = 3; i < 511; ++i)
        assert(r.data[i] == static_cast<uint8_t>((0x000102 * 7 + i * 3) & 0xFF));

    // A write must put the 3-byte block number on the wire little-endian and
    // carry the payload after the 11-byte header.
    std::vector<uint8_t> out(512, 0xDB);
    out[7] = 0xC0;
    const auto w = link.writeBlock(2, 0x0300FF, out.data(), out.size());
    assert(w.ok());

    bool found = false;
    for (const auto& req : peer.seen()) {
        if (req.size() != 11 + 512 || req[1] != kSpWriteBlock) continue;
        assert(req[3] == 2);                       // unit
        assert(req[6] == 0xFF);                    // block low
        assert(req[7] == 0x00);                    // block mid
        assert(req[8] == 0x03);                    // block high
        assert(std::memcmp(req.data() + 11, out.data(), out.size()) == 0);
        found = true;
    }
    assert(found);

    peer.stop();
    link.stop();
}

// ── 3. The stale response the sequence number exists for ─────────────────
void testStaleResponseDiscarded()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    std::atomic<bool> injectStale{false};
    FakePeer peer(port, [&](const std::vector<uint8_t>& req,
                            std::vector<uint8_t>& wire) {
        if (injectStale.load() && req[1] == kSpReadBlock) {
            // A leftover answer from before a guest reset: right shape,
            // wrong sequence number. The link must throw it away and keep
            // waiting for the one it asked for.
            std::vector<uint8_t> bogus(512, 0xEE);
            pushResponse(wire, static_cast<uint8_t>(req[0] ^ 0x5A), 0x00, bogus);
        }
        standardHandler(req, wire);
    });

    assert(waitFor([&] { return link.deviceCount() == 2; }));
    const uint64_t staleBefore = link.stats().stale;

    injectStale.store(true);
    const auto r = link.readBlock(1, 0x000005);
    assert(r.ok());
    assert(r.data.size() == 512);
    // The data returned is the REAL one, not the decoy.
    assert(r.data[100] != 0xEE ||
           r.data[100] == static_cast<uint8_t>((0x05 * 7 + 100 * 3) & 0xFF));
    assert(r.data[100] == static_cast<uint8_t>((0x05 * 7 + 100 * 3) & 0xFF));
    assert(link.stats().stale > staleBefore);

    peer.stop();
    link.stop();
}

// ── 4. A silent peer times out, and the link keeps working ───────────────
void testTimeoutAndRecovery()
{
    SpOverSlipLink link;
    link.setTimeoutMs(120);
    const uint16_t port = startLink(link);

    std::atomic<bool> mute{false};
    FakePeer peer(port, [&](const std::vector<uint8_t>& req,
                            std::vector<uint8_t>& wire) {
        if (mute.load() && req[1] == kSpReadBlock) return;   // answer nothing
        standardHandler(req, wire);
    });

    assert(waitFor([&] { return link.deviceCount() == 2; }));

    mute.store(true);
    const auto t0 = std::chrono::steady_clock::now();
    const auto bad = link.readBlock(1, 1);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    assert(!bad.replied);                 // caller turns this into $27
    assert(!bad.ok());
    assert(ms >= 100);                    // it really waited its budget...
    assert(ms < 2000);                    // ...and it really gave up
    assert(link.stats().timeouts >= 1);

    // The link must still be usable — a timeout is not a teardown.
    mute.store(false);
    const auto good = link.readBlock(1, 2);
    assert(good.ok());
    assert(good.data.size() == 512);

    peer.stop();
    link.stop();
}

// ── A peer that never answers is dropped, not paid for on every call ────
//
// The bug this pins is not "a call can time out" — testTimeoutAndRecovery
// covers that, and one timeout is an ordinary hiccup. It is that a bounded
// stall repeated without bound is not bounded: transact() waits inside a
// SmartPort call, on the CPU thread, holding the emulator's stateMutex, so a
// helper that ACCEPTS writes and never replies used to cost the full budget
// per call FOREVER. A ProDOS boot became a string of quarter-second freezes,
// and the FujiNet panel's own Stop button was unreachable because drawing it
// needs that same mutex. A write failure already declared the peer lost; a
// silence did not.
void testSilentPeerIsDroppedRatherThanPaidForEveryCall()
{
    SpOverSlipLink link;
    link.setTimeoutMs(120);
    const uint16_t port = startLink(link);

    std::atomic<bool> mute{false};
    FakePeer peer(port, [&](const std::vector<uint8_t>& req,
                            std::vector<uint8_t>& wire) {
        if (mute.load()) return;              // accept the write, answer nothing
        standardHandler(req, wire);
    });

    assert(waitFor([&] { return link.deviceCount() == 2; }));
    assert(link.isConnected());

    mute.store(true);

    // Each call costs its budget until the link gives up. The threshold is 3,
    // so three calls is the whole price of a dead helper.
    int paid = 0;
    for (int i = 0; i < 3; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto r  = link.readBlock(1, static_cast<uint32_t>(i));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        assert(!r.replied);
        if (ms >= 100) ++paid;
    }
    assert(paid >= 1 && "the link never actually waited — the test proves nothing");

    // Now the important half: the link has dropped the peer, so further calls
    // cost nothing at all.
    assert(!link.isConnected() &&
           "a peer that answered nothing three times is still held open");

    const auto t0 = std::chrono::steady_clock::now();
    const auto after = link.readBlock(1, 99);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    assert(!after.replied);
    assert(ms < 60 &&
           "a call after the peer was dropped still paid the timeout budget — "
           "every guest block read would freeze the machine again");

    peer.stop();
    link.stop();
}

// ── No peer at all: calls fail immediately, nothing hangs ────────────────
void testNoPeer()
{
    SpOverSlipLink link;
    startLink(link);

    assert(!link.isConnected());
    assert(link.deviceCount() == 0);

    const auto t0 = std::chrono::steady_clock::now();
    const auto r = link.readBlock(1, 0);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    assert(!r.replied);
    assert(ms < 100);        // no peer = no wait at all, not a full timeout

    link.stop();
}

// ── Guest reset: devices are told, and the sequence moves on ─────────────
void testGuestResetNotifiesDevices()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    FakePeer peer(port, standardHandler);
    assert(waitFor([&] { return link.deviceCount() == 2; }));

    link.notifyGuestReset();

    // Both units must have been told, with control code $00 — that is what
    // makes a modem hang up and a printer eject on Ctrl-Reset.
    int controls = 0;
    for (const auto& req : peer.seen())
        if (req.size() >= 11 && req[1] == kSpControl && req[6] == 0x00) ++controls;
    assert(controls == 2);

    peer.stop();
    link.stop();
}

// ── stop() must be safe with a peer attached and mid-poll ────────────────
void testCleanShutdown()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);
    {
        FakePeer peer(port, standardHandler);
        assert(waitFor([&] { return link.isConnected(); }));
        link.stop();               // with the peer still connected
    }
    assert(!link.isConnected());
    link.stop();                   // idempotent
}

// ── A peer that leaves while the guest is idle must be noticed ───────────
//
// The worker owns peer lifetime (fujinet_plan.md §6.4), but for a while it
// only slept once a peer was attached and left the discovery to the CPU
// thread's failed reads. So a helper that exited while the guest sat at the
// BASIC prompt was never noticed: isConnected() stayed true forever, the panel
// kept naming a corpse, and because pollForPeer() is gated behind !isOpen() a
// REPLACEMENT peer sat unaccepted in the listen backlog until the guest
// happened to issue a SmartPort call.
void testIdlePeerDeathIsNoticed()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    {
        FakePeer peer(port, standardHandler);
        assert(waitFor([&] { return link.deviceCount() == 2; }));
        assert(link.isConnected());
        peer.stop();                    // leaves with NO guest traffic in flight
    }

    // No readBlock()/status() call here on purpose — the worker has to find
    // this on its own.
    assert(waitFor([&] { return !link.isConnected(); }));
    assert(link.deviceCount() == 0);

    // And the slot is genuinely free again: a replacement is accepted without
    // the guest having to poke the bus first.
    {
        FakePeer peer2(port, standardHandler);
        assert(waitFor([&] { return link.deviceCount() == 2; }));
        assert(link.isConnected());
        peer2.stop();
    }

    link.stop();
}

// ── stop() must not leave half a frame in the decoder ────────────────────
//
// peerLostLocked() resets the framer precisely so a dead peer's bytes cannot
// glue onto the next peer's first packet. stop() tore the transport down
// without doing the same, so a peer that went quiet mid-frame left the framer
// in Body: the NEXT peer's leading $C0 then closed the stale body into a
// bogus frame and its real response was swallowed as line noise.
void testStopResetsTheFramer()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    {
        // A peer that emits half a frame and then stays connected and silent.
        FakePeer peer(port, [](const std::vector<uint8_t>&,
                               std::vector<uint8_t>& wire) {
            wire.push_back(0xC0);       // frame start
            wire.push_back(0x00);       // one body byte… and then nothing
        });
        assert(waitFor([&] { return link.isConnected(); }));
        // Let the enumeration time out against the truncated frame.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        link.stop();
    }

    // A fresh peer on a fresh link must enumerate normally. With a stale body
    // carried across the restart, its first frame was consumed as garbage and
    // the INIT sweep lost a reply.
    const uint16_t port2 = startLink(link);
    {
        FakePeer peer(port2, standardHandler);
        assert(waitFor([&] { return link.deviceCount() == 2; }));
        peer.stop();
    }
    link.stop();
}

/// A CONTROL request must carry the control list WITH its 2-byte
/// little-endian length prefix. The peer's parser skips exactly 11 header
/// bytes + 2 length bytes before reading the list
/// (FujiNetWIFI/AppleWin, source/devrelay/types/Request.cpp), so a bare list
/// is not merely misread: for a list SHORTER than two bytes the peer's
/// iterator runs past the end of the packet, it throws std::length_error,
/// does not catch it, and the whole FujiNet process aborts. A one-byte
/// control list is a real request (that is a FujiNet device-control call),
/// which is why this used to kill the peer in normal use.
void testControlListFraming()
{
    SpOverSlipLink link;
    std::vector<uint8_t> seen;          // the raw request the peer received
    std::mutex seenMtx;

    const uint16_t port = startLink(link);
    FakePeer peer(port, [&](const std::vector<uint8_t>& req,
                            std::vector<uint8_t>& wire) {
        if (req.size() >= 11 && req[1] == kSpControl) {
            std::lock_guard<std::mutex> lk(seenMtx);
            seen = req;
        }
        standardHandler(req, wire);
    });
    assert(waitFor([&] { return link.deviceCount() == 2; }));

    const uint8_t oneByte[1] = { 0x42 };
    const auto r = link.control(1, 0xD6, oneByte, sizeof oneByte);
    assert(r.replied);

    // Copy and RELEASE: the peer runs on its own thread and takes this mutex
    // to record each request, so holding it across the next call would
    // deadlock the reply and look like a timeout.
    std::vector<uint8_t> got;
    { std::lock_guard<std::mutex> lk(seenMtx); got = seen; }

    // 11-byte header, then the length prefix, then the list itself.
    assert(got.size() == 11 + 2 + 1);
    assert(got[1] == kSpControl);
    assert(got[6] == 0xD6);             // control code rides in the fields
    assert(got[11] == 0x01);            // length low
    assert(got[12] == 0x00);            // length high
    assert(got[13] == 0x42);            // the list
    // The invariant the peer actually relies on: there IS a byte at 11+2.
    assert(got.size() > 11 + 2);

    // A longer list keeps its prefix too — otherwise its first two bytes get
    // eaten as the length, which is what made the guest-side CONFIG program
    // read empty host and drive slots.
    const uint8_t five[5] = { 1, 2, 3, 4, 5 };
    const auto r2 = link.control(2, 0xE0, five, sizeof five);
    assert(r2.replied);
    { std::lock_guard<std::mutex> lk(seenMtx); got = seen; }
    assert(got.size() == 11 + 2 + 5);
    assert(got[11] == 0x05 && got[12] == 0x00);
    for (int i = 0; i < 5; ++i) assert(got[13 + i] == static_cast<uint8_t>(i + 1));

    std::puts("[ OK ] CONTROL carries the 2-byte control-list length prefix");
}

// ── stop() must not wait out an enumeration sweep ────────────────────────
//
// `stop()` JOINS the worker, and the worker can be inside a device sweep of
// up to kMaxUnits round trips. That join used to happen with the emulator's
// state mutex held (NetworkCoordinator's panel apply, and ~FujiNetCard through
// SlotBus::plug), which is the freeze CLAUDE.md forbids: CPU worker blocked on
// its next chunk, UI thread blocked trying to paint, and the panel's own Stop
// button unreachable because drawing it needs the same mutex. The stop is off
// that lock now, and the sweep re-reads the stop flag once per unit so it
// cannot outlast the unit it is on.
//
// The peer here answers EVERY unit, 50 ms at a time, so a full sweep is
// kMaxUnits × 50 ms ≈ 1.6 s. No timeouts are involved — that matters, because
// the consecutive-timeout guard would otherwise drop the peer and end the
// sweep early for the wrong reason.
void testStopDuringEnumerationIsPrompt()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    FakePeer peer(port, [](const std::vector<uint8_t>& req,
                           std::vector<uint8_t>& wire) {
        if (req.size() >= 11 && req[1] == kSpInit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            pushResponse(wire, req[0], 0x00);       // every unit is a device
            return;
        }
        standardHandler(req, wire);
    });

    assert(waitFor([&] { return link.isConnected(); }));
    // Solidly mid-sweep, with ~28 units still to go.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto t0 = std::chrono::steady_clock::now();
    link.stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    assert(ms < 900 && "stop() waited out the enumeration sweep");

    peer.stop();
}

#if !defined(_WIN32) && POM2_HAS_SERIAL
// ── A serial read must notice shutdown(), not wait out its budget ────────
//
// The TCP transport's shutdown() breaks a parked reader by ::shutdown()ing the
// socket under it. A serial line has nothing equivalent — shutdown() can only
// set a latch — so a readSome() that never looks at that latch holds
// SpOverSlipLink::stop() for the caller's whole budget, up to the panel's 5 s
// maximum. That stop is joined from ~FujiNetCard, which runs under the machine
// lock.
void testSerialReadNoticesShutdown()
{
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    assert(master >= 0);
    assert(::grantpt(master) == 0);
    assert(::unlockpt(master) == 0);
    const char* slave = ::ptsname(master);
    assert(slave != nullptr);

    SpSerialTransport t(slave, 115200);
    assert(t.pollForPeer(0) && "opening the device IS the connect event");
    assert(t.isOpen());

    // Nothing will ever arrive on this line: the master end is never written.
    std::thread stopper([&t] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        t.shutdown();
    });

    uint8_t buf[64];
    const auto t0 = std::chrono::steady_clock::now();
    const int r = t.readSome(buf, sizeof buf, 3000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    stopper.join();

    assert(r < 0 && "a stop reads as 'the peer is gone', which ends the call");
    assert(ms < 1000 && "readSome waited out its whole budget after shutdown()");

    t.dropPeer();
    ::close(master);
}
#endif  // !_WIN32 && POM2_HAS_SERIAL

} // namespace

int main()
{
    testEnumeration();
    testReadWriteBlock();
    testStaleResponseDiscarded();
    testTimeoutAndRecovery();
    testSilentPeerIsDroppedRatherThanPaidForEveryCall();
    testNoPeer();
    testGuestResetNotifiesDevices();
    testCleanShutdown();
    testIdlePeerDeathIsNoticed();
    testStopResetsTheFramer();
    testControlListFraming();
    testStopDuringEnumerationIsPrompt();
#if !defined(_WIN32) && POM2_HAS_SERIAL
    testSerialReadNoticesShutdown();
#endif

    std::puts("sp_over_slip_link: OK");
    return 0;
}

#endif // POM2_HAS_SOCKETS
