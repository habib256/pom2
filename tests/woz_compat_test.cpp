// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// woz_compat — what a WOZ says about its machine (WozCompat.h), the profile
// each bit stands for, and the fields as DiskImage reads them from a file.

#include "DiskImage.h"
#include "SystemProfile.h"
#include "WozCompat.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

uint32_t crc32(const uint8_t* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/// The smallest WOZ2 the loader takes: INFO, TMAP, TRKS with one track.
/// `infoVersion` 2 carries the two fields; 1 does not.
std::vector<uint8_t> tinyWoz(uint8_t infoVersion, uint16_t hardware, uint16_t ramK)
{
    std::vector<uint8_t> f = { 'W', 'O', 'Z', '2', 0xFF, 0x0A, 0x0D, 0x0A, 0, 0, 0, 0 };
    auto chunk = [&](const char* id, const std::vector<uint8_t>& body) {
        f.insert(f.end(), id, id + 4);
        const uint32_t n = static_cast<uint32_t>(body.size());
        for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
        f.insert(f.end(), body.begin(), body.end());
    };
    std::vector<uint8_t> info(60, 0);
    info[0]  = infoVersion;
    info[1]  = 1;                               // 5.25"
    std::memcpy(info.data() + 5, "POM2 test                       ", 32);
    info[37] = 1;                               // sides
    info[39] = 32;                              // 4 µs
    info[40] = static_cast<uint8_t>(hardware);
    info[41] = static_cast<uint8_t>(hardware >> 8);
    info[42] = static_cast<uint8_t>(ramK);
    info[43] = static_cast<uint8_t>(ramK >> 8);
    chunk("INFO", info);
    // One track, so the loader has something to mount: track 0 in 13 blocks
    // starting at block 3 (byte 1536, where the 160 records end), 51 200 bits.
    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;
    chunk("TMAP", tmap);
    std::vector<uint8_t> trks(160 * 8 + 13 * 512, 0xFF);
    std::memset(trks.data(), 0, 160 * 8);
    trks[0] = 3;                                // starting block
    trks[2] = 13;                               // block count
    trks[4] = 0x00; trks[5] = 0xC8;             // bit count 51 200, little-endian
    chunk("TRKS", trks);
    const uint32_t crc = crc32(f.data() + 12, f.size() - 12);
    for (int i = 0; i < 4; ++i) f[8 + i] = static_cast<uint8_t>(crc >> (8 * i));
    return f;
}
}  // namespace

int main()
{
    using namespace pom2::woz;

    // The list, in the reference's order; undefined bits are not guessed at.
    CHECK(hardwareList(0).empty());
    CHECK(hardwareList(kAppleIIeEnhanced) == "Apple //e Enhanced");
    CHECK(hardwareList(kAppleIIc | kAppleII) == "Apple ][, Apple //c");
    CHECK(hardwareList(0x8000).empty());
    CHECK(hardwareList(0x01FF).find("Apple /// Plus") != std::string::npos);

    // Nothing to say: the image names no machine, or names this one.
    CHECK(machineWarning(0, 0, kAppleIIPlus, "Apple ][+", 64).empty());
    CHECK(machineWarning(kAppleIIPlus | kAppleIIe, 48, kAppleIIPlus, "Apple ][+", 64).empty());
    CHECK(machineWarning(0x8000, 0, kAppleIIPlus, "Apple ][+", 64).empty());   // only unknown bits

    // Another machine: both are named.
    const std::string w = machineWarning(kAppleIIeEnhanced | kAppleIIc, 0, kAppleIIPlus,
                                         "Apple ][+", 64);
    CHECK(w.find("Apple //c, Apple //e Enhanced") != std::string::npos);   // the reference's order
    CHECK(w.find("Apple ][+") != std::string::npos);

    // RAM, on its own and with the machine; a machine RAM of 0 skips it.
    CHECK(machineWarning(kAppleIIe, 128, kAppleIIe, "Apple //e", 64).find("128K") !=
          std::string::npos);
    CHECK(machineWarning(kAppleIIe, 128, kAppleIIe, "Apple //e", 128).empty());
    CHECK(machineWarning(kAppleIIe, 128, kAppleIIe, "Apple //e", 0).empty());
    const std::string both = machineWarning(kAppleIIgs, 256, kAppleIIc, "Apple //c", 128);
    CHECK(both.find("Apple IIgs") != std::string::npos && both.find("256K") != std::string::npos);

    // Every profile has one bit, a PAL profile its sibling's, and the bits
    // are the reference's.
    CHECK(wozHardwareBit(SystemProfile::AppleII) == kAppleII);
    CHECK(wozHardwareBit(SystemProfile::AppleIIPlus) == kAppleIIPlus);
    CHECK(wozHardwareBit(SystemProfile::AppleIIeUnenhanced) == kAppleIIe);
    CHECK(wozHardwareBit(SystemProfile::AppleIIeUnenhancedPAL) == kAppleIIe);
    CHECK(wozHardwareBit(SystemProfile::AppleIIe) == kAppleIIeEnhanced);
    CHECK(wozHardwareBit(SystemProfile::AppleIIePAL) == kAppleIIeEnhanced);
    CHECK(wozHardwareBit(SystemProfile::AppleIIc) == kAppleIIc);
    CHECK(wozHardwareBit(SystemProfile::AppleIIcPAL) == kAppleIIc);
    CHECK(wozHardwareBit(SystemProfile::AppleIIcPlus) == kAppleIIcPlus);

    // As read from a file: INFO v2 carries the fields, v1 does not, and a
    // second load forgets the first image's.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pom2_woz_compat";
    fs::create_directories(dir);
    auto write = [&](const char* name, const std::vector<uint8_t>& bytes) {
        const fs::path p = dir / name;
        std::ofstream(p, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return p.string();
    };
    const std::string marked = write("marked.woz", tinyWoz(2, kAppleIIeEnhanced | kAppleIIc, 128));
    const std::string silent = write("silent.woz", tinyWoz(2, 0, 0));
    const std::string old    = write("old.woz", tinyWoz(1, 0x1234, 0x5678));
    {
        auto img = std::make_unique<DiskImage>();          // 242 KB: not on the stack
        CHECK(img->loadFile(marked));
        CHECK(img->integrityWarning().empty());
        CHECK(img->wozCompatibleHardware() == (kAppleIIeEnhanced | kAppleIIc));
        CHECK(img->wozRequiredRamK() == 128);
        CHECK(img->loadFile(silent));
        CHECK(img->wozCompatibleHardware() == 0 && img->wozRequiredRamK() == 0);
        CHECK(img->loadFile(marked));
        CHECK(img->loadFile(old));
        CHECK(img->wozCompatibleHardware() == 0 && img->wozRequiredRamK() == 0);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);

    if (failures) { std::printf("woz_compat: %d check(s) failed\n", failures); return 1; }
    std::printf("woz_compat: OK\n");
    return 0;
}
