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

// FloppyEmuDevice — see header. Pure model: mode, SD browser, favorites.

#include "FloppyEmuDevice.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pom2 {

namespace fs = std::filesystem;

namespace {

std::string toLower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string lowerExt(const std::string& name)
{
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    return toLower(name.substr(dot));
}

// Case-insensitive name ordering for the listing.
bool nameLess(const std::string& a, const std::string& b)
{
    return toLower(a) < toLower(b);
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// True iff `p` is `root` or lives under it. Component-wise, not a string
// prefix: "/sdcard" is longer than "/sd" and is NOT inside it. Same test
// goUp() applies to navigation; favourites now share it.
bool pathIsUnder(const fs::path& root, const fs::path& p)
{
    auto rIt = root.begin(), rEnd = root.end();
    auto pIt = p.begin(),    pEnd = p.end();
    for (; rIt != rEnd; ++rIt, ++pIt)
        if (pIt == pEnd || *pIt != *rIt) return false;
    return true;
}

} // namespace

const char* FloppyEmuDevice::modeLabel(FloppyEmuMode m)
{
    switch (m) {
        case FloppyEmuMode::Disk525:     return "Apple II 5.25 Floppy";
        case FloppyEmuMode::Disk35:      return "Apple II 3.5 Floppy";
        case FloppyEmuMode::SmartportHD: return "Smartport Hard Disk";
        case FloppyEmuMode::Unidisk35:   return "Unidisk 3.5";
    }
    return "?";
}

const char* FloppyEmuDevice::modeKey(FloppyEmuMode m)
{
    switch (m) {
        case FloppyEmuMode::Disk525:     return "disk525";
        case FloppyEmuMode::Disk35:      return "disk35";
        case FloppyEmuMode::SmartportHD: return "smartporthd";
        case FloppyEmuMode::Unidisk35:   return "unidisk35";
    }
    return "smartporthd";
}

bool FloppyEmuDevice::modeFromKey(const std::string& key, FloppyEmuMode& out)
{
    for (FloppyEmuMode m : allModes()) {
        if (key == modeKey(m)) { out = m; return true; }
    }
    return false;
}

std::array<FloppyEmuMode, 4> FloppyEmuDevice::allModes()
{
    return { FloppyEmuMode::Disk525, FloppyEmuMode::Disk35,
             FloppyEmuMode::SmartportHD, FloppyEmuMode::Unidisk35 };
}

void FloppyEmuDevice::setSdRoot(const std::string& path)
{
    // lexically_normal() preserves a trailing separator, which leaves an empty
    // trailing path component. goUp()'s component-by-component sandbox check
    // would then mismatch that empty component against a real subdir name and
    // snap any deeply-nested directory straight back to root. Strip it so a
    // user-entered path with a trailing '/' navigates one level at a time.
    fs::path p = fs::path(path).lexically_normal();
    if (!p.has_filename()) p = p.parent_path();   // drop empty trailing component
    sdRoot_     = p.string();
    currentDir_ = sdRoot_;
}

bool FloppyEmuDevice::sdPresent() const
{
    std::error_code ec;
    return !sdRoot_.empty() && fs::is_directory(sdRoot_, ec);
}

bool FloppyEmuDevice::atRoot() const
{
    if (currentDir_.empty()) return true;
    return fs::path(currentDir_).lexically_normal() ==
           fs::path(sdRoot_).lexically_normal();
}

bool FloppyEmuDevice::acceptsFile(const std::string& filename) const
{
    const std::string ext = lowerExt(filename);
    switch (mode_) {
        case FloppyEmuMode::Disk525:
            return ext == ".dsk" || ext == ".do" || ext == ".po" ||
                   ext == ".nib" || ext == ".woz" || ext == ".2mg";
        case FloppyEmuMode::Disk35:
        case FloppyEmuMode::Unidisk35:
            return ext == ".dsk" || ext == ".do" || ext == ".po" ||
                   ext == ".2mg";
        case FloppyEmuMode::SmartportHD:
            return ext == ".po" || ext == ".hdv" || ext == ".2mg";
    }
    return false;
}

std::vector<FloppyEmuDevice::Entry> FloppyEmuDevice::listing() const
{
    std::vector<Entry> dirs, files;
    std::error_code ec;
    if (fs::is_directory(currentDir_, ec)) {
        for (const auto& de : fs::directory_iterator(currentDir_, ec)) {
            const std::string name = de.path().filename().string();
            if (name.empty() || name.front() == '.') continue;  // skip hidden
            // Never follow a symlink: directory_iterator + is_directory()
            // dereference links, which would let an in-root symlink browse
            // (and mount) files anywhere on the host, escaping the SD-root
            // sandbox. Skip them so navigation stays inside floppyemu/.
            if (de.is_symlink(ec)) continue;
            if (de.is_directory(ec)) {
                Entry e;
                e.name     = name;
                e.fullPath = de.path().string();
                e.isDir    = true;
                dirs.push_back(std::move(e));
            } else if (de.is_regular_file(ec) && acceptsFile(name)) {
                Entry e;
                e.name      = name;
                e.fullPath  = de.path().string();
                // Same rule as parseFavorites below: file_size(ec) returns
                // (uintmax_t)-1 and sets ec when the file vanished between
                // the iterator step and the stat — honor ec → 0, not a
                // 16-EiB row in the SD browser.
                const auto sz = de.file_size(ec);
                e.sizeBytes = ec ? 0u : static_cast<uint64_t>(sz);
                files.push_back(std::move(e));
            }
        }
    }
    std::sort(dirs.begin(),  dirs.end(),
              [](const Entry& a, const Entry& b){ return nameLess(a.name, b.name); });
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b){ return nameLess(a.name, b.name); });

    std::vector<Entry> out;
    if (!atRoot()) {
        Entry up;
        up.name     = "..";
        up.fullPath = fs::path(currentDir_).parent_path().string();
        up.isDir    = true;
        up.isUp     = true;
        out.push_back(std::move(up));
    }
    out.insert(out.end(), dirs.begin(),  dirs.end());
    out.insert(out.end(), files.begin(), files.end());
    return out;
}

void FloppyEmuDevice::enterDir(const Entry& e)
{
    if (e.isUp)       { goUp(); return; }
    if (e.isDir && !e.fullPath.empty()) currentDir_ = e.fullPath;
}

void FloppyEmuDevice::goUp()
{
    if (atRoot()) return;
    const fs::path root   = fs::path(sdRoot_).lexically_normal();
    const fs::path parent = fs::path(currentDir_).parent_path().lexically_normal();
    // Clamp to root unless `parent` is root or a descendant of root. Use a
    // COMPONENT-wise prefix check, not a string-length test — a sibling like
    // "/sdcard" is longer than root "/sd" but is NOT under it.
    bool under = true;
    auto rIt = root.begin(), rEnd = root.end();
    auto pIt = parent.begin(), pEnd = parent.end();
    for (; rIt != rEnd; ++rIt, ++pIt) {
        if (pIt == pEnd || *pIt != *rIt) { under = false; break; }
    }
    currentDir_ = under ? parent.string() : root.string();
}

FloppyEmuDevice::Favorites
FloppyEmuDevice::parseFavorites(const std::string& content,
                                const std::string& resolveBase)
{
    Favorites fav;
    fav.present = true;
    std::istringstream in(content);
    std::string line;
    bool firstMeaningful = true;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t.front() == '#') continue;
        if (firstMeaningful) {
            firstMeaningful = false;
            // Optional "automount N" directive on the first meaningful line.
            std::istringstream ls(t);
            std::string kw; ls >> kw;
            if (toLower(kw) == "automount") {
                int n = 0; ls >> n;
                fav.automount = (n >= 0 && n <= 2) ? n : 0;
                continue;  // directive line is not a favorite
            }
        }
        // Resolve a favorite image path relative to the SD root.
        fs::path p(t);
        if (p.is_relative() && !resolveBase.empty())
            p = fs::path(resolveBase) / p;
        p = p.lexically_normal();
        // Then clamp to the sandbox, which the listing has always enforced
        // and this path never did: `favdisks.txt` is a plain text file inside
        // the emulated SD card, and an absolute line ("/etc/shadow") was used
        // verbatim while `../..` was collapsed out by lexically_normal and
        // then followed. Either one put a host file one click from being
        // mounted into the guest — through a file the guest itself can write.
        // Drop the entry rather than silently rewriting it: a favourite that
        // does not name something on the card is not a favourite.
        if (!resolveBase.empty()) {
            const fs::path root = fs::path(resolveBase).lexically_normal();
            if (!pathIsUnder(root, p)) continue;
        }
        Entry e;
        e.name      = p.filename().string();
        e.fullPath  = p.string();
        std::error_code ec;
        const auto sz = fs::file_size(e.fullPath, ec);
        // A favorite whose file is gone: file_size returns (uintmax_t)-1 and
        // sets ec. Honor ec → report 0 instead of "17592186044415M".
        e.sizeBytes = ec ? 0u : static_cast<uint64_t>(sz);
        fav.entries.push_back(std::move(e));
    }
    return fav;
}

FloppyEmuDevice::Favorites FloppyEmuDevice::favorites() const
{
    Favorites fav;
    if (sdRoot_.empty()) return fav;
    const std::string favPath = (fs::path(sdRoot_) / "favdisks.txt").string();
    std::error_code ec;
    if (!fs::is_regular_file(favPath, ec)) return fav;  // present=false
    std::ifstream f(favPath, std::ios::binary);
    if (!f) return fav;
    std::ostringstream ss; ss << f.rdbuf();
    return parseFavorites(ss.str(), sdRoot_);
}

} // namespace pom2
