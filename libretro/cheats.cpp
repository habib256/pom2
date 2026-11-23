// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cheats.hpp"

#include <cctype>
#include <string>

namespace pom2::libretro {
namespace {

constexpr std::uint32_t kAddressSpace = 0x20000;

bool hexDigit(char c)
{
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

std::optional<std::uint32_t> hexNumber(std::string_view text, std::size_t maxDigits)
{
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
    else if (!text.empty() && text[0] == '$') text.remove_prefix(1);
    if (text.empty() || text.size() > maxDigits) return std::nullopt;
    std::uint32_t value = 0;
    for (char c : text) {
        if (!hexDigit(c)) return std::nullopt;
        value = value * 16 + static_cast<std::uint32_t>(std::isdigit(static_cast<unsigned char>(c))
            ? c - '0' : std::toupper(static_cast<unsigned char>(c)) - 'A' + 10);
    }
    return value;
}

std::optional<CheatWrite> parseOne(std::string_view code)
{
    CheatWrite write;
    std::string_view address, value;
    const auto separator = code.find_first_of(":=");
    if (separator == std::string_view::npos) {
        // AAAAAAVV: a 24-bit address and a byte, the RetroArch cheat-file shape.
        if (code.size() != 8) return std::nullopt;
        address = code.substr(0, 6);
        value = code.substr(6);
    } else {
        address = code.substr(0, separator);
        value = code.substr(separator + 1);
    }
    const auto question = address.find('?');
    if (question != std::string_view::npos) {
        const auto compare = hexNumber(address.substr(question + 1), 2);
        if (!compare) return std::nullopt;
        write.compare = static_cast<std::uint8_t>(*compare);
        address = address.substr(0, question);
    }
    const auto parsedAddress = hexNumber(address, 6);
    const auto parsedValue = hexNumber(value, 2);
    if (!parsedAddress || !parsedValue || *parsedAddress >= kAddressSpace) return std::nullopt;
    write.address = *parsedAddress;
    write.value = static_cast<std::uint8_t>(*parsedValue);
    return write;
}

} // namespace

std::optional<std::vector<CheatWrite>> parseCheat(std::string_view code)
{
    std::vector<CheatWrite> writes;
    std::string token;
    const auto flush = [&]() -> bool {
        if (token.empty()) return true;
        const auto write = parseOne(token);
        token.clear();
        if (!write) return false;
        writes.push_back(*write);
        return true;
    };
    for (char c : code) {
        if (c == '+' || c == ';' || c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            if (!flush()) return std::nullopt;
        } else {
            token.push_back(c);
        }
    }
    if (!flush() || writes.empty()) return std::nullopt;
    return writes;
}

} // namespace pom2::libretro
