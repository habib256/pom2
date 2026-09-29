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
#include <cstring>
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
// Make a recording loop-clean: fold its first `window` frames into its last
// `window` frames, then DROP the head, so the loop runs [window, n) and the
// wrap lands on frame `window` — the frame that naturally follows the
// blended tail's end (≈ frame window-1). The previous form blended the tail
// toward frame window-1 and then wrapped to frame 0, which only moved the
// discontinuity: 525_spin_loaded still ticked once per 200 ms revolution
// (measured 0.0098 full-scale at the wrap, 3× the sample's own largest
// step), which is what a spinning motor sounded like under a file manager.
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
    data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(window));
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

bool FloppySoundDevice::loadViiFile(const std::string& dir, const char* rel,
                                    Sample& out, bool loop)
{
    namespace fs = std::filesystem;
    const fs::path primary = fs::path(dir) / rel;
    bool ok = loadOneWav(primary.string(), out);
    if (!ok) {
        // The bundle's lid / arm / power takes are AIFF. A `.wav` sibling
        // is what a test writes, and what the USB copy already has next
        // to the headerless `.raw` files.
        std::string alt(rel);
        constexpr const char* kAiff = ".aiff";
        const size_t n = std::strlen(kAiff);
        if (alt.size() >= n && alt.compare(alt.size() - n, n, kAiff) == 0) {
            alt.replace(alt.size() - n, n, ".wav");
            ok = loadOneWav((fs::path(dir) / alt).string(), out);
        }
    }
    if (!ok) return false;
    if (loop) applyLoopCrossfade(out.data);
    return !out.data.empty();
}

bool FloppySoundDevice::loadVirtualII(const std::string& dir)
{
    // Drop the ready flag before replacing the vectors. The callback
    // acquire-loads it before touching `vii_`, same publish as loadSamples.
    viiReady_.store(false, std::memory_order_release);
    struct Entry { ViiIdx idx; const char* rel; bool loop; bool required; };
    static constexpr Entry kFiles[] = {
        { VII_ROTATION, "lecteur/Disk Rotation.wav",  true,  true  },
        { VII_BOOT,     "lecteur/Boot.wav",           false, false },
        { VII_ARM,      "lecteur/Move arm.aiff",      false, true  },
        { VII_INSERT,   "lecteur/Disk Insertion.aiff",false, true  },
        { VII_EJECT,    "lecteur/Disk Removal.aiff",  false, true  },
        { VII_IOERR,    "lecteur/I-O Error.wav",      false, false },
        { VII_POP_ON,   "interface/PopOn2.aiff",      false, false },
        { VII_POP_OFF,  "interface/PopOff2.aiff",     false, false },
    };
    int loaded = 0;
    bool requiredOk = true;
    for (const auto& e : kFiles) {
        Sample s;
        if (loadViiFile(dir, e.rel, s, e.loop)) {
            vii_[e.idx] = std::move(s);
            ++loaded;
        } else {
            vii_[e.idx] = Sample{};
            if (e.required) requiredOk = false;
            pom2::log().warn("FloppySound",
                std::string("Virtual ][ missing: ") + dir + "/" + e.rel);
        }
    }
    viiReady_.store(requiredOk, std::memory_order_release);
    if (requiredOk) {
        pom2::log().info("FloppySound",
            "loaded Virtual ][ set (" + std::to_string(loaded) +
            " files) from " + dir);
    }
    return requiredOk;
}

void FloppySoundDevice::setBank(Bank b)
{
    if (bank_.load(std::memory_order_relaxed) == b) return;
    bank_.store(b, std::memory_order_release);
    const uint32_t bit = (b == Bank::VirtualII) ? 1u : 0u;
    uint32_t cur = bankGen_.load(std::memory_order_relaxed);
    while (!bankGen_.compare_exchange_weak(cur, ((cur | 1u) + 1u) | bit,
                                           std::memory_order_acq_rel)) {
    }
}

void FloppySoundDevice::powerSwitch()
{
    if (bank_.load(std::memory_order_acquire) != Bank::VirtualII) return;
    if (!viiReady_.load(std::memory_order_acquire)) return;
    if (vii_[VII_POP_ON].data.empty()) return;
    // First cold boot is a machine that was off: PopOn only. A later one
    // is the user hitting Cold boot on a running machine, so the switch
    // goes off and then on. The audio thread sequences them — queueing
    // both in one drain must not let PopOn cut PopOff off at frame 0.
    const bool cycle = powerLatched_.exchange(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(cmdMtx_);
    if (cycle && !vii_[VII_POP_OFF].data.empty())
        pushCommandLocked({CmdKind::Chassis, false, 0});
    pushCommandLocked({CmdKind::Chassis, true, 0});
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

void FloppySoundDevice::setCpuClock(double hz)
{
    if (hz > 0.0) cpuClockHz_.store(hz, std::memory_order_relaxed);
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

void FloppySoundDevice::pushCommandLocked(const Cmd& c)
{
    // Bounded queue (see kMaxCommands). Drop the OLDEST half-buffer's worth
    // in one go rather than one command per push: erase(begin()) is O(n) on
    // a vector, so a per-push shift at the cap would turn every enqueue into
    // a 4096-element memmove on the CPU thread.
    if (cmdQueue_.size() >= kMaxCommands) {
        const size_t drop = kMaxCommands / 4;
        cmdQueue_.erase(cmdQueue_.begin(),
                        cmdQueue_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    cmdQueue_.push_back(c);
}

int FloppySoundDevice::queuedCommandCount() const
{
    std::lock_guard<std::mutex> lk(cmdMtx_);
    return static_cast<int>(cmdQueue_.size());
}

// ─── CPU-thread API ─────────────────────────────────────────────────────

void FloppySoundDevice::motor(bool on, bool withDisk)
{
    if (!isLoaded()) return;
    std::lock_guard<std::mutex> lk(cmdMtx_);
    pushCommandLocked({on ? CmdKind::MotorOn : CmdKind::MotorOff, withDisk, 0});
}

void FloppySoundDevice::step(int /*newTrack*/, uint64_t emuCycles)
{
    if (!isLoaded()) return;
    std::lock_guard<std::mutex> lk(cmdMtx_);
    pushCommandLocked({CmdKind::Step, false, emuCycles});
}

void FloppySoundDevice::click()
{
    if (!isLoaded()) return;
    std::lock_guard<std::mutex> lk(cmdMtx_);
    if (bank_.load(std::memory_order_acquire) == Bank::VirtualII)
        pushCommandLocked({CmdKind::Latch, true, 0});
    else
        pushCommandLocked({CmdKind::Click, false, 0});
}

void FloppySoundDevice::latch(bool inserting)
{
    if (bank_.load(std::memory_order_acquire) == Bank::VirtualII) {
        if (!viiReady_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lk(cmdMtx_);
        pushCommandLocked({CmdKind::Latch, inserting, 0});
        return;
    }
    // MAME: 3.5" inserts and ejects have always clicked. 5.25" ones have
    // not — DiskIICard keeps startup restore silent, and a latch() from
    // the user mount path must not grow a new thunk on the default bank.
    if (formFactor_ != FormFactor::FF35) return;
    click();
}

void FloppySoundDevice::ioError()
{
    if (bank_.load(std::memory_order_acquire) != Bank::VirtualII) return;
    if (!viiReady_.load(std::memory_order_acquire)) return;
    if (vii_[VII_IOERR].data.empty()) return;
    std::lock_guard<std::mutex> lk(cmdMtx_);
    pushCommandLocked({CmdKind::IoError, false, 0});
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
    const bool vii = audioVii_;
    for (const Cmd& c : cmdScratch_) {
        if (vii) { drainVirtualII(c); continue; }
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
                    // Live clock, not the compile-time NTSC constant: the
                    // stamps come from the emulated CPU and a PAL profile
                    // runs it at 1.0156 MHz. setVideoStandard pushes the
                    // value, the same fan-out the speaker and cassette get.
                    gapMs = static_cast<double>(dc) * 1000.0 /
                            cpuClockHz_.load(std::memory_order_relaxed);
                } else if (c.emuCycles == lastStepCycle_) {
                    // Two events at the same emulated cycle — a real burst.
                    // The floor below clamps to 1 ms → SEEK_2MS @ pitch 2.0,
                    // the fastest seek class.
                    gapMs = 0.0;
                } else {
                    // BACKWARDS. Only a time jump does that — a rewind or a
                    // snapshot restore, and `noteTimeJump` re-bases the
                    // speaker and the deck but not this device (the drive
                    // keeps spinning across one, so it must not be reset).
                    // Reading the stamp difference as a 0-cycle burst turned
                    // the first isolated head move after every rewind into a
                    // 100 ms SEEK_2MS buzz (bug hunt #10). It is the first
                    // step of a new timeline: no measurable gap, one click.
                    gapMs = 1e9;
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
                        // Sample switched (e.g. step rate accelerated, or
                        // a click was playing) — the old voice fades out
                        // while the new sample starts from its head.
                        retireStepVoice();
                        stepSampleIdx_ = seekIdx;
                        stepPos_       = 0.0;
                        attackLeft_    = kFadeFrames;
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
            // Single step click. Whatever was playing — a seek loop, or the
            // previous click still decaying — fades out under it.
            retireStepVoice();
            audioInSeek_   = false;
            stepSampleIdx_ = STEP_1_1;
            stepPos_       = 0.0;
            stepPitch_     = 1.0;
            attackLeft_    = 0;          // a one-shot carries its own attack
            break;
        }
        case CmdKind::Click: {
            clickActive_ = true;
            clickPos_    = 0.0;
            break;
        }
        case CmdKind::Latch:
        case CmdKind::IoError:
        case CmdKind::Chassis:
            // Queued for the Virtual ][ bank. A bank change between the
            // push and this drain drops them — the MAME set has no voice
            // for a lid, an I/O grunt or the power switch.
            break;
        }
    }
    // Keep the capacity, drop the contents (no free on the audio thread).
    cmdScratch_.clear();
}

void FloppySoundDevice::retireStepVoice()
{
    if (stepSampleIdx_ < 0) return;
    const Sample& s = samples_[stepSampleIdx_];
    const bool loop = audioInSeek_ && stepSampleIdx_ >= SEEK_2MS && stepSampleIdx_ <= SEEK_20MS;
    if (!loop && stepPos_ >= static_cast<double>(s.data.size())) return;   // already silent
    fadeIdx_   = stepSampleIdx_;
    fadePos_   = stepPos_;
    fadePitch_ = stepPitch_;
    fadeLoop_  = loop;
    fadeLeft_  = kFadeFrames;
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

void FloppySoundDevice::clearVoices()
{
    audioMotorOn_ = false;
    audioWithDisk_ = false;
    pendingMotorOff_ = false;
    startIdx_ = -1;
    startPos_ = 0.0;
    spinLoopIdx_ = -1;
    spinLoopPos_ = 0.0;
    endIdx_ = -1;
    endPos_ = 0.0;
    stepSampleIdx_ = -1;
    stepPos_ = 0.0;
    stepPitch_ = 1.0;
    audioInSeek_ = false;
    anyStepSeen_ = false;
    lastStepCycle_ = 0;
    fadeIdx_ = -1;
    fadeLeft_ = 0;
    attackLeft_ = 0;
    clickActive_ = false;
    clickPos_ = 0.0;
    viiMotor_ = false;
    viiRotPos_ = 0.0;
    viiRotFade_ = 0;
    viiRotFadePos_ = 0.0;
    viiBoot_ = false;
    viiBootPos_ = 0.0;
    viiArm_ = false;
    viiArmPos_ = 0.0;
    viiDoor_ = false;
    viiDoorIdx_ = -1;
    viiDoorPos_ = 0.0;
    viiErr_ = false;
    viiErrPos_ = 0.0;
    viiChassis_ = false;
    viiChassisIdx_ = -1;
    viiChassisPos_ = 0.0;
    viiPopOnAfter_ = false;
}

// A phase sweep is a few milliseconds; the arm recording is 413 ms. Restarting
// it on every phase would stack the attack. A new movement after this gap
// (the head actually stopped) starts the take over.
static constexpr double kViiArmJoinMs = 120.0;

void FloppySoundDevice::drainVirtualII(const Cmd& c)
{
    switch (c.kind) {
    case CmdKind::MotorOn:
        pendingMotorOff_ = false;
        viiRotFade_ = 0;
        if (!viiMotor_) {
            viiMotor_ = true;
            audioMotorOn_ = true;
            viiRotPos_ = 0.0;
            if (!vii_[VII_BOOT].data.empty()) {
                viiBoot_ = true;
                viiBootPos_ = 0.0;
            }
        }
        audioWithDisk_ = c.withDisk;
        break;
    case CmdKind::MotorOff:
        if (viiMotor_ && !pendingMotorOff_) {
            const double sr = static_cast<double>(hostSampleRate());
            pendingMotorOff_ = true;
            motorOffDeadline_ =
                audioFrameCounter_.load(std::memory_order_relaxed) +
                static_cast<uint64_t>(kMotorOffHoldMs * sr / 1000.0);
        }
        break;
    case CmdKind::Step: {
        // Same emulated-time gap the MAME path uses. Under disk turbo a
        // whole phase sweep lands in one audio buffer; wall-clock gaps
        // would all read as zero and retrigger the arm every step.
        double gapMs = 1e9;
        if (anyStepSeen_) {
            if (c.emuCycles > lastStepCycle_) {
                gapMs = static_cast<double>(c.emuCycles - lastStepCycle_) * 1000.0 /
                        cpuClockHz_.load(std::memory_order_relaxed);
            } else if (c.emuCycles == lastStepCycle_) {
                gapMs = 0.0;
            }
        }
        anyStepSeen_ = true;
        if (gapMs < 1.0) gapMs = 1.0;
        lastStepCycle_ = c.emuCycles;
        const bool armPlaying = viiArm_ &&
            viiArmPos_ + 1.0 < static_cast<double>(vii_[VII_ARM].data.size());
        if (armPlaying && gapMs <= kViiArmJoinMs) break;
        viiArm_ = true;
        viiArmPos_ = 0.0;
        break;
    }
    case CmdKind::Click:
    case CmdKind::Latch: {
        const int idx = c.withDisk ? VII_INSERT : VII_EJECT;
        // A bare Click (the old undifferentiated thunk) is a lid close.
        const int use = (c.kind == CmdKind::Click) ? VII_INSERT : idx;
        if (vii_[use].data.empty()) break;
        viiDoor_ = true;
        viiDoorIdx_ = use;
        viiDoorPos_ = 0.0;
        break;
    }
    case CmdKind::IoError: {
        const Sample& s = vii_[VII_IOERR];
        if (s.data.empty()) break;
        // A 512-byte failure is one event. A retry storm that re-enters
        // before the grunt has decayed must not machine-gun it.
        if (viiErr_ && viiErrPos_ < static_cast<double>(s.data.size()) * 0.70)
            break;
        viiErr_ = true;
        viiErrPos_ = 0.0;
        break;
    }
    case CmdKind::Chassis:
        if (!c.withDisk) {
            if (vii_[VII_POP_OFF].data.empty()) break;
            viiChassis_ = true;
            viiChassisIdx_ = VII_POP_OFF;
            viiChassisPos_ = 0.0;
            viiPopOnAfter_ = false;
        } else if (viiChassis_ && viiChassisIdx_ == VII_POP_OFF) {
            viiPopOnAfter_ = true;
        } else if (!vii_[VII_POP_ON].data.empty()) {
            viiChassis_ = true;
            viiChassisIdx_ = VII_POP_ON;
            viiChassisPos_ = 0.0;
            viiPopOnAfter_ = false;
        }
        break;
    }
}

void FloppySoundDevice::mixVii(int idx, double& pos, double pitch, bool loop,
                               float* out, int frames, float gain)
{
    if (idx < 0 || idx >= VII_COUNT) return;
    const Sample& s = vii_[idx];
    if (s.data.size() < 2) return;
    const double rate = pitch * static_cast<double>(s.sourceRate)
                              / static_cast<double>(hostSampleRate());
    if (!(rate > 0.0) || rate > 1e6) {
        if (!loop) pos = static_cast<double>(s.data.size());
        return;
    }
    const double len = static_cast<double>(s.data.size());
    for (int i = 0; i < frames; ++i) {
        if (!loop && pos >= len - 1.0) {
            pos = len;
            break;
        }
        if (loop) {
            while (pos >= len) pos -= len;
            while (pos < 0.0)  pos += len;
        }
        const size_t k = static_cast<size_t>(pos);
        const size_t k1 = (k + 1 >= s.data.size()) ? 0 : k + 1;
        const float f = static_cast<float>(pos - static_cast<double>(k));
        out[i] += (s.data[k] + f * (s.data[k1] - s.data[k])) * gain;
        pos += rate;
    }
}

void FloppySoundDevice::fillVirtualII(float* output, int frameCount)
{
    drainCommands();
    if (!viiReady_.load(std::memory_order_acquire) ||
        muted_.load(std::memory_order_relaxed)) {
        audioFrameCounter_.fetch_add(static_cast<uint64_t>(frameCount),
                                     std::memory_order_relaxed);
        return;
    }
    const float gain = volume_.load(std::memory_order_relaxed);
    const double motorPitch =
        static_cast<double>(motorPitch_.load(std::memory_order_relaxed));

    if (pendingMotorOff_) {
        const uint64_t now = audioFrameCounter_.load(std::memory_order_relaxed);
        if (now >= motorOffDeadline_) {
            pendingMotorOff_ = false;
            audioMotorOn_ = false;
            viiMotor_ = false;
            viiRotFade_ = kFadeFrames;
            viiRotFadePos_ = viiRotPos_;
        }
    }

    if (viiMotor_) {
        mixVii(VII_ROTATION, viiRotPos_, motorPitch, true,
               output, frameCount, gain * 0.85f);
    } else if (viiRotFade_ > 0) {
        float tmp[512];
        int done = 0;
        const int n = std::min(frameCount, viiRotFade_);
        while (done < n) {
            const int chunk = std::min(n - done, 512);
            std::fill(tmp, tmp + chunk, 0.0f);
            mixVii(VII_ROTATION, viiRotFadePos_, motorPitch, true,
                   tmp, chunk, gain * 0.85f);
            for (int i = 0; i < chunk; ++i) {
                const float ramp = static_cast<float>(viiRotFade_ - i) /
                                   static_cast<float>(kFadeFrames);
                output[done + i] += tmp[i] * ramp;
            }
            viiRotFade_ -= chunk;
            done += chunk;
        }
        if (viiRotFade_ <= 0) viiRotFade_ = 0;
    }

    auto shot = [&](bool& active, int idx, double& pos, double pitch, float g) mutable {
        if (!active) return;
        const size_t len = (idx >= 0 && idx < VII_COUNT) ? vii_[idx].data.size() : 0;
        if (len < 2 || pos >= static_cast<double>(len)) { active = false; return; }
        mixVii(idx, pos, pitch, false, output, frameCount, g);
        if (pos >= static_cast<double>(len)) active = false;
    };
    shot(viiBoot_, VII_BOOT, viiBootPos_, motorPitch, gain);
    shot(viiArm_, VII_ARM, viiArmPos_, 1.0, gain * 0.9f);
    shot(viiDoor_, viiDoorIdx_, viiDoorPos_, 1.0, gain);
    shot(viiErr_, VII_IOERR, viiErrPos_, 1.0, gain);
    shot(viiChassis_, viiChassisIdx_, viiChassisPos_, 1.0, gain * 0.9f);
    // PopOn starts on the next buffer, at the frame after PopOff ended,
    // rather than from frame 0 of the buffer that finished the switch-off.
    if (!viiChassis_ && viiPopOnAfter_ && !vii_[VII_POP_ON].data.empty()) {
        viiPopOnAfter_ = false;
        viiChassis_ = true;
        viiChassisIdx_ = VII_POP_ON;
        viiChassisPos_ = 0.0;
    }

    audioFrameCounter_.fetch_add(static_cast<uint64_t>(frameCount),
                                 std::memory_order_relaxed);
}

void FloppySoundDevice::fillAudioBuffer(float* output, int frameCount)
{
    if (frameCount <= 0) return;
    // AudioDevice::mixSources zeroes the temp buffer before calling each
    // source, so we mix additively into a zero-initialised window.

    const uint32_t gen = bankGen_.load(std::memory_order_acquire);
    if (gen != audioBankGen_) {
        // The motor is STATE, not an event: the controllers push only its
        // edges, so a spindle that was turning must keep turning in the
        // other bank — nothing will send another MotorOn until the guest
        // cycles the drive. Every other voice is a one-shot and is dropped.
        const bool     spinning = audioMotorOn_;
        const bool     withDisk = audioWithDisk_;
        const bool     pending  = pendingMotorOff_;
        const uint64_t deadline = motorOffDeadline_;
        clearVoices();
        audioBankGen_ = gen;
        audioVii_     = (gen & 1u) != 0;
        if (spinning) {
            audioMotorOn_     = true;
            audioWithDisk_    = withDisk;
            pendingMotorOff_  = pending;
            motorOffDeadline_ = deadline;
            if (audioVii_) {
                viiMotor_ = true;      // straight into the loop, no boot chirp
            } else {
                spinLoopIdx_ = withDisk ? SPIN_LOADED : SPIN_EMPTY;   // no spin-up
            }
        }
    }
    if (audioVii_) {
        fillVirtualII(output, frameCount);
        return;
    }

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
    // Retire the spin-up BEFORE the choice, not inside its arm: retiring it
    // inside `if (startIdx_ >= 0)` consumed the whole buffer on the handover
    // — the one-shot no longer played and the `else if` was not reached — so
    // a motor that is running produced 5.3 ms of pure silence at every
    // spin-up (measured: RMS 0.006 -> 0.000 -> 0.006 across three 256-frame
    // buffers), a tick in the middle of a continuous whirr (bug hunt #10).
    if (startIdx_ >= 0 &&
        startPos_ >= static_cast<double>(samples_[startIdx_].data.size())) {
        startIdx_ = -1;
    }
    if (startIdx_ >= 0) {
        mixOneShot(startIdx_, startPos_, motorPitch, output, frameCount, gain);
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
        // step click for the "landing" sound; the loop fades out under it.
        retireStepVoice();
        audioInSeek_   = false;
        stepSampleIdx_ = STEP_1_1;
        stepPos_       = 0.0;
        stepPitch_     = 1.0;
    }
    // The retired voice: its own sample, its own cursor, a linear ramp to
    // silence over kFadeFrames, then gone.
    if (fadeIdx_ >= 0 && fadeLeft_ > 0) {
        const int n = std::min(frameCount, fadeLeft_);
        // Ramp applied per frame: render into a scratch window, then scale.
        float tmp[512];
        int done = 0;
        while (done < n) {
            const int chunk = std::min(n - done, 512);
            std::fill(tmp, tmp + chunk, 0.0f);
            if (fadeLoop_) mixLoop(fadeIdx_, fadePos_, fadePitch_, tmp, chunk, gain * 0.9f);
            else           mixOneShot(fadeIdx_, fadePos_, fadePitch_, tmp, chunk, gain * 0.9f);
            for (int i = 0; i < chunk; ++i) {
                const float ramp = static_cast<float>(fadeLeft_ - i) / static_cast<float>(kFadeFrames);
                output[done + i] += tmp[i] * ramp;
            }
            fadeLeft_ -= chunk;
            done += chunk;
        }
        if (fadeLeft_ <= 0) fadeIdx_ = -1;
    }
    if (stepSampleIdx_ >= 0) {
        const Sample& s = samples_[stepSampleIdx_];
        if (audioInSeek_ && stepSampleIdx_ >= SEEK_2MS && stepSampleIdx_ <= SEEK_20MS) {
            // Seek samples are also looped — mid-seek they keep ticking.
            // A loop that has just started ramps in (see attackLeft_).
            if (attackLeft_ > 0) {
                float tmp[512];
                int done = 0;
                while (done < frameCount) {
                    const int chunk = std::min(frameCount - done, 512);
                    std::fill(tmp, tmp + chunk, 0.0f);
                    mixLoop(stepSampleIdx_, stepPos_, stepPitch_, tmp, chunk, gain * 0.9f);
                    for (int i = 0; i < chunk; ++i) {
                        const float ramp = attackLeft_ > 0
                            ? static_cast<float>(kFadeFrames - attackLeft_) / static_cast<float>(kFadeFrames)
                            : 1.0f;
                        output[done + i] += tmp[i] * ramp;
                        if (attackLeft_ > 0) --attackLeft_;
                    }
                    done += chunk;
                }
            } else {
                mixLoop(stepSampleIdx_, stepPos_, stepPitch_, output, frameCount, gain * 0.9f);
            }
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
