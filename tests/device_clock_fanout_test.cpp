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

#include <memory>

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
        // The //c+: 68180 cycles per NTSC frame = 4x.
        const auto& cp = pom2::profileConfig(pom2::SystemProfile::AppleIIcPlus);
        ctrl.setCyclesPerFrame(cp.defaultCyclesPerFrame);
        ctrl.setVideoStandard(cp.videoStandard);
        assert(near(ctrl.emulatedCpuClockHz(), 4.0 * 1022727.0) &&
               "the //c+ must hand its devices the 4x clock it actually runs at");
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

    // The toolbar's 1x bucket = the profile's stock budget: 68180 on the //c+,
    // the video standard's own cyclesPerFrame everywhere else.
    for (const pom2::SystemProfile p : pom2::allProfiles()) {
        const auto& cfg = pom2::profileConfig(p);
        const int nominal = pom2VideoTiming(cfg.videoStandard).cyclesPerFrame;
        if (p == pom2::SystemProfile::AppleIIcPlus)
            assert(cfg.defaultCyclesPerFrame == 68180 && cfg.defaultCyclesPerFrame == 4 * nominal);
        else
            assert(cfg.defaultCyclesPerFrame == nominal);
    }
    std::printf("device_clock_fanout: 1x NTSC/PAL nominal, //c+ 4x, toolbar buckets OK\n");
    return 0;
}
