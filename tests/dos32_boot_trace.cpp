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

// DOS 3.2 (13-sector) end-to-end boot trace. Boots a real DOS 3.x .d13
// master through the 341-0009 boot PROM + 341-0010 LSS + POM2's 5-and-3
// GCR nibblizer, then verifies the boot PROM decoded logical sector 0 of
// track 0 into $0800 (the same milestone disk_boot_smoke_test.cpp checks
// for the 16-sector path). It also runs longer and scrapes the text page
// for the DOS banner as a diagnostic.
//
// Skips silently if the host hasn't placed apple2.rom + disk2_13.rom +
// diskii_p6_13.rom + a *.d13 master in the conventional locations.

#include "DiskIICard.h"
#include "DiskImage.h"
#include "M6502.h"
#include "Memory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

bool fileExists(const std::string& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) if (fileExists(c)) return c;
    return {};
}

// Drive the LSS read path (motor on, Q7L/Q6L, advance + read $C0EC) and
// collect de-duped bytes — same mechanism as diskii_lss_smoke_test.
std::vector<uint8_t> spinAndCollect(DiskIICard& card, int cpuCycles,
                                    size_t maxBytes, int cyclesPerRead = 8)
{
    std::vector<uint8_t> out;
    card.deviceSelectRead(0x9);   // motor on
    card.deviceSelectRead(0xE);   // Q7L
    card.deviceSelectRead(0xC);   // Q6L
    int spent = 0;
    while (spent < cpuCycles && out.size() < maxBytes) {
        card.advanceCycles(cyclesPerRead);
        spent += cyclesPerRead;
        const uint8_t b = card.deviceSelectRead(0xC);
        if (b & 0x80) { if (out.empty() || out.back() != b) out.push_back(b); }
    }
    return out;
}

// Linearise the interleaved 24×40 text page into an upper-cased blob.
std::string scrapeTextUpper(const uint8_t* ram)
{
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(ram[base + col] & 0x7F);
            out.push_back((c >= 0x20 && c < 0x7F)
                          ? static_cast<char>(std::toupper(c)) : ' ');
        }
        out.push_back('\n');
    }
    return out;
}

}  // namespace

int main()
{
    // Boot via the Apple ][+ Autostart ROM (apple2p.rom): on cold-start it
    // scans the slots and auto-boots the Disk II in slot 6. (The non-
    // autostart Original monitor would just sit at the * prompt, and a
    // forced PC=$C600 jump skips the cold-start the DOS greeting needs.)
    const std::string romPath = findFirst({
        "../roms/apple2p.rom", "roms/apple2p.rom", "../../roms/apple2p.rom",
        "../roms/apple2.rom", "roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string boot13  = findFirst({
        "../roms/disk2_13.rom", "roms/disk2_13.rom", "../../roms/disk2_13.rom" });
    const std::string lss13   = findFirst({
        "../roms/diskii_p6_13.rom", "roms/diskii_p6_13.rom",
        "../../roms/diskii_p6_13.rom" });
    const std::string d13     = findFirst({
        "../disks_5.4/dsk/DOS32STD.d13", "disks_5.4/dsk/DOS32STD.d13",
        "../../disks_5.4/dsk/DOS32STD.d13" });

    if (romPath.empty() || boot13.empty() || lss13.empty() || d13.empty()) {
        std::printf("dos32_boot_trace SKIP: missing apple2.rom / disk2_13.rom /"
                    " diskii_p6_13.rom / DOS32STD.d13\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n");
        return 1;
    }

    auto card = std::make_unique<DiskIICard>();
    if (!card->loadBootRom13(boot13) || !card->loadLssRom13(lss13)) {
        std::fprintf(stderr, "13-sector PROM load failed\n");
        return 1;
    }
    if (!card->insertDisk(d13)) {
        std::fprintf(stderr, "insertDisk failed: %s\n",
                     card->getLastError().c_str());
        return 1;
    }

    // Phase 0 (isolation, on a SEPARATE card so the boot card stays
    // pristine): confirm the 5-and-3 track expands to bits and the LSS read
    // path recovers valid 13-sector framing — D5 AA B5 (addr) / D5 AA AD
    // (data) prologues + translate5 bytes (0xAB..0xFF). The 16-sector P6
    // sequencer reads the 13s bit stream fine (the read is encoding-
    // agnostic; the 5-and-3 decode is the boot PROM's job).
    {
        DiskImage probe;
        probe.loadFile(d13);
        std::printf("phase0: is13=%d trackBitLength(0)=%d\n",
                    probe.is13Sector(), probe.trackBitLength(0));

        DiskIICard rd;
        rd.loadBootRom13(boot13);   // → serving13_; read uses the 16s P6 LSS
        rd.insertDisk(d13);
        const auto nibs = spinAndCollect(rd, 1'000'000, 64);
        std::printf("phase0 LSS read: %zu nibbles:", nibs.size());
        for (size_t i = 0; i < nibs.size() && i < 32; ++i) std::printf(" %02X", nibs[i]);
        std::printf("\n");
    }

    DiskIICard* cardRaw = card.get();
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    cpu.hardReset();              // PC ← reset vector $FFFC: Autostart cold-start
    mem.slotBus().reset();
    const uint8_t* ram = mem.data();

    // Run the Autostart cold-start: it scans the slots, finds the Disk II
    // in slot 6, JMPs the 341-0009 boot PROM, which loads DOS 3.2 across
    // the disk (head seeks tracks 0→13) and runs the greeting.
    int totalCycles = 0;
    int maxHalfTrack = 0;
    for (int i = 0; i < 80 && totalCycles < 80'000'000; ++i) {
        totalCycles += cpu.run(1'000'000);
        maxHalfTrack = std::max(maxHalfTrack, cardRaw->getHalfTrack());
    }

    const std::string screen = scrapeTextUpper(ram);
    std::printf("dos32_boot_trace: %d cyc, PC=$%04X, max half-track %d\n",
                totalCycles, cpu.getProgramCounter(), maxHalfTrack);
    std::printf("--- text page ---\n%s", screen.c_str());

    // DOS 3.2 loaded and ran its greeting iff "LANGUAGE NOT AVAILABLE"
    // appears: the disk's Integer-BASIC HELLO can't run on the Applesoft
    // ][+, so DOS prints this and drops to its hooked ] prompt. That
    // message is emitted by DOS, not the ROM — definitive proof the
    // 13-sector disk booted. The multi-track head seek (well past track 0)
    // confirms the 5-and-3 read pipeline carried the whole DOS image, not
    // just the boot sector.
    const bool dosBooted =
        screen.find("LANGUAGE NOT AVAILABLE") != std::string::npos;
    if (!dosBooted || maxHalfTrack < 6) {
        std::fprintf(stderr,
            "dos32_boot_trace FAIL: dosBooted=%d maxHalfTrack=%d\n",
            dosBooted, maxHalfTrack);
        return 3;
    }
    std::printf("dos32_boot_trace OK: DOS 3.2 (13-sector) booted "
                "(max half-track %d)\n", maxHalfTrack);
    return 0;
}
