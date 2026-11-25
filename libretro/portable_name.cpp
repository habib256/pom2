// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#include "portable_name.hpp"

#include "pom2/core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

namespace pom2::libretro {
namespace {

// Generated with Python's unicodedata (NFD, ASCII part kept; ligatures and
// strokes by hand): U+00C0-U+017F to plain ASCII.
constexpr struct { char32_t cp; const char* ascii; } kLatinAscii[] = {
    {0xC0, "A"}, {0xC1, "A"}, {0xC2, "A"}, {0xC3, "A"}, {0xC4, "A"}, {0xC5, "A"}, {0xC6, "AE"}, {0xC7, "C"},
    {0xC8, "E"}, {0xC9, "E"}, {0xCA, "E"}, {0xCB, "E"}, {0xCC, "I"}, {0xCD, "I"}, {0xCE, "I"}, {0xCF, "I"},
    {0xD0, "D"}, {0xD1, "N"}, {0xD2, "O"}, {0xD3, "O"}, {0xD4, "O"}, {0xD5, "O"}, {0xD6, "O"}, {0xD8, "O"},
    {0xD9, "U"}, {0xDA, "U"}, {0xDB, "U"}, {0xDC, "U"}, {0xDD, "Y"}, {0xDE, "TH"}, {0xDF, "ss"}, {0xE0, "a"},
    {0xE1, "a"}, {0xE2, "a"}, {0xE3, "a"}, {0xE4, "a"}, {0xE5, "a"}, {0xE6, "ae"}, {0xE7, "c"}, {0xE8, "e"},
    {0xE9, "e"}, {0xEA, "e"}, {0xEB, "e"}, {0xEC, "i"}, {0xED, "i"}, {0xEE, "i"}, {0xEF, "i"}, {0xF0, "d"},
    {0xF1, "n"}, {0xF2, "o"}, {0xF3, "o"}, {0xF4, "o"}, {0xF5, "o"}, {0xF6, "o"}, {0xF8, "o"}, {0xF9, "u"},
    {0xFA, "u"}, {0xFB, "u"}, {0xFC, "u"}, {0xFD, "y"}, {0xFE, "th"}, {0xFF, "y"}, {0x100, "A"}, {0x101, "a"},
    {0x102, "A"}, {0x103, "a"}, {0x104, "A"}, {0x105, "a"}, {0x106, "C"}, {0x107, "c"}, {0x108, "C"},
    {0x109, "c"}, {0x10A, "C"}, {0x10B, "c"}, {0x10C, "C"}, {0x10D, "c"}, {0x10E, "D"}, {0x10F, "d"},
    {0x110, "D"}, {0x111, "d"}, {0x112, "E"}, {0x113, "e"}, {0x114, "E"}, {0x115, "e"}, {0x116, "E"},
    {0x117, "e"}, {0x118, "E"}, {0x119, "e"}, {0x11A, "E"}, {0x11B, "e"}, {0x11C, "G"}, {0x11D, "g"},
    {0x11E, "G"}, {0x11F, "g"}, {0x120, "G"}, {0x121, "g"}, {0x122, "G"}, {0x123, "g"}, {0x124, "H"},
    {0x125, "h"}, {0x126, "H"}, {0x127, "h"}, {0x128, "I"}, {0x129, "i"}, {0x12A, "I"}, {0x12B, "i"},
    {0x12C, "I"}, {0x12D, "i"}, {0x12E, "I"}, {0x12F, "i"}, {0x130, "I"}, {0x131, "i"}, {0x132, "IJ"},
    {0x133, "ij"}, {0x134, "J"}, {0x135, "j"}, {0x136, "K"}, {0x137, "k"}, {0x138, "q"}, {0x139, "L"},
    {0x13A, "l"}, {0x13B, "L"}, {0x13C, "l"}, {0x13D, "L"}, {0x13E, "l"}, {0x13F, "L"}, {0x140, "l"},
    {0x141, "L"}, {0x142, "l"}, {0x143, "N"}, {0x144, "n"}, {0x145, "N"}, {0x146, "n"}, {0x147, "N"},
    {0x148, "n"}, {0x149, "n"}, {0x14A, "N"}, {0x14B, "n"}, {0x14C, "O"}, {0x14D, "o"}, {0x14E, "O"},
    {0x14F, "o"}, {0x150, "O"}, {0x151, "o"}, {0x152, "OE"}, {0x153, "oe"}, {0x154, "R"}, {0x155, "r"},
    {0x156, "R"}, {0x157, "r"}, {0x158, "R"}, {0x159, "r"}, {0x15A, "S"}, {0x15B, "s"}, {0x15C, "S"},
    {0x15D, "s"}, {0x15E, "S"}, {0x15F, "s"}, {0x160, "S"}, {0x161, "s"}, {0x162, "T"}, {0x163, "t"},
    {0x164, "T"}, {0x165, "t"}, {0x166, "T"}, {0x167, "t"}, {0x168, "U"}, {0x169, "u"}, {0x16A, "U"},
    {0x16B, "u"}, {0x16C, "U"}, {0x16D, "u"}, {0x16E, "U"}, {0x16F, "u"}, {0x170, "U"}, {0x171, "u"},
    {0x172, "U"}, {0x173, "u"}, {0x174, "W"}, {0x175, "w"}, {0x176, "Y"}, {0x177, "y"}, {0x178, "Y"},
    {0x179, "Z"}, {0x17A, "z"}, {0x17B, "Z"}, {0x17C, "z"}, {0x17D, "Z"}, {0x17E, "z"}, {0x17F, "s"},
};

// UTF-8 to code points; a malformed byte stands for itself as U+FFFD.
std::vector<char32_t> decode(const std::string& text)
{
    std::vector<char32_t> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        int extra = -1;
        char32_t cp = 0;
        if (lead < 0x80) { extra = 0; cp = lead; }
        else if ((lead >> 5) == 0x6) { extra = 1; cp = lead & 0x1F; }
        else if ((lead >> 4) == 0xE) { extra = 2; cp = lead & 0x0F; }
        else if ((lead >> 3) == 0x1E) { extra = 3; cp = lead & 0x07; }
        bool valid = extra >= 0 && i + static_cast<std::size_t>(extra) < text.size();
        for (int k = 1; valid && k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            valid = (next & 0xC0) == 0x80;
            cp = (cp << 6) | (next & 0x3F);
        }
        if (!valid) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
}

void encode(char32_t cp, std::string& out)
{
    if (cp < 0x80) { out += static_cast<char>(cp); return; }
    if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
           out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); }
    out += static_cast<char>(0x80 | (cp & 0x3F));
}

// Kana with a voiced mark (U+3099) or a semi-voiced one (U+309A) composed,
// as NFC does: the base's code point plus one, or plus two.
char32_t composeKana(char32_t base, char32_t mark)
{
    const auto in = [base](std::initializer_list<char32_t> set) {
        return std::find(set.begin(), set.end(), base) != set.end();
    };
    const auto both = [&](std::initializer_list<char32_t> hiragana) {
        return in(hiragana) || std::any_of(hiragana.begin(), hiragana.end(),
                                           [base](char32_t h) { return h + 0x60 == base; });
    };
    if (mark == 0x3099) {
        if (both({0x304B, 0x304D, 0x304F, 0x3051, 0x3053, 0x3055, 0x3057, 0x3059, 0x305B, 0x305D,
                  0x305F, 0x3061, 0x3064, 0x3066, 0x3068, 0x306F, 0x3072, 0x3075, 0x3078, 0x307B}))
            return base + 1;
        if (base == 0x3046) return 0x3094;   // ゔ
        if (base == 0x30A6) return 0x30F4;   // ヴ
        if (base >= 0x30EF && base <= 0x30F2) return base + 8;   // ヷ-ヺ
    }
    if (mark == 0x309A && both({0x306F, 0x3072, 0x3075, 0x3078, 0x307B})) return base + 2;
    return 0;
}

} // namespace

std::string portableName(const std::string& utf8)
{
    // The composed key (what the hash is taken from) and the visible ASCII.
    std::vector<char32_t> composed;
    for (const char32_t cp : decode(utf8)) {
        if (cp >= 0x0300 && cp <= 0x036F) continue;   // a Latin accent, decomposed: dropped below anyway
        if ((cp == 0x3099 || cp == 0x309A) && !composed.empty()) {
            if (const char32_t kana = composeKana(composed.back(), cp)) { composed.back() = kana; continue; }
        }
        composed.push_back(cp);
    }
    std::string key, out;
    for (const char32_t cp : composed) {
        const auto* latin = std::find_if(std::begin(kLatinAscii), std::end(kLatinAscii),
                                         [cp](const auto& row) { return row.cp == cp; });
        if (latin != std::end(kLatinAscii)) {
            key += latin->ascii;
            out += latin->ascii;
            continue;
        }
        encode(cp, key);
        out += (cp < 0x20 || cp >= 0x7f || std::strchr("<>:\"/\\|?*", static_cast<int>(cp))) ? '_'
                                                                                         : static_cast<char>(cp);
    }
    const auto trimEnd = [&out] {
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    };
    trimEnd();
    if (out.size() > 64) out.resize(64);
    trimEnd();
    std::string device = out.substr(0, out.find('.'));
    for (auto& c : device) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* const kDevices[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
        "LPT8", "LPT9"};
    const bool reserved = std::any_of(std::begin(kDevices), std::end(kDevices),
                                      [&device](const char* d) { return device == d; });
    if (out == utf8 && !reserved && !out.empty()) return out;
    const auto hash = Core::sha256(reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
    return (out.empty() ? std::string("disk") : out) + " [" + hash.substr(0, 8) + "]";
}

std::string portableExtension(const std::string& utf8Extension)
{
    std::string ext;
    for (const unsigned char c : utf8Extension)
        if (std::isalnum(c) || c == '.') ext += static_cast<char>(c);
    return ext.size() > 1 && ext.size() <= 8 && ext.size() == utf8Extension.size() ? ext : std::string();
}

} // namespace pom2::libretro
