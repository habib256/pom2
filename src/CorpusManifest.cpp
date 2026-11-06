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

#include "CorpusManifest.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace pom2::corpus {

bool Title::hasTag(const std::string& tag) const
{
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

namespace {

// Whitespace-separated words; a "double-quoted" word may hold spaces and the
// escapes \r \n \t \\ \". A '#' outside quotes starts a comment.
bool tokenize(const std::string& line, std::vector<std::string>& words,
              std::string& error)
{
    words.clear();
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '#') break;
        std::string w;
        if (c == '"') {
            ++i;
            bool closed = false;
            while (i < line.size()) {
                char d = line[i++];
                if (d == '"') { closed = true; break; }
                if (d == '\\' && i < line.size()) {
                    const char e = line[i++];
                    switch (e) {
                    case 'r': d = '\r'; break;
                    case 'n': d = '\n'; break;
                    case 't': d = '\t'; break;
                    case '\\': d = '\\'; break;
                    case '"': d = '"'; break;
                    default:
                        error = std::string("unknown escape \\") + e;
                        return false;
                    }
                }
                w.push_back(d);
            }
            if (!closed) { error = "unterminated quoted string"; return false; }
        } else {
            while (i < line.size() &&
                   !std::isspace(static_cast<unsigned char>(line[i])) && line[i] != '#')
                w.push_back(line[i++]);
        }
        words.push_back(std::move(w));
    }
    return true;
}

bool parseInt(const std::string& s, int lo, int hi, int& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (*end != '\0' || v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

bool isHex64(const std::string& s)
{
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c)) &&
                      !std::isupper(static_cast<unsigned char>(c));
           });
}

}  // namespace

bool parseKeyToken(const std::string& token, std::uint8_t& out)
{
    static const std::pair<const char*, std::uint8_t> kNames[] = {
        {"RETURN", 0x0D}, {"LEFT", 0x08}, {"RIGHT", 0x15}, {"UP", 0x0B},
        {"DOWN", 0x0A},   {"ESC", 0x1B},  {"SPACE", 0x20}, {"TAB", 0x09},
        {"DELETE", 0x7F},
    };
    for (const auto& [name, code] : kNames)
        if (token == name) { out = code; return true; }
    if (token.size() == 1 && token[0] > 0x20 && token[0] < 0x7F) {
        out = static_cast<std::uint8_t>(token[0]);
        return true;
    }
    if (token.size() == 3 && token[0] == '$' &&
        std::isxdigit(static_cast<unsigned char>(token[1])) &&
        std::isxdigit(static_cast<unsigned char>(token[2]))) {
        const unsigned v = static_cast<unsigned>(std::strtoul(token.c_str() + 1, nullptr, 16));
        if (v > 0x7F) return false;
        out = static_cast<std::uint8_t>(v);
        return true;
    }
    return false;
}

bool parseManifest(std::istream& in, Manifest& out, std::string& error)
{
    out = {};
    std::string line;
    int lineNo = 0;
    std::set<std::string> ids;
    std::set<std::string> checks;   // of the current title
    auto fail = [&](const std::string& why) {
        error = "line " + std::to_string(lineNo) + ": " + why;
        return false;
    };
    auto finishTitle = [&]() {
        if (out.titles.empty()) return true;
        const Title& t = out.titles.back();
        if (checks.empty()) {
            error = "line " + std::to_string(t.line) + ": title '" + t.id +
                    "' has no check";
            return false;
        }
        return true;
    };
    std::vector<std::string> w;
    while (std::getline(in, line)) {
        ++lineNo;
        std::string why;
        if (!tokenize(line, w, why)) return fail(why);
        if (w.empty()) continue;
        const std::string& kw = w[0];

        if (kw == "title") {
            if (!finishTitle()) return false;
            if (w.size() < 3) return fail("title needs an id and a profile");
            if (!ids.insert(w[1]).second) return fail("duplicate title '" + w[1] + "'");
            Title t;
            t.id = w[1];
            t.profile = w[2];
            t.tags.assign(w.begin() + 3, w.end());
            t.line = lineNo;
            out.titles.push_back(std::move(t));
            checks.clear();
            continue;
        }
        if (out.titles.empty()) return fail("'" + kw + "' before any title");
        Title& t = out.titles.back();

        if (kw == "diskii" || kw == "smartport" || kw == "hdd") {
            if (w.size() < 3 || w.size() > 4)
                return fail(kw + " takes a drive (1 or 2), a path and an optional sha256=");
            Device d;
            d.kind = kw;
            d.line = lineNo;
            if (!parseInt(w[1], 1, 2, d.unit)) return fail("drive must be 1 or 2");
            d.media = w[2];
            if (w.size() == 4) {
                if (w[3].rfind("sha256=", 0) != 0 || !isHex64(w[3].substr(7)))
                    return fail("expected sha256=<64 lowercase hex digits>");
                d.sha256 = w[3].substr(7);
            }
            for (const Device& other : t.devices)
                if (other.kind == kw && other.unit == d.unit)
                    return fail(kw + " drive " + w[1] + " given twice");
            t.devices.push_back(std::move(d));
            continue;
        }
        if (kw == "mockingboard" || kw == "mouse" || kw == "rgb") {
            if (w.size() < 2 || w.size() > 3)
                return fail(kw + " takes a slot and an optional model");
            Device d;
            d.kind = kw;
            d.line = lineNo;
            if (!parseInt(w[1], 1, 7, d.slot)) return fail("slot must be 1-7");
            if (w.size() == 3) d.model = w[2];
            static const std::map<std::string, std::set<std::string>> kModels = {
                {"mockingboard", {"ac", "soundii"}},
                {"mouse", {"apple", "applewin"}},
                {"rgb", {"feline", "iic"}},
            };
            if (!d.model.empty() && !kModels.at(kw).count(d.model))
                return fail("unknown " + kw + " model '" + d.model + "'");
            t.devices.push_back(std::move(d));
            continue;
        }
        if (kw == "boot") {
            if (w.size() != 2) return fail("boot takes a slot or 'cold'");
            if (w[1] == "cold") t.bootSlot = 0;
            else if (!parseInt(w[1], 1, 7, t.bootSlot)) return fail("boot slot must be 1-7");
            continue;
        }
        Step s;
        s.line = lineNo;
        if (kw == "run") {
            if (w.size() != 2) return fail("run takes emulated seconds");
            char* end = nullptr;
            s.seconds = std::strtod(w[1].c_str(), &end);
            if (*end != '\0' || !(s.seconds > 0) || s.seconds > 36000)
                return fail("run needs a positive number of seconds");
            s.kind = Step::Kind::Run;
        } else if (kw == "key") {
            if (w.size() != 2) return fail("key takes one key");
            if (!parseKeyToken(w[1], s.key)) return fail("unknown key '" + w[1] + "'");
            s.kind = Step::Kind::Key;
        } else if (kw == "type") {
            if (w.size() != 2) return fail("type takes one quoted string");
            s.kind = Step::Kind::Text;
            s.text = w[1];
        } else if (kw == "check") {
            if (w.size() != 2) return fail("check takes a name");
            if (!checks.insert(w[1]).second) return fail("duplicate check '" + w[1] + "'");
            s.kind = Step::Kind::Check;
            s.text = w[1];
        } else {
            return fail("unknown keyword '" + kw + "'");
        }
        t.steps.push_back(std::move(s));
    }
    return finishTitle();
}

bool parseExpectations(std::istream& in, Expectations& out, std::string& error)
{
    out.clear();
    std::string line;
    int lineNo = 0;
    std::vector<std::string> w;
    while (std::getline(in, line)) {
        ++lineNo;
        std::string why;
        if (!tokenize(line, w, why) ) {
            error = "line " + std::to_string(lineNo) + ": " + why;
            return false;
        }
        if (w.empty()) continue;
        if (w.size() != 4 || w[2].size() != 16 || w[3].rfind("fw=", 0) != 0) {
            error = "line " + std::to_string(lineNo) +
                    ": expected '<title> <check> <16-hex hash> fw=<firmware>'";
            return false;
        }
        if (!out.emplace(std::make_pair(w[0], w[1]), Expected{w[2], w[3].substr(3)}).second) {
            error = "line " + std::to_string(lineNo) + ": " + w[0] + " " + w[1] +
                    " recorded twice";
            return false;
        }
    }
    return true;
}

std::string formatExpectations(const Expectations& expectations)
{
    std::ostringstream o;
    o << "# POM2 compatibility corpus - recorded frames (VALIDATION-006).\n"
         "# Written by `pom2_corpus --bless`; see tests/corpus/README.md.\n"
         "# <title> <check> <frame hash> fw=<firmware fingerprint>\n";
    for (const auto& [key, e] : expectations)
        o << key.first << ' ' << key.second << ' ' << e.hash << " fw=" << e.firmware << '\n';
    return o.str();
}

std::uint64_t frameHash(const std::uint32_t* pixels, int width, int height)
{
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](std::uint32_t v) {
        for (int b = 0; b < 4; ++b) {
            h ^= (v >> (8 * b)) & 0xFFu;
            h *= 1099511628211ULL;
        }
    };
    mix(static_cast<std::uint32_t>(width));
    mix(static_cast<std::uint32_t>(height));
    if (!pixels || width <= 0 || height <= 0) return h;
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < n; ++i) mix(pixels[i]);
    return h;
}

std::string hashHex(std::uint64_t h)
{
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

}  // namespace pom2::corpus
