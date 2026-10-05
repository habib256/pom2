// POM2 Apple II Emulator — GPL-3.0-or-later
// Signal-level checks for alias rejection and sample-rate-independent bass.
#include "SpeakerDevice.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
std::vector<float> render(uint32_t sampleRate, double frequency, int chunk = 256)
{
    SpeakerDevice speaker;
    speaker.setSampleRate(sampleRate);
    // Stay below the catch-up threshold and queue capacity, so this probes
    // reconstruction rather than queue recovery.
    constexpr double seconds = 0.1;
    for (int edge = 0; edge < static_cast<int>(2 * frequency * seconds); ++edge)
        speaker.recordToggle(static_cast<uint64_t>(
            static_cast<double>(edge) * POM2_CPU_CLOCK_HZ / (2 * frequency)));
    std::vector<float> samples(static_cast<size_t>(sampleRate * seconds));
    for (int offset = 0; offset < static_cast<int>(samples.size()); offset += chunk)
        speaker.fillAudioBuffer(samples.data() + offset,
            std::min(chunk, static_cast<int>(samples.size()) - offset));
    return samples;
}

double rms(const std::vector<float>& samples)
{
    double energy = 0;
    for (size_t i = samples.size() / 2; i < samples.size(); ++i) {
        assert(std::isfinite(samples[i]));
        energy += samples[i] * samples[i];
    }
    return std::sqrt(energy / (samples.size() - samples.size() / 2));
}
}

int main()
{
    for (uint32_t rate : {44100u, 48000u, 96000u}) {
        const double tone = rms(render(rate, 1000));
        // A square wave above host Nyquist must not fold into a loud audible
        // tone. Use an integer CPU half-period to separate reconstruction
        // artifacts from timing quantisation in the program producing edges.
        const double halfPeriod = std::floor(POM2_CPU_CLOCK_HZ / (1.25 * rate));
        const double alias = rms(render(rate, POM2_CPU_CLOCK_HZ / (2.0 * halfPeriod)));
        std::printf("%u Hz: tone %.6f, ultrasonic alias %.6f (%.2f dB)\n",
                    rate, tone, alias, 20 * std::log10(alias / tone));
        assert(tone > 0.05 && tone < 0.15);
        assert(alias / tone < 0.002);
    }

    const double bass = rms(render(44100, 80));
    for (uint32_t rate : {48000u, 96000u}) {
        const double actual = rms(render(rate, 80));
        std::printf("%u Hz: bass %.6f (44.1 kHz reference %.6f)\n",
                    rate, actual, bass);
        assert(std::abs(actual / bass - 1) < 0.02);
    }

    // Callback size must not introduce clicks or change the signal.
    const auto small = render(48000, 4300, 17);
    const auto large = render(48000, 4300, 1024);
    assert(small.size() == large.size());
    for (size_t i = 0; i < small.size(); ++i)
        assert(std::abs(small[i] - large[i]) < 1e-6f);
    std::puts("Speaker audio quality: OK");
}
