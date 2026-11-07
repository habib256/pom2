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

// media_source — a .gz, a .zip or a URL turned into a mountable local image
// (MediaSource.h). The archives are built here (stored deflate blocks, a
// stored zip member), so the test needs no tool and no network:
//
//  1. a .dsk.gz inflates into the cache, byte for byte; the second resolve
//     reuses the cached copy (the guest's writes live there)
//  2. a .zip with a text file and a disk picks the disk
//  3. a .zip with no disk, a damaged .gz: refused with a reason
//  4. an unreachable URL fails cleanly (curl's exit, no partial file)
//  5. the predicates

#include "MediaSource.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

uint32_t crc32(const std::vector<uint8_t>& d)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void p16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void p32(std::vector<uint8_t>& o, uint32_t v) { p16(o, v & 0xFFFF); p16(o, v >> 16); }

// gzip of `data` in stored deflate blocks (BTYPE 00), with FNAME — or
// without one when `name` is null (`gzip -n`, or gzip reading stdin).
std::vector<uint8_t> gzipStored(const std::vector<uint8_t>& data, const char* name)
{
    std::vector<uint8_t> o = { 0x1F, 0x8B, 8, uint8_t(name ? 0x08 : 0), 0, 0, 0, 0, 0, 3 };
    if (name) {
        for (const char* p = name; *p; ++p) o.push_back(uint8_t(*p));
        o.push_back(0);
    }
    std::size_t at = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, data.size() - at);
        o.push_back(at + n == data.size() ? 1 : 0);          // BFINAL, BTYPE 00
        p16(o, uint32_t(n)); p16(o, uint32_t(~n & 0xFFFF));
        o.insert(o.end(), data.begin() + std::ptrdiff_t(at), data.begin() + std::ptrdiff_t(at + n));
        at += n;
    } while (at < data.size());
    p32(o, crc32(data));
    p32(o, uint32_t(data.size()));
    return o;
}

// A zip of stored members.
std::vector<uint8_t> zipStored(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files)
{
    std::vector<uint8_t> o, cd;
    for (const auto& [name, data] : files) {
        const uint32_t off = uint32_t(o.size()), c = crc32(data), n = uint32_t(data.size());
        p32(o, 0x04034B50); p16(o, 20); p16(o, 0); p16(o, 0); p16(o, 0); p16(o, 0);
        p32(o, c); p32(o, n); p32(o, n); p16(o, uint32_t(name.size())); p16(o, 0);
        o.insert(o.end(), name.begin(), name.end());
        o.insert(o.end(), data.begin(), data.end());
        p32(cd, 0x02014B50); p16(cd, 20); p16(cd, 20); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0);
        p32(cd, c); p32(cd, n); p32(cd, n); p16(cd, uint32_t(name.size())); p16(cd, 0); p16(cd, 0);
        p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, off);
        cd.insert(cd.end(), name.begin(), name.end());
    }
    const uint32_t cdOff = uint32_t(o.size());
    o.insert(o.end(), cd.begin(), cd.end());
    p32(o, 0x06054B50); p16(o, 0); p16(o, 0); p16(o, uint32_t(files.size())); p16(o, uint32_t(files.size()));
    p32(o, uint32_t(cd.size())); p32(o, cdOff); p16(o, 0);
    return o;
}

void put(const fs::path& p, const std::vector<uint8_t>& d)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

std::vector<uint8_t> get(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

} // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "pom2_media_source_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path cache = dir / "cache";

    std::vector<uint8_t> disk(143360);
    for (std::size_t i = 0; i < disk.size(); ++i) disk[i] = uint8_t(i * 13 + (i >> 8));

    // 1. .gz
    put(dir / "game.dsk.gz", gzipStored(disk, "game.dsk"));
    auto r = pom2::resolveMediaSource((dir / "game.dsk.gz").string(), cache);
    CHECK(r.ok && !r.fromCache);
    CHECK(fs::path(r.localPath).filename().string().find("game.dsk") != std::string::npos);
    CHECK(get(r.localPath) == disk);
    { // the guest writes to the cached copy; the next resolve keeps them
        std::fstream f(r.localPath, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(5); f.put(char(0x5A));
    }
    auto again = pom2::resolveMediaSource((dir / "game.dsk.gz").string(), cache);
    CHECK(again.ok && again.fromCache && again.localPath == r.localPath);
    CHECK(get(again.localPath)[5] == 0x5A);

    // 2. .zip: the disk, not the readme.
    put(dir / "pack.zip", zipStored({ { "README.TXT", { 'h', 'i' } },
                                      { "__MACOSX/._game.po", { 0 } },
                                      { "disks/game.po", disk } }));
    r = pom2::resolveMediaSource((dir / "pack.zip").string(), cache);
    CHECK(r.ok && get(r.localPath) == disk);
    CHECK(fs::path(r.localPath).filename().string().find("game.po") != std::string::npos);

    // 3. Refusals.
    put(dir / "notes.zip", zipStored({ { "a.txt", { 'x' } } }));
    r = pom2::resolveMediaSource((dir / "notes.zip").string(), cache);
    CHECK(!r.ok && r.error.find("no disk image") != std::string::npos);
    auto bad = gzipStored(disk, "x.dsk");
    bad[bad.size() - 9] ^= 0xFF;                        // the last data byte
    put(dir / "bad.dsk.gz", bad);
    r = pom2::resolveMediaSource((dir / "bad.dsk.gz").string(), cache);
    CHECK(!r.ok && r.error.find("CRC") != std::string::npos);
    { // the drop's refusal says what failed, not "no disk image format"
        const std::string why = pom2::explainMediaSourceRefusal((dir / "bad.dsk.gz").string(), r.error);
        CHECK(why.find("bad.dsk.gz") != std::string::npos && why.find("CRC") != std::string::npos);
        CHECK(why.find("match no Apple II disk format") == std::string::npos);
    }

    // 3b. '#' and '?' belong to a URL's query and fragment only. In a local
    //     path, a zip member or a gzip name they are part of the name: cut
    //     there, a FNAME-less "a#b.dsk.gz" threw std::out_of_range (the
    //     ".gz" erase ran past a one-letter name), a zip's "Disk #1.dsk" was
    //     "no disk image", and the cache copy lost its extension.
    {
        auto endsWith = [](const std::string& s, const std::string& x) {
            return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
        };
        put(dir / "a#b.dsk.gz", gzipStored(disk, nullptr));
        r = pom2::resolveMediaSource((dir / "a#b.dsk.gz").string(), cache);
        CHECK(r.ok && endsWith(fs::path(r.localPath).filename().string(), "a#b.dsk"));
        CHECK(r.ok && get(r.localPath) == disk);

        fs::create_directories(dir / "#games", ec);
        put(dir / "#games" / "x.dsk.gz", gzipStored(disk, nullptr));
        r = pom2::resolveMediaSource((dir / "#games" / "x.dsk.gz").string(), cache);
        CHECK(r.ok && endsWith(fs::path(r.localPath).filename().string(), "-x.dsk"));

        put(dir / "hash.zip", zipStored({ { "Side #2/Disk #1.dsk", disk } }));
        r = pom2::resolveMediaSource((dir / "hash.zip").string(), cache);
        CHECK(r.ok && endsWith(fs::path(r.localPath).filename().string(), "Disk #1.dsk"));

        put(dir / "named.gz", gzipStored(disk, "Disk #1.dsk"));
        r = pom2::resolveMediaSource((dir / "named.gz").string(), cache);
        CHECK(r.ok && endsWith(fs::path(r.localPath).filename().string(), "Disk #1.dsk"));
    }

    // 3c. One archive, two spellings (a symlinked folder; macOS's /tmp is
    //     /private/tmp): one cache copy, so the guest's writes are found
    //     again whichever path the next mount names.
    {
        fs::create_directories(dir / "real", ec);
        put(dir / "real" / "twice.dsk.gz", gzipStored(disk, "twice.dsk"));
        std::error_code lec;
        fs::create_directory_symlink(dir / "real", dir / "link", lec);
        if (!lec) {
            const auto a = pom2::resolveMediaSource((dir / "real" / "twice.dsk.gz").string(), cache);
            const auto b = pom2::resolveMediaSource((dir / "link" / "twice.dsk.gz").string(), cache);
            CHECK(a.ok && b.ok && b.fromCache && a.localPath == b.localPath);
        } else {
            std::printf("media_source: no symlink here (%s), spelling case skipped\n",
                        lec.message().c_str());
        }
    }

    // 4. An unreachable URL: a clean failure, nothing left in the cache.
    r = pom2::resolveMediaSource("http://127.0.0.1:9/nothing.dsk", cache);
    CHECK(!r.ok && !r.error.empty());
    bool partial = false;
    for (const auto& e : fs::directory_iterator(cache, ec))
        if (e.path().filename().string().find("nothing.dsk") != std::string::npos) partial = true;
    CHECK(!partial);

    // 4b. A body that is a web page is not a disk: never cached, and a copy
    //     an older build cached is dropped, so the next try downloads again
    //     instead of mounting the same HTML for good.
    {
        const std::string url = "http://127.0.0.1:9/soft404.dsk";
        const fs::path stale = pom2::mediaUrlCachePath(url, cache);
        CHECK(stale.filename().string().find("soft404.dsk") != std::string::npos);
        const std::string html = "\n  <!DOCTYPE html><html><body>Not found</body></html>";
        put(stale, std::vector<uint8_t>(html.begin(), html.end()));
        r = pom2::resolveMediaSource(url, cache);
        CHECK(!r.ok && !r.fromCache);
        CHECK(!fs::exists(stale));
        CHECK(pom2::mediaBodyIsWebPage(std::vector<uint8_t>(html.begin(), html.end())));
        const std::string xml = "<?xml version=\"1.0\"?><Error/>";
        CHECK(pom2::mediaBodyIsWebPage(std::vector<uint8_t>(xml.begin(), xml.end())));
        CHECK(!pom2::mediaBodyIsWebPage(disk));
        CHECK(!pom2::mediaBodyIsWebPage({ 'W', 'O', 'Z', '2' }));
        auto ext = [](const char* e) { return std::string(e ? e : ""); };
        CHECK(ext(pom2::mediaArchiveExtension(gzipStored(disk, "a.dsk"))) == ".gz");
        CHECK(ext(pom2::mediaArchiveExtension(zipStored({ { "a.dsk", disk } }))) == ".zip");
        CHECK(pom2::mediaArchiveExtension(disk) == nullptr);
    }

    // 4c. The cache key (bug hunt 2026-11-07). Two URLs with one last
    //     segment and one 32-bit FNV-1a: the old key gave them one cache
    //     copy, and a hit opens no socket, so whichever came first was
    //     served as the other for good.
    {
        auto fnv32 = [](const std::string& s) {
            uint32_t h = 2166136261u;
            for (unsigned char c : s) h = (h ^ c) * 16777619u;
            return h;
        };
        const std::string a = "http://example.test/disk.dsk?m4urx81r9b";
        const std::string b = "http://example.test/disk.dsk?lgll5sars5";
        CHECK(fnv32(a) == fnv32(b));                       // the pair does collide
        CHECK(pom2::mediaUrlCachePath(a, cache) != pom2::mediaUrlCachePath(b, cache));
        // A copy cached under the old key moves to the new one, with the
        // guest's writes in it, and no download is tried.
        const std::string url = "http://127.0.0.1:9/legacy.dsk";
        char old[9];
        std::snprintf(old, sizeof old, "%08x", fnv32(url));
        fs::create_directories(cache, ec);
        put(cache / (std::string(old) + "-legacy.dsk"), disk);
        r = pom2::resolveMediaSource(url, cache);
        CHECK(r.ok && r.fromCache && fs::path(r.localPath) == pom2::mediaUrlCachePath(url, cache));
        CHECK(r.ok && get(r.localPath) == disk);
        CHECK(!fs::exists(cache / (std::string(old) + "-legacy.dsk")));
    }

    // 5. Predicates.
    CHECK(pom2::isHttpUrl("HTTPS://example.org/a.dsk") && !pom2::isHttpUrl("tnfs://h/a.po"));
    CHECK(pom2::isCompressedImage("a.ZIP") && pom2::isCompressedImage("b.dsk.gz"));
    CHECK(pom2::isCompressedImage("https://x/y.zip?dl=1") && !pom2::isCompressedImage("c.dsk"));
    CHECK(!pom2::needsMediaResolve("/disks/plain.woz"));

    fs::remove_all(dir, ec);
    if (g_fail) {
        std::fprintf(stderr, "media_source: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("media_source: PASS\n");
    return 0;
}
