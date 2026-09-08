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

// AiControlServer — HTTP/1.1 localhost bridge for AI-agent control of POM2.
//
// Inspired by `paleotronic/microm8-cln` (`remint/`, `fastserv/`) and by
// modern MCP/AI-agent emulator drivers. Lets an external process — typically
// an AI agent like Claude Code via curl/MCP — drive POM2 the same way a
// human drives it from the UI: type at the keyboard, reset, mount disks,
// peek/poke RAM, save and load snapshots, scrub speed, grab the framebuffer.
//
// Wire format: plain HTTP/1.1 with JSON bodies. Hand-rolled parser, no
// third-party dependency — the SSC already proved a 200-line TCP listener
// is the right scope for POM2's "minimum external deps" policy.
//
// Endpoints (all opt-in via settings.ai_control_enable; default off):
//
//   GET  /status              → {profile, cpu_mode, mode, cycles_per_frame,
//                                cpu:{pc,a,x,y,p,sp,cycles}, disks:[...]}
//   POST /reset               → body {"kind":"soft|hard|cold"} (default hard)
//   GET  /cpu                 → CPU register dump
//   POST /cpu                 → body {"pc":?, "a":?, "x":?, "y":?} — set regs
//   GET  /mem?addr=N&len=N    → {"addr":N,"data":"FFEE..."} (hex, len ≤ 4096)
//   POST /mem?addr=N&bank=main|aux → body {"data":"FFEE..."} — bulk write (RAM)
//   POST /keyboard            → body {"text":"..."} or {"raw":"..."} — paste
//   POST /disk                → body {"slot":6,"drive":0,"path":"..."} — insert
//   POST /eject               → body {"slot":6,"drive":0} — eject
//   POST /snapshot/save       → body {"path":"....pom2snap"} — save path MUST
//                                end with `.pom2snap` so an agent can't
//                                clobber unrelated files inside cwd.
//   POST /snapshot/load       → body {"path":"..."} — any cwd-relative file;
//                                magic-byte check inside rejects non-snapshots.
//   POST /speed               → body {"cycles_per_frame":N} OR {"preset":"1x|2x|max"}
//   GET  /screen.ppm          → binary PPM of the live framebuffer
//   POST /mouse               → body {"dx":?,"dy":?} signed Apple-cursor delta
//                                (±127/call) OR {"x":?,"y":?} absolute counter,
//                                {"btn":0|1}, {"reset":1}. Drives the Mouse
//                                Card's host-motion input exactly as
//                                MainWindow::onMouseMove would — lets an agent
//                                exercise mouse-driven apps headlessly.
//
// Authentication: optional shared-secret in the `X-POM2-Token` header. When
// the configured token is empty, requests are accepted unauthenticated
// (loopback-only listener already limits exposure to local processes; an
// AI agent on the same machine doesn't need to fight a token round-trip).
// That perimeter used to include the EMULATED MACHINE: a guest driving the
// Uthernet II's host sockets, or libslirp's router, could reach 127.0.0.1
// here and looked native. Both are fenced off by default now
// (`W5100Device::checkDestination`, `SlirpOptions::allowHostLoopback`), and
// the two fences are part of this listener's threat model, not the cards'.
//
// Both modes ALSO require a loopback `Host` header (or none at all). A token
// is a secret, not an origin proof: a DNS-rebound page that guessed it would
// otherwise reach every endpoint from the browser. The token compare is
// constant-time and five failures inside five seconds put the listener into a
// 429 backoff, so a page cannot grind a human-typed secret. No CORS headers
// are emitted at all — a native client needs none, and `Access-Control-Allow-
// Origin: *` was what made the grinding cross-origin-readable in the first
// place.
//
// Threading: one worker thread, one client at a time. Each request acquires
// `EmulationController::stateMutex()` for the slice of work that touches
// CPU/Memory/slot state, mirrors the rule the UI thread already follows.
// Memory's own paste-queue mutex covers /keyboard without needing stateMtx.

#ifndef POM2_AI_CONTROL_SERVER_H
#define POM2_AI_CONTROL_SERVER_H

#include "SocketCompat.h"   // socket_t / kInvalidSocket for the listener fd

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class EmulationController;
class M6502;
class Memory;
class DiskIICard;
class ProDOSHardDiskCard;
class Apple2Display;
class MouseCard;

namespace pom2 {

struct SystemProfileSnapshot {
    std::string name;          // "Apple ][+", "Apple //e Enhanced", …
    std::string cpuMode;       // "nmos" or "65c02"
};

class AiControlServer
{
public:
    static constexpr uint16_t kDefaultPort = 6503;     // distinct from SSC's 6502

    /// Shortest token worth calling one. Below this a brute force wins even
    /// against the backoff — 16 base32-ish characters is ~80 bits, which it
    /// does not. The panel refuses to arm a shorter one; the server itself
    /// still honours whatever it was handed (a config file the user wrote by
    /// hand is their call), it only warns.
    static constexpr std::size_t kMinTokenLength = 16;

    /// A fresh 32-character token from the platform CSPRNG. What the panel's
    /// "Generate" button calls so nobody has to invent one — the previous UI
    /// offered an empty text box, and an empty box is open mode.
    static std::string generateToken();

    AiControlServer() = default;
    ~AiControlServer();

    AiControlServer(const AiControlServer&)            = delete;
    AiControlServer& operator=(const AiControlServer&) = delete;

    /// Wire the controller + display once the emulator is fully constructed.
    /// All pointers are non-owning; the server holds them across requests but
    /// never deletes them. Disk/HDV pointers may be null — the corresponding
    /// endpoints then return 503.
    ///
    /// Threading: when `ctrl_` is already bound (i.e., this is a re-attach
    /// after a profile switch), the caller MUST hold `ctrl->stateMutex()`
    /// for the duration of the call. Handlers read disk6_/hdv5_ under the
    /// same mutex, so this serialises the pointer swap against in-flight
    /// requests. On the very first call (no worker thread alive yet) the
    /// lock is optional.
    void attach(EmulationController* ctrl,
                Apple2Display*       display,
                DiskIICard*          disk6,
                ProDOSHardDiskCard*  hdv5);

    /// Null the slot-card pointers. Caller MUST hold the controller's
    /// stateMutex. Use this BEFORE the slot bus tears down its plugged
    /// cards during a profile switch — between detach() and the next
    /// attach(), card-touching endpoints return 503 instead of
    /// dereferencing freed memory. Pairs with attach().
    void detach();

    /// Update the cached display-side metadata that `/status` reports.
    /// Called by MainWindow whenever the profile or token changes.
    void setProfileLabel(const std::string& label) {
        std::lock_guard<std::mutex> lk(mtx_);
        profileLabel_ = label;
    }
    void setAuthToken(const std::string& token) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            authToken_ = token;
        }
        // Changing the secret invalidates every guess made against the old
        // one, so the penalty box is cleared with it — otherwise an operator
        // who fixes a typo in the panel is locked out of their own emulator
        // for the rest of the window.
        std::lock_guard<std::mutex> lk(authMtx_);
        authFailures_ = 0;
    }

    /// Start the TCP listener on `127.0.0.1:port`. Returns false on bind/
    /// listen failure (the previous instance, if any, is torn down first).
    /// Thread-safe.
    bool start(uint16_t port);
    /// Stop the listener and join the worker. Safe to call repeatedly.
    void stop();

    bool     isRunning() const { return running_.load(); }
    uint16_t getPort()   const { return port_;           }
    uint64_t requestsServed() const { return requestsServed_.load(); }
    std::string lastClientAddr() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastClient_;
    }

private:
    // ─── Bound emulator handles (non-owning) ─────────────────────────────
    // `ctrl_` / `display_` are set once and outlive every profile switch
    // (they are MainWindow members, not slot cards). The two CARD pointers
    // move at runtime — attach() republishes them, detach() nulls them — and
    // are read by the server thread, so they are atomic: a plain pointer
    // written by the UI thread and read by the worker is a data race even
    // when the stateMutex contract below is honoured, and a handler that
    // dereferenced the member twice could see two different cards.
    // Load ONCE into a local, then work with that local.
    EmulationController*             ctrl_    = nullptr;
    Apple2Display*                   display_ = nullptr;
    std::atomic<DiskIICard*>         disk6_   { nullptr };
    std::atomic<ProDOSHardDiskCard*> hdv5_    { nullptr };

    // ─── Listener state ──────────────────────────────────────────────────
    mutable std::mutex     mtx_;
    std::atomic<bool>      running_       { false };
    std::atomic<bool>      stopRequested_ { false };
    std::atomic<uint64_t>  requestsServed_{ 0 };
    // socket_t, not int: Winsock's SOCKET is an unsigned handle whose
    // failure value is INVALID_SOCKET rather than -1 (SocketCompat.h).
    std::atomic<socket_t>  listenFd_      { kInvalidSocket };
    // Published only while the worker owns an accepted client. stop() uses
    // it to interrupt a handler blocked in recv/send before joining.
    mutable std::mutex     clientFdMtx_;
    socket_t               clientFd_      = kInvalidSocket;
    uint16_t               port_     = kDefaultPort;
    // Serialises start()/stop()/~AiControlServer against each other. Without
    // it two threads could both pass `worker_.joinable()` and join the same
    // thread twice (std::terminate), or a stop() could close the listener fd
    // a concurrent start() had just published. NOT `mtx_`: the worker takes
    // that one on every request, and stop() joins the worker while holding
    // this. Never taken by a handler.
    std::mutex             lifecycleMtx_;
    std::thread            worker_;
    std::string            authToken_;
    std::string            profileLabel_;
    std::string            lastClient_;

    // ─── Auth-failure backoff ────────────────────────────────────────────
    // A human-typed token is short. Without a brake, a page (or any local
    // process) can try one per connection as fast as the listener accepts
    // them. Five failures inside the window arm a lockout: every further
    // request answers 429 until the window expires, whatever it carries.
    static constexpr int  kAuthFailureLimit  = 5;
    static constexpr long kAuthWindowMs      = 5000;
    mutable std::mutex    authMtx_;
    int                   authFailures_ = 0;
    std::chrono::steady_clock::time_point authWindowStart_{};

    // ─── /mouse running state ────────────────────────────────────────────
    // Running Apple-cursor counters mirrored from MainWindow's own
    // mouseAppleX/Y, so an agent can feed deltas across many requests and
    // the card sees a continuous 8-bit counter (the MCU firmware computes
    // motion via wrap-corrected subtraction — see MouseCard::updateAxis).
    // Guarded by ctrl_->stateMutex() when written (same as the card read).
    uint8_t mouseAccumX_ = 0;
    uint8_t mouseAccumY_ = 0;
    bool    mouseBtn_    = false;

    void runWorker();
    void handleClient(socket_t fd);
    /// stop() with `lifecycleMtx_` already held (start() reuses it).
    void stopLocked();

    /// True while the backoff window is armed — answer 429 and touch nothing.
    bool authBackoffArmed();
    /// Count one rejected request; arms the backoff on the fifth inside the
    /// window. `noteAuthSuccess` clears it, so a legitimate agent that
    /// mistyped once is not punished for the rest of the session.
    void noteAuthFailure();
    void noteAuthSuccess();

    // HTTP request shape (parsed in-place from the socket).
    struct Request {
        std::string method;             // "GET", "POST", …
        std::string path;               // path portion of the URL
        std::string query;              // raw query string (no leading '?')
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        std::string headerValue(const std::string& name) const;
    };

    /// True when the request's Host header names the loopback interface (or
    /// is absent). The token-less path needs this on top of the Origin test:
    /// a same-origin GET from a DNS-rebound page carries no Origin.
    static bool hostHeaderIsLoopback(const Request& req);

    /// Drain a single HTTP request from `fd` into `req`. Returns false if
    /// the peer closed before a full request landed, or if the request
    /// looks malformed (oversized body, missing terminator, etc.) — in
    /// which case the caller should send a 400 and close.
    bool readRequest(socket_t fd, Request& req);
    /// Send a response. Body may be binary; pass the right Content-Type.
    void sendResponse(socket_t fd,
                      int status,
                      const std::string& contentType,
                      const std::string& body);
    void sendJsonError(socket_t fd, int status, const std::string& message);
    void sendJsonOk   (socket_t fd, const std::string& body);

    // ─── Request handlers ────────────────────────────────────────────────
    void handleStatus  (socket_t fd, const Request& req);
    void handleReset   (socket_t fd, const Request& req);
    void handleCpuGet  (socket_t fd, const Request& req);
    void handleCpuSet  (socket_t fd, const Request& req);
    void handleMemGet  (socket_t fd, const Request& req);
    void handleMemSet  (socket_t fd, const Request& req);
    void handleKeyboard(socket_t fd, const Request& req);
    void handleDiskInsert(socket_t fd, const Request& req);
    void handleDiskEject (socket_t fd, const Request& req);
    void handleSnapshotSave(socket_t fd, const Request& req);
    void handleSnapshotLoad(socket_t fd, const Request& req);
    void handleSpeed   (socket_t fd, const Request& req);
    void handleScreen  (socket_t fd, const Request& req);
    void handleMouse   (socket_t fd, const Request& req);

    /// True when the request carries a valid auth token (or when no token
    /// is configured server-side). Caller still has to send the 401 — this
    /// is just the predicate.
    bool checkAuth(const Request& req) const;
};

} // namespace pom2

#endif // POM2_AI_CONTROL_SERVER_H
