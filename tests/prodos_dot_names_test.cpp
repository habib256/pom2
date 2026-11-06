// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ProDOS host folder and a guest name that starts with a period.
// The escape itself is HostFileTag.h's; this pins what the folder does
// with it, both ways. (Its own file: prodos_volume_smoke_test.cpp is at
// the 2000-line watch threshold.)

#include "ProDOSVolume.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBlockBytes = 512;

std::uint16_t rd16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

void writeFile(const fs::path& path, const std::vector<std::uint8_t>& data)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
}

fs::path makeTempDir(const std::string& tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() /
        ("pom2_dotnames_" + tag + "_" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

// ── A guest file whose name starts with a period ────────────────────────
//
// A2osX keeps `/ROOT/.PROFILE` (its 0.95 release image has it: the one entry
// of 238 that does not begin with a letter). The decode refused the name —
// rightly, as a host component — and counted the file as lost, so EVERY save
// of a folder holding one failed, naming a file the user could not rename.
// It now leaves under the CiderPress escape, `%2ePROFILE#040000`, and comes
// back as `.PROFILE`.
void testLeadingDotNamesRoundTripEscaped()
{
    const fs::path src = makeTempDir("dot_src");
    writeFile(src / "keep.txt",    {'k'});
    writeFile(src / "profile.txt", {'p'});
    fs::create_directories(src / "conf");
    writeFile(src / "conf" / "inner.txt", {'i'});

    // The entry named `name` in the directory starting at `first`, or null.
    auto findEntry = [](std::vector<std::uint8_t>& img, std::size_t first,
                        const std::string& name) -> std::uint8_t* {
        for (std::size_t b = first, guard = 0; b != 0 && guard < 64; ++guard) {
            std::uint8_t* blk = img.data() + b * kBlockBytes;
            for (int e = 0; e < 13; ++e) {
                std::uint8_t* ent = blk + 4 + e * 39;
                const std::uint8_t st = ent[0] >> 4;
                if (st == 0 || st >= 0xE) continue;
                if ((ent[0] & 0x0F) == name.size() &&
                    std::memcmp(ent + 1, name.data(), name.size()) == 0)
                    return ent;
            }
            b = rd16(blk + 2);
        }
        return nullptr;
    };
    auto rename = [](std::uint8_t* ent, const std::string& to) {
        ent[0] = static_cast<std::uint8_t>((ent[0] & 0xF0) | to.size());
        std::memset(ent + 1, 0, 15);
        std::memcpy(ent + 1, to.data(), to.size());
    };

    // The guest renames both, exactly as a system without the MLI's
    // leading-letter check can.
    std::vector<std::uint8_t> img;
    assert(pom2::buildVolumeFromFolder(src.string(), "HOST", img).ok);
    std::uint8_t* file = findEntry(img, 2, "PROFILE");
    std::uint8_t* sub  = findEntry(img, 2, "CONF");
    assert(file && sub && (sub[0] >> 4) == 0xD);
    rename(file, ".PROFILE");
    rename(sub,  ".CONF");

    const fs::path dst = makeTempDir("dot_dst");
    {
        const auto dr = pom2::decodeVolumeToFolder(img, dst.string());
        assert(dr.ok && "a guest dot-name must not fail the save");
        assert(dr.filesUnsaved == 0);
    }
    assert(fs::exists(dst / "%2ePROFILE#040000"));
    assert(fs::is_directory(dst / "%2eCONF"));
    assert(fs::exists(dst / "%2eCONF" / "INNER.txt"));
    assert(fs::exists(dst / "KEEP.txt"));
    // The fence stands: nothing the host would treat as hidden was created.
    auto countEntries = [&](bool& anyDotfile) {
        std::error_code ec;
        int n = 0;
        anyDotfile = false;
        for (const auto& e : fs::recursive_directory_iterator(dst, ec)) {
            ++n;
            if (e.path().filename().string().front() == '.') anyDotfile = true;
        }
        return n;
    };
    bool dotfile = false;
    const int entries = countEntries(dotfile);
    assert(entries == 4 && !dotfile);

    // Served again, the names are the guest's — and the round trip is closed:
    // nothing rewritten, nothing added, cycle after cycle.
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::vector<std::uint8_t> again;
        assert(pom2::buildVolumeFromFolder(dst.string(), "HOST", again).ok);
        std::uint8_t* f = findEntry(again, 2, ".PROFILE");
        std::uint8_t* d = findEntry(again, 2, ".CONF");
        assert(f && f[0x10] == 0x04 && rd16(f + 0x1F) == 0);
        assert(d && (d[0] >> 4) == 0xD);
        assert(findEntry(again, rd16(d + 0x11), "INNER"));
        assert(again[rd16(f + 0x11) * kBlockBytes] == 'p');
        const auto dr = pom2::decodeVolumeToFolder(again, dst.string());
        assert(dr.ok && dr.filesWritten == 0 && "nothing changed: nothing written");
        assert(countEntries(dotfile) == entries && !dotfile);
    }

    auto readOne = [&](const fs::path& leaf) {
        std::ifstream in(dst / leaf, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };
    // A guest edit lands IN the escaped file.
    {
        std::vector<std::uint8_t> again;
        assert(pom2::buildVolumeFromFolder(dst.string(), "HOST", again).ok);
        std::uint8_t* f = findEntry(again, 2, ".PROFILE");
        assert(f);
        again[rd16(f + 0x11) * kBlockBytes] = 'q';
        const auto dr = pom2::decodeVolumeToFolder(again, dst.string());
        assert(dr.ok && dr.filesWritten == 1);
        assert(readOne("%2ePROFILE#040000") == "q");
        assert(countEntries(dotfile) == entries && !dotfile);
    }
    // A type change RENAMES it; the escape and the new tag both hold.
    {
        std::vector<std::uint8_t> again;
        assert(pom2::buildVolumeFromFolder(dst.string(), "HOST", again).ok);
        std::uint8_t* f = findEntry(again, 2, ".PROFILE");
        assert(f);
        f[0x10] = 0x06;
        f[0x1F] = 0x00;
        f[0x20] = 0x20;
        const auto dr = pom2::decodeVolumeToFolder(again, dst.string());
        assert(dr.ok);
        assert(fs::exists(dst / "%2ePROFILE#062000"));
        assert(!fs::exists(dst / "%2ePROFILE#040000"));
        assert(readOne("%2ePROFILE#062000") == "q");
        assert(countEntries(dotfile) == entries && !dotfile);
    }

    // The escape counts only on a TAGGED file name (NAPS's own rule): a host
    // file somebody happens to call `%2eNOTES.txt` keeps the name it always
    // got, and `.` / `..` never come out of an escape.
    writeFile(dst / "%2eNOTES.txt", {'n'});
    writeFile(dst / "%2e.#040000",  {'d'});
    {
        std::vector<std::uint8_t> again;
        assert(pom2::buildVolumeFromFolder(dst.string(), "HOST", again).ok);
        assert(findEntry(again, 2, "A.2ENOTES"));
        assert(!findEntry(again, 2, ".NOTES"));
        assert(!findEntry(again, 2, "."));
        assert(!findEntry(again, 2, ".."));
        const auto dr = pom2::decodeVolumeToFolder(again, dst.string());
        assert(dr.ok && dr.filesWritten == 0);
    }

    // What has no host spelling still fails the save: a device name.
    assert(pom2::isProDOSLegalName(".PROFILE") &&
           !pom2::isHostSafeProDOSName(".PROFILE"));

    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
    std::printf("  ok: leading-dot names round-trip escaped\n");
}

}  // namespace

int main()
{
    testLeadingDotNamesRoundTripEscaped();
    std::printf("OK prodos_dot_names\n");
    return 0;
}
