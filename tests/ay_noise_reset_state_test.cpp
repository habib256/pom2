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

// AY-3-8910 noise output immediately after a chip reset.
//
// MAME reads the AY-3-8910's noise output as `m_rng & 1`
// (`ay8910.h:284 noise_output()`; `m_noise_out` is the AY8930-only
// branch), and `ay8910_reset_ym` seeds `m_rng = 1` — so the noise output
// is HIGH from the very first base tick after a reset.
//
// `ChipSynthState::noiseOut` is a cache of that bit, and it was seeded to
// 0 and only written when the LFSR actually advances. The LFSR advances
// every 2*NP base ticks (the prescaler halves the counter rate), so the
// noise channel was pinned LOW for 2*NP-1 base ticks after every /RESET
// strobe — up to 61 ticks = 477 us at NP=$1F — where hardware has it
// HIGH. Music drivers strobe PB2 low at init and on every silence, so a
// percussion hit fired right after that lost the front of its attack.
// Bug hunt 16.

#include "AyPsgSynth.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

namespace ay = pom2::ay;

// MAME's noise path, transcribed: the 17-bit LFSR x^17+x^14+1 plus the
// divide-by-two prescaler, read as `m_rng & 1`.
struct MameNoise {
    uint32_t rng = 1;
    int count = 0, prescale = 0;
    void tick(int np)
    {
        if ((++count) >= (np & 0x1F)) {
            count = 0;
            prescale ^= 1;
            if (!prescale)
                rng = (rng >> 1) | ((((rng >> 0) ^ (rng >> 3)) & 1) << 16);
        }
    }
    int out() const { return static_cast<int>(rng & 1); }
};

void testNoiseOutputMatchesMameFromTickZero()
{
    for (int np : {0, 1, 2, 4, 8, 16, 31}) {
        ay::ChipSynthState cs;
        cs.resetGenerators();                 // the /RESET strobe path
        uint8_t r[16] = {0};
        r[6] = static_cast<uint8_t>(np);
        MameNoise ref;
        for (int t = 0; t < 512; ++t) {
            ay::stepTick(cs, r);
            ref.tick(np);
            if (static_cast<int>(cs.noiseOut) != ref.out()) {
                std::fprintf(stderr,
                    "ay noise reset: NP=%d, base tick %d: POM2 noiseOut=%d, "
                    "MAME noise_output() (= m_rng & 1, m_rng seeded to 1) = %d. "
                    "The cached output is seeded to 0 instead of 1, so the noise "
                    "channel is held low for the first 2*NP-1 ticks after a "
                    "reset.\n", np, t, static_cast<int>(cs.noiseOut), ref.out());
                std::abort();
            }
        }
    }
    std::printf("  ok: noise output tracks m_rng&1 from tick 0 for NP = 0..31\n");
}

void testFreshStateAlsoStartsHigh()
{
    // A default-constructed ChipSynthState (a freshly plugged card, before
    // any /RESET event) must agree with a reset one.
    ay::ChipSynthState fresh;
    if (fresh.noiseOut != 1) {
        std::fprintf(stderr,
            "ay noise reset: default-constructed noiseOut = %d, expected 1 "
            "(m_rng seeded to 1)\n", static_cast<int>(fresh.noiseOut));
        std::abort();
    }
    // ...and a noise-only channel at full volume must be audible on the
    // very first sample, not silent.
    uint8_t r[16] = {0};
    r[6] = 0x1F;              // NP = 31 — the slowest noise, worst case
    r[7] = 0x07;              // tone disabled on all three, noise enabled
    r[8] = 0x0F;              // channel A at max
    const float lvl = ay::chipLevel(fresh, r);
    if (lvl <= 0.0f) {
        std::fprintf(stderr,
            "ay noise reset: noise-only channel A reads level %.4f on the first "
            "base tick after reset; hardware/MAME output the LFSR seed bit (1) "
            "so the channel is ON.\n", lvl);
        std::abort();
    }
    std::printf("  ok: noise-only channel is live on the first tick after reset "
                "(level %.4f)\n", lvl);
}

} // namespace

int main()
{
    std::printf("AY-3-8910 noise reset-state test\n");
    testFreshStateAlsoStartsHigh();
    testNoiseOutputMatchesMameFromTickZero();
    std::printf("PASS\n");
    return 0;
}
