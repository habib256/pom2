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

// AudioDevice + dual-instance FloppySoundDevice smoke test.
//
// Pins two regressions from the 2026-05-15/16 mixer + 3.5" sound work:
//
//   1. Two FloppySoundDevice instances coexist with different form
//      factors — the 5.25" bank doesn't get clobbered when the 3.5"
//      instance loads its WAVs (each device stores a single sample
//      bank, so we needed two devices).
//
//   2. AudioDevice master volume + master mute apply post-mix:
//      master_mute=true  → output silent regardless of source content
//      master_vol=0.5    → output halved
//      master_vol=1.0    → output unchanged

#include "AudioDevice.h"
#include "FloppySoundDevice.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string findSampleDir()
{
    static const char* candidates[] = {
        "roms/floppy_samples",
        "../roms/floppy_samples",
        "../../roms/floppy_samples",
        "../../../roms/floppy_samples",
    };
    for (const char* p : candidates) {
        if (fs::is_directory(p)) return p;
    }
    return {};
}

// Constant-output source for the master gain test. Emits +0.5 on every
// sample so the energy is predictable and we can compare scaling
// directly.
class ConstSource : public AudioSource
{
public:
    explicit ConstSource(float v) : v_(v) {}
    void fillAudioBuffer(float* output, int frameCount) override
    {
        for (int i = 0; i < frameCount; ++i) output[i] = v_;
    }
private:
    float v_;
};

void testDualInstanceCoexistence(const std::string& dir)
{
    FloppySoundDevice fs525;
    FloppySoundDevice fs35;

    // Load each instance with its own form factor. The bug we're
    // pinning: a single shared FloppySoundDevice would have its sample
    // bank overwritten by the second loadSamples() call, leaving one
    // form factor silent. With two instances both must report loaded.
    assert(fs525.loadSamples(dir, FloppySoundDevice::FormFactor::FF525));
    assert(fs35 .loadSamples(dir, FloppySoundDevice::FormFactor::FF35));
    assert(fs525.isLoaded());
    assert(fs35 .isLoaded());
    assert(fs525.formFactor() == FloppySoundDevice::FormFactor::FF525);
    assert(fs35 .formFactor() == FloppySoundDevice::FormFactor::FF35);

    // Independent audible motor — the 525 bank still produces output
    // even after the 35 bank has been loaded "next to" it.
    fs525.setSampleRate(44100);
    fs525.setVolume(1.0f);
    fs525.motor(true, true);
    std::vector<float> buf(2048, 0.0f);
    fs525.fillAudioBuffer(buf.data(), 2048);
    double e525 = 0.0;
    for (float v : buf) e525 += static_cast<double>(v) * v;
    assert(e525 > 0.01);

    // Same for 35.
    fs35.setSampleRate(44100);
    fs35.setVolume(1.0f);
    fs35.motor(true, true);
    buf.assign(2048, 0.0f);
    fs35.fillAudioBuffer(buf.data(), 2048);
    double e35 = 0.0;
    for (float v : buf) e35 += static_cast<double>(v) * v;
    assert(e35 > 0.01);

    std::puts("OK dual_instance_coexistence");
}

void testMasterVolumeAndMute()
{
    AudioDevice dev;
    // mixSources is called by miniaudio's callback on a real device,
    // but we drive it directly here — independent of whether the
    // hardware audio init succeeded (headless CI commonly fails it).
    ConstSource src(0.5f);
    dev.addSource(&src);

    // The bus is interleaved STEREO: mixSources writes
    // kFrames * kChannels floats. A mono source sits at the default
    // centre pan, which is unity on both channels, so every sample in
    // the buffer — left and right alike — carries the same value the
    // mono bus used to produce.
    constexpr int kFrames = 256;
    std::vector<float> out(kFrames * AudioDevice::kChannels, 0.0f);
    const int kSamples = kFrames * static_cast<int>(AudioDevice::kChannels);

    // Default master = 1.0, unmuted: output should match source value
    // (clamped to ±1).
    dev.setMasterVolume(1.0f);
    dev.setMasterMuted(false);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kSamples; ++i) {
        assert(std::fabs(out[i] - 0.5f) < 1e-6f);
    }

    // Half volume.
    dev.setMasterVolume(0.5f);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kSamples; ++i) {
        assert(std::fabs(out[i] - 0.25f) < 1e-6f);
    }

    // Master mute zeroes everything, regardless of master volume.
    dev.setMasterVolume(2.0f);
    dev.setMasterMuted(true);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kSamples; ++i) {
        assert(out[i] == 0.0f);
    }

    // Unmute + sane volume restores normal behaviour.
    dev.setMasterMuted(false);
    dev.setMasterVolume(1.0f);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kSamples; ++i) {
        assert(std::fabs(out[i] - 0.5f) < 1e-6f);
    }

    dev.removeSource(&src);
    std::puts("OK master_volume_and_mute");
}

void testPerSourcePeakTracking()
{
    // Pins the mixer-panel level meters: AudioDevice::mixSources updates
    // each source's lastBufferPeak with the absolute peak it produced,
    // and AudioDevice::getMasterPeak() reflects the post-clamp output.
    // Without this the UI can't show "channel is alive" feedback and
    // users wonder if the slider is wired at all.
    AudioDevice dev;
    ConstSource quiet(0.25f);
    ConstSource loud (0.80f);
    dev.addSource(&quiet);
    dev.addSource(&loud);

    constexpr int kFrames = 256;
    std::vector<float> out(kFrames * AudioDevice::kChannels, 0.0f);

    dev.setMasterVolume(1.0f);
    dev.setMasterMuted(false);
    dev.mixSources(out.data(), kFrames);

    // Each source's peak should reflect its own contribution, not the
    // sum — quiet sees 0.25, loud sees 0.80. (envelope release doesn't
    // matter on the first buffer because previous peak was 0.)
    const float qPeak = quiet.lastBufferPeak.load();
    const float lPeak = loud .lastBufferPeak.load();
    assert(std::fabs(qPeak - 0.25f) < 1e-6f);
    assert(std::fabs(lPeak - 0.80f) < 1e-6f);
    // Master peak is post-clamp: 0.25 + 0.80 = 1.05 → clamps to 1.0.
    assert(std::fabs(dev.getMasterPeak() - 1.0f) < 1e-6f);

    // Silence a buffer: peaks decay (0.85 per buffer) — they should be
    // strictly less than the previous peak but not yet zero.
    ConstSource silent(0.0f);
    dev.removeSource(&quiet);
    dev.removeSource(&loud);
    dev.addSource(&silent);
    // Need a source already producing the silent value so the mix
    // contributes 0 but the loud/quiet entries keep their old peaks
    // (they're no longer in the source list; peaks just hang at the
    // last seen value).
    dev.mixSources(out.data(), kFrames);
    const float silentPeak = silent.lastBufferPeak.load();
    assert(silentPeak == 0.0f);
    // Master peak should also decay now that no source contributes.
    assert(dev.getMasterPeak() < 1.0f);

    dev.removeSource(&silent);
    std::puts("OK per_source_peak_tracking");
}

void testRateAwareAutoConfig()
{
    // FloppySoundDevice inherits RateAware. AudioDevice::addSource
    // should auto-call setSampleRate(actualSampleRate) so a future hot-
    // plug path that forgets the explicit call still gets configured.
    AudioDevice dev;
    FloppySoundDevice fs;
    dev.addSource(&fs);
    // We can't directly observe `outputSampleRate_` from outside, but
    // the contract is: after addSource the source is rate-configured
    // and ready to mix. A subsequent fillAudioBuffer must not crash on
    // an uninitialised sample rate (the previous buggy state had
    // outputSampleRate_=44100 by default — fine — but with the auto
    // path the value should track the negotiated device rate).
    std::vector<float> buf(64, 0.0f);
    fs.fillAudioBuffer(buf.data(), 64);   // must not crash, must not write
    for (float v : buf) assert(v == 0.0f);
    dev.removeSource(&fs);
    std::puts("OK rate_aware_auto_config");
}

}  // namespace

int main()
{
    std::puts("=== AudioDevice + dual FloppySoundDevice smoke test ===");
    const std::string dir = findSampleDir();
    if (dir.empty()) {
        std::puts("SKIP: roms/floppy_samples not found from cwd");
        return 77;   // ctest SKIP_RETURN_CODE
    }
    std::printf("Using sample dir: %s\n", dir.c_str());
    testDualInstanceCoexistence(dir);
    testMasterVolumeAndMute();
    testPerSourcePeakTracking();
    testRateAwareAutoConfig();
    std::puts("All audio mixer smoke tests passed.");
    return 0;
}
