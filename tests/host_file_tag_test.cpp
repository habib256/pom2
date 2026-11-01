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

// host_file_tag — the host exchange convention (HostFileTag.h): one spelling
// of an Apple file's type for the ProDOS host folder, pom2_dos33 and anything
// else that hands a file to the host.
//
// What this pins, beyond the parse itself:
//   * the composed name PARSES BACK to the same (type, aux) — the property
//     the whole convention exists for, checked over every type the tables
//     know and a sweep of aux values;
//   * a B file's load address survives (`GAME#064000`), which is the
//     regression `pom2_dos33` carried until 2026-11-01: `get` dropped it and
//     `put` defaulted to $2000, silently relocating the file;
//   * the DOS 3.3 ↔ ProDOS type map is a bijection over the eight DOS types,
//     so no `get` of any catalog type collides with another's spelling.

#include "HostFileTag.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

using namespace pom2;

// What an importer does with a host name: tag first, else the extension,
// and a well-known extension comes off the Apple name. Mirrors
// `scanHostFolder` (ProDOSVolume.cpp) and `pom2_dos33 put`.
void importName(const std::string& hostName, std::string& apple,
                std::uint8_t& type, std::uint16_t& aux)
{
    apple = hostName;
    type = 0; aux = 0;
    if (stripHostFileTag(apple, type, aux)) return;   // keeps its extension
    const std::size_t dot = apple.rfind('.');
    type = prodosTypeFromExtension(dot == std::string::npos ? ""
                                                            : apple.substr(dot));
    apple.resize(apple.size() - strippedExtensionLength(apple));
    // What ProDOSVolume assigns an untagged `.bas`, and what BASIC.SYSTEM
    // writes: the Applesoft load address.
    if (type == 0xFC) aux = 0x0801;
}

void testTagRoundTrip()
{
    for (unsigned t = 0; t <= 0xFF; ++t) {
        for (std::uint16_t a : { 0x0000, 0x0801, 0x2000, 0x4000, 0xFFFF, 0x0123 }) {
            const std::string tagged = "GAME" + hostFileTag(std::uint8_t(t), a);
            std::string name = tagged;
            std::uint8_t rt = 0xAA; std::uint16_t ra = 0x5555;
            CHECK(stripHostFileTag(name, rt, ra));
            CHECK(name == "GAME");
            CHECK(rt == std::uint8_t(t));
            CHECK(ra == a);
        }
    }
    // Not a tag: wrong length, non-hex, no '#'. Both outputs untouched.
    for (const char* n : { "GAME", "GAME#06400", "GAME#0640000",
                           "GAME#06400G", "GAME#", "#062000X" }) {
        std::string name = n;
        std::uint8_t t = 0xAA; std::uint16_t a = 0x5555;
        CHECK(!stripHostFileTag(name, t, a));
        CHECK(name == n && t == 0xAA && a == 0x5555);
    }
    // The LAST '#' wins, so a name that itself holds one still round-trips.
    std::string odd = "A#B" + hostFileTag(0x06, 0x2000);
    std::uint8_t t = 0; std::uint16_t a = 0;
    CHECK(stripHostFileTag(odd, t, a));
    CHECK(odd == "A#B" && t == 0x06 && a == 0x2000);
}

// The property the convention exists for: compose, then import, and get the
// same type and aux back. Checked over every type an extension can name and
// every type it cannot.
void testComposeParses()
{
    for (unsigned ty = 0; ty <= 0xFF; ++ty) {
        for (std::uint16_t ax : { 0x0000, 0x0801, 0x2000, 0xBEEF }) {
            for (const char* apple : { "GAME", "HELLO.BAS", "NOTES.TXT",
                                       "PIC", "A.B", "DATA.XYZ" }) {
                const std::string host =
                    hostNameForType(apple, std::uint8_t(ty), ax);
                std::string back; std::uint8_t rt = 0; std::uint16_t ra = 0;
                importName(host, back, rt, ra);
                if (rt != std::uint8_t(ty) || ra != ax || back != apple)
                    std::fprintf(stderr,
                        "FAIL %s:%d: %s type $%02X aux $%04X -> \"%s\" -> "
                        "%s type $%02X aux $%04X\n", __FILE__, __LINE__,
                        apple, ty, ax, host.c_str(), back.c_str(), rt, ra);
                CHECK(rt == std::uint8_t(ty));
                CHECK(ra == ax);
                CHECK(back == apple);
            }
        }
    }
}

// The cheap spellings are still chosen where they are safe: an extension
// says the type when it can, and only then.
void testPrefersExtensions()
{
    CHECK(hostNameForType("NOTES", 0x04, 0x0000) == "NOTES.txt");
    CHECK(hostNameForType("HELLO", 0xFC, 0x0801) == "HELLO.bas");   // BASIC's own aux
    CHECK(hostNameForType("PROG",  0xFA, 0x0000) == "PROG.int");
    CHECK(hostNameForType("DRIVER", 0xFF, 0x0000) == "DRIVER.sys");
    CHECK(hostNameForType("GAME",  0x06, 0x0000) == "GAME.bin");
    CHECK(hostNameForType("GAME",  0x00, 0x0000) == "GAME");        // typeless
    // An aux an extension cannot carry, and a type no extension names.
    CHECK(hostNameForType("GAME", 0x06, 0x4000) == "GAME#064000");
    CHECK(hostNameForType("DOC",  0x1A, 0x0000) == "DOC#1A0000");   // AppleWorks
    CHECK(hostNameForType("HELLO", 0xFC, 0x2000) == "HELLO#FC2000");
    // An Apple name that already ends in a stripped extension goes out
    // TAGGED, or the import would rename it (`RUN HELLO.BAS` then failed).
    CHECK(hostNameForType("HELLO.BAS", 0xFC, 0x0801) == "HELLO.BAS#FC0801");
    CHECK(hostNameForType("NOTES.TXT", 0x04, 0x0000) == "NOTES.TXT#040000");
    // A dotted name whose own suffix names the type needs nothing added.
    CHECK(hostNameForType("DATA.XYZ", 0x06, 0x0000) == "DATA.XYZ");
}

void testDos33Map()
{
    // The eight DOS 3.3 catalog types, and nothing shared between them.
    const std::uint8_t dosTypes[] = { 0x00, 0x01, 0x02, 0x04,
                                      0x08, 0x10, 0x20, 0x40 };
    std::set<std::uint8_t> seen;
    for (std::uint8_t d : dosTypes) {
        const std::uint8_t p = prodosTypeFromDos33(d);
        CHECK(seen.insert(p).second);          // a bijection, so no collisions
        std::uint8_t back = 0xAA;
        CHECK(dos33TypeFromProdos(p, back));
        CHECK(back == d);
    }
    // The four equivalences ProDOS's own CONVERT uses — these are the ones
    // that must agree with `prodosTypeFromExtension`, so a file taken off a
    // DOS disk lands in a ProDOS host folder as the same type.
    CHECK(prodosTypeFromDos33(0x00) == prodosTypeFromExtension(".txt"));  // T
    CHECK(prodosTypeFromDos33(0x01) == prodosTypeFromExtension(".int"));  // I
    CHECK(prodosTypeFromDos33(0x02) == prodosTypeFromExtension(".bas"));  // A
    CHECK(prodosTypeFromDos33(0x04) == prodosTypeFromExtension(".bin"));  // B
    // A ProDOS type with no DOS counterpart is refused, not guessed.
    std::uint8_t unused = 0xAA;
    CHECK(!dos33TypeFromProdos(0xFF, unused));   // SYS
    CHECK(!dos33TypeFromProdos(0x1A, unused));   // AppleWorks WP
    CHECK(unused == 0xAA);
}

// The pom2_dos33 regression, at the level of the names it composes and reads.
void testDos33LoadAddressSurvives()
{
    // `get GAME` on a B file loaded at $4000.
    const std::string host =
        hostNameForType("GAME", prodosTypeFromDos33(0x04), 0x4000);
    CHECK(host == "GAME#064000");
    // `put GAME#064000` must put it back at $4000, not at the old $2000
    // default.
    std::string apple; std::uint8_t ptype = 0; std::uint16_t paux = 0;
    importName(host, apple, ptype, paux);
    std::uint8_t dosType = 0;
    CHECK(apple == "GAME");
    CHECK(dos33TypeFromProdos(ptype, dosType) && dosType == 0x04);
    CHECK(paux == 0x4000);
    // A B file at $0000 spells itself with an extension and still comes back
    // to $0000 — the aux an extension can carry.
    const std::string zero =
        hostNameForType("DATA", prodosTypeFromDos33(0x04), 0x0000);
    CHECK(zero == "DATA.bin");
    importName(zero, apple, ptype, paux);
    CHECK(apple == "DATA" && paux == 0x0000);
    CHECK(dos33TypeFromProdos(ptype, dosType) && dosType == 0x04);
    // An Applesoft file: `.bas` both ways, and the aux the import assigns is
    // the one `hostNameForType` treats as plain, so the name stays short.
    const std::string bas =
        hostNameForType("HELLO", prodosTypeFromDos33(0x02), 0x0801);
    CHECK(bas == "HELLO.bas");
    importName(bas, apple, ptype, paux);
    CHECK(apple == "HELLO" && paux == 0x0801);
    CHECK(dos33TypeFromProdos(ptype, dosType) && dosType == 0x02);
}

} // namespace

int main()
{
    testTagRoundTrip();
    testComposeParses();
    testPrefersExtensions();
    testDos33Map();
    testDos33LoadAddressSurvives();
    if (g_fail) { std::fprintf(stderr, "host_file_tag: %d failure(s)\n", g_fail); return 1; }
    std::printf("host_file_tag: OK\n");
    return 0;
}
