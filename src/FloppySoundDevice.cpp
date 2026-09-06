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

#include "FloppySoundDevice.h"
#include "CpuClock.h"
#include "Logger.h"

#include "third_party/miniaudio.h"  // IMPLEMENTATION lives in AudioDevice.cpp

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <type_traits>

namespace {
// MAME's floppy samples aren't designed for seamless looping — every
// looping clip (spin_loaded, spin_empty, all 4 seek_*) has a 0.4–2.7%
// amplitude jump between the last and first sample, which produces an
// audible click every loop iteration (~200 ms cadence for spin
// samples). Compensate at load time: blend the last N samples with the
// first N so the wraparound at `len → 0` is continuous in value. After
// preprocessing, playback can use the normal mixLoop without further
// boundary smoothing — at pos approaching len the blended values
// approach data[0], so wrap is seamless.
//
// N = ~3 ms at the sample's native rate (44.1 kHz → 132 frames). Short
// enough that the boundary isn't audibly amplitude-modulated, long
// enough to mask the 1–3 % step jumps.
void applyLoopCrossfade(std::vector<float>& data)
{
    if (data.size() < 8) return;
    const size_t n = data.size();
    const size_t window = std::min<size_t>(132, n / 4);
    for (size_t i = 0; i < window; ++i) {
        const float alpha = static_cast<float>(i + 1) / static_cast<float>(window);
        const size_t k = n - window + i;
        data[k] = data[k] * (1.0f - alpha) + data[i] * alpha;
    }
}
}  // namespace

FloppySoundDevice::FloppySoundDevice()
{
    // Threading-discipline guard, same pin as CassetteDevice's ctor: this
    // field is written by setSampleRate on the UI thread and read lock-free
    // by the audio callback (drainCommands + the two mixers). Reverting it
    // to a plain uint32_t breaks the build here on purpose.
    static_assert(std::is_same_v<decltype(outputSampleRate_),
                                 std::atomic<uint32_t>>,
                  "outputSampleRate_ must be atomic (UI -> audio thread)");
    // Pre-reserve the audio thread's command scratch so drainCommands never
    // allocates in the realtime callback (see its comment).
    cmdScratch_.reserve(64);
    cmdQueue_.reserve(64);
}

bool FloppySoundDevice::loadSamples(const std::string& dir, FormFactor ff)
{
    namespace fs = std::filesystem;
    formFactor_ = ff;
    const char* p = (ff == FormFactor::FF35) ? "35" : "525";
    struct Entry { SampleIdx idx; const char* stem; double nominalMs; };
    static constexpr Entry kSamples[] = {
        { SEEK_2MS,          "seek_2ms",            2.0 },
        { SEEK_6MS,          "seek_6ms",            6.0 },
        { SEEK_12MS,         "seek_12ms",          12.0 },
        { SEEK_20MS,         "seek_20ms",          20.0 },
        { SPIN_EMPTY,        "spin_empty",          0.0 },
        { SPIN_LOADED,       "spin_loaded",         0.0 },
        { SPIN_START_EMPTY,  "spin_start_empty",    0.0 },
        { SPIN_START_LOADED, "spin_start_loaded",   0.0 },
        { SPIN_END,          "spin_end",            0.0 },
        { STEP_1_1,          "step_1_1",            0.0 },
    };

    int loaded = 0;
    for (const auto& e : kSamples) {
        const fs::path full =
            fs::path(dir) / (std::string(p) + "_" + e.stem + ".wav");
        Sample s;
        if (loadOneWav(full.string(), s)) {
            s.nominalMs = e.nominalMs;
            // Crossfade looping samples — spin_loaded / spin_empty and
            // the four seek_* clips. spin_start_*, spin_end, step_1_1
            // are one-shots and would lose their natural attack/decay
            // if we blended them.
            const bool isLooping =
                (e.idx == SPIN_EMPTY || e.idx == SPIN_LOADED ||
                 e.idx == SEEK_2MS   || e.idx == SEEK_6MS    ||
                 e.idx == SEEK_12MS  || e.idx == SEEK_20MS);
            if (isLooping) applyLoopCrossfade(s.data);
            samples_[e.idx] = std::move(s);
            ++loaded;
        } else {
            pom2::log().warn("FloppySound",
                std::string("missing sample: ") + full.string());
        }
    }
    const bool allLoaded = (loaded == SAMPLE_COUNT);
    // Release-store publishes the samples_ writes above to the audio thread.
    samplesLoaded_.store(allLoaded, std::memory_order_release);
    if (allLoaded) {
        pom2::log().info("FloppySound",
            std::string("loaded 10 ") + p + "\" samples from " + dir);
    }
    return allLoaded;
}

bool FloppySoundDevice::loadOneWav(const std::string& path, Sample& out)
{
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) return false;

    out.sourceRate = dec.outputSampleRate;
    ma_uint64 totalFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &totalFrames) != MA_SUCCESS
        || totalFrames == 0) {
        ma_decoder_uninit(&dec);
        return false;
    }
    // These are sub-second mechanical effects. A corrupt WAV header used to
    // drive assign(totalFrames) directly and could terminate POM2 through a
    // multi-gigabyte allocation before the decoder discovered truncation.
    constexpr ma_uint64 kMaxSampleFrames = 10ull * 192000ull;
    if (totalFrames > kMaxSampleFrames ||
        totalFrames > static_cast<ma_uint64>(
            std::numeric_limits<size_t>::max() / sizeof(float))) {
        ma_decoder_uninit(&dec);
        return false;
    }
    out.data.assign(static_cast<size_t>(totalFrames), 0.0f);
    ma_uint64 framesRead = 0;
    ma_result r = ma_decoder_read_pcm_frames(&dec, out.data.data(),
                                             totalFrames, &framesRead);
    ma_decoder_uninit(&dec);
    if (r != MA_SUCCESS && r != MA_AT_END) return false;
    out.data.resize(static_cast<size_t>(framesRead));
    return !out.data.empty();
}

void FloppySoundDevice::setSampleRate(uint32_t hz)
{
    if (hz == 0) hz = kAudioSampleRate;
    outputSampleRate_.store(hz, std::memory_order_relaxed);
}

void FloppySoundDevice::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    volume_.store(v, std::memory_order_relaxed);
}

void FloppySoundDevice::setMotorPitch(float p)
{
    if (p < 0.5f) p = 0.5f;
    if (p > 2.0f) p = 2.0f;
    motorPitch_.store(p, std::memory_order_relaxed);
}

void FloppySoundDevice::reset()
{
    std::lock_guard<std::mutex> lk(cmdMtx_);
    cmdQueue_.clear();
    // Note: we don't touch audio-thread state here — that would race the
    // audio thread. Sending MotorOff via the queue is enough to bring
    // the next fillAudioBuffer to silence within ~one buffer.
    cmdQueue_.push_back({CmdKind::MotorOff, false, 0});
}

int FloppySoundDevice::queuedCommandCount() const
{
    std::lock_guard<std::mutex> lk(cmdMtx_);
    return static_cast<int>(cmdQueue_.size());
}

// ─── CPU-thread API ─────────────────────────────────────────────────────

void FloppySoundDevice::motor(bool on, bool withDisk)
{
    if (!samplesLoaded_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(cmdMtx_);
    cmdQueue_.push_back({on ? CmdKind::MotorOn : CmdKind::MotorOff, withDisk, 0});
}

void FloppySoundDevice::step(int /*newTrack*/, uint64_t emuCycles)
{
    if (!samplesLoaded_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(cmdMtx_);
    cmdQueue_.push_back({CmdKind::Step, false, emuCycles});
}

void FloppySoundDevice::click()
{
    if (!samplesLoaded_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(cmdMtx_);
    cmdQueue_.push_back({CmdKind::Click, false, 0});
}

// ─── Audio-thread internals ─────────────────────────────────────────────

int FloppySoundDevice::pickSeekSample(double rateMs)
{
    if (rateMs <= 3.0)  return SEEK_2MS;
    if (rateMs <= 9.0)  return SEEK_6MS;
    if (rateMs <= 15.0) return SEEK_12MS;
    if (rateMs <= 50.0) return SEEK_20MS;
    return SAMPLE_COUNT;     // out of seek range — fall back to a click
}

void FloppySoundDevice::drainCommands()
{
    // `cmdScratch_` is a member, not a local: this runs on the realtime
    // audio callback, where a per-buffer heap alloc/free is the canonical
    // source of underruns. Swapping the (empty, capacity-carrying) scratch
    // in hands the producer a vector that will not allocate for the first
    // ~64 commands either, and the clear() below keeps the capacity for the
    // next callback instead of destroying it. Audio thread only — the swap
    // is the sole point where it meets `cmdMtx_`.
    {
        std::lock_guard<std::mutex> lk(cmdMtx_);
        cmdScratch_.swap(cmdQueue_);
    }
    for (const Cmd& c : cmdScratch_) {
        switch (c.kind) {
        case CmdKind::MotorOn: {
            // A fresh MotorOn cancels any pending wall-clock spin-down.
            pendingMotorOff_ = false;
            if (!audioMotorOn_) {
                audioMotorOn_  = true;
                audioWithDisk_ = c.withDisk;
                startIdx_      = c.withDisk ? SPIN_START_LOADED : SPIN_START_EMPTY;
                startPos_      = 0.0;
                spinLoopIdx_   = c.withDisk ? SPIN_LOADED : SPIN_EMPTY;
                spinLoopPos_   = 0.0;
                // Cancel any pending spin-down.
                endIdx_ = -1;
            } else {
                // Already spinning; just refresh withDisk in case media
                // changed.
                audioWithDisk_ = c.withDisk;
                spinLoopIdx_   = c.withDisk ? SPIN_LOADED : SPIN_EMPTY;
            }
            break;
        }
        case CmdKind::MotorOff: {
            // Don't immediately silence the loop — schedule a wall-clock
            // hold-off so the user actually hears the motor at turbo
            // speeds where the controller's emulated 1-sec delay is too
            // short to play any loop samples.
            if (audioMotorOn_ && !pendingMotorOff_) {
                const double sr = static_cast<double>(hostSampleRate());
                pendingMotorOff_  = true;
                motorOffDeadline_ =
                    audioFrameCounter_.load(std::memory_order_relaxed) +
                    static_cast<uint64_t>(kMotorOffHoldMs * sr / 1000.0);
            }
            break;
        }
        case CmdKind::Step: {
            const uint64_t now = audioFrameCounter_.load(std::memory_order_relaxed);
            const double   sr  = static_cast<double>(hostSampleRate());
            // Inter-step gap in **emulated** CPU time — mirrors MAME's
            // `(now - m_last_step_time).as_double() * 1000` in
            // floppy_sound_device::step (floppy.cpp ~lines 1532-1540).
            // Audio-frame deltas would be wrong: under POM2's disk turbo
            // (~60× emulated speed) all 80 phase-sweep steps land in one
            // audio buffer, so audioFrameCounter_ shows gap=0 for every
            // step after the first, which classified them all as single
            // STEP_1_1 clicks (stepPos_=0 reset per event → user heard
            // step_1_1's attack repeated buffer after buffer, "haché").
            double gapMs = 1e9;
            if (anyStepSeen_) {
                if (c.emuCycles > lastStepCycle_) {
                    const uint64_t dc = c.emuCycles - lastStepCycle_;
                    gapMs = static_cast<double>(dc) * 1000.0 /
                            static_cast<double>(POM2_CPU_CLOCK_HZ);
                } else {
                    // c.emuCycles == lastStepCycle_ (multiple events
                    // queued at the same emulated cycle — edge case) or
                    // backwards (defensive). Treat as a 0-cycle burst;
                    // the floor below clamps to 1 ms → SEEK_2MS @ pitch
                    // 2.0, the fastest seek class.
                    gapMs = 0.0;
                }
            }
            anyStepSeen_   = true;
            // Floor at 1 ms — defends mixLoop against INF rate (`pos +=
            // INF` would spin forever, INF - len == INF in IEEE 754) and
            // keeps pitch in [1, ~2] for SEEK_2MS.
            if (gapMs < 1.0) gapMs = 1.0;
            lastStepCycle_ = c.emuCycles;
            // Decision: rapid steps → seek mode; otherwise single click.
            if (gapMs <= kSeekJoinMs) {
                const int seekIdx = pickSeekSample(gapMs);
                if (seekIdx < SAMPLE_COUNT
                    && !samples_[seekIdx].data.empty()
                    && samples_[seekIdx].nominalMs > 0.0) {
                    audioInSeek_   = true;
                    if (stepSampleIdx_ != seekIdx) {
                        // Sample switched (e.g. step rate accelerated) —
                        // reset cursor so the new sample starts cleanly.
                        stepSampleIdx_ = seekIdx;
                        stepPos_       = 0.0;
                    }
                    stepPitch_ = samples_[seekIdx].nominalMs / gapMs;
                    // Belt-and-braces: never let pitch produce a non-
                    // finite rate. mixLoop guards too, but clamping here
                    // keeps the seek loop musical.
                    if (!(stepPitch_ > 0.0) || stepPitch_ > 4.0) stepPitch_ = 1.0;
                    seekTimeoutFrame_ =
                        now + static_cast<uint64_t>(kSeekTimeoutMs * sr / 1000.0);
                    break;
                }
                // Fall through to single-step on out-of-range rate.
            }
            // Single step click.
            audioInSeek_   = false;
            stepSampleIdx_ = STEP_1_1;
            stepPos_       = 0.0;
            stepPitch_     = 1.0;
            break;
        }
        case CmdKind::Click: {
            clickActive_ = true;
            clickPos_    = 0.0;
            break;
        }
        }
    }
    // Keep the capacity, drop the contents (no free on the audio thread).
    cmdScratch_.clear();
}

void FloppySoundDevice::mixOneShot(int sampleIdx, double& pos, double pitch,
                                   float* out, int frames, float gain)
{
    if (sampleIdx < 0 || sampleIdx >= SAMPLE_COUNT) return;
    const Sample& s = samples_[sampleIdx];
    if (s.data.empty()) return;
    const double rate = pitch * static_cast<double>(s.sourceRate)
                              / static_cast<double>(hostSampleRate());
    // Defensive: a non-finite rate (NaN/INF) from pathological pitch
    // values would hang the wrap-loop in mixLoop. Bail silently — the
    // caller's state machine will recover on its next event.
    if (!(rate > 0.0) || rate > 1e6) { pos = static_cast<double>(s.data.size()); return; }
    const size_t n = s.data.size();
    for (int i = 0; i < frames; ++i) {
        if (pos < 0.0 || pos >= static_cast<double>(n - 1)) {
            pos = static_cast<double>(n);     // mark done
            break;
        }
        const size_t k = static_cast<size_t>(pos);
        const float  f = static_cast<float>(pos - static_cast<double>(k));
        const float  v = s.data[k] + f * (s.data[k + 1] - s.data[k]);
        out[i] += v * gain;
        pos += rate;
    }
}

void FloppySoundDevice::mixLoop(int sampleIdx, double& pos, double pitch,
                                float* out, int frames, float gain)
{
    if (sampleIdx < 0 || sampleIdx >= SAMPLE_COUNT) return;
    const Sample& s = samples_[sampleIdx];
    if (s.data.size() < 2) return;
    const double rate = pitch * static_cast<double>(s.sourceRate)
                              / static_cast<double>(hostSampleRate());
    // Defensive: see mixOneShot. INF rate would make the wrap-loop spin
    // forever (INF - len == INF in IEEE 754).
    if (!(rate > 0.0) || rate > 1e6) return;
    const double len  = static_cast<double>(s.data.size());
    for (int i = 0; i < frames; ++i) {
        while (pos >= len) pos -= len;
        while (pos < 0.0)  pos += len;
        const size_t k = static_cast<size_t>(pos);
        const size_t k1 = (k + 1 >= s.data.size()) ? 0 : k + 1;
        const float  f = static_cast<float>(pos - static_cast<double>(k));
        const float  v = s.data[k] + f * (s.data[k1] - s.data[k]);
        out[i] += v * gain;
        pos += rate;
    }
}

void FloppySoundDevice::fillAudioBuffer(float* output, int frameCount)
{
    if (frameCount <= 0) return;
    // AudioDevice::mixSources zeroes the temp buffer before calling each
    // source, so we mix additively into a zero-initialised window.

    drainCommands();

    if (!samplesLoaded_.load(std::memory_order_acquire) ||
        muted_.load(std::memory_order_relaxed)) {
        // Still advance the frame counter so step-rate measurements stay
        // consistent across mute toggles.
        audioFrameCounter_.fetch_add(static_cast<uint64_t>(frameCount),
                                     std::memory_order_relaxed);
        return;
    }

    const float gain = volume_.load(std::memory_order_relaxed);

    // ── Wall-clock motor-off hold-off ───────────────────────────────────
    // When the controller fires MotorOff at turbo speed, we defer the
    // audible transition (silence the loop + start spin_end) until
    // kMotorOffHoldMs of wall-clock audio has elapsed. A fresh MotorOn
    // arriving in the meantime cancels the pending transition (drainCommands
    // already clears pendingMotorOff_).
    if (pendingMotorOff_) {
        const uint64_t now =
            audioFrameCounter_.load(std::memory_order_relaxed);
        if (now >= motorOffDeadline_) {
            pendingMotorOff_ = false;
            audioMotorOn_    = false;
            spinLoopIdx_     = -1;
            startIdx_        = -1;
            endIdx_          = SPIN_END;
            endPos_          = 0.0;
        }
    }

    // ── Motor: start one-shot → loop steady-state → end one-shot ────────
    // start_loaded plays first; once exhausted, switch to spin_loaded
    // loop. spin_end plays on motor-off. motorPitch_ shifts all three
    // up for //c / //c+ profiles to approximate the Sony drive's
    // faster mechanism.
    const double motorPitch =
        static_cast<double>(motorPitch_.load(std::memory_order_relaxed));
    if (startIdx_ >= 0) {
        const size_t startLen = samples_[startIdx_].data.size();
        if (startPos_ >= static_cast<double>(startLen)) {
            startIdx_ = -1;
        } else {
            mixOneShot(startIdx_, startPos_, motorPitch, output, frameCount, gain);
        }
    } else if (spinLoopIdx_ >= 0 && audioMotorOn_) {
        mixLoop(spinLoopIdx_, spinLoopPos_, motorPitch, output, frameCount, gain);
    }

    if (endIdx_ >= 0) {
        const size_t endLen = samples_[endIdx_].data.size();
        if (endPos_ >= static_cast<double>(endLen)) {
            endIdx_ = -1;
        } else {
            mixOneShot(endIdx_, endPos_, motorPitch, output, frameCount, gain);
        }
    }

    // ── Step / seek ─────────────────────────────────────────────────────
    const uint64_t nowStart =
        audioFrameCounter_.load(std::memory_order_relaxed);
    const uint64_t nowEnd = nowStart + static_cast<uint64_t>(frameCount);
    if (audioInSeek_ && nowEnd >= seekTimeoutFrame_) {
        // Seek window ended mid-buffer — terminate seek and fire a final
        // step click for the "landing" sound.
        audioInSeek_   = false;
        stepSampleIdx_ = STEP_1_1;
        stepPos_       = 0.0;
        stepPitch_     = 1.0;
    }
    if (stepSampleIdx_ >= 0) {
        const Sample& s = samples_[stepSampleIdx_];
        if (audioInSeek_ && stepSampleIdx_ >= SEEK_2MS && stepSampleIdx_ <= SEEK_20MS) {
            // Seek samples are also looped — mid-seek they keep ticking.
            mixLoop(stepSampleIdx_, stepPos_, stepPitch_, output, frameCount, gain * 0.9f);
        } else if (stepPos_ < static_cast<double>(s.data.size())) {
            mixOneShot(stepSampleIdx_, stepPos_, stepPitch_,
                       output, frameCount, gain * 0.9f);
        } else {
            stepSampleIdx_ = -1;
        }
    }

    // ── Insert/eject click ──────────────────────────────────────────────
    if (clickActive_) {
        const size_t len = samples_[STEP_1_1].data.size();
        if (clickPos_ >= static_cast<double>(len)) {
            clickActive_ = false;
        } else {
            mixOneShot(STEP_1_1, clickPos_, 1.0, output, frameCount, gain * 1.1f);
        }
    }

    audioFrameCounter_.fetch_add(static_cast<uint64_t>(frameCount),
                                 std::memory_order_relaxed);
}
