// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// ApplesoftTokenizer — a text listing turned into the program Applesoft would
// hold in memory had the lines been typed at the ] prompt. The inverse of
// ApplesoftListing (which writes the text) and it shares that file's keyword
// table, so the two cannot disagree on a token.
//
// WHY THE RULES ARE THE ROM'S AND NOT A TIDIER SET: the bytes this produces
// are run by the real interpreter, and a program the real tokeniser could not
// have stored is a program LIST, the editor and RUN were never tested against.
// So every rule below is the ROM's own, read off its PARSE routine, warts
// included (`SCORE` is S C OR E; `IF A THEN` is IF AT HEN):
//
// [C] Applesoft PARSE.INPUT.LINE $D559-$D619, read off roms/apple2e.rom
//     (enhanced //e, sha1 d756eba5) and roms/apple2p.rom (][+, sha1
//     29a53f3b), 2026-11-05. The two are the same routine except for the
//     //e's case folding (below).
//   - A blank outside a string, REM or DATA is dropped ($D574), and also
//     BETWEEN the letters of a keyword ($D5AB): `PR INT` is PRINT.
//   - Keywords are tried in table order ($D0D0, tokens $80-$EA), first match
//     wins; a character that starts none is stored as itself ($D60B) and the
//     search restarts at the next one.
//   - Digits, ':' and ';' ($30-$3B) are stored without a search ($D588).
//   - '?' is the PRINT token $BA ($D580).
//   - A quote copies everything to the closing quote or the end of the line
//     ($D5F2), in a DATA statement too — so a ':' inside quotes does not end
//     the DATA.
//   - After DATA ($83) characters are stored as typed, blanks included, up to
//     a ':' ($D570 BVS, $D57E BVS, flag set $D5DC, cleared $D5DA).
//   - After REM ($B2) the rest of the line is stored as typed ($D5E2-$D5F7).
//   - AT ($C5) is refused when the character RIGHT AFTER its T — not the next
//     non-blank one — is 'N' or 'O' ($D5BA-$D5C7), so ATN and `A TO` come out
//     right; `AT N` (blank between) stays AT then N.
//   - [C] //e only, $F78C/$F79A: outside quotes, REM and DATA a fetched
//     character from $61 up is ANDed with $5F — lower case is folded, and so
//     is `{|}~` (`~` becomes `^`, the power token). The ][+ PARSE ($D56D
//     `LDA $0200,X`) has no fold: it stores lower case as typed and matches
//     no keyword with it. POM2 follows the //e: a listing typed in lower case
//     is meant as keywords.
//   - [C] The line number is read by LINGET $DA0C through CHRGET, which skips
//     blanks — `1 0 PRINT` is line 10 — and a digit arriving when the number
//     is already 6400 or more is ?SYNTAX ERROR ($DA1C `CMP #$19`), so the
//     ceiling is 63999.
//   - [C] A line with a number and nothing else deletes that line, and a
//     number already present is replaced ($D46C FNDLIN, $D471-$D4B3 delete,
//     $D4B5 `LDA $0200 / BEQ` — insert only a non-empty line).
//
// Control characters are stored as they come, in every context: PARSE has no
// case for them and stores them like any other character that starts no
// keyword. The text is the program, not keystrokes — a Ctrl-H is a byte here,
// not GETLN's backspace.

#ifndef POM2_APPLESOFT_TOKENIZER_H
#define POM2_APPLESOFT_TOKENIZER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2::applesoft {

/// The longest body (tokens and characters, without link, number or the
/// closing $00) Applesoft itself can store for one line.
/// [C] INLIN $D533-$D537 (`CPX #$EF / LDX #$EF`): a typed line is cut at 239
/// characters, its number included, and PARSE writes at most one byte per
/// character it reads (`$D5CD INX / INY` in step) — so a body never exceeds
/// 239 minus at least one digit.
constexpr std::size_t kMaxLineBody = 238;

/// The highest line number. [C] LINGET $DA1C, see the header.
constexpr unsigned kMaxLineNumber = 63999;

struct TokenizeError {
    int         sourceLine = 0;   ///< 1-based line of the TEXT
    std::string message;
};

/// `text`: lines separated by LF, CR or CRLF; blank lines (spaces and tabs
/// only) are ignored. A line must start with a line number 0..63999.
/// Lines may come in any order and a repeated number replaces the earlier
/// line, a bare number deletes it (as typing it would); the output is sorted.
/// `program`: the bytes as they sit in memory starting at `loadAddress`
/// (pointers are absolute), ending with the $00 $00 link. An empty listing is
/// the empty program, $00 $00.
/// Returns false and fills `errors` (program left empty) if any line is
/// unusable: no line number, number out of range, a byte with no Apple II
/// meaning ($00 or $80-$FF), a body longer than kMaxLineBody, or a program
/// that runs past $FFFF from `loadAddress`.
bool tokenize(const std::string& text, std::vector<uint8_t>& program,
              std::vector<TokenizeError>& errors, uint16_t loadAddress = 0x0801);

} // namespace pom2::applesoft

#endif // POM2_APPLESOFT_TOKENIZER_H
