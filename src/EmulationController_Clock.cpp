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

// EmulationController_Clock — the clock the machine runs at and the budget of
// each worker tick: the accelerator fan-out, the scaled budget the browser's
// RAF driver uses, and the tick cut to end on a video-frame boundary.
// Moved out of EmulationController.cpp (2026-11-08) when the last of these
// pushed it past the 2000-line cap; nothing in them changed but that one.

#include "EmulationController.h"

#include "CpuClock.h"

#include <mutex>

// Same derivation as setVideoStandard, times what the ACCELERATOR CARD
// adds. Without it a plugged TransWarp multiplied the frame's cycle budget
// but every emuCycles consumer kept the stock clock: the speaker read a 3.5x
// cycle stamp as 3.5x the time it is, so a 1 kHz tone came out at 298 Hz
// (bug hunt #7's //c+ defect, arriving through a slot). The caller passes
// the multiplier it sampled under the lock.
//
// The frame's cycle BUDGET has been chunk-granular since 2026-09-09 (the
// multiplier is re-read every 4096 cycles), while this fan-out stayed
// frame-granular — sampled once, before the frame's first chunk. The two
// then disagree for the whole of any frame in which a slow window opens or
// closes: measured on the `transwarp_chunk_sampling` fixture, a window
// opening a third of the way in burns 22 901 CPU cycles while every
// emuCycles consumer was told 3 579 544 Hz (the cycles are worth 1 374 096
// Hz — 2.61x out), and the frame in which a window CLOSES burns 49 417
// cycles while they were told 1 022 727 Hz (2.9x the other way). That is a
// frame of stamps the SpeakerDevice queue and the Mockingboard/Phasor
// replay cursor read as 2.6-2.9x the time they represent: the cursor
// starves or runs up onto the producer, trips its re-anchor and drops the
// backlog — a click per DOS disk read, which is exactly the shape of the
// //c+ and bug-hunt-#10 defects this function was written to remove.
//
// Sampling it where the chunk loop already samples the multiplier costs one
// double compare per chunk (four to fifteen per frame) and nothing at all on
// a machine with no accelerator, since `mul` is then permanently 1.0.
void EmulationController::applyAcceleratorClock(double mul)
{
    const VideoTiming& vt = pom2VideoTiming(videoStandard_.load());
    // The UI/CLI/API MAX bucket is the 1,000,000-cycle work budget. It
    // fast-forwards emulated time; interpreting it as a 60 MHz oscillator
    // makes the Disk II boot ROM miss every nibble on its fixed bus clock.
    // Custom 2x/4x CPU-frequency settings retain their existing clock model.
    const int requestedBase = baseCyclesPerFrame_.load();
    const int base = requestedBase == 1'000'000 ? vt.cyclesPerFrame : requestedBase;
    const double stockHz = static_cast<double>(vt.cpuClockHz) *
        (base > 0 ? static_cast<double>(base) / vt.cyclesPerFrame : 1.0);
    const double hz = stockHz * mul;
    // Fast path only when the BEAM agrees too: setVideoStandard and a stock
    // snapshot restore the clock, not the ratio (a //c+ → //e switch kept the
    // beam at ~1/4 speed; bug hunt 2026-11-07).
    if (hz == cpuClockHz_.load() && mem.videoClockRatio() == 1.0 / mul) return;
    cpuClockHz_.store(hz); mem.setVideoClockRatio(1.0 / mul);   // the beam keeps the crystal's time
    // The speaker stamps in stock time from THIS cycle on (setCpuClockAt):
    // its backlog then plays at the speed it was made at. The cards' AY
    // replays do the same through their bus clock (busNow_).
    if (spk)       spk->setCpuClockAt(mem.getCycleCounter(), hz, stockHz);
    if (tape)      tape->setCpuClock(hz);
    if (floppy525) floppy525->setCpuClock(stockHz);   // the Disk II stamps stock time
    if (floppy35)  floppy35->setCpuClock(hz);
    for (int slot = 0; slot < SlotBus::kSlotCount; ++slot)   // 0: the aux slot
        if (SlotPeripheral* card = mem.slotBus().peripheral(slot))
            card->setCpuClock(hz);
}

// NOT inside the __EMSCRIPTEN__ guard: tickFrame — the browser-RAF CPU
// driver on WASM — routes its chunks through here too. (2026-07-12 bug
// hunt: defining this below, worker-only, broke the wasm link.)
int64_t EmulationController::scaledFrameBudget() const
{
    const int64_t base = cyclesPerFrame.load();
    const double  mul  = mem.cpuSpeedMultiplier();
    if (mul == 1.0) return base;
    // Guard the arithmetic, not the caller: the multiplier comes from a
    // card and the base from --speed / the AI /speed endpoint, and their
    // product is the only place in the loop where either could overflow.
    const double scaled = static_cast<double>(base) * mul;
    constexpr double kCeil = 1.0e9;
    if (scaled <= 1.0)   return 1;
    if (scaled >= kCeil) return static_cast<int64_t>(kCeil);
    return static_cast<int64_t>(scaled);
}

// The worker's budget for the tick about to start: a whole number of video
// frames is cut to end ON a frame boundary (pom2AlignedTickBudget, CpuClock.h
// § cyclesPerFrame). Base cycles ARE video cycles — an accelerator changes CPU
// cycles per base cycle, never the beam's clock — so the beam position is
// the right origin whatever card is plugged.
int64_t EmulationController::alignedTickBudget()
{
    const int64_t budget = cyclesPerFrame.load();
    const VideoTiming& vt = pom2VideoTiming(videoStandard_.load());
    const int64_t frame = int64_t(vt.scanlinesPerFrame) * vt.cyclesPerScanline;
    uint64_t vnow;
    { std::lock_guard<std::mutex> lk(stateMtx); vnow = mem.videoNow(); }
    return pom2AlignedTickBudget(budget, frame, vnow);
}
