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

// SettingsList — packing a list of paths into one `state.cfg` value.
//
// The store is a flat `key=value` file with one entry per line, so a list
// has to survive as a single value. Disk paths can contain spaces, commas,
// semicolons and colons, so the separator must be a byte a path cannot hold:
// 0x1F, ASCII unit separator.
//
// It lives in a header rather than in whichever .cpp happened to need it
// first because the reader and the writer are now in two translation units
// (MainWindow.cpp restores, MainWindow_Session.cpp persists) — and a
// separator convention with two copies is a separator convention that can
// disagree with itself, at which point the user's favourites list comes back
// as one long path.

#ifndef POM2_SETTINGS_LIST_H
#define POM2_SETTINGS_LIST_H

#include <string>
#include <vector>

namespace pom2 {

inline constexpr char kSettingListSep = '\x1f';

// An entry that holds the separator itself is left out: written as is it
// would come back as two bogus paths, and any escape added now would
// mis-read every list already saved (ARCH-006). A path with a 0x1F byte in
// it is not a path anyone types; losing it costs one recent entry.
inline std::string joinSettingList(const std::vector<std::string>& v)
{
    std::string out;
    bool first = true;
    for (const std::string& item : v) {
        if (item.find(kSettingListSep) != std::string::npos) continue;
        if (!first) out += kSettingListSep;
        out += item;
        first = false;
    }
    return out;
}

inline std::vector<std::string> splitSettingList(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = s.find(kSettingListSep, start);
        const std::string piece = (end == std::string::npos)
                                ? s.substr(start)
                                : s.substr(start, end - start);
        if (!piece.empty()) out.push_back(piece);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

} // namespace pom2

#endif // POM2_SETTINGS_LIST_H
