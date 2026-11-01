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

// Integer BASIC listing — see IntegerBasicListing.h.

#include "IntegerBasicListing.h"

#include <cctype>

namespace pom2::integerbasic {

namespace {

// Integer BASIC's token table, index = the token byte, $00-$7F.
// CiderPress `reformat/BASIC.cpp` (`gIntegerTokens`, crediting Paul
// Schlyter), cross-checked against a2kit `lang/integer/tokenizer.rs` and
// against the programs on disks_5.4/dsk/saturn128k_software.dsk:
//   $4D CALL  $36 -  $61/$62/$63 PRINT  $2E PEEK  $3F (
//   $24 THEN  $16 =  $72 )  $5D REM  $28/$29 "
// $00-$11 are the immediate commands (LOAD, SAVE, RUN, DEL, NEW, AUTO, MAN,
// HIMEM:, LOMEM:) — a stored program never holds one.
constexpr const char* kTokens[128] = {
    "HIMEM:",  "<EOL>",  "_ ",      ":",
    "LOAD ",   "SAVE ",  "CON ",    "RUN ",
    "RUN ",    "DEL ",   ",",       "NEW ",
    "CLR ",    "AUTO ",  ",",       "MAN ",
    "HIMEM:",  "LOMEM:", "+",       "-",
    "*",       "/",      "=",       "#",
    ">=",      ">",      "<=",      "<>",
    "<",       "AND ",   "OR ",     "MOD ",
    "^ ",      "+",      "(",       ",",
    "THEN ",   "THEN ",  ",",       ",",
    "\"",      "\"",     "(",       "!",
    "!",       "(",      "PEEK ",   "RND ",
    "SGN ",    "ABS ",   "PDL ",    "RNDX ",
    "(",       "+",      "-",       "NOT ",
    "(",       "=",      "#",       "LEN(",
    "ASC( ",   "SCRN( ", ",",       "(",
    "$",       "$",      "(",       ",",
    ",",       ";",      ";",       ";",
    ",",       ",",      ",",       "TEXT ",
    "GR ",     "CALL ",  "DIM ",    "DIM ",
    "TAB ",    "END ",   "INPUT ",  "INPUT ",
    "INPUT ",  "FOR ",   "=",       "TO ",
    "STEP ",   "NEXT ",  ",",       "RETURN ",
    "GOSUB ",  "REM ",   "LET ",    "GOTO ",
    "IF ",     "PRINT ", "PRINT ",  "PRINT ",
    "POKE ",   ",",      "COLOR= ", "PLOT ",
    ",",       "HLIN ",  ",",       "AT ",
    "VLIN ",   ",",      "AT ",     "VTAB ",
    "=",       "=",      ")",       ")",
    "LIST ",   ",",      "LIST ",   "POP ",
    "NODSP ",  "NODSP ", "NOTRACE ","DSP ",
    "DSP ",    "TRACE ", "PR# ",    "IN# ",
};
static_assert(sizeof kTokens / sizeof kTokens[0] == 0x80, "tokens $00-$7F");

constexpr uint8_t kEol       = 0x01;
constexpr uint8_t kQuoteOpen = 0x28;
constexpr uint8_t kQuoteClose= 0x29;
constexpr uint8_t kRem       = 0x5D;

// A character as LIST would type it. Same rule as ApplesoftListing: a
// control character in a string IS the program (a Ctrl-D before `PRINT`
// drives DOS), so it goes out as it is; only CR and LF, which would cut the
// line, become '?'.
void appendChar(std::string& out, uint8_t b)
{
    const char c = char(b & 0x7F);
    const bool keep = (c >= 0x20 && c < 0x7F)
                   || (c >= 0x01 && c < 0x20 && c != 0x0A && c != 0x0D);
    out += keep ? c : '?';
}

} // namespace

const char* keyword(uint8_t t)
{
    return t < 0x80 ? kTokens[t] : nullptr;
}

bool looksLikeProgram(uint16_t start,
                      const std::function<uint8_t(uint16_t)>& peek,
                      std::size_t bytes)
{
    if (bytes < 4 || start + bytes > 0x10000) return false;
    const uint32_t end = start + static_cast<uint32_t>(bytes);
    uint32_t at = start;
    unsigned lines = 0;
    while (at < end) {
        if (end - at < 4) return false;
        const uint8_t len = peek(uint16_t(at));
        if (len < 4) return false;
        if (at + len > end) return false;
        // Every line ends in the end-of-line token.
        if (peek(uint16_t(at + len - 1)) != kEol) return false;
        at += len;
        if (++lines > 0x4000) return false;      // cannot happen; no runaway
    }
    return at == end && lines != 0;
}

std::string list(uint16_t start, const std::function<uint8_t(uint16_t)>& peek,
                 std::size_t maxBytes)
{
    std::string out;
    uint32_t at = start;
    const uint32_t end = start + static_cast<uint32_t>(maxBytes);
    auto byteAt = [&](uint32_t a) { return peek(uint16_t(a)); };

    while (at + 4 <= end && at + 4 <= 0x10000) {
        const uint8_t len = byteAt(at);
        // The shortest possible line is `len lo hi $01` = 4 bytes. A length
        // that cannot be one ends the walk rather than resyncing: past the
        // last line is whatever the file happens to hold, and a damaged
        // program must not send the listing wandering.
        if (len < 4) break;
        const uint32_t lineEnd = at + len;
        if (lineEnd > end || lineEnd > 0x10000) break;

        out += std::to_string(uint16_t(byteAt(at + 1) | byteAt(at + 2) << 8));
        out += ' ';

        bool inString = false;
        bool inRem    = false;
        // Whether what was written last already ends in a space. A keyword
        // needs one either side or a constant runs into it — the table's
        // "THEN " has a trailing space and no leading one, so
        // `IF PEEK (-16640)=141 THEN 200` listed as `…=141THEN 200`. Same
        // rule, and same reason, as ApplesoftListing.
        bool spaced = true;
        uint32_t p = at + 3;
        while (p < lineEnd) {
            const uint8_t b = byteAt(p);
            // $01 ends the line in EVERY mode. Inside a REM it is the
            // only thing that does, and leaving it to the character path
            // appended a literal $01 to the text (`appendChar` keeps the
            // control characters on purpose) — caught by
            // `integer_basic_listing`, which has a REM; the real programs
            // POM2's format notes came from have none.
            if (b == kEol) break;
            if (inRem) { appendChar(out, b); ++p; spaced = (b & 0x7F) == ' '; continue; }
            if (inString) {
                if (b == kQuoteClose) {
                    out += '"'; inString = false; spaced = false; ++p; continue;
                }
                appendChar(out, b);
                spaced = (b & 0x7F) == ' ';
                ++p;
                continue;
            }
            if (b < 0x80) {
                const char* kw = kTokens[b];
                if (std::isalpha(static_cast<unsigned char>(kw[0])) && !spaced)
                    out += ' ';
                out += kw;
                spaced = *kw && kw[std::char_traits<char>::length(kw) - 1] == ' ';
                if (b == kQuoteOpen) inString = true;
                if (b == kRem)       inRem    = true;
                ++p;
                continue;
            }
            // A variable name: it runs on while the bytes stay >= $80, which
            // is what keeps a digit INSIDE a name (`A1` → `C1 B1`) from
            // being read as the start of a constant.
            if (b >= 0xC1 && b <= 0xDA) {
                while (p < lineEnd && byteAt(p) >= 0x80) appendChar(out, byteAt(p++));
                spaced = false;
                continue;
            }
            // An integer constant: the marker is the first digit typed and
            // carries no value; the two bytes after it are the number.
            if (b >= 0xB0 && b <= 0xB9) {
                if (p + 3 > lineEnd) { appendChar(out, b); ++p; spaced = false; continue; }
                out += std::to_string(
                    uint16_t(byteAt(p + 1) | byteAt(p + 2) << 8));
                spaced = false;
                p += 3;
                continue;
            }
            appendChar(out, b);   // $BA-$C0, $DB-$FF outside a string
            spaced = (b & 0x7F) == ' ';
            ++p;
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        out += '\n';
        at = lineEnd;
    }
    return out;
}

} // namespace pom2::integerbasic
