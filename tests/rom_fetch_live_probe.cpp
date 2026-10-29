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

// rom_fetch_live_probe — run the ROM Status panel's "Download missing ROMs"
// for real, against the network, from a tree that holds no ROM: once from the
// POM2 archive on archive.org alone, once from RetroBIOS alone.
//
// `rom_fetch` (ctest) never touches the network, which is how the v1.0.0
// release shipped a catalogue where 26 of 45 URLs answered 404 after
// RetroBIOS reorganised `main` (2026-10-20/22). This probe is the check to
// run before moving POM2_RETROBIOS_COMMIT: it downloads the WHOLE catalogue
// through fetchMissingRoms() — the same size / CRC32 / SHA-256 gates the
// button applies — and exits non-zero unless every entry was saved.
//
// It changes the working directory and $HOME / %LOCALAPPDATA% to an empty
// scratch tree under probeOutDir() first, so findResource() sees no ROM and
// nothing is written to the user's real data directory.
//
//   cmake --build build --target rom_fetch_live_probe
//   build/tests/rom_fetch_live_probe [--out DIR]

#include "ProbeOutDir.h"
#include "RomFetch.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    std::string outArg;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--out") outArg = argv[i + 1];

    const fs::path root = fs::path(pom2test::probeOutDir(outArg)) / "rom_fetch_live";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "home", ec);
    fs::create_directories(root / "cwd", ec);
#ifdef _WIN32
    _putenv_s("LOCALAPPDATA", (root / "home").string().c_str());
    _putenv_s("APPDATA", (root / "home").string().c_str());
#else
    setenv("HOME", (root / "home").string().c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "home" / "data").string().c_str(), 1);
#endif
    fs::current_path(root / "cwd", ec);
    if (ec) {
        std::fprintf(stderr, "cannot enter %s: %s\n",
                     (root / "cwd").string().c_str(), ec.message().c_str());
        return 2;
    }

    const auto& cat  = pom2::romFetchCatalog();
    const auto  plan = pom2::romsToFetch();
    std::printf("archive %s\nRetroBIOS commit %s\ncatalogue %zu entries, %zu to fetch\n",
                pom2::kPom2RomArchiveUrl, pom2::kRetroBiosCommit, cat.size(), plan.size());
    if (plan.size() != cat.size()) {
        std::fprintf(stderr, "a ROM was found before the fetch — the scratch "
                             "tree is not empty\n");
        return 2;
    }

    // Each source ALONE, into its own empty folder: the archive must supply
    // the whole catalogue by itself, and so must RetroBIOS, its fallback —
    // a run with both would hide a gap in either.
    struct Leg { const char* name; pom2::RomFetchSources src; };
    const Leg legs[] = {
        { "POM2 archive (archive.org)", { true,  false } },
        { "RetroBIOS (pinned commit)",  { false, true  } },
    };
    bool allOk = true;
    int n = 0;
    for (const Leg& leg : legs) {
        const fs::path dest = root / ("roms-" + std::to_string(n++));
        std::printf("\n== %s ==\n", leg.name);
        const auto r = pom2::fetchMissingRoms(
            dest,
            [](int done, int total, const char* label) {
                std::printf("[%d/%d] %s\n", done, total, label ? label : "");
                std::fflush(stdout);
            },
            {}, leg.src);
        std::printf("%s\n", r.summary.c_str());
        if (!r.error.empty()) std::printf("error: %s\n", r.error.c_str());
        const bool ok = r.failed == 0 && r.saved == static_cast<int>(cat.size());
        std::printf("%s: %s, %d of %zu saved into %s\n", ok ? "PASS" : "FAIL",
                    leg.name, r.saved, cat.size(), dest.string().c_str());
        allOk = allOk && ok;
    }
    return allOk ? 0 : 1;
}
