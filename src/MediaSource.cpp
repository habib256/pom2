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

// MediaSource — see MediaSource.h.

#include "MediaSource.h"

#include "AtomicFileReplace.h"
#include "ChildProcess.h"
#include "ResourcePaths.h"
#include "ZipMember.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace pom2 {

namespace {

// The largest image POM2 mounts: a 32 MB ProDOS volume, with room for a
// 2MG header and the cache's own name. Bigger is not a disk.
constexpr std::size_t kMaxImageBytes = 34u * 1024u * 1024u;

std::string lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& s, const char* suffix)
{
    const std::string x(suffix);
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

// FNV-1a, 32 bits, as eight hex digits: the cache key of a source.
std::string key(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    char b[9];
    std::snprintf(b, sizeof b, "%08x", h);
    return b;
}

// A file name safe on every host, from the last segment of a URL or member.
std::string safeName(std::string s)
{
    if (const auto q = s.find_first_of("?#"); q != std::string::npos) s.erase(q);
    if (const auto slash = s.find_last_of("/\\"); slash != std::string::npos) s.erase(0, slash + 1);
    std::string out;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == '%' || u < 0x20 || std::string("<>:\"|?*").find(c) != std::string::npos) out.push_back('_');
        else out.push_back(c);
    }
    return out.empty() ? std::string("image") : out;
}

bool readFile(const fs::path& p, std::vector<uint8_t>& out, std::string& err)
{
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) { err = p.string() + ": " + ec.message(); return false; }
    if (size > 4 * kMaxImageBytes) { err = p.string() + ": too large to be a disk"; return false; }
    std::ifstream f(p, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (!f && !f.eof()) { err = p.string() + ": read failed"; return false; }
    return true;
}

bool writeFile(const fs::path& p, const std::vector<uint8_t>& data, std::string& err)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = tempSiblingPath(p);
    if (!prepareTempPath(tmp, ec)) { err = tmp.string() + ": " + ec.message(); return false; }
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!f) { fs::remove(tmp, ec); err = tmp.string() + ": write failed"; return false; }
    }
    if (!replaceMediaFileAtomic(tmp, p, ec)) { fs::remove(tmp, ec); err = p.string() + ": " + ec.message(); return false; }
    return true;
}

// curl, as RomFetch runs it, but for disks: http allowed (Asimov mirrors,
// old pages), a disk-sized cap, every redirect followed within http(s).
bool download(const std::string& url, const fs::path& dest, std::string& err,
              const std::atomic<bool>* abort)
{
#if !POM2_HAS_CHILD_PROCESS
    (void)url; (void)dest; (void)abort;
    err = "downloads need a helper program, not available in this build";
    return false;
#else
    const std::string curl = ChildProcess::findOnPath("curl");
    if (curl.empty()) { err = "curl was not found on PATH — install it to open disks by URL"; return false; }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = tempSiblingPath(dest);
    ChildProcess p;
    if (!p.start(curl, { "-fsL", "--proto", "=http,https", "--proto-redir", "=http,https",
                         "--max-filesize", std::to_string(kMaxImageBytes),
                         "--retry", "2", "--max-time", "120", "-o", tmp.string(), url },
                 "", err))
        return false;
    while (p.isRunning()) {
        if (abort && abort->load()) { p.stop(500); fs::remove(tmp, ec); err = "download cancelled"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (p.lastExitCode() != 0) {
        fs::remove(tmp, ec);
        err = "download failed (curl exit " + std::to_string(p.lastExitCode()) + "): " + url;
        return false;
    }
    if (!replaceMediaFileAtomic(tmp, dest, ec)) { fs::remove(tmp, ec); err = dest.string() + ": " + ec.message(); return false; }
    return true;
#endif
}

// The first member of a zip whose name a drive takes.
bool pickZipImage(const std::vector<uint8_t>& zip, std::string& member, std::string& err)
{
    std::vector<std::string> names;
    if (!listZipMembers(zip, names, err)) return false;
    for (const auto& n : names) {
        if (n.empty() || n.back() == '/') continue;
        const std::string base = safeName(n);
        if (base.rfind("._", 0) == 0) continue;            // macOS resource forks
        // By name only: the member is not a file yet, and the size half of
        // classifyDiskForSlot needs one. The drive classifies it once out.
        std::string ext = lower(base);
        ext = ext.substr(std::min(ext.size(), ext.find_last_of('.')));
        for (const char* e : { ".dsk", ".do", ".po", ".nib", ".woz", ".moof", ".2mg",
                               ".hdv", ".d13", ".image" })
            if (ext == e) { member = n; return true; }
    }
    err = "the archive holds no disk image POM2 can mount";
    return false;
}

} // namespace

bool isHttpUrl(const std::string& source)
{
    const std::string l = lower(source);
    return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
}

bool isCompressedImage(const std::string& source)
{
    std::string l = lower(source);
    if (const auto q = l.find_first_of("?#"); q != std::string::npos && isHttpUrl(source)) l.erase(q);
    return endsWith(l, ".gz") || endsWith(l, ".zip");
}

fs::path mediaCacheDir()
{
    return userDataDir() / "media_cache";
}

MediaSourceResult resolveMediaSource(const std::string& source, const fs::path& cacheDir,
                                     const std::atomic<bool>* abort)
{
    MediaSourceResult r;
    std::string path = source;

    // 1. A URL: into the cache, once.
    if (isHttpUrl(source)) {
        const fs::path dest = cacheDir / (key(source) + "-" + safeName(source));
        std::error_code ec;
        if (fs::is_regular_file(dest, ec) && fs::file_size(dest, ec) > 0) {
            r.fromCache = true;
        } else if (!download(source, dest, r.error, abort)) {
            return r;
        }
        path = dest.string();
        r.note = r.fromCache ? "from the download cache" : "downloaded";
    }

    // 2. An archive: its image into the cache, once per archive version (the
    //    key takes the archive's size and date, so a newer archive is
    //    unpacked afresh and an unchanged one keeps the guest's writes).
    if (isCompressedImage(path)) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        const long long when = static_cast<long long>(fs::last_write_time(path, ec).time_since_epoch().count());
        const std::string k = key(fs::absolute(path, ec).string() + "|" + std::to_string(size)
                                  + "|" + std::to_string(when));
        std::vector<uint8_t> packed, image;
        if (!readFile(path, packed, r.error)) return r;
        std::string member;
        if (endsWith(lower(path), ".zip")) {
            if (!pickZipImage(packed, member, r.error)) return r;
            const fs::path dest = cacheDir / (k + "-" + safeName(member));
            if (fs::is_regular_file(dest, ec)) {
                r.ok = true; r.fromCache = true; r.localPath = dest.string();
                r.note += std::string(r.note.empty() ? "" : ", ") + safeName(member) + " from the zip (cached copy)";
                return r;
            }
            if (!readZipMember(packed, member, image, r.error, kMaxImageBytes)) return r;
            if (!writeFile(dest, image, r.error)) return r;
            r.localPath = dest.string();
            r.note += std::string(r.note.empty() ? "" : ", ") + safeName(member) + " from the zip";
        } else {
            std::string inner;
            if (!gunzip(packed, image, r.error, kMaxImageBytes, &inner)) return r;
            std::string name = safeName(inner);
            if (inner.empty()) {
                name = safeName(path);
                name.erase(name.size() - 3);                  // drop ".gz"
            }
            const fs::path dest = cacheDir / (k + "-" + name);
            if (fs::is_regular_file(dest, ec)) {
                r.ok = true; r.fromCache = true; r.localPath = dest.string();
                r.note += std::string(r.note.empty() ? "" : ", ") + "ungzipped (cached copy)";
                return r;
            }
            if (!writeFile(dest, image, r.error)) return r;
            r.localPath = dest.string();
            r.note += std::string(r.note.empty() ? "" : ", ") + "ungzipped";
        }
        r.ok = true;
        return r;
    }

    r.ok = true;
    r.localPath = path;
    return r;
}

} // namespace pom2
