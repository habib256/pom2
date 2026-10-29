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

// diskii_pom2_boot_prom — POM2's own Disk II boot PROM (DiskIIBootRom.h).
//
// A card with no roms/disk2.rom boots with it, so it has to honour what boot
// sectors rely on, not merely load sector 0:
//   1. the page itself, for every slot: the Autostart/ProDOS signature, the
//      $CnFF Disk II id, the $Cn5C read entry, all built without error;
//   2. the nibble -> 6-bit table it builds at $02D6+nibble is exactly the 64
//      nibbles DOS writes (the rule it uses is pinned against the list);
//   3. the handover: page $08 holds sector 0 at the JMP $0801, with A = $3D
//      and X = slot*16 (ProDOS's block 0 reads both);
//   4. whole boots on the ][+ with the legacy nibble gate — no Apple file at
//      all on the controller: DOS 3.3 to its HELLO banner (boot1 calls back
//      into $Cn5C for nine sectors), ProDOS 2.4.3 to its splash (an RTS onto
//      $Cn5B for sector 2, A read on return, then $Cn63-$CnEA COPIED and
//      patched into its own sector reader — the grid DiskIIBootRom.h keeps).
//      Apple's PROM boots the same disks to the same text as the control,
//      so a screen check that could not succeed is caught;
//   5. the same DOS boot from slot 5, whose page addresses $C0DC, not $C0EC.

#include "DiskIIBootRom.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

// The 64 disk nibbles of 6-and-2, in value order (DOS 3.3's write table).
constexpr uint8_t kGcr[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE,
    0xAF, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
    0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3, 0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD,
    0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF2,
    0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

std::string findFile(const std::string& rel)
{
    for (const char* root : { "", "../", "../../" }) {
        const std::string p = root + rel;
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) return p;
    }
    std::fprintf(stderr, "missing %s — run tools/fetch_test_media.py\n", rel.c_str());
    std::exit(1);
}

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// The text page as ASCII, row by row, so a banner can be searched for.
std::string screenText(const uint8_t* ram)
{
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const uint8_t c = ram[base + col] & 0x7F;
            out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c)
                 : static_cast<char>(c + 0x40 < 0x7F ? c + 0x40 : '.');
        }
        out += '\n';
    }
    return out;
}

struct Boot {
    bool handedOver = false;
    std::array<uint8_t, 256> page08{};
    uint8_t a = 0, x = 0;
    bool bannerSeen = false;
    std::string screen;
};

// Boot `image` from `slot` on the ][+. `appleProm` empty = POM2's PROM.
Boot boot(const std::string& image, int slot, const std::string& appleProm,
          const char* banner)
{
    Boot b;
    Memory mem;
    if (!mem.loadAppleIIRom(findFile("roms/apple2p.rom").c_str())) {
        std::fprintf(stderr, "cannot load apple2p.rom\n");
        std::exit(1);
    }
    auto card = std::make_unique<DiskIICard>(slot);
    card->setWriteBackEnabled(false);
    if (!appleProm.empty() && !card->loadBootRom(appleProm)) {
        std::fprintf(stderr, "cannot load %s\n", appleProm.c_str());
        std::exit(1);
    }
    check(appleProm.empty() == card->usingPom2BootRom(),
          "usingPom2BootRom() tells which PROM the card boots with");
    if (!card->insertDisk(image)) {
        std::fprintf(stderr, "insertDisk %s: %s\n", image.c_str(),
                     card->getLastError().c_str());
        std::exit(1);
    }
    mem.slotBus().plug(slot, std::move(card));

    M6502 cpu(&mem);
    cpu.hardReset();
    mem.slotBus().reset();
    const uint16_t prom = static_cast<uint16_t>(0xC000 + slot * 0x100);
    cpu.setProgramCounter(prom);
    const uint8_t* ram = mem.data();

    // Step instructions until the PROM hands over (the first PC in page $08
    // after the PROM ran), then run on to the banner.
    long cycles = 0;
    bool inProm = false;
    while (cycles < 3'000'000 && !b.handedOver) {
        cycles += cpu.run(1);
        const uint16_t pc = cpu.getProgramCounter();
        if ((pc & 0xFF00) == prom) { inProm = true; continue; }
        if (inProm && (pc & 0xFF00) == 0x0800) {
            b.handedOver = true;
            std::memcpy(b.page08.data(), ram + 0x0800, 256);
            b.a = cpu.getAccumulator();
            b.x = cpu.getXRegister();
        }
    }
    for (long n = 0; b.handedOver && n < 400 && !b.bannerSeen; ++n) {
        for (int i = 0; i < 100'000; i += cpu.run(4096)) {}
        b.bannerSeen = screenText(ram).find(banner) != std::string::npos;
    }
    b.screen = screenText(ram);
    return b;
}

void expectBoot(const char* label, const std::string& image, int slot,
                const std::string& appleProm, const char* banner,
                const std::vector<uint8_t>& sector0, bool toBanner = true)
{
    const Boot b = boot(image, slot, appleProm, banner);
    std::printf("%s: handover=%d A=$%02X X=$%02X banner=%d\n", label,
                b.handedOver, b.a, b.x, b.bannerSeen);
    if (std::getenv("POM2_SHOW_SCREEN")) std::printf("%s", b.screen.c_str());
    check(b.handedOver, "the PROM jumps into page $08");
    if (!b.handedOver) return;
    check(std::memcmp(b.page08.data(), sector0.data(), 256) == 0,
          "page $08 holds sector 0 at the handover");
    check(b.x == slot * 16, "X = slot*16 at JMP $0801");
    check(b.a == 1, "A = $3D (one sector read) at JMP $0801");
    if (toBanner && !b.bannerSeen) {
        std::fprintf(stderr, "%s: no \"%s\" on screen:\n%s", label, banner,
                     b.screen.c_str());
        check(false, "the boot reaches its banner");
    }
}

} // namespace

int main()
{
    // 1. The page, for every slot.
    for (int slot = 1; slot <= 7; ++slot) {
        const pom2::DiskIIBootRomBuild p = pom2::buildDiskIIBootRom(slot);
        if (!p.ok) std::fprintf(stderr, "slot %d: %s\n", slot, p.error.c_str());
        check(p.ok, "the PROM assembles");
        check(p.rom[0x01] == 0x20 && p.rom[0x03] == 0x00 &&
              p.rom[0x05] == 0x03 && p.rom[0x07] == 0x3C,
              "Autostart/ProDOS signature at $Cn01/03/05/07");
        check(p.rom[0xFF] == 0x00, "$CnFF = $00 (16-sector Disk II)");
        check(p.listing.find("\nread  $05C..") != std::string::npos,
              "the read entry is $Cn5C");
    }
    {
        DiskIICard card(6);
        check(card.hasBootRom() && card.usingPom2BootRom(),
              "a card with no dump boots with POM2's PROM");
    }

    const std::string dos = findFile("disks_5.4/dsk/dos33_master.dsk");
    const std::string pro = findFile("disks_5.4/dsk/ProDOS_2_4_3.po");
    const std::vector<uint8_t> dosImg = readFile(dos), proImg = readFile(pro);
    const std::vector<uint8_t> dosS0(dosImg.begin(), dosImg.begin() + 256);
    const std::vector<uint8_t> proS0(proImg.begin(), proImg.begin() + 256);
    const std::string apple = findFile("roms/disk2.rom");

    // 2. The table the PROM built on its way through a boot.
    {
        Memory mem;
        mem.loadAppleIIRom(findFile("roms/apple2p.rom").c_str());
        auto card = std::make_unique<DiskIICard>(6);
        card->setWriteBackEnabled(false);
        card->insertDisk(dos);
        mem.slotBus().plug(6, std::move(card));
        M6502 cpu(&mem);
        cpu.hardReset();
        mem.slotBus().reset();
        cpu.setProgramCounter(0xC600);
        for (long c = 0; c < 3'000'000 && cpu.getProgramCounter() != 0x0801;)
            c += cpu.run(1);
        bool table = cpu.getProgramCounter() == 0x0801;
        for (int v = 0; v < 64; ++v)
            table = table && mem.data()[0x02D6 + kGcr[v]] == v;
        check(table, "$02D6+nibble holds the 6-bit value of all 64 nibbles");
    }

    // 3-4. Whole boots, POM2's PROM against Apple's.
    expectBoot("DOS 3.3, Apple PROM", dos, 6, apple, "DOS VERSION 3.3", dosS0);
    expectBoot("DOS 3.3, POM2 PROM ", dos, 6, {},    "DOS VERSION 3.3", dosS0);
    expectBoot("ProDOS, Apple PROM ", pro, 6, apple, "PRODOS 8 V2.4.3", proS0);
    expectBoot("ProDOS, POM2 PROM  ", pro, 6, {},    "PRODOS 8 V2.4.3", proS0);
    expectBoot("ProDOS, slot 5     ", pro, 5, {},    "PRODOS 8 V2.4.3", proS0);
    // 5. Another slot.
    expectBoot("DOS 3.3, slot 5    ", dos, 5, {},    "DOS VERSION 3.3", dosS0);

    if (failures) {
        std::fprintf(stderr, "diskii_pom2_boot_prom: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("diskii_pom2_boot_prom OK\n");
    return 0;
}
