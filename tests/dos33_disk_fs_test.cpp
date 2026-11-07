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

// dos33_disk_fs — pom2_dos33's library (Dos33DiskFs.h) and the Applesoft
// listing (ApplesoftListing.h).
//
//  1. A blank DOS 3.3 volume built here (VTOC + catalog on track 17, as
//     INIT lays them out): put / get / rm round trips for T, B and A files,
//     a multi-list file (> 122 sectors), the free count coming back, a locked
//     file refused, disk full refused with the image untouched.
//  2. The same volume in ProDOS order reads the same.
//  3. Applesoft tokens back to text.
//  4. The DOS 3.3 System Master, when present: HELLO's first line.

#include "ApplesoftListing.h"
#include "Dos33DiskFs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace pom2::dos33fs;

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

uint8_t* sec(std::vector<uint8_t>& img, int t, int s) { return img.data() + (t * 16 + s) * 256; }

// What INIT writes: VTOC at 17/0, catalog 17/15 down to 17/1, tracks 0-2
// (DOS) and 17 in use.
std::vector<uint8_t> blankVolume()
{
    std::vector<uint8_t> img(kImageBytes, 0);
    uint8_t* v = sec(img, 17, 0);
    v[0x01] = 17; v[0x02] = 15; v[0x03] = 3; v[0x06] = 254; v[0x27] = 122;
    v[0x30] = 17; v[0x31] = 1; v[0x34] = 35; v[0x35] = 16; v[0x36] = 0; v[0x37] = 1;
    for (int t = 0; t < 35; ++t) {
        const bool used = t <= 2 || t == 17;
        v[0x38 + t * 4] = used ? 0 : 0xFF;
        v[0x39 + t * 4] = used ? 0 : 0xFF;
    }
    for (int s = 15; s >= 1; --s) {
        uint8_t* c = sec(img, 17, s);
        if (s > 1) { c[0x01] = 17; c[0x02] = uint8_t(s - 1); }
    }
    return img;
}

std::vector<uint8_t> toProdos(const std::vector<uint8_t>& dos)
{
    static const uint8_t k[16] = { 0, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 15 };
    std::vector<uint8_t> out(dos.size());
    for (int t = 0; t < 35; ++t)
        for (int s = 0; s < 16; ++s)
            std::memcpy(out.data() + (t * 16 + k[s]) * 256, dos.data() + (t * 16 + s) * 256, 256);
    return out;
}

void testVolume()
{
    Dos33Disk d;
    std::string err;
    CHECK(d.open(blankVolume(), SectorOrder::Dos, err));
    const int free0 = d.freeSectors();
    CHECK(free0 == 31 * 16);
    CHECK(d.list().empty());

    // Text: host lines in, DOS text on disk, host lines back.
    const std::string note = "HELLO WORLD\nSECOND LINE\n";
    CHECK(d.writeFile("note", FileType::Text,
                      Dos33Disk::textToDos({ note.begin(), note.end() }), 0, err));
    std::vector<uint8_t> raw;
    CHECK(d.readFile("NOTE", raw, err));
    CHECK(raw.size() == 256 && raw[0] == ('H' | 0x80) && raw[11] == 0x8D);
    const auto back = Dos33Disk::textFromDos(Dos33Disk::payload(0x00, raw));
    CHECK(std::string(back.begin(), back.end()) == note);

    // Binary: address + length header, a 40 KB payload over two T/S lists.
    std::vector<uint8_t> big(40000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7);
    CHECK(d.writeFile("BIG.BIN", FileType::Binary, big, 0x0803, err));
    CHECK(d.readFile("big.bin", raw, err));
    uint16_t addr = 0;
    CHECK(Dos33Disk::payload(0x04, raw, &addr) == big && addr == 0x0803);
    const auto files = d.list();
    CHECK(files.size() == 2 && files[1].name == "BIG.BIN" && files[1].sectors == 157 + 2);

    // Replace keeps one entry; remove gives every sector back.
    CHECK(d.writeFile("NOTE", FileType::Text, Dos33Disk::textToDos({ 'X', '\n' }), 0, err));
    CHECK(d.list().size() == 2);
    CHECK(d.removeFile("BIG.BIN", err) && d.removeFile("note", err));
    CHECK(d.freeSectors() == free0 && d.list().empty());
    CHECK(!d.removeFile("NOTE", err));

    // Locked: refused to replace and to remove.
    CHECK(d.writeFile("KEEP", FileType::Applesoft, { 1, 2, 3 }, 0, err));
    std::vector<uint8_t> img = d.image();
    sec(img, 17, 15)[0x0B + 2] |= 0x80;
    CHECK(d.open(img, SectorOrder::Dos, err));
    CHECK(d.list()[0].locked);
    CHECK(!d.removeFile("KEEP", err));
    CHECK(!d.writeFile("KEEP", FileType::Text, {}, 0, err));

    // Disk full: refused, nothing changed.
    const std::vector<uint8_t> before = d.image();
    std::vector<uint8_t> giant(std::size_t(d.freeSectors()) * 256 + 1000);
    CHECK(!d.writeFile("GIANT", FileType::Text, giant, 0, err));
    CHECK(err == "disk full");
    CHECK(d.image() == before);

    // Names DOS cannot hold.
    CHECK(!d.writeFile("1ABC", FileType::Text, {}, 0, err));
    CHECK(!d.writeFile("A,B", FileType::Text, {}, 0, err));
    CHECK(!d.writeFile(std::string(31, 'A'), FileType::Text, {}, 0, err));
}

void testProdosOrder()
{
    Dos33Disk a;
    std::string err;
    CHECK(a.open(blankVolume(), SectorOrder::Dos, err));
    CHECK(a.writeFile("F", FileType::Binary, { 9, 8, 7 }, 0x2000, err));
    Dos33Disk b;
    CHECK(b.open(toProdos(a.image()), SectorOrder::Prodos, err));
    std::vector<uint8_t> raw;
    CHECK(b.readFile("F", raw, err) && Dos33Disk::payload(0x04, raw) == std::vector<uint8_t>({ 9, 8, 7 }));
    CHECK(orderForPath("x.po") == SectorOrder::Prodos && orderForPath("x.DSK") == SectorOrder::Dos);
}

// A DOS-order disk named `.po` (and a ProDOS-order one named `.dsk`): the
// VTOC passes in both orders, so only the catalog chain can tell, and the
// extension's order used to lose every entry past catalog sector 17/14 and
// WRITE each sector to the wrong place (bug hunt 2026-11-07).
void testWrongExtension()
{
    Dos33Disk a;
    std::string err;
    CHECK(a.open(blankVolume(), SectorOrder::Dos, err));
    CHECK(!a.orderOverridden());
    for (int i = 0; i < 9; ++i)   // 7 entries a catalog sector: two sectors used
        CHECK(a.writeFile(std::string("F") + char('A' + i), FileType::Binary,
                          { uint8_t(i), 1, 2 }, uint16_t(0x2000 + i), err));
    const std::vector<uint8_t> dosImage = a.image();

    Dos33Disk b;   // the same bytes, opened as if named GAME.po
    CHECK(b.open(dosImage, SectorOrder::Prodos, err));
    CHECK(b.orderOverridden() && b.order() == SectorOrder::Dos);
    CHECK(b.list().size() == 9);
    CHECK(b.writeFile("NEW", FileType::Binary, { 7, 7, 7 }, 0x4000, err));
    Dos33Disk c;   // read back in the order the disk really has
    CHECK(c.open(b.image(), SectorOrder::Dos, err));
    std::vector<uint8_t> raw;
    for (int i = 0; i < 9; ++i) {
        CHECK(c.readFile(std::string("F") + char('A' + i), raw, err));
        CHECK(Dos33Disk::payload(0x04, raw) == std::vector<uint8_t>({ uint8_t(i), 1, 2 }));
    }
    uint16_t addr = 0;
    CHECK(c.readFile("NEW", raw, err) && Dos33Disk::payload(0x04, raw, &addr) == std::vector<uint8_t>({ 7, 7, 7 }));
    CHECK(addr == 0x4000);

    Dos33Disk p;   // ProDOS-order bytes named `.dsk`
    CHECK(p.open(toProdos(dosImage), SectorOrder::Dos, err));
    CHECK(p.orderOverridden() && p.order() == SectorOrder::Prodos && p.list().size() == 9);
}

// A random-access text file: fixed-length records, $00 between them and
// 0/0 holes for sectors never written. `payload` used to stop at the first
// $00 and keep record 0 alone (bug hunt 2026-11-07).
void testRandomAccessText()
{
    Dos33Disk d;
    std::string err;
    CHECK(d.open(blankVolume(), SectorOrder::Dos, err));
    const char* r0 = "ALPHA\n"; const char* r3 = "OMEGA\n";
    const auto t0 = Dos33Disk::textToDos({ r0, r0 + 6 });
    const auto t3 = Dos33Disk::textToDos({ r3, r3 + 6 });
    std::vector<uint8_t> recs(768 + t3.size(), 0);  // record length 256
    std::copy(t0.begin(), t0.end(), recs.begin());
    std::copy(t3.begin(), t3.end(), recs.begin() + 768);
    CHECK(d.writeFile("RAND", FileType::Text, recs, 0, err));
    std::vector<uint8_t> img = d.image();
    // Records 1 and 2 were never written: make their sectors holes.
    uint8_t* list = sec(img, 18, 15);
    CHECK(list[0x0C + 2] != 0);
    list[0x0C + 2] = 0; list[0x0D + 2] = 0;
    list[0x0C + 4] = 0; list[0x0D + 4] = 0;
    CHECK(d.open(img, SectorOrder::Dos, err));
    std::vector<uint8_t> raw;
    CHECK(d.readFile("RAND", raw, err));
    CHECK(Dos33Disk::payload(0x00, raw) == recs);
    // $00 survives the host trip both ways.
    CHECK(Dos33Disk::textToDos({ 'A', 0, 'B' }) == std::vector<uint8_t>({ 0xC1, 0x00, 0xC2 }));
    // A sequential file still ends at its $00.
    CHECK(Dos33Disk::payload(0x00, { 0xC1, 0xC2, 0x00, 0x00 }) == std::vector<uint8_t>({ 0xC1, 0xC2 }));
}

// "FOO " is FOO: the catalog's names are right-trimmed, so the asked-for
// one must be too, or `put` added a second FOO that DOS never sees.
void testTrailingBlank()
{
    Dos33Disk d;
    std::string err;
    CHECK(d.open(blankVolume(), SectorOrder::Dos, err));
    CHECK(d.writeFile("FOO", FileType::Binary, { 1 }, 0x2000, err));
    CHECK(d.writeFile("FOO  ", FileType::Binary, { 2 }, 0x2000, err));
    CHECK(d.list().size() == 1);
    std::vector<uint8_t> raw;
    CHECK(d.readFile("FOO ", raw, err) && Dos33Disk::payload(0x04, raw) == std::vector<uint8_t>({ 2 }));
}

void testListing()
{
    // 10 HOME : PRINT "HI"   20 IF A=<1 THEN 10   (links as if at $0801)
    const std::vector<uint8_t> prog = {
        0x0D, 0x08, 0x0A, 0x00, 0x97, ':', 0xBA, '"', 'H', 'I', '"', 0x00,
        0x00,
    };
    // The second line, tokenised: IF A = < 1 THEN 10.
    std::vector<uint8_t> p2 = { 0x1A, 0x08, 0x14, 0x00, 0xAD, 'A', 0xD0, 0xD1, '1', 0xC4, '1', '0', 0x00,
                                0x00, 0x00 };
    std::vector<uint8_t> all(prog.begin(), prog.end() - 1);
    all.insert(all.end(), p2.begin(), p2.end());
    const std::string text = pom2::applesoft::list(0, [&](uint16_t a) {
        return a < all.size() ? all[a] : uint8_t(0);
    }, all.size());
    CHECK(text == "10 HOME : PRINT \"HI\"\n20 IF A=<1 THEN 10\n");
    if (g_fail) std::fprintf(stderr, "listing was:\n%s", text.c_str());
    CHECK(std::string(pom2::applesoft::keyword(0x80)) == "END");
    CHECK(std::string(pom2::applesoft::keyword(0xEA)) == "MID$");
    CHECK(pom2::applesoft::keyword(0xEB) == nullptr);

    // A control character inside a string is part of the program: DOS 3.3's
    // PRINT "<Ctrl-D>CATALOG" idiom. LIST types it as it is; it used to come
    // out as '?', and the listing typed back was another program. Only CR and
    // LF, which would cut the line in two, keep the '?'.
    const std::vector<uint8_t> ctl = {
        0x0D, 0x08, 0x1E, 0x00, 0xBA, '"', 0x04, 'C', 'A', 'T', 0x07, 0x0D, '"', 0x00,
        0x00, 0x00 };
    const std::string t2 = pom2::applesoft::list(0, [&](uint16_t a) {
        return a < ctl.size() ? ctl[a] : uint8_t(0);
    }, ctl.size());
    CHECK(t2 == "30 PRINT \"\x04" "CAT\x07?\"\n");

    // ── A listing typed back must rebuild the program (2026-11-05) ──────
    // The tokeniser STORES the spaces of a REM and of a DATA statement
    // (Applesoft ROM $D559: blanks are only eaten outside them), so the
    // bytes below are what `10 REM HELLO`, `20 DATA 1, 2 ` and the rest
    // become. The listing used to write a space after EVERY keyword, REM and
    // DATA included — one more stored space per trip — and to strip every
    // trailing space, the program's own with it.
    const auto listOf = [](const std::vector<uint8_t>& body,
                           pom2::applesoft::ListReport* report = nullptr) {
        std::vector<uint8_t> bytes = { 0x01, 0x08, 0x0A, 0x00 };   // link, line 10
        bytes.insert(bytes.end(), body.begin(), body.end());
        bytes.insert(bytes.end(), { 0x00, 0x00, 0x00 });
        return pom2::applesoft::list(0, [&](uint16_t a) {
            return a < bytes.size() ? bytes[a] : uint8_t(0);
        }, bytes.size(), report);
    };
    // REM: the payload as stored, the typed space and no other.
    CHECK(listOf({ 0xB2, ' ', 'H', 'E', 'L', 'L', 'O' }) == "10 REM HELLO\n");
    CHECK(listOf({ 0xB2, 'H', 'I' }) == "10 REMHI\n");          // typed `REMHI`
    CHECK(listOf({ 0xB2, ' ', 'A', ' ', ' ' }) == "10 REM A  \n");   // its trailing spaces
    // DATA: the same, up to the ':' that closes the statement.
    CHECK(listOf({ 0x83, ' ', '1', ',', ' ', '2', ' ' }) == "10 DATA 1, 2 \n");
    CHECK(listOf({ 0x83, ' ', '1', ':', 0xBA, 'A' }) == "10 DATA 1: PRINT A\n");
    CHECK(listOf({ 0x83, '"', 'A', ':', 'B', '"', ':', 0x80 }) == "10 DATA\"A:B\": END\n");
    // A string left open keeps the space it ends on.
    CHECK(listOf({ 0xBA, '"', 'A', ' ' }) == "10 PRINT \"A \n");
    // The space this listing adds after a last keyword still goes.
    CHECK(listOf({ 0x80 }) == "10 END\n");
    CHECK(listOf({ 0xB1 }) == "10 RETURN\n");
    // What text cannot hold is REPORTED: a byte past the last token, a token
    // inside a string, a CR. None of them changes what an ordinary program
    // lists as.
    pom2::applesoft::ListReport report;
    (void)listOf({ 0xBA, '"', 'A', '"' }, &report);
    CHECK(report.untypable == 0 && report.firstLine == -1);
    (void)listOf({ 0xBA, 0xEB }, &report);
    CHECK(report.untypable == 1 && report.firstLine == 10);
    (void)listOf({ 0xBA, '"', 0x80, 0x0D, '"' }, &report);
    CHECK(report.untypable == 2 && report.firstLine == 10);
}

int testMaster()
{
    const char* path = "disks_5.4/dsk/dos33_master.dsk";
    std::ifstream f(path, std::ios::binary);
    if (!f) return 77;
    std::vector<uint8_t> img{ std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
    Dos33Disk d;
    std::string err;
    CHECK(d.open(img, SectorOrder::Dos, err));
    std::vector<uint8_t> raw;
    CHECK(d.readFile("HELLO", raw, err));
    const auto prog = Dos33Disk::payload(0x02, raw);
    const std::string text = pom2::applesoft::list(0, [&](uint16_t a) {
        return a < prog.size() ? prog[a] : uint8_t(0);
    }, prog.size());
    CHECK(text.rfind("10 TEXT : HOME\n", 0) == 0);
    // COPYA line 70: PRINT "<Ctrl-D>BLOAD COPY.OBJ0".
    CHECK(d.readFile("COPYA", raw, err));
    const auto copya = Dos33Disk::payload(0x02, raw);
    const std::string t3 = pom2::applesoft::list(0, [&](uint16_t a) {
        return a < copya.size() ? copya[a] : uint8_t(0);
    }, copya.size());
    CHECK(t3.find("70 PRINT \"\x04" "BLOAD COPY.OBJ0\"") != std::string::npos);
    return 0;
}

} // namespace

// `payload` must not form an out-of-range iterator on a file with no data.
// A catalog entry whose T/S list is 122 all-zero pairs leaves `raw` EMPTY;
// `raw.begin() + 4` is then undefined — harmless in a release build, an abort
// under _LIBCPP_HARDENING_MODE / _GLIBCXX_ASSERTIONS / UBSan, which is what
// the nightly sanitizer job runs. Reachable from `pom2_dos33 get` on a
// corrupt image.
void testEmptyPayload()
{
    uint16_t addr = 0xFFFF;
    const std::vector<uint8_t> empty;
    CHECK(pom2::dos33fs::Dos33Disk::payload(0x04, empty, &addr).empty());   // B
    CHECK(pom2::dos33fs::Dos33Disk::payload(0x02, empty, nullptr).empty()); // A
    CHECK(pom2::dos33fs::Dos33Disk::payload(0x01, empty, nullptr).empty()); // I
    CHECK(pom2::dos33fs::Dos33Disk::payload(0x00, empty, nullptr).empty()); // T
    // A one-byte and a three-byte file are the other side of the bound: the
    // length word itself is incomplete, so there is no payload either.
    CHECK(pom2::dos33fs::Dos33Disk::payload(0x02, std::vector<uint8_t>{0x01}, nullptr).empty());
    CHECK(pom2::dos33fs::Dos33Disk::payload(0x04, std::vector<uint8_t>{0, 0x20, 0x01}, &addr).empty());

    // A LENGTH THAT LIES — the case the clamp is for, and the one a test can
    // really observe. A B file declaring $FFFF bytes with three on the disk
    // must yield those three, not read past the sectors that were read.
    // (The empty-vector guards above remove undefined `begin() + 4`
    // arithmetic that a release build happens to survive; only the nightly
    // sanitizer job can see that half, the way it saw TMS9918's `~regs[3]`.)
    {
        const std::vector<uint8_t> liar{ 0x00, 0x20, 0xFF, 0xFF, 1, 2, 3 };
        const auto got = pom2::dos33fs::Dos33Disk::payload(0x04, liar, &addr);
        CHECK(addr == 0x2000);
        CHECK(got.size() == 3);
        CHECK(got.size() == 3 && got[0] == 1 && got[1] == 2 && got[2] == 3);
        // And an A/I file the same way: length word $FFFF, two bytes present.
        const std::vector<uint8_t> liarA{ 0xFF, 0xFF, 0x41, 0x42 };
        const auto gotA = pom2::dos33fs::Dos33Disk::payload(0x02, liarA, nullptr);
        CHECK(gotA.size() == 2);
        CHECK(gotA.size() == 2 && gotA[0] == 0x41 && gotA[1] == 0x42);
    }
}

int main()
{
    testEmptyPayload();
    testVolume();
    testProdosOrder();
    testWrongExtension();
    testRandomAccessText();
    testTrailingBlank();
    testListing();
    const int master = testMaster();
    if (g_fail) {
        std::fprintf(stderr, "dos33_disk_fs: %d failure(s)\n", g_fail);
        return 1;
    }
    if (master == 77) {
        // RETURN it: the skip code used to decorate a string and the test
        // exited 0, so a missing System Master read as a full pass — and the
        // word was lowercase and mid-line, so neither half of
        // tools/check_ctest_skips.sh could see it either. The fixture is in
        // tools/test_media.sha256, so on CI this is never taken.
        std::printf("SKIP dos33_disk_fs: disks_5.4/dsk/dos33_master.dsk absent\n");
        return 77;
    }
    std::printf("dos33_disk_fs: PASS\n");
    return 0;
}
