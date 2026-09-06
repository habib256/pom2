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

// DROL (Brøderbund 1983) page-flip probe — diagnostic, not a pinned test.
//
// DROL is the canonical floating-bus game: it vapor-locks on the video
// scanner, then page-flips $C054/$C055 for flicker-free animation. The user
// reports flicker in POM2 (IIe, NTSC and PAL alike, every render mode except
// Chat Mauve — which masks page flips by re-reading the end-of-frame display
// state). This probe boots the real Drol.dsk and answers, with data:
//   1. does DROL page-flip at all (PAGE2 events present)?
//   2. WHERE in the video frame do the flips land (true line from emuCycle,
//      not the clamped event scanline) — VBL (192..261) or visible (0..191)?
//   3. is the position stable frame-to-frame (locked) or drifting (no lock)?
//
// Interpretation: stable flips inside VBL → machine side is right and any
// visible flicker is a renderer bug; drifting / visible-band flips → the
// game never locked → floating-bus protocol bug.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

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

const char* kindName(Memory::VideoEventKind k)
{
    switch (k) {
        case Memory::VideoEventKind::TextMode:  return "TEXT";
        case Memory::VideoEventKind::MixedMode: return "MIX";
        case Memory::VideoEventKind::Page2:     return "PAGE2";
        case Memory::VideoEventKind::HiRes:     return "HIRES";
        default:                                return "other";
    }
}

}  // namespace

int main()
{
    const std::string rom  = findFirst({
        "../roms/apple2e.rom", "roms/apple2e.rom", "../../roms/apple2e.rom" });
    const std::string boot = findFirst({
        "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    const std::string dsk  = findFirst({
        "../disks_5.4/woz/Drol (1983)(Brøderbund)[48K].woz",
        "disks_5.4/woz/Drol (1983)(Brøderbund)[48K].woz",
        "../../disks_5.4/woz/Drol (1983)(Brøderbund)[48K].woz",
        "../disks_5.4/gist/Drol.dsk", "disks_5.4/gist/Drol.dsk",
        "../../disks_5.4/gist/Drol.dsk" });
    if (rom.empty() || boot.empty() || dsk.empty()) {
        std::printf("drol_probe SKIP: missing apple2e.rom / disk2.rom / Drol.dsk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(rom.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n");
        return 1;
    }
    auto card = std::make_unique<DiskIICard>();
    if (!card->loadBootRom(boot) || !card->insertDisk(dsk)) {
        std::fprintf(stderr, "Disk II setup failed\n");
        return 1;
    }
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    cpu.hardReset();
    mem.slotBus().reset();

    // Boot + attract mode. DROL auto-runs its demo after loading.
    for (int s = 0; s < 40; ++s) cpu.run(1'022'727);
    std::printf("after boot: PC=$%04X page2=%d hires=%d text=%d\n",
                cpu.getProgramCounter(),
                mem.getDisplayState().page2 ? 1 : 0,
                mem.getDisplayState().hiRes ? 1 : 0,
                mem.getDisplayState().textMode ? 1 : 0);

    // Dump the loop the CPU sits in (title screen "wait" or sync loop).
    {
        const uint16_t pc = cpu.getProgramCounter();
        const uint16_t lo = static_cast<uint16_t>(pc & 0xFFF0) - 16;
        std::printf("code @$%04X:", lo);
        for (int i = 0; i < 48; ++i)
            std::printf(" %02X", mem.data()[static_cast<uint16_t>(lo + i)]);
        std::printf("\n");
    }

    // Start a game: DROL's title waits for a key/button.
    mem.queueKey(' ');
    for (int s = 0; s < 5; ++s) cpu.run(1'022'727);
    mem.queueKey('1');
    for (int s = 0; s < 10; ++s) cpu.run(1'022'727);
    std::printf("after keys: PC=$%04X page2=%d hires=%d text=%d\n",
                cpu.getProgramCounter(),
                mem.getDisplayState().page2 ? 1 : 0,
                mem.getDisplayState().hiRes ? 1 : 0,
                mem.getDisplayState().textMode ? 1 : 0);

    // Sample 120 consecutive NTSC video frames (65×262 = 17030 cycles).
    constexpr uint64_t F = 65ull * 262ull;
    int flips = 0, inVbl = 0, inVisible = 0;
    int minLine = 9999, maxLine = -1;
    for (int f = 0; f < 120; ++f) {
        mem.beginVideoEventFrame();
        const uint64_t now    = mem.getCycleCounter();
        const uint64_t target = now + (F - (now % F));
        while (mem.getCycleCounter() < target) cpu.run(50);
        auto evs = mem.takeVideoEvents();
        for (const auto& e : evs) {
            const int line = static_cast<int>((e.emuCycle % F) / 65);
            const int hpos = static_cast<int>(e.emuCycle % 65);
            if (f < 12 || e.kind == Memory::VideoEventKind::Page2)
                std::printf("frame %3d  %-5s=%d  line %3d  hpos %2d\n",
                            f, kindName(e.kind), e.value ? 1 : 0, line, hpos);
            if (e.kind == Memory::VideoEventKind::Page2) {
                ++flips;
                minLine = std::min(minLine, line);
                maxLine = std::max(maxLine, line);
                if (line >= 192) ++inVbl; else ++inVisible;
            }
        }
    }
    std::printf("\nsummary: %d PAGE2 flips / 120 frames — %d in VBL, %d in "
                "visible band, line range [%d..%d]\n",
                flips, inVbl, inVisible, minLine, maxLine);
    std::printf("end: PC=$%04X\n", cpu.getProgramCounter());

    // Where is DROL's beam-sync code? Scan RAM for absolute $C0xx accesses
    // (opcodes AD/BD/B9 = LDA abs[,X/,Y], 2C = BIT abs, 8D = STA abs) and
    // dump a window around each hit so the poll loop is readable.
    const uint8_t* ram = mem.data();
    std::printf("\n$C0xx absolute accesses in $0800-$BFFF:\n");
    for (uint32_t a = 0x0800; a < 0xBFFD; ++a) {
        const uint8_t op = ram[a];
        if ((op == 0xAD || op == 0xBD || op == 0xB9 || op == 0x2C ||
             op == 0x8D) && ram[a + 2] == 0xC0) {
            const uint8_t lo = ram[a + 1];
            if (lo >= 0x80) continue;   // slot I/O ($C08x disk) — not video
            std::printf("  $%04X: %02X %02X C0   ctx:", a, op, lo);
            for (int i = -8; i < 12; ++i)
                std::printf(" %02X", ram[(a + i) & 0xFFFF]);
            std::printf("\n");
        }
    }
    return 0;
}
