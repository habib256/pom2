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

// RomFetch — the RetroBIOS mapping must stay a subset of what POM2
// actually probes. A destRel that is not in SystemProfile / RomCatalog /
// CharRomCatalog would download a file the ROM Status panel never shows
// and that no card ever opens. The planner is the other half: present
// files are skipped, missing ones are queued, and the source URL is the
// one the panel quotes.

#include "ResourcePaths.h"
#include "RomFetch.h"
#include "ZipMember.h"

// Only to build a DEFLATED member for the in-process reader to undo.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
#include "RomCatalog.h"
#include "CharRomCatalog.h"
#include "SystemProfile.h"

#include <algorithm>
#include <iterator>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

std::uint32_t crc32Of(const std::vector<std::uint8_t>& b)
{
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint8_t x : b) {
        c ^= x;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

struct ZipEntry {
    std::string               name;
    std::vector<std::uint8_t> plain;
    int                       method;   // 0 stored, 8 deflate, else as-is
};

/// A minimal but well-formed PKZIP archive.
std::vector<std::uint8_t> buildZip(const std::vector<ZipEntry>& entries)
{
    std::vector<std::uint8_t> out, cdir;
    auto p16 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        v.push_back(static_cast<std::uint8_t>(x));
        v.push_back(static_cast<std::uint8_t>(x >> 8));
    };
    auto p32 = [&](std::vector<std::uint8_t>& v, std::uint32_t x) {
        p16(v, x & 0xFFFF); p16(v, x >> 16);
    };
    for (const auto& e : entries) {
        std::vector<std::uint8_t> packed = e.plain;
        if (e.method == 8) {
            int zlen = 0;
            std::vector<std::uint8_t> in = e.plain;
            unsigned char* z = stbi_zlib_compress(in.data(), static_cast<int>(in.size()), &zlen, 8);
            // zlib stream -> raw deflate: drop the 2-byte header and Adler-32.
            packed.assign(z + 2, z + zlen - 4);
            STBIW_FREE(z);
        }
        const std::uint32_t off = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32Of(e.plain);
        p32(out, 0x04034B50u); p16(out, 20); p16(out, 0);
        p16(out, static_cast<std::uint32_t>(e.method)); p16(out, 0); p16(out, 0);
        p32(out, crc); p32(out, static_cast<std::uint32_t>(packed.size()));
        p32(out, static_cast<std::uint32_t>(e.plain.size()));
        p16(out, static_cast<std::uint32_t>(e.name.size())); p16(out, 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), packed.begin(), packed.end());

        p32(cdir, 0x02014B50u); p16(cdir, 20); p16(cdir, 20); p16(cdir, 0);
        p16(cdir, static_cast<std::uint32_t>(e.method)); p16(cdir, 0); p16(cdir, 0);
        p32(cdir, crc); p32(cdir, static_cast<std::uint32_t>(packed.size()));
        p32(cdir, static_cast<std::uint32_t>(e.plain.size()));
        p16(cdir, static_cast<std::uint32_t>(e.name.size()));
        p16(cdir, 0); p16(cdir, 0); p16(cdir, 0); p16(cdir, 0); p32(cdir, 0);
        p32(cdir, off);
        cdir.insert(cdir.end(), e.name.begin(), e.name.end());
    }
    const std::uint32_t cdOff = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), cdir.begin(), cdir.end());
    p32(out, 0x06054B50u); p16(out, 0); p16(out, 0);
    p16(out, static_cast<std::uint32_t>(entries.size()));
    p16(out, static_cast<std::uint32_t>(entries.size()));
    p32(out, static_cast<std::uint32_t>(cdir.size())); p32(out, cdOff); p16(out, 0);
    return out;
}

std::set<std::string> knownDests()
{
    std::set<std::string> s;
    for (pom2::SystemProfile p : pom2::allProfiles()) {
        const auto& cfg = pom2::profileConfig(p);
        for (const auto& c : cfg.romProbeOrder) s.insert(c);
        for (const auto& c : cfg.charRomProbeOrder) s.insert(c);
    }
    for (const auto& e : pom2::charRomCatalog()) {
        if (e.path && *e.path) s.insert(e.path);
    }
    for (const auto& e : pom2::romCatalog()) {
        for (const char* c : e.candidates) s.insert(c);
    }
    // The panel and the card must agree on what counts as present: ClockCard
    // probes the upstream markadev filename too, and a dump under that name
    // used to read "missing" in ROM Status while the card was using it.
    assert(s.count("roms/Thunderware_REV_1.3_ROM_U9.bin") == 1);
    assert(s.count("roms/ae transwarp rom v1.4.bin") == 1);
    return s;
}

}  // namespace

int main()
{
    // Everything this test writes under the per-user data dir — the
    // roms-replaced backups below — must land in a scratch tree, not in the
    // user's real one. userDataDir() follows these, so point them away
    // BEFORE anything asks for it.
    const std::filesystem::path home =
        std::filesystem::temp_directory_path() / "pom2_rom_fetch_home";
    {
        std::error_code rec;
        std::filesystem::remove_all(home, rec);
        std::filesystem::create_directories(home, rec);
#ifdef _WIN32
        _putenv_s("LOCALAPPDATA", home.string().c_str());
        _putenv_s("APPDATA", home.string().c_str());
#else
        setenv("HOME", home.string().c_str(), 1);
        setenv("XDG_DATA_HOME", home.string().c_str(), 1);
#endif
    }
    // Where a download lands: the per-user roms/, the FIRST search root, so
    // a wrong file in a later root can never shadow it — and asking must not
    // create anything (the ROM Status tooltip asks on every hover).
    {
        namespace fs = std::filesystem;
        std::error_code dec;
        const fs::path want = pom2::userDataDir() / "roms";
        fs::remove_all(want, dec);
        const fs::path got = pom2::writableRomsDir();
        expect(pom2::userDataDirIsPerUser(), "the sandboxed data dir is per-user");
        expect(got == want, "downloads go to the per-user roms/ (got " +
                                got.string() + ")");
        expect(!fs::exists(want, dec), "asking for the directory created it");
    }

    // Romsets are unpacked in-process: the host fallback was `tar`, and GNU
    // tar does not read zip, so a Linux box without unzip failed every one.
    {
        std::vector<std::uint8_t> rom(12288);
        for (std::size_t i = 0; i < rom.size(); ++i)
            rom[i] = static_cast<std::uint8_t>((i * 7) ^ (i >> 5));
        const std::vector<std::uint8_t> small{ 'A', 'P', 'P', 'L', 'E' };
        const auto zip = buildZip({ { "341-0011.d0", rom, 8 },
                                    { "stored.bin", small, 0 },
                                    { "odd.bin", small, 12 } });
        std::vector<std::uint8_t> got;
        std::string zerr;
        bool unsupported = true;
        expect(pom2::readZipMember(zip, "341-0011.d0", got, zerr, 1 << 20, &unsupported)
                   && got == rom && !unsupported,
               "a deflated member unpacks in-process (" + zerr + ")");
        expect(pom2::readZipMember(zip, "stored.bin", got, zerr, 1 << 20) && got == small,
               "a stored member unpacks in-process");
        expect(!pom2::readZipMember(zip, "absent.bin", got, zerr, 1 << 20),
               "an absent member is an error");
        expect(!pom2::readZipMember(zip, "odd.bin", got, zerr, 1 << 20, &unsupported)
                   && unsupported,
               "an unknown method is flagged for the host tool");
        expect(!pom2::readZipMember(zip, "341-0011.d0", got, zerr, 1024),
               "the size cap applies before inflating");
        std::vector<std::uint8_t> damaged = zip;
        damaged[30 + 11 + 40] ^= 0xFF;          // inside the deflated payload
        expect(!pom2::readZipMember(damaged, "341-0011.d0", got, zerr, 1 << 20),
               "a damaged member is refused");
    }

    // Every catalogued card ROM says what its absence does, and the words
    // agree with the verdict (TODO G5-15: "missing" alone hid whether the
    // card still ran one level down).
    for (const auto& e : pom2::romCatalog()) {
        const std::string w = e.whenMissing ? e.whenMissing : "";
        const bool saysRefuses = w.find("refuses to plug") != std::string::npos ||
                                 w.find("cannot be plugged") != std::string::npos;
        if (saysRefuses)
            expect(e.effect == pom2::RomMissingEffect::Unavailable,
                   std::string("a card that refuses to plug is Unavailable: ") + e.name);
        if (w.rfind("Nothing", 0) == 0 || w.rfind("Not used", 0) == 0)
            expect(e.effect == pom2::RomMissingEffect::Unused,
                   std::string("an unread dump is Unused: ") + e.name);
    }

    const auto known = knownDests();
    const auto& cat  = pom2::romFetchCatalog();

    expect(!cat.empty(), "catalog is not empty");
    expect(std::string(pom2::kRetroBiosSourceUrl).find("Abdess/retrobios")
               != std::string::npos,
           "source URL names Abdess/retrobios");

    std::set<std::string> dests;
    for (const auto& e : cat) {
        expect(e.destRel && *e.destRel, "every entry has a destRel");
        expect(e.label && *e.label, std::string("label for ") +
               (e.destRel ? e.destRel : "?"));
        expect(e.url && *e.url, std::string("url for ") +
               (e.destRel ? e.destRel : "?"));
        expect(e.expectedSize > 0, std::string("size for ") +
               (e.destRel ? e.destRel : "?"));
        if (e.destRel) {
            expect(dests.insert(e.destRel).second,
                   std::string("unique destRel ") + e.destRel);
            expect(known.count(e.destRel) == 1,
                   std::string(e.destRel) +
                   " is a path POM2 actually probes");
            expect(std::string(e.destRel).rfind("roms/", 0) == 0,
                   std::string(e.destRel) + " lives under roms/");
        }
        if (e.url) {
            const std::string url(e.url);
            expect(url.rfind(pom2::kRetroBiosRawPrefix, 0) == 0,
                   std::string("url is under the RetroBIOS raw prefix: ") +
                   url);
        }
    }

    // RetroBIOS PR #75 (merged 2026-09-14) put the dumps POM2 used to have
    // no source for into the collection. Name them: a later edit that drops
    // one silently takes the ROM Status panel's Download button back to
    // "the //c firmware is your problem", which is exactly the state this
    // catalogue exists to end.
    {
        const char* added[] = {
            "roms/apple2c-32Kv0.rom", "roms/apple2c-16K.rom",
            "roms/apple2cp.rom", "roms/3420033a.256",
            "roms/liron.rom", "roms/341-0358-A.bin",
            "roms/thunderclock_u9_v1.3.bin", "roms/cffa20eec02.bin",
            "roms/ae_transwarp_1.4.bin",
            "roms/Videx Lower Case Chip ROM.bin",
            "roms/342-0274-a.e9", "roms/342-0326-a.f12",
            "roms/apple2e_char_fr.rom", "roms/apple2e_char_frca.rom",
            "roms/apple2e_char_uk.rom", "roms/apple2e_char_uk_unenh.rom",
            "roms/apple2e_char_de.rom", "roms/apple2e_char_de_improved.rom",
            "roms/apple2e_char_ft_blockascii.rom",
            // Same bytes as apple2e_char.rom / apple2e_char_us_unenh.rom,
            // but these are the names the char-ROM picker and the //e
            // Unenhanced profile actually probe.
            "roms/apple2e_char_us.rom", "roms/apple2e_char_2k.rom",
        };
        for (const char* d : added)
            expect(dests.count(d) == 1,
                   std::string(d) + " is served by the RetroBIOS catalogue");
    }

    // Only TWO entries still need a host unzip/tar: RetroBIOS publishes a
    // loose copy of everything else, so a machine without those tools can
    // still complete almost all of its romset. Name them — an entry that
    // quietly goes back to a zip costs every such machine that dump.
    {
        int zipped = 0;
        for (const auto& e : cat) {
            if (!e.zipMember) continue;
            ++zipped;
            const std::string d(e.destRel);
            expect(d == "roms/mouse_341-0270-c.bin" ||
                   d == "roms/apple2e_unenh.rom",
                   std::string("zip-sourced entries are the mouse slot eprom "
                               "and the unenhanced //e pair, not ") + d);
        }
        expect(zipped == 2, "exactly two entries unpack a zip");
        for (const auto& e : cat)
            expect(!e.zipConcat || e.zipMember,
                   std::string(e.destRel) +
                   " concat list needs a first zipMember");
    }

    // Bug hunt #10: the planner re-verified LOCAL files against the
    // catalogue's download size, and the II+ entry declared the 12 KB
    // six-chip image while roms/ ships a working 20 KB dump — so on a
    // complete tree "Download missing ROMs" listed apple2p.rom every launch
    // and overwrote a good dump with a different one. Both sizes are
    // accepted now, and the 20 KB one is what a fresh fetch installs.
    {
        const auto plan = pom2::romsToFetch();
        for (const auto* e : plan) {
            const std::string resolved = pom2::findResource(e->destRel);
            expect(resolved.empty(),
                   std::string("a ROM the tree ships is planned for download: ") + e->destRel);
        }
        if (!pom2::findResource("roms/apple2p.rom").empty()) {
            bool listed = false;
            for (const auto* e : plan) if (std::string(e->destRel) == "roms/apple2p.rom") listed = true;
            expect(!listed, "the shipped 20 KB II+ dump counts as present");
        }
    }

    // ── The planner's verdict on a dump already on disk ──────────────
    // The II+ entry fetches the 20 KB dump and ALSO accepts the 12 KB
    // six-chip image as present. The digests describe the 20 KB one, so they
    // cannot apply to the 12 KB alternate — checking them anyway re-opened
    // bug hunt #10 the day the entry gained a SHA-256: every 12 KB II+ tree
    // was flagged and "Download missing" replaced a firmware that boots
    // (bug hunt 2026-09-16).
    {
        const pom2::RomFetchEntry* ap = nullptr;
        for (const auto& e : cat)
            if (std::string(e.destRel) == "roms/apple2p.rom") ap = &e;
        expect(ap != nullptr, "the II+ entry exists");
        if (ap) {
            expect(ap->altPresentSize == 12288, "the II+ alternate is the 12 KB image");
            const std::vector<std::uint8_t> twelveK(12288, 0x4C);
            expect(pom2::localDumpAcceptable(*ap, twelveK),
                   "a 12 KB II+ firmware counts as present despite the 20 KB digest");
            const std::vector<std::uint8_t> wrong20K(ap->expectedSize, 0x00);
            expect(!pom2::localDumpAcceptable(*ap, wrong20K),
                   "a 20 KB file that is not the dump is still flagged");
            const std::vector<std::uint8_t> odd(1234, 0x00);
            expect(!pom2::localDumpAcceptable(*ap, odd),
                   "a file of neither size is flagged");
        }
    }

    // ── A replaced dump is kept, never destroyed ─────────────────────
    // The planner replaces a present file that is not the expected dump.
    // That is right for a damaged one and wrong for a legitimate variant
    // POM2 has no digest for, and it cannot tell them apart — so the old file
    // is copied aside first, and nothing is replaced if it cannot be
    // (bug hunt 2026-09-16).
    {
        namespace fs = std::filesystem;
        std::error_code rec;
        const fs::path roms = home / "tree" / "roms";
        fs::create_directories(roms, rec);
        const fs::path dest = roms / "variant.bin";
        const std::string oldBody = "A USER'S VARIANT";          // 16 bytes
        const std::vector<std::uint8_t> newBytes(16, 0xA9);
        const std::string newSha = pom2::sha256Hex(newBytes.data(), newBytes.size());
        const pom2::RomFetchEntry entry{
            "roms/variant.bin", "test variant", 16,
            "https://raw.githubusercontent.com/Abdess/retrobios/main/x",
            nullptr, nullptr, 0u, nullptr, newSha.c_str(), 0u };
        auto slurp = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        };
        auto put = [](const fs::path& p, const std::string& body) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << body;
        };

        // A download that is not the dump: refused, nothing touched.
        put(dest, oldBody);
        std::string err, backup;
        const std::vector<std::uint8_t> bogus(16, 0x00);
        expect(!pom2::installFetchedRom(dest, entry, bogus, err, backup),
               "a download failing its digest is refused");
        expect(slurp(dest) == oldBody && backup.empty(),
               "a refused download leaves the present file alone, with no backup");

        // The right dump over a different file: installed, old one kept.
        err.clear();
        expect(pom2::installFetchedRom(dest, entry, newBytes, err, backup),
               std::string("the expected dump installs over a variant: ") + err);
        expect(!backup.empty(), "the replaced file's backup is reported");
        expect(slurp(dest) == std::string(newBytes.begin(), newBytes.end()),
               "the new dump is in place");
        expect(!backup.empty() && slurp(backup) == oldBody,
               "the backup holds the user's old file, byte for byte");
        expect(!backup.empty() &&
                   fs::path(backup).parent_path().filename() == "roms-replaced" &&
                   std::string(backup).find(roms.string()) == std::string::npos,
               "the backup lives in roms-replaced, outside roms/ (it would ship)");

        // The same bytes again (a second POM2 fetching at the same time, or a
        // re-run): nothing replaced, nothing backed up — reporting a copy of
        // an identical file as "replaced a different file" was the race's
        // other symptom (bug hunt 2026-09-16).
        {
            err.clear();
            std::string again;
            const auto keptBefore = std::distance(fs::directory_iterator(fs::path(backup).parent_path()),
                                                  fs::directory_iterator());
            expect(pom2::installFetchedRom(dest, entry, newBytes, err, again),
                   "re-installing identical bytes succeeds");
            expect(again.empty(), "an identical file is not reported as replaced");
            expect(std::distance(fs::directory_iterator(fs::path(backup).parent_path()),
                                 fs::directory_iterator()) == keptBefore,
                   "an identical file is not backed up");
        }

        // No way to keep the old file: no replacement at all.
        put(dest, oldBody);
        const fs::path keepDir = pom2::userDataDir() / "roms-replaced";
        fs::remove_all(keepDir, rec);
        put(keepDir, "a FILE where the backup directory should be");
        err.clear(); backup.clear();
        expect(!pom2::installFetchedRom(dest, entry, newBytes, err, backup),
               "an impossible backup refuses the replacement");
        expect(slurp(dest) == oldBody,
               "the present file survives a failed backup");
        fs::remove(keepDir, rec);
    }

    // Planner: nothing present → every entry; everything present → none;
    // a single hit removes just that dest.
    {
        const auto all = pom2::romsToFetch([](const char*) { return false; });
        expect(all.size() == cat.size(), "missing-everything queues the catalog");
    }
    {
        const auto none = pom2::romsToFetch([](const char*) { return true; });
        expect(none.empty(), "present-everything queues nothing");
    }
    {
        const char* keep = cat.front().destRel;
        const auto rest = pom2::romsToFetch([&](const char* destRel) {
            return destRel && keep && std::string(destRel) == keep;
        });
        expect(rest.size() == cat.size() - 1,
               "one present dest is skipped and only that one");
        for (const auto* e : rest)
            expect(std::string(e->destRel) != keep,
                   "skipped dest does not reappear");
    }

    // Size was the ONLY gate: a download that was the right length and the
    // wrong file (a mirror serving another revision, an error page padded
    // out) was installed over the user's roms/ and surfaced days later as
    // "it doesn't boot". Entries POM2 has a documented reference dump for now
    // carry that CRC and it is checked before the file is published.
    // (Bug hunt 2026-09-06 #H9.)
    {
        int withCrc = 0;
        for (const auto& e : cat) {
            if (!e.expectedCrc) continue;
            ++withCrc;
            expect(e.crcLabel && *e.crcLabel,
                   std::string(e.destRel ? e.destRel : "?") +
                   " names the dump its CRC identifies");
        }
        expect(withCrc >= 2,
               "the entries with a documented reference dump carry its CRC32");
        // The two tables must not disagree about the same part: a CRC in
        // RomCatalog and a different one here means one of them is wrong,
        // and the one here is the gate that installs the file.
        for (const auto& e : cat) {
            if (!e.expectedCrc) continue;
            for (const auto& c : pom2::romCatalog()) {
                if (!c.knownCrc) continue;
                bool same = false;
                for (const char* cand : c.candidates)
                    if (std::string(cand) == e.destRel) same = true;
                if (same)
                    expect(c.knownCrc == e.expectedCrc,
                           std::string(e.destRel) +
                           " CRC32 agrees with RomCatalog");
            }
        }
    }

    // CRC32 detects DAMAGE. It does not identify a file: 32 non-cryptographic
    // bits are collidable on purpose, so "right size, right CRC" was never
    // evidence that a download is the dump POM2 vouches for — and the file is
    // about to be installed as the machine's firmware. Every entry whose dump
    // also ships in the repository now carries its SHA-256, verified before
    // install. (Bug hunt 2026-09-07 #S10.)
    {
        int withSha = 0;
        for (const auto& e : cat) {
            if (!e.expectedSha256) continue;
            ++withSha;
            const std::string d(e.expectedSha256);
            expect(d.size() == 64,
                   std::string(e.destRel ? e.destRel : "?") +
                   " SHA-256 is 64 hex characters");
            expect(d.find_first_not_of("0123456789abcdef") == std::string::npos,
                   std::string(e.destRel ? e.destRel : "?") +
                   " SHA-256 is lowercase hex");
        }
        // No exception left. The II+ entry was the one POM2 had no digest
        // for, because RetroBIOS served a DIFFERENT image (12 KB, six
        // chips) than the 20 KB dump in roms/; PR #75 published the 20 KB
        // one, so every entry is now gated on a digest of the exact bytes
        // POM2 ships. (was hunt #4 #31l.)
        expect(withSha == static_cast<int>(cat.size()),
               "every catalog entry carries a SHA-256");
    }

    // The digest law itself, against the FIPS 180-4 examples — a hand-rolled
    // SHA-256 that is subtly wrong would make the gate above reject every
    // legitimate download instead of protecting anything.
    {
        expect(pom2::sha256Hex(nullptr, 0) ==
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "SHA-256 of the empty string");
        const char* abc = "abc";
        expect(pom2::sha256Hex(reinterpret_cast<const std::uint8_t*>(abc), 3) ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "SHA-256 of \"abc\"");
        const char* two =
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        expect(pom2::sha256Hex(reinterpret_cast<const std::uint8_t*>(two), 56) ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
               "SHA-256 of the two-block example");
    }

    // The curl argv is policy, not decoration: `-fsSL` alone followed an
    // HTTPS→HTTP redirect without a word, and the 64 MB ceiling was applied
    // when READING the file back — after it had landed on the user's disk.
    {
        const auto args = pom2::curlDownloadArgs("https://example.invalid/x.rom",
                                                 "/tmp/x.part");
        auto has = [&](const std::string& v) {
            return std::find(args.begin(), args.end(), v) != args.end();
        };
        auto valueAfter = [&](const std::string& flag) -> std::string {
            for (std::size_t i = 0; i + 1 < args.size(); ++i)
                if (args[i] == flag) return args[i + 1];
            return {};
        };
        expect(valueAfter("--proto") == "=https",
               "curl is pinned to https for the first request");
        expect(valueAfter("--proto-redir") == "=https",
               "curl is pinned to https across every redirect");
        expect(!valueAfter("--max-filesize").empty(),
               "curl refuses an oversized body before it lands on disk");
        expect(has("https://example.invalid/x.rom") && has("/tmp/x.part"),
               "the url and the output path are passed through");
    }

    // Zip-bomb gate. The central directory states every member's uncompressed
    // size, so the cap can be enforced BEFORE unzip runs — afterwards the disk
    // is already full. Synthesise the two records the parser reads.
    {
        auto le16 = [](std::vector<std::uint8_t>& v, unsigned x) {
            v.push_back(static_cast<std::uint8_t>(x & 0xFF));
            v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
        };
        auto le32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
            for (int i = 0; i < 4; ++i)
                v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
        };
        auto makeZip = [&](std::uint32_t uncompressed) {
            std::vector<std::uint8_t> z;
            const std::size_t cdOff = 0;
            le32(z, 0x02014B50u);                 // central directory header
            for (int i = 0; i < 20; ++i) z.push_back(0);   // ... up to +24
            le32(z, uncompressed);                // +24 uncompressed size
            le32(z, 0);                           // +28 name/extra/comment lens
            le16(z, 0); le16(z, 0);               // (name=0, extra=0)
            le16(z, 0);                           // +32 comment len
            while (z.size() < 46) z.push_back(0);
            const std::size_t eocdOff = z.size();
            (void)eocdOff;
            le32(z, 0x06054B50u);                 // EOCD
            le16(z, 0); le16(z, 0);
            le16(z, 1); le16(z, 1);               // one entry
            le32(z, 46);                          // cd size
            le32(z, static_cast<std::uint32_t>(cdOff));
            le16(z, 0);                           // comment length
            return z;
        };

        std::uintmax_t total = 0;
        std::string err;
        expect(pom2::zipUnpackedSizeWithinCap(makeZip(4096), total, err),
               "a 4 KB member is accepted");
        expect(total == 4096, "the uncompressed total is read back");
        expect(!pom2::zipUnpackedSizeWithinCap(
                   makeZip(static_cast<std::uint32_t>(
                       pom2::kMaxUnpackedZipBytes + 1)), total, err),
               "a member past the cap is refused before unzip runs");
        std::vector<std::uint8_t> garbage(64, 0x41);
        expect(!pom2::zipUnpackedSizeWithinCap(garbage, total, err),
               "a file with no central directory is refused");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("rom_fetch: %zu catalog entries, planner ok\n", cat.size());
    return 0;
}
