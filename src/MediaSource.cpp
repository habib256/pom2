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
#include "Sha256.h"
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

// The cache key of a source: SHA-256 of its exact spelling, 128 bits of it
// in hex. It was a 32-bit FNV-1a, and a hit opens no socket: a URL padded
// until its hash matched a genuine one's, fetched first, was served in its
// place for good (bug hunt 2026-11-07; TnfsMedia.cpp closed the same hole).
std::string key(const std::string& s)
{
    return sha256Hex(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()).substr(0, 32);
}

// The key before 2026-11-07. Read only to carry an existing cache copy — and
// the guest's writes in it — over to its new name; nothing is created under it.
std::string legacyKey(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    char b[9];
    std::snprintf(b, sizeof b, "%08x", h);
    return b;
}

// `dest` absent and the copy an older POM2 made under `legacy` present: move it.
void adoptLegacyCopy(const fs::path& legacy, const fs::path& dest)
{
    std::error_code ec;
    if (!fs::exists(dest, ec) && !ec && fs::is_regular_file(legacy, ec))
        fs::rename(legacy, dest, ec);
}

// A file name safe on every host, from the last segment of a URL, a path or
// a member. Only a URL has a query or a fragment to drop: in a local path,
// a zip member or a gzip name, '#' and '?' are part of the name (cut there,
// "a#b.dsk.gz" became "a" and the ".gz" erase below ran off its end).
std::string safeName(std::string s, bool url = false)
{
    if (url)
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

std::vector<uint8_t> head(const fs::path& p, std::size_t n = 512)
{
    std::vector<uint8_t> b(n);
    std::ifstream f(p, std::ios::binary);
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(n));
    b.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, f.gcount())));
    return b;
}

// curl, as RomFetch runs it, but for disks: http allowed (Asimov mirrors,
// old pages), a disk-sized cap, every redirect followed within http(s).
bool download(const std::string& url, const fs::path& dest, std::string& err,
              const std::atomic<bool>* abort, fs::path& published)
{
#if !POM2_HAS_CHILD_PROCESS
    (void)url; (void)dest; (void)abort; (void)published;
    err = "downloads need a helper program, not available in this build";
    return false;
#else
    const std::string curl = ChildProcess::findOnPath("curl");
    if (curl.empty()) { err = "curl was not found on PATH — install it to open disks by URL"; return false; }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = tempSiblingPath(dest);
    // Our own cap as well as curl's: before curl 8.4 `--max-filesize` does
    // nothing when the size is not announced (chunked, no Content-Length), and
    // Ubuntu 22.04 / Debian 12 ship 7.81 / 7.88 — two minutes of a full link
    // went to disk and was then cached as a disk (bug hunt 2026-11-07).
    auto oversized = [&] {
        std::error_code sec;
        const auto n = fs::file_size(tmp, sec);
        return !sec && n > kMaxImageBytes;
    };
    ChildProcess p;
    if (!p.start(curl, { "-fsL", "--proto", "=http,https", "--proto-redir", "=http,https",
                         "--max-filesize", std::to_string(kMaxImageBytes),
                         "--retry", "2", "--max-time", "120", "-o", tmp.string(), url },
                 "", err))
        return false;
    while (p.isRunning()) {
        if (abort && abort->load()) { p.stop(500); fs::remove(tmp, ec); err = "download cancelled"; return false; }
        if (oversized()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (oversized()) {
        if (p.isRunning()) p.stop(500);
        fs::remove(tmp, ec);
        err = "the download is larger than any Apple II disk: " + url;
        return false;
    }
    if (p.lastExitCode() != 0) {
        fs::remove(tmp, ec);
        err = "download failed (curl exit " + std::to_string(p.lastExitCode()) + "): " + url;
        return false;
    }
    // A 200 is not a disk. A soft 404, a login wall or a "moved" page comes
    // back with curl's exit 0: cached under the disk's name it was mounted —
    // and refused — on every later try, with nothing in the UI to clear it.
    const auto h = head(tmp);
    if (mediaBodyIsWebPage(h)) {
        fs::remove(tmp, ec);
        err = "the server answered with a web page, not a disk image: " + url;
        return false;
    }
    // A link with no archive extension (".../download?id=12") serving a zip
    // or a gzip: name the copy so step 2 unpacks it.
    fs::path to = dest;
    const std::string low = lower(dest.filename().string());
    if (const char* ext = mediaArchiveExtension(h); ext && !endsWith(low, ext))
        to += ext;
    if (!replaceMediaFileAtomic(tmp, to, ec)) { fs::remove(tmp, ec); err = to.string() + ": " + ec.message(); return false; }
    published = to;
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
        for (const char* e : { ".dsk", ".do", ".po", ".nib", ".nb2", ".woz", ".moof", ".2mg",
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

fs::path mediaUrlCachePath(const std::string& url, const fs::path& cacheDir)
{
    return cacheDir / (key(url) + "-" + safeName(url, /*url=*/true));
}

bool mediaBodyIsWebPage(const std::vector<uint8_t>& h)
{
    std::size_t i = 0;
    if (h.size() >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) i = 3;   // UTF-8 BOM
    while (i < h.size() && (h[i] == ' ' || h[i] == '\t' || h[i] == '\r' || h[i] == '\n')) ++i;
    std::string s;
    for (; i < h.size() && s.size() < 9; ++i) s.push_back(char(std::tolower(h[i])));
    // No Apple II image starts with '<': DOS / ProDOS boot code is $01, a
    // NIB is $FF sync, WOZ / MOOF / 2IMG are their magic.
    return s.rfind("<!doctype", 0) == 0 || s.rfind("<html", 0) == 0 || s.rfind("<?xml", 0) == 0;
}

const char* mediaArchiveExtension(const std::vector<uint8_t>& h)
{
    if (h.size() >= 4 && h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4) return ".zip";
    if (h.size() >= 3 && h[0] == 0x1F && h[1] == 0x8B && h[2] == 8) return ".gz";
    return nullptr;
}

std::string explainMediaSourceRefusal(const std::string& archive, const std::string& error)
{
    const std::string name = isHttpUrl(archive) ? archive
                                                : fs::path(archive).filename().string();
    return "\"" + name + "\" could not be opened as a disk: " + error + ".\n\n"
           "POM2 opens a .zip (stored or deflated members) and a .gz itself, and takes "
           "the first .dsk, .do, .po, .nib, .nb2, .woz, .moof, .2mg, .hdv, .d13 or .image "
           "inside. A damaged archive has to be downloaded again; a .7z, .shk or an "
           "encrypted zip has to be unpacked first.";
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
        const fs::path dest = mediaUrlCachePath(source, cacheDir);
        fs::path got;
        std::error_code ec;
        // The copy keeps the archive extension the download sniffed.
        const fs::path legacy = cacheDir / (legacyKey(source) + "-" + safeName(source, true));
        for (const char* ext : { "", ".zip", ".gz" }) {
            fs::path c = dest, old = legacy;
            c += ext;
            old += ext;
            adoptLegacyCopy(old, c);
            if (!fs::is_regular_file(c, ec) || fs::file_size(c, ec) == 0) continue;
            // A page an older POM2 cached as a disk: drop it, fetch afresh.
            if (mediaBodyIsWebPage(head(c))) { fs::remove(c, ec); continue; }
            got = c;
            break;
        }
        if (!got.empty()) {
            r.fromCache = true;
        } else if (!download(source, dest, r.error, abort, got)) {
            return r;
        }
        path = got.string();
        r.note = r.fromCache ? "from the download cache" : "downloaded";
    }

    // 2. An archive: its image into the cache, once per archive version (the
    //    key takes the archive's size and date, so a newer archive is
    //    unpacked afresh and an unchanged one keeps the guest's writes).
    if (isCompressedImage(path)) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        const long long when = static_cast<long long>(fs::last_write_time(path, ec).time_since_epoch().count());
        // The FILE, not the spelling: through a symlinked folder (macOS's
        // /tmp is /private/tmp) or another case, `absolute` named a second
        // cache copy and the guest's writes seemed gone.
        fs::path canon = fs::weakly_canonical(path, ec);
        if (ec || canon.empty()) { ec.clear(); canon = fs::absolute(path, ec).lexically_normal(); }
        const std::string id = canon.string() + "|" + std::to_string(size)
                               + "|" + std::to_string(when);
        const std::string k = key(id);
        std::vector<uint8_t> packed, image;
        if (!readFile(path, packed, r.error)) return r;
        std::string member;
        if (endsWith(lower(path), ".zip")) {
            if (!pickZipImage(packed, member, r.error)) return r;
            const fs::path dest = cacheDir / (k + "-" + safeName(member));
            adoptLegacyCopy(cacheDir / (legacyKey(id) + "-" + safeName(member)), dest);
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
                name = safeName(fs::path(path).filename().string());
                if (name.size() > 3 && endsWith(lower(name), ".gz"))
                    name.erase(name.size() - 3);              // drop ".gz"
            }
            const fs::path dest = cacheDir / (k + "-" + name);
            adoptLegacyCopy(cacheDir / (legacyKey(id) + "-" + name), dest);
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
