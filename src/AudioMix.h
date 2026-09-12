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

// AudioMix — THE mix law, in one place (bug hunt #19).
//
// There were two mixers. `AudioDevice::mixSources` is the one the emulator
// plays through: per-source scratch, the balance pan law, per-source peak
// meters and click tallies, the master gain ramp, the mono downmix, the
// suspend cut, the post-clamp master meters. And `Pom2Core::pullAudio` —
// the public C++ integration API in `include/pom2/core.hpp`, the one an
// embedder gets its audio from — hand-rolled a SECOND one: speaker plus
// cassette plus ONE Mockingboard, summed and clamped. No pan, no master
// volume, no mute, no mono, no suspend, no meters, and no second sound
// card: plug a Phasor, an Echo+ or a second Mockingboard and the embedder
// heard silence from it while the GUI played it fine.
//
// Divergence like that is not a bug you fix once — every later fix to the
// real mixer (the master ramp in this very hunt, the click tallies in the
// last one) silently missed the API. So the law moved here, unchanged, and
// both call sites now run the same code over their own state.

#ifndef POM2_AUDIO_MIX_H
#define POM2_AUDIO_MIX_H

#include "AudioSource.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pom2 {

/// Interleaved stereo, as the bus has been since 2026-08-01.
inline constexpr int kMixChannels = 2;

/// What the mix law carries from one buffer to the next. One per bus.
/// The three meters are atomics because a UI thread reads them while the
/// audio thread writes them; everything else is audio-thread-only.
struct MixBusState {
    std::vector<float> tmpL, tmpR;      ///< per-source scratch, grown once
    float    masterGainRamp   = 0.0f;
    bool     masterGainPrimed = false;
    float    traceLastL = 0.0f, traceLastR = 0.0f;
    uint32_t traceJumps = 0, traceFrames = 0;
    std::atomic<float> peakL{0.0f};
    std::atomic<float> peakR{0.0f};
    std::atomic<float> clicks{0.0f};
};

/// The master-side settings, resolved once per buffer by the caller.
struct MixParams {
    float    masterGain = 1.0f;   ///< already resolved: muted ? 0 : volume
    bool     mono       = false;
    bool     suspended  = false;
    uint32_t sampleRate = 44100;
};

/// Mix `sources` into interleaved-stereo `output` (clamped to [-1, +1]).
/// The caller owns the lock on `sources`.
inline void mixSourcesInto(float* output, int frameCount,
                           const std::vector<AudioSource*>& sources,
                           const MixParams& p, MixBusState& st)
{
    // Interleaved stereo: 2 floats per frame.
    std::memset(output, 0,
                static_cast<size_t>(frameCount) * kMixChannels * sizeof(float));


    // Machine halted (see setSuspended): emit the silence the memset already
    // wrote and DON'T call the sources at all. Calling them would let the
    // free-running generators keep droning while the CPU that feeds them is
    // parked, and would walk the speaker's cycle cursor past a frozen
    // Memory::cycleCounter. The meters still bleed off on the usual 0.85
    // envelope so the mixer's needles fall to zero instead of freezing at
    // whatever the last live buffer measured.
    if (p.suspended) {
        for (AudioSource* src : sources) {
            const float peak = src->lastBufferPeak.load(std::memory_order_relaxed);
            src->lastBufferPeak.store(peak * 0.85f, std::memory_order_relaxed);
        }
        st.peakL.store(st.peakL.load(std::memory_order_relaxed) * 0.85f,
                           std::memory_order_relaxed);
        st.peakR.store(st.peakR.load(std::memory_order_relaxed) * 0.85f,
                           std::memory_order_relaxed);
        // …and the CRACKLE tally goes to zero with them (bug hunt #19). The
        // early return skips both tally blocks, so `clicksPerSecond` used to
        // freeze at its last live value for the whole pause: the panel then
        // accused a stopped machine of crackling, which is exactly the
        // diagnostic this column exists to make trustworthy. The trace
        // scratch is reset too, so the first buffer after the resume does
        // not book a jump against a pre-pause sample.
        for (AudioSource* src : sources) {
            src->clicksPerSecond.store(0.0f, std::memory_order_relaxed);
            src->traceLastL_ = src->traceLastR_ = 0.0f;
            src->traceJumps_ = src->traceFrames_ = 0;
        }
        st.clicks.store(0.0f, std::memory_order_relaxed);
        st.masterGainPrimed = false;   // the resume re-primes, it does not fade in
        st.traceLastL = st.traceLastR = 0.0f;
        st.traceJumps = st.traceFrames = 0;
        return;
    }

    if (static_cast<int>(st.tmpL.size()) < frameCount) {
        st.tmpL.resize(static_cast<size_t>(frameCount));
        st.tmpR.resize(static_cast<size_t>(frameCount));
    }

    for (AudioSource* src : sources) {
        // Zero the temp buffer before each source. The AudioSource
        // contract is *either* assign (Speaker, Cassette, Mockingboard
        // all do `output[i] = ...`) *or* mix additively starting from
        // silence (FloppySoundDevice). Without this memset, when the
        // sources iterate in order [A, B] and A writes its signal into
        // st.tmpL, an additive source B would see A's samples and add on
        // top — A then gets counted twice in output (A's pass already
        // added them once), producing audible doubling whenever two
        // sources are simultaneously active. Symptom seen during cold
        // boot: speaker bell beep + Disk II spin-up overlapped, giving
        // a "horrible" composite. The cost is one extra memset per
        // source per ~5 ms buffer — negligible.
        std::memset(st.tmpL.data(), 0,
                    static_cast<size_t>(frameCount) * sizeof(float));
        std::memset(st.tmpR.data(), 0,
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

        if (src->fillAudioBufferStereo(st.tmpL.data(), st.tmpR.data(),
                                       frameCount)) {
            // Native stereo: the source owns its placement (Mockingboard
            // AY1/AY2, Phasor AY pairs), so `pan` is deliberately NOT
            // applied here.
            for (int i = 0; i < frameCount; ++i) {
                const float l = st.tmpL[i];
                const float r = st.tmpR[i];
                const float a = std::max(std::fabs(l), std::fabs(r));
                if (a > srcPeak) srcPeak = a;
                output[2 * i]     += l;
                output[2 * i + 1] += r;
            }
        } else {
            src->fillAudioBuffer(st.tmpL.data(), frameCount);
            // Balance law, NOT constant power: centre is unity on both
            // channels. Constant power would have put every existing
            // mono source 3 dB down the day the bus went stereo, which
            // is a silent regression nobody asked for; the Apple speaker
            // has no stereo position to be faithful to anyway.
            const float pan = std::max(-1.0f, std::min(1.0f,
                src->pan.load(std::memory_order_relaxed)));
            const float gL = (pan > 0.0f) ? (1.0f - pan) : 1.0f;
            const float gR = (pan < 0.0f) ? (1.0f + pan) : 1.0f;
            for (int i = 0; i < frameCount; ++i) {
                const float s = st.tmpL[i];
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
                const float l = st.tmpL[i];
                const float r = st.tmpR[i];
                if (std::fabs(l - lastL) > AudioSource::kClickThreshold ||
                    std::fabs(r - lastR) > AudioSource::kClickThreshold)
                    ++jumps;
                lastL = l; lastR = r;
            }
            src->traceLastL_ = lastL; src->traceLastR_ = lastR;
            src->traceJumps_ += jumps;
            src->traceFrames_ += static_cast<uint32_t>(frameCount);
            const uint32_t sr = p.sampleRate > 0 ? p.sampleRate : 44100;
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
    const float masterGain = p.masterGain;
    // …reached by a RAMP, not a step (bug hunt #19): muting or dragging the
    // master slider during a sustained note stepped the whole mix by up to
    // full scale at a buffer boundary. 5 ms: instant to the hand, inaudible
    // to the ear.
    const float masterStep =
        1.0f / (0.005f * static_cast<float>(p.sampleRate > 0
                                                ? p.sampleRate : 44100));
    if (!st.masterGainPrimed) { st.masterGainRamp = masterGain; st.masterGainPrimed = true; }
    const bool mono = p.mono;
    float masterPkL = 0.0f;
    float masterPkR = 0.0f;
    // Master discontinuity tally, folded into the loop that already touches
    // every frame (see AudioDevice.h). Measured POST-clamp, so it counts the
    // steps the OS actually heard — the ones no per-source row can show.
    float    mLastL = st.traceLastL, mLastR = st.traceLastR;
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
        if (st.masterGainRamp < masterGain)
            st.masterGainRamp = std::min(masterGain, st.masterGainRamp + masterStep);
        else if (st.masterGainRamp > masterGain)
            st.masterGainRamp = std::max(masterGain, st.masterGainRamp - masterStep);
        const float cl = std::max(-1.0f, std::min(1.0f, l * st.masterGainRamp));
        const float cr = std::max(-1.0f, std::min(1.0f, r * st.masterGainRamp));
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
    st.traceLastL = mLastL; st.traceLastR = mLastR;
    st.traceJumps += mJumps;
    st.traceFrames += static_cast<uint32_t>(frameCount);
    {
        const uint32_t sr = p.sampleRate > 0 ? p.sampleRate : 44100;
        if (st.traceFrames >= sr) {
            st.clicks.store(
                static_cast<float>(st.traceJumps) * static_cast<float>(sr) /
                    static_cast<float>(st.traceFrames),
                std::memory_order_relaxed);
            st.traceJumps = 0; st.traceFrames = 0;
        }
    }
    const float prevL = st.peakL.load(std::memory_order_relaxed);
    const float prevR = st.peakR.load(std::memory_order_relaxed);
    st.peakL.store(masterPkL > prevL * 0.85f ? masterPkL : prevL * 0.85f,
                       std::memory_order_relaxed);
    st.peakR.store(masterPkR > prevR * 0.85f ? masterPkR : prevR * 0.85f,
                       std::memory_order_relaxed);
}

}  // namespace pom2

#endif  // POM2_AUDIO_MIX_H
