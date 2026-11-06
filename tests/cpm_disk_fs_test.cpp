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
    // An empty vector's data() may be null, and memcmp on null is UB even
    // for a zero length (UBSan's nonnull check, nightly asan-ubsan).
    if (!in.empty() && std::memcmp(in.data(), out.data(), in.size()) != 0) return false;
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

// MED-3 (bug hunt 2026-11-01 round 2): a file carrying CP/M's R/O attribute
// (t1', bit 7 of directory byte 9) is refused by ERA and by a write in CP/M,
// and the tool removed and replaced it. Set the bit in the image the way CP/M's
// STAT does, reopen, and both must be refused with the image untouched.
void testReadOnlyAttribute()
{
    CpmDisk d;
    std::string err;
    CHECK(d.open(blankImage(), SectorOrder::Dos, err));
    CHECK(d.writeFile(0, "KEEP.TXT", pattern(300, 5), err));
    auto img = d.image();
    const std::string n11 = "KEEP    TXT";
    int marked = 0;
    for (size_t i = 0; i + 12 <= img.size(); ++i)
        if (img[i] == 0 && std::memcmp(img.data() + i + 1, n11.data(), 11) == 0) {
            img[i + 9] |= 0x80;   // t1' = R/O
            ++marked;
        }
    CHECK(marked == 1);
    CpmDisk ro;
    CHECK(ro.open(img, SectorOrder::Dos, err));
    CHECK(ro.list().size() == 1 && ro.list()[0].readOnly);
    const auto before = ro.image();
    CHECK(!ro.removeFile(0, "KEEP.TXT", err) && err.find("read-only") != std::string::npos);
    CHECK(!ro.writeFile(0, "keep.txt", pattern(10, 1), err) && err.find("read-only") != std::string::npos);
    CHECK(ro.image() == before);
    std::vector<uint8_t> back;
    CHECK(ro.readFile(0, "KEEP.TXT", back, err) && roundTrips(pattern(300, 5), back));
    // Another user area's file of the same name is not the R/O one.
    CHECK(ro.writeFile(1, "KEEP.TXT", pattern(10, 1), err));
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

// A directory sector the formatter never wrote (zeros, not $E5) — ALS 3.01C
// Disks 2 and 3 end in one: its entries are free, not a reason to refuse.
void testZeroFilledDirectorySector()
{
    auto img = blankImage();
    std::memset(img.data() + (3 * 16 + kSkew[7]) * 256, 0x00, 256);
    CpmDisk d;
    std::string err;
    CHECK(d.open(img, SectorOrder::Dos, err));
    CHECK(d.list().empty());
    CHECK(d.freeBlocks() == 126);
    for (int i = 0; i < 64; ++i)
        CHECK(d.writeFile(0, "Z" + std::to_string(i), {}, err));
    CHECK(d.list().size() == 64);
}

// CP/M 3 (the ALS card's CP/M Plus) writes two entries that are not files:
// the directory label (user $20: SET [NAME=...], INITDIR) and, after INITDIR,
// an SFCB (user $21) every fourth entry, whose bytes 1-31 are date stamps,
// not block numbers. open() used to refuse such a disk outright.
void testCpm3LabelAndStamps()
{
    auto img = blankImage();
    uint8_t* dir = img.data() + (3 * 16 + kSkew[0]) * 256;
    const uint8_t label[32] = { 0x20, 'A', 'L', 'S', 'D', 'I', 'S', 'K', '1', ' ', ' ', ' ',
                                0x31, 0, 0, 0 };
    std::memcpy(dir, label, 32);
    std::memset(dir + 3 * 32, 0xC8, 32);            // an SFCB full of stamps
    dir[3 * 32] = 0x21;
    const std::vector<uint8_t> before(dir, dir + 4 * 32);
    CpmDisk d;
    std::string err;
    CHECK(d.open(img, SectorOrder::Dos, err));
    CHECK(d.list().empty());
    CHECK(d.freeBlocks() == 126);                   // stamps are not blocks
    const auto data = pattern(40000, 5);
    CHECK(d.writeFile(0, "BIG.DAT", data, err));
    std::vector<uint8_t> back;
    CHECK(d.readFile(0, "BIG.DAT", back, err) && roundTrips(data, back));
    CHECK(d.list().size() == 1);
    CHECK(d.removeFile(0, "BIG.DAT", err));
    const uint8_t* after = d.image().data() + (3 * 16 + kSkew[0]) * 256;
    CHECK(std::memcmp(after, before.data(), 32) == 0);           // label kept
    CHECK(std::memcmp(after + 96, before.data() + 96, 32) == 0); // SFCB kept
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
    for (int n = 0; n < 16; ++n) CHECK(kDosToCpm[kSkew[n]] == n);
    // CP/M order (.cpm): the de-skewed layout, convertible to DOS order.
    std::vector<uint8_t> cpm(kImageBytes);
    for (int t = 0; t < 35; ++t)
        for (int s = 0; s < 16; ++s)
            std::memcpy(cpm.data() + (t * 16 + kDosToCpm[s]) * 256,
                        d.image().data() + (t * 16 + s) * 256, 256);
    CpmDisk c;
    CHECK(c.open(cpm, SectorOrder::Cpm, err));
    back.clear();
    CHECK(c.readFile(0, "PO.TXT", back, err));
    CHECK(roundTrips(data, back));
    CHECK(c.imageInOrder(SectorOrder::Dos) == d.image());
    CHECK(orderForPath("a/b.cpm") == SectorOrder::Cpm);
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


// ---- Text exchange (2026-11-01). CP/M text is CR LF, ends at a ^Z and is
// stored in whole 128-byte records; pom2_cpm used to copy bytes both ways,
// so a `get` of a source file handed the host CR LF, a ^Z and the record
// padding, and a `put` left CP/M bare LF with no end marker.
void testTextExchange()
{
    const std::string host = "LDA 1\nSTA 2\nEND\n";
    const std::vector<uint8_t> hostBytes(host.begin(), host.end());

    const auto cpm = CpmDisk::textToCpm(hostBytes);
    // CR LF line ends, and a ^Z where the text stops.
    CHECK(cpm.size() == host.size() + 3 + 1);          // 3 LFs gain a CR, + ^Z
    CHECK(cpm[5] == '\r' && cpm[6] == '\n');
    CHECK(cpm.back() == 0x1A);
    // Every LF is preceded by a CR: no bare LF reaches a CP/M file.
    for (std::size_t i = 0; i < cpm.size(); ++i)
        CHECK(cpm[i] != '\n' || (i > 0 && cpm[i - 1] == '\r'));

    // And back, byte for byte.
    const auto back = CpmDisk::textFromCpm(cpm);
    CHECK(std::string(back.begin(), back.end()) == host);

    // The record padding goes with the end marker: what a real CP/M file
    // looks like after `writeFile` pads it.
    std::vector<uint8_t> padded = cpm;
    padded.resize(128, 0x1A);
    const auto unpadded = CpmDisk::textFromCpm(padded);
    CHECK(std::string(unpadded.begin(), unpadded.end()) == host);

    // CRLF in, and a lone CR (old Mac), both normalise.
    for (const char* src : { "A\r\nB\r\n", "A\rB\r", "A\nB\n" }) {
        const std::string s = src;
        const auto round = CpmDisk::textFromCpm(
            CpmDisk::textToCpm({ s.begin(), s.end() }));
        CHECK(std::string(round.begin(), round.end()) == "A\nB\n");
    }

    // Empty text is a lone ^Z, and comes back empty.
    CHECK(CpmDisk::textToCpm({}).size() == 1);
    CHECK(CpmDisk::textFromCpm(CpmDisk::textToCpm({})).empty());

    // A ^Z inside the text truncates there — CP/M 2.2 has no byte count, so
    // this is a property of the format, not a choice.
    const std::vector<uint8_t> cut = { 'A', 0x1A, 'B' };
    CHECK(CpmDisk::textFromCpm(cut).size() == 1);
}

// Which files are text, and the host name that says so.
void testTextNaming()
{
    CHECK(extensionMeansText("PROG.ASM"));
    CHECK(extensionMeansText("read.me"));              // case-insensitive
    CHECK(extensionMeansText("NOTES.TXT"));
    CHECK(extensionMeansText("HELLO.C") && extensionMeansText("HELLO.H"));
    CHECK(extensionMeansText("OLD.BAK"));
    CHECK(!extensionMeansText("BTERM.COM"));
    CHECK(!extensionMeansText("NOTES"));               // no extension at all
    // BASIC-80 tokenizes unless told `,A`, so BAS is NOT text by default.
    CHECK(!extensionMeansText("GAME.BAS"));

    // Export: `.TXT` marks a converted file, and is never doubled.
    CHECK(hostNameForCpm("PROG.ASM", true)  == "PROG.ASM.TXT");
    CHECK(hostNameForCpm("READ.ME",  true)  == "READ.ME.TXT");
    CHECK(hostNameForCpm("README.TXT", true) == "README.TXT");
    CHECK(hostNameForCpm("BTERM.COM", false) == "BTERM.COM");

    // Import: the marker comes off when what is left has its own extension.
    bool isText = false;
    CHECK(cpmNameFromHost("PROG.ASM.TXT", isText) == "PROG.ASM" && isText);
    CHECK(cpmNameFromHost("README.TXT", isText) == "README.TXT" && isText);
    CHECK(cpmNameFromHost("BTERM.COM", isText) == "BTERM.COM" && !isText);
    CHECK(cpmNameFromHost("PROG.ASM", isText) == "PROG.ASM" && isText);
    CHECK(cpmNameFromHost("GAME.BAS", isText) == "GAME.BAS" && !isText);
    // A path is accepted; only the leaf is the name.
    CHECK(cpmNameFromHost("some/dir/PROG.ASM.TXT", isText) == "PROG.ASM");

    // Every CP/M name that round-trips must come back as itself.
    for (const char* n : { "PROG.ASM", "READ.ME", "README.TXT", "BTERM.COM",
                           "GAME.BAS", "LETTER.DOC" }) {
        const bool text = extensionMeansText(n);
        bool backText = false;
        CHECK(cpmNameFromHost(hostNameForCpm(n, text), backText) == n);
        CHECK(backText == text);
    }

    // The one documented loss: an extensionless file taken off AS TEXT by
    // override cannot say it was extensionless.
    bool t2 = false;
    CHECK(hostNameForCpm("NOTES", true) == "NOTES.TXT");
    CHECK(cpmNameFromHost("NOTES.TXT", t2) == "NOTES.TXT");   // not "NOTES"
}

} // namespace

int main()
{
    testBlank();
    testReadOnlyAttribute();
    testDirectoryFull();
    testZeroFilledDirectorySector();
    testCpm3LabelAndStamps();
    testProdosOrder();
    testTracked();
    testTextExchange();
    testTextNaming();
    if (g_fail) { std::fprintf(stderr, "cpm_disk_fs: %d failure(s)\n", g_fail); return 1; }
    std::printf("cpm_disk_fs: PASS\n");
    return 0;
}
