// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// applesoft_tokenizer — text listing → tokenized program (ApplesoftTokenizer.h).
//
// Three kinds of oracle, kept apart because they are not worth the same:
//  1. Byte vectors Casso reports as typed into a real Applesoft and read back
//     from $0801 — [D] Casso UnitTest/ApplesoftTokenizerTests.cpp @ 2d899adf
//     (MIT): SpacingBytes, BoundaryBytes, CollisionBytes. Ported verbatim.
//  2. Lines worked out BY HAND from the ROM's PARSE ($D559, see the header),
//     including the cases where Casso's own tokenizer disagrees with the ROM
//     (`AT N`, `IF A THEN`, bare numbers, duplicates, control characters,
//     `~`, line length).
//  3. ApplesoftListing — tokens → text → tokens must give back the bytes, and
//     its keyword table is the one the tokenizer uses, so every keyword is
//     tokenized and listed back to catch a drift between the two.

#include "ApplesoftListing.h"
#include "ApplesoftTokenizer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

using Bytes = std::vector<uint8_t>;
namespace as = pom2::applesoft;

std::string hex(const Bytes& b)
{
    std::string s;
    char buf[4];
    for (uint8_t x : b) { std::snprintf(buf, sizeof buf, "%02X ", x); s += buf; }
    return s;
}

#define CHECK_BYTES(actual, expected) do { \
    const Bytes a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        std::fprintf(stderr, "FAIL %s:%d:\n  expected %s\n  actual   %s\n", \
                     __FILE__, __LINE__, hex(e_).c_str(), hex(a_).c_str()); \
        ++g_fail; } } while (0)

Bytes tok(const std::string& text, uint16_t load = 0x0801)
{
    Bytes out;
    std::vector<as::TokenizeError> errors;
    if (!as::tokenize(text, out, errors, load)) {
        std::fprintf(stderr, "  unexpected refusal of [%s]:", text.c_str());
        for (const auto& e : errors) std::fprintf(stderr, " (%d) %s;", e.sourceLine, e.message.c_str());
        std::fprintf(stderr, "\n");
        ++g_fail;
    }
    return out;
}

/// The body of a one-line program: link, number and both terminators off.
Bytes body(const std::string& line)
{
    const Bytes p = tok(line);
    if (p.size() < 7) return {};
    return Bytes(p.begin() + 4, p.end() - 3);
}

std::vector<as::TokenizeError> refusal(const std::string& text)
{
    Bytes out = { 0xAA };
    std::vector<as::TokenizeError> errors;
    const bool ok = as::tokenize(text, out, errors);
    CHECK(!ok);
    CHECK(out.empty());          // a refusal hands back nothing
    CHECK(!errors.empty());
    for (const auto& e : errors) CHECK(!e.message.empty());
    return errors;
}

std::string listOf(const Bytes& p, uint16_t load = 0x0801)
{
    return as::list(load, [&](uint16_t a) -> uint8_t {
        const std::size_t i = static_cast<uint16_t>(a - load);
        return i < p.size() ? p[i] : 0;
    }, p.size());
}

void checkRoundTrip(const Bytes& program, uint16_t load = 0x0801)
{
    as::ListReport report;
    const std::string text = as::list(load, [&](uint16_t a) -> uint8_t {
        const std::size_t i = static_cast<uint16_t>(a - load);
        return i < program.size() ? program[i] : 0;
    }, program.size(), &report);
    CHECK(report.untypable == 0);
    CHECK_BYTES(tok(text, load), program);
}

/// Every link points at the next line and the last one at the closing $0000.
void checkChain(const Bytes& p, uint16_t load)
{
    std::size_t at = 0;
    int lines = 0;
    while (at + 1 < p.size()) {
        const unsigned link = p[at] | p[at + 1] << 8;
        if (link == 0) break;
        std::size_t end = at + 4;
        while (end < p.size() && p[end] != 0) ++end;
        CHECK(end < p.size());
        CHECK(link == load + end + 1);
        at = end + 1;
        ++lines;
    }
    CHECK(at + 2 == p.size());
    CHECK(lines > 0);
}

// --- 1. Casso's measured vectors --------------------------------------------

const char* kSpacingSource =
    "10 PRINT \"HI\"\n"
    "20 A = 1\n"
    "30 REM  X Y\n"
    "40 DATA 1, 2 ,3\n"
    "50 ?\"Z\"\n"
    "60 FOR I = 1 TO 9: NEXT\n";

const Bytes kSpacingBytes = {
    0x0B, 0x08, 0x0A, 0x00, 0xBA, 0x22, 0x48, 0x49, 0x22, 0x00,
    0x13, 0x08, 0x14, 0x00, 0x41, 0xD0, 0x31, 0x00,
    0x1E, 0x08, 0x1E, 0x00, 0xB2, 0x20, 0x20, 0x58, 0x20, 0x59, 0x00,
    0x2C, 0x08, 0x28, 0x00, 0x83, 0x20, 0x31, 0x2C, 0x20, 0x32, 0x20, 0x2C, 0x33, 0x00,
    0x35, 0x08, 0x32, 0x00, 0xBA, 0x22, 0x5A, 0x22, 0x00,
    0x42, 0x08, 0x3C, 0x00, 0x81, 0x49, 0xD0, 0x31, 0xC1, 0x39, 0x3A, 0x82, 0x00,
    0x00, 0x00,
};

const char* kBoundarySource =
    "10 X = ATN(1)\n"
    "20 TOTAL = 5\n"
    "30 HLIN 0,39 AT 10\n"
    "40 PRINT \"PRINT AT TO\"\n"
    "50 print 1\n"
    "60 DATA \"A:B\",C: PRINT 1\n"
    "70 REM X: PRINT\n"
    "80 PR INT\n";

const Bytes kBoundaryBytes = {
    0x0C, 0x08, 0x0A, 0x00, 0x58, 0xD0, 0xE1, 0x28, 0x31, 0x29, 0x00,
    0x17, 0x08, 0x14, 0x00, 0xC1, 0x54, 0x41, 0x4C, 0xD0, 0x35, 0x00,
    0x24, 0x08, 0x1E, 0x00, 0x8E, 0x30, 0x2C, 0x33, 0x39, 0xC5, 0x31, 0x30, 0x00,
    0x37, 0x08, 0x28, 0x00, 0xBA, 0x22, 0x50, 0x52, 0x49, 0x4E, 0x54, 0x20,
                            0x41, 0x54, 0x20, 0x54, 0x4F, 0x22, 0x00,
    0x3E, 0x08, 0x32, 0x00, 0xBA, 0x31, 0x00,
    0x4F, 0x08, 0x3C, 0x00, 0x83, 0x20, 0x22, 0x41, 0x3A, 0x42, 0x22, 0x2C,
                            0x43, 0x3A, 0xBA, 0x31, 0x00,
    0x5E, 0x08, 0x46, 0x00, 0xB2, 0x20, 0x58, 0x3A, 0x20, 0x50, 0x52, 0x49,
                            0x4E, 0x54, 0x00,
    0x64, 0x08, 0x50, 0x00, 0xBA, 0x00,
    0x00, 0x00,
};

const char* kCollisionSource =
    "10 FOR I = A TO B: NEXT\n"
    "20 A$ = \"HI THERE\"\n"
    "30 IF A<=B THEN 10\n"
    "40 DATA A, B\n"
    "50 REM LOWER\n"
    "60 PRINT A$ ; CHR$ (7)\n"
    "70 ATN = 1\n"
    "80 X = A TOTAL\n";

const Bytes kCollisionBytes = {
    0x0E, 0x08, 0x0A, 0x00, 0x81, 0x49, 0xD0, 0x41, 0xC1, 0x42, 0x3A, 0x82, 0x00,
    0x20, 0x08, 0x14, 0x00, 0x41, 0x24, 0xD0, 0x22, 0x48, 0x49, 0x20, 0x54,
                            0x48, 0x45, 0x52, 0x45, 0x22, 0x00,
    0x2D, 0x08, 0x1E, 0x00, 0xAD, 0x41, 0xD1, 0xD0, 0x42, 0xC4, 0x31, 0x30, 0x00,
    0x38, 0x08, 0x28, 0x00, 0x83, 0x20, 0x41, 0x2C, 0x20, 0x42, 0x00,
    0x44, 0x08, 0x32, 0x00, 0xB2, 0x20, 0x4C, 0x4F, 0x57, 0x45, 0x52, 0x00,
    0x51, 0x08, 0x3C, 0x00, 0xBA, 0x41, 0x24, 0x3B, 0xE7, 0x28, 0x37, 0x29, 0x00,
    0x59, 0x08, 0x46, 0x00, 0xE1, 0xD0, 0x31, 0x00,
    0x65, 0x08, 0x50, 0x00, 0x58, 0xD0, 0x41, 0xC1, 0x54, 0x41, 0x4C, 0x00,
    0x00, 0x00,
};

void testCassoMeasured()
{
    CHECK_BYTES(tok(kSpacingSource), kSpacingBytes);
    CHECK_BYTES(tok(kBoundarySource), kBoundaryBytes);
    CHECK_BYTES(tok(kCollisionSource), kCollisionBytes);
    checkRoundTrip(kSpacingBytes);
    checkRoundTrip(kBoundaryBytes);
    checkRoundTrip(kCollisionBytes);
}

void testCassoSmallCases()
{
    CHECK_BYTES(tok("10 PR INT\n"), tok("10 PRINT\n"));
    CHECK_BYTES(tok("10 END\n20 END"), tok("10 END\n20 END\n"));   // last line, no newline
    CHECK_BYTES(tok("10 ?1\n"), tok("10 PRINT 1\n"));
    // DATA's trailing space is part of the program.
    const Bytes data = tok("10 DATA A \n");
    CHECK(data.size() == 11);
    checkRoundTrip(data);
    // Links: three six-byte lines at $0801.
    const Bytes three = tok("10 END\n20 END\n30 END\n");
    CHECK(three.size() == 20);
    CHECK((three[0] | three[1] << 8) == 0x0807);
    CHECK((three[6] | three[7] << 8) == 0x080D);
    CHECK((three[12] | three[13] << 8) == 0x0813);
    // A control character inside a string, a REM and a DATA is stored and
    // listed back as the byte it is.
    const std::string ctl = "10 PRINT \"\x04" "CATALOG\"\n"
                            "20 REM \x07" "BELL\n"
                            "30 DATA \x0B" "X,Y\n";
    const Bytes ctlBytes = tok(ctl);
    CHECK(listOf(ctlBytes) == ctl);
    checkRoundTrip(ctlBytes);
}

// --- 2. Worked out from the ROM ---------------------------------------------

void testRomRules()
{
    // [C] $D5BE: only the character RIGHT after the T counts. Casso skips
    // blanks there and makes ATN of `AT N` — the ROM stores AT, N.
    CHECK_BYTES(body("10 HLIN 1,2 AT N"), (Bytes{ 0x8E, 0x31, 0x2C, 0x32, 0xC5, 0x4E }));
    CHECK_BYTES(body("10 HLIN 1,2 ATN"),  (Bytes{ 0x8E, 0x31, 0x2C, 0x32, 0xE1 }));
    CHECK_BYTES(body("10 X=A TN(1)"),     (Bytes{ 0x58, 0xD0, 0xE1, 0x28, 0x31, 0x29 }));
    // `AT O` is AT then the letter O; Casso emits A and TO.
    CHECK_BYTES(body("10 X=AT O"),        (Bytes{ 0x58, 0xD0, 0xC5, 0x4F }));
    // The famous one: no special case for H, so A THEN is AT HEN.
    CHECK_BYTES(body("10 IF A THEN 5"),   (Bytes{ 0xAD, 0xC5, 0x48, 0x45, 0x4E, 0x35 }));
    // Table order, first match: SCORE is S C OR E; FOR I=ATOB is A then TO.
    CHECK_BYTES(body("10 SCORE"),         (Bytes{ 0x53, 0x43, 0xCE, 0x45 }));
    CHECK_BYTES(body("10 FOR I=ATOB"),    (Bytes{ 0x81, 0x49, 0xD0, 0x41, 0xC1, 0x42 }));
    // [C] $D588: digits, ':' and ';' go in as they are; PRINT"X";:GOTO10.
    CHECK_BYTES(body("10 PRINT\"X\";:GOTO10"),
                (Bytes{ 0xBA, 0x22, 0x58, 0x22, 0x3B, 0x3A, 0xAB, 0x31, 0x30 }));
    // [C] //e $F78C: lower case folded outside quotes/REM/DATA, kept inside;
    // the AT lookahead is folded too ($F787).
    CHECK_BYTES(body("10 x=a to b"),      (Bytes{ 0x58, 0xD0, 0x41, 0xC1, 0x42 }));
    CHECK_BYTES(body("10 ?\"abc\";a"),    (Bytes{ 0xBA, 0x22, 0x61, 0x62, 0x63, 0x22, 0x3B, 0x41 }));
    CHECK_BYTES(body("10 data abc:print"), (Bytes{ 0x83, 0x20, 0x61, 0x62, 0x63, 0x3A, 0xBA }));
    CHECK_BYTES(body("10 rem  Hi  there"), (Bytes{ 0xB2, 0x20, 0x20, 0x48, 0x69, 0x20, 0x20,
                                                   0x74, 0x68, 0x65, 0x72, 0x65 }));
    // ...and $7B-$7E fold as well: `~` becomes `^`, the power token $CC.
    CHECK_BYTES(body("10 X=2~3"),         (Bytes{ 0x58, 0xD0, 0x32, 0xCC, 0x33 }));
    // [C] $D57E: '?' inside DATA is a character, a quoted ':' does not end it.
    CHECK_BYTES(body("10 DATA ?,\"A:B\":?"),
                (Bytes{ 0x83, 0x20, 0x3F, 0x2C, 0x22, 0x41, 0x3A, 0x42, 0x22, 0x3A, 0xBA }));
    // An unclosed quote runs to the end of the line, REM included.
    CHECK_BYTES(body("10 PRINT \"A:REM"), (Bytes{ 0xBA, 0x22, 0x41, 0x3A, 0x52, 0x45, 0x4D }));
    // [C] $D60B: a control character outside quotes is stored, not refused
    // (Casso refuses it).
    CHECK_BYTES(body("10 PRINT \x04" "A"), (Bytes{ 0xBA, 0x04, 0x41 }));
    // [C] LINGET via CHRGET: blanks between the digits are skipped.
    CHECK_BYTES(tok("1 0 PRINT\n"), tok("10 PRINT\n"));
    CHECK_BYTES(tok("   00010PRINT\n"), tok("10 PRINT\n"));
    // 63999 is the last line number.
    const Bytes top = tok("63999 END\n");
    CHECK(top.size() == 8 && top[2] == 0xFF && top[3] == 0xF9);
}

void testEveryKeyword()
{
    int checked = 0;
    for (unsigned t = 0x80; t <= 0xEA; ++t) {
        const char* kw = as::keyword(static_cast<uint8_t>(t));
        CHECK(kw != nullptr);
        if (!kw) continue;
        const Bytes p = tok(std::string("10 ") + kw + "\n");
        CHECK(p.size() == 8);
        if (p.size() != 8) continue;
        CHECK(p[4] == t);
        // ...and the listing writes the same keyword back.
        const std::string listed = listOf(p);
        CHECK(listed == std::string("10 ") + kw + "\n");
        CHECK_BYTES(tok(listed), p);
        ++checked;
    }
    CHECK(checked == 107);
    CHECK(as::keyword(0x7F) == nullptr && as::keyword(0xEB) == nullptr);
}

void testRoundTrip()
{
    std::string text =
        "5 HOME : TEXT : PRINT CHR$(4);\"PR#3\"\n"
        "10 REM  lower case and  double  spaces: PRINT\n"
        "20 DATA \"A:B\", c d ,  \"E\" :?\"after\"\n"
        "30 FOR I=ATOB STEP 2: PRINT\"X\";:GOTO10\n"
        "40 ?AT 5; HLIN 1,2 AT N : X = ATN(Y) + A TO\n"
        "50 IF A$ <> \"Q\" THEN GOSUB 100: ON X GOTO 10,20\n"
        "60 ONERR GOTO 900: HGR2 : HGR : HCOLOR= 3: HPLOT 0,0 TO 10,10\n"
        "70 DEF FN F(X) = X ^ 2: PRINT FN F(3); SPC(2); TAB( 5)\n"
        "80 print \"unclosed\n"
        "90 DATA\n"
        "100 RETURN\n";
    // Every keyword, two per line so no line nears the limit.
    int n = 1000;
    for (unsigned t = 0x80; t <= 0xEA; t += 2) {
        text += std::to_string(n) + " " + as::keyword(static_cast<uint8_t>(t));
        if (t + 1 <= 0xEA) text += std::string(" : ") + as::keyword(static_cast<uint8_t>(t + 1));
        text += "\n";
        n += 10;
    }
    for (uint16_t load : { uint16_t(0x0801), uint16_t(0x4001) }) {
        const Bytes p = tok(text, load);
        checkChain(p, load);
        checkRoundTrip(p, load);
        // A second trip changes nothing either.
        CHECK(listOf(tok(listOf(p, load), load), load) == listOf(p, load));
    }
    // Same text at two addresses: same bodies, links shifted by the difference.
    const Bytes a = tok(text, 0x0801), b = tok(text, 0x4001);
    CHECK(a.size() == b.size());
    bool shifted = a.size() == b.size();
    for (std::size_t at = 0; shifted && at + 1 < a.size();) {
        const unsigned la = a[at] | a[at + 1] << 8, lb = b[at] | b[at + 1] << 8;
        if (la == 0) { shifted = lb == 0; break; }
        shifted = lb == la + (0x4001 - 0x0801);
        std::size_t end = at + 4;
        while (end < a.size() && a[end] != 0) ++end;
        for (std::size_t k = at + 2; k <= end && shifted; ++k) shifted = a[k] == b[k];
        at = end + 1;
    }
    CHECK(shifted);
}

void testOrderingAndReplacement()
{
    // Any order in, sorted out.
    CHECK_BYTES(tok("30 END\n10 GR\n20 TEXT\n"), tok("10 GR\n20 TEXT\n30 END\n"));
    // [C] $D46C: a repeated number replaces (Casso refuses it).
    CHECK_BYTES(tok("10 PRINT 1\n10 PRINT 2\n"), tok("10 PRINT 2\n"));
    // [C] $D4B5: a bare number deletes (Casso refuses it); a bare number for a
    // line that does not exist does nothing.
    CHECK_BYTES(tok("10 PRINT 1\n20 END\n10\n"), tok("20 END\n"));
    CHECK_BYTES(tok("10 PRINT 1\n55   \n"), tok("10 PRINT 1\n"));
    // ...and a line can come back after it was deleted.
    CHECK_BYTES(tok("10 GR\n10\n10 HGR\n"), tok("10 HGR\n"));
    // Blank lines, CR, CRLF and a byte-order mark.
    CHECK_BYTES(tok("\xEF\xBB\xBF" "10 GR\r\n\r\n  \t \r20 END\r"), tok("10 GR\n20 END\n"));
    // An empty listing is the empty program.
    CHECK_BYTES(tok(""), (Bytes{ 0x00, 0x00 }));
    CHECK_BYTES(tok("\n   \n\n"), (Bytes{ 0x00, 0x00 }));
}

void testErrors()
{
    auto e = refusal("10 PRINT 1\nPRINT 2\n");
    CHECK(e.size() == 1 && e[0].sourceLine == 2);
    e = refusal("64000 END\n");
    CHECK(e.size() == 1 && e[0].sourceLine == 1);
    e = refusal("99999999999 END\n");
    CHECK(e.size() == 1);
    e = refusal("10 PRINT \"caf\xC3\xA9\"\n");                 // UTF-8 é
    CHECK(e.size() == 1 && e[0].sourceLine == 1);
    e = refusal(std::string("10 PRINT \"A") + '\0' + "\"\n");
    CHECK(e.size() == 1);
    // Every bad line is reported, with its line in the TEXT (blank lines count).
    e = refusal("10 END\n\nX\n70000 END\n20 END\n");
    CHECK(e.size() == 2 && e[0].sourceLine == 3 && e[1].sourceLine == 4);

    // Line length: 238 bytes of body is the most a typed line can give.
    const std::string fits = "10 REM" + std::string(as::kMaxLineBody - 1, 'X');
    CHECK(tok(fits).size() == 4 + as::kMaxLineBody + 1 + 2);
    e = refusal("10 REM" + std::string(as::kMaxLineBody, 'X') + "\n");
    CHECK(e.size() == 1 && e[0].sourceLine == 1);
    // ...measured on the stored form: 300 characters that crunch to less fit.
    CHECK(!tok("10 PRINT" + std::string(292, ' ') + "1\n").empty());

    // A program that runs past $FFFF from where it is loaded.
    Bytes out;
    std::vector<as::TokenizeError> errors;
    std::string big;
    for (int line = 1; line <= 20; ++line)
        big += std::to_string(line) + " REM " + std::string(200, 'X') + "\n";
    CHECK(as::tokenize(big, out, errors, 0x0801));
    CHECK(!as::tokenize(big, out, errors, 0xF000));
    CHECK(out.empty() && errors.size() == 1);
}

} // namespace

int main()
{
    testCassoMeasured();
    testCassoSmallCases();
    testRomRules();
    testEveryKeyword();
    testRoundTrip();
    testOrderingAndReplacement();
    testErrors();
    if (g_fail) {
        std::fprintf(stderr, "applesoft_tokenizer: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("applesoft_tokenizer: OK\n");
    return 0;
}
