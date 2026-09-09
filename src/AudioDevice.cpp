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
    // Interleaved stereo: 2 floats per frame.
    std::memset(output, 0,
                static_cast<size_t>(frameCount) * kChannels * sizeof(float));

    std::lock_guard<std::mutex> lock(sourcesMutex);

    // Machine halted (see setSuspended): emit the silence the memset already
    // wrote and DON'T call the sources at all. Calling them would let the
    // free-running generators keep droning while the CPU that feeds them is
    // parked, and would walk the speaker's cycle cursor past a frozen
    // Memory::cycleCounter. The meters still bleed off on the usual 0.85
    // envelope so the mixer's needles fall to zero instead of freezing at
    // whatever the last live buffer measured.
    if (suspended_.load(std::memory_order_relaxed)) {
        for (AudioSource* src : sources) {
            const float p = src->lastBufferPeak.load(std::memory_order_relaxed);
            src->lastBufferPeak.store(p * 0.85f, std::memory_order_relaxed);
        }
        masterPeakL_.store(masterPeakL_.load(std::memory_order_relaxed) * 0.85f,
                           std::memory_order_relaxed);
        masterPeakR_.store(masterPeakR_.load(std::memory_order_relaxed) * 0.85f,
                           std::memory_order_relaxed);
        return;
    }

    if (static_cast<int>(tmpBuf.size()) < frameCount) {
        tmpBuf.resize(static_cast<size_t>(frameCount));
        tmpBufR.resize(static_cast<size_t>(frameCount));
    }

    for (AudioSource* src : sources) {
        // Zero the temp buffer before each source. The AudioSource
        // contract is *either* assign (Speaker, Cassette, Mockingboard
        // all do `output[i] = ...`) *or* mix additively starting from
        // silence (FloppySoundDevice). Without this memset, when the
        // sources iterate in order [A, B] and A writes its signal into
        // tmpBuf, an additive source B would see A's samples and add on
        // top — A then gets counted twice in output (A's pass already
        // added them once), producing audible doubling whenever two
        // sources are simultaneously active. Symptom seen during cold
        // boot: speaker bell beep + Disk II spin-up overlapped, giving
        // a "horrible" composite. The cost is one extra memset per
        // source per ~5 ms buffer — negligible.
        std::memset(tmpBuf.data(), 0,
                    static_cast<size_t>(frameCount) * sizeof(float));
        std::memset(tmpBufR.data(), 0,
                    static_cast<size_t>(frameCount) * sizeof(float));

        // Per-source peak with a short release envelope. 0.85 per
        // ~5 ms buffer settles to <5 % after ~100 ms of silence,
        // matches a typical VU meter release. The peak is sampled
        // BEFORE master gain so the mixer panel reflects the
        // channel's contribution at the slider position; the master
        // peak below reflects what the OS actually plays. On a stereo
        // source it is the louder of the two sides, so one meter still
        // catches a channel that only feeds one speaker.
        float srcPeak = 0.0f;

        if (src->fillAudioBufferStereo(tmpBuf.data(), tmpBufR.data(),
                                       frameCount)) {
            // Native stereo: the source owns its placement (Mockingboard
            // AY1/AY2, Phasor AY pairs), so `pan` is deliberately NOT
            // applied here.
            for (int i = 0; i < frameCount; ++i) {
                const float l = tmpBuf[i];
                const float r = tmpBufR[i];
                const float a = std::max(std::fabs(l), std::fabs(r));
                if (a > srcPeak) srcPeak = a;
                output[2 * i]     += l;
                output[2 * i + 1] += r;
            }
        } else {
            src->fillAudioBuffer(tmpBuf.data(), frameCount);
            // Balance law, NOT constant power: centre is unity on both
            // channels. Constant power would have put every existing
            // mono source 3 dB down the day the bus went stereo, which
            // is a silent regression nobody asked for; the Apple speaker
            // has no stereo position to be faithful to anyway.
            const float p  = std::max(-1.0f, std::min(1.0f,
                src->pan.load(std::memory_order_relaxed)));
            const float gL = (p > 0.0f) ? (1.0f - p) : 1.0f;
            const float gR = (p < 0.0f) ? (1.0f + p) : 1.0f;
            for (int i = 0; i < frameCount; ++i) {
                const float s = tmpBuf[i];
                const float a = std::fabs(s);
                if (a > srcPeak) srcPeak = a;
                output[2 * i]     += s * gL;
                output[2 * i + 1] += s * gR;
            }
        }
        // The per-source discontinuity tally (AudioSource::clicksPerSecond):
        // this source's own output, before it is summed into the bus, so a
        // click is attributed to the source that made it.
        {
            float lastL = src->traceLastL_, lastR = src->traceLastR_;
            uint32_t jumps = 0;
            for (int i = 0; i < frameCount; ++i) {
                const float l = tmpBuf[i];
                const float r = tmpBufR[i];
                if (std::fabs(l - lastL) > AudioSource::kClickThreshold ||
                    std::fabs(r - lastR) > AudioSource::kClickThreshold)
                    ++jumps;
                lastL = l; lastR = r;
            }
            src->traceLastL_ = lastL; src->traceLastR_ = lastR;
            src->traceJumps_ += jumps;
            src->traceFrames_ += static_cast<uint32_t>(frameCount);
            const uint32_t sr = actualSampleRate > 0 ? actualSampleRate : 44100;
            if (src->traceFrames_ >= sr) {
                src->clicksPerSecond.store(
                    static_cast<float>(src->traceJumps_) * static_cast<float>(sr) /
                        static_cast<float>(src->traceFrames_),
                    std::memory_order_relaxed);
                src->traceJumps_ = 0; src->traceFrames_ = 0;
            }
        }
        const float prevSrc =
            src->lastBufferPeak.load(std::memory_order_relaxed);
        const float decayedSrc =
            srcPeak > prevSrc * 0.85f ? srcPeak : prevSrc * 0.85f;
        src->lastBufferPeak.store(decayedSrc, std::memory_order_relaxed);
    }

    // Master gain + mute, then clamp. Snapshot atomics once per buffer
    // (tens of ns vs frameCount loads) — they don't change mid-buffer in
    // any user-perceptible way.
    const float masterGain =
        masterMuted_.load(std::memory_order_relaxed)
            ? 0.0f
            : masterVolume_.load(std::memory_order_relaxed);
    const bool mono = monoDownmix_.load(std::memory_order_relaxed);
    float masterPkL = 0.0f;
    float masterPkR = 0.0f;
    // Master discontinuity tally, folded into the loop that already touches
    // every frame (see AudioDevice.h). Measured POST-clamp, so it counts the
    // steps the OS actually heard — the ones no per-source row can show.
    float    mLastL = masterTraceLastL_, mLastR = masterTraceLastR_;
    uint32_t mJumps = 0;
    for (int i = 0; i < frameCount; ++i) {
        float l = output[2 * i];
        float r = output[2 * i + 1];
        if (mono) {
            // 0.5 * (L + R): the average, not the sum. A centred source
            // sits at unity on both channels, so summing would make it
            // 6 dB louder the moment the user ticked "mono" — and for a
            // stereo card whose two sides used to be summed and halved
            // by the /6 normalisation, the average reproduces the old
            // mono render exactly.
            const float m = 0.5f * (l + r);
            l = m;
            r = m;
        }
        const float cl = std::max(-1.0f, std::min(1.0f, l * masterGain));
        const float cr = std::max(-1.0f, std::min(1.0f, r * masterGain));
        output[2 * i]     = cl;
        output[2 * i + 1] = cr;
        const float al = std::fabs(cl);
        const float ar = std::fabs(cr);
        if (al > masterPkL) masterPkL = al;
        if (ar > masterPkR) masterPkR = ar;
        if (std::fabs(cl - mLastL) > AudioSource::kClickThreshold ||
            std::fabs(cr - mLastR) > AudioSource::kClickThreshold)
            ++mJumps;
        mLastL = cl; mLastR = cr;
    }
    masterTraceLastL_ = mLastL; masterTraceLastR_ = mLastR;
    masterTraceJumps_ += mJumps;
    masterTraceFrames_ += static_cast<uint32_t>(frameCount);
    {
        const uint32_t sr = actualSampleRate > 0 ? actualSampleRate : 44100;
        if (masterTraceFrames_ >= sr) {
            masterClicks_.store(
                static_cast<float>(masterTraceJumps_) * static_cast<float>(sr) /
                    static_cast<float>(masterTraceFrames_),
                std::memory_order_relaxed);
            masterTraceJumps_ = 0; masterTraceFrames_ = 0;
        }
    }
    const float prevL = masterPeakL_.load(std::memory_order_relaxed);
    const float prevR = masterPeakR_.load(std::memory_order_relaxed);
    masterPeakL_.store(masterPkL > prevL * 0.85f ? masterPkL : prevL * 0.85f,
                       std::memory_order_relaxed);
    masterPeakR_.store(masterPkR > prevR * 0.85f ? masterPkR : prevR * 0.85f,
                       std::memory_order_relaxed);
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
    config.sampleRate         = kSampleRate;
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
        std::string("miniaudio ready: requested ") + std::to_string(kSampleRate) +
        " Hz, got " + std::to_string(actualSampleRate) + " Hz");

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
