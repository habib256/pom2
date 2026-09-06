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

// Host-socket compatibility layer test — pins src/SocketCompat.h.
//
// This file exists because the POSIX-vs-Winsock differences are SILENT:
// code that compiles clean against Winsock can still be wrong (see the
// five traps documented in the header). The test runs on the POSIX side
// in CI, but its value is that the SAME source is what the Windows build
// compiles, so the contract each helper promises is asserted once and
// holds for both.
//
// What is pinned, in order of how expensive the mistake would be:
//
//   1. HANDLE VALIDITY. `kInvalidSocket` / `isValidSocket()` must be the
//      test, never `>= 0` or `== -1`: Winsock's SOCKET is unsigned and
//      its failure value is INVALID_SOCKET, so the POSIX idiom inverts
//      its meaning there without a warning.
//   2. A REFUSED non-blocking connect must be REPORTED. This is the one
//      W5100Device::poll() depends on to move a socket out of SYNSENT:
//      if the readiness wait can only report success, a guest polls
//      SN_SR forever on a connection that was refused. It is also why
//      the Windows path is select() with an exception set rather than
//      WSAPoll.
//   3. A SUCCESSFUL non-blocking connect resolves to connectResult()==0.
//   4. closeHostSocket() invalidates the handle it was given, so a
//      double close cannot reach a recycled descriptor.
//   5. An idle listener yields Retry, not Accepted or Shutdown — the
//      property the two TCP workers' stop flags depend on.
//   6. THE BIND POLICY IS NOT `SO_REUSEADDR`. That option means opposite
//      things on the two families: POSIX only relaxes TIME_WAIT, Winsock
//      hands the address to whoever binds it LAST, even while the first
//      socket is listening. Setting it the POSIX way on Windows lets any
//      local process take over 127.0.0.1:6503 and serve the agent's
//      requests — token included. `setListenerBindPolicy()` is the split,
//      and the property asserted here is the one that must hold on both:
//      a second LIVE bind to a listening address always fails.

#include "SocketCompat.h"
#include "SocketUtil.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#if !POM2_HAS_SOCKETS
int main()
{
    std::puts("SKIP: built without host sockets (Emscripten)");
    return 77;   // ctest SKIP_RETURN_CODE
}
#else

namespace {

using namespace pom2;

/// Bind a listener on an ephemeral loopback port and report the port.
socket_t makeListener(uint16_t& portOut, int backlog = 4)
{
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(isValidSocket(s));
    // The same policy the two real listeners use, so this helper cannot
    // drift away from what it is meant to be modelling.
    setListenerBindPolicy(s);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;                       // kernel picks
    assert(::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::listen(s, backlog) == 0);

    sockaddr_in bound{};
    socklen_c len = sizeof(bound);
    assert(::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0);
    portOut = ntohs(bound.sin_port);
    return s;
}

/// Start a non-blocking connect to 127.0.0.1:port. Returns the socket;
/// `inFlight` says whether it is still handshaking (the usual case).
socket_t startConnect(uint16_t port, bool& inFlight)
{
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(isValidSocket(s));
    assert(setNonBlocking(s));

    sockaddr_in dest{};
    dest.sin_family      = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port        = htons(port);
    const int r = ::connect(s, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    if (r == 0) { inFlight = false; return s; }
    // Anything else must be the "in progress" spelling for this platform;
    // a connect that fails outright here would mean errInProgress() is
    // testing the wrong code.
    assert(errInProgress(lastSocketError()));
    inFlight = true;
    return s;
}

// ─── 1: handle validity ───────────────────────────────────────────────
void testHandleValidity()
{
    socket_t s = kInvalidSocket;
    assert(!isValidSocket(s));

    s = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(isValidSocket(s));

    // closeHostSocket takes the handle by reference and invalidates it,
    // so the caller cannot leave a dangling one behind.
    closeHostSocket(s);
    assert(!isValidSocket(s));
    // Idempotent: a second call on an already-cleared handle is a no-op,
    // never a close of some recycled descriptor.
    closeHostSocket(s);
    assert(!isValidSocket(s));

    std::puts("OK handle_validity");
}

// ─── 2: a refused connect is reported ─────────────────────────────────
//
// The failure mode this guards against is a readiness wait that only
// signals success. Bind a port, learn its number, then close it — nothing
// is listening there, so the connect is refused.
void testConnectRefused()
{
    uint16_t port = 0;
    socket_t dead = makeListener(port);
    closeHostSocket(dead);

    bool inFlight = false;
    socket_t s = startConnect(port, inFlight);

    // Loopback resolves this immediately, but allow real time so the test
    // cannot flake on a loaded machine.
    const WaitResult wr = waitSocket(s, SocketWait::Write, 2000);
    assert(wr == WaitResult::Ready);      // NOT Timeout: refusal is an event

    const int err = connectResult(s);
    assert(err != 0);                     // and it is reported as an error
    std::printf("  refused connect surfaced as: %s\n",
                socketErrorText(err).c_str());

    closeHostSocket(s);
    std::puts("OK connect_refused_is_reported");
}

// ─── 3: a successful connect resolves to 0 ────────────────────────────
void testConnectSucceeds()
{
    uint16_t port = 0;
    socket_t listener = makeListener(port);

    bool inFlight = false;
    socket_t s = startConnect(port, inFlight);

    assert(waitSocket(s, SocketWait::Write, 2000) == WaitResult::Ready);
    assert(connectResult(s) == 0);

    // The listener now has a pending client, so the accept idiom must
    // report Accepted rather than Retry.
    sockaddr_in peer{};
    socket_t client = kInvalidSocket;
    const auto pa = pollAcceptOnce(listener, 2000, client, peer);
    assert(pa == PollAccept::Accepted);
    assert(isValidSocket(client));
    std::printf("  accepted from: %s\n", peerAddressText(peer).c_str());

    // Round-trip a byte so the pair is proven usable, not merely open.
    const char out = 'A';
    assert(sendSocket(s, &out, 1) == 1);
    char in = 0;
    assert(recvSocket(client, &in, 1) == 1);
    assert(in == 'A');

    closeHostSocket(client);
    closeHostSocket(s);
    closeHostSocket(listener);
    std::puts("OK connect_succeeds_and_round_trips");
}

// ─── 4: an idle listener retries rather than blocking forever ─────────
//
// Both TCP workers loop on Retry so their stop flag is re-checked every
// timeout. Retry (not Shutdown) is what keeps them serving; Shutdown on
// an idle listener would make them exit on the first quiet moment.
void testIdleListenerRetries()
{
    uint16_t port = 0;
    socket_t listener = makeListener(port);

    sockaddr_in peer{};
    socket_t client = kInvalidSocket;
    const auto pa = pollAcceptOnce(listener, 50, client, peer);
    assert(pa == PollAccept::Retry);
    assert(!isValidSocket(client));

    // A closed listener, by contrast, must report Shutdown so the worker
    // leaves its loop instead of spinning on a dead handle.
    closeHostSocket(listener);
    const auto pa2 = pollAcceptOnce(listener, 50, client, peer);
    assert(pa2 == PollAccept::Shutdown);

    std::puts("OK idle_listener_retries");
}

// ─── 5: error classification ──────────────────────────────────────────
//
// A non-blocking recv on a socket with nothing to read must classify as
// "would block", not as a fatal error — every read loop in the three
// networking TUs branches on exactly that.
void testWouldBlockClassification()
{
    uint16_t port = 0;
    socket_t listener = makeListener(port);
    bool inFlight = false;
    socket_t s = startConnect(port, inFlight);
    assert(waitSocket(s, SocketWait::Write, 2000) == WaitResult::Ready);
    assert(connectResult(s) == 0);

    sockaddr_in peer{};
    socket_t client = kInvalidSocket;
    assert(pollAcceptOnce(listener, 2000, client, peer) == PollAccept::Accepted);
    assert(setNonBlocking(client));

    char buf[8];
    const iolen_t got = recvSocket(client, buf, sizeof(buf));
    assert(got < 0);
    const int e = lastSocketError();
    assert(errWouldBlock(e));
    assert(!errInterrupted(e));

    closeHostSocket(client);
    closeHostSocket(s);
    closeHostSocket(listener);
    std::puts("OK would_block_classification");
}

// ─── 6: a live listener cannot be hijacked ────────────────────────────
//
// AiControlServer and SuperSerialCard both bind a loopback port that a
// local agent (or the user's telnet client) connects to. The bind policy
// has to allow a restart of THIS process to re-take its own port while
// refusing to let a second live socket steal it. On POSIX both halves
// come from SO_REUSEADDR; on Winsock that option grants exactly the
// hijack, so the platform split lives in setListenerBindPolicy().
void testListenerBindPolicy()
{
    // The option the policy actually applies, per platform.
    socket_t probe = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(isValidSocket(probe));
    assert(setListenerBindPolicy(probe));

    int value = 0;
    socklen_c len = sizeof(value);
#ifdef _WIN32
    assert(::getsockopt(probe, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                        reinterpret_cast<char*>(&value), &len) == 0);
    assert(value != 0);
    // And emphatically NOT SO_REUSEADDR, which is the hijack switch here.
    value = 0; len = sizeof(value);
    assert(::getsockopt(probe, SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<char*>(&value), &len) == 0);
    assert(value == 0);
#else
    assert(::getsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &value, &len) == 0);
    assert(value != 0);          // the TIME_WAIT relaxation we do want
#endif
    closeHostSocket(probe);

    // The behaviour that must hold on both families: while one socket is
    // listening on 127.0.0.1:<port>, a second one cannot take the address
    // even though it asks for the same policy.
    uint16_t port = 0;
    socket_t listener = makeListener(port);

    socket_t thief = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(isValidSocket(thief));
    setListenerBindPolicy(thief);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
    const int r = ::bind(thief, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(r != 0);
    std::printf("  second bind to 127.0.0.1:%u refused: %s\n",
                static_cast<unsigned>(port), lastSocketErrorText().c_str());

    closeHostSocket(thief);
    closeHostSocket(listener);
    std::puts("OK listener_bind_policy_refuses_hijack");
}

}  // namespace

int main()
{
    std::puts("=== host socket compatibility test ===");
    ensureSocketStack();
    testHandleValidity();
    testConnectRefused();
    testConnectSucceeds();
    testIdleListenerRetries();
    testWouldBlockClassification();
    testListenerBindPolicy();
    std::puts("All socket compatibility tests passed.");
    return 0;
}

#endif // POM2_HAS_SOCKETS
