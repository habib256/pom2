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

// TextScreenCapture — what the text screen SAYS, as host text.
//
// The point is testing. Validating a program's text output by comparing
// pictures means a golden image per screen, a tolerance, and a diff nobody
// can read; comparing `"ENTER SLOT# OF APPLESOFT CARD "` against a string is
// one line. So this reads the 40- or 80-column text screen out of RAM and
// hands back the characters, for the clipboard (File ▸ Copy screen text),
// for `GET /screen/text` on the AI control server, and for
// `pom2_headless --screen-text`.
//
// **It reads RAM, not pixels.** No glyph matching, no character ROM, no
// shader — so it is unaffected by the display pipeline, the CRT effects, the
// monitor tint or a card's own picture, and it works while the machine is
// paused. What it cannot see, equally: anything a card paints itself (a Videx
// Videoterm's own 80 columns live in the CARD's RAM, not the Apple's), and
// anything in a graphics mode. It is the Apple's text page, nothing else.
//
// Addressing follows the motherboard video scanner. Character decoding is
// a conventional ASCII interpretation, not a decode of a custom character ROM:
//
//   * rows interleave as `base + $80*(y%8) + $28*(y/8)`, base $400 or $800
//     (`Apple2Display::textRowAddress`);
//   * page 2 only when PAGE2 is set and 80STORE is clear
//     (`videoTextPage2`, Sather § 5-25 table 5.10);
//   * 80 columns iff the machine is a //e-class and 80COL is on, and then
//     **aux RAM holds the EVEN columns, main RAM the odd ones**, both at
//     offset `col/2` (AppleWin's scanner convention, as `paintText80`);
//   * inverse/flashing attributes are discarded; MouseText becomes `?`.
// International/custom character ROM glyphs and raster splits are not decoded.

#ifndef POM2_TEXT_SCREEN_CAPTURE_H
#define POM2_TEXT_SCREEN_CAPTURE_H

#include <cstdint>
#include <functional>
#include <string>

class Memory;

namespace pom2 {

inline constexpr int kTextScreenRows    = 24;
inline constexpr int kTextScreenCols40  = 40;
inline constexpr int kTextScreenCols80  = 80;

/// Where the characters are, and how to read them. A `std::function` rather
/// than a `Memory&` so the whole thing is testable with no machine, and so
/// the caller decides what it is safe to read under which lock.
struct TextScreenSource {
    std::function<std::uint8_t(std::uint16_t)> main;   ///< main RAM
    std::function<std::uint8_t(std::uint16_t)> aux;    ///< aux RAM (80 col)
    bool page2      = false;   ///< `videoTextPage2(state)`, NOT raw PAGE2
    bool eightyCols = false;   ///< `isIIE() && state.eightyCol`
    bool altCharset = false;   ///< `state.altChar`
};

/// Conventional ASCII interpretation of `screenByte` (ignores glyph ROM).
///
/// `'?'` for a cell with no ASCII equivalent, which under ALTCHAR means the
/// MouseText range `$40-$5F` — those glyphs are arrows, a running man and a
/// folder, not letters, and guessing a letter for them would read back as
/// text the screen never said.
char textScreenChar(std::uint8_t screenByte, bool altCharset);

/// One row, `0`-`23`: 40 or 80 characters with trailing blanks removed.
std::string captureTextScreenRow(const TextScreenSource& src, int row);

/// All 24 rows, each with its trailing blanks removed and ending in `'\n'`.
/// Interior blank lines are kept — `trimCapturedText` is the separate step.
std::string captureTextScreen(const TextScreenSource& src);

/// Current motherboard text state, not the last rendered frame or card output.
/// Rows are 24 in TEXT, 4 in MIXED (firstRow=20), zero in pure graphics.
/// Each captured row ends in '\n'; only trailing spaces within rows are removed.
struct CapturedScreenText {
    std::string text;
    int columns = 40;
    int rows = 0;
    int firstRow = 0;
    int page = 1;
};

/// Caller must hold the controller state lock if a CPU worker can run.
/// Reads RAM directly, without triggering bus side effects or read watches.
CapturedScreenText captureTextScreen(const Memory& memory);

/// Cut a capture down to what was actually written: no leading blank lines,
/// no trailing blank lines, and a run of blank lines inside collapsed to one.
///
/// The shape is the print spool's trim in § 5.3 of the Host File Transfer
/// Card (sbabcoc/ReplicApple2Plus) — "from the end, any bytes after the last
/// carriage return, then repeated carriage returns down to one; then, from
/// the start, every leading carriage return" — read one level up, in lines
/// rather than bytes, because here a line is the unit. It exists for the
/// same reason: a 24-row screen holding four lines of output should compare
/// against four lines, not against four lines and twenty blanks.
///
/// Nothing inside a line is touched, so a deliberately blank-padded column
/// of a table survives.
std::string trimCapturedText(const std::string& text);

} // namespace pom2

#endif // POM2_TEXT_SCREEN_CAPTURE_H
