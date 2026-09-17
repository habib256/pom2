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
//   4. back to NTSC restores the constants.

#include "CpuClock.h"
#include "EmulationController.h"
#include "LironCard.h"
#include "Sony35Drive.h"

#include <cstdint>
#include <cstdio>
#include <memory>

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

    if (failures) { std::printf("device_standard_clock: %d failure(s)\n", failures); return 1; }
    std::printf("device_standard_clock OK\n");
    return 0;
}
