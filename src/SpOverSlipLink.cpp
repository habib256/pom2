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

// SpOverSlipLink implementation. See the header for the wire format and the
// threading contract.

#include "SpOverSlipLink.h"

#include "Logger.h"
#include "ThreadGuard.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace pom2 {

namespace {

/// Poll interval while looking for a peer. Short enough that plugging a
/// FujiNet in feels immediate, long enough that an idle POM2 is not spinning.
constexpr int kPeerPollMs = 200;

/// The 11-byte request header every command shares.
constexpr std::size_t kRequestHeaderBytes = 11;
/// Response header: sequence number + status.
constexpr std::size_t kResponseHeaderBytes = 2;

/// A SmartPort DIB (Status code $03) as the spec's referenced manuals lay it
/// out: 4 bytes general status (status byte + 3-byte block count), a 1-byte
/// ID string length, 16 bytes of ID string, then type/subtype/version.
constexpr std::size_t kDibMinBytes = 21;

/// POM2_TRACE_FUJINET=1 turns on a per-call trace of everything crossing the
/// relay. This subsystem has three moving parts in two processes — the guest,
/// POM2, and a peer that may be a desktop build or a board on USB — and when
/// something does not work the only question that matters is WHICH of them
/// went quiet. Resolved once: the check is on the per-call path.
bool traceEnabled()
{
    static const bool on = [] {
        const char* v = std::getenv("POM2_TRACE_FUJINET");
        return v && *v && *v != '0';
    }();
    return on;
}

/// Names for the commands the trace prints, so a log line reads as protocol
/// rather than as hex.
const char* commandName(uint8_t c)
{
    switch (c) {
    case kSpStatus:     return "STATUS";
    case kSpReadBlock:  return "READ_BLOCK";
    case kSpWriteBlock: return "WRITE_BLOCK";
    case kSpFormat:     return "FORMAT";
    case kSpControl:    return "CONTROL";
    case kSpInit:       return "INIT";
    case kSpOpen:       return "OPEN";
    case kSpClose:      return "CLOSE";
    case kSpRead:       return "READ";
    case kSpWrite:      return "WRITE";
    default:            return "?";
    }
}

} // namespace

SpOverSlipLink::SpOverSlipLink() = default;

SpOverSlipLink::~SpOverSlipLink() { stop(); }

// ── Transport selection ──────────────────────────────────────────────────

void SpOverSlipLink::setTcpMode(uint16_t port)
{
    const bool wasRunning = isRunning();
    stop();
    mode_    = Mode::Tcp;
    tcpPort_ = port;
    if (wasRunning) { std::string err; start(err); }
}

void SpOverSlipLink::setSerialMode(std::string devicePath, int baud)
{
    const bool wasRunning = isRunning();
    stop();
    mode_       = Mode::Serial;
    serialPath_ = std::move(devicePath);
    serialBaud_ = baud;
    if (wasRunning) { std::string err; start(err); }
}

void SpOverSlipLink::setOff()
{
    stop();
    mode_ = Mode::Off;
}

bool SpOverSlipLink::start(std::string& errOut)
{
    if (running_.load()) return true;
    if (mode_ == Mode::Off) { errOut = "FujiNet link is disabled"; return false; }

    stopFlag_.store(false);

    if (mode_ == Mode::Tcp) {
        auto tcp = std::make_unique<SpTcpTransport>(tcpPort_);
        if (!tcp->startListening(errOut)) {
            std::lock_guard<std::mutex> lk(stateMtx_);
            lastError_ = errOut;
            return false;
        }
        transport_ = std::move(tcp);
    } else {
        transport_ = std::make_unique<SpSerialTransport>(serialPath_, serialBaud_);
    }

    {
        std::lock_guard<std::mutex> lk(stateMtx_);
        lastError_.clear();
    }
    running_.store(true);
    worker_ = pom2::guardedThread("FujiNet", [this] { workerLoop(); });
    return true;
}

void SpOverSlipLink::stop()
{
    if (!running_.load() && !worker_.joinable()) {
        std::lock_guard<std::mutex> callLk(callMtx_);
        transport_.reset();
        rx_.reset();
        return;
    }
    stopFlag_.store(true);
    // Wake a worker parked in accept()/poll() WITHOUT closing anything under
    // it — the descriptor must stay alive until the thread is gone.
    if (transport_) transport_->shutdown();
    if (worker_.joinable()) worker_.join();
    running_.store(false);

    // The join settles the WORKER; it says nothing about the CPU thread. A
    // guest SmartPort call can be inside transact() at this instant, holding
    // a raw `SpTransport*` — so destroying the transport from here without
    // `callMtx_` is a use-after-free. It was survivable only by accident:
    // the panel's stop ran under the emulator's stateMutex, which the CPU
    // thread also holds for the whole SmartPort call, so the two could never
    // overlap. Moving that stop off stateMutex (NetworkCoordinator, so a 2 s
    // helper teardown stops freezing the machine) removed the accident, and
    // this mutex is what replaces it.
    //
    // AFTER the join, never before: the worker reaches transact() through
    // enumerateDevices(), so holding callMtx_ across the join would park the
    // two threads on each other.
    std::lock_guard<std::mutex> callLk(callMtx_);

    // Only now is it safe to tear the transport down: nothing else can be
    // inside it.
    if (transport_) {
        transport_->dropPeer();
        if (auto* tcp = dynamic_cast<SpTcpTransport*>(transport_.get()))
            tcp->stopListening();
    }
    transport_.reset();

    // Same invariant peerLostLocked() documents, for the same reason: a peer
    // that left the framer mid-frame (partial packet, then silence past the
    // timeout) would otherwise have its stale body glue onto the FIRST $C0 of
    // the next peer after a stop/start, costing that peer one whole frame.
    // Safe here — the worker is joined, so no other thread is inside the link.
    rx_.reset();

    std::lock_guard<std::mutex> lk(stateMtx_);
    devices_.clear();
}

// ── Worker ───────────────────────────────────────────────────────────────

void SpOverSlipLink::workerLoop()
{
    while (!stopFlag_.load()) {
        SpTransport* t = transport_.get();
        if (!t) break;

        if (!t->isOpen()) {
            if (t->pollForPeer(kPeerPollMs)) {
                if (stopFlag_.load()) {
                    t->dropPeer();
                    break;
                }
                // A peer just appeared. Enumerating immediately is safe even
                // for a board that is still booting: enumerateDevices()
                // retries the sweep.
                enumerateDevices();
            } else if (!stopFlag_.load() && mode_ == Mode::Serial) {
                // A serial pollForPeer does not wait on anything (the device
                // node is either there or not), so pace the retry ourselves
                // rather than spinning a core looking for an unplugged board.
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kPeerPollMs));
            }
            continue;
        }

        // Peer is attached and the CPU thread owns the conversation. The
        // worker still owns peer LIFETIME (fujinet_plan.md §6.4), so it has to
        // probe: relying on the CPU thread's failed reads means a peer that
        // closes while the guest sits at the BASIC prompt is never noticed —
        // isOpen() stays true forever, the panel keeps naming a corpse, and
        // because pollForPeer() is gated behind !isOpen() a replacement peer
        // waits unaccepted in the listen backlog until the guest happens to
        // issue a SmartPort call. The probe peeks without consuming, so it
        // cannot steal a byte from an in-flight transact().
        if (!t->checkPeerAlive()) {
            log().info("FujiNet", "SP-over-SLIP peer went away while idle");
            handlePeerLost();
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPeerPollMs));
    }
}

void SpOverSlipLink::enumerateDevices()
{
    // The worker calls this the moment a peer appears, so it is the natural
    // place to start the clock on that peer's session.
    notePeerConnected();

    // SmartPort daisy-chain enumeration: INIT unit 1, then 2, … until a
    // non-zero status says "no more devices". Same sweep the FujiNet AppleWin
    // fork does in Listener::create_connection.
    //
    // DELIBERATE DIVERGENCE from that reference: AppleWin registers a device
    // even on the iteration whose INIT *failed* (it inserts before testing
    // `still_scanning`), so its device count runs one high. POM2 registers
    // only units that answered with status $00.
    std::vector<SpDevice> found;

    for (uint8_t unit = 1; unit <= kMaxUnits; ++unit) {
        // Once per unit, because a sweep is up to kMaxUnits round trips and
        // `stop()` joins this thread. A peer that answers every INIT but is
        // slow about it — or one that answers INIT and drops the DIB STATUS,
        // where the consecutive-timeout guard never trips because each INIT
        // resets it — costs one timeout per unit, and with the panel's 5 s
        // maximum that is minutes of a stop() that cannot return. Bail with
        // whatever has been found; stop() clears `devices_` anyway.
        if (stopFlag_.load()) return;
        Response r = init(unit);

        if (!r.replied) {
            // A board that just enumerated its USB endpoint may not have its
            // firmware up yet, and a live board can be slow on ONE unit while
            // it brings its own device stack up. Retry before concluding
            // anything — for every unit, not just the first.
            for (int attempt = 0; attempt < 2 && !r.replied; ++attempt) {
                // Sliced, so a stop() lands INSIDE the retry pause rather
                // than after it — this pause is on the path stop() joins.
                for (int slept = 0; slept < 200; slept += 25) {
                    if (stopFlag_.load()) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                if (stopFlag_.load()) return;
                r = init(unit);
            }
        }
        if (!r.replied) {
            if (found.empty()) {
                // Nothing answered at all — there is no peer to talk to.
                handlePeerLost();
                return;
            }
            // Units already answered, so the peer is ALIVE and merely slow on
            // this one. Dropping the connection here used to throw away every
            // device found so far (the publish below is never reached), after
            // which the worker reconnected, hit the same slow unit and dropped
            // again — a livelock where the panel and the guest both saw ZERO
            // devices for ever, and where the serial transport reopened the
            // CDC device several times a second, driving the ESP32's
            // auto-reset line. Raising kMaxUnits 8 -> 32 made this far easier
            // to hit. Keep what answered and stop the sweep.
            log().warn("FujiNet", "unit " + std::to_string(unit) +
                                  " did not answer within " +
                                  std::to_string(timeoutMs()) + " ms — keeping the " +
                                  std::to_string(found.size()) +
                                  " device(s) already enumerated");
            break;
        }
        if (r.status != kSpOk) break;      // end of the chain

        SpDevice dev;
        dev.unit = unit;

        // Ask for the DIB so the panel can name the device. A failure here is
        // not fatal: the unit exists and works, we just have no label.
        const Response dib = status(unit, 0x03);
        if (dib.ok() && dib.data.size() >= kDibMinBytes) {
            dev.blocks = static_cast<uint32_t>(dib.data[1]) |
                         (static_cast<uint32_t>(dib.data[2]) << 8) |
                         (static_cast<uint32_t>(dib.data[3]) << 16);
            const std::size_t nameLen = std::min<std::size_t>(dib.data[4], 16);
            dev.name.assign(reinterpret_cast<const char*>(dib.data.data() + 5),
                            nameLen);
            while (!dev.name.empty() && dev.name.back() == ' ') dev.name.pop_back();
            if (dib.data.size() >= 22) dev.type    = dib.data[21];
            if (dib.data.size() >= 23) dev.subtype = dib.data[22];
        }
        if (traceEnabled())
            log().info("FujiNet",
                       "  unit " + std::to_string(unit) + " \"" + dev.name +
                       "\" type=" + std::to_string(dev.type) +
                       " blocks=" + std::to_string(dev.blocks));
        found.push_back(dev);
    }

    {
        std::lock_guard<std::mutex> lk(stateMtx_);
        devices_ = found;
    }
    log().info("FujiNet", "enumerated " + std::to_string(found.size()) +
                              " SmartPort device(s) on " + describe());
}

namespace {
/// Say it once, with the count, on the way out of a call. Logging per bad
/// byte turned a garbling peer into hundreds of stderr writes under the
/// Logger mutex while the emulator's state mutex was held.
void noteTruncatedFrames(unsigned n)
{
    if (!n) return;
    log().warn("FujiNet", "discarded " + std::to_string(n) +
                          " truncated SP-over-SLIP frame(s) during one call");
}
}  // namespace

void SpOverSlipLink::peerLostLocked()
{
    // A peer dying is the single most consequential event in this subsystem
    // and it used to slip past as one INFO line among hundreds: every symptom
    // downstream (the guest's "FujiNet not found", a boot that reads no
    // blocks, a control call answered "no device") is really this, seen
    // later and from further away. So say it LOUDLY and say how long it
    // lasted and how much work it did — a peer that dies after two calls is a
    // different bug from one that dies after ten thousand.
    // Snapshot and clear the whole triple under ONE lock: the worker can be
    // in notePeerConnected() at the same moment (this path runs on the CPU
    // thread too, via transact()), and reading the timestamp outside the
    // mutex that guards its counters is what paired one peer's connect with
    // another's loss.
    bool     hadPeer  = false;
    long long secs    = 0;
    uint64_t calls    = 0;
    uint64_t timeouts = 0;
    {
        std::lock_guard<std::mutex> sl(statsMtx_);
        if (peerSince_ != std::chrono::steady_clock::time_point{}) {
            hadPeer  = true;
            secs     = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - peerSince_).count();
            calls    = stats_.calls    - peerCallsAtConnect_;
            timeouts = stats_.timeouts - peerTimeoutsAtConnect_;
            peerSince_ = {};
        }
    }
    if (hadPeer) {
        log().warn("FujiNet",
                   "peer LOST after " + std::to_string(secs) + " s — " +
                   std::to_string(calls) + " call(s) served, " +
                   std::to_string(timeouts) + " timeout(s)");
    }

    consecutiveTimeouts_ = 0;      // a replacement peer starts with a clean slate
    // Devices first, socket second. isConnected() reads the transport and
    // deviceCount() reads devices_ under a different mutex, so the order
    // here is the only thing that keeps an observer from seeing "not
    // connected, two devices" in between — which the panel would show, and
    // which sp_over_slip_link caught once on a slow macOS runner (the
    // worker's dropPeer() landed, the test's poll saw it, and the clear was
    // still a few instructions away).
    {
        std::lock_guard<std::mutex> lk(stateMtx_);
        devices_.clear();
    }
    if (transport_) transport_->dropPeer();
    // Bytes from the dead peer must not glue themselves to the first packet
    // of the next one.
    rx_.reset();
}

void SpOverSlipLink::notePeerConnected()
{
    // Under statsMtx_ with the counters it is reported next to. The worker
    // writes it here while the CPU thread can be reading and clearing it in
    // peerLostLocked() (transact() calls that path when a write to a
    // just-reset connection fails), so an unlocked store was a real race —
    // and the symptom was a "peer LOST after <nonsense> s" line pairing one
    // peer's connect with another's loss.
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> sl(statsMtx_);
    peerSince_             = now;
    peerCallsAtConnect_    = stats_.calls;
    peerTimeoutsAtConnect_ = stats_.timeouts;
}

void SpOverSlipLink::handlePeerLost()
{
    std::lock_guard<std::mutex> lk(callMtx_);
    peerLostLocked();
}

// ── State ────────────────────────────────────────────────────────────────

bool SpOverSlipLink::isConnected() const
{ return transport_ && transport_->isOpen(); }

std::string SpOverSlipLink::describe() const
{
    if (!transport_) return "off";
    return transport_->describe();
}

std::string SpOverSlipLink::lastError() const
{
    {
        std::lock_guard<std::mutex> lk(stateMtx_);
        if (!lastError_.empty()) return lastError_;
    }
    // A serial transport's open failures are the interesting ones (device
    // missing, permission denied) and it keeps its own text.
    if (auto* ser = dynamic_cast<SpSerialTransport*>(transport_.get()))
        return ser->lastError();
    return std::string{};
}

std::vector<SpDevice> SpOverSlipLink::devices() const
{
    std::lock_guard<std::mutex> lk(stateMtx_);
    return devices_;
}

std::size_t SpOverSlipLink::deviceCount() const
{
    std::lock_guard<std::mutex> lk(stateMtx_);
    return devices_.size();
}

SpOverSlipLink::Stats SpOverSlipLink::stats() const
{
    std::lock_guard<std::mutex> lk(statsMtx_);
    return stats_;
}

void SpOverSlipLink::setTimeoutMs(int ms)
{
    timeoutMs_.store(std::max(kMinTimeoutMs, std::min(kMaxTimeoutMs, ms)));
}

// ── The round trip ───────────────────────────────────────────────────────

uint8_t SpOverSlipLink::nextSequence()
{
    // Skip 0 so a zeroed buffer can never look like a valid sequence number.
    if (++sequence_ == 0) sequence_ = 1;
    return sequence_;
}

namespace {
/// Consecutive unanswered calls before the peer is declared lost. Three, not
/// one: a single timeout is an ordinary hiccup on a busy helper, and dropping
/// a live peer for one slow reply would be its own bug. Three at the 250 ms
/// default is 0.75 s of freeze before the link goes quiet — bounded, and after
/// that every call fails instantly instead of costing another quarter second.
constexpr unsigned kMaxConsecutiveTimeouts = 3;
}  // namespace

SpOverSlipLink::Response
SpOverSlipLink::transact(uint8_t command, uint8_t paramCount, uint8_t unit,
                         const uint8_t fields[5],
                         const uint8_t* data, std::size_t dataLen)
{
    Response out;

    // Read the transport UNDER callMtx_, never before it. stop() destroys it
    // with this same mutex held, so a pointer sampled outside the lock can be
    // dangling by the time the lock is granted — the window the emulator's
    // stateMutex used to hide.
    std::lock_guard<std::mutex> lk(callMtx_);
    SpTransport* t = transport_.get();
    if (!t || !t->isOpen()) return out;      // replied = false → no device

    const uint8_t seq = nextSequence();

    uint8_t header[kRequestHeaderBytes] = {};
    header[0] = seq;
    header[1] = command;
    header[2] = paramCount;
    header[3] = unit;
    header[4] = 0x00;                        // reserved
    header[5] = 0x00;
    std::memcpy(header + 6, fields, 5);

    std::vector<uint8_t> packet;
    packet.reserve(kRequestHeaderBytes + dataLen);
    packet.insert(packet.end(), header, header + kRequestHeaderBytes);
    if (data && dataLen) packet.insert(packet.end(), data, data + dataLen);

    if (traceEnabled()) {
        // The packet bytes, not just a summary: when a peer dies mid-session
        // the only useful question is what the LAST request looked like, and
        // a summary cannot answer it.
        std::string hex;
        for (std::size_t i = 0; i < packet.size() && i < 24; ++i) {
            char b[4];
            std::snprintf(b, sizeof b, "%02X ", packet[i]);
            hex += b;
        }
        log().info("FujiNet", std::string("-> ") + commandName(command) +
                   " unit=" + std::to_string(unit) +
                   " code=" + std::to_string(fields[0]) +
                   " len=" + std::to_string(packet.size()) + " : " + hex);
    }

    txBuf_.clear();
    SlipFramer::encode(packet, txBuf_);

    {
        std::lock_guard<std::mutex> sl(statsMtx_);
        ++stats_.calls;
        stats_.bytesOut += txBuf_.size();
    }

    if (!t->writeAll(txBuf_.data(), txBuf_.size())) {
        peerLostLocked();          // callMtx_ is ours right now
        return out;
    }

    // Wait for OUR response. The deadline covers the whole exchange, not each
    // read, so a peer dribbling bytes cannot extend the stall indefinitely.
    const int budgetMs = timeoutMs_.load();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(budgetMs);

    uint8_t buf[1024];
    unsigned truncated = 0;                  // reported once, when we leave
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            noteTruncatedFrames(truncated);
            {
                std::lock_guard<std::mutex> sl(statsMtx_);
                ++stats_.timeouts;
            }
            // A peer that accepts writes and never answers is gone in every
            // way that matters here. Declaring it lost closes the socket, so
            // the next call returns at the isOpen() gate instead of paying
            // another `timeoutMs_` — which is the difference between a boot
            // that stalls once and one that stalls on every block.
            if (++consecutiveTimeouts_ >= kMaxConsecutiveTimeouts) {
                log().warn("FujiNet",
                           "peer stopped answering (" +
                           std::to_string(consecutiveTimeouts_) +
                           " consecutive timeouts) — dropping the link so "
                           "calls fail fast instead of freezing the machine");
                peerLostLocked();            // callMtx_ is ours right now
            }
            return out;                      // replied = false
        }
        const int waitMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count());

        const int got = t->readSome(buf, sizeof(buf), waitMs > 0 ? waitMs : 1);
        if (got < 0) { noteTruncatedFrames(truncated); peerLostLocked(); return out; }
        if (got == 0) continue;              // nothing yet; deadline re-checked

        {
            std::lock_guard<std::mutex> sl(statsMtx_);
            stats_.bytesIn += static_cast<uint64_t>(got);
        }

        for (int i = 0; i < got; ++i) {
            switch (rx_.feed(buf[static_cast<std::size_t>(i)])) {
            case SlipFramer::Feed::NeedMore:
                break;

            case SlipFramer::Feed::Truncated:
                // A frame cut short — exactly what a guest reset mid-transfer
                // looks like. Not fatal: resync and keep waiting for ours.
                //
                // Counted always, logged ONCE per call. The framer reports
                // this for every $DB not followed by an escape byte, so a
                // garbling peer sending `C0 DB 00` over and over produced one
                // unthrottled stderr write per three bytes — each under
                // Logger's mutex while callMtx_ and the emulator's state mutex
                // are both held, for the whole 250 ms budget. Hundreds of
                // syscalls stalling the emulated CPU, to say the same sentence
                // hundreds of times.
                ++truncated;
                break;

            case SlipFramer::Feed::Frame: {
                const auto& f = rx_.frame();
                if (f.size() < kResponseHeaderBytes) break;   // runt, ignore
                if (f[0] != seq) {
                    // THE reason the sequence number exists: a response left
                    // over from before a guest reset. Drop it and keep
                    // waiting for the one we asked for.
                    std::lock_guard<std::mutex> sl(statsMtx_);
                    ++stats_.stale;
                    break;
                }
                out.replied = true;
                out.status  = f[1];
                consecutiveTimeouts_ = 0;    // the peer is answering again
                out.data.assign(f.begin() + kResponseHeaderBytes, f.end());
                if (traceEnabled())
                    log().info("FujiNet",
                               "<- " + std::string(commandName(command)) +
                               " unit=" + std::to_string(unit) +
                               " status=" + std::to_string(out.status) +
                               " data=" + std::to_string(out.data.size()) + "o");
                noteTruncatedFrames(truncated);
                return out;
            }
            }
        }
    }
}

// ── Typed calls ──────────────────────────────────────────────────────────
//
// Field layouts are the spec's tables, verbatim. Parameter counts likewise —
// they are what the guest's own parameter list carries, and the peer checks
// them.

namespace {

/// Repair an upstream malformation in the DIB name, in place.
///
/// fujinet-firmware builds a disk's SmartPort ID string as
///
///     char disk_num;                                        // disk.h:33
///     std::string name = "FUJINET_DISK_" + std::to_string(disk_num);
///                                                           // disk.cpp:106
///
/// `disk_num` holds an ASCII DIGIT ('0'..'7'), and `std::to_string` has no
/// `char` overload, so the char promotes to int: the device that means to
/// call itself FUJINET_DISK_0 goes on the wire as **FUJINET_DISK_48**, and
/// 1..7 as 49..55. Guest software finds the FujiNet by that exact name, so
/// against an affected build it simply does not: NETCAT prints
/// "FUJINET_DISK_0 NOT FOUND" and stops.
///
/// POM2 relays verbatim as a rule, and this is the documented exception —
/// the same call the printer unit already gets, where the firmware advertises
/// it with the modem's type byte and POM2 matches on the name instead (see
/// DEV.md § FujiNet). Both are upstream bugs that would otherwise make the
/// relay look broken. The rewrite is deliberately narrow: only the exact
/// shape "FUJINET_DISK_" + two decimal digits whose value is 48..55, which
/// no correct firmware can emit, so this evaporates on its own the day
/// upstream fixes the `to_string`.
void repairDibName(std::vector<uint8_t>& dib)
{
    // status(1) + blocks(3) + name_len(1) + name(16) …
    if (dib.size() < 22) return;
    const std::size_t nameLen = dib[4];
    if (nameLen != 15 || dib.size() < 5 + 16) return;

    static const char kPrefix[] = "FUJINET_DISK_";
    constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;   // 13
    if (std::memcmp(dib.data() + 5, kPrefix, kPrefixLen) != 0) return;

    const uint8_t d0 = dib[5 + kPrefixLen];
    const uint8_t d1 = dib[5 + kPrefixLen + 1];
    if (d0 < '0' || d0 > '9' || d1 < '0' || d1 > '9') return;
    const int value = (d0 - '0') * 10 + (d1 - '0');
    if (value < '0' || value > '7') return;      // 48..55 only

    dib[5 + kPrefixLen] = static_cast<uint8_t>(value);   // the digit it meant
    dib[5 + kPrefixLen + 1] = ' ';                       // pad, name is fixed
    dib[4] = static_cast<uint8_t>(kPrefixLen + 1);       // …and shorten it
}

} // namespace

SpOverSlipLink::Response SpOverSlipLink::status(uint8_t unit, uint8_t statusCode)
{
    const uint8_t fields[5] = { statusCode, 0, 0, 0, 0 };
    Response r = transact(kSpStatus, 0x03, unit, fields, nullptr, 0);
    // Status code $03 is the DIB. Repaired here rather than at either call
    // site so POM2's own device table and the bytes the guest reads cannot
    // disagree about what a device is called.
    if (statusCode == 0x03 && r.ok()) repairDibName(r.data);
    return r;
}

SpOverSlipLink::Response SpOverSlipLink::readBlock(uint8_t unit, uint32_t block)
{
    const uint8_t fields[5] = {
        static_cast<uint8_t>(block & 0xFF),
        static_cast<uint8_t>((block >> 8) & 0xFF),
        static_cast<uint8_t>((block >> 16) & 0xFF),
        0, 0 };
    return transact(kSpReadBlock, 0x03, unit, fields, nullptr, 0);
}

SpOverSlipLink::Response
SpOverSlipLink::writeBlock(uint8_t unit, uint32_t block,
                           const uint8_t* data, std::size_t n)
{
    const uint8_t fields[5] = {
        static_cast<uint8_t>(block & 0xFF),
        static_cast<uint8_t>((block >> 8) & 0xFF),
        static_cast<uint8_t>((block >> 16) & 0xFF),
        0, 0 };
    return transact(kSpWriteBlock, 0x03, unit, fields, data, n);
}

SpOverSlipLink::Response SpOverSlipLink::format(uint8_t unit)
{
    const uint8_t fields[5] = { 0, 0, 0, 0, 0 };
    return transact(kSpFormat, 0x01, unit, fields, nullptr, 0);
}

SpOverSlipLink::Response
SpOverSlipLink::control(uint8_t unit, uint8_t controlCode,
                        const uint8_t* list, std::size_t n)
{
    const uint8_t fields[5] = { controlCode, 0, 0, 0, 0 };

    // The control list goes on the wire WITH its 2-byte little-endian length
    // prefix — the shape the guest already laid out in its own memory, and
    // what the peer expects. The reference parser is explicit about it:
    //
    //     case CMD_CONTROL:
    //         // +2 for control list length bytes we need to skip
    //         std::vector<uint8_t> payload(packet.begin() + 11+2, packet.end());
    //
    // (FujiNetWIFI/AppleWin, source/devrelay/types/Request.cpp.)
    //
    // Sending the bare list was a protocol bug with two faces, and it cost
    // most of a day to corner. A list SHORTER than two bytes made that
    // iterator run past the end of the packet, so the peer threw
    // std::length_error, did not catch it, and **aborted the whole FujiNet
    // process** — every "the firmware keeps dying" symptom in this subsystem
    // traced back to exactly this packet (`cmd=04 unit=00 len=12`, a 1-byte
    // control list). A longer list did not crash it but had its first two
    // bytes eaten as the length, which is why the guest-side CONFIG program
    // showed empty host and drive slots while the peer's own web UI showed
    // them populated. Pinned by sp_over_slip_link's control-framing case.
    std::vector<uint8_t> framed;
    framed.reserve(2 + n);
    framed.push_back(static_cast<uint8_t>(n & 0xFF));
    framed.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    if (list && n) framed.insert(framed.end(), list, list + n);

    return transact(kSpControl, 0x03, unit, fields, framed.data(), framed.size());
}

SpOverSlipLink::Response SpOverSlipLink::init(uint8_t unit)
{
    const uint8_t fields[5] = { 0, 0, 0, 0, 0 };
    return transact(kSpInit, 0x01, unit, fields, nullptr, 0);
}

SpOverSlipLink::Response SpOverSlipLink::open(uint8_t unit)
{
    const uint8_t fields[5] = { 0, 0, 0, 0, 0 };
    return transact(kSpOpen, 0x01, unit, fields, nullptr, 0);
}

SpOverSlipLink::Response SpOverSlipLink::close(uint8_t unit)
{
    const uint8_t fields[5] = { 0, 0, 0, 0, 0 };
    return transact(kSpClose, 0x01, unit, fields, nullptr, 0);
}

SpOverSlipLink::Response
SpOverSlipLink::read(uint8_t unit, uint16_t byteCount, uint32_t address)
{
    const uint8_t fields[5] = {
        static_cast<uint8_t>(byteCount & 0xFF),
        static_cast<uint8_t>(byteCount >> 8),
        static_cast<uint8_t>(address & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
        static_cast<uint8_t>((address >> 16) & 0xFF) };
    return transact(kSpRead, 0x04, unit, fields, nullptr, 0);
}

SpOverSlipLink::Response
SpOverSlipLink::write(uint8_t unit, uint16_t byteCount, uint32_t address,
                      const uint8_t* data, std::size_t n)
{
    const uint8_t fields[5] = {
        static_cast<uint8_t>(byteCount & 0xFF),
        static_cast<uint8_t>(byteCount >> 8),
        static_cast<uint8_t>(address & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
        static_cast<uint8_t>((address >> 16) & 0xFF) };
    return transact(kSpWrite, 0x04, unit, fields, data, n);
}

// ── Guest reset ──────────────────────────────────────────────────────────

void SpOverSlipLink::resync()
{
    std::lock_guard<std::mutex> lk(callMtx_);
    nextSequence();
    rx_.reset();
}

void SpOverSlipLink::notifyGuestReset()
{
    // The correctness-critical half, and it is cheap: move the sequence
    // number on so any response still in flight for the pre-reset request is
    // rejected as stale by the next transact().
    resync();

    if (!isConnected()) return;

    // The courtesy half: tell each device the machine reset, so a modem drops
    // its connection and a printer ejects a partial page (the spec asks the
    // Apple II side to send Control code $00 for exactly this).
    //
    // Bounded on purpose: if the FIRST device does not answer, the peer is
    // gone and there is no point stalling the reset for every remaining unit.
    // A live peer answers each of these in microseconds.
    const auto list = devices();
    for (const auto& d : list) {
        // NOT the printer. Sending it the reset the spec asks for ABORTS the
        // peer: measured three runs out of three, the firmware throws
        // std::length_error out of Request::from_packet on this exact call
        // and the whole FujiNet process dies, while units either side answer
        // a byte-identical request normally. Same unit whose DIB already
        // advertises the modem's type byte — its device code is shaky
        // upstream.
        //
        // This mattered far more than a skipped courtesy: POM2 sends this
        // broadcast on EVERY guest reset, so every Ctrl-Reset and every boot
        // killed the peer a moment later. The guest then reported whatever it
        // was doing when the corpse stopped answering — "connection error",
        // "FujiNet not found", a browser that loads and then cannot fetch —
        // and none of those point here. Skipping one no-op reset for a
        // printer that has printed nothing costs the guest nothing.
        //
        // Remove once upstream stops aborting; `isPrinter()` already matches
        // on the DIB name so a fixed firmware needs no change here.
        if (d.isPrinter()) continue;
        const Response r = control(d.unit, 0x00, nullptr, 0);
        if (!r.replied) break;
    }
}

} // namespace pom2
