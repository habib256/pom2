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

// Disk II end-to-end boot smoke test.
//
// Wires the same components MainWindow does (Memory + M6502 + SlotBus +
// DiskIICard) and runs the CPU from $C600 for long enough that the boot
// PROM can: spin the motor, recalibrate the head, find D5 AA 96 / D5 AA AD,
// decode 256 bytes into $0800, and JMP $0801. The test then compares
// $0800-$08FF against the first 256 bytes of the .dsk image.
//
// Skips silently if the host hasn't placed apple2.rom + disk2.rom +
// dos33_master.dsk in the conventional locations. This isn't a CI gate —
// it's a debug aid for the boot pipeline that the project maintainer can
// re-run after touching DiskIICard or DiskImage.

#include "DiskIICard.h"
#include "DiskImage.h"
#include "M6502.h"
#include "Memory.h"

#include <array>
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

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize(static_cast<size_t>(f.tellg()));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

}  // namespace

// Boot a single disk image and compare $0800-$08FD against the file's
// first 256 bytes (logical sector 0, which always lands on physical
// sector 0 regardless of skew). Returns 0 on success.
int bootAndVerify(const std::string& romPath, const std::string& promPath,
                  const std::string& imgPath, const char* label)
{
    Memory mem;
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "%s: loadAppleIIRom failed\n", label);
        return 1;
    }
    auto card = std::make_unique<DiskIICard>();
    if (!card->loadBootRom(promPath)) {
        std::fprintf(stderr, "%s: loadBootRom failed\n", label);
        return 1;
    }
    if (!card->insertDisk(imgPath)) {
        std::fprintf(stderr, "%s: insertDisk failed: %s\n", label,
                     card->getLastError().c_str());
        return 1;
    }
    DiskIICard* cardRaw = card.get();
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    cpu.hardReset();
    mem.slotBus().reset();
    cpu.setProgramCounter(0xC600);
    const uint8_t* ram = mem.data();

    // Stop the moment control has LEFT the boot PROM with T0S0 deposited:
    // $0800 non-zero (the PROM has written the page) and PC outside $C600-
    // $C6FF (the PROM has jumped to $0801). Only PROM code writes page $08 —
    // boot0's later sector reads land at $B600-$BFFF — so the page is stable
    // from here until boot1 has finished with it.
    //
    // The condition used to require PC INSIDE $0800-$08FF, sampled only at
    // 1024-cycle slice boundaries; boot0 spends too few cycles there to be
    // caught, so the loop always ran the full 5 M-cycle budget. That was
    // invisible while the fixture path was wrong and the test skipped — with
    // the disk found, it compared page $08 after DOS 3.3 had fully booted and
    // run HELLO, which by then has zeroed $0800-$0802: two mismatches on a
    // boot that was in fact perfect. Breaking on $0800 alone is too EARLY
    // (the PROM's denibblise loop fills the page while $0800 already reads
    // non-zero — 166 mismatches).
    constexpr int kMaxCycles = 5'000'000;
    int totalCycles = 0;
    while (totalCycles < kMaxCycles) {
        const int slice = cpu.run(1024);
        totalCycles += slice;
        const uint16_t pc = cpu.getProgramCounter();
        if (ram[0x0800] != 0x00 && (pc < 0xC600 || pc > 0xC6FF)) break;
    }

    std::array<uint8_t, 256> loaded;
    std::memcpy(loaded.data(), ram + 0x0800, 256);

    std::vector<uint8_t> diskBytes;
    if (!readFile(imgPath, diskBytes) || diskBytes.size() < 256) {
        std::fprintf(stderr, "%s: cannot re-read for compare\n", label);
        return 2;
    }

    std::printf("disk_boot_smoke[%s]: ran %d cycles, PC=$%04X, half-track %d\n",
                label, totalCycles, cpu.getProgramCounter(),
                cardRaw->getHalfTrack());

    // Compare $0801-$08FD. Skip $0800 (block-count register, modified by
    // the ProDOS boot loader during chain-loading; harmless to skip on
    // DOS 3.3 too since boot1 leaves it alone). Skip $08FE/$08FF (DOS 3.3
    // boot1 mutates them as page-shift state).
    int diffs = 0;
    int firstDiff = -1;
    for (int i = 1; i < 254; ++i) {
        if (loaded[i] != diskBytes[i]) {
            if (firstDiff < 0) firstDiff = i;
            ++diffs;
        }
    }
    if (diffs == 0) return 0;
    std::fprintf(stderr,
        "%s FAIL: %d byte mismatches; first diff at $%02X"
        " (got $%02X want $%02X)\n",
        label, diffs, firstDiff, loaded[firstDiff], diskBytes[firstDiff]);
    return 3;
}

int main()
{
    // Probe the standard locations (relative to the build dir, which is
    // where ctest cd's into).
    const std::string romPath  = findFirst({
        "../roms/apple2.rom", "roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = findFirst({
        "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    // Sector images moved to disks_5.4/dsk/ in 2026; the bare roots stay as
    // a fallback for older trees.
    const std::string dskPath  = findFirst({
        "../disks_5.4/dsk/dos33_master.dsk", "disks_5.4/dsk/dos33_master.dsk",
        "../../disks_5.4/dsk/dos33_master.dsk",
        "../disks_5.4/dos33_master.dsk", "disks_5.4/dos33_master.dsk",
        "../../disks_5.4/dos33_master.dsk" });

    if (romPath.empty() || promPath.empty() || dskPath.empty()) {
        std::printf("disk_boot_smoke SKIP: missing one of"
                    " roms/apple2.rom, roms/disk2.rom, disks_5.4/dos33_master.dsk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    if (const int r = bootAndVerify(romPath, promPath, dskPath, "DOS 3.3");
        r != 0) return r;

    // ProDOS image — only run if the user has placed it. The skew is
    // already pinned by disk_image_smoke; this is a full PROM-driven boot
    // check that the .po path round-trips through DiskIICard / GCR / the
    // boot loader.
    const std::string poPath = findFirst({
        "../disks_5.4/dsk/ProDOS_2_4_3.po", "disks_5.4/dsk/ProDOS_2_4_3.po",
        "../../disks_5.4/dsk/ProDOS_2_4_3.po",
        "../disks_5.4/ProDOS_2_4_3.po", "disks_5.4/ProDOS_2_4_3.po",
        "../../disks_5.4/ProDOS_2_4_3.po" });
    if (!poPath.empty()) {
        if (const int r = bootAndVerify(romPath, promPath, poPath, "ProDOS .po");
            r != 0) return r;
    }

    std::printf("disk_boot_smoke OK\n");
    return 0;
}
