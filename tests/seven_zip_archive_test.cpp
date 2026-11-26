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

// seven_zip_archive — the .7z reader (SevenZipArchive.h) on archives another
// writer made (bsdtar / libarchive, tests/SevenZipFixtures.h):
//
//  1. LZMA, LZMA2 and PPMd: every member, byte for byte, names in UTF-8
//  2. BZip2: every member refused as UNSUPPORTED — the second one too (the
//     SDK's block cache used to turn it into a CRC failure)
//  3. the ceilings: a block larger than allowed, too many names, a member
//     larger than the caller takes
//  4. damage: every single byte of an archive flipped in turn, and every
//     truncation — refused or read, never a crash (run it under ASan)

#include "SevenZipArchive.h"
#include "SevenZipFixtures.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

constexpr std::size_t kBlock = 1u << 20;
constexpr std::size_t kMember = 1u << 20;

// What every fixture holds, in its own order (bsdtar writes the folder last).
struct Expected {
    const char* name;
    bool directory;
    std::vector<std::uint8_t> bytes;
};

std::vector<Expected> expected()
{
    return { { "GAME/README.TXT", false, { 'h', 'e', 'l', 'l', 'o' } },
             { "GAME/Side A.dsk", false, pom2test::sevenZipFixtureDisk('A') },
             { "GAME/Side B.dsk", false, pom2test::sevenZipFixtureDisk('B') },
             { "GAME", true, {} } };
}

void readsEverything(const char* method, const std::vector<std::uint8_t>& archive)
{
    pom2::SevenZipArchive z;
    std::string err;
    CHECK(pom2::isSevenZip(archive));
    if (!z.open(archive, err, kBlock)) {
        std::fprintf(stderr, "FAIL %s: open: %s\n", method, err.c_str());
        ++g_fail;
        return;
    }
    const auto want = expected();
    CHECK(z.entries().size() == want.size());
    for (std::size_t i = 0; i < want.size() && i < z.entries().size(); ++i) {
        const auto& e = z.entries()[i];
        CHECK(e.name == want[i].name);
        CHECK(e.directory == want[i].directory);
        CHECK(e.size == want[i].bytes.size());
        std::vector<std::uint8_t> out;
        bool unsupported = true;
        const bool ok = z.extract(i, out, err, kMember, &unsupported);
        if (!ok || out != want[i].bytes)
            std::fprintf(stderr, "FAIL %s: member %s: %s\n", method, want[i].name, ok ? "wrong bytes" : err.c_str());
        CHECK(ok && out == want[i].bytes && !unsupported);
    }
    // Out of order, and twice: the block cache serves any member again.
    std::vector<std::uint8_t> again;
    CHECK(z.extract(2, again, err, kMember) && again == want[2].bytes);
    CHECK(z.extract(0, again, err, kMember) && again == want[0].bytes);
    CHECK(!z.extract(want.size(), again, err, kMember));
}

} // namespace

int main()
{
    // 1. The methods the LZMA SDK decodes.
    readsEverything("LZMA", pom2test::kSevenZipLzma);
    readsEverything("LZMA2", pom2test::kSevenZipLzma2);
    readsEverything("PPMd", pom2test::kSevenZipPpmd);

    // The signature, and only it.
    CHECK(!pom2::isSevenZip({ '7', 'z', 0xBC, 0xAF, 0x27 }));
    CHECK(!pom2::isSevenZip({ 'P', 'K', 3, 4, 0, 0, 0, 0 }));
    {
        pom2::SevenZipArchive z;
        std::string err;
        CHECK(!z.open({ 'P', 'K', 3, 4, 0, 0, 0, 0 }, err, kBlock) && !err.empty());
        CHECK(z.entries().empty());
    }

    // 2. A method it does not: the directory reads, every member is
    //    refused as unsupported — not as damaged, which would send the
    //    player to download a good archive again.
    {
        pom2::SevenZipArchive z;
        std::string err;
        CHECK(z.open(pom2test::kSevenZipBzip2, err, kBlock));
        for (std::size_t i = 0; i < 3 && i < z.entries().size(); ++i) {
            std::vector<std::uint8_t> out;
            bool unsupported = false;
            CHECK(!z.extract(i, out, err, kMember, &unsupported));
            CHECK(unsupported);
            CHECK(err.find("method") != std::string::npos);
            CHECK(out.empty());
        }
    }

    // 3. The ceilings, each refused before anything is decoded.
    {
        pom2::SevenZipArchive z;
        std::string err;
        // The solid block holds 286 725 bytes: a 100 000-byte ceiling refuses it.
        CHECK(!z.open(pom2test::kSevenZipLzma2, err, 100000));
        CHECK(err.find("more than") != std::string::npos);
        CHECK(z.entries().empty());
        // Four names, three allowed.
        CHECK(!z.open(pom2test::kSevenZipLzma2, err, kBlock, 3));
        CHECK(err.find("too many") != std::string::npos);
        // A member above what the caller takes.
        CHECK(z.open(pom2test::kSevenZipLzma2, err, kBlock));
        std::vector<std::uint8_t> out;
        CHECK(!z.extract(1, out, err, 1000));
        CHECK(err.find("too large") != std::string::npos);
        CHECK(z.extract(0, out, err, 1000) && out.size() == 5);
    }

    // 4. Damage. A flip in the packed data, the header or the signature
    //    header must come back as a refusal (or, where the byte is slack, as
    //    the right bytes) — never a crash or a read past the buffer.
    //    Every byte of the LZMA2 archive; every fifth of the PPMd one, whose
    //    16 MiB model is built afresh for each try (seconds under ASan).
    for (const auto* fixture : { &pom2test::kSevenZipLzma2, &pom2test::kSevenZipPpmd }) {
        const auto want = expected();
        const std::size_t stride = fixture == &pom2test::kSevenZipPpmd ? 5 : 1;
        std::size_t refused = 0, tried = 0;
        for (std::size_t at = 0; at < fixture->size(); at += stride) {
            ++tried;
            auto bad = *fixture;
            bad[at] ^= 0x5A;
            pom2::SevenZipArchive z;
            std::string err;
            if (!z.open(bad, err, kBlock)) { ++refused; continue; }
            for (std::size_t i = 0; i < z.entries().size(); ++i) {
                std::vector<std::uint8_t> out;
                if (!z.extract(i, out, err, kMember)) { ++refused; continue; }
                // Read without complaint: then it is what was packed.
                if (i < want.size() && z.entries()[i].name == want[i].name) CHECK(out == want[i].bytes);
            }
        }
        CHECK(refused > tried / 2);
        for (std::size_t cut = 0; cut < fixture->size(); cut += 7) {
            const std::vector<std::uint8_t> shorter(fixture->begin(), fixture->begin() + long(cut));
            pom2::SevenZipArchive z;
            std::string err;
            CHECK(!z.open(shorter, err, kBlock));
        }
    }

    if (g_fail) {
        std::fprintf(stderr, "seven_zip_archive: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("seven_zip_archive: PASS\n");
    return 0;
}
