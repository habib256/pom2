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

// Text screen capture — see TextScreenCapture.h.

#include "TextScreenCapture.h"

#include <vector>

namespace pom2 {

namespace {

/// `Apple2Display::textRowAddress`, kept in step by `text_screen_capture`,
/// which asserts the two agree rather than trusting the copy.
std::uint16_t rowAddress(int row, bool page2)
{
    const std::uint16_t base = page2 ? 0x0800 : 0x0400;
    return static_cast<std::uint16_t>(base + 0x80 * (row & 7) + 0x28 * (row >> 3));
}

} // namespace

char textScreenChar(std::uint8_t screenByte, bool altCharset)
{
    // `Apple2Display`'s `resolveGlyph`, which is what decides what the
    // renderer draws. Normal video is $80-$FF, inverse $00-$3F and flashing
    // $40-$7F; the two halves of each range select the $40-$5F or the
    // $20-$3F ASCII block, and only $E0-$FF reaches lowercase.
    std::uint8_t ascii;
    if (screenByte & 0x80) {
        // $80-$9F are @A-Z[\]^_ (the $C0 glyphs), not a second space block.
        ascii = static_cast<std::uint8_t>((screenByte & 0x60) == 0
                    ? (screenByte & 0x1F) + 0x40
                    : (screenByte & 0x7F));
    } else if (altCharset && screenByte >= 0x40 && screenByte <= 0x5F) {
        // ALTCHAR puts MouseText here — arrows, a running man, a folder.
        // There is no letter to report.
        return '?';
    } else if (altCharset && screenByte >= 0x60 && screenByte <= 0x7F) {
        // ALTCHAR shows the lowercase glyph of $E0-$FF, as inverse video.
        ascii = static_cast<std::uint8_t>((screenByte & 0x1F) + 0x60);
    } else {
        const std::uint8_t idx6 = screenByte & 0x3F;
        ascii = (idx6 < 0x20) ? static_cast<std::uint8_t>(0x40 + idx6) : idx6;
    }
    // Everything the mapping can produce is $20-$7F, so this only catches a
    // mapping change: a cell is never allowed to emit a control character
    // into the host's clipboard.
    return (ascii >= 0x20 && ascii < 0x7F) ? static_cast<char>(ascii) : '?';
}

std::string captureTextScreenRow(const TextScreenSource& src, int row)
{
    std::string out;
    if (row < 0 || row >= kTextScreenRows || !src.main) return out;
    const std::uint16_t addr = rowAddress(row, src.page2);

    if (src.eightyCols && src.aux) {
        out.reserve(kTextScreenCols80);
        for (int col = 0; col < kTextScreenCols80; ++col) {
            // Aux holds the EVEN columns, main the odd ones, both at col/2.
            const std::uint16_t at =
                static_cast<std::uint16_t>(addr + (col >> 1));
            const std::uint8_t b = (col & 1) ? src.main(at) : src.aux(at);
            out += textScreenChar(b, src.altCharset);
        }
    } else {
        out.reserve(kTextScreenCols40);
        for (int col = 0; col < kTextScreenCols40; ++col)
            out += textScreenChar(
                src.main(static_cast<std::uint16_t>(addr + col)),
                src.altCharset);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string captureTextScreen(const TextScreenSource& src)
{
    std::string out;
    out.reserve(kTextScreenRows * (kTextScreenCols80 + 1));
    for (int row = 0; row < kTextScreenRows; ++row) {
        out += captureTextScreenRow(src, row);
        out += '\n';
    }
    return out;
}

std::string trimCapturedText(const std::string& text)
{
    // Split on '\n' without inventing a trailing empty line for a string
    // that ends in one.
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') { lines.push_back(line); line.clear(); }
        else           { line += c; }
    }
    if (!line.empty()) lines.push_back(line);

    auto blank = [](const std::string& s) {
        for (char c : s) if (c != ' ') return false;
        return true;
    };

    std::size_t first = 0, last = lines.size();
    while (first < last && blank(lines[first])) ++first;
    while (last > first && blank(lines[last - 1])) --last;

    std::string out;
    bool pendingBlank = false;
    for (std::size_t i = first; i < last; ++i) {
        if (blank(lines[i])) { pendingBlank = true; continue; }
        if (pendingBlank) { out += '\n'; pendingBlank = false; }
        out += lines[i];
        out += '\n';
    }
    return out;
}

} // namespace pom2
