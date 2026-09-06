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

#include "RomFetch.h"

#include "AtomicFileReplace.h"
#include "ChildProcess.h"
#include "Logger.h"
#include "Pom2Build.h"
#include "ResourcePaths.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace pom2 {

namespace {

const char* kApple2pChips[] = {
    "341-0012.d8",
    "341-0013.e0",
    "341-0014.e8",
    "341-0015.f0",
    "341-0020-00.f8",
    nullptr,
};

// Loose dumps first; MAME zips only when RetroBIOS has no standalone file
// under the name POM2 probes. Spaces in GitHub paths are already encoded.
const std::vector<RomFetchEntry>& catalogStorage()
{
    static const std::vector<RomFetchEntry> k = {
        // ── Machine firmware ──────────────────────────────────────────
        { "roms/apple2o.rom", "Apple ][ Original", 12288,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2o.rom",
          nullptr, nullptr, 0u, nullptr },
        { "roms/apple2.rom", "Apple ][ generic fallback", 12288,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2.rom",
          nullptr, nullptr, 0u, nullptr },
        { "roms/apple2p.rom", "Apple ][+ (six 2 KB chips)", 12288,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2p.zip",
          "341-0011.d0", kApple2pChips, 0u, nullptr },
        { "roms/apple2e.rom", "Apple //e Enhanced", 32768,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e.rom",
          nullptr, nullptr, 0u, nullptr },
        { "roms/apple2e_unenh.rom", "Apple //e Unenhanced", 16384,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/AppleIIe.rom",
          nullptr, nullptr, 0u, nullptr },

        // ── Character generators ──────────────────────────────────────
        { "roms/apple2_char.rom", "II/II+ character ROM", 2048,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2-character.rom",
          nullptr, nullptr, 0u, nullptr },
        { "roms/apple2e_char.rom", "//e Enhanced character ROM", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e-character.rom",
          nullptr, nullptr, 0u, nullptr },
        { "roms/apple2e_char_us_unenh.rom", "//e Unenhanced character ROM", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2eu-character.rom",
          nullptr, nullptr, 0u, nullptr },

        // ── Disk II ───────────────────────────────────────────────────
        { "roms/disk2.rom", "Disk II boot PROM 16-sector", 256,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/DiskII/boot-16.rom",
          nullptr, nullptr, 0u, nullptr },
        { "roms/diskii_p6.rom", "Disk II P6 sequencer 16-sector", 256,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/DiskII/state-machine-16.rom",
          nullptr, nullptr, 0u, nullptr },
        { "roms/disk2_13.rom", "Disk II boot PROM 13-sector", 256,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/DiskII/boot-13.rom",
          nullptr, nullptr, 0u, nullptr },
        { "roms/diskii_p6_13.rom", "Disk II P6 sequencer 13-sector", 256,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/DiskII/state-machine-13.rom",
          nullptr, nullptr, 0u, nullptr },

        // ── Cards (MAME romsets sitting next to the Apple II bios/) ──
        { "roms/cffa20ee02.bin", "CFFA 2.0 (6502)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/a2cffa02.zip",
          "cffa20ee02.bin", nullptr,
          0x3ECAFCE5u, "dreher.net Run6_CDROM.zip" },
        { "roms/mouse_341-0270-c.bin", "Mouse card slot EPROM", 2048,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Arcade/MAME/a2mouse.zip",
          "341-0270-c.4b", nullptr, 0u, nullptr },
        { "roms/mouse_341-0269.bin", "Mouse card 6805 MCU", 2048,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Arcade/MAME/a2mouse.zip",
          "341-0269.2b", nullptr, 0u, nullptr },
        { "roms/grappler_plus.bin", "Grappler+ EPROM 3.2", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Arcade/MAME/a2grapplerplus.zip",
          "3.2.u9", nullptr, 0u, nullptr },
        { "roms/341-0438-a.bin", "Apple 3.5\" SuperDrive controller", 32768,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Arcade/MAME/a2superdrive.zip",
          "341-0438-a.bin", nullptr,
          0xC73FF25Bu, "MAME a2superdrive" },
    };
    return k;
}

bool dirIsWritable(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
#if defined(_WIN32)
    const fs::path probe = dir / ".pom2_write_probe";
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (!f) return false;
    }
    fs::remove(probe, ec);
    return true;
#else
    return ::access(dir.string().c_str(), W_OK) == 0;
#endif
}

bool runHostTool(const std::string& exe,
                 const std::vector<std::string>& args,
                 const std::string& cwd,
                 int timeoutMs,
                 std::string& err,
                 const RomFetchCancel& cancelled = {})
{
#if !POM2_HAS_CHILD_PROCESS
    (void)exe; (void)args; (void)cwd; (void)timeoutMs; (void)cancelled;
    err = "helper programs are not available in this build";
    return false;
#else
    ChildProcess p;
    if (!p.start(exe, args, cwd, err)) return false;
    const auto t0 = std::chrono::steady_clock::now();
    while (p.isRunning()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed > timeoutMs) {
            p.stop(500);
            err = exe + " timed out";
            return false;
        }
        // The 30 ms poll is also where a quit gets answered: without this the
        // panel's destructor waited out curl's full --max-time.
        if (cancelled && cancelled()) {
            p.stop(500);
            err = exe + " cancelled";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (p.lastExitCode() != 0) {
        err = exe + " exited " + std::to_string(p.lastExitCode());
        return false;
    }
    return true;
#endif
}

bool downloadUrl(const std::string& url, const fs::path& dest, std::string& err,
                 const RomFetchCancel& cancelled = {})
{
    const std::string curl = ChildProcess::findOnPath("curl");
    if (curl.empty()) {
        err = "curl was not found on PATH — install it to fetch ROMs";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = dest.string() + ".part";
    fs::remove(tmp, ec);
    // -f fail on HTTP errors, -S show them, -L follow redirects, --retry
    // covers a flaky raw.githubusercontent.com hop.
    if (!runHostTool(curl,
                     { "-fsSL", "--retry", "2", "--max-time", "90",
                       "-o", tmp.string(), url },
                     {}, 120000, err, cancelled)) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        err = "could not publish download: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

fs::path extractedMemberPath(const fs::path& dir, const std::string& member)
{
    // unzip -j and tar both drop a member with slashes as its basename
    // when we ask them to flatten; we only ever extract flat names, but
    // keep the last component so a future nested member still resolves.
    const auto pos = member.find_last_of("/\\");
    const std::string base =
        (pos == std::string::npos) ? member : member.substr(pos + 1);
    return dir / base;
}

bool extractZipMember(const fs::path& zip, const std::string& member,
                      const fs::path& destDir, std::string& err)
{
    std::error_code ec;
    fs::create_directories(destDir, ec);

    const std::string unzip = ChildProcess::findOnPath("unzip");
    if (!unzip.empty()) {
        return runHostTool(unzip,
                           { "-o", "-q", "-j", zip.string(), member, "-d",
                             destDir.string() },
                           {}, 30000, err);
    }

    const std::string tar = ChildProcess::findOnPath("tar");
    if (!tar.empty()) {
        return runHostTool(tar,
                           { "-xf", zip.string(), "-C", destDir.string(),
                             member },
                           {}, 30000, err);
    }

    err = "neither unzip nor tar was found — cannot unpack a MAME romset";
    return false;
}

/// Largest thing this module will ever read into memory. The biggest catalog
/// entry is 32 KB and the biggest zip a few hundred; the cap exists because
/// the file is whatever the network handed us.
constexpr std::size_t kMaxReadBytes = 64u * 1024u * 1024u;

bool readAll(const fs::path& path, std::vector<std::uint8_t>& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "could not read " + path.string();
        return false;
    }
    f.seekg(0, std::ios::end);
    // `tellg()` returns -1 on a stream that cannot seek (a fifo left behind
    // where the download should be) and the old cast turned that into
    // SIZE_MAX: `out.resize(SIZE_MAX)` throws length_error/bad_alloc, and the
    // throw escaped through the fetch thread — leaving `fetchRunning_` stuck
    // true, so the panel's Download button stayed disabled for the rest of
    // the session with no message anywhere.
    const std::streampos end = f.tellg();
    if (end < 0) {
        err = "could not measure " + path.string();
        return false;
    }
    const auto n = static_cast<std::uintmax_t>(end);
    if (n > kMaxReadBytes) {
        err = path.filename().string() + " is " + std::to_string(n) +
              " bytes — refusing to load it";
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(n));
    if (n && !f.read(reinterpret_cast<char*>(out.data()),
                     static_cast<std::streamsize>(n))) {
        err = "short read of " + path.string();
        return false;
    }
    return true;
}

/// CRC-32 (IEEE, reflected) — the same law RomStatus_ImGui displays, so the
/// panel's verdict and this gate cannot disagree.
std::uint32_t crc32Bytes(const std::vector<std::uint8_t>& bytes)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint8_t b : bytes) c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/// Install `bytes` as `dest`, but only if they are the dump we asked for.
///
/// Size was the only gate. The catalogue carries a CRC32 for the entries POM2
/// has a documented reference for, and it was displayed in the ROM Status
/// panel and used for nothing else — so a download that was the right LENGTH
/// and the wrong FILE (a mirror serving another revision, an error page
/// padded out) landed in the user's roms/ and surfaced days later as "it
/// doesn't boot". Verify before publishing, refuse on mismatch, and say what
/// was expected. (Bug hunt 2026-09-06 #H9.)
bool commitBytes(const fs::path& dest, const RomFetchEntry& entry,
                 const std::vector<std::uint8_t>& bytes, std::string& err)
{
    if (entry.expectedSize && bytes.size() != entry.expectedSize) {
        err = dest.filename().string() + " is " + std::to_string(bytes.size()) +
              " bytes, expected " + std::to_string(entry.expectedSize);
        return false;
    }
    if (entry.expectedCrc) {
        const std::uint32_t got = crc32Bytes(bytes);
        if (got != entry.expectedCrc) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "%s has CRC32 %08X, expected %08X%s%s",
                          dest.filename().string().c_str(), got,
                          entry.expectedCrc,
                          entry.crcLabel ? " — " : "",
                          entry.crcLabel ? entry.crcLabel : "");
            err = buf;
            return false;
        }
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (!writeFileAtomic(dest, bytes.data(), bytes.size(), ec)) {
        err = "could not write " + dest.string() +
              (ec ? (": " + ec.message()) : std::string());
        return false;
    }
    return true;
}

std::string defaultPresent(const char* destRel)
{
    return findResource(destRel);
}

}  // namespace

const std::vector<RomFetchEntry>& romFetchCatalog()
{
    return catalogStorage();
}

std::filesystem::path writableRomsDir()
{
    for (const auto& base : resourceSearchDirs()) {
        if (base.empty()) continue;
        const fs::path roms = base / "roms";
        std::error_code ec;
        if (fs::is_directory(roms, ec) && dirIsWritable(roms)) return roms;
    }
    const fs::path fallback = userDataDir() / "roms";
    std::error_code ec;
    fs::create_directories(fallback, ec);
    return fallback;
}

std::vector<const RomFetchEntry*> romsToFetch(
    const std::function<bool(const char* destRel)>& present)
{
    std::vector<const RomFetchEntry*> out;
    for (const auto& e : catalogStorage()) {
        if (!present(e.destRel)) out.push_back(&e);
    }
    return out;
}

std::vector<const RomFetchEntry*> romsToFetch()
{
    return romsToFetch([](const char* destRel) {
        return !defaultPresent(destRel).empty();
    });
}

RomFetchResult fetchMissingRoms(const fs::path& destRoot,
                                const RomFetchProgress& progress,
                                const RomFetchCancel& cancelled)
{
    RomFetchResult r;
    r.destDir = destRoot.string();

#if defined(__EMSCRIPTEN__)
    r.error   = "ROM download is not available in the browser build";
    r.summary = r.error;
    return r;
#else
    if (ChildProcess::findOnPath("curl").empty()) {
        r.error   = "curl was not found on PATH — install it to fetch ROMs";
        r.summary = r.error;
        return r;
    }

    const auto missing = romsToFetch();
    r.skipped = static_cast<int>(catalogStorage().size() - missing.size());
    if (missing.empty()) {
        r.summary = "Every RetroBIOS dump POM2 can use is already present.";
        return r;
    }

    std::error_code ec;
    fs::create_directories(destRoot, ec);
    if (!fs::is_directory(destRoot, ec)) {
        r.error   = "cannot create " + destRoot.string();
        r.summary = r.error;
        return r;
    }

    const fs::path scratch = destRoot / ".retrobios-fetch";
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);

    std::unordered_map<std::string, fs::path> zipCache;
    const int total = static_cast<int>(missing.size());
    int done = 0;

    auto tick = [&](const char* label) {
        if (progress) progress(done, total, label);
    };

    for (const RomFetchEntry* e : missing) {
        if (cancelled && cancelled()) {
            if (r.error.empty()) r.error = "cancelled";
            break;
        }
        tick(e->label);
        std::string err;
        const fs::path dest = destRoot / fs::path(e->destRel).filename();

        bool ok = false;
        if (!e->zipMember) {
            const fs::path raw = scratch / fs::path(e->destRel).filename();
            ok = downloadUrl(e->url, raw, err, cancelled);
            if (ok) {
                std::vector<std::uint8_t> bytes;
                ok = readAll(raw, bytes, err) &&
                     commitBytes(dest, *e, bytes, err);
            }
        } else {
            fs::path zipPath;
            auto it = zipCache.find(e->url);
            if (it == zipCache.end()) {
                zipPath = scratch / ("pack-" + std::to_string(zipCache.size()) + ".zip");
                if (!downloadUrl(e->url, zipPath, err, cancelled)) {
                    zipPath.clear();
                } else {
                    zipCache.emplace(e->url, zipPath);
                }
            } else {
                zipPath = it->second;
            }

            if (!zipPath.empty()) {
                const fs::path extractDir = scratch / ("x-" + std::to_string(done));
                std::vector<std::uint8_t> bytes;
                ok = extractZipMember(zipPath, e->zipMember, extractDir, err);
                if (ok) {
                    ok = readAll(extractedMemberPath(extractDir, e->zipMember),
                                 bytes, err);
                }
                if (ok && e->zipConcat) {
                    for (const char* const* m = e->zipConcat; *m; ++m) {
                        if (!extractZipMember(zipPath, *m, extractDir, err)) {
                            ok = false;
                            break;
                        }
                        std::vector<std::uint8_t> more;
                        if (!readAll(extractedMemberPath(extractDir, *m), more, err)) {
                            ok = false;
                            break;
                        }
                        bytes.insert(bytes.end(), more.begin(), more.end());
                    }
                }
                if (ok)
                    ok = commitBytes(dest, *e, bytes, err);
            }
        }

        if (ok) {
            ++r.saved;
            log().info("ROM", std::string("fetched ") + e->destRel +
                       " from RetroBIOS");
        } else {
            ++r.failed;
            log().warn("ROM", std::string("RetroBIOS fetch of ") + e->destRel +
                       " failed: " + err);
            if (r.error.empty()) r.error = err;
        }
        ++done;
        tick(e->label);
    }

    fs::remove_all(scratch, ec);

    char buf[192];
    if (r.failed == 0) {
        std::snprintf(buf, sizeof(buf),
                      "Saved %d ROM%s to %s (%d already present).",
                      r.saved, r.saved == 1 ? "" : "s",
                      destRoot.string().c_str(), r.skipped);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "Saved %d, failed %d, skipped %d — %s",
                      r.saved, r.failed, r.skipped, r.error.c_str());
    }
    r.summary = buf;
    return r;
#endif
}

}  // namespace pom2
