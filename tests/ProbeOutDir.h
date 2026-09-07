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

#ifndef POM2_TESTS_PROBEOUTDIR_H
#define POM2_TESTS_PROBEOUTDIR_H

// Where a probe drops the PPMs / dumps a human is meant to look at.
//
// NOT the current directory, which is what several probes used to default to:
// they are run from `build/` (or, via ctest, from wherever ctest cd's), so a
// single `./tests/dix_menu_raster_probe` left 53 untracked .ppm files in the
// repo root — indistinguishable from real work in `git status`, and swept
// into the next `git clean`.
//
// Resolution order:
//   1. an explicit directory the caller parsed off the command line (--out),
//   2. $POM2_PROBE_OUT — the convention purplesoft_eve_probe already used,
//   3. <TMPDIR>/pom2_probes, created on demand.
// Never "."; a probe that cannot create its directory says so and falls back
// to the temp root rather than silently writing where it was launched.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace pom2test {

inline std::string probeOutDir(const std::string& explicitDir = {})
{
    std::error_code ec;
    std::filesystem::path dir;
    if (!explicitDir.empty()) {
        dir = explicitDir;
    } else if (const char* env = std::getenv("POM2_PROBE_OUT");
               env && *env) {
        dir = env;
    } else {
        dir = std::filesystem::temp_directory_path(ec) / "pom2_probes";
    }
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr,
            "probeOutDir: cannot create %s (%s) — falling back to the "
            "temp root\n", dir.string().c_str(), ec.message().c_str());
        std::error_code ec2;
        dir = std::filesystem::temp_directory_path(ec2);
    }
    return dir.string();
}

}  // namespace pom2test

#endif  // POM2_TESTS_PROBEOUTDIR_H
