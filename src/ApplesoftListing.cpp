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

// Applesoft listing — see ApplesoftListing.h.

#include "ApplesoftListing.h"

#include <cctype>

namespace pom2::applesoft {

namespace {

// Applesoft's keyword table, tokens $80-$EA in order (Applesoft II BASIC
// Programming Reference Manual, Appendix F; the ROM's TOKEN.NAME.TABLE).
constexpr const char* kKeywords[] = {
    "END", "FOR", "NEXT", "DATA", "INPUT", "DEL", "DIM", "READ",
    "GR", "TEXT", "PR#", "IN#", "CALL", "PLOT", "HLIN", "VLIN",
    "HGR2", "HGR", "HCOLOR=", "HPLOT", "DRAW", "XDRAW", "HTAB", "HOME",
    "ROT=", "SCALE=", "SHLOAD", "TRACE", "NOTRACE", "NORMAL", "INVERSE", "FLASH",
    "COLOR=", "POP", "VTAB", "HIMEM:", "LOMEM:", "ONERR", "RESUME", "RECALL",
    "STORE", "SPEED=", "LET", "GOTO", "RUN", "IF", "RESTORE", "&",
    "GOSUB", "RETURN", "REM", "STOP", "ON", "WAIT", "LOAD", "SAVE",
    "DEF", "POKE", "PRINT", "CONT", "LIST", "CLEAR", "GET", "NEW",
    "TAB(", "TO", "FN", "SPC(", "THEN", "AT", "NOT", "STEP",
    "+", "-", "*", "/", "^", "AND", "OR", ">",
    "=", "<", "SGN", "INT", "ABS", "USR", "FRE", "SCRN(",
    "PDL", "POS", "SQR", "RND", "LOG", "EXP", "COS", "SIN",
    "TAN", "ATN", "PEEK", "LEN", "STR$", "VAL", "ASC", "CHR$",
    "LEFT$", "RIGHT$", "MID$",
};
static_assert(sizeof kKeywords / sizeof kKeywords[0] == 0xEB - 0x80, "tokens $80-$EA");

} // namespace

const char* keyword(uint8_t t)
{
    return (t >= 0x80 && t <= 0xEA) ? kKeywords[t - 0x80] : nullptr;
}

std::string list(uint16_t start, const std::function<uint8_t(uint16_t)>& peek,
                 std::size_t maxBytes)
{
    std::string out;
    uint32_t at = start;
    const uint32_t end = start + static_cast<uint32_t>(maxBytes);
    auto word = [&](uint32_t a) { return uint16_t(peek(uint16_t(a)) | peek(uint16_t(a + 1)) << 8); };
    while (at + 4 <= end && at + 4 <= 0x10000) {
        const uint16_t next = word(at);
        if (next == 0) break;
        out += std::to_string(word(at + 2));
        out += ' ';
        uint32_t p = at + 4;
        bool spaced = true;    // the last thing written ends in a space
        while (p < end && p < 0x10000) {
            const uint8_t b = peek(uint16_t(p++));
            if (b == 0) break;
            if (const char* kw = keyword(b)) {
                // Words get a space either side; the operators stay tight.
                if (std::isalpha(static_cast<unsigned char>(kw[0])) || kw[0] == '&') {
                    if (!spaced) out += ' ';
                    out += kw;
                    out += ' ';
                    spaced = true;
                } else {
                    out += kw;
                    spaced = false;
                }
                continue;
            }
            const char c = char(b & 0x7F);
            out += (c >= 0x20 && c < 0x7F) ? c : '?';
            spaced = c == ' ';
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        out += '\n';
        // Lines are contiguous, so the walk follows the bytes, not the link:
        // in a DOS file the links point where the program WILL load, and a
        // damaged one cannot send the listing round in a loop.
        at = p;
    }
    return out;
}

} // namespace pom2::applesoft
