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
// spaced so the text can be typed back in (EXEC, paste). Control characters
// in strings and REMs are kept as they are (DOS's Ctrl-D); only CR and LF,
// which would cut the line, become '?'.
//
// "Typed back in" is the contract, and it is NOT what LIST produces. LIST
// puts a space either side of every keyword; the tokeniser keeps every space
// it is given after REM and inside a DATA statement:
// [C] Applesoft ROM, PARSE.INPUT.LINE $D559 (read off roms/apple2e.rom,
//     2026-11-05) — $D570 `BIT DATAFLG / BVS` skips the blank-eating test at
//     $D574 while a DATA statement is open ($D5DC sets the flag on token $83,
//     $D5DA clears it on ':'), and $D5E2-$D5F7 copies the rest of a REM line
//     verbatim.
// So a space written after REM or DATA is STORED by the next tokenise, and a
// program listed and typed back grew by one space per REM and per DATA, every
// trip. The payload of both is therefore written exactly as stored — which
// includes the space the user typed, if they typed one — and a trailing space
// is only removed when this listing put it there.
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

/// What a listing could not carry. Text has no way to hold these bytes, so a
/// program that has them does not come back from its own listing: a byte
/// above the last token ($EB-$FF — real LIST prints pieces of the error
/// messages for those), a token inside a string or a REM (it is written as
/// its keyword, and typed back as letters), a CR or LF. They can only get
/// into a program by POKE or from another tool; the caller should say so
/// rather than hand over a listing that looks complete.
struct ListReport {
    int untypable = 0;      ///< bytes the text does not reproduce
    int firstLine = -1;     ///< line number of the first one, or -1
};

/// List the program that starts at `start`, reading bytes through `peek`.
/// Stops at the $0000 link or after `maxBytes` bytes. Lines are walked in
/// order rather than through their links, which in a DOS file point where
/// the program will load. Lines end in '\n'.
std::string list(uint16_t start, const std::function<uint8_t(uint16_t)>& peek,
                 std::size_t maxBytes = 0xC000, ListReport* report = nullptr);

} // namespace pom2::applesoft

#endif // POM2_APPLESOFT_LISTING_H
