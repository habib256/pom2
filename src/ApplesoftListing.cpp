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
                 std::size_t maxBytes, ListReport* report)
{
    constexpr uint8_t kData = 0x83, kRem = 0xB2;
    if (report) *report = ListReport{};
    std::string out;
    uint32_t at = start;
    const uint32_t end = start + static_cast<uint32_t>(maxBytes);
    auto word = [&](uint32_t a) { return uint16_t(peek(uint16_t(a)) | peek(uint16_t(a + 1)) << 8); };
    while (at + 4 <= end && at + 4 <= 0x10000) {
        const uint16_t next = word(at);
        if (next == 0) break;
        const uint16_t lineNumber = word(at + 2);
        out += std::to_string(lineNumber);
        out += ' ';
        uint32_t p = at + 4;
        bool spaced = true;    // the last thing written ends in a space
        bool ours   = true;    // …and this listing put that space there
        bool quoted = false;   // inside "…"
        bool rem    = false;   // after REM: verbatim to the end of the line
        bool data   = false;   // inside a DATA statement: verbatim to its ':'
        const auto lost = [&] {
            if (!report) return;
            if (report->untypable++ == 0) report->firstLine = lineNumber;
        };
        while (p < end && p < 0x10000) {
            const uint8_t b = peek(uint16_t(p++));
            if (b == 0) break;
            const char* kw = keyword(b);
            if (kw) {
                // A token where the tokeniser never puts one: its keyword is
                // written, and typed back it is only letters.
                if (quoted || rem || data) lost();
                // Words get a space either side; the operators stay tight.
                if (std::isalpha(static_cast<unsigned char>(kw[0])) || kw[0] == '&') {
                    if (!spaced) out += ' ';
                    out += kw;
                    // …except after REM and DATA, whose payload is stored
                    // with every space it was typed with (see the header).
                    if (!quoted && !rem && !data && (b == kRem || b == kData)) {
                        (b == kRem ? rem : data) = true;
                        spaced = ours = false;
                    } else {
                        out += ' ';
                        spaced = ours = true;
                    }
                } else {
                    out += kw;
                    spaced = ours = false;
                }
                continue;
            }
            if (b >= 0x80) lost();                 // above the last token
            // Control characters go out as they are, as LIST types them: a
            // string's Ctrl-D (PRINT "<Ctrl-D>BLOAD ...") is the program, and
            // a '?' in its place listed another one. CR and LF alone would
            // cut the line, so they keep the '?'.
            const char c = char(b & 0x7F);
            const bool keep = (c >= 0x20 && c < 0x7F)
                           || (c >= 0x01 && c < 0x20 && c != 0x0A && c != 0x0D);
            if (!keep && b < 0x80) lost();         // CR, LF, DEL
            out += keep ? c : '?';
            spaced = c == ' ';
            ours   = false;
            if (c == '"' && !rem) quoted = !quoted;
            else if (c == ':' && !quoted && !rem) data = false;
        }
        // Only the space this listing added after a last keyword goes: one
        // the program holds — the end of a REM, of a DATA item, of a string
        // left open — is part of it, and stripping it listed another program.
        if (spaced && ours && !out.empty() && out.back() == ' ') out.pop_back();
        out += '\n';
        // Lines are contiguous, so the walk follows the bytes, not the link:
        // in a DOS file the links point where the program WILL load, and a
        // damaged one cannot send the listing round in a loop.
        at = p;
    }
    return out;
}

} // namespace pom2::applesoft
