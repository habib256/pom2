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

// TnfsClient against a stub TNFS server, in-process.
//
// No network dependency: the stub binds 127.0.0.1 on an ephemeral port and
// speaks just enough of the protocol (MOUNT, OPENDIR/READDIR/CLOSEDIR, OPEN,
// STAT, LSEEK, READ, CLOSE) for a real mount-and-read. That keeps this
// runnable in CI, and it lets the test do the one thing a live server never
// does on demand: split a response across TCP segments.
//
// What it pins:
//   * MOUNT adopts the session id the server assigns IN THE RESPONSE HEADER.
//     Getting this wrong is invisible against a permissive server and fatal
//     against a strict one, because every later request echoes it back.
//   * a read larger than the 525-byte payload cap is reassembled from several
//     round trips;
//   * a READ response deliberately split across two TCP segments is still
//     read whole. The reference client reads one bufferful and trusts the
//     packet arrived intact; that assumption is what this guards against.
//   * TCP is preferred, and a server that refuses TCP is reached over UDP.
//     That fallback is not academic — a host firewall that drops outbound UDP
//     while allowing TCP is exactly why the preference order exists.

#include "TnfsClient.h"
#include "TnfsMedia.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include <atomic>
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

namespace fs = std::filesystem;

constexpr uint16_t kSession   = 0xBEEF;
constexpr uint8_t  kFileHnd   = 0x07;
constexpr uint8_t  kDirHnd    = 0x03;
const     char*    kFileName  = "GAME.PO";

std::vector<uint8_t> makeContent(std::size_t n)
{
    std::vector<uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<uint8_t>((i * 7 + (i >> 8)) & 0xFF);
    return v;
}

/// Builds a response packet: the client's session/sequence echoed back, then
/// the command, then the payload the caller supplies.
std::vector<uint8_t> reply(const uint8_t* req, uint16_t session,
                           const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> r;
    r.push_back(static_cast<uint8_t>(session & 0xFF));
    r.push_back(static_cast<uint8_t>(session >> 8));
    r.push_back(req[2]);                    // sequence
    r.push_back(req[3]);                    // command
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
}

/// Does a NUL-terminated request path name the one file this stub serves?
/// Leading '/' ignored; the directory the listing advertises is accepted too.
bool pathIsOurs(const uint8_t* p, std::size_t n)
{
    std::string s;
    for (std::size_t i = 0; i < n && p[i]; ++i) s.push_back(static_cast<char>(p[i]));
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    // The two extra names exist for the cache-collision case below: they
    // FOLD to the same flat filename ('/' → '_'), which is exactly what the
    // old cache key could not tell apart.
    return s == kFileName || s == "SUB/GAME.PO" || s == "SUB_GAME.PO" ||
           s == "SUBDIR" || s.empty();
}

struct Stub {
    std::vector<uint8_t> content = makeContent(1500);
    std::atomic<bool>    stop{false};
    std::atomic<bool>    splitNextRead{false};   ///< TCP: send in two segments
    std::atomic<bool>    splitNextStat{false};   ///< TCP: split a STAT reply
    uint32_t             filePos = 0;
    int                  dirIndex = 0;

    /// Answer one request. Returns the response payload (status byte first),
    /// or an empty vector to say "send nothing".
    std::vector<uint8_t> handle(const uint8_t* req, std::size_t n)
    {
        const uint8_t cmd = req[3];
        const uint8_t* body = req + 4;
        const std::size_t bodyLen = n - 4;
        switch (cmd) {
        case pom2::kTnfsMount:
            return {0x00, 0x00, 0x01, 0x00, 0x00};      // ok + version + retry
        case pom2::kTnfsUnmount:
            return {0x00};
        case pom2::kTnfsOpenDir:
            dirIndex = 0;
            return {0x00, kDirHnd};
        case pom2::kTnfsReadDir: {
            static const char* names[] = {"SUBDIR/", kFileName};
            if (dirIndex >= 2) return {0x21};           // END_OF_FILE = end of dir
            const char* nm = names[dirIndex++];
            std::vector<uint8_t> p{0x00};
            p.insert(p.end(), nm, nm + std::strlen(nm));
            p.push_back(0x00);
            return p;
        }
        case pom2::kTnfsCloseDir:
            return {0x00};
        // OPEN and STAT name a PATH, and a real server answers ENOENT for
        // one it does not have. The stub used to answer for anything, which
        // made "a missing file must fail the fetch rather than produce an
        // empty disk" untestable.
        case pom2::kTnfsOpen:
            if (!pathIsOurs(body + 4, bodyLen > 4 ? bodyLen - 4 : 0))
                return {0x02};                       // TNFS_ENOENT
            filePos = 0;
            return {0x00, kFileHnd};
        case pom2::kTnfsStat: {
            if (!pathIsOurs(body, bodyLen)) return {0x02};
            std::vector<uint8_t> p{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            const uint32_t sz = static_cast<uint32_t>(content.size());
            p.push_back(static_cast<uint8_t>(sz & 0xFF));
            p.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
            p.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
            p.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
            return p;
        }
        case pom2::kTnfsLseek: {
            if (bodyLen < 6) return {0x05};
            filePos = static_cast<uint32_t>(body[2]) |
                      (static_cast<uint32_t>(body[3]) << 8) |
                      (static_cast<uint32_t>(body[4]) << 16) |
                      (static_cast<uint32_t>(body[5]) << 24);
            return {0x00};
        }
        case pom2::kTnfsRead: {
            if (bodyLen < 3) return {0x05};
            uint16_t want = static_cast<uint16_t>(body[1] | (body[2] << 8));
            if (filePos >= content.size()) return {0x02};
            const uint32_t avail =
                static_cast<uint32_t>(content.size()) - filePos;
            if (want > avail) want = static_cast<uint16_t>(avail);
            std::vector<uint8_t> p{0x00};
            p.push_back(static_cast<uint8_t>(want & 0xFF));
            p.push_back(static_cast<uint8_t>(want >> 8));
            p.insert(p.end(), content.begin() + filePos,
                     content.begin() + filePos + want);
            filePos += want;
            return p;
        }
        case pom2::kTnfsClose:
            return {0x00};
        default:
            return {0x05};                              // EIO
        }
    }
};

/// TCP stub: accepts one client, then serves requests until told to stop.
struct TcpStub {
    Stub        s;
    int         listenFd = -1;
    uint16_t    port     = 0;
    std::thread th;

    bool start()
    {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) return false;
        int one = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        socklen_t al = sizeof a;
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&a), &al) != 0) return false;
        port = ntohs(a.sin_port);
        if (::listen(listenFd, 2) != 0) return false;

        th = std::thread([this] {
            const int c = ::accept(listenFd, nullptr, nullptr);
            if (c < 0) return;
            uint8_t buf[1024];
            while (!s.stop.load()) {
                const ssize_t r = ::recv(c, buf, sizeof buf, 0);
                if (r <= 4) break;
                const auto payload = s.handle(buf, static_cast<std::size_t>(r));
                if (payload.empty()) continue;
                const auto pkt = reply(buf, kSession, payload);
                std::size_t cut = 0;
                if (s.splitNextRead.load() && buf[3] == pom2::kTnfsRead) {
                    s.splitNextRead.store(false);
                    cut = 9;
                } else if (s.splitNextStat.load() &&
                           buf[3] == pom2::kTnfsStat) {
                    // Split INSIDE the fixed STAT payload (header + status
                    // + 1 byte): only READ used to be reassembled, so this
                    // runt failed fileSize AND stranded its 9-byte tail in
                    // the socket buffer — session desync.
                    s.splitNextStat.store(false);
                    cut = 6;
                }
                if (cut) {
                    // Two segments, with a pause between: a legal TCP split
                    // that a one-recv reader would mistake for a whole packet.
                    ::send(c, pkt.data(), cut, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    ::send(c, pkt.data() + cut, pkt.size() - cut, 0);
                } else {
                    ::send(c, pkt.data(), pkt.size(), 0);
                }
            }
            ::close(c);
        });
        return true;
    }

    void shutdownStub()
    {
        s.stop.store(true);
        if (listenFd >= 0) { ::shutdown(listenFd, SHUT_RDWR); ::close(listenFd); }
        if (th.joinable()) th.join();
    }
};

/// UDP-only stub: nothing listens on TCP, so the client must fall back.
struct UdpStub {
    Stub        s;
    int         fd   = -1;
    uint16_t    port = 0;
    std::thread th;

    bool start()
    {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        socklen_t al = sizeof a;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &al) != 0) return false;
        port = ntohs(a.sin_port);

        th = std::thread([this] {
            uint8_t buf[1024];
            while (!s.stop.load()) {
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                const ssize_t r = ::recvfrom(fd, buf, sizeof buf, 0,
                                             reinterpret_cast<sockaddr*>(&from), &fl);
                if (r <= 4) break;
                const auto payload = s.handle(buf, static_cast<std::size_t>(r));
                if (payload.empty()) continue;
                const auto pkt = reply(buf, kSession, payload);
                ::sendto(fd, pkt.data(), pkt.size(), 0,
                         reinterpret_cast<sockaddr*>(&from), fl);
            }
        });
        return true;
    }

    void shutdownStub()
    {
        s.stop.store(true);
        if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); }
        if (th.joinable()) th.join();
    }
};

int failures = 0;
void check(bool cond, const char* what)
{
    if (cond) { std::printf("[ OK ] %s\n", what); }
    else      { std::printf("FAIL: %s\n", what); ++failures; }
}

bool exercise(pom2::TnfsClient& c, const std::vector<uint8_t>& expect,
              const char* label)
{
    std::string err;
    std::vector<pom2::TnfsClient::DirEntry> entries;
    if (!c.listDir("/", entries, err)) {
        std::printf("FAIL: %s listDir: %s\n", label, err.c_str());
        ++failures;
        return false;
    }
    check(entries.size() == 2, "listDir returns both entries");
    if (entries.size() == 2) {
        check(entries[0].isDir && entries[0].name == "SUBDIR",
              "trailing '/' marks a directory and is stripped");
        check(!entries[1].isDir && entries[1].name == kFileName,
              "plain file is not marked as a directory");
    }

    uint32_t size = 0;
    check(c.fileSize(std::string("/") + kFileName, size, err) &&
          size == expect.size(), "STAT reports the file size");

    const int h = c.openFile(std::string("/") + kFileName, err);
    check(h >= 0, "OPEN returns a handle");
    if (h < 0) { std::printf("      (%s)\n", err.c_str()); return false; }

    std::vector<uint8_t> got(expect.size());
    if (!c.readAt(h, 0, got.data(), static_cast<uint32_t>(got.size()), err)) {
        std::printf("FAIL: %s readAt: %s\n", label, err.c_str());
        ++failures;
        return false;
    }
    check(got == expect, "a read spanning several 525-byte chunks round-trips");

    // Partial read from an offset: the seek must land, not just the length.
    std::vector<uint8_t> mid(300);
    if (c.readAt(h, 600, mid.data(), 300, err)) {
        check(std::equal(mid.begin(), mid.end(), expect.begin() + 600),
              "readAt honours the offset");
    } else {
        std::printf("FAIL: %s offset read: %s\n", label, err.c_str());
        ++failures;
    }
    c.closeFile(h);
    return true;
}

}  // namespace

int main()
{
    const auto expect = makeContent(1500);

    // ── TCP, including a deliberately split READ response ─────────────────
    {
        TcpStub stub;
        if (!stub.start()) { std::printf("FAIL: cannot start TCP stub\n"); return 1; }

        pom2::TnfsClient c;
        c.setTimeoutMs(3000);
        std::string err;
        if (!c.mount("127.0.0.1", stub.port, "/", err)) {
            std::printf("FAIL: TCP mount: %s\n", err.c_str());
            stub.shutdownStub();
            return 1;
        }
        check(c.usingTcp(), "TCP is preferred when the server accepts it");
        stub.s.splitNextRead.store(true);
        exercise(c, expect, "tcp");

        // A non-READ reply split across segments must be reassembled too —
        // and must not leave its tail behind (the next requests below would
        // then parse garbage headers: session desync until remount).
        stub.s.splitNextStat.store(true);
        uint32_t statSz = 0;
        check(c.fileSize(std::string("/") + kFileName, statSz, err) &&
              statSz == expect.size(), "a split STAT reply is reassembled");
        exercise(c, expect, "tcp-after-split-stat");
        c.unmount();
        stub.shutdownStub();
    }

    // ── UDP fallback: nothing is listening on TCP at that port ────────────
    {
        UdpStub stub;
        if (!stub.start()) { std::printf("FAIL: cannot start UDP stub\n"); return 1; }

        pom2::TnfsClient c;
        c.setTimeoutMs(1500);
        c.setMaxRetries(3);
        std::string err;
        if (!c.mount("127.0.0.1", stub.port, "/", err)) {
            std::printf("FAIL: UDP mount: %s\n", err.c_str());
            stub.shutdownStub();
            return 1;
        }
        check(!c.usingTcp(), "falls back to UDP when TCP is refused");
        exercise(c, expect, "udp");
        c.unmount();
        stub.shutdownStub();
    }

    // ── The URL parser, which is the wiring's untrusted-input surface ────
    // A TNFS URL reaches this from a command line or a text field, so every
    // shape it must REFUSE matters more than the ones it accepts.
    {
        std::string host, path;
        std::uint16_t port = 0;

        check(pom2::parseTnfsUrl("tnfs://example.com/disks/game.po", host, port, path) &&
              host == "example.com" && port == pom2::TnfsClient::kDefaultPort &&
              path == "/disks/game.po", "scheme + default port");
        check(pom2::parseTnfsUrl("TNFS://Example.COM:16385/x.po", host, port, path) &&
              host == "Example.COM" && port == 16385 && path == "/x.po",
              "the scheme is case-insensitive and an explicit port is kept");
        check(pom2::parseTnfsUrl("tnfs.fujinet.online/Apple/a.po", host, port, path) &&
              host == "tnfs.fujinet.online" && path == "/Apple/a.po",
              "the scheme is optional — a text field may omit it");
        check(pom2::parseTnfsUrl("h/a b/c.2mg", host, port, path) &&
              path == "/a b/c.2mg", "spaces in a path survive");

        check(!pom2::parseTnfsUrl("http://example.com/x.po", host, port, path),
              "another scheme is refused rather than half-parsed");
        check(!pom2::parseTnfsUrl("example.com", host, port, path),
              "a host with no file is refused");
        check(!pom2::parseTnfsUrl("example.com/", host, port, path),
              "a bare trailing slash names no file");
        check(!pom2::parseTnfsUrl("/local/path.po", host, port, path),
              "an absolute local path is not a TNFS URL");
        check(!pom2::parseTnfsUrl("h:0/x.po", host, port, path), "port 0");
        check(!pom2::parseTnfsUrl("h:99999/x.po", host, port, path), "port > 65535");
        check(!pom2::parseTnfsUrl("h:/x.po", host, port, path), "empty port");
        check(!pom2::parseTnfsUrl("h:12a4/x.po", host, port, path),
              "a non-numeric port is refused, not silently truncated");
        check(!pom2::parseTnfsUrl(":16384/x.po", host, port, path), "empty host");
        check(!pom2::parseTnfsUrl("", host, port, path), "empty string");
    }

    // ── Fetch a whole image, and then don't fetch it again ───────────────
    {
        TcpStub stub;
        if (!stub.start()) { std::printf("FAIL: cannot start fetch stub\n"); return 1; }

        const auto cache = fs::temp_directory_path() / "pom2_tnfs_fetch_cache";
        std::error_code ec;
        fs::remove_all(cache, ec);

        const std::string url = "tnfs://127.0.0.1:" + std::to_string(stub.port) +
                                "/" + kFileName;
        const auto got = pom2::tnfsFetchImage(url, cache.string());
        if (!got.ok) std::printf("      fetch error: %s\n", got.error.c_str());
        check(got.ok, "fetch succeeds");
        check(!got.fromCache, "the first fetch is not a cache hit");
        check(got.bytes == expect.size(), "the whole file arrives");

        // The extension has to survive into the cache name: everything
        // downstream picks the drive from it (classifyDiskForSlot).
        check(got.localPath.size() > 3 &&
              got.localPath.compare(got.localPath.size() - 3, 3, ".PO") == 0,
              "the cache file keeps the image's extension");

        std::ifstream in(got.localPath, std::ios::binary);
        const std::vector<std::uint8_t> onDisk(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(onDisk == expect, "the cached bytes are the served bytes");
        in.close();

        // Second call: same size, so the server is not asked again.
        const auto again = pom2::tnfsFetchImage(url, cache.string());
        check(again.ok && again.fromCache, "the second fetch comes from the cache");
        check(again.localPath == got.localPath, "and names the same file");

        stub.shutdownStub();
        fs::remove_all(cache, ec);
    }

    // ── Two URLs that FOLD to one filename must not share a cache file ──
    // The key hashed only above 120 characters; below that the folding
    // ('/' → '_') made `SUB/GAME.PO` and `SUB_GAME.PO` the same name, and the
    // cache check is existence-only and opens no socket — so the second URL
    // was silently served the FIRST one's bytes, offline and repeatably.
    // (Bug hunt 2026-09-06 #H5.)
    {
        TcpStub stub;
        if (!stub.start()) { std::printf("FAIL: cannot start fetch stub\n"); return 1; }
        const auto cache = fs::temp_directory_path() / "pom2_tnfs_collide_cache";
        std::error_code ec;
        fs::remove_all(cache, ec);
        const std::string base = "tnfs://127.0.0.1:" + std::to_string(stub.port);

        const auto a = pom2::tnfsFetchImage(base + "/SUB/GAME.PO", cache.string());
        if (!a.ok) std::printf("      a error: %s\n", a.error.c_str());
        check(a.ok && !a.fromCache, "the first colliding path fetches");

        // The stub serves ONE connection, so this second call cannot reach a
        // server — which is the point. Before the fix it did not need to: the
        // folded key was already on disk, the existence-only check hit it, and
        // the caller was handed the OTHER URL's bytes with no socket opened
        // at all. What must not happen is a cache HIT.
        const auto b = pom2::tnfsFetchImage(base + "/SUB_GAME.PO", cache.string());
        check(!b.fromCache,
              "a different URL must not be served from this one's cache file");
        check(b.localPath != a.localPath,
              "two distinct URLs keep two distinct cache files");

        stub.shutdownStub();
        fs::remove_all(cache, ec);
    }

    // A path that is not there must fail the fetch, not produce an empty disk.
    {
        TcpStub stub;
        if (!stub.start()) { std::printf("FAIL: cannot start fetch stub\n"); return 1; }
        const auto cache = fs::temp_directory_path() / "pom2_tnfs_fetch_cache2";
        std::error_code ec;
        fs::remove_all(cache, ec);
        const auto got = pom2::tnfsFetchImage(
            "tnfs://127.0.0.1:" + std::to_string(stub.port) + "/NOPE.PO",
            cache.string());
        check(!got.ok, "a missing file fails the fetch");
        check(got.localPath.empty(), "and leaves no half-made image behind");
        stub.shutdownStub();
        fs::remove_all(cache, ec);
    }

    if (failures) {
        std::printf("tnfs_client: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("tnfs_client OK\n");
    return 0;
}
