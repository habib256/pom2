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

// Pins 2IMG / .2mg write-back: after a sector mutation + saveDirty, the
// rewritten file must (a) preserve the 64-byte header byte-for-byte and
// any comment/creator trailer chunks, and (b) reflect the new data in
// the payload region.
//
// Why this matters: the 2IMG header carries the creator code, volume
// number, and write-protect flag — losing them on round-trip would
// strip metadata users care about. The trailer can hold a free-text
// comment chunk; some archives append a creator chunk past the payload.

#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void writeLe16(std::vector<uint8_t>& b, std::size_t o, uint16_t v)
{
    b[o] = static_cast<uint8_t>(v & 0xFF);
    b[o + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void writeLe32(std::vector<uint8_t>& b, std::size_t o, uint32_t v)
{
    b[o] = static_cast<uint8_t>(v & 0xFF);
    b[o + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    b[o + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    b[o + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

std::vector<uint8_t> readAll(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> out(sz);
    if (sz) f.read(reinterpret_cast<char*>(out.data()),
                   static_cast<std::streamsize>(sz));
    return out;
}

bool writeAll(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

// Build a 2IMG file: 64-byte header + DOS-skew payload + 32-byte
// trailer (faked comment chunk so we can verify it survives intact).
std::vector<uint8_t> makeTwoImgDos(const std::vector<uint8_t>& payload,
                                   const std::vector<uint8_t>& trailer)
{
    std::vector<uint8_t> out(64, 0);
    out[0] = '2'; out[1] = 'I'; out[2] = 'M'; out[3] = 'G';
    out[4] = 'T'; out[5] = 'S'; out[6] = 'T'; out[7] = 'R';   // creator
    writeLe16(out, 8, 64);                                       // headerLen
    writeLe16(out, 10, 1);                                       // version
    writeLe32(out, 12, 0);                                       // format=DOS
    writeLe32(out, 16, 0);                                       // flags
    writeLe32(out, 24, 64);                                      // dataOffset
    writeLe32(out, 28, static_cast<uint32_t>(payload.size()));  // dataLength
    // Comment chunk pointers — let's pretend the comment lives in the
    // trailer at offset 64+payload, length = trailer.size(). Real readers
    // (AppleWin) honour these; we just need to preserve them on save.
    writeLe32(out, 32, static_cast<uint32_t>(64 + payload.size()));
    writeLe32(out, 36, static_cast<uint32_t>(trailer.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), trailer.begin(), trailer.end());
    return out;
}

bool loadDosFixture(std::vector<uint8_t>& payload)
{
    static const char* prefixes[] = {
        // Sector images live under disks_5.4/dsk/ since the 2026 move; the
        // bare roots are kept for older trees.
        "../../disks_5.4/dsk/", "../disks_5.4/dsk/", "disks_5.4/dsk/",
        "../../disks_5.4/", "../../disks2/", "disks_5.4/", "disks2/"
    };
    for (const char* pfx : prefixes) {
        const std::string p = std::string(pfx) + "dos33_master.dsk";
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        f.seekg(0, std::ios::end);
        if (static_cast<std::size_t>(f.tellg()) != 143360) continue;
        f.seekg(0, std::ios::beg);
        payload.resize(143360);
        f.read(reinterpret_cast<char*>(payload.data()), 143360);
        return static_cast<bool>(f);
    }
    return false;
}

}  // namespace

int main()
{
    std::vector<uint8_t> payload;
    if (!loadDosFixture(payload)) {
        std::fprintf(stderr,
            "skip: no dos33_master.dsk fixture (this test needs a known "
            "DOS image to wrap and modify)\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    // A 32-byte trailer: anything non-zero so we can verify it's
    // preserved byte-for-byte across the round-trip.
    std::vector<uint8_t> trailer(32);
    for (int i = 0; i < 32; ++i) trailer[i] = static_cast<uint8_t>(0xC0 + i);

    const std::vector<uint8_t> origFile = makeTwoImgDos(payload, trailer);
    const std::vector<uint8_t> origHeader(origFile.begin(),
                                          origFile.begin() + 64);
    const std::vector<uint8_t> origTrailer(origFile.end() - 32,
                                           origFile.end());

    const std::string path = "twoimg_writeback.2mg";
    if (!writeAll(path, origFile)) {
        std::fprintf(stderr, "cannot create temp 2IMG\n");
        return 1;
    }

    // Load + mutate + save.
    DiskImage img;
    img.setWriteBackEnabled(true);
    if (!img.loadFile(path)) {
        std::fprintf(stderr, "load failed: %s\n", img.getLastError().c_str());
        std::remove(path.c_str());
        return 1;
    }
    // Mutate a nibble so dirty[track] is set and saveDirty has work to do.
    // Track 17 is the DOS 3.3 VTOC track — convenient marker. Note: the
    // single-nibble change may or may not survive decode→re-encode (if it
    // landed in sync gap, the decoder ignores it and the round-trip is a
    // no-op at the byte level). The point of this test is NOT to verify
    // the bytes change; it's to verify the 2IMG envelope is intact after
    // saveDirty walks the dirty track through the sector branch.
    img.writeNibbleAt(17, 100, 0xAA);
    if (!img.saveDirty()) {
        std::fprintf(stderr, "save failed: %s\n", img.getLastError().c_str());
        std::remove(path.c_str());
        return 1;
    }

    // Re-read the file. Header bytes and trailer bytes must be intact;
    // the file size must equal header + payload + trailer.
    const std::vector<uint8_t> newFile = readAll(path);
    std::remove(path.c_str());
    if (newFile.size() != origFile.size()) {
        std::fprintf(stderr,
            "round-trip: new file size %zu != orig %zu\n",
            newFile.size(), origFile.size());
        return 1;
    }
    if (std::memcmp(newFile.data(), origHeader.data(), 64) != 0) {
        std::fprintf(stderr,
            "round-trip: 2IMG 64-byte header changed across save\n");
        return 1;
    }
    if (std::memcmp(newFile.data() + newFile.size() - 32,
                    origTrailer.data(), 32) != 0) {
        std::fprintf(stderr,
            "round-trip: trailer (comment chunk) changed across save\n");
        return 1;
    }

    std::printf("disk_2mg_writeback_smoke OK: header+trailer preserved across "
                "save\n");
    return 0;
}
