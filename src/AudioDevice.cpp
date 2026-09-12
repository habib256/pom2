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

#include "AudioDevice.h"

#include "AudioMix.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>     // std::fabs — libstdc++ leaks it via other headers,
                     // MSVC does not (caught by the Windows release build)
#include <cstring>

// Vorbis backend for miniaudio: including stb_vorbis.c in this TU defines
// STB_VORBIS_INCLUDE_STB_VORBIS_H, which lets miniaudio find its decoder.
// stb_vorbis raises benign warnings under -Wall — silence them locally.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#pragma clang diagnostic ignored "-Wshadow"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "third_party/stb_vorbis.c"
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

// GCC's -Wstringop-overflow trips a false positive on miniaudio's atomic
// intrinsics (ma_atomic_load_64 on &pSound->seekTarget). Silence locally;
// no other warnings appear in the miniaudio TU at our level.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#if defined(_WIN32)
#  ifdef min
#    undef min
#  endif
#  ifdef max
#    undef max
#  endif
#endif

void AudioDevice::mixSources(float* output, int frameCount)
{
    std::lock_guard<std::mutex> lock(sourcesMutex);
    pom2::MixParams p;
    p.masterGain = masterMuted_.load(std::memory_order_relaxed)
                       ? 0.0f
                       : masterVolume_.load(std::memory_order_relaxed);
    p.mono       = monoDownmix_.load(std::memory_order_relaxed);
    p.suspended  = suspended_.load(std::memory_order_relaxed);
    p.sampleRate = actualSampleRate;
    pom2::mixSourcesInto(output, frameCount, sources, p, mix_);
}

void AudioDevice::setMasterVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    masterVolume_.store(v, std::memory_order_relaxed);
}

void AudioDevice::addSource(AudioSource* source)
{
    if (!source) return;
    // Defensive auto-config: any source that exposes RateAware gets the
    // currently negotiated rate before its first fillAudioBuffer. The
    // existing call sites in EmulationController / MainWindow already
    // call setSampleRate manually, so this is normally redundant — it
    // exists to avoid silent drift if a future hot-plug path forgets.
    if (auto* ra = dynamic_cast<RateAware*>(source))
        ra->setSampleRate(actualSampleRate);
    std::lock_guard<std::mutex> lock(sourcesMutex);
    sources.push_back(source);
}

void AudioDevice::removeSource(AudioSource* source)
{
    std::lock_guard<std::mutex> lock(sourcesMutex);
    sources.erase(std::remove(sources.begin(), sources.end(), source), sources.end());
}

void AudioDevice::audioDataCallback(ma_device* pDevice, void* pOutput,
                                    const void* /*pInput*/, uint32_t frameCount)
{
    AudioDevice* self = static_cast<AudioDevice*>(pDevice->pUserData);
    float* output = static_cast<float*>(pOutput);
    if (self == nullptr) {
        std::fill(output, output + frameCount * kChannels, 0.0f);
        return;
    }
    self->mixSources(output, static_cast<int>(frameCount));
}

AudioDevice::AudioDevice()
{
    initAudio();
}

AudioDevice::~AudioDevice()
{
    shutdownAudio();
}

bool AudioDevice::initAudio()
{
    shutdownAudio();

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format    = ma_format_f32;
    // Stereo since 2026-08-01 — the Mockingboard/Phasor AY chips are
    // panned per chip on real hardware (see the header). miniaudio
    // handles the mono-device case itself when the OS only offers one
    // channel, and the mixer's own "mono downmix" covers the user who
    // wants a centred image on stereo hardware.
    config.playback.channels  = kChannels;
    // ZERO = "give me the device's own rate", and that is the whole point of
    // `getActualSampleRate()` (bug hunt #19). Asking for 44100 was not a
    // request: miniaudio assigns `pDevice->sampleRate = pConfig->sampleRate`
    // and only adopts the hardware's rate `if (pDevice->sampleRate == 0)`
    // (`miniaudio.h:42543`), so the getter answered 44100 on every machine
    // for ever — and on 48 kHz-only hardware (Apple Silicon) miniaudio
    // silently inserted its OWN conversion, defaulting to
    // `ma_resample_algorithm_linear`, documented as "Fastest, lowest
    // quality" (`miniaudio.h:5403`). Every source synthesised on a 44.1 kHz
    // grid was then linearly interpolated to 48 kHz behind POM2's back:
    // imaging on the speaker's square edges and on the AY's top octave.
    // Now the sources are told the real rate and synthesise straight to it
    // (`AudioSource.h`'s RateAware contract, which `addSource` applies).
    config.sampleRate         = 0;
    config.periodSizeInFrames = 256;
    config.periods            = 3;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback       = &AudioDevice::audioDataCallback;
    config.pUserData          = this;

    ma_device* raw = new ma_device();
    if (ma_device_init(nullptr, &config, raw) != MA_SUCCESS) {
        delete raw;
        audioAvailable = false;
        pom2::log().warn("Audio", "ma_device_init failed — audio disabled");
        return false;
    }
    if (ma_device_start(raw) != MA_SUCCESS) {
        ma_device_uninit(raw);
        delete raw;
        audioAvailable = false;
        pom2::log().warn("Audio", "ma_device_start failed — audio disabled");
        return false;
    }

    actualSampleRate = raw->sampleRate;
    pom2::log().info("Audio",
        std::string("miniaudio ready: the device's own rate is ") +
        std::to_string(actualSampleRate) + " Hz" +
        (actualSampleRate == kSampleRate
             ? std::string(" (POM2's nominal rate)")
             : std::string(" — every source synthesises straight to it, so "
                           "miniaudio inserts no conversion of its own")));

    // Forward miniaudio's internal log (warnings, underruns, device drops…)
    // into pom2::log so they show up next to our own audio diagnostics.
    // Helps explain user-reported "pertes de son" when the cause is a
    // device-side underrun rather than a source bug.
    if (ma_log* pLog = ma_device_get_log(raw)) {
        ma_log_register_callback(pLog,
            ma_log_callback_init(&AudioDevice::miniaudioLogCallback, this));
    }

    device.reset(raw);
    audioAvailable = true;
    return true;
}

void AudioDevice::miniaudioLogCallback(void* /*pUserData*/, uint32_t level,
                                       const char* pMessage)
{
    if (!pMessage) return;
    // Strip the trailing newline miniaudio appends — pom2::log adds its
    // own framing.
    std::string msg(pMessage);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    if (msg.empty()) return;
    if (level <= MA_LOG_LEVEL_WARNING) {
        pom2::log().warn("Audio/ma", msg);
    } else if (level == MA_LOG_LEVEL_INFO) {
        pom2::log().info("Audio/ma", msg);
    }
    // Drop MA_LOG_LEVEL_DEBUG — too chatty (per-buffer messages).
}

void AudioDevice::shutdownAudio()
{
    // device.reset() -> MaDeviceDeleter -> ma_device_uninit synchronously
    // drains the callback, so by the time we clear sources no audio thread
    // is racing with us.
    device.reset();
    audioAvailable = false;
    {
        std::lock_guard<std::mutex> lock(sourcesMutex);
        sources.clear();
    }
}

void AudioDevice::MaDeviceDeleter::operator()(ma_device* d) const noexcept
{
    if (!d) return;
    ma_device_uninit(d);
    delete d;
}
