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

// A 3.5" platter turns at a real-time RPM — TODO.md G5-3.
//
// `Sony35Drive` converted its five RPMs to CPU cycles with the compile-time
// NTSC clock, so on both PAL profiles every revolution was 0.7 % too long.
// `EmulationController::setVideoStandard` now hands the drives the STANDARD's
// clock (`setStandardClock`), on-board and on a Liron card alike, and a Slot
// Config re-plug does the same. Pinned here:
//   1. NTSC reproduces the historical per-zone constants exactly;
//   2. PAL gives `1 015 625 × 60 / RPM` in every zone, on both on-board drives
//      and on a Liron's mechanisms;
//   3. an accelerated budget (the //c+ at 4×) moves the emulated CPU clock but
//      NOT the drives — the IWM counts crystal ticks, which no accelerator
//      speeds up, and a 4× revolution would be unreadable;
//   4. back to NTSC restores the constants;
//   5. a Disk II's medium learns the same clock: a WOZ FLUX track counts
//      125 ns ticks and the sequencer half bus cycles, so the ratio between
//      them is the machine's (DiskImage.h, fluxBusHz_) — for a disk already
//      in the drive and for one inserted afterwards, and never an
//      accelerator's.

#include "CpuClock.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "LironCard.h"
#include "Sony35Drive.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

constexpr int kRpm[5] = { 394, 429, 472, 525, 590 };

// Strobe drive register `reg` the way the IWM does (LSTRB rising edge).
void strobe(pom2::Sony35Drive& d, uint8_t reg)
{
    d.seekPhaseW(static_cast<uint8_t>(reg & 0x07), 0);
    d.seekPhaseW(static_cast<uint8_t>((reg & 0x07) | 0x08), 0);
}

void stepTo(pom2::Sony35Drive& d, int track)
{
    strobe(d, d.track() < track ? 0x0 : 0x4);   // DirNext (out) / DirPrev (in)
    for (int guard = 0; d.track() != track && guard < 100; ++guard)
        strobe(d, 0x1);                          // StepOn
}

/// cyclesPerRev() for every zone, by stepping the head to a track in each.
bool zonesMatch(pom2::Sony35Drive& d, int64_t clock)
{
    for (int z = 0; z < 5; ++z) {
        stepTo(d, z * 16);
        if (d.track() != z * 16) {
            std::printf("  could not step to track %d (at %d)\n", z * 16, d.track());
            return false;
        }
        if (d.cyclesPerRev() != clock * 60 / kRpm[z]) {
            std::printf("  zone %d: %lld cycles, want %lld\n", z,
                        static_cast<long long>(d.cyclesPerRev()),
                        static_cast<long long>(clock * 60 / kRpm[z]));
            return false;
        }
    }
    return true;
}

}  // namespace

// A WOZ 2.1 with one FLUX track of 1600 ticks (200 us): the smallest file
// that makes `trackPeriod` depend on the tick-to-cycle ratio.
std::string writeFluxWoz()
{
    std::vector<uint8_t> woz(2048 + 8 + 160, 0);
    auto put = [&](size_t at, const char* id, uint32_t len) {
        for (int i = 0; i < 4; ++i) woz[at + i] = static_cast<uint8_t>(id[i]);
        for (int i = 0; i < 4; ++i) woz[at + 4 + i] = static_cast<uint8_t>(len >> (8 * i));
    };
    const uint8_t magic[8] = {'W','O','Z','2', 0xFF, 0x0A, 0x0D, 0x0A};
    for (int i = 0; i < 8; ++i) woz[i] = magic[i];         // CRC 0 = not checked
    put(12, "INFO", 60);
    woz[20] = 3; woz[21] = 1; woz[57] = 1; woz[59] = 32;   // v3, 5.25", 1 side, 4 us
    woz[66] = 4; woz[68] = 1;                              // FLUX at block 4
    put(80, "TMAP", 160);
    for (int i = 0; i < 160; ++i) woz[88 + i] = 0xFF;
    put(248, "TRKS", 1280 + 512);
    woz[256] = 3; woz[258] = 1; woz[260] = 9;              // block 3, 9 deltas
    const uint8_t deltas[9] = {33, 33, 31, 255, 255, 255, 255, 255, 228};
    for (int i = 0; i < 9; ++i) woz[1536 + i] = deltas[i];
    put(2048, "FLUX", 160);
    for (int i = 0; i < 160; ++i) woz[2056 + i] = 0xFF;
    woz[2056] = 0;
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / "pom2_standard_clock_flux.woz";
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char*>(woz.data()),
               static_cast<std::streamsize>(woz.size()));
    return p.string();
}

int main()
{
    const int64_t ntsc = POM2_TIMING_NTSC.cpuClockHz;
    const int64_t pal  = POM2_TIMING_PAL.cpuClockHz;

    EmulationController controller;
    auto* liron = new pom2::LironCard(5);
    {
        auto st = controller.lockState();
        st.memory().slotBus().plug(5, std::unique_ptr<SlotPeripheral>(liron));
    }

    // 1. NTSC: the historical constants (155 745 … 103 999).
    controller.setVideoStandard(VideoStandard::NTSC);
    expect(zonesMatch(controller.sony35Internal(), ntsc), "NTSC on-board drive");
    stepTo(controller.sony35Internal(), 0);
    expect(controller.sony35Internal().cyclesPerRev() == 155745,
           "NTSC reproduces the historical outer-zone constant");

    // 2. PAL, everywhere.
    controller.setVideoStandard(VideoStandard::PAL);
    expect(zonesMatch(controller.sony35Internal(), pal), "PAL internal drive");
    expect(zonesMatch(controller.sony35External(), pal), "PAL external drive");
    for (int b = 0; b < 2; ++b) {
        auto& mech = const_cast<pom2::Sony35Drive&>(liron->drive(b));
        expect(mech.standardClockHz() == static_cast<double>(pal),
               "PAL reaches a Liron's mechanisms");
        expect(zonesMatch(mech, pal), "PAL Liron mechanism zones");
    }

    // 3. An accelerator moves the CPU clock, not the platter.
    controller.setCyclesPerFrame(4 * POM2_TIMING_PAL.cyclesPerFrame);
    controller.setVideoStandard(VideoStandard::PAL);
    expect(controller.emulatedCpuClockHz() > 4.0 * pal - 1.0,
           "the accelerated budget does raise the emulated CPU clock");
    expect(controller.sony35Internal().standardClockHz() == static_cast<double>(pal),
           "…but the drive keeps the standard's crystal clock");
    controller.setCyclesPerFrame(POM2_TIMING_PAL.cyclesPerFrame);

    // 4. And back.
    controller.setVideoStandard(VideoStandard::NTSC);
    expect(zonesMatch(controller.sony35Internal(), ntsc), "NTSC again");
    expect(zonesMatch(const_cast<pom2::Sony35Drive&>(liron->drive(0)), ntsc),
           "NTSC again on the Liron");

    // 5. The Disk II's medium. 1600 ticks = 200 us: 409.1 LSS cycles on an
    //    NTSC machine, 406.3 on a PAL one (a revolution lasts until its last
    //    tick is over, so 410 and 407) — and 400 only on a 1 MHz bus.
    {
        const std::string fluxPath = writeFluxWoz();
        auto* disk2 = new DiskIICard();
        {
            auto st = controller.lockState();
            st.memory().slotBus().plug(6, std::unique_ptr<SlotPeripheral>(disk2));
        }
        controller.setVideoStandard(VideoStandard::NTSC);
        bool inserted = false;
        {
            auto st = controller.lockState();
            inserted = disk2->insertDisk(0, fluxPath);
        }
        expect(inserted, "the FLUX test image mounts");
        expect(disk2->driveImage(0).fluxClockHz() == ntsc &&
               disk2->driveImage(0).trackPeriod(0) == 410,
               "NTSC: a mounted FLUX track runs on 176/45 ticks per LSS cycle");
        controller.setVideoStandard(VideoStandard::PAL);
        expect(disk2->driveImage(0).fluxClockHz() == pal &&
               disk2->driveImage(0).trackPeriod(0) == 407,
               "PAL reaches the disk already in the drive");
        controller.setCyclesPerFrame(4 * POM2_TIMING_PAL.cyclesPerFrame);
        controller.setVideoStandard(VideoStandard::PAL);
        expect(disk2->driveImage(0).fluxClockHz() == pal,
               "an accelerator does not move the flux time base");
        controller.setCyclesPerFrame(POM2_TIMING_PAL.cyclesPerFrame);
        controller.setVideoStandard(VideoStandard::PAL);
        {
            auto st = controller.lockState();
            inserted = disk2->insertDisk(1, fluxPath);
        }
        expect(inserted && disk2->driveImage(1).fluxClockHz() == pal &&
               disk2->driveImage(1).trackPeriod(0) == 407,
               "PAL reaches a disk inserted afterwards");
        controller.setVideoStandard(VideoStandard::NTSC);
        expect(disk2->driveImage(1).trackPeriod(0) == 410, "NTSC again on the Disk II");
        {
            auto st = controller.lockState();
            disk2->ejectDisk(0);
            disk2->ejectDisk(1);
        }
        std::error_code ec;
        std::filesystem::remove(fluxPath, ec);
    }

    if (failures) { std::printf("device_standard_clock: %d failure(s)\n", failures); return 1; }
    std::printf("device_standard_clock OK\n");
    return 0;
}
