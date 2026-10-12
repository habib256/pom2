// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// Manual integration diagnostic. Captures evidence; a non-blank picture
// is NOT proof that a game's copy protection passed. No game data bundled.
#include "Apple2Display.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

void capture(Memory& mem, Apple2Display& display, const fs::path& output, int second) {
    display.render(mem);
    std::ofstream ppm(output / ("screen-" + std::to_string(second) + ".ppm"), std::ios::binary);
    ppm << "P6\n" << display.width() << ' ' << display.height() << "\n255\n";
    for (int i = 0; i < display.width() * display.height(); ++i) {
        const uint32_t pixel = display.pixels()[i];
        const char rgb[] = {char(pixel), char(pixel >> 8), char(pixel >> 16)};
        ppm.write(rgb, 3);
    }
    require(bool(ppm), "write screen capture failed");
    std::ofstream ram(output / ("ram-" + std::to_string(second) + ".bin"), std::ios::binary);
    ram.write(reinterpret_cast<const char*>(mem.data()), 65536);
    ram.write(reinterpret_cast<const char*>(mem.auxData()), 65536);
    require(bool(ram), "write RAM capture failed");
}

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 9) {
            std::fprintf(stderr, "Usage: protected_disk_boot_probe disk.woz NEW-output-dir [seconds=60] [key-second=0] [keys=space] [cpu=cmos|nmos] [side-B.woz] [swap-second=key-second]\nRun from the source root; side B replaces drive 1, then keys are sent again.\n");
            return 2;
        }
        const fs::path disk = argv[1], output = argv[2];
        const int seconds = argc > 3 ? std::stoi(argv[3]) : 60;
        const int keySecond = argc > 4 ? std::stoi(argv[4]) : 0;
        const std::string keys = argc > 5 ? argv[5] : " ";
        const std::string model = argc > 6 ? argv[6] : "cmos";
        const int swapSecond = argc > 8 ? std::stoi(argv[8]) : keySecond;
        require(seconds > 0 && seconds <= 600 && keySecond >= 0 && keySecond <= seconds,
                "invalid duration / key time");
        require(argc <= 7 || (swapSecond > 0 && swapSecond <= seconds), "invalid side B swap time");
        require(model == "cmos" || model == "nmos", "CPU must be cmos or nmos");
        require(fs::is_regular_file(disk), "missing disk image");
        require(fs::create_directory(output), "output directory must be new");

        Memory mem;
        mem.setIIEMode(true);
        require(mem.loadAppleIIRom("roms/apple2e.rom"), "missing Apple IIe ROM");
        require(mem.loadCharRom("roms/apple2e_char.rom"), "missing Apple IIe character ROM");
        M6502 cpu(&mem);
        mem.setCpu(&cpu);
        cpu.setCpuMode(model == "cmos" ? M6502::CpuMode::CMOS : M6502::CpuMode::NMOS);
        auto card = std::make_unique<DiskIICard>();
        card->setIwmHost(false);
        card->setWriteBackEnabled(false);
        require(card->loadBootRom("roms/disk2.rom"), "missing Disk II boot ROM");
        require(card->insertDisk(disk.string()), "cannot mount disk image");
        require(card->driveImage(0).isWoz(), "original WOZ image required");
        std::set<int> lengths;
        int populated = 0;
        for (int qt = 0; qt < 160; ++qt) {
            const int length = card->driveImage(0).trackBitLength(qt);
            if (length > 0) { lengths.insert(length); ++populated; }
        }
        auto* drive = card.get();
        mem.slotBus().plug(6, std::move(card));
        cpu.hardReset();
        mem.slotBus().reset();
        Apple2Display display;
        display.setAuxMemory(mem.auxData());
        std::ofstream report(output / "timeline.csv");
        report << "second,cycles,pc,drive,qt,min_qt,max_qt,motor,text,hires,page2\n";
        std::printf("disk=%s cpu=%s populated_qt=%d distinct_lengths=%zu bit_LSS=%d\n",
                    disk.c_str(), model.c_str(), populated, lengths.size(), drive->usingBitLss());
        constexpr uint64_t clock = 1'022'727;
        int minimum = 159, maximum = 0;
        for (int second = 1; second <= seconds; ++second) {
            const auto target = uint64_t(second) * clock;
            while (mem.getCycleCounter() < target) {
                cpu.run(int(std::min(uint64_t(256), target - mem.getCycleCounter())));
                const int qt = drive->getQuarterTrack(drive->getActiveDrive());
                minimum = std::min(minimum, qt); maximum = std::max(maximum, qt);
            }
            const auto state = mem.getDisplayState();
            report << second << ',' << mem.getCycleCounter() << ',' << cpu.getProgramCounter()
                   << ',' << drive->getActiveDrive() << ',' << drive->getQuarterTrack(drive->getActiveDrive())
                   << ',' << minimum << ',' << maximum << ',' << drive->isMotorOn()
                   << ',' << state.textMode << ',' << state.hiRes << ',' << state.page2 << '\n';
            if (second % 5 == 0 || second == keySecond || second == swapSecond || second == seconds) {
                capture(mem, display, output, second);
                std::printf("t=%ds PC=$%04X qt=%d range=%d..%d motor=%d text=%d hires=%d page2=%d\n",
                    second, cpu.getProgramCounter(), drive->getQuarterTrack(drive->getActiveDrive()),
                    minimum, maximum, int(drive->isMotorOn()), state.textMode, state.hiRes, state.page2);
                std::fflush(stdout);
            }
            const bool swapNow = argc > 7 && second == swapSecond;
            if (swapNow) {
                require(drive->insertDisk(0, argv[7]), "side B mount failed");
                std::printf("t=%ds inserted side B: %s\n", second, argv[7]);
            }
            if (second == keySecond || swapNow) {
                // Paste queue preserves order and lets the guest consume
                // each byte without advancing the scheduled timeline here.
                mem.pasteText(keys);
            }
        }
        require(bool(report), "timeline write failed");
        std::printf("Diagnostic complete: inspect captures; protection success is not asserted.\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
