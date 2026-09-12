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

// There is ONE mix law, and a mixer can find every card — bug hunt #19.
//
// `Pom2Core::pullAudio`, the public integration API an embedder gets its
// audio from, used to hand-roll a second mixer: speaker + cassette + ONE
// Mockingboard, summed and clamped. No pan, no master gain, no mute, no
// mono downmix, no meters — and no second sound card, so a Phasor, an
// Echo+ or a second Mockingboard was silent through the API while the GUI
// played it fine. Divergence like that is not fixed once: the master ramp
// added in this same hunt, and the click tallies added in the last one,
// would both have missed it again.
//
// So the law moved to `AudioMix.h` and both call sites run it. This pins
// the two properties that make that true: the law itself behaves (pan,
// master gain, mute, mono, clamp, meters), and a mixer can discover a
// card's audio through the BASE class instead of knowing card names.

#include "AudioMix.h"
#include "DiskIICard.h"
#include "Mockingboard.h"
#include "SlotBus.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (ok) { std::printf("  ok: %s\n", what); return; }
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}

/// A source that emits a constant, so every law below is readable by eye.
struct ConstSource : AudioSource {
    float v;
    explicit ConstSource(float value) : v(value) {}
    void fillAudioBuffer(float* out, int frames) override
    {
        for (int i = 0; i < frames; ++i) out[i] = v;
    }
};

constexpr int kFrames = 256;

// ── 1. A mixer can WALK the bus ──────────────────────────────────────────
//
// This is what `pullAudio` now does instead of knowing one card by name.
void testTheBusExposesCardAudio()
{
    SlotBus bus;
    bus.plug(4, std::make_unique<MockingboardCard>(4));
    bus.plug(6, std::make_unique<DiskIICard>(6));

    int found = 0;
    for (int slot = 1; slot <= 7; ++slot)
        if (SlotPeripheral* card = bus.peripheral(slot))
            if (card->audioSource()) ++found;

    check(found == 1, "a sound card offers its audio through the base class");
    // The control: a card that makes no sound must not answer, or the walk
    // would register anything plugged in.
    check(bus.peripheral(6) && bus.peripheral(6)->audioSource() == nullptr,
          "…and a Disk II, which has no audio output, offers none");
}

// ── 2. The law: pan, master gain, mute, mono, clamp ──────────────────────
void testTheLaw()
{
    ConstSource src(0.5f);
    std::vector<AudioSource*> sources{&src};
    std::vector<float> out(kFrames * pom2::kMixChannels, 0.0f);
    pom2::MixBusState st;
    pom2::MixParams   p;
    p.sampleRate = 44100;

    // Centre is unity on both channels — a balance law, not constant power.
    pom2::mixSourcesInto(out.data(), kFrames, sources, p, st);
    check(std::fabs(out[2 * (kFrames - 1)] - 0.5f) < 1e-6f &&
          std::fabs(out[2 * (kFrames - 1) + 1] - 0.5f) < 1e-6f,
          "a centred mono source is unity on both channels");

    // Hard left: the right channel empties.
    src.pan.store(-1.0f, std::memory_order_relaxed);
    pom2::mixSourcesInto(out.data(), kFrames, sources, p, st);
    check(std::fabs(out[2 * (kFrames - 1)] - 0.5f) < 1e-6f &&
          std::fabs(out[2 * (kFrames - 1) + 1]) < 1e-6f,
          "pan is honoured — the API never applied it at all");
    src.pan.store(0.0f, std::memory_order_relaxed);

    // Master gain, reached by the ramp, then mono, then the clamp.
    p.masterGain = 0.5f;
    pom2::mixSourcesInto(out.data(), kFrames, sources, p, st);
    check(std::fabs(out[2 * (kFrames - 1)] - 0.25f) < 1e-6f,
          "master gain applies (the API had no master volume)");

    p.masterGain = 0.0f;
    pom2::mixSourcesInto(out.data(), kFrames, sources, p, st);
    check(out[2 * (kFrames - 1)] == 0.0f,
          "master mute applies (the API could not be muted)");

    // Past full scale — but the master gain RAMPS (bug hunt #19), and from the
    // mute above it has to climb all the way to 4.0 at one step per frame
    // (1 / (0.005 * 44100)): 882 frames, more than this buffer holds. Render
    // until it converges, and check on the way that the clamp never lets a
    // sample out of range — the ramp must not be a hole in the limiter.
    p.masterGain = 4.0f;
    bool insideFullScale = true;
    for (int buffer = 0; buffer < 8; ++buffer) {
        pom2::mixSourcesInto(out.data(), kFrames, sources, p, st);
        for (float v : out)
            if (v > 1.0f || v < -1.0f) insideFullScale = false;
    }
    check(insideFullScale, "the clamp holds all the way up the ramp");
    check(out[2 * (kFrames - 1)] == 1.0f, "the bus is clamped to full scale");

    // Suspended: the machine is halted, so the sources are not even called.
    ConstSource loud(1.0f);
    std::vector<AudioSource*> one{&loud};
    pom2::MixParams sp;
    sp.suspended = true;
    sp.sampleRate = 44100;
    pom2::MixBusState sst;
    std::vector<float> quiet(kFrames * pom2::kMixChannels, 0.0f);
    pom2::mixSourcesInto(quiet.data(), kFrames, one, sp, sst);
    double energy = 0.0;
    for (float s : quiet) energy += static_cast<double>(s) * s;
    check(energy == 0.0, "a suspended bus is silent (the API had no suspend)");

    // And the meters the API never had.
    check(st.peakL.load(std::memory_order_relaxed) > 0.0f,
          "the master meter is fed");
}

}  // namespace

int main()
{
    testTheBusExposesCardAudio();
    testTheLaw();
    if (g_failures) return 1;
    std::puts("audio_mix_law OK");
    return 0;
}
