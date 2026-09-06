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

#include "ResourcePaths.h"

#include <cstdlib>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>      // _NSGetExecutablePath
#elif defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>          // GetModuleFileNameW
#else
#  include <unistd.h>           // readlink (/proc/self/exe)
#  include <limits.h>
#endif

namespace pom2 {

namespace fs = std::filesystem;

namespace {

// Best-effort absolute path of the running executable. Empty on failure.
fs::path probeExecutablePath()
{
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);   // first call learns the length
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(buf.data()), ec);
    return ec ? fs::path(buf.data()) : p;
#elif defined(_WIN32)
    // MAX_PATH is not a valid executable-path bound. Grow until the complete
    // path fits; the manifest opts the rest of the Win32 filesystem calls
    // into long-path handling on supported systems.
    for (DWORD cap = 512; cap <= 32768; cap *= 2) {
        std::vector<wchar_t> buf(cap, L'\0');
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), cap);
        if (n == 0) return {};
        if (n < cap - 1 || (n < cap && GetLastError() != ERROR_INSUFFICIENT_BUFFER))
            return fs::path(std::wstring(buf.data(), n));
    }
    return {};
#else
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return self;
    // Last resort: some sandboxes don't expose /proc — give up gracefully.
    return {};
#endif
}

} // namespace

fs::path executableDir()
{
    static const fs::path cached = [] {
        fs::path exe = probeExecutablePath();
        return exe.empty() ? fs::path{} : exe.parent_path();
    }();
    return cached;
}

namespace {

struct UserDataDir {
    fs::path path;
    /// False when nothing per-user resolved and this is the temp-dir last
    /// resort. See resourceSearchDirs() for why the distinction matters.
    bool     perUser = false;
};

const UserDataDir& userDataDirInfo()
{
    static const UserDataDir cached = [] {
        UserDataDir out;
        fs::path dir;
#if defined(_WIN32)
        if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
            dir = fs::path(local) / "POM2";
#elif defined(__APPLE__)
        if (const char* home = std::getenv("HOME"); home && *home)
            dir = fs::path(home) / "Library" / "Application Support" / "POM2";
#else
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
            dir = fs::path(xdg) / "POM2";
        else if (const char* home = std::getenv("HOME"); home && *home)
            dir = fs::path(home) / ".local" / "share" / "POM2";
#endif
        out.perUser = !dir.empty();
        if (dir.empty()) {
            std::error_code ec;
            dir = fs::temp_directory_path(ec) / "POM2";
            if (ec) dir = fs::path("POM2");
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        out.path = dir;
        return out;
    }();
    return cached;
}

} // namespace

fs::path userDataDir()
{
    return userDataDirInfo().path;
}

fs::path userConfigDir()
{
    // Emscripten: the IDBFS mount from wasm/shell.html's preRun hook. Not a
    // guess — the mount is a hard prerequisite, and if it failed the shell
    // logged it and this directory behaves like any other MEMFS path (writes
    // succeed, nothing survives the reload).
#if defined(__EMSCRIPTEN__)
    return fs::path("/persistent");
#else
    // Candidates in preference order. Windows lists both roaming and local
    // because the second is a real fallback, not a duplicate: a locked-down
    // profile can have %APPDATA% pointing at an unwritable network share
    // while %LOCALAPPDATA% is fine.
    std::vector<fs::path> candidates;
#  if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"); appData && *appData)
        candidates.emplace_back(fs::path(appData) / "POM2");
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        candidates.emplace_back(fs::path(local) / "POM2");
#  elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        candidates.emplace_back(fs::path(home) / "Library" / "Application Support" / "POM2");
#  else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
        candidates.emplace_back(fs::path(config) / "POM2");
    else if (const char* home = std::getenv("HOME"); home && *home)
        candidates.emplace_back(fs::path(home) / ".config" / "POM2");
#  endif
    for (const fs::path& dir : candidates) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (fs::is_directory(dir, ec)) return dir;
    }
    // Nothing usable. Empty is a meaningful answer: the caller has its own
    // last-resort location, and writing into a path that is not a directory
    // would fail on every save with a message about the wrong thing.
    return {};
#endif
}

const std::vector<fs::path>& resourceSearchDirs()
{
    static const std::vector<fs::path> cached = [] {
        std::vector<fs::path> dirs;
        auto push = [&dirs](fs::path p) {
            std::error_code ec;
            // Keep the empty path (== CWD) verbatim; normalise the rest so
            // duplicate roots collapse (e.g. exeDir == CWD in a dev run).
            if (!p.empty()) {
                fs::path norm = fs::weakly_canonical(p, ec);
                if (!ec) p = norm;
            }
            for (const auto& d : dirs) if (d == p) return;
            dirs.push_back(std::move(p));
        };

        // 1: per-user data dir — explicit overrides for a bundled dump
        // package (AppImage / .deb / .dmg). Searched before the install tree
        // so a file dropped in ~/.local/share/POM2/roms/ wins. Honours
        // $XDG_DATA_HOME, else $HOME/.local/share (XDG basedir); on Windows
        // also %LOCALAPPDATA%\POM2.
        // Keep this exactly aligned with writable output. The previous
        // duplicated XDG logic accidentally searched ~/.local/share on macOS
        // while documentation and writes used ~/Library/Application Support;
        // user ROM overrides therefore worked on Linux but not in a .app.
        //
        // ONLY when it really is a per-user directory. With no $HOME (a
        // daemon, a locked-down service account, a container) userDataDir()
        // falls back to `<temp>/POM2`, and /tmp is world-writable: any local
        // user could plant `/tmp/POM2/roms/apple2e.rom` and have POM2 boot it
        // in preference to the installed dump. The fallback stays searchable
        // — it is where that session's own downloads land — but behind the
        // install tree, where it can no longer override anything shipped.
        const UserDataDir& udd = userDataDirInfo();
        if (udd.perUser) push(udd.path);

        // 2-4: executable-relative roots (portable bundle + FHS install).
        const fs::path exe = executableDir();
        if (!exe.empty()) {
            push(exe);                        // binary beside roms/, fonts/
            push(exe / "..");                 // binary in bin/, assets a level up
            push(exe / ".." / "share" / "POM2");  // /usr/bin + /usr/share/POM2
        }
        if (!udd.perUser) push(udd.path);

        // 5-7: legacy development roots. Keep them last: a desktop launch
        // often inherits the directory containing a disk image, and an
        // unrelated `roms/apple2e.rom` there must not override the installed
        // application or the user's explicit override directory.
        push(fs::path{});
        push("..");
        push("../..");
        return dirs;
    }();
    return cached;
}

std::string findResource(const std::string& rel)
{
    if (rel.empty()) return {};
    std::error_code ec;

    // Absolute paths bypass the search roots entirely.
    fs::path relp(rel);
    if (relp.is_absolute()) {
        return fs::exists(relp, ec) ? rel : std::string{};
    }

    for (const fs::path& base : resourceSearchDirs()) {
        fs::path cand = base.empty() ? relp : (base / relp);
        if (fs::exists(cand, ec)) return cand.string();
    }
    return {};
}

std::string findFirstResource(const std::vector<std::string>& candidates)
{
    for (const std::string& c : candidates) {
        std::string r = findResource(c);
        if (!r.empty()) return r;
    }
    return {};
}

} // namespace pom2
