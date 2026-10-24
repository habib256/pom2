// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// CARDS-012: DOS 3.3 + PR#2 on the Super Serial Card's hand-assembled page.
//
// Without Apple's EPROM the SSC serves a page POM2 assembles, whose $Cn00
// entry must tell an output call from an input one. The 2026-10-06 bug hunt
// feared that under DOS 3.3 — `PR#2` stores $C200 in DOS's own hook — the
// call would arrive with CSW still naming DOS and the page would take the
// input path and spin. It does not: DOS puts the true hook back in CSW
// around the call, and this test passes on the old page too. The page now
// decides the way the real firmware does (341-0065-A, $C245-$C25C): bind
// CSW if it names $Cn00, input if KSW names the card, output otherwise.
//
// Boots the DOS 3.3 master, `PR#2`, `CATALOG`, `PR#0`, and checks the
// catalog went out on the 6551 (its last entry, README) and the machine
// came back to the prompt.
// Skips (77) without apple2.rom / disk2.rom / diskii_p6.rom / the master.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "SuperSerialCard.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string findFirst(std::initializer_list<const char*> candidates)
{
    std::error_code ec;
    for (const char* c : candidates) if (fs::is_regular_file(c, ec)) return c;
    return {};
}

std::string scrapeTextPage(const uint8_t* ram)
{
    std::string out;
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

bool waitForText(M6502& cpu, const uint8_t* ram, const char* needle,
                 long long maxCycles, long long minCycles = 0)
{
    long long total = 0;
    while (total < maxCycles) {
        total += cpu.run(8192);
        if (total >= minCycles && scrapeTextPage(ram).find(needle) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

int main()
{
    const std::string romPath  = findFirst({"../roms/apple2.rom", "roms/apple2.rom",
                                              "../../roms/apple2.rom"});
    const std::string promPath = findFirst({"../roms/disk2.rom", "roms/disk2.rom",
                                              "../../roms/disk2.rom"});
    const std::string p6Path   = findFirst({"../roms/diskii_p6.rom", "roms/diskii_p6.rom",
                                              "../../roms/diskii_p6.rom"});
    const std::string masterPath = findFirst({"../disks_5.4/dsk/dos33_master.dsk",
                                              "disks_5.4/dsk/dos33_master.dsk",
                                              "../../disks_5.4/dsk/dos33_master.dsk"});
    if (romPath.empty() || promPath.empty() || p6Path.empty() || masterPath.empty()) {
        std::printf("ssc_fallback_dos SKIP: missing ROM or disk\n");
        return 77;
    }
    // A copy: the card commits a dirty disk when it goes away.
    const fs::path master = fs::temp_directory_path() / "pom2_ssc_fallback_master.dsk";
    std::error_code ec;
    fs::copy_file(masterPath, master, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "cannot copy master\n"); return 1; }

    Memory mem;
    if (!mem.loadAppleIIRom(romPath.c_str())) { std::fprintf(stderr, "ROM load failed\n"); return 1; }
    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(promPath) || !disk->loadLssRom(p6Path) ||
        !disk->insertDisk(master.string())) {
        std::fprintf(stderr, "Disk II setup failed\n"); return 1;
    }
    mem.slotBus().plug(6, std::move(disk));
    auto ssc = std::make_unique<SuperSerialCard>(2);   // no EPROM: the hand-assembled page
    SuperSerialCard* sscRaw = ssc.get();
    if (sscRaw->firmwareLoaded()) { std::fprintf(stderr, "expected no EPROM\n"); return 1; }
    mem.slotBus().plug(2, std::move(ssc));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setProgramCounter(0xC600);
    const uint8_t* ram = mem.data();

    if (!waitForText(cpu, ram, "\n]", 250'000'000, 10'000'000)) {
        std::fprintf(stderr, "FAIL: DOS never reached the prompt\n%s\n", scrapeTextPage(ram).c_str());
        return 2;
    }
    // The master's catalog runs past one screen and DOS waits for a key
    // there: the space answers that pause. The rest waits until the
    // catalog has gone out.
    const char* print = "PR#2\rCATALOG\r ";
    mem.pasteText(print, std::strlen(print));
    for (long long n = 0; n < 400'000'000 &&
         sscRaw->recentTxText().find("README") == std::string::npos;)
        n += cpu.run(8192);
    for (long long n = 0; n < 20'000'000;) n += cpu.run(8192);
    // recentTxText is a short window: read the catalog before more goes out.
    const std::string sent = sscRaw->recentTxText();
    const char* back = "PR#0\rPRINT 1234+5\r";
    mem.pasteText(back, std::strlen(back));
    if (!waitForText(cpu, ram, "1239", 400'000'000)) {
        std::fprintf(stderr, "FAIL: PR#2 + CATALOG never came back to the prompt\n"
                     "PC=$%04X sent=\"%s\"\n%s\n", cpu.getProgramCounter(),
                     sscRaw->recentTxText().c_str(), scrapeTextPage(ram).c_str());
        return 3;
    }
    if (sent.find("README") == std::string::npos) {   // the catalog's last entry
        std::fprintf(stderr, "FAIL: the catalog did not go out on the SSC: \"%s\"\n", sent.c_str());
        return 4;
    }
    fs::remove(master, ec);
    std::printf("ssc_fallback_dos OK\n");
    return 0;
}
