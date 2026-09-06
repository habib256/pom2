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

// ProDOS 2.4.3 SAVE / LOAD round-trip smoke test.
//
// Parallel to dos33_save_smoke_test.cpp but driving a ProDOS .po image.
// Pins the same Disk II write-pipeline regression: if the LSS pacing
// drifts and the data field's running-XOR is off by one nibble, ProDOS
// would report DISK I/O ERROR or an UNABLE TO WRITE FILE on the SAVE.
//
// The first run is exploratory — we don't know exactly what ProDOS 2.4.3
// runs at startup (BITSY BYE? a custom launcher?). The test is written
// to fail loudly with a screen dump so the maintainer can adapt it.

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

int main() {
    const std::string romPath  = findFirst({
        "../roms/apple2.rom", "roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = findFirst({
        "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    // Sector images moved to disks_5.4/dsk/ in 2026; the bare roots stay as
    // a fallback for older trees.
    const std::string masterPath = findFirst({
        "../disks_5.4/dsk/ProDOS_2_4_3.po", "disks_5.4/dsk/ProDOS_2_4_3.po",
        "../../disks_5.4/dsk/ProDOS_2_4_3.po",
        "../disks_5.4/ProDOS_2_4_3.po", "disks_5.4/ProDOS_2_4_3.po",
        "../../disks_5.4/ProDOS_2_4_3.po" });

    if (romPath.empty() || promPath.empty() || masterPath.empty()) {
        std::printf("prodos_save_smoke SKIP: missing ROM or disk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    fs::path scratch = fs::temp_directory_path() / "pom2_prodos_save_scratch.po";
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

    // ── Boot. This particular .po image launches BITSY BYE (a ProDOS
    //    file launcher). We wait for its characteristic header line
    //    "BITSY  BYE" to appear. Boot path: PROM → ProDOS → STARTUP
    //    (BITSY.BOOT) which loads the launcher. ~50M cycles in our emu.
    constexpr int kBootTimeout = 180'000'000;
    if (!waitForText(cpu, ram, "BITSY  BYE", kBootTimeout, /*minCycles=*/10'000'000)) {
        std::fprintf(stderr, "FAIL: BITSY BYE launcher didn't appear\n");
        std::fprintf(stderr, "Screen:\n%s\n", scrapeTextPage(ram).c_str());
        return 2;
    }
    runFor(cpu, 30'000'000);

    // ── Navigate BITSY BYE to BASIC.SYSTEM. The launcher accepts Ctrl-J
    //    ($0A) to step the cursor down one entry; the file list is
    //    [".", BITSY.BOOT, QUIT.SYSTEM, BASIC.SYSTEM, …] with the cursor
    //    starting on ".", so 3 presses land on BASIC.SYSTEM. Each press
    //    needs settle time — BITSY BYE drops fast bursts. RETURN then
    //    launches BASIC.SYSTEM, which takes ~30M cycles to init the
    //    ProDOS command interpreter.
    auto pressKey = [&](char key, int waitCycles) {
        const char k[] = { key };
        mem.pasteRawKeys(k, 1);
        runFor(cpu, waitCycles);
    };
    pressKey(0x0A, 5'000'000);
    pressKey(0x0A, 5'000'000);
    pressKey(0x0A, 5'000'000);
    pressKey(0x0D, 90'000'000);

    if (scrapeTextPage(ram).find("\n]") == std::string::npos) {
        std::fprintf(stderr,
            "FAIL: BASIC prompt didn't appear after BITSY BYE → BASIC.SYSTEM.\n"
            "  Screen:\n%s\n", scrapeTextPage(ram).c_str());
        return 3;
    }

    // ── Paste program + SAVE. The Space-Trip image is nearly full, so
    //    we DELETE a non-essential file (README) first to make room for
    //    POMTEST. If the test ever runs against a different ProDOS .po,
    //    this DELETE may fail harmlessly ("FILE NOT FOUND") and SAVE
    //    will then fail with DISK FULL — caught below.
    {
        const char* prog =
            "DELETE README\r"
            "NEW\r"
            "10  PRINT \"HI\"\r"
            "20  PRINT \"BYE\"\r"
            "SAVE POMTEST\r";
        mem.pasteText(prog, std::strlen(prog));
        runFor(cpu, 90'000'000);
    }

    const std::string afterSave = scrapeTextPage(ram);

    // ProDOS error strings: "I/O ERROR", "DISK FULL", "UNABLE TO WRITE",
    // "WRITE PROTECTED", "FILE NOT FOUND", "NO BUFFERS AVAILABLE".
    auto contains = [&](const char* needle) {
        return afterSave.find(needle) != std::string::npos;
    };
    if (contains("I/O ERROR")) {
        std::fprintf(stderr,
            "FAIL: SAVE returned I/O ERROR (the regression).\n"
            "  Disk state: ht=%d nib=%d writeFlushes=%llu\n",
            cardRaw->getHalfTrack(), cardRaw->getTrackPosition(),
            static_cast<unsigned long long>(cardRaw->getWriteFlushCount()));
        return 4;
    }
    if (contains("WRITE PROTECTED")) {
        std::fprintf(stderr, "FAIL: disk reported as write protected\n");
        return 5;
    }
    if (contains("DUPLICATE FILE NAME") || contains("FILE EXISTS")) {
        std::fprintf(stderr,
            "FAIL: POMTEST already exists on the .po image — clean up "
            "the source disk or rename the test file.\n");
        return 6;
    }
    if (contains("DISK FULL")) {
        std::fprintf(stderr,
            "FAIL: SAVE got DISK FULL — the .po image doesn't have a free\n"
            "  block for POMTEST. The DELETE README step before SAVE may\n"
            "  have failed if README isn't on this image. Try a different\n"
            "  ProDOS image with at least a few free blocks.\n");
        return 7;
    }
    if (!cardRaw->hasUnsavedChanges()) {
        std::fprintf(stderr, "FAIL: SAVE didn't touch the disk\n");
        return 7;
    }

    std::printf("SAVE OK: %llu nibble flushes, head at half-track %d\n",
                static_cast<unsigned long long>(cardRaw->getWriteFlushCount()),
                cardRaw->getHalfTrack());

    // ── NEW + LOAD POMTEST + LIST and verify both program lines round-trip.
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
        return 8;
    }
    if (afterList.find("PRINT \"HI\"") == std::string::npos ||
        afterList.find("PRINT \"BYE\"") == std::string::npos) {
        std::fprintf(stderr,
            "FAIL: LIST after LOAD doesn't show the saved program lines.\n"
            "  Screen:\n%s\n", afterList.c_str());
        return 9;
    }

    std::printf("LOAD OK: program lines round-tripped through ProDOS\n");
    std::printf("prodos_save_smoke OK\n");
    return 0;
}
