// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// diskii_boot13 — POM2's own 13-sector boot PROM (DiskIIBootRom13.h), with no
// Apple dump anywhere.
//
//   A. A synthesized .d13 whose sector 0 decodes to a boot sector of known
//      bytes, booted on a stand-in ROM (WAIT is an RTS): the page at $0300
//      must hold them ($03FF is boot1's to finish), and the code at $0301 must
//      run with Y = $99 and X = $33, the registers Battalion Commander's boot1
//      reads. No media, no ROM file. The boot decode does NOT put the file's
//      bytes in order (DOS 3.2's RWTS and the boot PROM join the 5-and-3
//      values differently), so the sector is built by inverting it. With
//      Apple's 341-0009 present, its page and registers must match.
//   B. With the test media: Apple's DOS 3.2 System Master (DOS32STD.d13) on
//      a ][+ booted through the own PROM only, to DOS's own greeting — the
//      "LANGUAGE NOT AVAILABLE" its Integer BASIC HELLO gets on a ][+, as
//      dos32_boot_trace gets with Apple's 341-0009. Skipped without media.
//   C. Apple's dump, when present, still wins (loadBootRom13).

#include "DiskIIBootRom13.h"
#include "DiskIICard.h"
#include "DiskIIRoms.h"
#include "M6502.h"
#include "Memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

std::string findFirst(std::initializer_list<const char*> candidates)
{
    std::error_code ec;
    for (const char* c : candidates)
        if (fs::is_regular_file(c, ec)) return c;
    return {};
}

std::string scrapeText(const uint8_t* ram)
{
    std::string s;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x400 + (row % 8) * 0x80 + (row / 8) * 0x28;
        for (int c = 0; c < 40; ++c) s.push_back(static_cast<char>(ram[base + c] & 0x7F));
        s.push_back('\n');
    }
    return s;
}

// The 5-and-3 stream a .d13 sector becomes ([C] DOS 3.2 RWTS layout, as
// DiskImage::writeDataField13 writes it): 154 secondary values, then 256
// primary values, before the XOR chain and the translate table.
std::vector<uint8_t> streamOf(const uint8_t* src)
{
    std::vector<uint8_t> v;
    v.push_back(static_cast<uint8_t>(src[255] & 7));
    for (int k = 2; k >= 0; --k)
        for (int j = 0; j < 51; ++j)
            v.push_back(static_cast<uint8_t>(((src[j * 5 + k] & 7) << 2) |
                                             (((src[j * 5 + 3] >> (2 - k)) & 1) << 1) |
                                             ((src[j * 5 + 4] >> (2 - k)) & 1)));
    for (int k = 0; k < 5; ++k)
        for (int j = 50; j >= 0; --j) v.push_back(static_cast<uint8_t>(src[j * 5 + k] >> 3));
    v.push_back(static_cast<uint8_t>(src[255] >> 3));
    return v;
}

// What the boot PROM leaves at $0300 for that stream: the contract in
// DiskIIBootRom13.h (secondary $0899 down, primary up, three passes of 51).
std::array<uint8_t, 256> bootPageOf(const std::vector<uint8_t>& v)
{
    std::array<uint8_t, 0x9A> sec{};
    std::array<uint8_t, 256> page{};
    for (int i = 0; i < 0x9A; ++i) sec[0x99 - i] = v[i];
    for (int i = 0; i < 256; ++i) page[i] = v[0x9A + i];
    for (int y = 0, x = 0; y < 0x99; ++y, x = (x + 1) % 51) {
        uint8_t a = sec[y];
        page[0xCC + x] = static_cast<uint8_t>(page[0xCC + x] << 1 | (a & 1)); a >>= 1;
        page[0x99 + x] = static_cast<uint8_t>(page[0x99 + x] << 1 | (a & 1)); a >>= 1;
        page[y] = static_cast<uint8_t>(page[y] << 3 | a);
    }
    return page;
}

// The decode is a permutation of bits (the 3 low bits of the sector's last
// byte are dropped), so a wanted page $0300-$03FE has exactly one sector.
std::array<uint8_t, 256> sectorFor(const std::array<uint8_t, 256>& want)
{
    std::array<uint8_t, 256> src{};
    for (int fb = 0; fb < 256 * 8; ++fb) {
        std::array<uint8_t, 256> unit{};
        unit[fb / 8] = static_cast<uint8_t>(1 << (fb % 8));
        const auto page = bootPageOf(streamOf(unit.data()));
        for (int mb = 0; mb < 255 * 8; ++mb)
            if ((page[mb / 8] >> (mb % 8)) & 1)
                if ((want[mb / 8] >> (mb % 8)) & 1) src[fb / 8] |= static_cast<uint8_t>(1 << (fb % 8));
    }
    return src;
}

struct BootResult {
    bool ran = false;
    uint8_t y = 0, x = 0, slot16 = 0;
    std::array<uint8_t, 256> page{};
    std::array<uint8_t, 0x56> table{};   // $08AA-$08FF, the translate table
    uint8_t zp[6] = {};                   // $26 $27 $2A $2B $3C $3D
};

// Boots the .d13 at `path` on a stand-in ROM (WAIT is an RTS, reset enters
// the slot-6 PROM) or on `systemRom`: POM2's page, or Apple's 341-0009 when
// `apple13` names it. Apple's page needs the real Monitor around it.
BootResult bootSynthetic(const fs::path& path, const std::string& p6, const std::string& apple13,
                         const std::string& systemRom = {})
{
    Memory mem;
    if (systemRom.empty()) {
        std::vector<uint8_t> rom(0x3000, 0x00);
        rom[0xFCA8 - 0xD000] = 0x60;
        rom[0xFFFC - 0xD000] = 0x00;
        rom[0xFFFD - 0xD000] = 0xC6;
        mem.loadRomBytes(rom.data(), rom.size(), 0xD000);
    } else {
        mem.loadAppleIIRom(systemRom.c_str());
    }
    auto card = std::make_unique<DiskIICard>();
    if (!p6.empty()) card->loadLssRom(p6);
    if (!apple13.empty()) card->loadBootRom13(apple13);
    card->insertDisk(path.string());
    mem.slotBus().plug(6, std::move(card));
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.slotBus().reset();
    cpu.hardReset();
    const uint8_t* ram = mem.data();
    for (long spent = 0; spent < 20'000'000 && ram[0x2002] != 0xA5; spent += cpu.run(100'000)) {}
    BootResult r;
    r.ran = ram[0x2002] == 0xA5;
    r.y = ram[0x2000];
    r.x = ram[0x2001];
    r.slot16 = ram[0x2B];
    std::copy(ram + 0x300, ram + 0x400, r.page.begin());
    std::copy(ram + 0x8AA, ram + 0x900, r.table.begin());
    const int zpAt[6] = {0x26, 0x27, 0x2A, 0x2B, 0x3C, 0x3D};
    for (int i = 0; i < 6; ++i) r.zp[i] = ram[zpAt[i]];
    return r;
}

void testSyntheticBootSector()
{
    std::printf("\n-- A: a synthesized boot sector through the own PROM --\n");
    std::array<uint8_t, 256> want{};
    const uint8_t code[] = {
        0x99,               // $0300 the boot sector's mark
        0x8C, 0x00, 0x20,   // $0301 STY $2000
        0x8E, 0x01, 0x20,   // $0304 STX $2001
        0xA9, 0xA5,         // $0307 LDA #$A5
        0x8D, 0x02, 0x20,   // $0309 STA $2002
        0x4C, 0x0C, 0x03,   // $030C JMP *
    };
    std::copy(std::begin(code), std::end(code), want.begin());
    for (int i = static_cast<int>(sizeof(code)); i < 255; ++i) want[i] = static_cast<uint8_t>(i * 7 + 3);
    const auto sector0 = sectorFor(want);
    const auto model = bootPageOf(streamOf(sector0.data()));
    check(std::equal(want.begin(), want.begin() + 255, model.begin()), "the inverted sector decodes to the wanted page");

    std::vector<uint8_t> d13(35 * 13 * 256, 0x00);
    std::copy(sector0.begin(), sector0.end(), d13.begin());
    const fs::path path = fs::temp_directory_path() / "pom2_boot13.d13";
    { std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(d13.data()), static_cast<std::streamsize>(d13.size())); }

    const std::string p6 = findFirst({"roms/diskii_p6.rom", "../roms/diskii_p6.rom"});
    const std::string apple13 = findFirst({"roms/disk2_13.rom", "../roms/disk2_13.rom"});
    const std::string plusRom = findFirst({"roms/apple2p.rom", "../roms/apple2p.rom"});
    for (const bool withP6 : {false, true}) {
        if (withP6 && p6.empty()) { std::printf("  SKIP Apple's P6: roms/diskii_p6.rom absent\n"); continue; }
        std::printf("  %s:\n", withP6 ? "Apple's P6 sequencer" : "POM2's own sequencer, no ROM at all");
        const BootResult r = bootSynthetic(path, withP6 ? p6 : std::string(), {});
        check(r.ran, "the boot sector's code at $0301 ran");
        check(r.y == 0x99 && r.x == 0x33, "with Y = $99 and X = $33");
        check(std::equal(want.begin(), want.begin() + 255, r.page.begin()), "$0300-$03FE hold the decoded sector");
        check(r.slot16 == 0x60, "$2B holds slot*16");
        if (!apple13.empty() && !plusRom.empty()) {
            const BootResult own = bootSynthetic(path, withP6 ? p6 : std::string(), {}, plusRom);
            const BootResult ref = bootSynthetic(path, withP6 ? p6 : std::string(), apple13, plusRom);
            check(ref.ran && own.ran && ref.y == own.y && ref.x == own.x &&
                  std::equal(ref.page.begin(), ref.page.begin() + 255, own.page.begin()),
                  "on a ][+, Apple's 341-0009 leaves the same page and registers");
            check(ref.table == own.table, "and the same translate table at $08AA-$08FF");
            const char* zpName[6] = {"$26", "$27", "$2A", "$2B", "$3C", "$3D"};
            bool zpSame = true;
            for (int i = 0; i < 6; ++i)
                if (ref.zp[i] != own.zp[i]) {
                    zpSame = false;
                    std::printf("    %s: Apple %02X, POM2 %02X\n", zpName[i], ref.zp[i], own.zp[i]);
                }
            check(zpSame, "and the same $26/$27/$2A/$2B/$3C/$3D");
        }
    }
    std::error_code ec;
    fs::remove(path, ec);
}

void testDos32Master()
{
    std::printf("\n-- B: DOS 3.2's System Master through the own PROM --\n");
    const std::string romPath = findFirst({"roms/apple2p.rom", "../roms/apple2p.rom"});
    const std::string d13 = findFirst({"disks_5.4/dsk/DOS32STD.d13", "../disks_5.4/dsk/DOS32STD.d13"});
    if (romPath.empty() || d13.empty()) {
        std::printf("  SKIP: needs roms/apple2p.rom and the test media's DOS32STD.d13\n");
        return;
    }
    Memory mem;
    check(mem.loadAppleIIRom(romPath.c_str()) != 0, "the ][+ ROM loads");
    auto card = std::make_unique<DiskIICard>();
    // Apple's P5 for 16-sector disks only: the 13-sector page stays POM2's.
    pom2test::loadDiskIIDefaults(*card);
    check(card->insertDisk(d13), "DOS32STD.d13 mounts");
    DiskIICard* raw = card.get();
    mem.slotBus().plug(6, std::move(card));
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    cpu.hardReset();
    mem.slotBus().reset();
    int maxHalfTrack = 0;
    for (int i = 0; i < 80; ++i) {
        cpu.run(1'000'000);
        maxHalfTrack = std::max(maxHalfTrack, raw->getHalfTrack());
    }
    const std::string screen = scrapeText(mem.data());
    check(screen.find("LANGUAGE NOT AVAILABLE") != std::string::npos,
          "DOS 3.2 loaded and ran its greeting");
    check(maxHalfTrack >= 6, "the whole DOS image came off the disk");
}

void testAppleDumpWins()
{
    std::printf("\n-- C: Apple's 341-0009 is preferred when present --\n");
    const std::string boot13 = findFirst({"roms/disk2_13.rom", "../roms/disk2_13.rom"});
    if (boot13.empty()) {
        std::printf("  SKIP: roms/disk2_13.rom absent\n");
        return;
    }
    DiskIICard card;
    check(card.loadBootRom13(boot13), "the dump loads");
    std::ifstream f(boot13, std::ios::binary);
    std::vector<uint8_t> dump{std::istreambuf_iterator<char>(f), {}};
    check(dump.size() >= 256 && !std::equal(dump.begin(), dump.begin() + 256,
                                            pom2::buildDiskIIBootRom13(6).rom.begin()),
          "it is not POM2's page (so the test means something)");
}

} // namespace

int main()
{
    std::printf("== Disk II: POM2's own 13-sector boot PROM ==\n");
    const auto build = pom2::buildDiskIIBootRom13(6);
    check(build.ok, build.ok ? "the page assembles" : build.error.c_str());
    testSyntheticBootSector();
    testDos32Master();
    testAppleDumpWins();
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
