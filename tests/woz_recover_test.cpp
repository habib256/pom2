// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// woz_recover — WozRecover.h: a damaged WOZ becomes a healthy one, and the
// report says what can no longer be trusted.
//
// The sound image is made by POM2's OWN writer (DiskImage reads a patterned
// .dsk and `buildWozFromSurface` lays it down), so the sector checks here
// are run against an encoder that shares no table and no line with the
// module under test. And "healthy" is not the module's opinion of itself:
// every recovered file is loaded by DiskImage, which must find nothing to
// warn about.

#include "DiskImage.h"
#include "WozRecover.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pom2;

namespace {
int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

fs::path workDir()
{
    static const fs::path dir = fs::temp_directory_path() / "pom2_woz_recover";
    fs::create_directories(dir);
    return dir;
}

std::string writeFile(const char* name, const std::vector<uint8_t>& bytes)
{
    const fs::path p = workDir() / name;
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return p.string();
}

/// A WOZ made by POM2's writer from a sector image whose every sector is
/// distinct. `sectors` 16 or 13.
std::vector<uint8_t> soundWoz(int sectors)
{
    std::vector<uint8_t> image(static_cast<std::size_t>(35) * sectors * 256);
    for (std::size_t i = 0; i < image.size(); ++i)
        image[i] = static_cast<uint8_t>((i * 31u) ^ (i >> 8) ^ (i >> 16));
    const std::string path = writeFile(sectors == 16 ? "source.dsk" : "source.d13", image);
    auto img = std::make_unique<DiskImage>();              // 242 KB: not on the stack
    if (!img->loadFile(path)) {
        std::printf("  (cannot mount the %d-sector source: %s)\n", sectors,
                    img->getLastError().c_str());
        return {};
    }
    std::vector<DiskImage::TrackBuffer> surface(DiskImage::kTracks);
    for (int t = 0; t < DiskImage::kTracks; ++t)
        for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i)
            surface[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)] = img->nibbleAt(t, i);
    return DiskImage::buildWozFromSurface(surface, sectors == 13);
}

/// What DiskImage says of a file: "" = loads with nothing to warn about.
std::string verdict(const std::vector<uint8_t>& bytes, const char* name)
{
    const std::string path = writeFile(name, bytes);
    auto img = std::make_unique<DiskImage>();
    if (!img->loadFile(path)) return "REFUSED: " + img->getLastError();
    return img->integrityWarning();
}

uint16_t rd16(const std::vector<uint8_t>& f, std::size_t at) { return static_cast<uint16_t>(f[at] | (f[at + 1] << 8)); }

const woz::TrackReport* track(const woz::Report& r, int whole)
{
    for (const auto& t : r.tracks) if (t.firstQuarterTrack == whole * 4) return &t;
    return nullptr;
}
}  // namespace

int main()
{
    const std::vector<uint8_t> sound = soundWoz(16);
    if (sound.empty()) { std::printf("woz_recover: no source image\n"); return 1; }
    CHECK(verdict(sound, "sound.woz").empty());
    const std::size_t kTrks = 256;                           // fixed position
    const auto blockOf = [&](int t) { return static_cast<std::size_t>(rd16(sound, kTrks + static_cast<std::size_t>(t) * 8)) * 512; };

    // [1] A sound image: nothing to repair, every sector of every track
    //     verifies — against an encoder this module has never seen.
    {
        const woz::Report r = woz::inspect(sound);
        CHECK(r.isWoz && r.version == 2 && r.diskType == 1);
        CHECK(!r.crcWasWrong && r.crcStated == r.crcComputed && r.crcStated != 0);
        CHECK(r.repairs.empty() && r.clean());
        CHECK(r.tracks.size() == 35 && r.tracksNotVerifying() == 0 && r.tracksDropped() == 0);
        for (const auto& t : r.tracks) {
            CHECK(t.sectorsGood == 0xFFFF && t.sectorsBad == 0 && !t.thirteenSector);
            CHECK(t.addressFields >= 16 && !t.wrongTrackNumber);
        }
        std::vector<uint8_t> out;
        woz::Report rr;
        std::string error;
        CHECK(woz::recover(sound, out, rr, error) && out == sound);   // a no-op, byte for byte
        CHECK(woz::formatReport(r).find("35 track(s) verify in full, 0 do not, 0 unmapped") !=
              std::string::npos);
    }

    // [2] Bit rot inside track 17: the CRC is wrong, POM2 mounts the file
    //     read-only — and after recovery it loads clean, with the report
    //     naming track 17 and no other.
    {
        std::vector<uint8_t> rot = sound;
        for (std::size_t i = 0; i < 500; ++i) rot[blockOf(17) + 1500 + i] ^= 0x5A;
        CHECK(!verdict(rot, "rot.woz").empty());
        std::vector<uint8_t> out;
        woz::Report r;
        std::string error;
        CHECK(woz::recover(rot, out, r, error));
        CHECK(r.crcWasWrong && !r.clean());
        CHECK(verdict(out, "rot_recovered.woz").empty());
        CHECK(r.tracksNotVerifying() == 1 && r.tracksDropped() == 0);
        const woz::TrackReport* t17 = track(r, 17);
        CHECK(t17 && !t17->verifies() && (t17->sectorsGood != 0xFFFF));
        for (const auto& t : r.tracks) if (t.firstQuarterTrack != 68) CHECK(t.verifies());
        // Nothing but the CRC changed: the damaged bits are reported, not "fixed".
        CHECK(out.size() == rot.size() &&
              std::memcmp(out.data() + 12, rot.data() + 12, rot.size() - 12) == 0);
        const std::string text = woz::formatReport(r);
        CHECK(text.find("track 17:") != std::string::npos);
        CHECK(text.find("track 16:") == std::string::npos);
        CHECK(text.find("34 track(s) verify in full, 1 do not") != std::string::npos);
    }

    // [3] A file cut short inside track 30: padded back, the tracks whose
    //     bits are gone are unmapped, every earlier track is bit-identical.
    {
        std::vector<uint8_t> cut(sound.begin(), sound.begin() + static_cast<std::ptrdiff_t>(blockOf(30) + 700));
        CHECK(!verdict(cut, "cut.woz").empty());
        std::vector<uint8_t> out;
        woz::Report r;
        std::string error;
        CHECK(woz::recover(cut, out, r, error));
        CHECK(verdict(out, "cut_recovered.woz").empty());
        CHECK(out.size() == sound.size());
        CHECK(r.tracksDropped() == 5);                        // 30..34
        for (int t = 0; t < 35; ++t) {
            const bool gone = t >= 30;
            CHECK(out[88 + static_cast<std::size_t>(t) * 4] == (gone ? 0xFF : t));
            const woz::TrackReport* tr = track(r, t);
            CHECK(tr != nullptr);
            if (!tr) continue;
            CHECK(tr->kept == !gone);
            if (!gone) CHECK(tr->verifies());
        }
        CHECK(std::memcmp(out.data() + blockOf(0), sound.data() + blockOf(0),
                          blockOf(30) - blockOf(0)) == 0);
        CHECK(woz::formatReport(r).find("5 unmapped") != std::string::npos);
    }

    // [4] One record pointing outside the file: that track goes, alone.
    {
        std::vector<uint8_t> bad = sound;
        bad[kTrks + 5 * 8] = 0xFF;
        bad[kTrks + 5 * 8 + 1] = 0xFF;
        CHECK(!verdict(bad, "record.woz").empty());
        std::vector<uint8_t> out;
        woz::Report r;
        std::string error;
        CHECK(woz::recover(bad, out, r, error));
        CHECK(verdict(out, "record_recovered.woz").empty());
        CHECK(r.tracksDropped() == 1 && track(r, 5) && !track(r, 5)->kept);
        CHECK(out[88 + 20] == 0xFF && out[88 + 16] == 4 && out[88 + 24] == 6);
        CHECK(r.tracksNotVerifying() == 0);
    }

    // [5] The chunk chain broken (INFO's length field): the reference fixes
    //     where the three primary chunks are, and they are found there.
    {
        std::vector<uint8_t> chain = sound;
        chain[16] = 0x13; chain[17] = 0x37; chain[18] = 0x00; chain[19] = 0x40;
        std::vector<uint8_t> out;
        woz::Report r;
        std::string error;
        CHECK(woz::recover(chain, out, r, error));
        CHECK(!r.repairs.empty() && r.repairs[0].find("fixed positions") != std::string::npos);
        CHECK(verdict(out, "chain_recovered.woz").empty());
        CHECK(r.tracksNotVerifying() == 0 && r.tracks.size() == 35);
    }

    // [6] No CRC stated (0 is legal): it is not invented when nothing else
    //     needed repair — the file comes back as it went in.
    {
        std::vector<uint8_t> none = sound;
        std::memset(none.data() + 8, 0, 4);
        std::vector<uint8_t> out;
        woz::Report r;
        std::string error;
        CHECK(woz::recover(none, out, r, error) && out == none && r.clean());
    }

    // [7] Not a WOZ, and hostile bytes under a WOZ signature: an error or a
    //     bounded answer, never a read outside the buffer.
    {
        std::vector<uint8_t> out = { 1, 2, 3 };
        woz::Report r;
        std::string error;
        CHECK(!woz::recover({ 'A', 'B', 'C' }, out, r, error) && !error.empty());
        CHECK(out.size() == 3);                               // untouched on failure
        CHECK(!woz::inspect({}).isWoz);
        uint32_t seed = 12345;
        for (int round = 0; round < 60; ++round) {
            std::vector<uint8_t> f = sound;
            for (int k = 0; k < 40; ++k) {
                seed = seed * 1664525u + 1013904223u;
                const std::size_t at = 12 + (seed >> 8) % 1600;   // header, INFO, TMAP, records
                seed = seed * 1664525u + 1013904223u;
                f[at] = static_cast<uint8_t>(seed >> 16);
            }
            if (round % 3 == 0) f.resize(1536 + (seed >> 4) % 60000);
            std::vector<uint8_t> healed;
            woz::Report rr;
            std::string e;
            if (woz::recover(f, healed, rr, e)) {
                // Recovering what was just recovered changes nothing more.
                std::vector<uint8_t> again;
                woz::Report r2;
                CHECK(woz::recover(healed, again, r2, e) && again == healed);
            }
            (void)woz::formatReport(woz::inspect(f));
        }
        std::vector<uint8_t> tiny = { 'W', 'O', 'Z', '2', 0xFF, 0x0A, 0x0D, 0x0A, 0, 0, 0, 0 };
        CHECK(!woz::recover(tiny, out, r, error));
    }

    // [8] A 13-sector disk is recognised as one and verifies as one.
    {
        const std::vector<uint8_t> dos32 = soundWoz(13);
        if (dos32.empty()) {
            std::printf("  SKIP: no 13-sector source\n");
        } else {
            const woz::Report r = woz::inspect(dos32);
            CHECK(r.tracksNotVerifying() == 0 && r.tracks.size() == 35);
            for (const auto& t : r.tracks)
                CHECK(t.thirteenSector && t.sectorsGood == 0x1FFF && t.sectorsBad == 0);
        }
    }

    // The CRC is the reference's: the standard check value.
    CHECK(woz::crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);

    std::error_code ec;
    fs::remove_all(workDir(), ec);
    if (failures) { std::printf("woz_recover: %d check(s) failed\n", failures); return 1; }
    std::printf("woz_recover: OK\n");
    return 0;
}
