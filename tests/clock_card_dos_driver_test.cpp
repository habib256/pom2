// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// CARDS-005: the ThunderClock+ EPROM's driver, run by guest code.
//
// `rom_path_taken` proves the Thunderware U9 dump is the slot ROM and
// `clock_card_smoke` pins its bus contract; neither runs the firmware the
// way a program does. This boots DOS 3.3 with the card in slot 4 and runs
// the manual's Applesoft read — `IN#4`, then `INPUT "%";T$`, the prompt
// character choosing the time format — against a fixed clock, and checks
// the string that comes back is that time, "05/09 14;37;42.000".
// Skips (77) without apple2.rom / disk2.rom / diskii_p6.rom / the DOS 3.3
// master / the ThunderClock+ dump.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "ClockCard.h"

#include <cstdio>
#include <ctime>
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

std::tm fixedTime()
{
    std::tm t{};
    t.tm_year = 126; t.tm_mon = 4; t.tm_mday = 9;      // 2026-05-09, Saturday
    t.tm_hour = 14;  t.tm_min = 37; t.tm_sec = 42; t.tm_wday = 6;
    return t;
}

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
        std::printf("clock_card_dos_driver SKIP: missing ROM or disk\n");
        return 77;
    }
    auto clock = ClockCard::makeForTest(4, &fixedTime);
    if (!clock->romFromDump()) {
        std::printf("clock_card_dos_driver SKIP: no ThunderClock+ dump\n");
        return 77;
    }
    const fs::path master = fs::temp_directory_path() / "pom2_clock_dos_master.dsk";
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
    mem.slotBus().plug(4, std::move(clock));

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
    const char* prog =
        "NEW\r"
        "10 D$ = CHR$(4)\r"
        "20 PRINT D$;\"IN#4\"\r"
        "30 INPUT \"%\";T$\r"
        "40 PRINT D$;\"IN#0\"\r"
        "50 PRINT \"T=\";T$\r"
        "60 PRINT 1234+6\r"
        "RUN\r";
    mem.pasteText(prog, std::strlen(prog));
    if (!waitForText(cpu, ram, "1240", 400'000'000)) {
        std::fprintf(stderr, "FAIL: the program never finished\nPC=$%04X\n%s\n",
                     cpu.getProgramCounter(), scrapeTextPage(ram).c_str());
        return 3;
    }
    const std::string screen = scrapeTextPage(ram);
    size_t at = screen.find("\nT=");                // the output, not the listing
    if (at != std::string::npos) ++at;
    const std::string line = at == std::string::npos ? "" : screen.substr(at, screen.find('\n', at) - at);
    std::printf("%s\n", line.c_str());
    // The EPROM's own format: "05/09 14;37;42.000" (semicolons, so an
    // INPUT never splits it).
    if (line.find("05/09 14;37;42") == std::string::npos) {
        std::fprintf(stderr, "FAIL: the driver's time string is not 05/09 14;37;42\n%s\n",
                     screen.c_str());
        return 4;
    }
    fs::remove(master, ec);
    std::printf("clock_card_dos_driver OK\n");
    return 0;
}
