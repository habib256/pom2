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

// DOS 3.3 SAVE / LOAD round-trip smoke test.
//
// Boots a DOS 3.3 master into a writable copy, waits for the BASIC ready
// prompt by scanning the text page, pastes a small BASIC program, SAVEs
// it under a fresh name, then NEW + LOADs it back, and asserts both
// program lines reappear on the screen via LIST.
//
// Pinned regression: I/O ERROR on SAVE due to the LSS write-side timing
// bug that fired an extra flush of the stale prologue latch in the slot
// right after the data prologue, shifting the running-XOR checksum.
//
// Skips silently when the user hasn't placed apple2.rom + disk2.rom +
// dos33_master.dsk in the conventional locations (this isn't a CI gate,
// it's a regression check the maintainer runs after touching DiskIICard).

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string findFirst(std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) if (fileExists(c)) return c;
    return {};
}

bool copyFile(const std::string& src, const fs::path& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return static_cast<bool>(out);
}

// Apple II text page 1 layout: row Y at base $0400 + 0x80*(Y%8) + 0x28*(Y/8).
// We strip bit-7 and map control chars to ' ' so a substring search works.
std::string scrapeTextPage(const uint8_t* ram) {
    std::string out;
    out.reserve(24 * 41);
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(ram[base + col] & 0x7F);
            out.push_back((c >= 0x20 && c < 0x7F) ? c : ' ');
        }
        out.push_back('\n');
    }
    return out;
}

void runFor(M6502& cpu, int cycles) {
    int n = 0;
    while (n < cycles) n += cpu.run(1024);
}

// Run until `needle` appears in the text page or `maxCycles` elapse.
bool waitForText(M6502& cpu, const uint8_t* ram, const char* needle,
                 int maxCycles, int minCycles = 0) {
    int total = 0;
    while (total < maxCycles) {
        const int slice = cpu.run(8192);
        total += slice;
        if (total >= minCycles && scrapeTextPage(ram).find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string romPath  = findFirst({
        "../roms/apple2.rom", "roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = findFirst({
        "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    // argv[1] overrides the image — the same SAVE/LOAD round-trip is the
    // guest-side check for every 5.25" container (.dsk, .nib, .woz).
    const std::string masterPath = (argc > 1) ? std::string(argv[1]) : findFirst({
        "../disks_5.4/dsk/dos33_master.dsk", "disks_5.4/dsk/dos33_master.dsk",
        "../../disks_5.4/dsk/dos33_master.dsk",
        "../disks_5.4/dos33_master.dsk", "disks_5.4/dos33_master.dsk",
        "../../disks_5.4/dos33_master.dsk" });

    if (romPath.empty() || promPath.empty() || masterPath.empty()) {
        std::printf("dos33_save_smoke SKIP: missing ROM or disk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    fs::path scratch = fs::temp_directory_path() /
        ("pom2_dos33_save_scratch" + fs::path(masterPath).extension().string());
    if (!copyFile(masterPath, scratch)) {
        std::fprintf(stderr, "cannot copy master to %s\n", scratch.string().c_str());
        return 1;
    }

    Memory mem;
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n"); return 1;
    }

    auto card = std::make_unique<DiskIICard>();
    if (!card->loadBootRom(promPath))           { std::fprintf(stderr, "PROM load failed\n");   return 1; }
    if (!card->insertDisk(scratch.string()))    { std::fprintf(stderr, "insertDisk failed\n");  return 1; }
    card->setWriteBackEnabled(true);
    DiskIICard* cardRaw = card.get();
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setProgramCounter(0xC600);

    const uint8_t* ram = mem.data();

    // ── Boot to BASIC prompt. DOS 3.3 in this emulator takes ~30M cycles
    //    of emulated time to reach the prompt; bound generously.
    if (!waitForText(cpu, ram, "\n]", 120'000'000, /*minCycles=*/10'000'000)) {
        std::fprintf(stderr,
            "FAIL: BASIC prompt never appeared. PC=$%04X qt=%d\n%s\n",
            cpu.getProgramCounter(), cardRaw->getQuarterTrack(),
            scrapeTextPage(ram).c_str());
        return 2;
    }
    // Let the SYSTEM MASTER's HELLO autorun finish so the prompt is stable.
    runFor(cpu, 30'000'000);

    // ── Paste program + SAVE.
    {
        const char* prog =
            "NEW\r"
            "10  PRINT \"HI\"\r"
            "20  PRINT \"BYE\"\r"
            "SAVE POMTEST\r";
        mem.pasteText(prog, std::strlen(prog));
        runFor(cpu, 60'000'000);
    }

    const std::string afterSave = scrapeTextPage(ram);
    if (afterSave.find("I/O ERROR") != std::string::npos) {
        std::fprintf(stderr,
            "FAIL: SAVE returned I/O ERROR (the regression).\n"
            "  Disk state: ht=%d nib=%d writeFlushes=%llu\n"
            "  Screen:\n%s\n",
            cardRaw->getHalfTrack(), cardRaw->getTrackPosition(),
            static_cast<unsigned long long>(cardRaw->getWriteFlushCount()),
            afterSave.c_str());
        return 3;
    }
    if (afterSave.find("FILE LOCKED") != std::string::npos) {
        std::fprintf(stderr,
            "FAIL: SAVE hit FILE LOCKED — POMTEST was created by an earlier "
            "test run that didn't clean up.\n");
        return 4;
    }
    if (!cardRaw->hasUnsavedChanges()) {
        std::fprintf(stderr, "FAIL: SAVE didn't touch the disk\n");
        return 5;
    }

    std::printf("SAVE OK: %llu nibble flushes, head at half-track %d\n",
                static_cast<unsigned long long>(cardRaw->getWriteFlushCount()),
                cardRaw->getHalfTrack());

    // ── Now NEW + LOAD POMTEST + LIST, and verify both program lines
    //    reappear. NEW erases the program from RAM; LOAD reads it back
    //    from disk; LIST prints it. If LOAD hits I/O ERROR, the data we
    //    wrote was unreadable.
    {
        const char* loadProg =
            "NEW\r"
            "LOAD POMTEST\r"
            "LIST\r";
        mem.pasteText(loadProg, std::strlen(loadProg));
        runFor(cpu, 30'000'000);
    }

    const std::string afterList = scrapeTextPage(ram);
    if (afterList.find("I/O ERROR") != std::string::npos) {
        std::fprintf(stderr,
            "FAIL: LOAD returned I/O ERROR — the SAVEd file is unreadable.\n"
            "  Screen:\n%s\n", afterList.c_str());
        return 6;
    }
    if (afterList.find("PRINT \"HI\"") == std::string::npos ||
        afterList.find("PRINT \"BYE\"") == std::string::npos) {
        std::fprintf(stderr,
            "FAIL: LIST after LOAD doesn't show the saved program lines.\n"
            "  Screen:\n%s\n", afterList.c_str());
        return 7;
    }

    std::printf("LOAD OK: program lines round-tripped through disk\n");
    std::printf("dos33_save_smoke OK\n");
    return 0;
}
