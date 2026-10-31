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
    return 0;
}

} // namespace

int main()
{
    testVolume();
    testProdosOrder();
    testListing();
    const int master = testMaster();
    if (g_fail) {
        std::fprintf(stderr, "dos33_disk_fs: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("dos33_disk_fs: PASS%s\n", master == 77 ? " (System Master absent: skipped)" : "");
    return 0;
}
