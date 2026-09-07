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

// TnfsClient — a TNFS (Trivial Network File System) client.
//
// This is the transport under POM2's BUILT-IN FujiNet: the native
// `FujiNetDevice` mounts a TNFS server with it and serves the images it finds
// to the guest as SmartPort block devices, with no external firmware and no
// SP-over-SLIP relay in the path. The relay stays for a real USB board or a
// full external firmware — see TODO § [Network].
//
// ── Protocol ──────────────────────────────────────────────────────────────
//
// Reference: the TNFS specification and the FujiNet firmware's own client,
// `lib/TNFSlib/tnfslib.{h,cpp}` (GPLv3, consulted not copied). Every packet is
//
//     session_id_lo | session_id_hi | sequence | command | payload...
//
// 4 header bytes, payload at most 528, so 532 on the wire. A response carries
// the same header back with a status byte first in the payload: 0 = success,
// anything else is a TNFS error code. The session id comes from MOUNT and is
// echoed on every later request; the sequence number increments per request
// and lets a late UDP reply be recognised as stale.
//
// ── Two transports, and why TCP is tried first ────────────────────────────
//
// TNFS runs over UDP (port 16384) or TCP on the same port. The firmware tries
// TCP and falls back to UDP, and so does this client — deliberately, because
// the fallback is not academic: on a host with a per-application outbound
// firewall, UDP is routinely blocked while TCP is allowed, and TNFS then works
// only over TCP (measured 2026-08-21; see DEV.md § FujiNet).
//
// **TCP framing is the protocol's weak spot**: there is no length prefix. The
// reference client just reads a bufferful and trusts that one response arrives
// per request, which holds only because the protocol is strictly
// request/response with no pipelining. This client relies on the same
// invariant but does NOT assume one read returns a whole packet: it keeps
// reading until it has the header plus, for a READ response, the byte count
// that response declares. A stream that splits a packet across segments is
// legal TCP and would desynchronise a naive reader on the very first large
// block.

#ifndef POM2_TNFS_CLIENT_H
#define POM2_TNFS_CLIENT_H

#include "SocketCompat.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

/// TNFS wire commands actually used here. The full set is in the spec; these
/// are what mounting a server and reading a disk image needs.
enum : uint8_t {
    kTnfsMount    = 0x00,
    kTnfsUnmount  = 0x01,
    kTnfsOpenDir  = 0x10,
    kTnfsReadDir  = 0x11,
    kTnfsCloseDir = 0x12,
    kTnfsRead     = 0x21,
    kTnfsClose    = 0x23,
    kTnfsStat     = 0x24,
    kTnfsLseek    = 0x25,
    kTnfsOpen     = 0x29,
};

class TnfsClient {
public:
    static constexpr uint16_t kDefaultPort   = 16384;
    static constexpr int      kHeaderSize    = 4;
    static constexpr int      kMaxPayload    = 528;
    /// A READ response spends 1 byte on the status and 2 on the count, so the
    /// most data one round trip can carry is 525.
    static constexpr int      kMaxReadChunk  = kMaxPayload - 3;

    // ── Bounds on what a REMOTE server can make this client do ────────────
    //
    // Every loop below is driven by replies from a server POM2 does not own
    // (tnfs.fujinet.online and friends), so each one carries a hard cap. They
    // are not tuning knobs: without them a hostile or merely broken server
    // hangs the caller, floods the network, or grows the heap without bound.

    /// Out-of-sequence datagrams tolerated per attempt before re-sending.
    static constexpr int      kMaxStragglers = 8;
    /// Directory entries accepted before a listing is called truncated. No
    /// real folder of disk images approaches this.
    static constexpr std::size_t kMaxDirEntries = 4096;

    ~TnfsClient();

    /// Mount `path` (usually "/") on `host`. Tries TCP, then UDP. On failure
    /// `errOut` says which stage failed and why.
    bool mount(const std::string& host, uint16_t port, const std::string& path,
               std::string& errOut);
    void unmount();
    bool isMounted() const { return mounted_; }

    /// True when the session ended up on TCP rather than UDP. Diagnostic: it
    /// is the single most useful thing to show a user whose UDP is filtered.
    bool usingTcp() const { return tcp_; }

    struct DirEntry {
        std::string name;
        bool        isDir = false;
    };
    bool listDir(const std::string& path, std::vector<DirEntry>& out,
                 std::string& errOut);

    /// Open a file read-only. Returns the TNFS file handle, or -1.
    int  openFile(const std::string& path, std::string& errOut);
    /// Total size of an open handle, via STAT on the path used to open it.
    bool fileSize(const std::string& path, uint32_t& out, std::string& errOut);
    /// Read exactly `n` bytes at `offset`, looping over the 525-byte cap.
    bool readAt(int handle, uint32_t offset, uint8_t* dst, uint32_t n,
                std::string& errOut);
    void closeFile(int handle);

    /// Per-request timeout and, on UDP, how many times a request is repeated
    /// before giving up. Ignored on TCP, where a lost packet is the stack's
    /// problem rather than ours.
    void setTimeoutMs(int ms)   { timeoutMs_ = (ms > 0) ? ms : 1; }
    void setMaxRetries(int n)   { retries_   = (n  > 0) ? n  : 1; }

    /// Cooperative cancellation, polled between a UDP retry and between the
    /// TCP/UDP passes of mount(). WHY it lives here and not only in the
    /// caller: `mount()` on an unreachable host spends TCP-connect + retries ×
    /// per-request timeout entirely INSIDE this class, and the positional
    /// `tnfs://` fetch runs before the window exists — so a Ctrl+C that only
    /// the caller can see is a Ctrl+C nobody answers for ~30 s. The pointer is
    /// borrowed and must outlive the client.
    void setAbortFlag(const std::atomic<bool>* flag) { abort_ = flag; }
    bool aborted() const { return abort_ && abort_->load(); }

private:
    /// One request/response round trip. `payload` is the request payload;
    /// `reply` receives the response payload INCLUDING its leading status
    /// byte. Returns false when nothing came back at all; a TNFS-level error
    /// is a successful round trip with a non-zero status.
    bool transact(uint8_t command, const uint8_t* payload, std::size_t n,
                  std::vector<uint8_t>& reply, std::string& errOut);
    bool sendRecvTcp(const uint8_t* pkt, std::size_t n,
                     std::vector<uint8_t>& reply, std::string& errOut);
    bool sendRecvUdp(const uint8_t* pkt, std::size_t n,
                     std::vector<uint8_t>& reply, std::string& errOut);
    bool openTransport(const std::string& host, uint16_t port, bool wantTcp,
                       std::string& errOut);
    void closeTransport();

    socket_t  fd_       = kInvalidSocket;
    bool      tcp_      = false;
    bool      mounted_  = false;
    uint16_t  session_  = 0;
    /// Session id seen in the LAST response header. MOUNT is where it is
    /// assigned; every other command just echoes ours back.
    uint16_t  replySession_ = 0;
    uint8_t   sequence_ = 0;
    int       timeoutMs_ = 5000;
    int       retries_   = 5;
    std::string host_;
    uint16_t    port_ = kDefaultPort;
    /// Kept so a UDP session can re-address the server on every send without
    /// resolving the name again.
    std::vector<uint8_t> peerAddr_;
    /// Borrowed cancellation flag; see setAbortFlag().
    const std::atomic<bool>* abort_ = nullptr;
};

}  // namespace pom2

#endif  // POM2_TNFS_CLIENT_H
