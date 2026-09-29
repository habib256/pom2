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

// PrinterSoundDevice — mechanical noise of a 9-pin dot-matrix printer: the
// head buzz, the carriage-return sweep and the platen motor.
//
// SYNTHESISED, not sampled — and unlike every other sound in POM2, that is
// not a fallback. Port of the model in mikedaley/web-a2e's
// `src/js/printer/printer-sound.js` (MIT, Shawn Bullock), which ships no audio
// assets at all: there is no free ImageWriter sample set the way MAME provides
// one for floppy drives, so nothing exists to load. POM2 keeps
// `roms/floppy_samples/` for the drives and generates the printer instead.
//
// ── The model, and why it is grains ───────────────────────────────────────
//
// A dot-matrix impact is a short broadband NOISE click, not a tone: spectral
// work on these printers puts the head energy near the basic printing
// frequency (~900-1000 Hz) with a broad skirt to ~5 kHz and no clean
// fundamental. So every voice here is bandpassed noise with a wide Q — using
// an oscillator instead is what makes a printer emulation "sing" rather than
// clack.
//
// One GRAIN is scheduled per printed character (11 ms) or line feed (40 ms),
// and the grains are spaced along the audio timeline rather than all starting
// at the instant the event arrived. That spacing is the whole trick: at print
// rate the grains overlap and fuse into the familiar continuous buzz, while a
// lone character is heard as one tick. Density of print therefore drives the
// texture for free.
//
// ── Adapting a Web Audio design to a pull mixer ───────────────────────────
//
// The reference schedules grains on a `AudioContext` timeline. POM2's mixer
// pulls a mono float buffer from `fillAudioBuffer`, so the timeline becomes an
// audio FRAME COUNTER the audio thread advances and the UI thread reads when
// it stamps a new grain — exactly the arrangement `FloppySoundDevice`
// documents for its step cadence.
//
// The grain cursor is capped `kMaxAheadSeconds` in front of the clock. A dense
// burst (a full-black screen dump) then simply THINS — grains past the window
// are dropped — instead of the cursor running away and leaving the printer
// silent for seconds afterwards. That cap is load-bearing, not a nicety.

#ifndef POM2_PRINTER_SOUND_DEVICE_H
#define POM2_PRINTER_SOUND_DEVICE_H

#include "AudioSource.h"
#include "PrinterSoundSink.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace pom2 {

class PrinterSoundDevice : public AudioSource, public RateAware,
                           public PrinterSoundSink
{
public:
    PrinterSoundDevice();
    ~PrinterSoundDevice() override = default;

    // ── AudioSource / RateAware ───────────────────────────────────────────
    void setSampleRate(uint32_t hz) override;
    void fillAudioBuffer(float* output, int frameCount) override;

    void  setVolume(float v);
    float volume() const { return volume_.load(std::memory_order_relaxed); }
    void  setMuted(bool m) { muted_.store(m, std::memory_order_relaxed); }
    bool  muted() const { return muted_.load(std::memory_order_relaxed); }

    // ── PrinterSoundSink (UI thread) ──────────────────────────────────────
    void strike(int pins) override;
    void carriageReturn(double inches) override;
    void paperFeed(double inches) override;
    void power(bool on) override;

    /// Virtual ][ "Matrix Printer" recording. When `setSampled` is on and
    /// the file decoded, strikes / feeds / returns keep this loop running
    /// instead of scheduling grains. The recording is commercial and lives
    /// only in the local `roms/virtual_ii_sons/` tree.
    bool loadSample(const std::string& wavPath);
    bool sampleReady() const { return loopReady_.load(std::memory_order_acquire); }
    void setSampled(bool on);
    bool sampled() const { return useLoop_.load(std::memory_order_acquire); }

    /// Grains currently scheduled or sounding. Exposed for the test, which
    /// has no ears — it is how "a burst thins instead of running away" is
    /// asserted.
    int activeGrains() const;

private:
    /// Direct-form-I biquad. Two per grain (bandpass then highpass), which is
    /// what turns white noise into an impact rather than a hiss.
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        void  bandpass(double f0, double q, double fs);
        void  highpass(double f0, double fs);
        float run(float x)
        {
            const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
    };

    struct Grain {
        uint64_t start = 0;      ///< audio frame it begins on
        uint64_t end   = 0;      ///< and ends on
        Biquad   bp, hp;
        float    peak  = 0.0f;
        float    env   = 0.0f;
        float    attackStep = 0.0f;
        float    decayK = 0.0f;
        /// Explicit phase, NOT `env < peak`. Every grain shape here has an
        /// attack step larger than one decay step, so inferring the phase from
        /// the envelope made it oscillate two frames below the peak forever:
        /// each decay step was undone by the next attack step, and the grain
        /// became a flat-top burst hard-cut at `end` instead of the documented
        /// exponential decay.
        bool     attacking = true;
        bool     active = false;
    };

    /// Ceiling on simultaneous voices. Past this a new grain is dropped —
    /// the same thinning the cursor cap produces, from the other direction.
    static constexpr int kMaxGrains = 48;

    void  schedule(double durSeconds, double freqHz, double q, double hpHz,
                   double peak, double spacingSeconds, double jitterHz);
    void  armLoop(double seconds);
    void  mixLoop(float* output, int frameCount, uint64_t base, float vol);
    float noise();

    std::vector<float> loop_;
    uint32_t           loopRate_ = 44100;
    double             loopPos_  = 0.0;   ///< audio thread only
    float              loopEnv_  = 0.0f;  ///< audio thread only: fade in/out gain
    std::atomic<bool>     loopReady_{false};
    std::atomic<bool>     useLoop_{false};
    std::atomic<uint64_t> loopUntil_{0};

    uint32_t sampleRate_ = 44100;

    mutable std::mutex mtx_;          ///< guards the pool and the cursor
    Grain    grains_[kMaxGrains];
    uint64_t nextGrainFrame_ = 0;     ///< the scheduling cursor
    bool     powered_        = true;

    /// Advanced by the audio thread, read by the UI thread when it stamps a
    /// grain. Same pattern as FloppySoundDevice's audioFrameCounter_.
    std::atomic<uint64_t> frameCounter_{0};

    /// Used by BOTH threads — grain jitter when scheduling, and the noise
    /// source when filling — but every use is inside `mtx_`, which is what
    /// makes that safe. Do not "optimise" the lock out of fillAudioBuffer
    /// without giving the audio thread its own generator.
    uint32_t rng_ = 0x1234567u;

    std::atomic<float> volume_{0.35f};
    std::atomic<bool>  muted_{false};
};

} // namespace pom2

#endif // POM2_PRINTER_SOUND_DEVICE_H
