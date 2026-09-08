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

// FloppySoundDevice — mechanical sounds for the Disk II drive (head step,
// motor spin, disk insert/eject click). Port of MAME's
// `src/devices/imagedev/floppy.cpp::floppy_sound_device`, sample-based
// playback driven by the 10 5.25" WAVs from `samples/floppy/` of the MAME
// tree (BSD-3-Clause; see `roms/floppy_samples/README.txt`).
//
// CPU thread → audio thread coupling
// ----------------------------------
// The Disk II card lives on the CPU thread; the audio mixer pulls
// fillAudioBuffer() on miniaudio's callback thread. Coupling is a tiny
// mutex-guarded command queue (motor on/off, step, click). Events are
// rare (<100/s during a full 0→34 seek; ~2/s during typical reads), so
// a lock-free SPSC ring would be over-engineered.
//
// Step / seek decision (MAME parity, floppy.cpp ~lines 2925-3020)
// ---------------------------------------------------------------
//   * First step OR gap > kSeekJoinMs since last step → play `step_1_1`
//     as a single-shot click.
//   * Rapid steps (gap ≤ kSeekJoinMs) → enter seek mode. Pick the seek
//     sample whose nominal cadence best matches the observed step rate
//     (2 / 6 / 12 / 20 ms). Pitch-scale playback so the per-click
//     interval lands on the observed cadence.
//   * After kSeekTimeoutMs without further steps, exit seek and emit one
//     final `step_1_1` to "land" the head.
//
// Time is measured in **audio output frames** (= audioFrameCounter_),
// not CPU cycles. The audio thread advances that counter inside
// fillAudioBuffer(); the CPU thread reads it when timestamping a new
// event. Granularity is one audio buffer (~5 ms at 256-frame buffers),
// which is plenty for ms-scale step/seek classification.
//
// Mixing pipeline (per output frame)
// ----------------------------------
//   sum = spin_start ⨉ one-shot   (motor spin-up)
//       + spin       ⨉ loop       (motor steady-state)
//       + spin_end   ⨉ one-shot   (motor spin-down)
//       + step|seek  ⨉ one-shot   (head movement)
//       + click      ⨉ one-shot   (disk insert/eject)
//   * volume × !muted
//
// All sources mix additively into the AudioDevice's mono float32 stream.

#ifndef POM2_FLOPPY_SOUND_DEVICE_H
#define POM2_FLOPPY_SOUND_DEVICE_H

#include "AudioSource.h"
#include "CpuClock.h"
#include "FloppySoundSink.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class FloppySoundDevice : public AudioSource, public RateAware, public FloppySoundSink
{
public:
    /// 5.25" Disk II is the only form factor POM2 currently emulates; the
    /// 3.5" path is wired (sample set ships in roms/floppy_samples/) so a
    /// future SmartPort / Liron card can opt in by passing FormFactor::FF35.
    enum class FormFactor { FF525, FF35 };

    FloppySoundDevice();
    ~FloppySoundDevice() override = default;

    /// Load the 10 sample WAVs for `ff` from `dir` (typically
    /// `roms/floppy_samples`). Filenames are prefixed `525_` or `35_`
    /// per form factor:
    ///   {prefix}_seek_{2,6,12,20}ms.wav, {prefix}_step_1_1.wav,
    ///   {prefix}_spin_{empty,loaded}.wav,
    ///   {prefix}_spin_start_{empty,loaded}.wav, {prefix}_spin_end.wav
    /// Each must be 16-bit mono PCM (MAME's stock format). Returns true
    /// when every sample loaded; on partial failure the device degrades
    /// gracefully (missing samples just go silent).
    bool loadSamples(const std::string& dir,
                     FormFactor ff = FormFactor::FF525);
    bool isLoaded() const { return samplesLoaded_.load(std::memory_order_acquire); }
    FormFactor formFactor() const { return formFactor_; }

    /// Configure output sample rate. Resamples are computed on the fly
    /// (linear interpolation) so the 44.1 kHz source samples play
    /// natural-pitch regardless of the negotiated device rate.
    void setSampleRate(uint32_t hz) override;

    /// Emulated CPU clock (Hz) that `step()`'s `emuCycles` stamps are
    /// measured against. The step-cadence classifier turns a cycle delta into
    /// milliseconds to pick a seek sample, and it used the compile-time NTSC
    /// constant — 0.7 % wrong on the three PAL profiles, which is inaudible
    /// but is also exactly the mistake `setVideoStandard` fans out to every
    /// other cycle-stamped audio consumer. Defaults to the NTSC nominal.
    void setCpuClock(double hz);

    // ─── CPU-thread API ─────────────────────────────────────────────────
    /// Motor state changed. `withDisk` chooses the loaded vs empty
    /// spin/start sample pair. Called from DiskIICard when the spindle
    /// truly turns on / off (after MODE_DELAY expires for spin-down).
    void motor(bool on, bool withDisk) override;
    /// Head moved. `newTrack` is the destination track (0..34) — we
    /// derive the step rate from inter-call cadence (in **emulated**
    /// CPU cycles, via `emuCycles`), not from the track value, so the
    /// destination is informational only. See FloppySoundSink.h for the
    /// rationale on emulated vs wall-clock timing.
    void step(int newTrack, uint64_t emuCycles) override;
    /// Single-shot "click" for disk insertion / ejection. Uses the
    /// step_1_1 sample at moderate gain.
    void click() override;

    // ─── UI-thread API ──────────────────────────────────────────────────
    void  setVolume(float v);
    float getVolume() const { return volume_.load(std::memory_order_relaxed); }
    void  setMuted (bool m) { muted_.store(m, std::memory_order_relaxed); }
    bool  isMuted () const  { return muted_.load(std::memory_order_relaxed); }

    /// Apply a profile-specific pitch multiplier to all motor samples
    /// (spin_start / spin_loop / spin_end). The MAME samples were
    /// recorded from an original Disk II Shugart-based mechanism
    /// (~1978); the //c and //c+ Sony internal drives are noticeably
    /// faster and higher-pitched, so MainWindow bumps this to ~1.4
    /// when one of those profiles is active. Clamped to [0.5, 2.0].
    /// Step / seek / click samples are unaffected (head stepper noise
    /// is largely the same across Apple 5.25" drives).
    void  setMotorPitch(float p);
    float getMotorPitch() const { return motorPitch_.load(std::memory_order_relaxed); }

    /// Drop all in-flight playback. Called on hard reset / profile switch.
    void reset();

    // ─── AudioSource ────────────────────────────────────────────────────
    void fillAudioBuffer(float* output, int frameCount) override;

    // Diagnostics (used by smoke tests).
    bool   audioMotorOn() const { return audioMotorOn_; }
    bool   audioInSeek () const { return audioInSeek_; }
    int    queuedCommandCount() const;

private:
    // Sample indexing — order is hot-path; keep packed.
    enum SampleIdx {
        SEEK_2MS = 0, SEEK_6MS, SEEK_12MS, SEEK_20MS,
        SPIN_EMPTY, SPIN_LOADED,
        SPIN_START_EMPTY, SPIN_START_LOADED,
        SPIN_END,
        STEP_1_1,
        SAMPLE_COUNT
    };

    struct Sample {
        std::vector<float> data;          // mono float32 at sourceRate
        uint32_t           sourceRate = 44100;
        // For seek samples: the recorded cadence (one click per
        // `nominalMs` ms) drives the pitch-matching scaler. 0 for
        // non-seek samples.
        double nominalMs = 0.0;
    };
    std::array<Sample, SAMPLE_COUNT> samples_{};
    // Atomic: written once by loadSamples() on the main thread, but the audio
    // callback thread is already live (addSource happens before loadSamples),
    // so a release-store here pairs with the callback's acquire-loads to
    // guarantee the samples_ vectors are fully published before the flag flips.
    std::atomic<bool> samplesLoaded_{false};
    FormFactor formFactor_    = FormFactor::FF525;

    // ─── Command queue (CPU → audio) ────────────────────────────────────
    enum class CmdKind : uint8_t { MotorOn, MotorOff, Step, Click };
    struct Cmd {
        CmdKind  kind;
        bool     withDisk;    // valid for MotorOn / MotorOff
        uint64_t emuCycles;   // valid for Step — emulated CPU cycle stamp
    };
    mutable std::mutex cmdMtx_;
    std::vector<Cmd>   cmdQueue_;
    /// Hard cap on the CPU→audio queue. `drainCommands` is the ONLY consumer
    /// and it runs on the miniaudio callback, so on a host with no audio
    /// device (headless, WASM before the first user gesture, a machine whose
    /// output failed to open) nothing ever drains it — while samples still
    /// load unconditionally, so `motor`/`step`/`click` keep pushing. A
    /// DOS 3.3 disk copy is ~80 phase steps per seek; a long session grew
    /// this without bound. 4096 commands is ~40 s of the busiest real seek
    /// traffic and ~64 KB, i.e. far past any audible backlog: past the cap we
    /// drop the OLDEST, because what a listener needs is the head of the
    /// mechanism NOW, not a minute-old spin-up.
    static constexpr size_t kMaxCommands = 4096;
    /// Push under `cmdMtx_`, enforcing `kMaxCommands` by dropping oldest.
    void pushCommandLocked(const Cmd& c);
    /// Audio-thread-only scratch that `drainCommands` swaps `cmdQueue_`
    /// into. A local `std::vector<Cmd>` there allocated on every buffer
    /// that carried a command and freed at the end of the callback — a
    /// malloc/free pair on the realtime thread. Cleared (not destroyed)
    /// after each drain so the capacity survives. Never touched by the
    /// producer threads.
    std::vector<Cmd>   cmdScratch_;

    // ─── Audio-thread state ─────────────────────────────────────────────
    // Frame counter advanced by fillAudioBuffer (host sample rate).
    // Read by CPU thread (atomic) only as a free-running "now"; used
    // here for step-rate classification.
    std::atomic<uint64_t> audioFrameCounter_{0};
    /// Host output rate. Written by `setSampleRate` (main/UI thread, when
    /// AudioDevice negotiates or re-negotiates the device) and read by
    /// every seek/spin cadence computation inside `fillAudioBuffer` on the
    /// audio-callback thread — a cross-thread scalar, hence atomic like
    /// `audioFrameCounter_` and `volume_` beside it. Relaxed: it carries no
    /// other state, and a callback that renders one buffer at the previous
    /// rate is inaudible. Pinned by the static_assert in the constructor.
    std::atomic<uint32_t> outputSampleRate_{44100};
    uint32_t hostSampleRate() const
    {
        return outputSampleRate_.load(std::memory_order_relaxed);
    }

    // Spin state.
    bool   audioMotorOn_ = false;
    bool   audioWithDisk_ = false;
    // Wall-clock hold-off for motor-off transitions. POM2's
    // diskTurboWhileMotor bumps the emulated CPU to ~60 MHz during disk
    // I/O, which compresses the controller's 1-sec emulated spin-down
    // delay to ~17 ms wall-clock. Without this hold-off the sound system
    // would receive MotorOff before the start sample even finished —
    // user hears "click, click, silence" instead of recognisable motor
    // whirr. Counts in audio frames (wall-clock), not CPU cycles.
    bool     pendingMotorOff_  = false;
    uint64_t motorOffDeadline_ = 0;
    // start_{empty,loaded} one-shot.
    int    startIdx_ = -1;
    double startPos_ = 0.0;
    // spin_{empty,loaded} loop.
    int    spinLoopIdx_ = -1;
    double spinLoopPos_ = 0.0;
    // spin_end one-shot.
    int    endIdx_ = -1;
    double endPos_ = 0.0;

    // Step / seek state. `lastStepCycle_` is the emulated CPU cycle at
    // the previous step event — used to measure inter-step cadence in
    // MAME-compatible emulated time. `seekTimeoutFrame_` stays in
    // wall-clock audio frames because the seek loop's timeout marks
    // when the audio thread should stop the seek sample, which is a
    // real-time event.
    int      stepSampleIdx_ = -1;
    double   stepPos_  = 0.0;
    double   stepPitch_ = 1.0;
    bool     audioInSeek_ = false;
    uint64_t lastStepCycle_ = 0;
    uint64_t seekTimeoutFrame_ = 0;
    bool     anyStepSeen_ = false;

    // The step voice that was just replaced, fading out (2026-09-08). Every
    // transition of the step voice — a seek class switch, seek → landing
    // click, a click retriggered mid-decay, click → seek — used to reset the
    // cursor to frame 0 of the new sample with the old one cut mid-wave: a
    // hard discontinuity, up to 0.047 full-scale, the "crackle" a file
    // manager's scattered block reads produced twice per burst (pinned by
    // `floppy_sound_crackle`). The outgoing voice now keeps playing for
    // kFadeFrames with a linear ramp to zero while the new one starts.
    int      fadeIdx_    = -1;
    double   fadePos_    = 0.0;
    double   fadePitch_  = 1.0;
    bool     fadeLoop_   = false;
    int      fadeLeft_   = 0;       // frames of ramp remaining
    /// The new seek loop's attack: a loop-clean sample starts at its old
    /// frame `window`, mid-wave, so a fresh loop voice ramps in over the
    /// same kFadeFrames the retired one ramps out — a crossfade.
    int      attackLeft_ = 0;
    static constexpr int kFadeFrames = 96;   // ~2.2 ms at 44.1 kHz

    /// Move the live step voice into the fade slot (if it is playing) so the
    /// caller can start a new one without a cut. Audio thread only.
    void retireStepVoice();

    // Click (insert / eject).
    double clickPos_ = 0.0;
    bool   clickActive_ = false;

    // Volume / mute.
    std::atomic<float> volume_{0.6f};
    std::atomic<bool>  muted_{false};
    // Profile-specific pitch multiplier for motor samples. 1.0 = native
    // (original Disk II Shugart). MainWindow bumps to ~1.4 on //c / //c+
    // to approximate the Sony internal drive's faster spin-up.
    std::atomic<float> motorPitch_{1.0f};
    /// Emulated CPU clock the step-cadence classifier divides by. UI/worker
    /// thread writes (setCpuClock), audio thread reads (drainCommands).
    std::atomic<double> cpuClockHz_{ static_cast<double>(POM2_CPU_CLOCK_HZ) };

    // ─── Helpers ────────────────────────────────────────────────────────
    bool loadOneWav(const std::string& path, Sample& out);

    /// Mix a one-shot sample into `out`, advancing `pos` by `pitch *
    /// (sourceRate / outputRate)` per output frame. Stops when pos
    /// reaches the end (caller checks `pos >= data.size()`).
    void mixOneShot(int sampleIdx, double& pos, double pitch,
                    float* out, int frames, float gain);

    /// Mix a looping sample into `out`, wrapping `pos` modulo data length.
    void mixLoop(int sampleIdx, double& pos, double pitch,
                 float* out, int frames, float gain);

    /// Drain the command queue under cmdMtx_, updating audio-thread state.
    /// Called once at the top of fillAudioBuffer.
    void drainCommands();

    /// Pick the seek sample whose nominal cadence is closest to
    /// `rate_ms`. Returns SAMPLE_COUNT if rate is out of range.
    static int pickSeekSample(double rateMs);

    /// Time thresholds, in ms. kSeekJoinMs (the "still seeking" gap) matches
    /// the slowest seek sample pickSeekSample() can supply (20 ms sample,
    /// usable up to ~50 ms): gaps beyond it are genuinely isolated steps, so
    /// classifying them as seek only to fall through to a click (the old
    /// 50–100 ms dead band) was inconsistent. kSeekTimeoutMs (leave-seek)
    /// must be STRICTLY GREATER than kSeekJoinMs, else a steady stream at the
    /// max joined cadence trips the timeout between steps → a spurious
    /// mid-seek "landing" click. (Previously both were 100 → overlap.)
    static constexpr double kSeekJoinMs    = 50.0;
    static constexpr double kSeekTimeoutMs = 100.0;
    /// Wall-clock hold after motor(false) before the spin loop yields
    /// to spin_end. Decouples audible motor duration from emulated CPU
    /// speed — at 60× turbo the controller's 1-sec emulated spin-down
    /// delay is only ~17 ms wall-clock, far too short to hear the loop.
    static constexpr double kMotorOffHoldMs = 800.0;
};

#endif // POM2_FLOPPY_SOUND_DEVICE_H
