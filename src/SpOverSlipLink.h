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

// SpOverSlipLink — the SmartPort-over-SLIP session layer.
//
// Owns everything between "a byte pipe" (SpTransport) and "a SmartPort call"
// (FujiNetCard): request sequence numbers, the bounded round trip, device
// enumeration, and the worker thread that watches for a peer appearing or
// dying. It knows nothing about the 6502 and nothing about which transport it
// holds.
//
// Protocol reference: the FujiNet wiki page "Apple II SP over SLIP"
// (https://github.com/FujiNetWIFI/fujinet-firmware/wiki/Apple-II-SP-over-SLIP,
// revision of 2025-01-25), which supplements the Apple IIc Technical Reference
// ch. 6 and the Apple IIgs Firmware Reference ch. 7. Cross-checked against the
// FujiNet AppleWin fork's `source/devrelay/`.
//
// ── Wire format ───────────────────────────────────────────────────────────
//
// Every request is an 11-byte header, optionally followed by data; every
// response is a 2-byte header, optionally followed by data. The header's
// first byte is always the REQUEST SEQUENCE NUMBER, which exists for one
// specific failure: an Apple II reset after a request was sent but before its
// response was read leaves that response sitting in a TCP or USB buffer, so
// the NEXT call would read somebody else's answer. Matching the number on the
// way back is what makes a guest reset survivable.
//
//   off  0    request sequence number
//   off  1    command number
//   off  2    parameter count
//   off  3    SmartPort unit number
//   off  4-5  reserved ($00)
//   off  6-10 command-specific (status code / block number / byte count +
//             address), zero-padded
//
// Multi-byte fields are LITTLE-ENDIAN, matching the SmartPort parameter lists
// they are copied from (block number low/mid/high, as ProDOS lays it out at
// $46/$47).
//
// ── Threading ─────────────────────────────────────────────────────────────
//
// `transact()` and every typed call run on the CPU thread, inside a SmartPort
// call, under EmulationController's stateMutex — blocking, bounded by
// `timeoutMs()` (250 ms by default). See docs/fujinet_plan.md § 9 for why
// blocking there is the right answer and not a compromise.
//
// The worker thread only acquires and releases peers, and enumerates devices
// when one appears. Calls from the two threads serialise on `callMtx_`, so
// two requests can never be in flight at once and a response can never be
// matched to the wrong caller.

#ifndef POM2_SP_OVER_SLIP_LINK_H
#define POM2_SP_OVER_SLIP_LINK_H

#include "SlipFramer.h"
#include "SpTransport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "FujiNetLink.h"
#include "FujiNetTransport.h"

namespace pom2 {

/// The production FujiNetLink: SLIP framing over TCP loopback or a USB CDC
/// device, plus the helper process, timeouts and resync the interface
/// deliberately does not expose.
class SpOverSlipLink : public FujiNetLink, public FujiNetTransport
{
public:
    // kDefaultTimeoutMs is inherited from FujiNetTransport.
    static constexpr int kMinTimeoutMs     = 50;
    static constexpr int kMaxTimeoutMs     = 5000;

    /// Ceiling on the INIT enumeration sweep.
    ///
    /// This was 8, and 8 is wrong for the device POM2 exists to talk to. A
    /// FujiNet's SmartPort chain is not "up to eight drives": it is the disk
    /// slots FOLLOWED BY the Fuji control device, the `N:` network device,
    /// the clock, the printer, the modem and CP/M (lib/device/iwm/ in the
    /// firmware — disk, iwmFuji, network, clock, modem, cpm). With the sweep
    /// stopping at 8 the enumeration saw eight FUJINET_DISK_* units and
    /// nothing else, and — because the guest's "how many devices?" call is
    /// answered locally from that count — the guest never probed past unit 8
    /// either. Every non-disk FujiNet function was therefore invisible to
    /// guest software: NETCAT got as far as "NETWORK NOT FOUND".
    ///
    /// The sweep stops on the first unit whose INIT does not answer $00, so
    /// this is only a safety ceiling, not a cost: a chain of two still costs
    /// two round trips. SmartPort itself addresses up to 127 units; 32 is far
    /// past anything a FujiNet presents while still bounding a peer that
    /// answers $00 to everything.
    static constexpr uint8_t kMaxUnits = 32;

    // Response is inherited from FujiNetLink: the reply shape belongs to the
    // SmartPort protocol both ends speak, not to this transport.


    SpOverSlipLink();
    ~SpOverSlipLink();

    SpOverSlipLink(const SpOverSlipLink&)            = delete;
    SpOverSlipLink& operator=(const SpOverSlipLink&) = delete;

    // ── Transport selection ───────────────────────────────────────────────
    // Exclusive: TCP or serial, never both. Two peers would mean two device
    // number spaces to merge, which no real configuration needs. Changing
    // transport while running restarts the worker.

    // Mode and Stats are inherited from FujiNetTransport.

    void setTcpMode(uint16_t port) override;
    void setSerialMode(std::string devicePath, int baud) override;
    void setOff() override;
    Mode mode() const override { return mode_; }

    /// The configured transport parameters, whether or not that transport is
    /// the active one — the panel edits both and the host persists both, so a
    /// user who switches TCP → serial → TCP does not lose their port.
    uint16_t           tcpPort()    const override { return tcpPort_; }
    const std::string& serialPath() const override { return serialPath_; }
    int                serialBaud() const override { return serialBaud_; }

    /// Start the worker. Returns false (with `errOut` filled) when the chosen
    /// transport could not even be armed — a TCP port already in use, say.
    bool start(std::string& errOut) override;
    /// Stop the worker, join it, and release the peer. Safe to call twice.
    void stop() override;
    bool isRunning() const override { return running_.load(); }

    // ── State for the panel ───────────────────────────────────────────────
    bool        isConnected() const override;
    std::string describe() const override;
    std::string lastError() const override;
    std::vector<SpDevice> devices() const override;
    std::size_t deviceCount() const override;
    Stats       stats() const override;

    int  timeoutMs() const override { return timeoutMs_.load(); }
    void setTimeoutMs(int ms) override;

    // ── SmartPort calls (CPU thread) ──────────────────────────────────────
    // `unit` is the SmartPort unit number, 1-based. Unit 0 addresses the bus
    // itself and is answered locally (see FujiNetCard), never forwarded.

    Response status(uint8_t unit, uint8_t statusCode) override;
    Response readBlock(uint8_t unit, uint32_t block) override;
    Response writeBlock(uint8_t unit, uint32_t block,
                        const uint8_t* data, std::size_t n) override;
    Response format(uint8_t unit) override;
    Response control(uint8_t unit, uint8_t controlCode,
                     const uint8_t* list, std::size_t n) override;
    Response init(uint8_t unit) override;
    Response open(uint8_t unit) override;
    Response close(uint8_t unit) override;
    Response read(uint8_t unit, uint16_t byteCount, uint32_t address) override;
    Response write(uint8_t unit, uint16_t byteCount, uint32_t address,
                   const uint8_t* data, std::size_t n) override;

    /// Guest reset. Bumps the sequence number so a response in flight for the
    /// pre-reset request can never be mistaken for an answer to the next one,
    /// then tells every enumerated device (Control code $00) so a modem drops
    /// its connection and a printer ejects its partial page — the behaviour
    /// the spec asks the Apple II side to provide.
    void notifyGuestReset() override;

    /// Move the sequence number on and drop any half-decoded frame, WITHOUT
    /// telling the devices anything. This is the rewind case: the guest's
    /// clock went backwards, so an in-flight response must not be matched to
    /// the next request — but the peer did not reset, and telling a modem to
    /// hang up because the *user* rewound would be wrong. See
    /// FujiNetCard::loadSnapshotState.
    void resync() override;

private:
    /// One request/response exchange. `fields` is the 5 command-specific
    /// bytes at offsets 6-10. Returns a Response whose `replied` is false on
    /// timeout or a dead peer.
    Response transact(uint8_t command, uint8_t paramCount, uint8_t unit,
                      const uint8_t fields[5],
                      const uint8_t* data, std::size_t dataLen);

    void workerLoop();
    void enumerateDevices();
    /// Peer teardown. TWO forms on purpose: `transact()` discovers the loss
    /// while it already holds `callMtx_`, and std::mutex is not recursive —
    /// taking it again there deadlocks the CPU thread with the emulated 6502
    /// parked mid-SmartPort-call, which is as bad as it sounds.
    void peerLostLocked();   ///< callMtx_ MUST be held

    /// Consecutive `transact()` calls that timed out with no reply. Guarded
    /// by `callMtx_` — transact() holds it for the whole exchange, and
    /// peerLostLocked() is only ever reached with it held.
    ///
    /// This exists because a bounded stall repeated without bound is not
    /// bounded. `transact()` waits up to `timeoutMs_` (250 ms default) inside
    /// a SmartPort call, on the CPU thread, holding the emulator's stateMutex
    /// — so a peer that ACCEPTS writes but never answers used to cost that
    /// much per call, for every call, forever: a ProDOS boot became a string
    /// of quarter-second freezes with the FujiNet panel's own Stop button
    /// unreachable, because drawing it needs the same mutex. A write failure
    /// already declared the peer lost; a silence did not.
    unsigned consecutiveTimeouts_ = 0;
    void handlePeerLost();   ///< callMtx_ must NOT be held

    uint8_t nextSequence();

    Mode                       mode_ = Mode::Off;
    uint16_t                   tcpPort_ = SpTcpTransport::kDefaultPort;
    std::string                serialPath_;
    int                        serialBaud_ = SerialPort::kDefaultBaud;

    /// Owned. Created by start() and destroyed by stop() — the destruction
    /// under `callMtx_`, because transact() reaches it through a raw pointer
    /// and only that mutex keeps the two apart (see stop()).
    std::unique_ptr<SpTransport> transport_;

    /// Guards the transport POINTER for the status readers, and nothing else.
    ///
    /// `isConnected()` / `describe()` / `lastError()` dereferenced `transport_`
    /// with no lock at all while `stop()` reset it under `callMtx_` — a
    /// use-after-free reachable from the CPU thread (FujiNetCard asks
    /// isConnected() on every guest SmartPort access) and from the UI thread
    /// every frame.
    ///
    /// A SECOND mutex rather than `callMtx_`, and that is the point: callMtx_
    /// is held for a whole request/response exchange, so a status reader
    /// taking it would park the UI thread — and through the emulator's
    /// stateMutex the CPU worker with it — for a full call budget behind a
    /// silent peer. The three methods this guards call only `isOpen()` /
    /// `describe()` / `lastError()`, which are documented never to block, so
    /// this mutex is never held across a syscall. `stop()` takes callMtx_
    /// first (excluding transact) and this one second (excluding the readers)
    /// before destroying the transport; lock order is callMtx_ → transportMtx_
    /// and never the reverse.
    mutable std::mutex           transportMtx_;

    std::thread                worker_;
    std::atomic<bool>          running_{false};
    std::atomic<bool>          stopFlag_{false};
    std::atomic<int>           timeoutMs_{kDefaultTimeoutMs};

    /// Lifetime tracking for the CURRENT peer, so peerLostLocked() can say
    /// how long it held and how much it served. Written by the worker thread
    /// only (connect and loss both happen there).
    std::chrono::steady_clock::time_point peerSince_{};
    uint64_t peerCallsAtConnect_    = 0;
    uint64_t peerTimeoutsAtConnect_ = 0;
    void notePeerConnected();

    /// Serialises whole request/response exchanges. Held by transact() for
    /// its entire duration, so the worker's enumeration and the CPU thread's
    /// SmartPort calls can never have two requests in flight at once.
    mutable std::mutex         callMtx_;
    SlipFramer                 rx_;            ///< guarded by callMtx_
    uint8_t                    sequence_ = 0;  ///< guarded by callMtx_
    std::vector<uint8_t>       txBuf_;         ///< guarded by callMtx_

    mutable std::mutex         stateMtx_;      ///< guards devices_/lastError_
    std::vector<SpDevice>      devices_;
    std::string                lastError_;

    mutable std::mutex         statsMtx_;
    Stats                      stats_;
};

} // namespace pom2

#endif // POM2_SP_OVER_SLIP_LINK_H
