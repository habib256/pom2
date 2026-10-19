// POM2 Apple II Emulator — GPL-3.0-or-later
// User symbols, independent of the CPU and UI. Imports are transactional.
#ifndef POM2_DEBUGGER_SYMBOLS_H
#define POM2_DEBUGGER_SYMBOLS_H

#include "DebugAddress.h"

#include <charconv>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace pom2 {

class DebuggerSymbols {
public:
    static bool address(std::string_view text, uint16_t& out) {
        if (!text.empty() && text.front() == '$') text.remove_prefix(1);
        else if (text.size() > 2 && text.substr(0, 2) == "0x") text.remove_prefix(2);
        if (text.empty()) return false;
        unsigned value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 0xFFFF)
            return false;
        out = static_cast<uint16_t>(value);
        return true;
    }

    bool resolve(std::string_view name, uint16_t& out) const {
        const auto it = names_.find(std::string(name));
        if (it != names_.end()) { out = it->second.addr; return true; }
        return address(name, out);
    }

    bool resolveLocation(std::string_view text, DebugAddress& out) const {
        DebugAddress value;
        const auto named = names_.find(std::string(text));
        if (named != names_.end()) { out=named->second; return true; }
        if (address(text,value.addr)) { out=value; return true; }
        const auto colon = text.find(':');
        if (colon != std::string_view::npos) {
            if (!DebugBank::parse(std::string(text.substr(0,colon)), value.bank)) return false;
            text.remove_prefix(colon+1);
        }
        if (!resolve(text,value.addr)) return false;
        out=value; return true;
    }

    std::string label(uint16_t addr, DebugBank bank = {}) const {
        auto it = labels_.find({addr,bank});
        if (it == labels_.end()) it = labels_.find({addr,{}});
        return it == labels_.end() ? std::string{} : it->second;
    }

    std::size_t size() const { return names_.size(); }

    /// AppleWin: "0800 NAME"; VICE/cc65: "al 000800 .NAME";
    /// assembler assignments: "NAME = $0800" or "NAME EQU $0800".
    /// Optional POM2 bank qualifiers (aux1:$0800); numeric addresses stay 16-bit.
    bool load(std::string_view text, std::string& error) {
        error.clear();
        if (text.size() > 4 * 1024 * 1024) { error = "Symbol file exceeds 4 MiB"; return false; }
        std::map<std::string, DebugAddress> names;
        std::map<DebugAddress, std::string> labels;
        std::istringstream input{std::string(text)};
        std::string line;
        std::size_t number = 0;
        while (std::getline(input, line)) {
            ++number;
            const auto comment = line.find_first_of(";#");
            if (comment != std::string::npos) line.resize(comment);
            std::istringstream words(line);
            std::string first, second, third, extra, name, value;
            if (!(words >> first)) continue;
            words >> second >> third >> extra;
            if (first == "al" && !third.empty() && extra.empty()) {
                value = second;
                name = third;
            } else if ((second == "=" || second == "EQU" || second == "equ") &&
                       !third.empty() && extra.empty()) {
                name = first;
                value = third;
            } else if (!second.empty() && third.empty()) {
                value = first;
                name = second;
            }
            DebugAddress addr;
            const bool validName = !name.empty() && name.size() <= 255 &&
                name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.$@:") == std::string::npos;
            if (!validName || !DebuggerSymbols{}.resolveLocation(value, addr) ||
                (names.count(name) && names.at(name) != addr) || names.size() >= 65536) {
                error = "Invalid or conflicting symbol at line " + std::to_string(number);
                return false;
            }
            names[name] = addr;
            labels.emplace(addr, name);
        }
        names_ = std::move(names);
        labels_ = std::move(labels);
        return true;
    }

private:
    std::map<std::string, DebugAddress> names_;
    std::map<DebugAddress, std::string> labels_;
};

} // namespace pom2
#endif
