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

// IntegerBasicListing — a tokenised Integer BASIC program as text, the
// sibling of ApplesoftListing.h. POM2 ships an `Apple ][ Original (1977)`
// profile whose BASIC *is* Integer BASIC, and until 2026-11-01 nothing could
// read one: `pom2_dos33 list` refused anything but an `A` file.
//
// A program is a run of lines, each one:
//
//     <length>  <line number, 2 bytes LE>  <item> ...  <$01>
//
// `length` counts the WHOLE line, its own byte and the closing $01 included,
// so the next line starts at `lineStart + length` — measured on the two
// Integer BASIC programs of `disks_5.4/dsk/saturn128k_software.dsk`:
// `09 05 00 | 4D 36 B9 A8 03 | 01` is line 5, nine bytes, `CALL -936`
// (`B9 A8 03` is the constant: the marker, then 936 as $03A8 — not a
// `:` token, which is what a first reading of those bytes suggested).
//
// An item is one of:
//   * $01        end of line;
//   * $02-$7F    a language token (`kTokens`, index = the byte);
//   * $B0-$B9    an integer constant: the marker is the first DIGIT the user
//                typed and carries no value, the two bytes after it are the
//                number, LE. `B1 8D 00` is 141;
//   * $C1-$DA    the first letter of a variable name, which runs on while
//                the bytes stay >= $80 (so `A1`'s `$B1` is a NAME character,
//                not a constant — the one place the three ranges overlap);
//   * >= $80     elsewhere, a character: ASCII with bit 7 set.
//
// Two token values change how what follows is read, and both must be
// tracked or digits inside them decode as constants:
//   * $28 opens a string constant, $29 closes it;
//   * $5D (REM) makes the rest of the line text.
//
// Sources: the format and the token table agree across CiderPress's
// `reformat/BASIC.cpp` (which credits Paul Schlyter) and a2kit's
// `lang/integer/tokenizer.rs`, and were then checked byte for byte against
// the two programs above. **Not sourced:** the exact spacing real Integer
// BASIC `LIST` puts around each keyword. The table's trailing spaces are
// CiderPress's rendering; POM2 keeps them because they make the text
// re-typable, which is what this is for. A listing captured off the screen
// would also right-align the line number in five columns — POM2 does not,
// for the same reason `ApplesoftListing` does not pad: this is source to
// type back, not a screen transcript.

#ifndef POM2_INTEGER_BASIC_LISTING_H
#define POM2_INTEGER_BASIC_LISTING_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace pom2::integerbasic {

/// Keyword of token `t`, or nullptr when `t` is not in $00-$7F.
/// $00-$11 are the immediate commands, which never appear in a stored
/// program; they are in the table for completeness.
const char* keyword(uint8_t t);

/// Whether `bytes` from `start` are a well-formed Integer BASIC program:
/// the chain of length bytes walks from `start` and lands EXACTLY on
/// `start + bytes`, every line being at least 4 bytes and ending in $01.
///
/// This exists because the pointers that say where a program is are plain
/// zero-page bytes, and on a machine that is not running Integer BASIC they
/// hold something else entirely. Landing exactly on the end is a strong
/// check: random bytes almost never do.
///
/// The pointers themselves, for a caller reading RAM (Woz, "Apple II Basic
/// Structure", callapple.org, and the Apple II memory map): **PP** at
/// `$CA/$CB` is the program's first byte — its LOWEST-numbered line, and it
/// DECREASES as the program grows, because Integer BASIC stores program text
/// downward from the top while Applesoft stores it upward from the bottom.
/// **HIMEM** at `$4C/$4D` is just past the program's last byte. (LOMEM is
/// `$4A/$4B` and PV, the end of variable storage, `$CC/$CD`; neither is
/// needed here.) So the program is `[PP, HIMEM)`.
bool looksLikeProgram(uint16_t start,
                      const std::function<uint8_t(uint16_t)>& peek,
                      std::size_t bytes);

/// List the program that starts at `start`, reading bytes through `peek`.
/// Stops after `maxBytes`, at a length byte that cannot be a line, or when a
/// line runs past the end. Lines end in '\n'.
std::string list(uint16_t start, const std::function<uint8_t(uint16_t)>& peek,
                 std::size_t maxBytes = 0xC000);

} // namespace pom2::integerbasic

#endif // POM2_INTEGER_BASIC_LISTING_H
