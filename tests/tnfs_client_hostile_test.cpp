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

// TnfsClient against a HOSTILE stub server, in-process.
//
// The sibling tnfs_client_test.cpp checks that the client works against a
// server that behaves. This one checks what happens when it does not — which
// is the case that actually matters, because TnfsClient's whole job is to
// talk to a public server on the internet (tnfs.fujinet.online and friends)
// that POM2 neither owns nor audits. Every loop and every buffer here is
// driven by bytes that server chose.
//
// Each case below was a live defect, found 2026-08-21 and reproduced against
// this stub before being fixed:
//
//   * READ declaring more bytes than were asked for. `memcpy(dst + done,
//     ..., got)` used to trust the server's count: a 525-byte answer to an
//     8-byte request wrote 517 attacker-chosen bytes past the caller's heap
//     block (confirmed under ASan).
//   * A UDP reply with the wrong sequence byte. The straggler drop did
//     `--attempt; continue;`, which `++attempt` undid — an endless loop that
//     re-sent on every pass: 446 000 requests in 6 s against a 0.6 s budget.
//   * A short STAT reply. The guard asked for 9 bytes to protect a field that
//     ends at byte 11, so a 9-byte reply read past the vector and returned a
//     size built from adjacent heap.
//   * A directory that never ends, and a directory listing cut short by the
//     transport being reported as complete.
//   * A reply answering a different command than the one sent, which over TCP
//     desynchronises the stream for good.

#include "TnfsClient.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;
void check(bool cond, const char* what)
{
    if (cond) std::printf("[ OK ] %s\n", what);
    else    { std::printf("FAIL: %s\n", what); ++failures; }
}

constexpr uint16_t kSession = 0xBEEF;
constexpr uint8_t  kHnd     = 0x07;

/// What flavour of lie this run tells.
enum class Evil {
    OversizedRead,     ///< answers every READ with the maximum payload
    ShortStat,         ///< STAT reply two bytes shorter than the size field
    EndlessDir,        ///< READDIR never says "end of directory"
    DirCutShort,       ///< closes the connection mid-listing
    WrongCommand,      ///< answers with a command byte we never sent
    WrongSequence,     ///< answers with a sequence byte we never sent
    DirError,          ///< an I/O error after one valid directory entry
    DirEmptyReply,     ///< a READDIR response with no status byte (UDP)
    DirMissingNul,     ///< a successful READDIR name without its terminator (UDP)
};

std::vector<uint8_t> reply(const uint8_t* req, const std::vector<uint8_t>& payload,
                           bool wrongCommand = false, bool wrongSequence = false)
{
    std::vector<uint8_t> r;
    r.push_back(static_cast<uint8_t>(kSession & 0xFF));
    r.push_back(static_cast<uint8_t>(kSession >> 8));
    r.push_back(wrongSequence ? static_cast<uint8_t>(req[2] ^ 0x5A) : req[2]);
    r.push_back(wrongCommand  ? static_cast<uint8_t>(0xFE)          : req[3]);
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
}

struct Hostile {
    Evil              evil;
    std::atomic<bool> stop{false};
    std::atomic<int>  requests{0};
    std::atomic<int>  readDirs{0};

    /// Returns the payload, or empty to send nothing.
    std::vector<uint8_t> handle(const uint8_t* req, std::size_t n)
    {
        requests.fetch_add(1);
        const uint8_t cmd = req[3];
        (void)n;
        switch (cmd) {
        case pom2::kTnfsMount:   return {0x00, 0x00, 0x01, 0x00, 0x00};
        case pom2::kTnfsUnmount: return {0x00};
        case pom2::kTnfsOpen:    return {0x00, kHnd};
        case pom2::kTnfsClose:   return {0x00};
        case pom2::kTnfsLseek:   return {0x00};
        case pom2::kTnfsOpenDir: return {0x00, 0x03};
        case pom2::kTnfsCloseDir:return {0x00};

        case pom2::kTnfsReadDir: {
            const int index = readDirs.fetch_add(1);
            if (evil == Evil::DirError && index > 0) return {0x03};
            if (evil == Evil::DirEmptyReply) return {};
            if (evil == Evil::DirMissingNul) return {0x00, 'B', 'A', 'D'};
            // A name, every time, for ever.
            std::vector<uint8_t> p{0x00};
            const char* nm = "AAAAAAA";
            p.insert(p.end(), nm, nm + std::strlen(nm));
            p.push_back(0x00);
            return p;
        }

        case pom2::kTnfsStat: {
            // status + mode(2) + uid(2) + gid(2) = 7 bytes, then only TWO of
            // the four size bytes: exactly the length the old guard let past.
            return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF};
        }

        case pom2::kTnfsRead: {
            // The lie: the maximum a reply can carry, whatever was requested.
            const std::size_t got = pom2::TnfsClient::kMaxReadChunk;
            std::vector<uint8_t> p{0x00};
            p.push_back(static_cast<uint8_t>(got & 0xFF));
            p.push_back(static_cast<uint8_t>(got >> 8));
            p.insert(p.end(), got, 0x41);      // 'A' — visible in a canary
            return p;
        }
        default: return {0x05};
        }
    }
};

struct TcpStub {
    Hostile           s;
    int               listenFd = -1;
    /// The ACCEPTED socket, published so the shutdown can wake the thread:
    /// it parks in recv() there, and closing the LISTENING socket does not
    /// disturb that at all — join() would wait for ever.
    std::atomic<int>  clientFd{-1};
    uint16_t          port     = 0;
    std::thread       th;

    explicit TcpStub(Evil e) { s.evil = e; }

    bool start()
    {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) return false;
        int one = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        socklen_t al = sizeof a;
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&a), &al) != 0) return false;
        port = ntohs(a.sin_port);
        if (::listen(listenFd, 2) != 0) return false;

        th = std::thread([this] {
            const int c = ::accept(listenFd, nullptr, nullptr);
            if (c < 0) return;
            clientFd.store(c);
            uint8_t buf[2048];
            while (!s.stop.load()) {
                const ssize_t r = ::recv(c, buf, sizeof buf, 0);
                if (r <= 4) break;
                // Hang up mid-listing: the transport dies, not the directory.
                if (s.evil == Evil::DirCutShort && buf[3] == pom2::kTnfsReadDir &&
                    s.readDirs.load() >= 1) {
                    break;
                }
                const auto payload = s.handle(buf, static_cast<std::size_t>(r));
                if (payload.empty()) continue;
                // MOUNT must succeed even in the WrongCommand run, or nothing
                // downstream is reachable to test.
                const bool lie = (s.evil == Evil::WrongCommand &&
                                  buf[3] != pom2::kTnfsMount);
                const bool wrongSeq = (s.evil == Evil::WrongSequence &&
                                       buf[3] != pom2::kTnfsMount);
                const auto pkt = reply(buf, payload, lie, wrongSeq);
                ::send(c, pkt.data(), pkt.size(), 0);
            }
            clientFd.store(-1);
            ::close(c);
        });
        return true;
    }

    void shutdownStub()
    {
        s.stop.store(true);
        const int c = clientFd.exchange(-1);
        if (c >= 0) ::shutdown(c, SHUT_RDWR);   // wakes the parked recv()
        if (listenFd >= 0) { ::shutdown(listenFd, SHUT_RDWR); ::close(listenFd); listenFd = -1; }
        if (th.joinable()) th.join();
    }
};

/// UDP-only, so the client falls back to it: nothing listens on TCP.
struct UdpStub {
    Hostile     s;
    int         fd   = -1;
    uint16_t    port = 0;
    std::thread th;

    explicit UdpStub(Evil e) { s.evil = e; }

    bool start()
    {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        socklen_t al = sizeof a;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &al) != 0) return false;
        port = ntohs(a.sin_port);

        th = std::thread([this] {
            uint8_t buf[2048];
            while (!s.stop.load()) {
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                const ssize_t r = ::recvfrom(fd, buf, sizeof buf, 0,
                                             reinterpret_cast<sockaddr*>(&from), &fl);
                if (r <= 4) break;
                const auto payload = s.handle(buf, static_cast<std::size_t>(r));
                if (payload.empty() && s.evil != Evil::DirEmptyReply) continue;
                // MOUNT answers straight, so the session is established; from
                // then on every sequence byte is wrong.
                const bool wrongSeq = (s.evil == Evil::WrongSequence &&
                                       buf[3] != pom2::kTnfsMount);
                const auto pkt = reply(buf, payload, false, wrongSeq);
                ::sendto(fd, pkt.data(), pkt.size(), 0,
                         reinterpret_cast<sockaddr*>(&from), fl);
            }
        });
        return true;
    }

    void shutdownStub()
    {
        s.stop.store(true);
        if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); fd = -1; }
        if (th.joinable()) th.join();
    }
};

}  // namespace

int main()
{
    for (const Evil evil : {Evil::DirError, Evil::WrongSequence}) {
        TcpStub stub(evil);
        if (!stub.start()) return 1;
        pom2::TnfsClient c;
        c.setTimeoutMs(200);
        std::string err;
        check(c.mount("127.0.0.1", stub.port, "/", err), "mounts TCP regression stub");
        if (evil == Evil::WrongSequence) {
            check(c.openFile("/GAME.PO", err) < 0,
                  "rejects a TCP OPEN reply with the wrong sequence");
            check(!err.empty(), "reports the sequence failure reason");
            c.unmount();
            stub.shutdownStub();
            continue;
        }
        std::vector<pom2::TnfsClient::DirEntry> out;
        check(!c.listDir("/", out, err), "READDIR I/O error fails the listing");
        check(!err.empty(), "reports the listing failure reason");
        check(out.size() == 1, "I/O error occurs after one valid entry");
        c.unmount();
        stub.shutdownStub();
    }
    for (const Evil evil : {Evil::DirEmptyReply, Evil::DirMissingNul}) {
        UdpStub stub(evil);
        if (!stub.start()) return 1;
        pom2::TnfsClient c;
        c.setTimeoutMs(200);
        std::string err;
        check(c.mount("127.0.0.1", stub.port, "/", err), "mounts UDP regression stub");
        std::vector<pom2::TnfsClient::DirEntry> out;
        check(!c.listDir("/", out, err), "malformed READDIR fails the listing");
        check(out.empty(), "malformed directory names are not returned");
        check(!err.empty(), "reports the malformed reply reason");
        c.unmount();
        stub.shutdownStub();
    }
    // ── A READ may not deliver more than was asked for ────────────────────
    {
        TcpStub stub(Evil::OversizedRead);
        if (!stub.start()) { std::printf("FAIL: stub\n"); return 1; }

        pom2::TnfsClient c;
        std::string err;
        check(c.mount("127.0.0.1", stub.port, "/", err), "mounts the hostile server");

        const int h = c.openFile("/GAME.PO", err);
        check(h >= 0, "opens a file on it");

        // 8 bytes wanted, inside a block whose tail is a canary. The server
        // will answer 525.
        std::vector<uint8_t> buf(64, 0xCD);
        const bool ok = c.readAt(h, 0, buf.data(), 8, err);
        check(!ok, "refuses a READ declaring more bytes than were requested");

        bool canaryIntact = true;
        for (std::size_t i = 8; i < buf.size(); ++i)
            if (buf[i] != 0xCD) { canaryIntact = false; break; }
        check(canaryIntact, "and writes nothing past the caller's buffer");
        c.closeFile(h);
        stub.shutdownStub();
    }

    // ── A STAT reply too short to hold a size is refused ──────────────────
    {
        TcpStub stub(Evil::ShortStat);
        if (!stub.start()) { std::printf("FAIL: stub\n"); return 1; }

        pom2::TnfsClient c;
        std::string err;
        check(c.mount("127.0.0.1", stub.port, "/", err), "mounts");
        uint32_t size = 0xDEADBEEF;
        check(!c.fileSize("/GAME.PO", size, err),
              "refuses a STAT reply too short to hold the size field");
        stub.shutdownStub();
    }

    // ── A directory that never ends must still end ────────────────────────
    {
        TcpStub stub(Evil::EndlessDir);
        if (!stub.start()) { std::printf("FAIL: stub\n"); return 1; }

        pom2::TnfsClient c;
        std::string err;
        check(c.mount("127.0.0.1", stub.port, "/", err), "mounts");

        std::vector<pom2::TnfsClient::DirEntry> out;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = c.listDir("/", out, err);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();

        check(!ok, "reports an endless directory as a failure, not a listing");
        check(out.size() <= pom2::TnfsClient::kMaxDirEntries,
              "stops at the entry cap instead of growing without bound");
        check(ms < 20000, "and gets there in bounded time");
        stub.shutdownStub();
    }

    // ── A listing cut short by the transport is not a complete listing ────
    {
        TcpStub stub(Evil::DirCutShort);
        if (!stub.start()) { std::printf("FAIL: stub\n"); return 1; }

        pom2::TnfsClient c;
        std::string err;
        check(c.mount("127.0.0.1", stub.port, "/", err), "mounts");
        std::vector<pom2::TnfsClient::DirEntry> out;
        check(!c.listDir("/", out, err),
              "a connection dropped mid-listing fails instead of returning a short one");
        stub.shutdownStub();
    }

    // ── A reply must answer the command that was sent ─────────────────────
    {
        TcpStub stub(Evil::WrongCommand);
        if (!stub.start()) { std::printf("FAIL: stub\n"); return 1; }

        pom2::TnfsClient c;
        std::string err;
        check(c.mount("127.0.0.1", stub.port, "/", err), "mounts");
        std::vector<pom2::TnfsClient::DirEntry> out;
        check(!c.listDir("/", out, err),
              "rejects a reply answering a command that was never sent");
        stub.shutdownStub();
    }

    // ── Endless wrong-sequence replies must not become a packet storm ─────
    {
        UdpStub stub(Evil::WrongSequence);
        if (!stub.start()) { std::printf("FAIL: stub\n"); return 1; }

        pom2::TnfsClient c;
        c.setTimeoutMs(200);
        c.setMaxRetries(2);
        std::string err;
        check(c.mount("127.0.0.1", stub.port, "/", err), "mounts over UDP");

        const int before = stub.s.requests.load();
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<pom2::TnfsClient::DirEntry> out;
        const bool ok = c.listDir("/", out, err);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        const int sent = stub.s.requests.load() - before;

        check(!ok, "gives up when every reply carries the wrong sequence");
        check(ms < 15000, "within its retry budget rather than for ever");
        // The old code re-sent on every straggler: 446 000 requests in 6 s.
        std::printf("       (%d requests in %lld ms)\n", sent, (long long)ms);
        check(sent < 200, "sends a bounded number of requests, not a storm");
        stub.shutdownStub();
    }

    if (failures) { std::printf("tnfs_client_hostile: %d failure(s)\n", failures); return 2; }
    std::printf("tnfs_client_hostile OK\n");
    return 0;
}
