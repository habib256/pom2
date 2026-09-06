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

// 13-sector (DOS 3.1/3.2/3.2.1) round-trip pin. A synthesized 116480-byte
// .d13 image nibblizes through the 5-and-3 GCR encoder (nibblizeTrack13)
// on load, and decodes back byte-for-byte through the write-back path
// (decodeTrack13) on saveDirty. No network, no PROM, no boot needed —
// this pins the GCR encode↔decode pair (port of MAME ap2_dsk.cpp
// a2_13sect_format) and the 116480-byte format detection.
//
// Core test: no ImGui, no OpenGL.

#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "TestTempPath.h"

namespace {

void writeFile(const std::string& p, const std::vector<uint8_t>& b)
{
    std::ofstream f(p, std::ios::binary);
    assert(f.good());
    f.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
    assert(f.good());
}

std::vector<uint8_t> readFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    f.seekg(0, std::ios::end);
    const size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

}  // namespace

int main()
{
    constexpr int kImg = DiskImage::kBytesPerImage13;   // 116480

    // Deterministic, high-entropy pattern so the 5-and-3 bit-shuffling
    // (low-3 triples spread across bytes j*5+3 / j*5+4, high-5 split) is
    // fully exercised across all byte values and all 13 sectors / track.
    std::vector<uint8_t> orig(kImg);
    for (int i = 0; i < kImg; ++i)
        orig[i] = static_cast<uint8_t>((i * 197u + (i >> 8) * 13u + 0x5A) & 0xFF);

    const std::string p = pom2test::tempPath("pom2_d13_roundtrip.d13");
    writeFile(p, orig);

    DiskImage img;
    img.setWriteBackEnabled(true);
    assert(img.loadFile(p));
    assert(img.is13Sector());
    assert(img.sectorsPerTrack() == 13);

    // Dirty every track WITHOUT changing its nibbles (flip + restore), so
    // saveDirty re-decodes the pristine 5-and-3 stream for all 35 tracks —
    // a decode bug on any track would overwrite that track's sectors with
    // wrong data and fail the byte-compare below.
    for (int t = 0; t < DiskImage::kTracks; ++t) {
        const uint8_t n0 = img.nibbleAt(t, 0);
        img.writeNibbleAt(t, 0, static_cast<uint8_t>(n0 ^ 0xFF));
        img.writeNibbleAt(t, 0, n0);
    }
    assert(img.saveDirty());

    const auto rt = readFile(p);
    assert(rt.size() == orig.size());
    assert(std::memcmp(rt.data(), orig.data(), orig.size()) == 0);   // round-trip

    // Format detection: a ragged (non-13×256-multiple) file is refused.
    {
        const std::string bad = pom2test::tempPath("pom2_d13_bad.d13");
        writeFile(bad, std::vector<uint8_t>(kImg + 7, 0));
        DiskImage b;
        assert(!b.loadFile(bad));
    }
    // A 16-sector image must NOT be flagged 13-sector.
    {
        const std::string p16 = pom2test::tempPath("pom2_d13_16.dsk");
        writeFile(p16, std::vector<uint8_t>(DiskImage::kBytesPerImage, 0));
        DiskImage s;
        assert(s.loadFile(p16));
        assert(!s.is13Sector());
        assert(s.sectorsPerTrack() == 16);
    }

    std::printf("d13 round-trip smoke: OK\n");
    return 0;
}
