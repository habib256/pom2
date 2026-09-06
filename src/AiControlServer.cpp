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

#include "AiControlServer.h"
#include "Pom2Build.h"

#include "Apple2Display.h"
#include "AtomicFileReplace.h"
#include "CpuClock.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "Logger.h"
#include "ThreadGuard.h"
#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "SocketUtil.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "SlotBus.h"
#include "SnapshotIO.h"
#include "SystemProfile.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#if POM2_HAS_SOCKETS
// POSIX socket stack — the HTTP control endpoint is desktop-only. In a
// WASM build the listener loop and every socket call are compiled out,
// and start()/stop() become logged no-ops. The rest of the server's
// state machine (attach, detach, handlers) stays linked so any callers
// inside the binary still see a valid object.
// Host socket stack for both families — POSIX and Winsock. SocketUtil.h
// (included above) is built on it and carries the accept/SIGPIPE idioms.
#include "SocketCompat.h"
#endif

namespace pom2 {

namespace {

constexpr size_t kMaxBodyBytes    = 1 << 20;   // 1 MiB hard cap on body
constexpr size_t kMaxHeaderBytes  = 64 * 1024; // 64 KiB on the request preamble
constexpr int    kRecvTimeoutMs   = 4000;      // per-recv SO_RCVTIMEO
// Wall-clock cap on the whole readRequest call. The per-recv timeout above
// only protects against an idle peer (no bytes at all); a slow-drip attacker
// dribbling 1 byte every <kRecvTimeoutMs holds the single worker thread —
// the entire AI bridge — for hours, because each recv() returns >0 and the
// loop continues. With a deadline, the loop walks bytes up to a fixed wall-
// clock budget regardless of drip rate.
constexpr int    kRequestDeadlineMs = 10000;   // 10 s — generous for localhost

// ─── Path safety ─────────────────────────────────────────────────────────
// Canonicalise the caller-supplied path and require it to resolve under
// the emulator's current working directory. The AI control server is
// localhost-only and token-gated, but a compromised agent (or a browser
// hijacked via DNS-rebinding) could otherwise hand `/etc/passwd` or
// `~/.ssh/id_rsa` to /disk, or have /snapshot/save overwrite arbitrary
// files. `weakly_canonical` is the right tool: it resolves symlinks and
// `..` for the existing prefix, and treats the rest lexically — so it
// works for both load paths (file must exist) and save paths (file
// doesn't exist yet). Caller decides which mode it needs via the
// `mustExist` flag.
/// True when `s` carries a byte that must never reach a filesystem call.
///
/// A NUL is the one that matters. Every path check in this file runs on a
/// `std::string` (which happily holds a NUL) while the OS call underneath
/// runs on `c_str()` (which stops at one), so the two see DIFFERENT paths:
/// `"roms/apple2e.rom\0.pom2snap"` passes an "ends with .pom2snap" test and
/// then opens `roms/apple2e.rom`. Every other control byte is refused with
/// it — nothing legitimate names a file with a newline or an escape, and a
/// path spliced into a log line or a JSON reply is one less thing to think
/// about.
bool pathHasControlBytes(const std::string& s)
{
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

std::optional<std::string> safeCwdRelativePath(const std::string& in,
                                               bool mustExist)
{
    namespace fs = std::filesystem;
    if (in.empty()) return std::nullopt;
    // Before anything looks at the string's SHAPE: see pathHasControlBytes.
    if (pathHasControlBytes(in)) return std::nullopt;
    std::error_code ec;
    const fs::path cwd = fs::weakly_canonical(fs::current_path(ec), ec);
    if (ec) return std::nullopt;
    // `weakly_canonical` only resolves the existing prefix of its argument;
    // a relative path to a non-existent file (e.g. for /snapshot/save)
    // would otherwise stay relative and fail the absolute-cwd prefix check
    // even when the file lives right inside cwd. fs::absolute promotes
    // relative inputs to <cwd>/<in> first so weakly_canonical then has a
    // stable root to lexically normalise against.
    const fs::path full = fs::weakly_canonical(fs::absolute(fs::path(in), ec), ec);
    if (ec) return std::nullopt;
    // Reject a symlink as the resolved target. weakly_canonical can return a
    // final-component symlink LEXICALLY (so the path looks inside cwd) while
    // the caller's ofstream/open() then FOLLOWS it out of the jail. The load
    // path's is_regular_file dereferences, but the save path (mustExist=false)
    // would otherwise overwrite an arbitrary file via a planted symlink.
    if (fs::is_symlink(fs::symlink_status(full, ec))) return std::nullopt;
    if (mustExist && !fs::is_regular_file(full, ec)) return std::nullopt;
    // Component-wise prefix check on canonical paths. We can't use a
    // string-level `starts_with` because `/foo/barbaz` would falsely
    // match `/foo/bar`. fs::path iteration handles separator quirks.
    auto cwdIt = cwd.begin(), cwdEnd = cwd.end();
    auto fullIt = full.begin(), fullEnd = full.end();
    for (; cwdIt != cwdEnd; ++cwdIt, ++fullIt) {
        if (fullIt == fullEnd || *cwdIt != *fullIt) return std::nullopt;
    }
    return full.string();
}

// ─── String / parsing helpers ────────────────────────────────────────────

std::string toLowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::pair<std::string,std::string>> parseQuery(const std::string& q)
{
    std::vector<std::pair<std::string,std::string>> out;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos) amp = q.size();
        size_t eq = q.find('=', i);
        std::string key, val;
        if (eq == std::string::npos || eq > amp) {
            key = q.substr(i, amp - i);
        } else {
            key = q.substr(i, eq - i);
            val = q.substr(eq + 1, amp - eq - 1);
        }
        out.emplace_back(std::move(key), std::move(val));
        i = amp + 1;
    }
    return out;
}

std::string queryParam(const std::string& q, const std::string& key)
{
    for (const auto& kv : parseQuery(q)) {
        if (kv.first == key) return kv.second;
    }
    return {};
}

/// Minimal extractor for `"key":<literal>` and `"key":"<quoted>"` shapes.
/// POM2's API surface uses flat one-level JSON only — no nested objects or
/// arrays — so a hand-rolled scanner is enough and avoids dragging in a
/// JSON dependency. Accepts unquoted tokens (numbers, true/false), quoted
/// strings with `\"`, `\\`, `\n`, `\r`, `\t` escapes. Unknown keys → empty
/// string; the caller supplies the default.
// Parse a JSON value (quoted string with escapes, or a bare token) that
// starts at body[pos] (pos just past the key's ':'). Returns unescaped text.
std::string jsonParseValueAt(const std::string& body, size_t pos)
{
    const size_t n = body.size();
    while (pos < n && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= n) return {};
    if (body[pos] == '"') {
        ++pos;
        std::string out;
        while (pos < n && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < n) {
                switch (body[pos + 1]) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '"': out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    default:   out.push_back(body[pos + 1]); break;
                }
                pos += 2;
            } else {
                out.push_back(body[pos]); ++pos;
            }
        }
        return out;
    }
    std::string out;
    while (pos < n && body[pos] != ',' && body[pos] != '}' &&
           !std::isspace(static_cast<unsigned char>(body[pos]))) {
        out.push_back(body[pos]); ++pos;
    }
    return out;
}

// Return the index just past the value at body[pos] (past ':'), so the key
// scan can skip a non-matching field's value without mistaking the value's
// contents for a key.
size_t jsonSkipValueAt(const std::string& body, size_t pos)
{
    const size_t n = body.size();
    while (pos < n && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos < n && body[pos] == '"') {
        ++pos;
        while (pos < n && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < n) pos += 2; else ++pos;
        }
        if (pos < n) ++pos;                 // past closing quote
    } else {
        while (pos < n && body[pos] != ',' && body[pos] != '}') ++pos;
    }
    return pos;
}

/// Extract `key`'s value from flat one-level JSON. Walks the body skipping
/// over string VALUES so the key only matches at an object-key position
/// (immediately followed by ':') — a value containing another field's name
/// as a substring no longer hijacks the match. Unknown key → empty string.
std::string jsonGetString(const std::string& body, const std::string& key)
{
    const size_t n = body.size();
    size_t i = 0;
    while (i < n) {
        if (body[i] != '"') { ++i; continue; }
        // Read the string token at i (a key candidate).
        std::string name;
        size_t j = i + 1;
        for (; j < n && body[j] != '"'; ++j) {
            if (body[j] == '\\' && j + 1 < n) { name.push_back(body[j + 1]); ++j; }
            else                              { name.push_back(body[j]); }
        }
        if (j >= n) break;                  // unterminated string
        size_t k = j + 1;
        while (k < n && std::isspace(static_cast<unsigned char>(body[k]))) ++k;
        if (k < n && body[k] == ':') {       // `name` is an object key
            if (name == key) return jsonParseValueAt(body, k + 1);
            i = jsonSkipValueAt(body, k + 1);
        } else {
            i = j + 1;                       // `name` was a value string
        }
    }
    return {};
}

bool jsonGetInt(const std::string& body, const std::string& key, long& out)
{
    const std::string s = jsonGetString(body, key);
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        // Accept decimal or 0x… hex for convenience.
        const int base = (s.size() > 2 && s[0] == '0' && (s[1]=='x'||s[1]=='X'))
                       ? 16 : 10;
        out = std::stol(s, &pos, base);
        // The WHOLE token has to be a number. `pos > 0` accepted a partial
        // parse, and the value was mangled BEFORE the careful range checks
        // downstream ever saw it: `{"cycles_per_frame":2.5e6}` — legal JSON —
        // parsed as 2, passed the [1, 2000000] check, and set the machine to
        // ~120 emulated cycles per second while answering 200 OK. Same shape
        // for `{"pc":1e3}` -> $0001 and `{"drive":"1x"}` -> 1.
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

bool fromHex(char c, uint8_t& nib)
{
    if (c >= '0' && c <= '9') { nib = static_cast<uint8_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { nib = static_cast<uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { nib = static_cast<uint8_t>(c - 'A' + 10); return true; }
    return false;
}

bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out)
{
    if (hex.size() % 2) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        uint8_t hi, lo;
        if (!fromHex(hex[i], hi) || !fromHex(hex[i + 1], lo)) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string bytesToHex(const uint8_t* data, size_t n)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

std::string jsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else if (c < 0x80) {
                    out.push_back(static_cast<char>(c));
                } else {
                    // Escape everything above ASCII rather than passing the
                    // raw byte through. Filenames on macOS and Linux may hold
                    // bytes that are not valid UTF-8, and one of them inside a
                    // mounted image's path made every subsequent /status reply
                    // undecodable — the agent's polling broke permanently,
                    // until the disk was ejected, with nothing to point at.
                    // \u00XX is valid JSON and round-trips the byte value.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                }
        }
    }
    return out;
}

#if POM2_HAS_SOCKETS
void applyRecvTimeout(pom2::socket_t fd, int timeoutMs)
{
#ifdef _WIN32
    // Winsock's SO_RCVTIMEO takes a DWORD of milliseconds, NOT a timeval —
    // passing a timeval there is accepted and then read as garbage, which
    // is how a 2-second timeout silently becomes minutes.
    const DWORD ms = static_cast<DWORD>(timeoutMs);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void applySendTimeout(pom2::socket_t fd, int timeoutMs)
{
#ifdef _WIN32
    const DWORD ms = static_cast<DWORD>(timeoutMs);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool sendAll(pom2::socket_t fd, const char* buf, size_t n)
{
    while (n > 0) {
        const pom2::iolen_t s = pom2::sendNoSignal(fd, buf, n);
        if (s <= 0) {
            if (s < 0 && pom2::errInterrupted(pom2::lastSocketError())) continue;
            return false;
        }
        buf += s;
        n   -= static_cast<size_t>(s);
    }
    return true;
}
#endif // POM2_HAS_SOCKETS

std::string cpuModeName(M6502::CpuMode m)
{
    return m == M6502::CpuMode::CMOS ? "65c02" : "nmos";
}

std::string runModeName(EmulationController::Mode m)
{
    switch (m) {
        case EmulationController::Mode::Stopped: return "stopped";
        case EmulationController::Mode::Running: return "running";
        case EmulationController::Mode::Step:    return "step";
    }
    return "?";
}

} // namespace

// ─── Request helpers ──────────────────────────────────────────────────────

std::string AiControlServer::Request::headerValue(const std::string& name) const
{
    const std::string want = toLowerAscii(name);
    for (const auto& kv : headers) {
        if (toLowerAscii(kv.first) == want) return kv.second;
    }
    return {};
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

AiControlServer::~AiControlServer()
{
    stop();
}

void AiControlServer::attach(EmulationController* ctrl,
                             Apple2Display*       display,
                             DiskIICard*          disk6,
                             ProDOSHardDiskCard*  hdv5)
{
    ctrl_    = ctrl;
    display_ = display;
    disk6_   = disk6;
    hdv5_    = hdv5;
}

void AiControlServer::detach()
{
    // Pointer-only clear under caller-held stateMutex. Concurrent
    // handlers either (a) blocked waiting on the lock and will see the
    // nulls when they unblock and re-check, or (b) already past their
    // null-check and inside the lock — in which case the caller is
    // blocked behind them and applyProfile's actual card teardown
    // hasn't happened yet, so the still-alive cards are safe to use.
    // ctrl_ and display_ live across profile switches (members of
    // MainWindow / EmulationController, not re-created) — only the
    // slot cards get torn down and rebuilt, so only their pointers
    // need clearing.
    disk6_   = nullptr;
    hdv5_    = nullptr;
}

bool AiControlServer::start(uint16_t port)
{
#if !POM2_HAS_SOCKETS
    (void)port;
    pom2::log().info("AICtrl", "HTTP control listener disabled in WASM build");
    return false;
#else
    stop();
    if (!ctrl_) {
        pom2::log().warn("AICtrl", "start() called before attach() — refusing");
        return false;
    }
    pom2::ensureSocketStack();     // Winsock needs WSAStartup; no-op elsewhere
    const pom2::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!pom2::isValidSocket(fd)) {
        pom2::log().warn("AICtrl", "socket() failed: " + pom2::lastSocketErrorText());
        return false;
    }
    // NOT a bare SO_REUSEADDR: on Winsock that option would let any local
    // process bind 127.0.0.1:<port> while this listener is up and take the
    // agent's requests — token and all. SocketCompat.h, trap 6.
    pom2::setListenerBindPolicy(fd);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        pom2::log().warn("AICtrl",
            "bind 127.0.0.1:" + std::to_string(port) + " failed: " +
            pom2::lastSocketErrorText());
        pom2::closeHostSocketValue(fd);
        return false;
    }
    if (::listen(fd, 4) != 0) {
        pom2::log().warn("AICtrl", "listen() failed: " + pom2::lastSocketErrorText());
        pom2::closeHostSocketValue(fd);
        return false;
    }
    listenFd_.store(fd, std::memory_order_release);
    port_           = port;
    stopRequested_  = false;
    running_        = true;
    // Outer barrier. The per-request handlers inside runWorker() already
    // catch (a bad request must not kill the server); this covers the loop
    // scaffolding around them, so no path out of this thread reaches
    // std::terminate().
    worker_         = pom2::guardedThread("AI", [this] { runWorker(); });
    pom2::log().info("AICtrl",
        "listening on 127.0.0.1:" + std::to_string(port_) +
        " — POST/GET to drive the emulator from an AI agent");
    return true;
#endif
}

void AiControlServer::stop()
{
#if !POM2_HAS_SOCKETS
    running_ = false;
    return;
#else
    if (!running_ && !worker_.joinable()) return;
    stopRequested_ = true;
    // shutdown() wakes the worker out of accept() without invalidating
    // the fd. close() must happen AFTER join() — closing first lets the
    // kernel recycle the descriptor while the worker still holds it in
    // an in-flight accept(), creating a use-after-close window. Mirrors
    // SuperSerialCard::stopListening() exactly.
    pom2::shutdownBoth(listenFd_.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lk(clientFdMtx_);
        pom2::shutdownBoth(clientFd_);
    }
    if (worker_.joinable()) worker_.join();
    pom2::closeHostSocketValue(
        listenFd_.exchange(pom2::kInvalidSocket, std::memory_order_acq_rel));
    running_ = false;
#endif
}

#if POM2_HAS_SOCKETS
void AiControlServer::runWorker()
{
    while (!stopRequested_) {
        // poll-then-accept via SocketUtil (the macOS shutdown-vs-accept
        // deadlock rationale lives there; it was first reproduced here as
        // the ai_control_server_smoke ctest timeout — every assertion
        // passed, then the binary died in stop()'s worker_.join()).
        sockaddr_in peer{};
        pom2::socket_t fd = pom2::kInvalidSocket;
        const auto pa = pom2::pollAcceptOnce(
            listenFd_.load(std::memory_order_acquire), 200, fd, peer);
        if (pa == pom2::PollAccept::Retry)    continue;
        if (pa == pom2::PollAccept::Shutdown) break;
        {
            std::lock_guard<std::mutex> lk(clientFdMtx_);
            if (stopRequested_) {
                pom2::closeHostSocketValue(fd);
                break;
            }
            clientFd_ = fd;
        }
        pom2::disableSigpipe(fd);
        pom2::setSockOptInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
        applySendTimeout(fd, kRecvTimeoutMs);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            lastClient_ = pom2::peerAddressText(peer);
        }
        // A handler must never escape an exception out of the worker thread —
        // that calls std::terminate() and kills the whole emulator. Catch all,
        // log, and keep serving (the fd is still closed below).
        try {
            handleClient(fd);
        } catch (const std::exception& e) {
            pom2::log().warn("AICtrl", std::string("handler exception: ") + e.what());
        } catch (...) {
            pom2::log().warn("AICtrl", "handler exception (unknown)");
        }
        {
            std::lock_guard<std::mutex> lk(clientFdMtx_);
            if (clientFd_ == fd) clientFd_ = pom2::kInvalidSocket;
        }
        pom2::shutdownBoth(fd);
        pom2::closeHostSocketValue(fd);
        ++requestsServed_;
    }
}

// ─── Request parsing ──────────────────────────────────────────────────────

bool AiControlServer::readRequest(socket_t fd, Request& req)
{
    applyRecvTimeout(fd, kRecvTimeoutMs);

    // Wall-clock deadline for the whole request — see kRequestDeadlineMs
    // comment. Independent of the per-recv timeout because a slow-drip
    // attacker keeps every individual recv inside that budget.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kRequestDeadlineMs);
    auto pastDeadline = [&]() {
        return std::chrono::steady_clock::now() >= deadline;
    };

    std::string buffer;
    buffer.reserve(2048);
    size_t headerEnd = std::string::npos;
    char chunk[2048];
    while (headerEnd == std::string::npos) {
        if (buffer.size() > kMaxHeaderBytes) return false;
        if (pastDeadline()) return false;
        const iolen_t got = recvSocket(fd, chunk, sizeof(chunk));
        if (got <= 0) return false;
        buffer.append(chunk, chunk + got);
        headerEnd = buffer.find("\r\n\r\n");
    }

    // Request line: "METHOD path?query HTTP/x.y"
    const size_t lineEnd = buffer.find("\r\n");
    if (lineEnd == std::string::npos || lineEnd == 0) return false;
    std::istringstream iss(buffer.substr(0, lineEnd));
    std::string url;
    iss >> req.method >> url;
    if (req.method.empty() || url.empty()) return false;
    const size_t q = url.find('?');
    if (q == std::string::npos) {
        req.path  = url;
        req.query.clear();
    } else {
        req.path  = url.substr(0, q);
        req.query = url.substr(q + 1);
    }

    // Header lines until the blank line.
    size_t pos = lineEnd + 2;
    while (pos < headerEnd) {
        const size_t eol = buffer.find("\r\n", pos);
        if (eol == std::string::npos || eol > headerEnd) break;
        const std::string line = buffer.substr(pos, eol - pos);
        pos = eol + 2;
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name  = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        size_t v = 0;
        while (v < value.size() && (value[v] == ' ' || value[v] == '\t')) ++v;
        value.erase(0, v);
        req.headers.emplace_back(std::move(name), std::move(value));
    }

    // Body bounded by Content-Length. Whatever already landed in `buffer`
    // past the header terminator counts toward the body, plus extra recv()s
    // until we have the full length.
    const size_t bodyStart = headerEnd + 4;
    const std::string clStr = req.headerValue("Content-Length");
    if (!clStr.empty()) {
        long cl = 0;
        try { cl = std::stol(clStr); } catch (...) { return false; }
        if (cl < 0 || static_cast<size_t>(cl) > kMaxBodyBytes) return false;
        req.body.reserve(static_cast<size_t>(cl));
        if (bodyStart < buffer.size()) {
            req.body.append(buffer, bodyStart, std::string::npos);
        }
        while (req.body.size() < static_cast<size_t>(cl)) {
            if (pastDeadline()) return false;
            const iolen_t got = recvSocket(fd, chunk, sizeof(chunk));
            if (got <= 0) return false;
            req.body.append(chunk, chunk + got);
        }
        if (req.body.size() > static_cast<size_t>(cl)) {
            req.body.resize(static_cast<size_t>(cl));
        }
    }
    return true;
}

// ─── Response helpers ─────────────────────────────────────────────────────

void AiControlServer::sendResponse(socket_t fd,
                                   int status,
                                   const std::string& contentType,
                                   const std::string& body)
{
    const char* reason = "OK";
    switch (status) {
        case 200: reason = "OK"; break;
        case 201: reason = "Created"; break;
        case 204: reason = "No Content"; break;
        case 400: reason = "Bad Request"; break;
        case 401: reason = "Unauthorized"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 500: reason = "Internal Server Error"; break;
        case 503: reason = "Service Unavailable"; break;
        default:  reason = "Status"; break;
    }
    char head[512];
    const int n = std::snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Server: POM2-AICtrl\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, reason, contentType.c_str(), body.size());
    if (n <= 0) return;
    sendAll(fd, head, static_cast<size_t>(n));
    if (!body.empty()) sendAll(fd, body.data(), body.size());
}

void AiControlServer::sendJsonError(socket_t fd, int status, const std::string& message)
{
    const std::string b = "{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}";
    sendResponse(fd, status, "application/json", b);
}

void AiControlServer::sendJsonOk(socket_t fd, const std::string& body)
{
    // `body` is expected to be either an empty JSON object `{}` or a body
    // starting with `{` and ending with `}`. We inject `"ok":true` after
    // the opening brace so every successful response shares the same shape.
    if (body.size() < 2 || body.front() != '{' || body.back() != '}') {
        sendResponse(fd, 200, "application/json",
                     "{\"ok\":true,\"data\":" + body + "}");
        return;
    }
    if (body.size() == 2) {
        sendResponse(fd, 200, "application/json", "{\"ok\":true}");
        return;
    }
    std::string merged = "{\"ok\":true,";
    merged.append(body.begin() + 1, body.end());
    sendResponse(fd, 200, "application/json", merged);
}

namespace {

/// Strip the ":port" from a Host header value, leaving the host part.
/// IPv6 literals are bracketed, so the last colon only counts outside them.
std::string hostPartOf(const std::string& host)
{
    if (!host.empty() && host.front() == '[') {
        const std::size_t close = host.find(']');
        return (close == std::string::npos) ? host : host.substr(1, close - 1);
    }
    const std::size_t colon = host.rfind(':');
    return (colon == std::string::npos) ? host : host.substr(0, colon);
}

}  // namespace

bool AiControlServer::hostHeaderIsLoopback(const Request& req)
{
    const std::string host = hostPartOf(req.headerValue("Host"));
    // No Host at all: HTTP/1.0 native clients omit it, and a browser never
    // does — so an empty one cannot be a rebound page.
    if (host.empty()) return true;
    // A literal loopback address only. A NAME is refused even if it currently
    // resolves to 127.0.0.1: resolving it again is precisely the attack.
    return host == "127.0.0.1" || host == "::1" || host == "0:0:0:0:0:0:0:1";
}

bool AiControlServer::checkAuth(const Request& req) const
{
    std::string configured;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        configured = authToken_;
    }
    // Keep token-less localhost access for native clients, but do not expose
    // the emulator control plane to arbitrary browser pages. Browsers attach
    // Origin to cross-origin requests; command-line/native clients do not.
    // Configuring a token explicitly opts into browser access guarded by that
    // secret.
    //
    // The Origin test alone is NOT enough, and only for reads. Per Fetch, a
    // browser attaches Origin when the response tainting is `cors` or the
    // method is anything other than GET/HEAD — so a SAME-ORIGIN GET carries no
    // Origin at all. That is exactly the request a DNS-rebound page issues:
    // the victim loads attacker.example, its name is re-pointed at 127.0.0.1,
    // and the page then reads /mem, /screen.ppm and /status same-origin with
    // no Origin header to give it away. POSTs were always safe (Origin is
    // unconditional for non-GET); reads were not.
    //
    // Host is the discriminator: a rebound page sends the attacker's hostname,
    // a native client sends the loopback address it dialled (or nothing at
    // all, on HTTP/1.0).
    if (configured.empty())
        return req.headerValue("Origin").empty() && hostHeaderIsLoopback(req);
    return req.headerValue("X-POM2-Token") == configured;
}

// ─── Dispatch ─────────────────────────────────────────────────────────────

void AiControlServer::handleClient(socket_t fd)
{
    Request req;
    if (!readRequest(fd, req)) {
        sendJsonError(fd, 400, "malformed request");
        return;
    }
    // CORS preflight — bypasses auth so a browser-hosted agent can probe
    // the API. Auth still gates the real request that follows.
    if (req.method == "OPTIONS") {
        char head[256];
        const int n = std::snprintf(head, sizeof(head),
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-POM2-Token\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        if (n > 0) sendAll(fd, head, static_cast<size_t>(n));
        return;
    }
    if (!checkAuth(req)) {
        sendJsonError(fd, 401, "missing or invalid X-POM2-Token");
        return;
    }

    if (req.path == "/status")               return handleStatus(fd, req);
    if (req.path == "/reset")                return handleReset(fd, req);
    if (req.path == "/cpu") {
        return req.method == "GET" ? handleCpuGet(fd, req)
                                   : handleCpuSet(fd, req);
    }
    if (req.path == "/mem") {
        return req.method == "GET" ? handleMemGet(fd, req)
                                   : handleMemSet(fd, req);
    }
    if (req.path == "/keyboard")             return handleKeyboard(fd, req);
    if (req.path == "/disk")                 return handleDiskInsert(fd, req);
    if (req.path == "/eject")                return handleDiskEject(fd, req);
    if (req.path == "/snapshot/save")        return handleSnapshotSave(fd, req);
    if (req.path == "/snapshot/load")        return handleSnapshotLoad(fd, req);
    if (req.path == "/speed")                return handleSpeed(fd, req);
    if (req.path == "/screen.ppm")           return handleScreen(fd, req);
    if (req.path == "/mouse")                return handleMouse(fd, req);

    sendJsonError(fd, 404, "no such endpoint: " + req.path);
}

// ─── Endpoint implementations ─────────────────────────────────────────────

void AiControlServer::handleStatus(socket_t fd, const Request& /*req*/)
{
    std::string profile;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        profile = profileLabel_;
    }
    // Snapshot the CPU/memory state under stateMutex so we don't sample a
    // half-written PC or A register mid-instruction.
    EmulationController::Mode mode = ctrl_->getMode();
    int cpf = ctrl_->getCyclesPerFrame();

    // Sample CPU regs AND disk state under ONE lock so the /status JSON is a
    // single coherent snapshot (the worker releases the lock between 4096-
    // cycle chunks, so two separate lock scopes could straddle an emulated-
    // time gap). Lock-then-check also serialises against a profile switch
    // nulling disk6_ (see `detach()`).
    uint16_t pc; uint8_t a, x, y, p, sp; uint64_t cycles;
    std::string cpuMode;
    std::string disks = "[]";
    {
        auto st = ctrl_->lockState();
        M6502& cpu = st.cpu();
        pc = cpu.getProgramCounter();
        a  = cpu.getAccumulator();
        x  = cpu.getXRegister();
        y  = cpu.getYRegister();
        p  = cpu.getStatusRegister();
        sp = cpu.getStackPointer();
        cycles = st.memory().getCycleCounter();
        cpuMode = cpuModeName(cpu.getCpuMode());
        if (disk6_) {
            std::ostringstream oss;
            oss << "[";
            for (int d = 0; d < DiskIICard::kDriveCount; ++d) {
                if (d) oss << ",";
                oss << "{\"slot\":" << disk6_->getSlot()
                    << ",\"drive\":" << d
                    << ",\"path\":\"" << jsonEscape(disk6_->getDiskPath(d)) << "\""
                    << ",\"loaded\":" << (disk6_->isDiskLoaded(d) ? "true" : "false")
                    << ",\"track\":" << disk6_->getCurrentTrack(d)
                    << "}";
            }
            oss << "]";
            disks = oss.str();
        }
    }

    std::ostringstream oss;
    oss << "{"
        << "\"profile\":\""        << jsonEscape(profile) << "\","
        << "\"cpu_mode\":\""       << cpuMode              << "\","
        << "\"mode\":\""           << runModeName(mode)    << "\","
        << "\"cycles_per_frame\":" << cpf                  << ","
        << "\"requests_served\":"  << requestsServed_.load() << ","
        << "\"cpu\":{"
            << "\"pc\":"     << pc     << ","
            << "\"a\":"      << +a     << ","
            << "\"x\":"      << +x     << ","
            << "\"y\":"      << +y     << ","
            << "\"p\":"      << +p     << ","
            << "\"sp\":"     << +sp    << ","
            << "\"cycles\":" << cycles
        << "},"
        << "\"disks\":" << disks
        << "}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleReset(socket_t fd, const Request& req)
{
    if (req.method != "POST") {
        sendJsonError(fd, 405, "POST only"); return;
    }
    std::string kind = jsonGetString(req.body, "kind");
    if (kind.empty()) kind = "hard";
    if      (kind == "soft") ctrl_->softReset();
    else if (kind == "hard") ctrl_->hardReset();
    else if (kind == "cold") ctrl_->coldBoot();
    else { sendJsonError(fd, 400, "kind must be soft|hard|cold"); return; }
    sendJsonOk(fd, "{\"kind\":\"" + kind + "\"}");
}

void AiControlServer::handleCpuGet(socket_t fd, const Request& /*req*/)
{
    auto st = ctrl_->lockState();
    M6502& cpu = st.cpu();
    std::ostringstream oss;
    oss << "{"
        << "\"pc\":"     << cpu.getProgramCounter() << ","
        << "\"a\":"      << +cpu.getAccumulator()   << ","
        << "\"x\":"      << +cpu.getXRegister()     << ","
        << "\"y\":"      << +cpu.getYRegister()     << ","
        << "\"p\":"      << +cpu.getStatusRegister()<< ","
        << "\"sp\":"     << +cpu.getStackPointer()  << ","
        << "\"cpu_mode\":\"" << cpuModeName(cpu.getCpuMode()) << "\","
        << "\"cycles\":" << st.memory().getCycleCounter()
        << "}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleCpuSet(socket_t fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    auto st = ctrl_->lockState();
    M6502& cpu = st.cpu();
    long v = 0;
    // Full register set — the setters exist on M6502 (M6502.h:133-137,
    // added for snapshot restore), so the documented POST /cpu API accepts
    // every register, not just PC.
    if (jsonGetInt(req.body, "pc", v)) cpu.setProgramCounter(static_cast<uint16_t>(v & 0xFFFF));
    if (jsonGetInt(req.body, "a",  v)) cpu.setAccumulator   (static_cast<uint8_t>(v & 0xFF));
    if (jsonGetInt(req.body, "x",  v)) cpu.setXRegister     (static_cast<uint8_t>(v & 0xFF));
    if (jsonGetInt(req.body, "y",  v)) cpu.setYRegister     (static_cast<uint8_t>(v & 0xFF));
    if (jsonGetInt(req.body, "p",  v)) cpu.setStatusRegister(static_cast<uint8_t>(v & 0xFF));
    if (jsonGetInt(req.body, "sp", v)) cpu.setStackPointer  (static_cast<uint8_t>(v & 0xFF));
    sendJsonOk(fd, "{}");
}

void AiControlServer::handleMemGet(socket_t fd, const Request& req)
{
    if (req.method != "GET") { sendJsonError(fd, 405, "GET only"); return; }
    long addr = -1, len = -1;
    try { addr = std::stol(queryParam(req.query, "addr"), nullptr, 0); } catch (...) {}
    try { len  = std::stol(queryParam(req.query, "len"),  nullptr, 0); } catch (...) {}
    // Optional `bank` query param: "main" (default) or "aux" — //e aux 64KB.
    const std::string bank = queryParam(req.query, "bank");
    const bool useAux = (bank == "aux");
    if (addr < 0 || addr >= 0x10000) { sendJsonError(fd, 400, "addr out of range"); return; }
    if (len  < 0 || len  > 4096)     { sendJsonError(fd, 400, "len must be 0..4096"); return; }
    if (addr + len > 0x10000) len = 0x10000 - addr;

    std::vector<uint8_t> buf(static_cast<size_t>(len));
    {
        auto st = ctrl_->lockState();
        const uint8_t* mem = useAux ? st.memory().auxData()
                                    : st.memory().data();
        std::memcpy(buf.data(), mem + addr, static_cast<size_t>(len));
    }
    std::ostringstream oss;
    oss << "{\"addr\":" << addr
        << ",\"len\":" << len
        << ",\"bank\":\"" << (useAux ? "aux" : "main") << "\""
        << ",\"data\":\"" << bytesToHex(buf.data(), buf.size()) << "\"}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleMemSet(socket_t fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    long addr = -1;
    try { addr = std::stol(queryParam(req.query, "addr"), nullptr, 0); } catch (...) {}
    if (addr < 0 || addr >= 0x10000) { sendJsonError(fd, 400, "addr out of range"); return; }

    const std::string hex = jsonGetString(req.body, "data");
    if (hex.empty()) { sendJsonError(fd, 400, "missing \"data\" hex string"); return; }
    std::vector<uint8_t> bytes;
    if (!hexToBytes(hex, bytes)) { sendJsonError(fd, 400, "data is not even-length hex"); return; }
    if (addr + static_cast<long>(bytes.size()) > 0x10000) {
        sendJsonError(fd, 400, "write overflows address space"); return;
    }
    // /mem is a RAM editor, not an MMIO bus driver. Writes into $C000-$CFFF
    // trigger soft switches and $D000-$FFFF is normally ROM-protected; the
    // old endpoint nevertheless replied `written:N` for bytes that stayed
    // unchanged. Refuse both ranges explicitly and report honestly.
    if (addr + static_cast<long>(bytes.size()) > 0xC000) {
        sendJsonError(fd, 400, "write targets I/O or ROM; RAM ends at $BFFF");
        return;
    }
    size_t written = 0;
    {
        auto st = ctrl_->lockState();
        Memory& mem = st.memory();
        for (size_t i = 0; i < bytes.size(); ++i) {
            // memWrite respects ROM protection and routes through soft-
            // switches; that's exactly what we want for "drive the Apple
            // II as a peer" — agents should not be able to overwrite the
            // monitor ROM by accident.
            mem.memWrite(static_cast<uint16_t>(addr + i), bytes[i]);
            ++written;
        }
    }
    std::ostringstream oss;
    oss << "{\"addr\":" << addr << ",\"written\":" << written << "}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleKeyboard(socket_t fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    const std::string text = jsonGetString(req.body, "text");
    const std::string raw  = jsonGetString(req.body, "raw");
    if (text.empty() && raw.empty()) {
        sendJsonError(fd, 400, "supply \"text\" or \"raw\""); return;
    }
    size_t n = 0;
    // Deliberately NOT under `lockState()`: the keyboard latch and paste
    // queue have their own finer-grained `Memory::kbMutex`, taken inside
    // pasteText / pasteRawKeys (Memory.cpp:1147,1166). That is what lets the
    // UI and this HTTP thread inject keys without contending with the worker
    // on every keystroke — see the note at Memory.cpp:1260. The raw
    // `memory()` accessor is correct here and nowhere else in this file.
    if (!text.empty()) n += ctrl_->memory().pasteText(text);
    if (!raw.empty())  n += ctrl_->memory().pasteRawKeys(raw.data(), raw.size());
    sendJsonOk(fd, "{\"queued\":" + std::to_string(n) + "}");
}

void AiControlServer::handleMouse(socket_t fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    if (!ctrl_)               { sendJsonError(fd, 503, "no controller"); return; }

    // All keys optional. Absolute "x"/"y" win over relative "dx"/"dy".
    long dx = 0, dy = 0, ax = 0, ay = 0, btn = 0, rst = 0;
    const bool haveDx  = jsonGetInt(req.body, "dx",    dx);
    const bool haveDy  = jsonGetInt(req.body, "dy",    dy);
    const bool haveAx  = jsonGetInt(req.body, "x",     ax);
    const bool haveAy  = jsonGetInt(req.body, "y",     ay);
    const bool haveBtn = jsonGetInt(req.body, "btn",   btn);
    jsonGetInt(req.body, "reset", rst);

    // Clamp per-call delta to ±127 — the MCU's 8-bit signed wrap window,
    // matching MainWindow::onMouseMove. Larger deltas must be split across
    // requests (which is what we want anyway: one "pixel ramp" per tick so
    // the live CPU drains quadrature between calls).
    auto clamp127 = [](long v) -> int {
        if (v >  127) return  127;
        if (v < -127) return -127;
        return static_cast<int>(v);
    };

    int slot = -1;
    uint8_t outX = 0, outY = 0;
    bool outBtn = false;
    bool noCard = false;
    {
        auto st = ctrl_->lockState();
        SlotBus& bus = st.memory().slotBus();
        // Two interchangeable mouse cards exist: the MAME-LLE MouseCard
        // (MC68705 mask ROM) and the AppleWin-HLE MouseCardAppleWin — a
        // SIBLING class, not a subclass, and the default built-in mouse on
        // every //c profile, so probing for MouseCard alone left /mouse
        // returning 503 on the //c family. The HLE variant is identified
        // by its name tag + static_cast rather than dynamic_cast so this
        // TU doesn't pull MouseCardAppleWin.o's typeinfo into headless
        // binaries (its setHostMouse/getSlot are header-inline).
        MouseCard*         mouseLle = nullptr;
        MouseCardAppleWin* mouseHle = nullptr;
        for (int s = 1; s <= 7 && !mouseLle && !mouseHle; ++s) {
            SlotPeripheral* p = bus.peripheral(s);
            if (!p) continue;
            mouseLle = dynamic_cast<MouseCard*>(p);
            if (!mouseLle && p->name() == MouseCardAppleWin::kCardName)
                mouseHle = static_cast<MouseCardAppleWin*>(p);
        }
        // NOT sent from here: writing the reply is a socket write with a 4 s
        // send timeout, and the emulated machine — CPU worker and the UI
        // thread's next frame both — would wait behind it for as long as the
        // client takes to read. Note it and answer once the lock is gone.
        noCard = (!mouseLle && !mouseHle);
        if (!noCard) {
            slot = mouseLle ? mouseLle->getSlot() : mouseHle->getSlot();

            if (rst) { mouseAccumX_ = 0; mouseAccumY_ = 0; }

            if (haveAx)      mouseAccumX_ = static_cast<uint8_t>(ax & 0xFF);
            else if (haveDx) mouseAccumX_ = static_cast<uint8_t>(mouseAccumX_ + clamp127(dx));
            if (haveAy)      mouseAccumY_ = static_cast<uint8_t>(ay & 0xFF);
            else if (haveDy) mouseAccumY_ = static_cast<uint8_t>(mouseAccumY_ + clamp127(dy));
            if (haveBtn)     mouseBtn_ = (btn != 0);

            if (mouseLle) mouseLle->setHostMouse(mouseAccumX_, mouseAccumY_, mouseBtn_);
            else          mouseHle->setHostMouse(mouseAccumX_, mouseAccumY_, mouseBtn_);
            outX = mouseAccumX_; outY = mouseAccumY_; outBtn = mouseBtn_;
        }
    }

    if (noCard) { sendJsonError(fd, 503, "no Mouse Card plugged"); return; }

    std::ostringstream oss;
    oss << "{\"x\":"   << +outX
        << ",\"y\":"   << +outY
        << ",\"btn\":" << (outBtn ? 1 : 0)
        << ",\"slot\":" << slot << "}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleDiskInsert(socket_t fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    // Parse the body BEFORE locking — these are pure JSON ops, no card
    // access. Defer the disk6_ null-check to inside the lock so a
    // concurrent profile switch (which detaches() before tearing down
    // the card) can't slip a null past us.
    long slot  = -1;                     // -1 = "the bound Disk II card"
    long drive = 0;
    jsonGetInt(req.body, "slot",  slot);
    jsonGetInt(req.body, "drive", drive);
    const std::string path = jsonGetString(req.body, "path");
    if (drive < 0 || drive >= DiskIICard::kDriveCount) {
        sendJsonError(fd, 400, "drive must be 0 or 1"); return;
    }
    if (path.empty()) { sendJsonError(fd, 400, "missing \"path\""); return; }
    const auto safe = safeCwdRelativePath(path, /*mustExist=*/true);
    if (!safe) {
        sendJsonError(fd, 403,
            "path rejected: must resolve to a file under the emulator "
            "working directory (received \"" + path + "\")");
        return;
    }
    // Two-phase mount: the file read stays OUT of stateMutex. An agent pushing
    // a 32 MB image used to freeze the emulator and the window for the whole
    // read, and the HTTP handler is exactly the caller with no way for the
    // user to see why.
    //
    // Spelled out here rather than through pom2::mountDiskII because this
    // handler runs on the SERVER's thread, not the UI's. `disk6_` is a card
    // pointer that a profile switch nulls under `stateMutex` (see
    // MainWindow_Slots.cpp's applyProfile steps 3-4), so it may not be read
    // once and trusted across an unlocked phase 1 — it is re-checked under the
    // lock before the install. The UI callers get that guarantee for free from
    // the SlotBus topology rule, being on the thread that does the swapping.
    // `disk6_` is the PRIMARY (lowest-slot) Disk II, not necessarily slot
    // 6: with cards in slots 5 AND 6 the old hard-coded `slot != 6` check
    // accepted "slot 6" and then operated on the slot-5 card — the wrong
    // medium touched, 200 returned. Validate against — and report — the
    // bound card's REAL slot, read under the lock like the pointer itself.
    bool writeBack = false;
    int  boundSlot = -1;
    {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        if (!disk6_) { sendJsonError(fd, 503, "no Disk II card plugged"); return; }
        writeBack = disk6_->isWriteBackEnabled();
        boundSlot = disk6_->getSlot();
    }
    if (slot != -1 && slot != boundSlot) {
        sendJsonError(fd, 400, "Disk II endpoints drive the primary card, "
                      "which is in slot " + std::to_string(boundSlot));
        return;
    }

    DiskImage   prepared;
    std::string errMsg;
    if (!DiskIICard::prepareDisk(*safe, writeBack, prepared, errMsg)) {
        sendJsonError(fd, 400, "insert failed: " + errMsg);
        return;
    }

    bool noCard = false;
    bool ok     = false;
    {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        if (!disk6_) {
            noCard = true;
        } else {
            ok = disk6_->installDisk(static_cast<int>(drive), std::move(prepared));
            if (!ok) errMsg = disk6_->getLastError(static_cast<int>(drive));
        }
    }
    if (noCard) { sendJsonError(fd, 503, "no Disk II card plugged"); return; }
    if (!ok)    { sendJsonError(fd, 400, "insert failed: " + errMsg); return; }
    sendJsonOk(fd, "{\"slot\":" + std::to_string(boundSlot) +
                   ",\"drive\":" + std::to_string(drive) +
                   ",\"path\":\"" + jsonEscape(*safe) + "\"}");
}

void AiControlServer::handleDiskEject(socket_t fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    // `slot` is documented and its sibling /disk validates it — this handler
    // used to parse only `drive` and silently EJECT SLOT 6 whatever the caller
    // asked for. An agent unmounting SmartPort media with {"slot":5,"drive":1}
    // got a 200 saying so, while the Disk II in slot 6 was flushed to disk and
    // dropped instead: the wrong medium written back, and nothing to see.
    long slot  = -1;                     // -1 = "the bound Disk II card"
    long drive = 0;
    jsonGetInt(req.body, "slot",  slot);
    jsonGetInt(req.body, "drive", drive);
    if (drive < 0 || drive >= DiskIICard::kDriveCount) {
        sendJsonError(fd, 400, "drive must be 0 or 1"); return;
    }
    bool noCard = false;
    bool wrongSlot = false;
    bool ejected = false;
    int  boundSlot = -1;
    std::string errMsg;
    // Two-phase, like the insert path above: `ejectDisk` re-encodes the dirty
    // tracks, writes the image and fsyncs twice, and doing that under
    // `stateMutex` froze the CPU worker and the window for the whole
    // round-trip. Phase 1 lifts the medium out (a memcpy), phase 2 writes it
    // with the lock released, phase 3 puts it back if the write failed.
    std::unique_ptr<DiskImage> pending;
    {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        if (!disk6_) noCard = true;
        else {
            // Same primary-vs-slot-6 rule as /disk/insert: validate the
            // requested slot against the bound card's real one BEFORE
            // touching a drive — a hard-coded "6" here ejected (and
            // flushed) the slot-5 primary while confirming slot 6.
            boundSlot = disk6_->getSlot();
            if (slot != -1 && slot != boundSlot) wrongSlot = true;
            else {
                pending = disk6_->takeEjectWriteBack(static_cast<int>(drive));
                ejected = true;
            }
        }
    }
    if (pending) {
        if (!DiskIICard::commitEjectWriteBack(*pending, errMsg)) {
            ejected = false;
            std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
            if (disk6_)
                (void)disk6_->restoreEjected(static_cast<int>(drive),
                                             std::move(pending));
        }
    }
    if (noCard) { sendJsonError(fd, 503, "no Disk II card plugged"); return; }
    if (wrongSlot) {
        sendJsonError(fd, 400, "Disk II endpoints drive the primary card, "
                      "which is in slot " + std::to_string(boundSlot));
        return;
    }
    if (!ejected) { sendJsonError(fd, 500, "eject failed: " + errMsg); return; }
    sendJsonOk(fd, "{\"slot\":" + std::to_string(boundSlot) +
                   ",\"drive\":" + std::to_string(drive) + "}");
}

void AiControlServer::handleSnapshotSave(socket_t fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    const std::string path = jsonGetString(req.body, "path");
    if (path.empty()) { sendJsonError(fd, 400, "missing \"path\""); return; }
    // BEFORE the extension test, because that test is exactly what a NUL
    // defeats: `"roms/apple2e.rom\0.pom2snap"` ends with ".pom2snap" as far
    // as std::string is concerned, and the write underneath — which goes
    // through c_str() — lands on the ROM. See pathHasControlBytes.
    if (pathHasControlBytes(path)) {
        sendJsonError(fd, 400,
            "path contains a control byte (NUL or similar) — refused");
        return;
    }
    // The save path must end with `.pom2snap`. Without this an agent with
    // an empty / leaked auth token could `POST /snapshot/save {"path":
    // "roms/apple2.rom"}` and shred a ROM, settings file, or disk image
    // with snapshot bytes — `safeCwdRelativePath` only checks cwd
    // containment, not extension. The load path is read-only and the
    // magic-byte check rejects non-snapshots, so loads stay permissive.
    constexpr const char* kSaveExt = ".pom2snap";
    constexpr size_t      kSaveExtLen = 9;
    if (path.size() <= kSaveExtLen ||
        path.compare(path.size() - kSaveExtLen, kSaveExtLen, kSaveExt) != 0) {
        sendJsonError(fd, 400,
            "path must end with \".pom2snap\" — refusing to write snapshot "
            "bytes over an unrelated file (received \"" + path + "\")");
        return;
    }
    const auto safe = safeCwdRelativePath(path, /*mustExist=*/false);
    if (!safe) {
        sendJsonError(fd, 403,
            "path rejected: must resolve under the emulator working "
            "directory (received \"" + path + "\")");
        return;
    }
    // Re-check on the RESOLVED path. The test above ran on the request's
    // string; `weakly_canonical` resolves symlinks and `..`, so what is
    // actually about to be written is this, and it is what the extension
    // rule has to hold for.
    if (safe->size() <= kSaveExtLen ||
        safe->compare(safe->size() - kSaveExtLen, kSaveExtLen, kSaveExt) != 0) {
        sendJsonError(fd, 400,
            "path resolves to \"" + *safe +
            "\", which does not end with \".pom2snap\" — refused");
        return;
    }

    // Serialise under the lock, WRITE outside it. The capture is RAM-only —
    // CPU regs, 64 KiB main RAM, the MEX extended state — and takes
    // microseconds; the file write and its two fsyncs are what used to hold
    // `stateMutex` (30 ms for 4 MB on the measured host), freezing the
    // emulator and the window for an agent's snapshot request.
    //
    // Disk state is deliberately excluded per CLAUDE.md. See MachineSnapshot
    // for the exact section roster (shared with the rewind ring buffer).
    std::vector<uint8_t> blob;
    bool captured = false;
    {
        SnapshotWriter mem(blob, ctrl_->machineId());
        auto st = ctrl_->lockState();
        pom2::captureMachineState(mem, st.cpu(), st.memory());
        captured = mem.finish();
    }
    if (!captured) {
        sendJsonError(fd, 500, "snapshot capture failed");
        return;
    }
    std::error_code ec;
    if (!pom2::writeFileAtomic(*safe, blob.data(), blob.size(), ec)) {
        sendJsonError(fd, 500,
            "snapshot write failed for " + *safe + ": " + ec.message());
        return;
    }
    sendJsonOk(fd, "{\"path\":\"" + jsonEscape(*safe) + "\"}");
}

void AiControlServer::handleSnapshotLoad(socket_t fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    const std::string path = jsonGetString(req.body, "path");
    if (path.empty()) { sendJsonError(fd, 400, "missing \"path\""); return; }
    const auto safe = safeCwdRelativePath(path, /*mustExist=*/true);
    if (!safe) {
        sendJsonError(fd, 403,
            "path rejected: must resolve to a file under the emulator "
            "working directory (received \"" + path + "\")");
        return;
    }

    // Read the whole file BEFORE taking the lock, then parse from memory. The
    // file-backed reader pulls its bytes lazily from inside
    // restoreMachineState(), so constructing it here and restoring under the
    // lock still put the disk read inside the critical section.
    std::vector<uint8_t> blob;
    {
        std::ifstream in(*safe, std::ios::binary);
        if (!in) {
            sendJsonError(fd, 400, "cannot read " + *safe);
            return;
        }
        blob.assign(std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>());
        if (!in && !in.eof()) {
            sendJsonError(fd, 400, "read error on " + *safe);
            return;
        }
    }
    SnapshotReader r(blob.data(), blob.size());
    if (!r.good()) { sendJsonError(fd, 400, "cannot read " + *safe + ": " + r.error()); return; }
    // Machine identity before any state is touched — see the same guard in
    // CliRunner. CPU/MEM/MEX restore unconditionally, so a snapshot from a
    // different Apple puts PC and RAM against a foreign ROM and memory map;
    // an agent driving this endpoint gets a named 400 instead of a machine
    // that freezes or quietly runs the wrong code. Identity 0 = written
    // before the field existed, and still loads.
    {
        const std::uint32_t want = ctrl_->machineId();
        if (want != 0 && r.machineId() != 0 && r.machineId() != want) {
            const std::string_view from =
                pom2::profileNameForMachineId(r.machineId());
            sendJsonError(fd, 400,
                "snapshot refused: taken on " +
                (from.empty() ? std::string("another machine")
                              : std::string(from)) +
                ", this session is " +
                std::string(pom2::profileNameForMachineId(want)));
            return;
        }
    }
    // The restore under the lock, the HTTP reply outside it. Writing the reply
    // is a socket write with a 4 s send timeout, and the state mutex is taken
    // by the CPU worker every 4096-cycle chunk and by the UI thread to paint
    // every frame — so answering from inside this scope made a slow or stalled
    // client freeze the machine and the window together (CLAUDE.md, "never
    // hold stateMutex across file I/O"; a socket is worse, since the far end
    // decides when it drains).
    bool        ok = false;
    std::string error;
    {
        auto st = ctrl_->lockState();
        // Shared with the rewind ring buffer. Preserves the CPU-section length
        // gate (crafted-snapshot over-read hardening) and the MEX size cap; an
        // oversized MEX aborts the restore with a 400.
        const auto res = pom2::restoreMachineState(r, st.cpu(), st.memory());
        ok    = res.ok;
        error = res.error;
        if (ok) {
            // The restore usually rewinds mem's cycleCounter, which strands
            // every free-running audio device on the abandoned timeline (the
            // speaker's reconstruction cursor only snaps FORWARD; the
            // cassette's stamps wrap under unsigned subtraction). This is the
            // same call the rewind transport makes — hand-rolling
            // `speaker().reset()` here missed the deck entirely.
            ctrl_->noteTimeJump();
            // The rewind ring recorded the abandoned timeline — its stamps
            // would break indexForCycle's monotonicity — so drop it after
            // SUCCESS. Failed loads roll back completely and must preserve
            // the existing timeline.
            ctrl_->rewind().clear();
        }
    }
    if (!ok) { sendJsonError(fd, 400, error); return; }
    sendJsonOk(fd, "{\"path\":\"" + jsonEscape(*safe) + "\"}");
}

void AiControlServer::handleSpeed(socket_t fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    long cpf = -1;
    if (jsonGetInt(req.body, "cycles_per_frame", cpf)) {
        // The value becomes the per-"frame" worker cycle budget. An
        // unbounded value freezes the UI (billions of cycles per frame
        // holding the state lock), and values whose low 32 bits are ≤0
        // truncate to a dead/negative budget when cast to int. Bound it
        // to the shared POM2_MAX_CYCLES_PER_FRAME ceiling (CpuClock.h —
        // the same constant the CLI --speed clamp uses) and reject
        // anything outside [1, max] instead of silently casting.
        constexpr long kMaxCpf = POM2_MAX_CYCLES_PER_FRAME;
        if (cpf <= 0 || cpf > kMaxCpf) {
            sendJsonError(fd, 400,
                "cycles_per_frame out of range (1.." + std::to_string(kMaxCpf) + ")");
            return;
        }
        ctrl_->setCyclesPerFrame(static_cast<int>(cpf));
    } else {
        const std::string preset = jsonGetString(req.body, "preset");
        // 1× follows the active video standard (17045 NTSC / 20313 PAL) —
        // same rule as the toolbar buckets.
        const int base =
            pom2VideoTiming(ctrl_->getVideoStandard()).cyclesPerFrame;
        if      (preset == "1x")  ctrl_->setCyclesPerFrame(base);
        else if (preset == "2x")  ctrl_->setCyclesPerFrame(base * 2);
        else if (preset == "max") ctrl_->setCyclesPerFrame(1000000);
        else { sendJsonError(fd, 400, "supply cycles_per_frame or preset 1x|2x|max"); return; }
    }
    std::ostringstream oss;
    oss << "{\"cycles_per_frame\":" << ctrl_->getCyclesPerFrame() << "}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleScreen(socket_t fd, const Request& /*req*/)
{
    if (!display_) { sendJsonError(fd, 503, "no display attached"); return; }
    // Render under stateMutex so we don't race the CPU thread mutating
    // VRAM mid-scan — same contract as MainWindow::render().
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    {
        // demodMutex covers render + demod + pixel copy; stateMutex covers
        // only render() (the guest-RAM snapshot). The UI thread runs the
        // same phases under the same two locks in the same order
        // (stateMutex → demodMutex) — without the shared demodMutex the
        // two threads raced over the display-owned demod buffers.
        std::unique_lock<std::mutex> demodLk(display_->demodMutex(),
                                             std::defer_lock);
        {
            auto st = ctrl_->lockState();
            demodLk.lock();
            display_->render(st.memory());
        }
        // OE-GPU mode demodulates in a GLSL pass MainWindow owns; pixels()
        // would return the LUT fallback framebuffer, not the composite image
        // on screen. Schedule the pixel-identical CPU demod (pinned by
        // oe_demod_gpu_cpu_parity) so the capture matches the display —
        // pixels() below runs it lazily. No-op in every other mode.
        display_->demodCompositeForCapture();
        // Apple2Display packs pixels as `0xAABBGGRR` (RGBA in LE memory:
        // R G B A bytes) — see Apple2Display.cpp ctor + the MainWindow
        // upload site. We re-interpret as raw bytes and drop the alpha
        // byte to produce a PPM-friendly RGB triplet stream.
        const uint8_t* rgba = reinterpret_cast<const uint8_t*>(display_->pixels());
        w = display_->width();
        h = display_->height();
        rgb.resize(static_cast<size_t>(w) * h * 3);
        for (int i = 0; i < w * h; ++i) {
            rgb[i * 3 + 0] = rgba[i * 4 + 0];
            rgb[i * 3 + 1] = rgba[i * 4 + 1];
            rgb[i * 3 + 2] = rgba[i * 4 + 2];
        }
    }
    char header[64];
    const int hn = std::snprintf(header, sizeof(header), "P6\n%d %d\n255\n", w, h);
    std::string body;
    body.reserve(static_cast<size_t>(hn) + rgb.size());
    body.append(header, static_cast<size_t>(hn));
    body.append(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    sendResponse(fd, 200, "image/x-portable-pixmap", body);
}
#endif // POM2_HAS_SOCKETS

} // namespace pom2
