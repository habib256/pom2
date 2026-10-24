// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// STORAGE-012: a write to a quarter-track the WOZ does not carry.
//
// Applesauce leaves a half-used disk's empty tracks out of TMAP ($FF). The
// WOZ branch of DiskImage::writeFlux used to `return` on such a track: no
// dirty flag, no error, and INIT / FORMAT failed its verify, so the disk
// could never be reformatted. Real media takes the write, and MAME creates
// the track on save. writeFlux now adds a TRKS slot (allocateWozTrack).
//
// Pinned here, for WOZ2 and WOZ1: the write lands, survives saveDirty and a
// reload (TMAP points at the new slot, the old track is untouched), a chunk
// after TRKS keeps its bytes, and a physically write-protected image is
// still never mutated.

#include "DiskImage.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

void putU32(std::vector<uint8_t>& o, uint32_t v)
{
    for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
void putU16(std::vector<uint8_t>& o, uint16_t v)
{
    o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8));
}
void addChunk(std::vector<uint8_t>& woz, const char* id, const std::vector<uint8_t>& p)
{
    woz.insert(woz.end(), id, id + 4);
    putU32(woz, uint32_t(p.size()));
    woz.insert(woz.end(), p.begin(), p.end());
}

const std::vector<uint8_t> kMeta = {'t','i','t','l','e','\t','P','O','M','2','\n'};
constexpr uint32_t kBits = 200;

std::vector<uint8_t> header(const char* magic)
{
    std::vector<uint8_t> woz(magic, magic + 4);
    woz.insert(woz.end(), {0xFF, 0x0A, 0x0D, 0x0A});
    putU32(woz, 0);                                   // CRC unchecked
    return woz;
}

std::vector<uint8_t> info(uint8_t version, bool writeProtected)
{
    std::vector<uint8_t> i(60, 0);
    i[0] = version; i[1] = 1; i[2] = writeProtected ? 1 : 0;
    for (int k = 5; k < 37; ++k) i[k] = ' ';
    if (version >= 2) { i[37] = 1; i[39] = 32; i[44] = 1; }  // largest_track 1 block
    return i;
}

// WOZ2: track 0 only (one 512-byte block at block 3), META after TRKS.
std::vector<uint8_t> buildWoz2(bool writeProtected)
{
    auto woz = header("WOZ2");
    addChunk(woz, "INFO", info(2, writeProtected));
    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;
    addChunk(woz, "TMAP", tmap);
    std::vector<uint8_t> trks;
    putU16(trks, 3); putU16(trks, 1); putU32(trks, kBits);
    trks.resize(160 * 8, 0);
    std::vector<uint8_t> block(512, 0);
    block[0] = 0x80;                                  // one flux event, cell 0
    trks.insert(trks.end(), block.begin(), block.end());
    addChunk(woz, "TRKS", trks);
    assert(woz.size() == 2048);                       // TRKS ends on a block
    addChunk(woz, "META", kMeta);
    return woz;
}

// WOZ1: one 6656-byte slot for track 0, META after TRKS.
std::vector<uint8_t> buildWoz1()
{
    auto woz = header("WOZ1");
    addChunk(woz, "INFO", info(1, false));
    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;
    addChunk(woz, "TMAP", tmap);
    std::vector<uint8_t> slot(6656, 0);
    slot[0] = 0x80;
    slot[6646] = uint8_t((kBits + 7) / 8);
    slot[6648] = uint8_t(kBits);
    slot[6650] = 0xFF; slot[6651] = 0xFF;
    addChunk(woz, "TRKS", slot);
    addChunk(woz, "META", kMeta);
    return woz;
}

std::string writeTemp(const std::vector<uint8_t>& bytes, const std::string& tag)
{
    const auto p = fs::temp_directory_path() / ("pom2_woz_unmapped_" + tag + ".woz");
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return p.string();
}

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

void checkFormat(const char* name, const std::vector<uint8_t>& woz)
{
    std::printf("-- %s\n", name);
    const auto path = writeTemp(woz, name);
    constexpr int kQt = 8;                            // track 2: TMAP $FF
    const int cyc = 8;
    {
        DiskImage img;
        expect(img.loadFile(path), "load");
        img.setWriteBackEnabled(true);
        expect(img.trackPeriod(kQt) == 0, "track 2 starts absent");
        const int64_t ts = 100 * cyc + cyc / 2;
        int64_t transitions[1] = { ts };
        img.writeFlux(kQt, 0, int64_t(kBits) * cyc, 1, transitions);
        expect(img.trackPeriod(kQt) == int(kBits) * cyc,
               "the new track takes the image's track length");
        const auto& flux = img.fluxEvents(kQt);
        expect(flux.size() == 1 && flux[0] == ts, "the write landed on track 2");
        expect(img.hasUnsavedChanges(), "the write marks the image dirty");
        expect(img.saveDirty(), "saveDirty");
    }
    DiskImage back;
    expect(back.loadFile(path), "reload");
    const auto& flux = back.fluxEvents(kQt);
    expect(flux.size() == 1 && flux[0] == 100 * cyc + cyc / 2,
           "track 2 survives save + reload");
    const auto& t0 = back.fluxEvents(0);
    expect(back.trackPeriod(0) == int(kBits) * cyc && t0.size() == 1 && t0[0] == cyc / 2,
           "track 0 untouched");
    expect(back.trackPeriod(kQt + 4) == 0, "track 3 still absent");
    const auto bytes = readFile(path);
    const auto it = std::search(bytes.begin(), bytes.end(), kMeta.begin(), kMeta.end());
    expect(it != bytes.end() && std::equal(it - 8, it - 4, "META"),
           "the chunk after TRKS kept its bytes");
}

void checkWriteProtected()
{
    std::printf("-- write-protected WOZ2\n");
    const auto path = writeTemp(buildWoz2(true), "wp");
    const auto before = readFile(path);
    DiskImage img;
    expect(img.loadFile(path), "load wp");
    img.setWriteBackEnabled(true);
    int64_t transitions[1] = { 804 };
    img.writeFlux(8, 0, int64_t(kBits) * 8, 1, transitions);
    expect(img.trackPeriod(8) == 0, "a protected disk gains no track");
    expect(!img.hasUnsavedChanges(), "a protected disk stays clean");
    expect(readFile(path) == before, "a protected file is untouched");
}

}  // namespace

int main()
{
    checkFormat("woz2", buildWoz2(false));
    checkFormat("woz1", buildWoz1());
    checkWriteProtected();
    std::printf("woz_unmapped_track_write: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
