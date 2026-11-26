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

// media_source — a .gz, a .zip, a .7z or a URL turned into a mountable local
// image (MediaSource.h). The archives are built here (stored deflate blocks, a
// stored zip member) or embedded (SevenZipFixtures.h), so the test needs no
// tool and no network:
//
//  1. a .dsk.gz inflates into the cache, byte for byte; the second resolve
//     reuses the cached copy (the guest's writes live there)
//  2. a .zip with a text file and a disk picks the disk
//  3. a .zip with no disk, a damaged .gz: refused with a reason
//  4. an unreachable URL fails cleanly (curl's exit, no partial file)
//  5. the predicates
//  6. a .7z: its first disk mounted, the other beside it; a .7z packed with
//     a method POM2 lacks refused as such

#include "MediaSource.h"
#include "SevenZipFixtures.h"
#include "ZipMember.h"

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

    // 0. Hostile zip offsets (bug hunt 2026-11-17). The bounds were `cd + 46
    //    > n`, which wraps on wasm32 for a uint32 offset near 4 GiB and read
    //    before the buffer (ASan, browser build). On 64-bit the old form held,
    //    so this pins that every walk REFUSES them cleanly; the subtraction
    //    form is what keeps a 32-bit size_t honest.
    {
        std::string err;
        std::vector<std::string> names;
        std::vector<uint8_t> out;
        std::vector<uint8_t> eocdOnly(178, 0);
        p32(eocdOnly, 0x06054B50); p16(eocdOnly, 0); p16(eocdOnly, 0);
        p16(eocdOnly, 1); p16(eocdOnly, 1); p32(eocdOnly, 46); p32(eocdOnly, 0xFFFFFFF0u); p16(eocdOnly, 0);
        CHECK(!pom2::listZipMembers(eocdOnly, names, err));
        CHECK(!pom2::readZipMember(eocdOnly, "a", out, err, 1 << 20));
        // A good central entry whose local-header offset is near 4 GiB.
        std::vector<uint8_t> farLocal = zipStored({ { "a", { 1, 2, 3 } } });
        const std::size_t cdAt = farLocal.size() - 22 - (46 + 1);
        for (int k = 0; k < 4; ++k) farLocal[cdAt + 42 + k] = uint8_t(0xFFFFFFF0u >> (8 * k));
        CHECK(pom2::listZipMembers(farLocal, names, err) && names.size() == 1);
        CHECK(!pom2::readZipMember(farLocal, "a", out, err, 1 << 20));
        // listZipEntries + readZipEntry read what readZipMember reads.
        const std::vector<uint8_t> two = zipStored({ { "x", { 7 } }, { "y", { 8, 9 } } });
        std::vector<pom2::ZipEntry> entries;
        CHECK(pom2::listZipEntries(two, entries, err) && entries.size() == 2);
        std::vector<uint8_t> viaEntry, viaName;
        CHECK(entries.size() == 2 && pom2::readZipEntry(two, entries[1], viaEntry, err, 1 << 20) &&
              pom2::readZipMember(two, "y", viaName, err, 1 << 20) &&
              viaEntry == viaName && viaName == std::vector<uint8_t>({ 8, 9 }));
    }

    // 0b. A copy an older POM2 cached under the 32-bit FNV key is not
    //     adopted (bug hunt 2026-11-17): that key collides, and the query it
    //     ignores is free padding. Seeded under this URL's own legacy name;
    //     the URL itself is unreachable (local port 9), so nothing leaves
    //     the machine and the only way to "succeed" is to serve the seed.
    {
        const std::string url = "http://127.0.0.1:9/legacy.dsk";
        uint32_t h = 2166136261u;
        for (unsigned char c : url) h = (h ^ c) * 16777619u;
        char key[9];
        std::snprintf(key, sizeof key, "%08x", h);
        fs::create_directories(cache);
        put(cache / (std::string(key) + "-legacy.dsk"), disk);
        const auto r = pom2::resolveMediaSource(url, cache);
        CHECK(!r.ok && "a legacy FNV-keyed copy was served for a URL");
        CHECK(!fs::exists(pom2::mediaUrlCachePath(url, cache)));
    }

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
        // A URL copy cached under the old key is no longer adopted — see 0b.
    }

    // 5. Predicates.
    CHECK(pom2::isHttpUrl("HTTPS://example.org/a.dsk") && !pom2::isHttpUrl("tnfs://h/a.po"));
    CHECK(pom2::isCompressedImage("a.ZIP") && pom2::isCompressedImage("b.dsk.gz"));
    CHECK(pom2::isCompressedImage("https://x/y.zip?dl=1") && !pom2::isCompressedImage("c.dsk"));
    CHECK(!pom2::needsMediaResolve("/disks/plain.woz"));

    // 6. Exchange files (ExchangeMedia.h), converted once into the cache.
    //    A DiskCopy 4.2 image built here, its blocks as a .po; the guest's
    //    writes to the converted copy survive the next resolve.
    {
        auto endsWith = [](const std::string& s, const std::string& x) {
            return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
        };
        std::vector<uint8_t> blocks(819200);
        for (std::size_t i = 0; i < blocks.size(); ++i) blocks[i] = uint8_t(i * 7 + (i >> 9));
        auto be32 = [](std::vector<uint8_t>& v, std::size_t at, uint32_t x) {
            for (int k = 0; k < 4; ++k) v[at + k] = uint8_t(x >> (24 - 8 * k));
        };
        std::vector<uint8_t> dc(84, 0);
        dc[0] = 4; dc[1] = 'T'; dc[2] = 'E'; dc[3] = 'S'; dc[4] = 'T';
        be32(dc, 64, 819200);
        uint32_t sum = 0;   // [B] File Type Note $E0/$0005: add each word, rotate right
        for (std::size_t i = 0; i < blocks.size(); i += 2) {
            sum += (uint32_t(blocks[i]) << 8) | blocks[i + 1];
            sum = (sum >> 1) | (sum << 31);
        }
        be32(dc, 72, sum);
        dc[82] = 0x01;
        dc.insert(dc.end(), blocks.begin(), blocks.end());
        put(dir / "Game.image", dc);
        CHECK(pom2::needsMediaResolve((dir / "Game.image").string()));
        auto got = pom2::resolveMediaSource((dir / "Game.image").string(), cache);
        CHECK(got.ok && endsWith(fs::path(got.localPath).filename().string(), "Game.po"));
        CHECK(got.ok && get(got.localPath) == blocks);
        CHECK(got.note.find("DiskCopy") != std::string::npos);
        if (got.ok) {
            auto written = get(got.localPath);
            written[1024] ^= 0xFF;
            put(got.localPath, written);
            auto reopened = pom2::resolveMediaSource((dir / "Game.image").string(), cache);
            CHECK(reopened.ok && reopened.localPath == got.localPath && get(reopened.localPath) == written);
        }
        // A raw 800 KB .image is mounted as it is.
        put(dir / "raw.image", blocks);
        CHECK(!pom2::needsMediaResolve((dir / "raw.image").string()));
        // A ShrinkIt archive inside a zip: unzipped, then converted.
        std::ifstream shk("tests/fixtures/nufx/files.shk", std::ios::binary);
        std::vector<uint8_t> archive{std::istreambuf_iterator<char>(shk), {}};
        CHECK(!archive.empty());
        CHECK(pom2::isExchangeMedia("Files.SHK") && pom2::needsMediaResolve("x.sdk"));
        put(dir / "files.zip", zipStored({ { "Files.shk", archive } }));
        auto zipped = pom2::resolveMediaSource((dir / "files.zip").string(), cache);
        CHECK(zipped.ok && endsWith(fs::path(zipped.localPath).filename().string(), "Files.po"));
        CHECK(zipped.ok && get(zipped.localPath).size() == 143360);
        CHECK(zipped.note.find("ShrinkIt") != std::string::npos);

        // A zip of two sides: both land in the cache under one key, the
        // first is mounted, and the set names both, in name order.
        std::vector<uint8_t> sideB = disk;
        sideB[0] ^= 0xFF;
        put(dir / "game2.zip", zipStored({ { "Game Side B.dsk", sideB }, { "Game Side A.dsk", disk } }));
        auto two = pom2::resolveMediaSource((dir / "game2.zip").string(), cache);
        CHECK(two.ok && endsWith(fs::path(two.localPath).filename().string(), "Game Side B.dsk"));
        CHECK(two.note.find("2 disks") != std::string::npos);
        const auto set = pom2::mediaSetOf(two.localPath, cache);
        CHECK(set.size() == 2);
        if (set.size() == 2) {
            CHECK(pom2::mediaDisplayName(set[0]) == "Game Side A.dsk" && get(set[0]) == disk);
            CHECK(pom2::mediaDisplayName(set[1]) == "Game Side B.dsk" && get(set[1]) == sideB);
        }
        // A disk outside the cache is a set of one, and keeps its name.
        CHECK(pom2::mediaSetOf((dir / "raw.image").string(), cache).size() == 1);
        CHECK(pom2::mediaDisplayName((dir / "raw.image").string()) == "raw.image");
    }

    // 6. A .7z (LZMA2, solid, written by bsdtar): the disks in the archive's
    //    order, the readme skipped, the second side in the set; again from
    //    the cache. A BZip2 one is refused for its method, in words.
    {
        auto endsWith = [](const std::string& s, const std::string& x) {
            return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
        };
        put(dir / "Game.7z", pom2test::kSevenZipLzma2);
        auto seven = pom2::resolveMediaSource((dir / "Game.7z").string(), cache);
        CHECK(seven.ok && !seven.fromCache);
        CHECK(seven.ok && endsWith(fs::path(seven.localPath).filename().string(), "Side A.dsk"));
        CHECK(seven.ok && get(seven.localPath) == pom2test::sevenZipFixtureDisk('A'));
        CHECK(seven.note.find("from the 7z") != std::string::npos);
        CHECK(seven.note.find("2 disks in the 7z") != std::string::npos);
        const auto sides = pom2::mediaSetOf(seven.localPath, cache);
        CHECK(sides.size() == 2);
        if (sides.size() == 2) CHECK(get(sides[1]) == pom2test::sevenZipFixtureDisk('B'));
        // The disk mounted names its archive; side B, which resolving the
        // archive again would not mount, names nothing; nor does a file.
        CHECK(pom2::mediaSourceOf(seven.localPath) == (dir / "Game.7z").string());
        if (sides.size() == 2) CHECK(pom2::mediaSourceOf(sides[1]).empty());
        CHECK(pom2::mediaSourceOf((dir / "Game.7z").string()).empty());
        auto cachedSeven = pom2::resolveMediaSource((dir / "Game.7z").string(), cache);
        CHECK(cachedSeven.ok && cachedSeven.fromCache && cachedSeven.localPath == seven.localPath);

        put(dir / "PPMd.7z", pom2test::kSevenZipPpmd);
        auto ppmd = pom2::resolveMediaSource((dir / "PPMd.7z").string(), cache);
        CHECK(ppmd.ok && get(ppmd.localPath) == pom2test::sevenZipFixtureDisk('A'));

        put(dir / "Old.7z", pom2test::kSevenZipBzip2);
        auto old = pom2::resolveMediaSource((dir / "Old.7z").string(), cache);
        CHECK(!old.ok && old.error.find("method") != std::string::npos);
        const std::string why = pom2::explainMediaSourceRefusal((dir / "Old.7z").string(), old.error);
        CHECK(why.find("Old.7z") != std::string::npos && why.find("BZip2") != std::string::npos);

        // A .7z cut after its signature.
        put(dir / "cut.7z", std::vector<uint8_t>(pom2test::kSevenZipLzma2.begin(),
                                                   pom2test::kSevenZipLzma2.begin() + 6));
        CHECK(!pom2::resolveMediaSource((dir / "cut.7z").string(), cache).ok);

        CHECK(pom2::isCompressedImage("GAME.7Z") && pom2::needsMediaResolve("x/y.7z"));
        CHECK(pom2::isCompressedImage("https://x/y.7z?dl=1"));
        const char* sniffed = pom2::mediaArchiveExtension(pom2test::kSevenZipLzma2);
        CHECK(sniffed && std::string(sniffed) == ".7z");
    }

    fs::remove_all(dir, ec);
    if (g_fail) {
        std::fprintf(stderr, "media_source: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("media_source: PASS\n");
    return 0;
}
