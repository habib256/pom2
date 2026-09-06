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

#ifndef POM2_TESTS_TESTTEMPPATH_H
#define POM2_TESTS_TESTTEMPPATH_H

// Scratch-file naming for the test suite.
//
// Two rules, both learned the hard way:
//   • Never hard-code "/tmp". It does not exist on Windows, and on a CI box
//     with a per-job TMPDIR the file escapes the sandbox that gets cleaned
//     up. `std::filesystem::temp_directory_path()` honours TMPDIR/TEMP.
//   • Never use a fixed leaf name. `ctest -j` runs the suite in parallel and
//     two tests (or two ctest runs on one shared box) that both open
//     "/tmp/pom2_hdv_smoke.hdv" truncate each other mid-read. The pid makes
//     the name unique per process; callers add their own suffix per case.

#include <cstdio>
#include <filesystem>
#include <string>

#ifdef _WIN32
#  include <process.h>
#  define POM2_TEST_GETPID _getpid
#else
#  include <unistd.h>
#  define POM2_TEST_GETPID getpid
#endif

namespace pom2test {

// "<TMPDIR>/<leaf-stem>-<pid><leaf-extension>", e.g.
// tempPath("pom2_hdv_smoke.hdv") → "/var/folders/…/pom2_hdv_smoke-4711.hdv".
inline std::string tempPath(const std::string& leaf)
{
    std::filesystem::path p(leaf);
    const std::string stem = p.stem().string();
    const std::string ext  = p.extension().string();
    const std::string uniq =
        stem + "-" + std::to_string(static_cast<long>(POM2_TEST_GETPID())) + ext;
    std::error_code ec;
    return (std::filesystem::temp_directory_path(ec) / uniq).string();
}

}  // namespace pom2test

#endif  // POM2_TESTS_TESTTEMPPATH_H
