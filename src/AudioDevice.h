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

// Central audio output (miniaudio). Mixes registered AudioSource instances
// into the device's INTERLEAVED STEREO float32 output buffer. Ported from
// POM1's AudioDevice — POM2 is desktop-only for now (no WASM target), so the
// Web Audio fallback is dropped.
//
// ── Why the bus is stereo (2026-08-01) ───────────────────────────────────
// The Mockingboard is a stereo card: MAME gives it one 2-channel speaker
// with AY1 on channel 0 and AY2 on channel 1 (`a2mockingboard.cpp:159-165`),
// and the Phasor gets a second one, left = AY1+AY2, right = AY3+AY4
// (`:192-208`). Summing both chips to one channel destroyed the deliberate
// A/B/C pan that music like Digidream 1 writes.
//
// The MONO CONTRACT IS UNCHANGED. `fillAudioBuffer` still hands a source a
// single-channel buffer; the mixer places it with `AudioSource::pan`, whose
// default (centre) gives unity gain on BOTH channels — so speaker, cassette
// and floppy sounds keep exactly the level they had. A source that is
// natively stereo overrides `fillAudioBufferStereo` instead, which lets the
// cards migrate one at a time.

#ifndef POM2_AUDIO_DEVICE_H
#define POM2_AUDIO_DEVICE_H

#include <algorithm>   // std::max — getMasterPeak folds the two channels
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "AudioSource.h"

struct ma_device;

class AudioDevice
{
public:
    static constexpr uint32_t kSampleRate = kAudioSampleRate;
    /// Interleaved stereo. `mixSources` writes frameCount * kChannels
    /// floats — callers sizing a buffer must account for both.
    static constexpr uint32_t kChannels   = 2;

    AudioDevice();
    ~AudioDevice();

    void addSource(AudioSource* source);
    void removeSource(AudioSource* source);

    bool isAvailable() const { return audioAvailable; }

    /// Sample rate negotiated with the OS device. miniaudio frequently
    /// picks 48 kHz on Apple Silicon even when 44.1 kHz is requested;
    /// cycle-driven sources (cassette, future speaker) MUST use this rate
    /// or their playback drifts by the rate ratio.
    uint32_t getActualSampleRate() const { return actualSampleRate; }

    /// Master gain applied after per-source mix, before clamp. Range
    /// [0, 2]. UI mixer panel binds directly to these atomics.
    void  setMasterVolume(float v);
    float getMasterVolume() const { return masterVolume_.load(std::memory_order_relaxed); }
    void  setMasterMuted(bool m) { masterMuted_.store(m, std::memory_order_relaxed); }
    bool  isMasterMuted() const  { return masterMuted_.load(std::memory_order_relaxed); }

    /// Fold the stereo mix back down to a centred mono image
    /// (0.5 * (L + R) on both channels — the exact signal the bus
    /// produced before it went stereo). For mono playback hardware, and
    /// for users who would rather not have a single-AY tune (Digidream 2
    /// never touches chip 2) come out of the left speaker only. Off by
    /// default: the hardware IS stereo, and that is what MAME renders.
    void setMonoDownmix(bool m) { monoDownmix_.store(m, std::memory_order_relaxed); }
    bool isMonoDownmix() const  { return monoDownmix_.load(std::memory_order_relaxed); }

    /// Post-clamp peak abs amplitude of the last mixed buffer, same
    /// release envelope as AudioSource::lastBufferPeak. Mirrors what
    /// the OS actually heard, so the master meter reflects clipping
    /// (saturates at 1.0). Read by the mixer panel. `getMasterPeak` is
    /// the louder of the two channels, so a single meter still shows
    /// clipping on either side.
    float getMasterPeakL() const { return masterPeakL_.load(std::memory_order_relaxed); }
    float getMasterPeakR() const { return masterPeakR_.load(std::memory_order_relaxed); }
    float getMasterPeak() const
    {
        return std::max(masterPeakL_.load(std::memory_order_relaxed),
                        masterPeakR_.load(std::memory_order_relaxed));
    }

    /// Discontinuities per second in the POST-CLAMP bus, the master half of
    /// AudioSource::clicksPerSecond. The per-source tally measures each
    /// source's own output BEFORE the sum, the pan law and the master gain,
    /// so by construction it cannot see a crackle the MIXER made: hard
    /// clipping when the sources sum past full scale (measured: three 0.9
    /// tones clip 36 % of frames while every source row reads 0.0), a
    /// source appearing or vanishing mid-stream (a card plug, a profile
    /// rebuild), a pan flip, or the suspend cut. Without this the Audio
    /// panel's advice — "the crackle comes from the row that shows a
    /// number" — has no row to point at for any of them.
    float getMasterClicksPerSecond() const
    {
        return masterClicks_.load(std::memory_order_relaxed);
    }

    /// Silence the whole bus without touching any source's state or the
    /// user's mixer settings. The emulated machine can be halted (toolbar
    /// pause, a debugger breakpoint, a profile switch, a rewind scrub) while
    /// the host device keeps calling us at 60-ish buffers a second: the
    /// free-running sources — the AY generators on a Mockingboard/Phasor,
    /// the floppy motor loop — have no idea the CPU stopped and go on
    /// droning the last register set for as long as the machine is paused.
    /// `EmulationController::setMode` drives this: silent unless Running.
    /// Suspending here rather than in each source also freezes the speaker's
    /// cycle→sample cursor, which is what the consumer-ahead re-anchor in
    /// `SpeakerDevice::fillAudioBuffer` exists to repair after a pause.
    void setSuspended(bool s) { suspended_.store(s, std::memory_order_relaxed); }
    bool isSuspended() const  { return suspended_.load(std::memory_order_relaxed); }

    /// Mix all registered sources into `output` (clamped to [-1, +1]).
    /// Called from miniaudio's data callback. `output` is INTERLEAVED
    /// STEREO and must hold `frameCount * kChannels` floats.
    void mixSources(float* output, int frameCount);

private:
    bool initAudio();
    void shutdownAudio();

    std::vector<AudioSource*> sources;
    mutable std::mutex sourcesMutex;
    // Per-source planar scratch. `tmpBuf` alone in the mono days; the
    // stereo path needs a second plane, and the mono path still fills
    // only tmpBuf (which the pan law then spreads across both).
    std::vector<float> tmpBuf;
    std::vector<float> tmpBufR;
    bool audioAvailable = false;
    uint32_t actualSampleRate = kSampleRate;

    std::atomic<float> masterVolume_{1.0f};
    std::atomic<bool>  masterMuted_{false};
    std::atomic<bool>  monoDownmix_{false};
    // Default FALSE: a bare AudioDevice (tests, headless composition) must
    // behave exactly as before. Only an explicit setMode(Stopped) mutes it.
    std::atomic<bool>  suspended_{false};
    std::atomic<float> masterPeakL_{0.0f};
    std::atomic<float> masterPeakR_{0.0f};
    // Master discontinuity tally — same shape and same publish period as
    // AudioSource's, but measured on the clamped bus. Scratch is audio-
    // thread-only (mixSources holds sourcesMutex for the whole callback).
    std::atomic<float> masterClicks_{0.0f};
    float    masterTraceLastL_ = 0.0f, masterTraceLastR_ = 0.0f;
    uint32_t masterTraceJumps_ = 0, masterTraceFrames_ = 0;

    struct MaDeviceDeleter { void operator()(ma_device* d) const noexcept; };
    std::unique_ptr<ma_device, MaDeviceDeleter> device;
    static void audioDataCallback(ma_device* pDevice, void* pOutput,
                                  const void* pInput, uint32_t frameCount);
    static void miniaudioLogCallback(void* pUserData, uint32_t level,
                                     const char* pMessage);
};

#endif // POM2_AUDIO_DEVICE_H
