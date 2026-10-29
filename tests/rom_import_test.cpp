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

// RomImport — a dump the user already has is identified by its SHA-256,
// whatever it is called, and lands under the name POM2 probes. Pinned here:
// identification of the tracked roms/ dumps, the install into the per-user
// roms/ (replacing a wrong file, which is kept), "already present" when the
// machine already loads those bytes, MAME romsets including the //e
// Unenhanced two-chip image, and the refusal of anything unidentified.
// Runs from the repo root, against a sandboxed HOME.

#include "ResourcePaths.h"
#include "RomFetch.h"
#include "RomImport.h"
#include "ZipMember.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

std::vector<std::uint8_t> readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                     std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const std::vector<std::uint8_t>& b)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
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

/// A stored-only PKZIP archive — enough for the importer, which shares the
/// deflate path with RomFetch (pinned by rom_fetch).
std::vector<std::uint8_t> buildZip(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries)
{
    std::vector<std::uint8_t> out, cdir;
    auto p16 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        v.push_back(static_cast<std::uint8_t>(x));
        v.push_back(static_cast<std::uint8_t>(x >> 8));
    };
    auto p32 = [&](std::vector<std::uint8_t>& v, std::uint32_t x) {
        p16(v, x & 0xFFFF); p16(v, x >> 16);
    };
    for (const auto& [name, plain] : entries) {
        const std::uint32_t off = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32Of(plain);
        const auto sz = static_cast<std::uint32_t>(plain.size());
        p32(out, 0x04034B50u); p16(out, 20); p16(out, 0); p16(out, 0);
        p16(out, 0); p16(out, 0); p32(out, crc); p32(out, sz); p32(out, sz);
        p16(out, static_cast<std::uint32_t>(name.size())); p16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), plain.begin(), plain.end());

        p32(cdir, 0x02014B50u); p16(cdir, 20); p16(cdir, 20); p16(cdir, 0);
        p16(cdir, 0); p16(cdir, 0); p16(cdir, 0); p32(cdir, crc);
        p32(cdir, sz); p32(cdir, sz);
        p16(cdir, static_cast<std::uint32_t>(name.size()));
        p16(cdir, 0); p16(cdir, 0); p16(cdir, 0); p16(cdir, 0); p32(cdir, 0);
        p32(cdir, off);
        cdir.insert(cdir.end(), name.begin(), name.end());
    }
    const std::uint32_t cdOff = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), cdir.begin(), cdir.end());
    p32(out, 0x06054B50u); p16(out, 0); p16(out, 0);
    p16(out, static_cast<std::uint32_t>(entries.size()));
    p16(out, static_cast<std::uint32_t>(entries.size()));
    p32(out, static_cast<std::uint32_t>(cdir.size())); p32(out, cdOff); p16(out, 0);
    return out;
}

const pom2::RomImportItem* itemFor(const pom2::RomImportResult& r, const char* destRel)
{
    for (const auto& it : r.items)
        if (it.destRel == destRel) return &it;
    return nullptr;
}

}  // namespace

int main()
{
    const fs::path home = fs::temp_directory_path() / "pom2_rom_import_home";
    {
        std::error_code ec;
        fs::remove_all(home, ec);
        fs::create_directories(home, ec);
#ifdef _WIN32
        _putenv_s("LOCALAPPDATA", home.string().c_str());
        _putenv_s("APPDATA", home.string().c_str());
#else
        setenv("HOME", home.string().c_str(), 1);
        setenv("XDG_DATA_HOME", home.string().c_str(), 1);
#endif
    }
    // Explicit, not writableRomsDir(): run from the repo root that is the
    // tracked roms/, and this test writes wrong bytes on purpose.
    const fs::path dest = pom2::userDataDir() / "roms";
    const fs::path scratch = home / "drop";
    fs::create_directories(scratch);

    // ── Identification: every tracked dump the catalogue names is itself.
    {
        int checked = 0;
        for (const auto& e : pom2::romFetchCatalog()) {
            const fs::path local = e.destRel;
            if (!fs::exists(local)) continue;
            const auto bytes = readFile(local);
            if (e.altPresentSize && bytes.size() == e.altPresentSize) continue;
            const auto got = pom2::identifyRom(bytes);
            expect(std::find(got.begin(), got.end(), &e) != got.end(),
                   std::string("identifyRom(") + e.destRel + ")");
            ++checked;
        }
        expect(checked >= 40, "most tracked dumps were identified (" +
                                  std::to_string(checked) + ")");
        std::vector<std::uint8_t> junk(256, 0xA5);
        expect(pom2::identifyRom(junk).empty(), "256 bytes of $A5 is no ROM");
        expect(pom2::identifyRom({}).empty(), "an empty file is no ROM");
    }

    // ── A renamed copy of a dump the machine already loads: nothing written.
    {
        const fs::path f = scratch / "Apple Disk II 16-sector.bin";
        writeFile(f, readFile("roms/disk2.rom"));
        const auto r = pom2::importRoms({ f }, dest);
        const auto* it = itemFor(r, "roms/disk2.rom");
        expect(it && it->alreadyPresent, "disk2 copy reported already present");
        expect(r.installed == 0 && r.alreadyPresent == 1, "nothing installed: " + r.summary);
        expect(!fs::exists(dest / "disk2.rom"), "already-present dump was not copied");
    }

    // ── A wrong file in the FIRST search root: the import replaces it, and
    //    keeps it in roms-replaced/.
    {
        fs::create_directories(dest);
        const std::vector<std::uint8_t> wrong(4096, 0x00);
        writeFile(dest / "liron.rom", wrong);
        const fs::path f = scratch / "LIRONALL.bin";
        const auto liron = readFile("roms/liron.rom");
        writeFile(f, liron);
        const auto r = pom2::importRoms({ f }, dest);
        const auto* it = itemFor(r, "roms/liron.rom");
        expect(it && it->error.empty() && !it->alreadyPresent,
               "liron imported over the wrong file: " + r.summary);
        expect(readFile(dest / "liron.rom") == liron, "liron.rom now holds the dump");
        expect(it && !it->backup.empty() && fs::exists(it->backup) &&
                   readFile(it->backup) == wrong,
               "the replaced file is kept");
        std::error_code eq;
        expect(fs::equivalent(pom2::findResource("roms/liron.rom"),
                              dest / "liron.rom", eq),
               "the machine now resolves the imported copy");
    }

    // ── A MAME romset: the //e Unenhanced is assembled from its two chips,
    //    a member in a subdirectory is identified by content, and members
    //    that match nothing are ignored.
    {
        const auto unenh = readFile("roms/apple2e_unenh.rom");
        const auto mouse = readFile("roms/mouse_341-0270-c.bin");
        expect(unenh.size() == 16384, "tracked apple2e_unenh.rom is 16 KB");
        std::vector<std::uint8_t> cd(unenh.begin(), unenh.begin() + 8192);
        std::vector<std::uint8_t> ef(unenh.begin() + 8192, unenh.end());
        // Wrong file first, so the install path (not "already present") runs.
        writeFile(dest / "apple2e_unenh.rom", std::vector<std::uint8_t>(16384, 0xEA));
        const auto zip = buildZip({ { "342-0135-b.64", cd },
                                    { "342-0134-a.64", ef },
                                    { "sub/", {} },
                                    { "sub/341-0270-c.4b", mouse },
                                    { "readme.txt", { 'h', 'i' } } });
        std::vector<std::string> names;
        std::string err;
        expect(pom2::listZipMembers(zip, names, err) && names.size() == 5,
               "listZipMembers sees all five members " + err);
        const fs::path f = scratch / "apple2e.zip";
        writeFile(f, zip);
        expect(pom2::romImportCandidate(f), "a .zip is an import candidate");
        const auto r = pom2::importRoms({ f }, dest);
        const auto* u = itemFor(r, "roms/apple2e_unenh.rom");
        expect(u && u->error.empty() && !u->alreadyPresent,
               "//e Unenhanced assembled from its chips: " + r.summary);
        expect(readFile(dest / "apple2e_unenh.rom") == unenh, "assembled bytes installed");
        const auto* m = itemFor(r, "roms/mouse_341-0270-c.bin");
        expect(m && m->alreadyPresent, "mouse EPROM found in a subdirectory");
        expect(r.items.size() == 2, "only the two catalogued dumps: " + r.summary);
    }

    // ── Nothing identifiable: nothing installed, every input reported.
    {
        const fs::path a = scratch / "mystery.rom";
        writeFile(a, std::vector<std::uint8_t>(2048, 0x60));
        const fs::path big = scratch / "huge.bin";
        writeFile(big, std::vector<std::uint8_t>(pom2::kMaxRomImportFileBytes + 1, 0));
        expect(!pom2::romImportCandidate(big), "an oversized file is not a candidate");
        expect(!pom2::romImportCandidate(scratch), "a directory is not a candidate");
        const auto r = pom2::importRoms({ a }, dest);
        expect(r.items.empty() && r.unrecognised.size() == 1, "mystery.rom refused");
        expect(!fs::exists(dest / "mystery.rom"), "an unknown file is never installed");
    }

    // ── The same dump twice in one drop is installed once.
    {
        const fs::path a = scratch / "ssc1.bin";
        const fs::path b = scratch / "ssc2.bin";
        const auto ssc = readFile("roms/ssc_341-0065-a.bin");
        writeFile(a, ssc);
        writeFile(b, ssc);
        const auto r = pom2::importRoms({ a, b }, dest);
        expect(r.items.size() == 1, "duplicate collapsed: " + r.summary);
    }

    std::error_code ec;
    fs::remove_all(home, ec);
    if (failures) {
        std::printf("rom_import: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("rom_import: OK\n");
    return 0;
}
