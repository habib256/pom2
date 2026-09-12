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

// Speech runs on the emuCycles timeline, like everything else — hunt #19.
//
// The SSI263's register writes took effect on the audio side the moment the
// CPU made them, while the card's PSG writes travel to the audio thread as
// cycle-stamped events replayed through a ~40 ms jitter buffer. On a Sound
// II — one card, one speaker, speech and music together — that put the
// speech a whole buffer AHEAD of the music it is synchronised with, and
// quantised every phoneme boundary to the CPU worker's chunk rather than
// the cycle the driver wrote it on.
//
// This pins the fix at chip level: a phoneme queued for a cycle in the
// middle of a buffer starts in the middle of that buffer. The control run
// is the old path (`fillAudio`), which voices the buffer from sample 0 —
// without it this test would pass on any implementation that makes noise.

#include "Ssi263.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;
void fail(const char* what) { std::printf("FAIL: %s\n", what); ++g_failures; }

constexpr uint32_t kSr     = 44100;
constexpr int      kFrames = 1024;
// One sample at 1.0227 MHz / 44100.
constexpr double   kCps    = 1022727.0 / 44100.0;

double energy(const std::vector<float>& b, int from, int to)
{
    double acc = 0.0;
    for (int i = from; i < to; ++i) acc += static_cast<double>(b[i]) * b[i];
    return acc;
}

// Latch a phoneme with the chip held in power-down, then prime the audio
// side by rendering one silent buffer.
void arm(pom2::Ssi263& chip, std::vector<float>& buf, double startCycle)
{
    chip.write(pom2::Ssi263::REG_CTTRAMP, 0x8F);   // CTL high: powered down
    chip.write(pom2::Ssi263::REG_FILFREQ, 0x80);
    chip.write(pom2::Ssi263::REG_DURPHON, 0x0A);   // a voiced phoneme
    std::fill(buf.begin(), buf.end(), 0.0f);
    chip.fillAudioTimed(buf.data(), kFrames, kSr, startCycle, kCps);
    if (energy(buf, 0, kFrames) != 0.0)
        fail("a powered-down chip must render silence");
}

}  // namespace

int main()
{
    std::vector<float> buf(kFrames, 0.0f);

    // ── The fix: the phoneme starts where the driver put it ──────────────
    {
        pom2::Ssi263 chip;
        chip.reset();
        arm(chip, buf, 0.0);

        // Second buffer covers cycles [B, B + kFrames*kCps). Start speaking
        // at the cycle that falls on sample 512 — the middle.
        const double bufStart = static_cast<double>(kFrames) * kCps;
        const double atCycle  = bufStart + 512.0 * kCps;
        chip.write(pom2::Ssi263::REG_CTTRAMP, 0x0F);            // CPU-NOW half
        chip.queuePlaybackEvent(pom2::Ssi263::REG_CTTRAMP, 0x0F,
                                static_cast<uint64_t>(atCycle));

        std::fill(buf.begin(), buf.end(), 0.0f);
        chip.fillAudioTimed(buf.data(), kFrames, kSr, bufStart, kCps);

        const double before = energy(buf, 0, 500);
        const double after  = energy(buf, 524, kFrames);
        std::printf("  energy before the stamped cycle %.6f, after %.6f\n",
                    before, after);
        if (before != 0.0)
            fail("speech began BEFORE the cycle the driver wrote it on");
        if (!(after > 0.0))
            fail("speech never began at all");
    }

    // ── The control: the untimed path voices the whole buffer ────────────
    //
    // Same chip, same writes, rendered the old way. If this were also
    // silent up to sample 500, the assertion above would be measuring
    // nothing.
    {
        pom2::Ssi263 chip;
        chip.reset();
        arm(chip, buf, 0.0);
        chip.write(pom2::Ssi263::REG_CTTRAMP, 0x0F);
        std::fill(buf.begin(), buf.end(), 0.0f);
        chip.fillAudio(buf.data(), kFrames, kSr);
        const double before = energy(buf, 0, 500);
        if (!(before > 0.0))
            fail("control: the untimed path should voice from sample 0");
        else
            std::printf("  ok: control — the untimed path voices from sample 0\n");
    }

    if (g_failures == 0)
        std::printf("  ok: a phoneme starts on the sample its cycle falls in\n");
    if (g_failures) return 1;
    std::puts("ssi263_speech_timeline OK");
    return 0;
}
