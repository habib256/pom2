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

// arekkusu_disks — arekkusu42's self-checking video tests, run on six machines.
//
// The two disks (disks_5.4/arekkusu/, OPTIONAL local references: not in git,
// not in the test-media archive, so the test skips without them) carry
// programs that check the machine and print their own verdict:
//   * FloatBus_260213.po — FLOATBUS ("VAPOR SNAPSHOTS"): nine floating-bus
//     spot checks (HGR, HGR2, both MIXED, GR, GR2, GR MIXED, TXT, TXT2), then
//     "ALL SPOTCHECKS PASSED". It saves its snapshots to the disk, so it runs
//     on a scratch copy.
//   * VidModes_260421.po — menu choice 6, BEAMPOS: "TEST C019:ALIGNMENTS"
//     reads the floating bus at a fixed distance from the $C019 edge (the
//     //c's VBLINT on a //c) and prints PASS or FAIL.
// The expectations are the author's, written as hardware tests. BEAMPOS
// failed by exactly one cycle on every machine until 2026-11-08, which is
// what moved $C019 / VBLINT one cycle ahead (Memory.cpp, MemoryVideoLog.cpp)
// and re-anchored the switch columns (Apple2Display_Beam.cpp).
//
// Machines: //e unenhanced (NMOS) and enhanced (65C02), //c, each PAL and
// NTSC, through the public core (include/pom2/core.hpp) with Apple's Disk II
// PROMs. Exit 77 when a disk or ROM is absent (an allowed skip,
// tools/check_ctest_skips.sh).

#include <pom2/core.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using namespace pom2;

namespace {

int failures = 0;

struct Variant {
    const char* name;
    Machine     machine;
    VideoTiming timing;
    const char* rom;
};

const Variant kVariants[] = {
    {"//e unenhanced PAL",  Machine::AppleIIe,         VideoTiming::PAL,  "roms/apple2e_unenh.rom"},
    {"//e enhanced PAL",    Machine::AppleIIeEnhanced, VideoTiming::PAL,  "roms/apple2e.rom"},
    {"//c PAL",             Machine::AppleIIc,         VideoTiming::PAL,  "roms/apple2c-32Kv0.rom"},
    {"//e unenhanced NTSC", Machine::AppleIIe,         VideoTiming::NTSC, "roms/apple2e_unenh.rom"},
    {"//e enhanced NTSC",   Machine::AppleIIeEnhanced, VideoTiming::NTSC, "roms/apple2e.rom"},
    {"//c NTSC",            Machine::AppleIIc,         VideoTiming::NTSC, "roms/apple2c-32Kv0.rom"},
};

constexpr const char* kFloatBus = "disks_5.4/arekkusu/FloatBus_260213.po";
constexpr const char* kVidModes = "disks_5.4/arekkusu/VidModes_260421.po";

int cyclesPerSecond(VideoTiming t)
{
    return t == VideoTiming::PAL ? 312 * 65 * 50 : 262 * 65 * 60;
}

/// The 40-column text page as ASCII, one string per row.
std::string screenRow(Core& c, int row)
{
    uint8_t line[40];
    const uint16_t base = static_cast<uint16_t>(0x400 + 0x80 * (row & 7) + 0x28 * (row >> 3));
    c.readRam(RamBank::Main, base, line, sizeof line);
    std::string s;
    for (uint8_t v : line) {
        int x = v & 0x3F;
        if (v >= 0x80) x = v & 0x7F;
        if (x < 0x20) x += 0x40;
        s += static_cast<char>(x);
    }
    return s;
}

bool screenHas(Core& c, const char* text)
{
    for (int row = 0; row < 24; ++row)
        if (screenRow(c, row).find(text) != std::string::npos) return true;
    return false;
}

/// A fresh machine with a scratch copy of `disk` in drive 1, cold-booted.
bool boot(Core& c, const Variant& v, const fs::path& scratch)
{
    const bool iic = v.machine == Machine::AppleIIc;
    if (!c.loadRom(v.rom, iic)) return false;
    c.loadCharacterRom("roms/apple2e_char.rom");
    if (!c.attachDiskII(iic ? "" : "roms/disk2.rom", "roms/diskii_p6.rom", 6)) return false;
    if (!c.insertDisk(0, scratch.string())) return false;
    c.coldBoot();
    return true;
}

fs::path scratchCopy(const char* disk, const fs::path& dir, const char* tag)
{
    const fs::path dst = dir / (std::string(tag) + ".po");
    std::error_code ec;
    fs::copy_file(disk, dst, fs::copy_options::overwrite_existing, ec);
    fs::permissions(dst, fs::perms::owner_write, fs::perm_options::add, ec);
    return dst;
}

void runFloatBus(const Variant& v, const fs::path& dir)
{
    CoreConfig cfg; cfg.machine = v.machine; cfg.timing = v.timing;
    Core c(cfg);
    if (!boot(c, v, scratchCopy(kFloatBus, dir, "floatbus"))) {
        std::printf("FAIL %s: FLOATBUS did not boot (%s)\n", v.name, c.lastError().c_str());
        ++failures; return;
    }
    const int second = cyclesPerSecond(v.timing);
    for (int s = 0; s < 420; s += 5) {
        c.run(5 * second);
        if (screenHas(c, "ALL SPOTCHECKS PASSED")) {
            std::printf("[ OK ] %s: FLOATBUS, all spot checks passed\n", v.name);
            return;
        }
        if (screenHas(c, "FAIL")) break;
    }
    std::printf("FAIL %s: FLOATBUS did not pass\n", v.name);
    for (int row = 0; row < 24; ++row) std::printf("   |%s|\n", screenRow(c, row).c_str());
    ++failures;
}

void runBeamPos(const Variant& v, const fs::path& dir)
{
    CoreConfig cfg; cfg.machine = v.machine; cfg.timing = v.timing;
    Core c(cfg);
    if (!boot(c, v, scratchCopy(kVidModes, dir, "vidmodes"))) {
        std::printf("FAIL %s: VIDMODES did not boot (%s)\n", v.name, c.lastError().c_str());
        ++failures; return;
    }
    const int second = cyclesPerSecond(v.timing);
    c.run(20 * second);                       // ProDOS, BASIC.SYSTEM, the menu
    c.queueKey('6');                          // (6) BEAMPOS
    for (int s = 0; s < 90; s += 2) {
        c.run(2 * second);
        const std::string top = screenRow(c, 0);
        if (top.find("C019:ALIGNMENTS") == std::string::npos) continue;
        if (top.find("PASS") != std::string::npos) {
            std::printf("[ OK ] %s: BEAMPOS C019 alignments pass\n", v.name);
            return;
        }
        if (top.find("FAIL") != std::string::npos) break;
    }
    std::printf("FAIL %s: BEAMPOS did not pass\n", v.name);
    for (int row = 0; row < 4; ++row) std::printf("   |%s|\n", screenRow(c, row).c_str());
    ++failures;
}

} // namespace

int main()
{
    // The suite runs write-protected (tests/CMakeLists.txt); these programs
    // write their snapshots, and only ever to the scratch copies below.
#ifdef _WIN32
    _putenv_s("POM2_MEDIA_WRITE_DEFAULT", "");
#else
    unsetenv("POM2_MEDIA_WRITE_DEFAULT");
#endif

    // The disks are optional references: a tree without them skips, and
    // tools/check_ctest_skips.sh allows this test's skip.
    for (const char* disk : {kFloatBus, kVidModes}) {
        if (!fs::exists(disk)) {
            std::printf("SKIP arekkusu_disks: optional reference disk not present: %s\n", disk);
            return 77;
        }
    }
    for (const char* need : {"roms/apple2e_unenh.rom", "roms/apple2e.rom",
                             "roms/apple2c-32Kv0.rom", "roms/apple2e_char.rom",
                             "roms/disk2.rom", "roms/diskii_p6.rom"}) {
        if (!fs::exists(need)) {
            std::printf("SKIP arekkusu_disks: %s is absent (tools/fetch_test_media.py)\n", need);
            return 77;
        }
    }
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("pom2_arekkusu_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir, ec);

    for (const Variant& v : kVariants) {
        runFloatBus(v, dir);
        runBeamPos(v, dir);
    }
    fs::remove_all(dir, ec);

    if (failures) { std::printf("arekkusu_disks: %d FAILURE(S)\n", failures); return 1; }
    std::printf("arekkusu_disks: OK\n");
    return 0;
}
