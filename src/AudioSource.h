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

// AudioSource — what a sound-producing device must offer the mixer.
//
// Deliberately separate from AudioDevice.h. A device (cassette, speaker, a
// Mockingboard) needs to BE an audio source; it has no business seeing the
// mixer, the miniaudio handle or the master volume. Keeping the interface
// here is what lets the machine layer depend on it without depending on the
// runtime — the configure-time layer guard rejects the other direction.

#ifndef POM2_AUDIO_SOURCE_H
#define POM2_AUDIO_SOURCE_H

#include <atomic>
#include <cstdint>

/// The rate every source assumes until the device negotiates another. Lives
/// here rather than on AudioDevice so a source can name it without seeing the
/// mixer.
inline constexpr uint32_t kAudioSampleRate = 44100;

class AudioSource
{
public:
    virtual ~AudioSource() = default;
    /// Fill `output` with `frameCount` mono float32 samples. Called from
    /// the audio callback thread — must be fast and thread-safe.
    /// The mixer places the result in the stereo field using `pan`.
    virtual void fillAudioBuffer(float* output, int frameCount) = 0;

    /// Optional native-stereo path, for sources whose hardware really has
    /// two outputs (Mockingboard: AY1 left / AY2 right; Phasor: one AY
    /// pair per side). `left` and `right` are separate `frameCount`-sample
    /// planes, both pre-zeroed by the mixer, and `pan` is NOT applied —
    /// the source owns its own placement, exactly as the card's wiring
    /// does. Return true when filled; the default returns false and the
    /// mixer falls back to the mono path above, so existing sources need
    /// no change.
    virtual bool fillAudioBufferStereo(float* /*left*/, float* /*right*/,
                                       int /*frameCount*/) { return false; }

    /// Post-fill peak abs amplitude of the last buffer this source
    /// produced, with a short release envelope (decays ~85% per 5 ms
    /// buffer → invisible after ~100 ms of silence). Updated by
    /// AudioDevice::mixSources right after calling fillAudioBuffer; the
    /// UI mixer panel reads it to draw a small level meter so users can
    /// immediately confirm a channel is alive. Stored on AudioSource
    /// (not AudioDevice) so it survives source list reshuffles and
    /// avoids a parallel-vector lookup on the audio thread.
    std::atomic<float> lastBufferPeak{0.0f};
    /// Discontinuities this source produced, per second (2026-09-09): the
    /// count of frame-to-frame jumps larger than kClickThreshold in its own
    /// output, published once a second by AudioDevice::mixSources and shown
    /// next to the meter in the Audio panel. A "crackle" report is a source
    /// with a number here; a 1-bit speaker legitimately has one (every
    /// toggle is a step), a sampled or synthesised source should read 0.
    /// Measured per source so the user can name the culprit instead of
    /// guessing it by ear — which is how a report blamed the printer for
    /// what was not the printer.
    std::atomic<float> clicksPerSecond{0.0f};
    static constexpr float kClickThreshold = 0.10f;
    // Audio-thread scratch for the tally above (mixSources only).
    float    traceLastL_ = 0.0f, traceLastR_ = 0.0f;
    uint32_t traceJumps_ = 0, traceFrames_ = 0;

    /// Stereo placement for the MONO path, -1 = hard left, 0 = centre,
    /// +1 = hard right. A balance law, not a constant-power one: the
    /// centre position is unity on both channels, so making the bus
    /// stereo did not change the level of any existing mono source.
    /// Ignored by sources that implement `fillAudioBufferStereo`.
    std::atomic<float> pan{0.0f};
};

/// Optional mixin for sources whose synthesis depends on the host audio
/// rate. AudioDevice::addSource auto-calls setSampleRate(actualSampleRate)
/// on any source that also inherits this — defensive against forgetting
/// the explicit call after a hot-plug. Existing call sites keep their
/// manual setSampleRate; the auto path just guarantees the source is
/// configured before the first fillAudioBuffer.
class RateAware
{
public:
    virtual ~RateAware() = default;
    virtual void setSampleRate(uint32_t hz) = 0;
};

#endif // POM2_AUDIO_SOURCE_H
