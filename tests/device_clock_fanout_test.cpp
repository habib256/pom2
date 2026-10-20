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

// The clock every emuCycles consumer is handed is the clock the MACHINE runs
// at — not the video standard's nominal. The //c+ carries a soldered 4×
// accelerator (defaultCyclesPerFrame = 68180); setVideoStandard used to hand
// its speaker, cassette, floppy sounds and every slot card 1 022 727 Hz, so
// each of them read a //c+ cycle stamp as four times the time it is (a 4 kHz
// tone at 1 kHz, four seconds of audio per second). The toolbar's speed
// buckets have the same rule: "1×" is the profile's stock speed, so a stock
// //c+ does not read as "4×" and picking 1× does not un-solder it.

#include "CpuClock.h"
#include "EmulationController.h"
#include "SystemProfile.h"
#include "TranswarpCard.h"
#include "M6502.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "ZipChip.h"

#include <memory>
#include <mutex>
#include <string>

#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    auto near = [](double a, double b) { return std::fabs(a - b) < 2.0; };
    {
        EmulationController ctrl;
        ctrl.setCyclesPerFrame(pom2VideoTiming(VideoStandard::NTSC).cyclesPerFrame);
        ctrl.setVideoStandard(VideoStandard::NTSC);
        assert(near(ctrl.emulatedCpuClockHz(), 1022727.0) && "a 1x NTSC machine keeps the nominal");
        ctrl.setCyclesPerFrame(pom2VideoTiming(VideoStandard::PAL).cyclesPerFrame);
        ctrl.setVideoStandard(VideoStandard::PAL);
        assert(near(ctrl.emulatedCpuClockHz(), 1015625.0) && "a 1x PAL machine keeps its nominal");
    }
    // The //c+: its 4 MHz is its built-in accelerator since 2026-10-21 (a
    // ZipChip its ROM sets fast at reset), on a 1x budget — and the devices
    // must be told the clock it actually burns, 4 MHz (MAME's Zip speed).
    if (const std::string rom = pom2::findResource("roms/apple2cp.rom"); !rom.empty()) {
        EmulationController ctrl;
        const auto& cp = pom2::profileConfig(pom2::SystemProfile::AppleIIcPlus);
        ctrl.setCyclesPerFrame(cp.defaultCyclesPerFrame);
        ctrl.setVideoStandard(cp.videoStandard);
        {
            auto st = ctrl.lockState();
            st.memory().setIIEMode(true);
            assert(st.memory().loadAppleIIRom(rom.c_str(), true));
            st.cpu().setCpuMode(M6502::CpuMode::CMOS);
        }
        {
            std::lock_guard<std::mutex> lk(ctrl.stateMutex());
            ctrl.installSocketAccelerator();
        }
        {
            auto st = ctrl.lockState();
            st.memory().memWrite(0x0300, 0x4C);   // JMP $0300 — no slot access
            st.memory().memWrite(0x0301, 0x00);
            st.memory().memWrite(0x0302, 0x03);
            st.cpu().setProgramCounter(0x0300);
        }
        ctrl.setMode(EmulationController::Mode::Running);
        for (int i = 0; i < 3; ++i) ctrl.tickFrame();
        assert(near(ctrl.emulatedCpuClockHz(), pom2::ZipChip::kFastHz) &&
               "the //c+ must hand its devices the 4 MHz clock it actually runs at");
        ctrl.setMode(EmulationController::Mode::Stopped);
    }
    // Bug hunt #10: a plugged TransWarp multiplied the frame's cycle budget
    // by 3.5 but every emuCycles consumer kept the stock clock — a 1 kHz tone
    // rendered at 298 Hz and two thirds of the toggles were purged. The //c+
    // defect of bug hunt #7, arriving through a slot. The clock the devices
    // are told must be the clock the frame actually burns.
    {
        EmulationController ctrl;
        ctrl.setVideoStandard(VideoStandard::NTSC);
        {
            auto st = ctrl.lockState();
            st.memory().slotBus().plug(2, std::make_unique<pom2::TranswarpCard>(2));
        }
        ctrl.setMode(EmulationController::Mode::Running);
        for (int i = 0; i < 3; ++i) ctrl.tickFrame();
        assert(near(ctrl.emulatedCpuClockHz(), 3.5 * 1022727.0) &&
               "a plugged TransWarp must hand its 3.5x clock to every device");
        assert(ctrl.scaledFrameBudget() == static_cast<int64_t>(3.5 * 17045) &&
               "the budget and the audio clock must agree");
    }

    // The toolbar's 1x bucket = the profile's stock budget, which is the
    // video standard's own cyclesPerFrame on every profile — the //c+'s 4x
    // is its accelerator's, not its budget's (68180 until 2026-10-21).
    for (const pom2::SystemProfile p : pom2::allProfiles()) {
        const auto& cfg = pom2::profileConfig(p);
        assert(cfg.defaultCyclesPerFrame == pom2VideoTiming(cfg.videoStandard).cyclesPerFrame);
    }
    std::printf("device_clock_fanout: 1x NTSC/PAL nominal, //c+ accelerator 4 MHz, toolbar buckets OK\n");
    return 0;
}
