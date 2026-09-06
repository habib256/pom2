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

#include "Settings.h"
#include "AtomicFileReplace.h"
#include "Logger.h"
#include "PersistentFs.h"
#include "ResourcePaths.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace pom2 {

namespace fs = std::filesystem;

namespace {

// Resolve the per-user POM2 state file.
//
// The directory itself is `pom2::userConfigDir()` — shared with the ImGui
// layout file so ONE function decides where POM2's configuration lives.
// This used to be a private copy of the platform dance, and main.cpp held a
// third one for `imgui.ini` with a comment saying it "mirrors
// Settings::resolveStorePath"; a mirror is a thing that can stop matching,
// and under Emscripten it had — neither copy knew about the IDBFS mount, so
// both wrote to MEMFS and the browser build lost every setting on reload.
//
// The fallbacks stay here rather than in ResourcePaths because they are
// this store's: an empty config dir means "put the file somewhere the user
// can still find it", and for a settings store that is a dotfile in $HOME.
fs::path resolveStorePath()
{
    if (const fs::path dir = pom2::userConfigDir(); !dir.empty())
        return dir / "state.cfg";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".pom2_state";
    // No writable home either — the working directory is the last resort.
    return fs::path("pom2_state.cfg");
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// Escape the record-separator and the escape char so arbitrary string values
// round-trip through the line-oriented `key=value` store: a newline would
// otherwise split one entry into two (the second dropped as a no-`=` line),
// and a backslash is escaped to keep the encoding unambiguous.
//
// A KEY needs two more escapes a value does not, because load() decides where
// a record starts and where it splits BEFORE it unescapes anything:
//   '='  — the split is on the FIRST '=', so a key containing one lands half
//          of itself in the value ("a=b" → key "a", value "b=v") and the
//          setting silently becomes a different setting.
//   '#'  — only at position 0, where it makes the whole line a comment and
//          the key vanishes on the next load.
// Values are exempt from both: a '#' after column 0 is data, and every '='
// past the first is data too. `isKey` therefore widens the escape set rather
// than changing it for everyone — state.cfg stays readable for the ~200 keys
// that need neither.
std::string escapeValue(const std::string& s, bool isKey = false)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (isKey && c == '=') { out += "\\="; continue; }
        if (isKey && c == '#' && i == 0) { out += "\\#"; continue; }
        // Leading/trailing space & tab must be escaped too: load() runs the
        // value through trim() (needed to drop a CRLF '\r' artifact), which
        // would otherwise silently strip boundary whitespace from a value —
        // e.g. a path legitimately ending in a space. Escaping only the
        // boundary chars keeps interior spaces readable in the file.
        const bool boundary = (i == 0 || i + 1 == s.size());
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case ' ':  out += boundary ? "\\s" : " ";  break;
            case '\t': out += boundary ? "\\t" : "\t"; break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string unescapeValue(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            if      (n == 'n')  out += '\n';
            else if (n == 'r')  out += '\r';
            else if (n == 's')  out += ' ';
            else if (n == 't')  out += '\t';
            else if (n == '=')  out += '=';    // key-only escapes (see above)
            else if (n == '#')  out += '#';
            else if (n == '\\') out += '\\';
            else { out += '\\'; out += n; }   // unknown escape — pass through
        } else {
            out += s[i];
        }
    }
    return out;
}

// Index of the '=' that separates key from value: the first one NOT preceded
// by an odd run of backslashes. A plain `find('=')` would split inside the
// `\=` an escaped key writes, turning "a\=b=v" into key "a\" / value "b=v".
size_t findSeparator(const std::string& line)
{
    size_t backslashes = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') { ++backslashes; continue; }
        if (line[i] == '=' && (backslashes % 2) == 0) return i;
        backslashes = 0;
    }
    return std::string::npos;
}

} // namespace

bool Settings::load()
{
    const fs::path path = resolveStorePath();
    std::error_code sizeEc;
    constexpr std::uintmax_t kMaxSettingsBytes = 4u * 1024u * 1024u;
    const auto bytes = fs::file_size(path, sizeEc);
    if (!sizeEc && bytes > kMaxSettingsBytes) {
        pom2::log().warn("Settings", "Refusing oversized settings file: " +
                                     path.string());
        return false;
    }
    std::error_code existsEc;
    if (sizeEc && fs::exists(path, existsEc) && !existsEc) {
        pom2::log().warn("Settings", "Refusing non-regular settings file: " +
                                     path.string());
        return false;
    }
    std::ifstream f(path);
    if (!f) return false;     // missing → use defaults; not an error

    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        // A UTF-8 BOM is what a Windows text editor leaves behind when a user
        // hand-edits state.cfg (the header invites exactly that). Those three
        // bytes glue themselves to the first key, which then matches nothing
        // and is silently dropped — one setting lost, no message, and only on
        // the machine where someone opened the file in Notepad.
        if (first) {
            first = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);
        }
        line = trim(line);
        // A '#' is a comment marker ONLY at the start of a line — stripping
        // after the first '#' anywhere would truncate any value that legally
        // contains '#' (e.g. a disk path "/home/u/My#Disks/game.dsk").
        if (line.empty() || line[0] == '#') continue;

        const size_t eq = findSeparator(line);
        if (eq == std::string::npos) continue;
        const std::string key   = unescapeValue(trim(line.substr(0, eq)));
        const std::string value = unescapeValue(trim(line.substr(eq + 1)));
        if (!key.empty()) store[key] = value;
    }
    pom2::log().info("Settings",
        "Loaded " + std::to_string(store.size()) + " keys from " + path.string());
    return true;
}

bool Settings::save() const
{
    // Central read-only gate. Kiosk must never write state.cfg, and the
    // ~20 call sites scattered across the UI cannot each be trusted to
    // remember that — before this, only 4 of them checked, so a
    // `--kiosk` session that toggled to the GUI (F10) and changed a
    // profile or a slot silently rewrote the user's config. Reported as
    // success: callers treat `false` as an I/O error worth warning about,
    // and suppression is not an error.
    if (readOnly_) return true;
    // Nothing changed since this process last wrote the file: skip. The check
    // is against what we wrote, not against the file (which would mean reading
    // it back), so an external edit is not clobbered any more or less than
    // before — the next real change still overwrites it, exactly as it did
    // when every save wrote unconditionally.
    if (hasWritten_ && store == lastWritten_) return true;
    const fs::path path = resolveStorePath();
    // Unique per process AND per call: two POM2 instances sharing one $HOME
    // both used to open `state.cfg.tmp` with trunc and interleave their
    // writes. See tempSiblingPath().
    const fs::path tmp  = pom2::tempSiblingPath(path);
    // Every early return below must take the temp file with it: a leftover
    // `state.cfg.<pid>-<n>.pom2tmp` beside the config is debris the user
    // cannot interpret, and a full disk (the commonest reason to get here)
    // would grow one on every save attempt.
    auto discardTemp = [&tmp] {
        std::error_code rm;
        fs::remove(tmp, rm);
    };
    std::error_code tmpEc;
    if (!pom2::prepareTempPath(tmp, tmpEc)) {
        pom2::log().warn("Settings",
            "Cannot prepare " + tmp.string() + ": " + tmpEc.message());
        return false;
    }
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            pom2::log().warn("Settings",
                "Cannot open " + tmp.string() + " for write");
            discardTemp();
            return false;
        }
        f << "# POM2 runtime config — written automatically on exit.\n";
        f << "# Edit by hand at your own risk; unknown keys are preserved.\n";
        for (const auto& kv : store) {
            f << escapeValue(kv.first, /*isKey=*/true) << '='
              << escapeValue(kv.second) << '\n';
        }
        // Flush + close BEFORE the rename so a deferred write error (disk full /
        // quota) is observed here — checking the stream while it's still open
        // misses the failure and would rename a truncated .tmp over the good
        // config, defeating the whole atomic-write dance.
        f.flush();
        f.close();
        if (!f) {
            pom2::log().warn("Settings", "Write/flush failed on " + tmp.string());
            discardTemp();
            return false;
        }
    }
    // state.cfg holds the AI control server's auth token, which is the whole
    // of that server's security: anyone who can read it can drive the
    // emulator, mount images and write snapshots over the user's files. A
    // 0644 file in a shared $HOME hands it to every local account. Set the
    // mode on the TEMP file, before the rename, so the published file is
    // never briefly world-readable. Windows ignores POSIX bits — the API is a
    // no-op there and the file inherits the directory ACL, which for
    // %APPDATA%\POM2 is already per-user.
    {
        std::error_code permEc;
        fs::permissions(tmp,
                        fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, permEc);
        // Best effort: a filesystem with no permission model (FAT, MEMFS)
        // fails here and that is not a reason to lose the user's settings.
    }
    std::error_code ec;
    if (!replaceFileAtomic(tmp, path, ec)) {
        pom2::log().warn("Settings",
            "Rename " + tmp.string() + " → " + path.string() + " failed: " + ec.message());
        discardTemp();
        return false;
    }
    // Native: the rename above is already durable. Browser: the file now
    // exists in the IDBFS mount's memory image and nowhere else until the
    // frame loop's pump flushes it to IndexedDB. No-op off Emscripten.
    markPersistentStateDirty();
    lastWritten_ = store;
    hasWritten_  = true;
    return true;
}

std::string Settings::getString(const std::string& key, const std::string& def) const
{
    auto it = store.find(key);
    return it == store.end() ? def : it->second;
}

bool Settings::getBool(const std::string& key, bool def) const
{
    auto it = store.find(key);
    if (it == store.end()) return def;
    const std::string& v = it->second;
    if (v == "true" || v == "1" || v == "yes" || v == "on")  return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return def;
}

// True when `idx` (where std::sto* stopped) is the end of the value, give or
// take trailing blanks — a hand-edited "42 " is still an honest 42, while
// "42 rpm" is not.
static bool fullyConsumed(const std::string& s, size_t idx)
{
    while (idx < s.size() && (s[idx] == ' ' || s[idx] == '\t')) ++idx;
    return idx == s.size();
}

// Both numeric getters reject a PARTIAL parse: `std::stoi("6503 # comment")`
// happily returns 6503 and `std::stof("1.5x")` returns 1.5f, so a hand-edited
// state.cfg line with a trailing comment or a stray unit silently yielded a
// value the user never wrote — and "4M" became a 4-cycle CPU budget rather
// than falling back to the default. The whole value (bar surrounding
// whitespace, which load() already trims) must be consumed, the same rule
// CliDispatcher::parseIntPositive applies to command-line numbers.
int Settings::getInt(const std::string& key, int def) const
{
    auto it = store.find(key);
    if (it == store.end()) return def;
    try {
        size_t idx = 0;
        const int v = std::stoi(it->second, &idx);
        return fullyConsumed(it->second, idx) ? v : def;
    } catch (...) { return def; }
}

float Settings::getFloat(const std::string& key, float def) const
{
    auto it = store.find(key);
    if (it == store.end()) return def;
    try {
        size_t idx = 0;
        const float v = std::stof(it->second, &idx);
        return fullyConsumed(it->second, idx) ? v : def;
    } catch (...) { return def; }
}

void Settings::setString(const std::string& key, std::string value) { store[key] = std::move(value); }
void Settings::setBool  (const std::string& key, bool  v) { store[key] = v ? "true" : "false"; }
void Settings::setInt   (const std::string& key, int   v) { store[key] = std::to_string(v); }
void Settings::setFloat (const std::string& key, float v)
{
    // A bare `os << v` uses ostream's DEFAULT precision of 6 significant
    // digits, which is not enough to round-trip a float: 1.0f/3.0f writes as
    // "0.333333" and reads back as a different float. Every float setting —
    // all five volumes, ui_scale, and the ~15 NTSC/CRT + voxel shader
    // parameters — therefore shifted slightly on the first save/load cycle,
    // so what the user dialled in was not what they got back. (The drift is
    // one-shot, not cumulative: the reloaded value re-serialises to the same
    // text.) `max_digits10` (9 for float) is the guaranteed round-trip width.
    //
    // Emit the SHORTEST width that still round-trips rather than always 9, so
    // state.cfg keeps values like "0.5" instead of "0.500000000" — the file
    // header invites hand-editing, and a wall of noise digits works against
    // that. Everyday values cost one extra conversion; only the genuinely
    // awkward ones widen.
    std::string out;
    for (int prec = 6; prec <= std::numeric_limits<float>::max_digits10; ++prec) {
        std::ostringstream os;
        os << std::setprecision(prec) << v;
        out = os.str();
        float back = 0.0f;
        try { back = std::stof(out); } catch (...) { continue; }
        if (back == v) break;
    }
    store[key] = out;
}

std::string Settings::getStorePath() const { return resolveStorePath().string(); }

} // namespace pom2
