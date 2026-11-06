// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// Applesoft tokenizer — see ApplesoftTokenizer.h for the rules and where in
// the ROM each one was read. The body loop below is PARSE.INPUT.LINE
// ($D559) restated: the same tests in the same order, so a reader can hold
// the two side by side. The structure (split lines, tokenize each, sort,
// link) follows [D] Casso CassoCore/ApplesoftTokenizer.cpp @ 2d899adf (MIT);
// where Casso and the ROM differ (AT lookahead, duplicates, bare numbers,
// control characters, line length, `{|}~`), the ROM wins.

#include "ApplesoftTokenizer.h"

#include "ApplesoftListing.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pom2::applesoft {

namespace {

constexpr uint8_t kFirstToken = 0x80;
constexpr uint8_t kLastToken  = 0xEA;
constexpr uint8_t kTokenData  = 0x83;
constexpr uint8_t kTokenRem   = 0xB2;
constexpr uint8_t kTokenPrint = 0xBA;
constexpr uint8_t kTokenAt    = 0xC5;
constexpr std::size_t kNoMatch = static_cast<std::size_t>(-1);

// [C] //e $F79A-$F7A5: `CMP #$61 / BCC / AND #$5F` — everything from 'a' up,
// `{|}~` and DEL included, loses bit 5. Only called where the //e folds:
// outside quotes, REM and DATA (its $F78C tests ENDCHR and DATAFLG first).
uint8_t fold(uint8_t c)
{
    return c >= 0x61 ? static_cast<uint8_t>(c & 0x5F) : c;
}

// One keyword against the text at `at`, blanks skipped before each of its
// letters ([C] $D5A7-$D5AD), letters compared folded ([C] //e $D5A8 → $F78C).
// Returns the index just past the keyword's last character, or kNoMatch.
std::size_t matchKeyword(const std::string& s, std::size_t at, const char* kw)
{
    std::size_t i = at;
    for (const char* k = kw; *k; ++k) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size() || fold(static_cast<uint8_t>(s[i])) != static_cast<uint8_t>(*k))
            return kNoMatch;
        ++i;
    }
    return i;
}

// The statement part of one line, from `i` (just past the line number).
void tokenizeBody(const std::string& s, std::size_t i, std::vector<uint8_t>& out)
{
    bool data = false;   // [C] DATAFLG $13 = $49 after DATA, 0 after ':'
    const std::size_t n = s.size();
    while (i < n) {
        const uint8_t raw = static_cast<uint8_t>(s[i]);
        // [C] $D570-$D576: a blank is dropped, except inside DATA.
        if (!data && raw == ' ') { ++i; continue; }
        // [C] $D57A → $D5F2: a quote copies to its partner or the line end,
        // ahead of the DATA test, so DATA strings are copied the same way.
        if (raw == '"') {
            out.push_back(raw);
            ++i;
            while (i < n) {
                const uint8_t c = static_cast<uint8_t>(s[i++]);
                out.push_back(c);
                if (c == '"') break;
            }
            continue;
        }
        // [C] $D57E BVS: inside DATA, stored as typed; ':' ends it ($D5DA).
        if (data) {
            out.push_back(raw);
            ++i;
            if (raw == ':') data = false;
            continue;
        }
        const uint8_t c = fold(raw);
        // [C] $D580-$D586: '?' is PRINT.
        if (c == '?') { out.push_back(kTokenPrint); ++i; continue; }
        // [C] $D588-$D58E: '0'..';' never start a keyword.
        if (c >= '0' && c <= ';') { out.push_back(c); ++i; continue; }
        // [C] $D590-$D60E: the table, in order, first match wins.
        uint8_t token = 0;
        std::size_t end = kNoMatch;
        for (unsigned t = kFirstToken; t <= kLastToken; ++t) {
            end = matchKeyword(s, i, keyword(static_cast<uint8_t>(t)));
            if (end == kNoMatch) continue;
            if (t == kTokenAt) {
                // [C] $D5BE-$D5C7 (//e: $F787, folded): the character right
                // after the T, blanks NOT skipped. 'N' leaves ATN to match
                // later in the table; 'O' leaves A to be a letter so TO can.
                const uint8_t next = end < n ? fold(static_cast<uint8_t>(s[end])) : 0;
                if (next == 'N' || next == 'O') { end = kNoMatch; continue; }
            }
            token = static_cast<uint8_t>(t);
            break;
        }
        if (end == kNoMatch) {
            // [C] $D60B: no keyword — the character itself, folded.
            out.push_back(c);
            ++i;
            continue;
        }
        out.push_back(token);
        i = end;
        if (token == kTokenData) data = true;          // [C] $D5DC-$D5E0
        if (token == kTokenRem) {                      // [C] $D5E2-$D5F7
            while (i < n) out.push_back(static_cast<uint8_t>(s[i++]));
        }
    }
}

struct Line {
    std::vector<uint8_t> body;
    int sourceLine = 0;
};

std::string hexByte(uint8_t b)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "$%02X", static_cast<unsigned>(b));
    return buf;
}

} // namespace

bool tokenize(const std::string& textIn, std::vector<uint8_t>& program,
              std::vector<TokenizeError>& errors, uint16_t loadAddress)
{
    program.clear();
    errors.clear();

    // A UTF-8 byte-order mark is an editor's, not the program's.
    std::size_t start = 0;
    if (textIn.size() >= 3 && static_cast<uint8_t>(textIn[0]) == 0xEF
        && static_cast<uint8_t>(textIn[1]) == 0xBB && static_cast<uint8_t>(textIn[2]) == 0xBF)
        start = 3;

    std::map<unsigned, Line> lines;   // sorted by number, as Applesoft keeps them
    int sourceLine = 0;
    std::size_t pos = start;
    while (pos < textIn.size()) {
        std::size_t eol = textIn.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = textIn.size();
        const std::string s = textIn.substr(pos, eol - pos);
        pos = eol;
        if (pos < textIn.size()) {
            if (textIn[pos] == '\r' && pos + 1 < textIn.size() && textIn[pos + 1] == '\n') ++pos;
            ++pos;
        }
        ++sourceLine;

        if (s.find_first_not_of(" \t") == std::string::npos) continue;   // blank line
        const auto fail = [&](const std::string& why) {
            errors.push_back(TokenizeError{sourceLine, why});
        };

        bool bad = false;
        for (std::size_t k = 0; k < s.size(); ++k) {
            const uint8_t b = static_cast<uint8_t>(s[k]);
            if (b == 0 || b >= 0x80) {
                fail("byte " + hexByte(b) + " at column " + std::to_string(k + 1)
                     + " has no Apple II representation");
                bad = true;
                break;
            }
        }
        if (bad) continue;

        // [C] LINGET $DA0C via CHRGET: blanks skipped before and between the
        // digits; a digit arriving at 6400 or more is ?SYNTAX ERROR.
        std::size_t i = 0;
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            fail("no line number");
            continue;
        }
        unsigned number = 0;
        bool range = true;
        while (i < s.size()) {
            if (s[i] == ' ') { ++i; continue; }
            if (s[i] < '0' || s[i] > '9') break;
            if (number >= 6400) { range = false; break; }
            number = number * 10 + static_cast<unsigned>(s[i] - '0');
            ++i;
        }
        if (!range) {
            fail("line number out of range (Applesoft accepts 0 to "
                 + std::to_string(kMaxLineNumber) + ")");
            continue;
        }

        Line line;
        line.sourceLine = sourceLine;
        tokenizeBody(s, i, line.body);
        if (line.body.empty()) {               // [C] $D4B5: a bare number deletes
            lines.erase(number);
            continue;
        }
        if (line.body.size() > kMaxLineBody) {
            fail("line " + std::to_string(number) + " tokenizes to "
                 + std::to_string(line.body.size()) + " bytes; Applesoft stores at most "
                 + std::to_string(kMaxLineBody) + " (a typed line holds 239 characters)");
            continue;
        }
        lines[number] = std::move(line);       // [C] $D46C: a repeated number replaces
    }

    // Links are absolute: each is the address of the next line, the last one
    // that of the closing $00 $00.
    std::vector<uint8_t> out;
    uint32_t address = loadAddress;
    for (const auto& [number, line] : lines) {
        const uint32_t next = address + 4 + static_cast<uint32_t>(line.body.size()) + 1;
        if (next + 2 > 0x10000) {
            errors.push_back(TokenizeError{line.sourceLine,
                "line " + std::to_string(number) + " runs past $FFFF from load address "
                + hexByte(static_cast<uint8_t>(loadAddress >> 8))
                + hexByte(static_cast<uint8_t>(loadAddress)).substr(1)});
            break;
        }
        out.push_back(static_cast<uint8_t>(next & 0xFF));
        out.push_back(static_cast<uint8_t>(next >> 8));
        out.push_back(static_cast<uint8_t>(number & 0xFF));
        out.push_back(static_cast<uint8_t>(number >> 8));
        out.insert(out.end(), line.body.begin(), line.body.end());
        out.push_back(0x00);
        address = next;
    }
    if (!errors.empty()) return false;
    out.push_back(0x00);
    out.push_back(0x00);
    program = std::move(out);
    return true;
}

} // namespace pom2::applesoft
