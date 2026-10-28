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

// cpm_disk_fs — the CP/M 2.2 file system of an Apple II 140 KB image
// (CpmDiskFs.h), on a synthesised blank disk and on the two tracked
// SoftCard masters. In memory only: the tracked images are never written.

#include "CpmDiskFs.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace pom2::cpmfs;

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

// A freshly formatted disk: the directory blocks (track 3, DOS sectors
// kSkew[0..7]) all $E5, everything else zero.
std::vector<uint8_t> blankImage()
{
    std::vector<uint8_t> img(kImageBytes, 0);
    for (int n = 0; n < 8; ++n)
        std::memset(img.data() + (3 * 16 + kSkew[n]) * 256, 0xE5, 256);
    return img;
}

std::vector<uint8_t> pattern(size_t n, uint8_t seed)
{
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : v) { x = x * 1103515245u + 12345u; b = uint8_t(x >> 16); }
    return v;
}

// CP/M keeps whole 128-byte records: what comes back is the data, then ^Z.
bool roundTrips(const std::vector<uint8_t>& in, const std::vector<uint8_t>& out)
{
    if (out.size() != (in.size() + 127) / 128 * 128) return false;
    if (std::memcmp(in.data(), out.data(), in.size()) != 0) return false;
    for (size_t i = in.size(); i < out.size(); ++i) if (out[i] != 0x1A) return false;
    return true;
}

std::vector<uint8_t> loadTracked(const char* rel)
{
    for (const char* pre : { "", "../", "../../" }) {
        std::ifstream f(std::string(pre) + rel, std::ios::binary);
        if (f) return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
    }
    return {};
}

void testBlank()
{
    CpmDisk d;
    std::string err;
    CHECK(d.open(blankImage(), SectorOrder::Dos, err));
    CHECK(d.list().empty());
    CHECK(d.freeBlocks() == 126);   // 128 - the two directory blocks

    // Sizes either side of a record, a block and an extent (16 KB).
    const size_t sizes[] = { 0, 1, 127, 128, 129, 1024, 1025, 16384, 16385, 40000 };
    int k = 0;
    for (size_t n : sizes) {
        const auto data = pattern(n, uint8_t(k));
        const std::string name = "F" + std::to_string(k++) + ".BIN";
        CHECK(d.writeFile(0, name, data, err));
        std::vector<uint8_t> back;
        CHECK(d.readFile(0, name, back, err));
        CHECK(roundTrips(data, back));
    }
    const auto files = d.list();
    CHECK(files.size() == 10);
    CHECK(files.back().name == "F9.BIN" && files.back().bytes == 40064 && files.back().blocks == 40);

    // Replace keeps one file, frees the old blocks first.
    const int before = d.freeBlocks();
    CHECK(d.writeFile(0, "f9.bin", pattern(1000, 99), err));
    CHECK(d.list().size() == 10);
    CHECK(d.freeBlocks() == before + 40 - 1);

    // User areas are separate name spaces.
    CHECK(d.writeFile(3, "F0.BIN", pattern(300, 7), err));
    std::vector<uint8_t> a, b;
    CHECK(d.readFile(0, "F0.BIN", a, err) && a.empty());
    CHECK(d.readFile(3, "F0.BIN", b, err) && b.size() == 384);

    CHECK(d.removeFile(0, "F8.BIN", err));
    CHECK(!d.readFile(0, "F8.BIN", a, err));
    CHECK(!d.removeFile(0, "F8.BIN", err));

    // Disk full: refused, and nothing changed.
    const auto snapshot = d.image();
    CHECK(!d.writeFile(0, "HUGE.BIN", pattern(200 * 1024, 1), err));
    CHECK(err.find("disk full") != std::string::npos);
    CHECK(d.image() == snapshot);

    // Names CP/M cannot hold or the CCP cannot type.
    for (const char* bad : { "", ".COM", "TOOLONGNAME.COM", "A.LONG", "A B.COM",
                             "A*.COM", "A:B", "A,B" })
        CHECK(!d.writeFile(0, bad, {}, err));
}

void testDirectoryFull()
{
    CpmDisk d;
    std::string err;
    CHECK(d.open(blankImage(), SectorOrder::Dos, err));
    for (int i = 0; i < 64; ++i)
        CHECK(d.writeFile(0, "E" + std::to_string(i), {}, err));
    const auto snapshot = d.image();
    CHECK(!d.writeFile(0, "ONEMORE", pattern(10, 1), err));
    CHECK(err.find("directory full") != std::string::npos);
    CHECK(d.image() == snapshot);
}

// The same disk in ProDOS order reads the same files.
void testProdosOrder()
{
    CpmDisk d;
    std::string err;
    CHECK(d.open(blankImage(), SectorOrder::Dos, err));
    const auto data = pattern(5000, 42);
    CHECK(d.writeFile(0, "PO.TXT", data, err));
    std::vector<uint8_t> po(kImageBytes);
    for (int t = 0; t < 35; ++t)
        for (int s = 0; s < 16; ++s)
            std::memcpy(po.data() + (t * 16 + kDosToProdos[s]) * 256,
                        d.image().data() + (t * 16 + s) * 256, 256);
    CpmDisk p;
    CHECK(p.open(po, SectorOrder::Prodos, err));
    std::vector<uint8_t> back;
    CHECK(p.readFile(0, "PO.TXT", back, err));
    CHECK(roundTrips(data, back));
    // Read in the wrong order, the directory is not there.
    CpmDisk wrong;
    CHECK(!wrong.open(po, SectorOrder::Dos, err) || wrong.list().empty());
    CHECK(orderForPath("x/DISK.PO") == SectorOrder::Prodos);
    CHECK(orderForPath("disk.dsk") == SectorOrder::Dos);
}

void testTracked()
{
    const auto img22 = loadTracked("disks_5.4/dsk/cpm22.dsk");
    const auto img60 = loadTracked("disks_5.4/dsk/cpm60k.dsk");
    if (img22.empty() || img60.empty()) {
        std::fprintf(stderr, "FAIL: tracked CP/M masters not found\n");
        ++g_fail;
        return;
    }
    std::string err;
    CpmDisk d22;
    CHECK(d22.open(img22, SectorOrder::Dos, err));
    std::vector<uint8_t> dump;
    CHECK(d22.readFile(0, "DUMP.ASM", dump, err));
    CHECK(dump.size() == 4224);
    CHECK(dump.size() > 26 && std::memcmp(dump.data(), ";\tFILE DUMP PROGRAM, READS", 26) == 0);
    bool mbasic = false;
    for (const auto& f : d22.list())
        if (f.name == "MBASIC.COM") mbasic = f.blocks > 16;   // two extents, one file
    CHECK(mbasic);

    // The 60K sysgen: the user-31 "cp/m.sys" entry maps blocks 128-139
    // onto tracks 0-2. Not listed, never freed, never overwritten.
    CpmDisk d60;
    CHECK(d60.open(img60, SectorOrder::Dos, err));
    for (const auto& f : d60.list()) CHECK(f.user <= 15);
    CHECK(d60.freeBlocks() == 0);
    CHECK(!d60.writeFile(0, "X.COM", pattern(2000, 3), err));
    CHECK(d60.removeFile(0, "DUMP.ASM", err));
    CHECK(d60.writeFile(0, "X.COM", pattern(2000, 3), err));
    CHECK(std::memcmp(d60.image().data(), img60.data(), 3 * 16 * 256) == 0);
}

} // namespace

int main()
{
    testBlank();
    testDirectoryFull();
    testProdosOrder();
    testTracked();
    if (g_fail) { std::fprintf(stderr, "cpm_disk_fs: %d failure(s)\n", g_fail); return 1; }
    std::printf("cpm_disk_fs: PASS\n");
    return 0;
}
