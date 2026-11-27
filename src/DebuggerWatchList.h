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

// DebuggerWatchList — the addresses the Watch window shows, and nothing else.
//
// French Touch's request (2026-11-27): "a window in which one adds addresses
// and which shows, at all times, the values they hold" — AppleWin's watches.
// A watch never stops the machine; stopping on an access is a memory
// breakpoint (Debugger.h, BPMR / BPMW). POM2 called those "watches" until
// that day, which is the confusion this file's name settles.
//
// The list is plain data with a text form, so it persists in one settings
// key (`debugger_watch_list`) and is testable without ImGui:
//
//     *:$0010/b,main:$00FE/w,aux0:$0800/n
//
// bank:$address as DebugAddress::text() spells it, then the format: b = one
// byte, w = a 16-bit little-endian word (a zero-page pointer), n = a byte in
// binary. The VALUE is the CPU's view at this instant: an entry qualified by
// a bank that is not the one mapped right now has no value to show.

#ifndef POM2_DEBUGGER_WATCH_LIST_H
#define POM2_DEBUGGER_WATCH_LIST_H

#include "DebugAddress.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 {

struct WatchEntry {
    enum class Format : uint8_t { Byte, Word, Binary };
    DebugAddress at;
    Format format = Format::Byte;
    bool operator==(const WatchEntry& o) const { return at == o.at && format == o.format; }
};

class DebuggerWatchList
{
public:
    static constexpr std::size_t kMax = 64;

    const std::vector<WatchEntry>& entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }

    /// False when it is already there or the list is full.
    bool add(const WatchEntry& entry)
    {
        if (entries_.size() >= kMax) return false;
        for (const WatchEntry& e : entries_) if (e == entry) return false;
        entries_.push_back(entry);
        return true;
    }
    void remove(std::size_t index)
    {
        if (index < entries_.size()) entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    void setFormat(std::size_t index, WatchEntry::Format format)
    {
        if (index < entries_.size()) entries_[index].format = format;
    }
    void clear() { entries_.clear(); }

    std::string text() const
    {
        std::string out;
        for (const WatchEntry& e : entries_) {
            if (!out.empty()) out += ',';
            out += e.at.text();
            out += '/';
            out += e.format == WatchEntry::Format::Word ? 'w'
                 : e.format == WatchEntry::Format::Binary ? 'n' : 'b';
        }
        return out;
    }

    /// Replace the list by what `text` spells. A malformed item is skipped:
    /// a settings file edited by hand must not cost the rest of the list.
    void parse(std::string_view text)
    {
        entries_.clear();
        while (!text.empty()) {
            const std::size_t comma = text.find(',');
            const std::string_view item = text.substr(0, comma);
            text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
            WatchEntry entry;
            if (parseItem(item, entry)) add(entry);
        }
    }

    /// The value as the window prints it: "$5A", "$C600", "%01011010".
    static std::string value(WatchEntry::Format format, uint8_t lo, uint8_t hi)
    {
        char out[16];
        switch (format) {
            case WatchEntry::Format::Word:
                std::snprintf(out, sizeof out, "$%02X%02X", hi, lo);
                break;
            case WatchEntry::Format::Binary: {
                out[0] = '%';
                for (int bit = 0; bit < 8; ++bit) out[1 + bit] = (lo & (0x80u >> bit)) ? '1' : '0';
                out[9] = '\0';
                break;
            }
            default:
                std::snprintf(out, sizeof out, "$%02X", lo);
        }
        return out;
    }

private:
    static bool parseItem(std::string_view item, WatchEntry& out)
    {
        const std::size_t slash = item.rfind('/');
        const std::size_t colon = item.rfind(':', slash);
        if (slash == std::string_view::npos || colon == std::string_view::npos) return false;
        if (slash + 2 != item.size()) return false;
        switch (item[slash + 1]) {
            case 'b': out.format = WatchEntry::Format::Byte;   break;
            case 'w': out.format = WatchEntry::Format::Word;   break;
            case 'n': out.format = WatchEntry::Format::Binary; break;
            default:  return false;
        }
        if (!DebugBank::parse(item.substr(0, colon), out.at.bank)) return false;
        const std::string_view hex = item.substr(colon + 1, slash - colon - 1);
        if (hex.size() != 5 || hex[0] != '$') return false;
        unsigned addr = 0;
        for (const char c : hex.substr(1)) {
            const int digit = c >= '0' && c <= '9' ? c - '0'
                            : c >= 'A' && c <= 'F' ? c - 'A' + 10
                            : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0) return false;
            addr = addr * 16 + static_cast<unsigned>(digit);
        }
        out.at.addr = static_cast<uint16_t>(addr);
        return true;
    }

    std::vector<WatchEntry> entries_;
};

} // namespace pom2

#endif // POM2_DEBUGGER_WATCH_LIST_H
