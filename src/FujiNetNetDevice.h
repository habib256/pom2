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

// FujiNetNetDevice — POM2's OWN `N:` network device.
//
// The FujiNet's headline feature is `N:`: a deported TCP/IP stack the guest
// drives with simple commands, so an Apple II gets HTTP without running a byte
// of TCP/IP itself. POM2 normally relays that to a real FujiNet — but the
// desktop firmware build answers the guest's open with success and then never
// opens a socket at all (measured 2026-08-21 on the peer's own descriptors;
// the same firmware opens real TCP for TNFS on the same machine), so on that
// peer `N:` is inert and no amount of relaying helps.
//
// This is the answer: when the built-in network device is enabled, POM2 serves
// the `N:` unit itself, out of host sockets. The rest of the chain — disks,
// CONFIG, the clock — still goes to the peer, which handles them well. See
// TODO § [Network] for the "native + relay coexisting" decision this
// implements.
//
// ── Protocol ──────────────────────────────────────────────────────────────
//
// Command bytes are ASCII letters, from the firmware's own table
// (`include/fujiCommandID.h`):
//
//     'O' 0x4F  OPEN    control list carries the devicespec, e.g.
//                       "N:HTTP://THEOLDNET.COM/"
//     'C' 0x43  CLOSE
//     'S' 0x53  STATUS  4-byte reply: bytes-waiting (LE 16), connected, error
//     'R' 0x52  READ    (data itself comes back through SmartPort READ)
//     'W' 0x57  WRITE
//     'E' 0x45  GET_ERROR
//
// The status reply's byte count is capped at 512 by the firmware, and this
// follows suit: guest code sizes its buffer from that number.
//
// ── Scope, deliberately ───────────────────────────────────────────────────
//
// HTTP over plain TCP, which is what the retro web actually serves (and what
// theoldnet.com — the reason this exists — speaks). No TLS: that would mean
// carrying a TLS stack, and `docs/fujinet_plan.md` § 2 is right that
// reimplementing the firmware's whole network stack buys nothing. No SSH, no
// JSON parser, no `N:` filesystem verbs. A real FujiNet board over USB
// remains the way to get all of those; this exists so that the machine can
// browse at all when the peer's own `N:` cannot.

#ifndef POM2_FUJINET_NET_DEVICE_H
#define POM2_FUJINET_NET_DEVICE_H

#include "FujiNetNetwork.h"
#include "SocketCompat.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pom2 {

// The kNet* command bytes live in FujiNetNetwork.h — they are protocol,
// and the card decodes them without knowing this implementation exists.

class FujiNetNetDevice : public FujiNetNetwork {
public:
    /// The firmware caps a status reply's byte count at 512 and guest code
    /// sizes its buffer from it, so promising more would overrun the guest.
    static constexpr uint16_t kMaxStatusAvail = 512;

    ~FujiNetNetDevice();

    /// Open a devicespec: "N:HTTP://host[:port]/path" (the "N:" and the
    /// scheme are case-insensitive, as the guest usually shouts them). The
    /// whole response body is fetched and buffered — the guest then drains it
    /// with STATUS/READ, which is exactly the shape its code expects and
    /// avoids holding a socket open across an emulated machine's lifetime.
    ///
    /// THE FETCH DOES NOT HAPPEN HERE. This is called from a SmartPort call,
    /// on the CPU thread, with the emulator's state mutex held, and a fetch
    /// is DNS + connect + a whole HTTP body: bounded at 12 s by
    /// `deadlineMs_`, which is 12 s of frozen machine and frozen window, with
    /// the FujiNet panel's own controls unreachable because drawing them
    /// needs the same mutex. So `open()` STARTS the fetch on a worker and
    /// returns at once.
    ///
    /// The return value is therefore "accepted", not "succeeded": false only
    /// for a spec that cannot be served at all (not HTTP over plain TCP, no
    /// host, a bad port). The verdict arrives through `status()` — which is
    /// how the guest is meant to drive `N:` anyway (STATUS until bytes are
    /// waiting, then READ), and it reads "connected, nothing waiting yet"
    /// while the fetch is in flight, exactly as it does for a slow server.
    bool open(const std::string& devicespec) override;
    void close() override;

    /// True from `open()` until `close()`, a fetch still in flight included:
    /// the session exists, whatever its bytes are doing.
    bool isOpen() const override { pump(); return open_ || fetch_ != nullptr; }

    /// A fetch started by `open()` has not landed yet. Nothing is readable
    /// while this is true, and `status()` says "connected, 0 waiting".
    bool busy() const { pump(); return fetch_ != nullptr; }

    /// 4-byte status: bytes waiting (LE 16), connected flag, error code.
    void status(uint8_t out[4]) const override;

    /// Copy up to `n` buffered bytes out. Returns how many were copied.
    std::size_t read(uint8_t* dst, std::size_t n) override;

    /// Bytes still unread.
    std::size_t available() const override
    { pump(); return body_.size() - cursor_; }

    /// Last error, in the firmware's numbering: 1 = success, 144 = general
    /// failure, 170 = file not found. Guest code compares against these.
    uint8_t lastError() const { return error_; }

    /// What the panel shows. Empty when nothing has been opened.
    const std::string& describe() const { return description_; }

    /// Wall-clock budget for a whole fetch — DNS, connect, request and body
    /// together. No longer "how long may the emulated machine freeze" (the
    /// fetch is off the CPU thread now), but still a hard ceiling: a server
    /// that drip-feeds one byte just inside a per-recv timeout would keep a
    /// worker and a socket alive for ever. Lowered by the tests so the
    /// truncation rule can be pinned in under a second.
    void setFetchDeadlineMs(int ms) { deadlineMs_ = ms; }
    int  fetchDeadlineMs() const    { return deadlineMs_; }

private:
    /// A fetch in flight, shared with the worker that performs it.
    ///
    /// Shared rather than owned so the DEVICE can be destroyed while the
    /// fetch is still running — which it can be: `~FujiNetCard` runs inside
    /// `SlotBus::plug()` with the emulator's state mutex held, and joining a
    /// 12-second fetch there would be the very freeze this class moved the
    /// fetch off the CPU thread to avoid. The device just sets `cancel` and
    /// lets go; the worker notices at its next wait slice, frees its own
    /// socket and exits. Same shape as SocketUtil.h's BoundedLookup, for the
    /// same reason.
    struct Fetch {
        std::mutex           mtx;
        bool                 done  = false;   ///< guarded by mtx
        bool                 ok    = false;   ///< guarded by mtx
        uint8_t              error = kNetErrGeneral;   ///< guarded by mtx
        std::vector<uint8_t> body;             ///< guarded by mtx
        std::atomic<bool>    cancel{false};
    };

    /// The whole fetch. Static, and it touches nothing but its own `Fetch`:
    /// the device may be gone by the time it finishes.
    static void fetchInto(Fetch& out, std::string host, uint16_t port,
                          std::string path, int deadlineMs);

    /// Publish a finished fetch into the readable state. Called by every
    /// accessor, which is what makes "the answer arrived" visible to a guest
    /// that is polling STATUS.
    void harvest();
    /// harvest() from a const accessor. The mutation is entirely of members
    /// this object owns and the alternative is marking six of them mutable.
    void pump() const { const_cast<FujiNetNetDevice*>(this)->harvest(); }

    int                  deadlineMs_ = 12000;
    bool                 open_   = false;
    uint8_t              error_  = 1;      ///< 1 = SUCCESS in the firmware's table
    std::vector<uint8_t> body_;
    std::size_t          cursor_ = 0;
    std::string          description_;
    std::string          spec_;            ///< what open() was asked for
    std::shared_ptr<Fetch> fetch_;         ///< null when nothing is in flight
};

}  // namespace pom2

#endif  // POM2_FUJINET_NET_DEVICE_H
