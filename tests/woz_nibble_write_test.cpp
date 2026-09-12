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

// A nibble write must not destroy a WOZ quarter-track (bug hunt 12).
//
// A WOZ's surface is `bitStream[qt]` (backed by `wozRaw`), not the 6656-byte
// nibble buffer — writes to it go through writeFlux. `writeNibbleAt` took
// the non-WOZ branch anyway: it dirtied a buffer saveDirty's WOZ branch
// never reads, and its `invalidateWholeTrack` cleared `bitStream[track*4]`
// — which `expandTrackBits` refuses to rebuild for a WOZ (it is the
// authoritative store, there is nothing to rebuild it from). The
// quarter-track was left with ZERO bit cells and zero flux events: the track
// vanished from the medium for the rest of the session, and the LSS fell
// through to read-amplifier noise on it.
//
// Reachable through the legacy 32-cycle nibble gate (`legacyAdvance`) and
// through the public API. Pure DiskImage unit test, synthetic WOZ1.

#include "DiskImage.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void putU32LE(std::vector<uint8_t>& o, uint32_t v)
{
    o.push_back(static_cast<uint8_t>(v & 0xFF));
    o.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    o.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    o.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

std::vector<uint8_t> buildWoz1(const std::vector<uint8_t>& bitData,
                               uint16_t bitCount)
{
    std::vector<uint8_t> woz{'W', 'O', 'Z', '1', 0xFF, 0x0A, 0x0D, 0x0A};
    putU32LE(woz, 0);   // CRC32 = 0 → "not computed", loader skips the check
    const auto addChunk = [&](const char* id, const std::vector<uint8_t>& p) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(p.size()));
        woz.insert(woz.end(), p.begin(), p.end());
    };
    std::vector<uint8_t> info(60, 0);
    info[0] = 1; info[1] = 1; info[2] = 0;
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    addChunk("INFO", info);
    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0; tmap[1] = 0;
    addChunk("TMAP", tmap);
    std::vector<uint8_t> trks(6656, 0);
    const size_t cb = std::min<size_t>(bitData.size(), 6646);
    std::memcpy(trks.data(), bitData.data(), cb);
    trks[6646] = static_cast<uint8_t>(cb & 0xFF);
    trks[6647] = static_cast<uint8_t>((cb >> 8) & 0xFF);
    trks[6648] = static_cast<uint8_t>(bitCount & 0xFF);
    trks[6649] = static_cast<uint8_t>((bitCount >> 8) & 0xFF);
    trks[6650] = 0xFF; trks[6651] = 0xFF;   // splice_point = none
    addChunk("TRKS", trks);
    return woz;
}

}  // namespace

int main()
{
    std::vector<uint8_t> bitData(6646);
    for (size_t i = 0; i < bitData.size(); ++i)
        bitData[i] = (i & 1) ? 0xAA : 0xD5;

    const fs::path path =
        fs::temp_directory_path() / "pom2_woz_nibble_write.woz";
    {
        const auto woz = buildWoz1(bitData, static_cast<uint16_t>(6646 * 8));
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(woz.data()),
                static_cast<std::streamsize>(woz.size()));
    }

    DiskImage img;
    img.setWriteBackEnabled(true);
    if (!img.loadFile(path.string())) {
        std::printf("FAIL: load WOZ1: %s\n", img.getLastError().c_str());
        return 1;
    }
    if (!img.isWoz()) { std::printf("FAIL: not recognised as WOZ\n"); return 1; }

    const int    bitsBefore = img.trackBitLength(0);
    const int    perBefore  = img.trackPeriod(0);
    const size_t fluxBefore = img.fluxEvents(0).size();
    if (bitsBefore <= 0 || fluxBefore == 0) {
        std::printf("FAIL: rig produced an empty quarter-track (bits=%d "
                    "flux=%zu)\n", bitsBefore, fluxBefore);
        return 1;
    }

    img.writeNibbleAt(0, 100, 0x96);

    const int    bitsAfter = img.trackBitLength(0);
    const int    perAfter  = img.trackPeriod(0);
    const size_t fluxAfter = img.fluxEvents(0).size();

    int rc = 0;
    if (bitsAfter != bitsBefore || perAfter != perBefore ||
        fluxAfter != fluxBefore) {
        std::printf("FAIL: writeNibbleAt destroyed the WOZ quarter-track — "
                    "bit cells %d -> %d, period %d -> %d, flux events "
                    "%zu -> %zu\n",
                    bitsBefore, bitsAfter, perBefore, perAfter,
                    fluxBefore, fluxAfter);
        rc = 1;
    }
    if (img.hasUnsavedChanges()) {
        std::printf("FAIL: writeNibbleAt dirtied a WOZ through the nibble "
                    "buffer — saveDirty's WOZ branch cannot see it\n");
        rc = 1;
    }
    if (rc == 0) {
        std::printf("[ OK ] a nibble write leaves a WOZ quarter-track intact "
                    "(%d bit cells, %zu flux events)\n",
                    bitsAfter, fluxAfter);
        std::printf("PASS: WOZ nibble-write guard\n");
    }
    std::error_code ec;
    fs::remove(path, ec);
    return rc;
}
