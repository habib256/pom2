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
#ifdef __EMSCRIPTEN__
#include <emscripten.h>   // EM_ASM_INT: the user agent, for the platform
#endif

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
// intrinsics (ma_atomic_load_64 on &pSound->seekTarget). The WebAudio backend
// also leaves context parameters unused. Keep these vendor diagnostics local
// to the implementation include; first-party audio code stays fully checked.
#if defined(__clang__) && defined(__EMSCRIPTEN__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio.h"
#if defined(__clang__) && defined(__EMSCRIPTEN__)
#pragma clang diagnostic pop
#elif defined(__GNUC__) && !defined(__clang__)
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
    // The late-callback tally (AudioBufferPolicy.h). A gap longer than the
    // whole buffer means the device ran dry before it asked again; the
    // coordinator climbs the buffer ladder on these. Wall clock, not
    // frames: it is the host's scheduling that is being measured.
    const auto now = std::chrono::steady_clock::now();
    if (self->haveLastCallback_) {
        const double gap =
            std::chrono::duration<double>(now - self->lastCallback_).count();
        if (pom2::AudioBufferPolicy::callbackIsLate(
                gap, self->periodFrames_.load(std::memory_order_relaxed),
                self->actualSampleRate))
            self->lateCallbacks_.fetch_add(1, std::memory_order_relaxed);
    }
    self->lastCallback_     = now;
    self->haveLastCallback_ = true;
    self->callbacks_.fetch_add(1, std::memory_order_relaxed);
    self->mixSources(output, static_cast<int>(frameCount));
}

AudioDevice::Platform AudioDevice::detectPlatform()
{
#if defined(__EMSCRIPTEN__)
    // A phone's browser runs the emulated frame AND the audio callback on
    // one thread, and the frame is slower there: start a rung higher.
    const int mobile = EM_ASM_INT({
        return (typeof navigator !== 'undefined' &&
                /Android|Mobi|iPhone|iPad|iPod/i.test(navigator.userAgent)) ? 1 : 0;
    });
    return mobile ? Platform::BrowserMobile : Platform::Browser;
#elif defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
    return Platform::LinuxArm;
#else
    return Platform::Desktop;
#endif
}

AudioDevice::AudioDevice() : AudioDevice(OutputMode::Host)
{
}

// The rung the browser remembered across loads. localStorage rather than
// state.cfg because the device is opened in EmulationController's
// constructor, before the settings are restored (and before IDBFS has
// necessarily synced), and the whole point is to open it right the first
// time. Guarded: a private window or a blocked origin throws on access.
uint32_t AudioDevice::rememberedPeriodFrames()
{
#if defined(__EMSCRIPTEN__)
    const int frames = EM_ASM_INT({
        try {
            var v = window.localStorage.getItem('pom2.audio_buffer_frames');
            var n = v === null ? 0 : parseInt(v, 10);
            return (n > 0 && n < 65536) ? n : 0;
        } catch (e) { return 0; }
    });
    return static_cast<uint32_t>(frames);
#else
    return 0;
#endif
}

void AudioDevice::rememberPeriodFrames(uint32_t frames)
{
#if defined(__EMSCRIPTEN__)
    EM_ASM({
        try { window.localStorage.setItem('pom2.audio_buffer_frames', String($0)); }
        catch (e) {}
    }, static_cast<int>(frames));
#else
    (void)frames;
#endif
}

AudioDevice::AudioDevice(OutputMode mode)
    : platform_(detectPlatform())
{
    uint32_t start = pom2::AudioBufferPolicy::defaultPeriodFrames(platform_);
    // The browser's remembered rung, when it is one and above the default
    // (a default that moved up makes an older memory stale).
    const uint32_t remembered = rememberedPeriodFrames();
    if (pom2::AudioBufferPolicy::isRung(remembered, platform_) && remembered > start)
        start = remembered;
    periodFrames_.store(start, std::memory_order_relaxed);
    nextLoadPeriodFrames_ = start;
    if (mode == OutputMode::Host) initAudio(periodFrames());
}

bool AudioDevice::reconfigurePeriod(uint32_t periodFrames)
{
    if (!pom2::AudioBufferPolicy::isRung(periodFrames, platform_)) return false;
    if (appliesAtNextLoad()) {
        // Browser: remember, never re-open (see the header).
        nextLoadPeriodFrames_ = periodFrames;
        rememberPeriodFrames(periodFrames);
        return true;
    }
    nextLoadPeriodFrames_ = periodFrames;
    if (periodFrames == this->periodFrames() && device) return true;
    if (!device) {
        // Offline, or a first open that failed: nothing to re-open. Record
        // the rung so a later open uses it.
        periodFrames_.store(periodFrames, std::memory_order_relaxed);
        return true;
    }
    // Stop the old device first — the deleter's ma_device_uninit joins the
    // audio thread — so the fields below are written with no reader alive.
    device.reset();
    audioAvailable    = false;
    haveLastCallback_ = false;
    periodFrames_.store(periodFrames, std::memory_order_relaxed);
    const uint32_t rateBefore = actualSampleRate;
    const bool ok = initAudio(periodFrames);
    // The sources outlive the device. Tell the RateAware ones the rate again:
    // a re-opened device may negotiate another one, and a source that kept
    // synthesising on the old grid would drift by the ratio (bug hunt #19).
    if (ok && actualSampleRate != rateBefore) {
        std::lock_guard<std::mutex> lock(sourcesMutex);
        for (AudioSource* s : sources)
            if (auto* ra = dynamic_cast<RateAware*>(s))
                ra->setSampleRate(actualSampleRate);
    }
    return ok;
}

AudioDevice::~AudioDevice()
{
    shutdownAudio();
}

bool AudioDevice::initAudio(uint32_t periodFrames)
{
    // Not shutdownAudio(): that clears the source list, and reconfigurePeriod
    // re-opens the device UNDER a live list. The caller guarantees no device
    // is open here (the constructor has none; reconfigurePeriod reset it).
    device.reset();
    audioAvailable = false;

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
    // The period is a rung of AudioBufferPolicy's ladder: 256 frames
    // (5.3 ms at 48 kHz) on a desktop, 1024 on a Linux ARM board, 2048 in
    // the browser — which has no audio thread: without -sAUDIO_WORKLET
    // miniaudio plays through a ScriptProcessorNode whose `onaudioprocess`
    // runs on the MAIN thread, queued behind the rAF frame that runs the
    // CPU and paints ImGui (`miniaudio.h`, ma_device_init__webaudio), so a
    // 5.3 ms period missed its deadline almost every frame (2026-10-23) —
    // and 4096 on a phone. The coordinator climbs a rung whenever the
    // callbacks above come late. The sources' jitter buffers size
    // themselves from the frameCount they are asked for (Mockingboard /
    // Phasor `targetLag`), so a larger callback does not make them
    // re-anchor.
    config.periodSizeInFrames = periodFrames;
    config.periods            = pom2::AudioBufferPolicy::kPeriods;
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
    // Publish the negotiated rate BEFORE starting the device. ma_device_start
    // launches miniaudio's callback thread, and that thread reads
    // actualSampleRate in mixSources() — so assigning it afterwards is a plain
    // unsynchronised write racing a live reader. TSan caught it on four
    // nightly tests (rewind_transport, ui_worker_contention,
    // ai_control_server_smoke, audio_source_teardown), every one pointing at
    // this one uint32_t. ma_device_init has already filled raw->sampleRate, so
    // the value is final here and the write now happens-before the thread that
    // reads it exists — no atomic needed, and nothing writes it again.
    actualSampleRate = raw->sampleRate;

    if (ma_device_start(raw) != MA_SUCCESS) {
        ma_device_uninit(raw);
        delete raw;
        audioAvailable = false;
        pom2::log().warn("Audio", "ma_device_start failed — audio disabled");
        return false;
    }

    pom2::log().info("Audio",
        std::string("miniaudio ready: the device's own rate is ") +
        std::to_string(actualSampleRate) + " Hz" +
        (actualSampleRate == kSampleRate
             ? std::string(" (POM2's nominal rate)")
             : std::string(" — every source synthesises straight to it, so "
                           "miniaudio inserts no conversion of its own")) +
        "; buffer " + std::to_string(pom2::AudioBufferPolicy::kPeriods) +
        " x " + std::to_string(periodFrames) + " frames (" +
        std::to_string(static_cast<int>(
            pom2::AudioBufferPolicy::bufferSeconds(periodFrames, actualSampleRate) *
            1000.0 + 0.5)) + " ms)");

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
