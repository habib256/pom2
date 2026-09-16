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

// ResourcePaths — one place that knows where POM2's bundled read-only
// assets (roms/, fonts/, roms/floppy_samples/, …) live at runtime.
//
// Historically every asset-loading site repeated the same idiom:
//   probe `roms/X`, then `../roms/X`, then `../../roms/X`
// which only works when the current working directory is the repo root
// (dev: run_emulator.sh) or one/two levels below it (dev: launched from
// build/). That breaks the moment POM2 is *installed* or *bundled* and
// launched by an absolute path with an unrelated CWD — exactly what a
// release does (AppImage, .deb → /usr/bin/POM2, a desktop launcher, or a
// portable tarball double-clicked from a file manager).
//
// This helper adds executable-relative and FHS-install search roots on
// top of the legacy CWD-relative ones, so the same `findResource("roms/
// apple2.rom")` call resolves in dev, in a portable bundle, and in an
// FHS install — without any call site having to know which.

#ifndef POM2_RESOURCE_PATHS_H
#define POM2_RESOURCE_PATHS_H

#include <filesystem>
#include <string>
#include <vector>

namespace pom2 {

/// Absolute directory holding the running executable, or an empty path
/// when it can't be determined. Cached after the first call. Platform
/// back-ends: `/proc/self/exe` (Linux), `_NSGetExecutablePath` (macOS),
/// `GetModuleFileNameW` (Windows).
std::filesystem::path executableDir();

/// Writable per-user POM2 data directory. Unlike resourceSearchDirs(), this
/// never points at the install tree or the current working directory. The
/// directory is created best-effort and is suitable for printouts and other
/// durable application output.
std::filesystem::path userDataDir();

/// True when userDataDir() really is a per-user directory — and therefore the
/// FIRST resourceSearchDirs() root. False when it fell back to `<temp>/POM2`,
/// which is searched last (see resourceSearchDirs()).
bool userDataDirIsPerUser();

/// Writable per-user POM2 **configuration** directory — where `state.cfg`
/// and `imgui.ini` live. Distinct from userDataDir() on purpose: on Linux
/// the config dir follows `XDG_CONFIG_HOME` (`~/.config/POM2`) while data
/// follows `XDG_DATA_HOME` (`~/.local/share/POM2`), and moving either would
/// orphan every existing user's settings.
///
/// Returns an EMPTY path when no directory could be created, which is a
/// meaningful answer: callers fall back to a dotfile in `$HOME` or to the
/// working directory. Not cached — the environment it reads can change
/// under a test, and it is called a handful of times per session.
///
/// Under Emscripten this is `/persistent`, the IDBFS mount the shell page
/// sets up before the runtime starts. That is the ONLY writable location
/// in the browser that survives a reload: everything else is MEMFS, which
/// is a fresh empty filesystem on every visit. See PersistentFs.h for the
/// other half — a write to IDBFS is not durable until an `FS.syncfs`.
std::filesystem::path userConfigDir();

/// Ordered, de-duplicated base directories searched for bundled assets.
/// Search order (first hit wins):
///   1. per-user data dir          — explicit override (XDG / LOCALAPPDATA)
///   2. <exeDir>                  — portable bundle (binary beside roms/)
///   3. <exeDir>/..               — portable bundle (binary in bin/)
///   4. <exeDir>/../share/POM2    — FHS install (/usr/bin + /usr/share/POM2)
///   5. CWD, ../, ../../          — development fallback only
/// Cached after the first call.
const std::vector<std::filesystem::path>& resourceSearchDirs();

/// Resolve a relative asset path (e.g. "roms/apple2.rom") against every
/// search dir, returning the first existing one (as a string usable by
/// std::ifstream / Memory). An absolute `rel` that exists is returned
/// unchanged. Returns "" when nothing matches.
std::string findResource(const std::string& rel);

/// First of `candidates` (each tried via findResource) that resolves to
/// an existing file; "" when none do. For probe-order lists.
std::string findFirstResource(const std::vector<std::string>& candidates);

} // namespace pom2

#endif // POM2_RESOURCE_PATHS_H
