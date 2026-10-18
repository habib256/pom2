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

// WOZ 3.5" load test — pins `Disk35Image`'s flux path.
//
// POM2 stores 3.5" media as a flat block array and has no GCR *encoder*, so
// a `.woz` (which holds bit cells, not blocks) has nothing to be mounted as
// unless it is decoded at LOAD time. That decode is `Sony35Gcr`, shared with
// `Sony35Drive::decodeAndCommit` — the drive reaches the same cells from the
// other side, when the guest writes a track.
//
// The oracle here is a SECOND, INDEPENDENT encoder, written against MAME
// `flopimg.cpp:2017 build_mac_track_gcr` rather than reusing POM2's: a test
// that shared the decoder's tables could not catch a bad table, and the
// table is exactly the sort of thing that is transcribed wrong once and
// believed forever.
//
// Pinned:
//   1. Round trip — blocks encoded into a synthetic WOZ2 come back byte
//      for byte, on both heads and across a zone boundary (the sector count
//      per track steps 12/11/10/9/8).
//   2. A WOZ mounts WRITE-PROTECTED. Giving blocks back would mean
//      re-encoding the user's flux, which POM2 cannot do; silently
//      accepting writes and dropping them at flush is the failure this
//      forecloses.
//   3. The refusals are specific: a 5.25" WOZ says so (mount it as a Disk
//      II image) and a WOZ1 says so, rather than "not an 800K image".

#include "Disk35Image.h"
#include "Woz35Fixture.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The synthetic Sony 800K GCR encoder and the WOZ2 container writer now live
// in `Woz35Fixture.h`, shared with `storage_coordinator_test` since the
// commercial 3.5" WOZ images left the repository (TODO.md § G1). It is still a
// SECOND, INDEPENDENT encoder — see that header's note before touching it.

using pom2::test::blockIndex;
using pom2::test::buildWoz;
using pom2::test::encodeTrack;
using pom2::test::fillBlock;
using pom2::test::sectorsFor;
using pom2::test::writeTemp;

void testRoundTrip()
{
    // Track 0 (zone 0, 12 sectors) and track 16 (zone 1, 11) — a zone
    // boundary — on both heads.
    const int kTracks[] = { 0, 16 };
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<std::pair<int, std::vector<uint8_t>>> expect;

    for (int t : kTracks)
        for (int head = 0; head < 2; ++head) {
            const int n = sectorsFor(t);
            std::vector<uint8_t> payload(size_t(n) * 512);
            for (int s = 0; s < n; ++s) {
                const int bi = blockIndex(t, head, s);
                fillBlock(payload.data() + s * 512, bi);
                expect.push_back({ bi, std::vector<uint8_t>(
                    payload.begin() + s * 512, payload.begin() + (s + 1) * 512) });
            }
            tracks[t * 2 + head] = encodeTrack(t, head, payload.data());
        }

    const fs::path p = writeTemp("pom2_woz35_roundtrip.woz", buildWoz(tracks));
    pom2::Disk35Image img;
    const bool ok = img.loadFile(p.string());
    assert(ok && "a synthetic 3.5\" WOZ2 must mount");
    assert(img.kind() == pom2::Disk35Image::ImageKind::Woz35);

    for (const auto& [bi, want] : expect) {
        uint8_t got[512];
        assert(img.readBlock(uint32_t(bi), got));
        assert(std::memcmp(got, want.data(), 512) == 0 &&
               "every encoded sector must decode back byte for byte");
    }
    // Tracks the WOZ never described read as zeros rather than garbage.
    uint8_t blank[512];
    assert(img.readBlock(800, blank));
    bool allZero = true;
    for (uint8_t v : blank) if (v) { allZero = false; break; }
    assert(allZero);

    std::error_code ec; fs::remove(p, ec);
    std::printf("  ok: %zu sectors round-trip through a synthetic WOZ2\n",
                expect.size());
}

void testWriteProtected()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<uint8_t> payload(12 * 512);
    for (int s = 0; s < 12; ++s) fillBlock(payload.data() + s * 512, s);
    tracks[0] = encodeTrack(0, 0, payload.data());

    const fs::path p = writeTemp("pom2_woz35_wp.woz", buildWoz(tracks));
    pom2::Disk35Image img;
    assert(img.loadFile(p.string()));
    // Even with write-back explicitly enabled: there is no GCR encoder, so
    // handing blocks back to a .woz is not something POM2 can honour.
    img.setWriteBackEnabled(true);
    assert(img.isWriteProtected());
    uint8_t junk[512] = {};
    assert(!img.writeBlock(0, junk));
    assert(!img.hasUnsavedChanges());

    std::error_code ec; fs::remove(p, ec);
    std::printf("  ok: a WOZ mounts read-only, writes refused\n");
}

// The way OUT of the read-only WOZ above: convert to a .po. Pinned because
// the whole point is that the copy is byte-identical AND writable — a
// conversion that quietly dropped a block, or produced another read-only
// image, would look like it worked.
void testConvertToPo()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<std::pair<int, std::vector<uint8_t>>> expect;
    for (int t : { 0, 16 })
        for (int head = 0; head < 2; ++head) {
            const int n = sectorsFor(t);
            std::vector<uint8_t> payload(size_t(n) * 512);
            for (int s = 0; s < n; ++s) {
                const int bi = blockIndex(t, head, s);
                fillBlock(payload.data() + s * 512, bi);
                expect.push_back({ bi, std::vector<uint8_t>(
                    payload.begin() + s * 512, payload.begin() + (s + 1) * 512) });
            }
            tracks[t * 2 + head] = encodeTrack(t, head, payload.data());
        }

    const fs::path p = writeTemp("pom2_woz35_convert.woz", buildWoz(tracks));
    const fs::path out = fs::temp_directory_path() / "pom2_woz35_convert.po";
    std::error_code ec; fs::remove(out, ec);
    // An export's temporary must be unique to this call. A file left at the
    // old fixed name belongs to another attempt and must remain untouched.
    const fs::path legacyTmp = out.string() + ".pom2tmp";
    { std::ofstream f(legacyTmp, std::ios::binary | std::ios::trunc); f << "keep"; }

    pom2::Disk35Image woz;
    assert(woz.loadFile(p.string()));
    std::string err;
    assert(woz.exportRawTo(out.string(), err) && "export must succeed");
    {
        std::ifstream f(legacyTmp, std::ios::binary);
        assert(std::string((std::istreambuf_iterator<char>(f)), {}) == "keep" &&
               "export must not consume another attempt's fixed temp name");
    }
    assert(fs::file_size(out, ec) == pom2::Disk35Image::kBytesPerImage &&
           "a converted 3.5\" image is exactly 819200 bytes");

    // Never clobber: a second export to the same name must refuse rather
    // than replace a disk the user may have been working in.
    assert(!woz.exportRawTo(out.string(), err));
    assert(err.find("already exists") != std::string::npos);

    pom2::Disk35Image po;
    assert(po.loadFile(out.string()));
    assert(po.kind() == pom2::Disk35Image::ImageKind::Raw800k);
    for (const auto& [bi, want] : expect) {
        uint8_t got[512];
        assert(po.readBlock(uint32_t(bi), got));
        assert(std::memcmp(got, want.data(), 512) == 0 &&
               "the .po must carry the WOZ's decoded blocks unchanged");
    }
    // And, unlike its source, it takes writes once the user opts in.
    po.setWriteBackEnabled(true);
    assert(!po.isWriteProtected());
    uint8_t mark[512];
    std::memset(mark, 0xA5, sizeof(mark));
    assert(po.writeBlock(9, mark));
    assert(po.saveDirty());
    pom2::Disk35Image again;
    assert(again.loadFile(out.string()));
    uint8_t got[512];
    assert(again.readBlock(9, got));
    assert(std::memcmp(got, mark, 512) == 0 && "the write must be durable");

    fs::remove(p, ec); fs::remove(out, ec); fs::remove(legacyTmp, ec);
    std::printf("  ok: WOZ converts to an identical, writable .po\n");
}

void testRefusals()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<uint8_t> payload(12 * 512, 0);
    tracks[0] = encodeTrack(0, 0, payload.data());

    {   // 5.25" WOZ — belongs to DiskImage, and the message must say so.
        const fs::path p = writeTemp("pom2_woz35_525.woz",
                                     buildWoz(tracks, /*diskType=*/1));
        pom2::Disk35Image img;
        assert(!img.loadFile(p.string()));
        assert(img.lastError().find("5.25") != std::string::npos);
        std::error_code ec; fs::remove(p, ec);
    }
    {   // WOZ1 — a different TRKS shape, not decoded.
        const fs::path p = writeTemp("pom2_woz35_v1.woz",
                                     buildWoz(tracks, 2, /*version=*/'1'));
        pom2::Disk35Image img;
        assert(!img.loadFile(p.string()));
        assert(img.lastError().find("WOZ1") != std::string::npos);
        std::error_code ec; fs::remove(p, ec);
    }
    std::printf("  ok: 5.25\" and WOZ1 images are refused by name\n");
}

}  // namespace

int main()
{
    std::printf("woz35_load\n");
    testRoundTrip();
    testWriteProtected();
    testConvertToPo();
    testRefusals();
    std::printf("PASS\n");
    return 0;
}
