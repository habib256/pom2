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

// ApplesoftListing — a tokenised Applesoft program as text, the way LIST
// would type it, minus the screen: one line per program line, keywords
// spaced so the text can be typed back in (EXEC, paste).
//
// The program is a chain of lines: next-line pointer (2), line number (2),
// tokens and characters, $00. A next pointer of $0000 ends it. Tokens are
// $80-$EA, Applesoft's keyword table (the ROM's at $D0D0, 107 entries). Used
// for a program in RAM (TXTTAB $67/$68, the GUI's "Save BASIC listing") and
// for an A file on a DOS 3.3 disk (pom2_dos33 list).

#ifndef POM2_APPLESOFT_LISTING_H
#define POM2_APPLESOFT_LISTING_H

#include <cstdint>
#include <functional>
#include <string>

namespace pom2::applesoft {

/// Keyword of token `t` ($80-$EA), or nullptr.
const char* keyword(uint8_t t);

/// List the program that starts at `start`, reading bytes through `peek`.
/// Stops at the $0000 link or after `maxBytes` bytes. Lines are walked in
/// order rather than through their links, which in a DOS file point where
/// the program will load. Lines end in '\n'.
std::string list(uint16_t start, const std::function<uint8_t(uint16_t)>& peek,
                 std::size_t maxBytes = 0xC000);

} // namespace pom2::applesoft

#endif // POM2_APPLESOFT_LISTING_H
