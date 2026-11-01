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

// integer_basic_listing — the Integer BASIC lister (IntegerBasicListing.h),
// the ][ Original's BASIC, which POM2 could not read at all until
// 2026-11-01.
//
// Every token value used to build the synthetic programs below was read off
// REAL Integer BASIC files — the two on
// `disks_5.4/dsk/saturn128k_software.dsk` — not off a table alone:
//   $4D CALL  $36 -  $61 PRINT  $2E PEEK  $3F (  $24 THEN
//   $16 =  $72 )  $5D REM  $28/$29 "  $B0-$B9 constant  $C1-$DA name
//
// What this pins, beyond "it decodes": the three byte ranges OVERLAP, and
// each overlap is a way to produce garbage —
//   * `$B1` is the digit 1 inside a variable name (`A1`) and the marker of
//     an integer constant. A name runs on while the bytes stay >= $80.
//   * `$B1` inside a string constant is the character '1'.
//   * `$B1` after REM is the character '1'.
// Read the wrong way, each one eats the two bytes that follow as a value and
// the rest of the line decodes as noise.

#include "Dos33DiskFs.h"
#include "IntegerBasicListing.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

/// Like CHECK, but says what came out — a listing mismatch is unreadable
/// without both sides.
#define EXPECT(actual, expected) do { \
    const std::string a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        std::fprintf(stderr, "FAIL %s:%d:\n  expected [%s]\n  actual   [%s]\n", \
                     __FILE__, __LINE__, e_.c_str(), a_.c_str()); \
        ++g_fail; } } while (0)

std::string listOf(const std::vector<uint8_t>& prog)
{
    return pom2::integerbasic::list(0, [&](uint16_t a) {
        return a < prog.size() ? prog[a] : uint8_t(0);
    }, prog.size());
}

/// One line: the length byte is the WHOLE line, its own byte and the $01
/// included — so the builder computes it rather than the test spelling it.
std::vector<uint8_t> line(uint16_t number, std::vector<uint8_t> body)
{
    std::vector<uint8_t> out;
    out.push_back(uint8_t(body.size() + 4));          // len + 2 number + $01
    out.push_back(uint8_t(number & 0xFF));
    out.push_back(uint8_t(number >> 8));
    out.insert(out.end(), body.begin(), body.end());
    out.push_back(0x01);
    return out;
}

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

/// "AB" as high-bit characters, the way a name or a string holds them.
std::vector<uint8_t> hi(const std::string& s)
{
    std::vector<uint8_t> out;
    for (char c : s) out.push_back(uint8_t(c) | 0x80);
    return out;
}

/// An integer constant: the marker is the first digit the user typed and
/// carries no value; the two bytes after it are the number, LE.
std::vector<uint8_t> konst(uint16_t v, char firstDigit)
{
    return { uint8_t(0xB0 + (firstDigit - '0')),
             uint8_t(v & 0xFF), uint8_t(v >> 8) };
}

void testLineStructure()
{
    // `09 05 00 | 4D 36 B9 A8 03 | 01` — line 5 of SET APPLESOFT SLOT, nine
    // bytes, exactly as it sits on the disk. `B9 A8 03` is ONE item: the
    // constant marker and 936 as $03A8. Reading the `03` as the `:` token
    // instead is the obvious first mistake, so it is pinned here.
    const std::vector<uint8_t> prog = {
        0x09, 0x05, 0x00, 0x4D, 0x36, 0xB9, 0xA8, 0x03, 0x01 };
    EXPECT(listOf(prog), "5 CALL -936\n");

    // Two lines run back to back, found by the length byte.
    std::vector<uint8_t> two;
    append(two, line(10, { 0x61 }));                       // PRINT
    append(two, line(20, { 0x4D, 0x36, 0xB9, 0xA8, 0x03 }));
    EXPECT(listOf(two), "10 PRINT\n20 CALL -936\n");

    // Trailing zeros (a file padded to its sector) end the walk, they do not
    // list as a line.
    std::vector<uint8_t> padded = two;
    padded.resize(padded.size() + 64, 0x00);
    CHECK(listOf(padded) == listOf(two));

    // A length byte that cannot be a line, and one that runs past the end:
    // both stop, neither loops or reads out of bounds.
    CHECK(listOf({ 0x03, 0x0A, 0x00, 0x01 }).empty());
    CHECK(listOf({ 0x40, 0x0A, 0x00, 0x61, 0x01 }).empty());
    CHECK(listOf({}).empty());
}

// The overlap: $B0-$B9 is a digit in a name, a digit in a string, a digit
// after REM, and the marker of a constant everywhere else.
void testDigitsInNamesStringsAndRems()
{
    // A1=141 — the name `A1` is `C1 B1`. Read as a constant, `$B1` would
    // swallow `16 B1` as its value and the line would decode as noise.
    std::vector<uint8_t> assign;
    append(assign, hi("A1"));
    assign.push_back(0x16);                      // =
    append(assign, konst(141, '1'));
    EXPECT(listOf(line(20, assign)), "20 A1=141\n");

    // A longer name, and one whose digits are at the end.
    std::vector<uint8_t> names;
    append(names, hi("SLOT1"));
    names.push_back(0x16);
    append(names, konst(7, '7'));
    EXPECT(listOf(line(30, names)), "30 SLOT1=7\n");

    // PRINT "A1" — inside a string the same byte is the character '1'.
    std::vector<uint8_t> str = { 0x61, 0x28 };
    append(str, hi("A1"));
    str.push_back(0x29);
    EXPECT(listOf(line(10, str)), "10 PRINT \"A1\"\n");

    // A string holding what looks like a token, and one holding a digit run.
    std::vector<uint8_t> str2 = { 0x61, 0x28 };
    append(str2, hi("1234567890"));
    str2.push_back(0x29);
    EXPECT(listOf(line(11, str2)), "11 PRINT \"1234567890\"\n");

    // REM 12 — after REM the rest of the line is text, digits included.
    std::vector<uint8_t> rem = { 0x5D };
    append(rem, hi("12 IS NOT A TOKEN"));
    EXPECT(listOf(line(40, rem)), "40 REM 12 IS NOT A TOKEN\n");

    // A REM that holds a byte which WOULD be a token outside one.
    std::vector<uint8_t> rem2 = { 0x5D };
    append(rem2, hi("A"));
    rem2.push_back(0xB0 | 0x00);                 // '0'
    EXPECT(listOf(line(41, rem2)), "41 REM A0\n");
}

void testKeywordSpacing()
{
    // IF PEEK (-16640)=141 THEN 200 — line 100 of SET APPLESOFT SLOT. The
    // table gives "THEN " a trailing space and no leading one, so without
    // the spacing rule this listed as `=141THEN 200`.
    std::vector<uint8_t> body = { 0x60, 0x2E, 0x3F, 0x36 };
    append(body, konst(16640, '1'));
    body.push_back(0x72);                        // )
    body.push_back(0x16);                        // =
    append(body, konst(141, '1'));
    body.push_back(0x24);                        // THEN
    append(body, konst(200, '2'));
    EXPECT(listOf(line(100, body)), "100 IF PEEK (-16640)=141 THEN 200\n");

    // An operator stays tight against what it joins.
    std::vector<uint8_t> tight;
    append(tight, konst(16, '1'));
    tight.push_back(0x14);                       // *
    append(tight, hi("SLOT"));
    EXPECT(listOf(line(210, tight)), "210 16*SLOT\n");
}

void testControlCharactersSurvive()
{
    // PRINT "<Ctrl-D>INT" — line 100 of APPLESOFT on the same disk. The
    // Ctrl-D IS the program (it is how a DOS command is issued), so it goes
    // out as it is, the rule ApplesoftListing already follows.
    std::vector<uint8_t> body = { 0x61, 0x28, 0x84 };   // $84 = Ctrl-D | $80
    append(body, hi("INT"));
    body.push_back(0x29);
    const std::string text = listOf(line(100, body));
    EXPECT(text, std::string("100 PRINT \"\x04") + "INT\"\n");

    // CR and LF would cut the line, so they keep the '?'.
    std::vector<uint8_t> cut = { 0x61, 0x28, 0x8D, 0x8A, 0x29 };
    EXPECT(listOf(line(101, cut)), "101 PRINT \"??\"\n");
}

void testKeywordTable()
{
    CHECK(std::string(pom2::integerbasic::keyword(0x4D)) == "CALL ");
    CHECK(std::string(pom2::integerbasic::keyword(0x5D)) == "REM ");
    CHECK(std::string(pom2::integerbasic::keyword(0x5F)) == "GOTO ");
    CHECK(std::string(pom2::integerbasic::keyword(0x60)) == "IF ");
    CHECK(std::string(pom2::integerbasic::keyword(0x03)) == ":");
    CHECK(std::string(pom2::integerbasic::keyword(0x28)) == "\"");
    CHECK(pom2::integerbasic::keyword(0x80) == nullptr);
    CHECK(pom2::integerbasic::keyword(0xFF) == nullptr);
}

// The real thing, when the tracked disk is there: the two Integer BASIC
// files POM2's format notes were read from. Skipped without it.
void testTrackedDisk()
{
    const char* path = "disks_5.4/dsk/saturn128k_software.dsk";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("integer_basic_listing: %s absent, skipped\n", path);
        return;
    }
    std::vector<uint8_t> image(std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>{});
    pom2::dos33fs::Dos33Disk disk;
    std::string err;
    if (!disk.open(std::move(image), pom2::dos33fs::orderForPath(path), err)) {
        std::fprintf(stderr, "FAIL %s: %s\n", path, err.c_str());
        ++g_fail;
        return;
    }
    std::vector<uint8_t> raw;
    CHECK(disk.readFile("SET APPLESOFT SLOT", raw, err));
    const auto prog = pom2::dos33fs::Dos33Disk::payload(0x01, raw);
    // The gate the GUI uses, against a real program: its line lengths must
    // walk from the start and land exactly on the end.
    CHECK(pom2::integerbasic::looksLikeProgram(0, [&](uint16_t a) {
        return a < prog.size() ? prog[a] : uint8_t(0);
    }, prog.size()));
    const std::string text = listOf(prog);

    // The whole program lists: first line, last line, and the one that
    // needed the spacing rule.
    CHECK(text.rfind("5 CALL -936\n", 0) == 0);
    CHECK(text.find("100 IF PEEK (-16640)=141 THEN 200\n") != std::string::npos);
    CHECK(text.find("200 INPUT \"ENTER SLOT# OF APPLESOFT CARD \",SLOT\n")
          != std::string::npos);
    CHECK(text.find("210 POKE -16636,16*SLOT+128+1\n") != std::string::npos);
    CHECK(text.size() > 800 && "the whole program, not the first few lines");
    // Every line starts with its number and ends in a newline, and nothing
    // in the middle decoded into a stray NUL.
    CHECK(text.back() == '\n');
    CHECK(text.find('\0') == std::string::npos);
    std::printf("integer_basic_listing: tracked disk OK (%zu bytes listed)\n",
                text.size());
}

// `looksLikeProgram` — the gate the GUI needs, because PP ($CA/$CB) and
// HIMEM ($4C/$4D) are plain zero-page bytes that hold something else on a
// machine running Applesoft. Landing EXACTLY on the end is the check.
void testLooksLikeProgram()
{
    auto looks = [](const std::vector<uint8_t>& p) {
        return pom2::integerbasic::looksLikeProgram(0, [&](uint16_t a) {
            return a < p.size() ? p[a] : uint8_t(0);
        }, p.size());
    };

    std::vector<uint8_t> good;
    append(good, line(10, { 0x61 }));
    append(good, line(20, { 0x4D, 0x36, 0xB9, 0xA8, 0x03 }));
    CHECK(looks(good));
    CHECK(looks(line(10, { 0x61 })));            // one line is a program

    // One byte short, one byte long: the walk must not land on the end.
    std::vector<uint8_t> shortened = good; shortened.pop_back();
    CHECK(!looks(shortened));
    std::vector<uint8_t> extended = good; extended.push_back(0x00);
    CHECK(!looks(extended));

    // A line that does not end in $01.
    std::vector<uint8_t> noEol = { 0x04, 0x0A, 0x00, 0x61 };
    CHECK(!looks(noEol));

    // A length byte too small to be a line, and one that overruns.
    CHECK(!looks({ 0x03, 0x0A, 0x00, 0x01 }));
    CHECK(!looks({ 0x08, 0x0A, 0x00, 0x61, 0x01 }));
    // Empty, and too short to hold a line at all.
    CHECK(!looks({}));
    CHECK(!looks({ 0x04, 0x0A, 0x00 }));

    // What the gate is really for: bytes that are NOT a program. A machine
    // running Applesoft has whatever BASIC.SYSTEM left in $CA/$CB.
    std::vector<uint8_t> noise(256);
    uint32_t x = 12345;
    for (auto& b : noise) { x = x * 1103515245u + 12345u; b = uint8_t(x >> 16); }
    CHECK(!looks(noise));
    std::vector<uint8_t> zeros(256, 0x00);
    CHECK(!looks(zeros));
    std::vector<uint8_t> ones(256, 0xFF);
    CHECK(!looks(ones));
}

} // namespace

int main()
{
    testLineStructure();
    testDigitsInNamesStringsAndRems();
    testKeywordSpacing();
    testControlCharactersSurvive();
    testKeywordTable();
    testLooksLikeProgram();
    testTrackedDisk();
    if (g_fail) {
        std::fprintf(stderr, "integer_basic_listing: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("integer_basic_listing: OK\n");
    return 0;
}
