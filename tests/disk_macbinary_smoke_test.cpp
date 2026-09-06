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

// Pins MacBinary 128-byte wrapper stripping in DiskImage::detectFormat.
//
// MacBinary headers come from legacy Mac archives — when an Apple II disk
// image was downloaded via a Mac and re-uploaded the way it was on disk,
// the resulting file has 128 bytes of metadata in front of the actual
// image. AppleWin recognises and strips this transparently; we now do
// the same. The predicate (from AppleWin's DiskImageHelper.cpp) is four
// independent constraints (byte 0 = 0, name length in [1..63], name
// terminator at the expected position, reserved bytes at 122/123 = 0)
// so random images almost never trigger it.
//
// Positive case: build a real MacBinary header + a known DOS 3.3 image
// payload, write to disk, load via loadFile, verify the inner format
// was correctly detected and the payload decodes.
//
// Negative case: a 143488-byte file (would-be MacBinary-wrapped DOS by
// size) but with byte 0 ≠ 0 must NOT trigger the strip, leaving the
// payload size unrecognised and the load refused with a clear error.

#include "DiskImage.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool loadPayload(std::vector<uint8_t>& out)
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
        const auto sz = static_cast<std::size_t>(f.tellg());
        if (sz != 143360) continue;
        f.seekg(0, std::ios::beg);
        out.resize(sz);
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(sz));
        return static_cast<bool>(f);
    }
    return false;
}

bool runPositive(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> wrapped(128 + payload.size(), 0);
    // Byte 0 already 0 (old version marker).
    wrapped[1] = 8;           // name length
    static const char name[8] = {'D','O','S','3','3','.','D','K'};
    std::memcpy(wrapped.data() + 2, name, 8);
    // wrapped[1 + 8 + 1] = wrapped[10] terminator already zero.
    // wrapped[122] / [123] already zero (reserved).
    std::memcpy(wrapped.data() + 128, payload.data(), payload.size());

    const std::string path = "macbinary_positive.dsk";
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) {
        std::fprintf(stderr, "positive: cannot create temp file\n");
        return false;
    }
    wf.write(reinterpret_cast<const char*>(wrapped.data()),
             static_cast<std::streamsize>(wrapped.size()));
    wf.close();

    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "positive: loadFile failed: %s\n",
                     img.getLastError().c_str());
        return false;
    }
    if (img.getSectorOrder() != DiskImage::SectorOrder::Dos33) {
        std::fprintf(stderr,
            "positive: wrong sector order after strip "
            "(payload is dos33_master.dsk → DOS expected)\n");
        return false;
    }
    return true;
}

bool runNegative(const std::vector<uint8_t>& payload)
{
    // Same size as a MacBinary-wrapped DOS image but with byte 0 != 0,
    // so the MacBinary predicate must fail. The remaining 143488 bytes
    // match no known format → loadFile must refuse with a clear error.
    std::vector<uint8_t> blob(128 + payload.size(), 0);
    blob[0] = 0xFF;  // breaks MacBinary predicate (byte 0 must be 0)
    blob[1] = 8;
    std::memcpy(blob.data() + 128, payload.data(), payload.size());

    const std::string path = "macbinary_negative.dsk";
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) {
        std::fprintf(stderr, "negative: cannot create temp file\n");
        return false;
    }
    wf.write(reinterpret_cast<const char*>(blob.data()),
             static_cast<std::streamsize>(blob.size()));
    wf.close();

    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (ok) {
        std::fprintf(stderr,
            "negative: loadFile accepted a non-MacBinary 143488-byte blob "
            "(should have refused — predicate must require byte 0 == 0)\n");
        return false;
    }
    const std::string& err = img.getLastError();
    if (err.find("doesn't match any supported format") == std::string::npos) {
        std::fprintf(stderr,
            "negative: lastError missing expected message; got: %s\n",
            err.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    std::vector<uint8_t> payload;
    if (!loadPayload(payload)) {
        std::fprintf(stderr,
            "skip: no dos33_master.dsk fixture available under disks_5.4/ or "
            "disks2/ — this test needs a known-good DOS image to wrap\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    bool ok = true;
    ok &= runPositive(payload);
    ok &= runNegative(payload);
    if (!ok) return 1;
    std::printf("disk_macbinary_smoke OK: positive + negative\n");
    return 0;
}
