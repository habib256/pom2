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

// FloppySoundDevice smoke test. Pins:
//   1. Sample loading succeeds against the bundled roms/floppy_samples/
//      tree (both 5.25" and 3.5" sets).
//   2. With no samples loaded: fillAudioBuffer is silent (and bumps the
//      internal frame counter so step-rate measurements stay consistent
//      across mute toggles).
//   3. motor(true, withDisk) drives a non-silent buffer (spin-up audible).
//   4. step() classifies rapid pulses (~6 ms cadence) into seek mode
//      and slow pulses (>= 100 ms) as single-step clicks.
//   5. mute zeroes the output; volume scales it.
//   6. reset() drains the queue and silences the next buffer.

#include "FloppySoundDevice.h"
#include "CpuClock.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {
// Cycle-count helper: `n` ms expressed in emulated 6502 cycles.
inline uint64_t cyclesForMs(double ms)
{
    return static_cast<uint64_t>(ms * POM2_CPU_CLOCK_HZ / 1000.0);
}
}  // namespace

namespace fs = std::filesystem;

namespace {

// Find roms/floppy_samples/ relative to the test binary's likely cwd.
std::string findSampleDir()
{
    static const char* candidates[] = {
        "roms/floppy_samples",
        "../roms/floppy_samples",
        "../../roms/floppy_samples",
        "../../../roms/floppy_samples",
    };
    for (const char* p : candidates) {
        if (fs::is_directory(p)) return p;
    }
    return {};
}

float bufferEnergy(const std::vector<float>& b)
{
    double e = 0.0;
    for (float v : b) e += static_cast<double>(v) * v;
    return static_cast<float>(e);
}

void testSilenceWithoutSamples()
{
    FloppySoundDevice fs;
    assert(!fs.isLoaded());
    fs.setSampleRate(44100);
    fs.motor(true, true);     // queued, but no samples → silent anyway
    fs.step(5, cyclesForMs(0));
    std::vector<float> buf(512, 1.23f);
    fs.fillAudioBuffer(buf.data(), 512);
    // The source mixes additively; since no samples are loaded it must
    // not touch the buffer. We pre-filled with 1.23f to detect any
    // accidental write — the value must survive.
    for (float v : buf) assert(v == 1.23f);
    std::puts("OK silence_without_samples");
}

void testLoadSamples525(const std::string& dir)
{
    FloppySoundDevice fs;
    const bool ok = fs.loadSamples(dir, FloppySoundDevice::FormFactor::FF525);
    assert(ok);
    assert(fs.isLoaded());
    assert(fs.formFactor() == FloppySoundDevice::FormFactor::FF525);
    std::puts("OK load_525_samples");
}

void testLoadSamples35(const std::string& dir)
{
    FloppySoundDevice fs;
    const bool ok = fs.loadSamples(dir, FloppySoundDevice::FormFactor::FF35);
    assert(ok);
    assert(fs.formFactor() == FloppySoundDevice::FormFactor::FF35);
    std::puts("OK load_35_samples");
}

void testMotorAudible(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.setVolume(1.0f);

    // Mix a buffer with motor OFF — should be (very near) silent.
    std::vector<float> off(2048, 0.0f);
    fs.fillAudioBuffer(off.data(), 2048);
    const float offE = bufferEnergy(off);

    // Switch motor on and mix again. The spin_start_loaded sample is
    // a >300 ms burst of motor noise; one 2048-sample buffer (~46 ms)
    // must contain audible energy well above the off-state.
    fs.motor(true, true);
    std::vector<float> on(2048, 0.0f);
    fs.fillAudioBuffer(on.data(), 2048);
    const float onE = bufferEnergy(on);
    std::printf("  motor energy: off=%.6f on=%.6f\n", offE, onE);
    assert(onE > 0.01f);
    assert(onE > offE * 1000.0f || (offE < 1e-6f && onE > 0.01f));
    assert(fs.audioMotorOn());
    std::puts("OK motor_audible");
}

void testStepAndSeek(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.setVolume(1.0f);
    // Drain any pending state.
    std::vector<float> buf(256, 0.0f);
    fs.fillAudioBuffer(buf.data(), 256);

    // Slow single step → not seek mode. (Even the first step should
    // count as a single-step click since gap > kSeekJoinMs from the
    // never-seen state.)
    fs.step(0, cyclesForMs(10));
    buf.assign(256, 0.0f);
    fs.fillAudioBuffer(buf.data(), 256);
    assert(!fs.audioInSeek());
    std::puts("OK single_step_not_seek");

    // Now fire 8 rapid steps at ~6 ms cadence — perfect for SEEK_6MS
    // classification. Cycle stamps advance independently of the audio
    // buffer drain (which simulates POM2's CPU thread/audio thread
    // decoupling: CPU keeps queuing events while audio drains them).
    double tMs = 16.0;   // first rapid step lands 6 ms after the slow one
    for (int i = 0; i < 8; ++i) {
        fs.step(i + 1, cyclesForMs(tMs));
        buf.assign(256, 0.0f);
        fs.fillAudioBuffer(buf.data(), 256);
        tMs += 6.0;
    }
    assert(fs.audioInSeek());
    std::puts("OK rapid_steps_enter_seek");
}

void testMuteSilencesOutput(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.setVolume(1.0f);
    fs.motor(true, true);
    std::vector<float> a(2048, 0.0f);
    fs.fillAudioBuffer(a.data(), 2048);
    assert(bufferEnergy(a) > 0.01f);

    fs.setMuted(true);
    std::vector<float> b(2048, 0.0f);
    fs.fillAudioBuffer(b.data(), 2048);
    // Mute must zero the contribution; buffer was pre-zero so it stays
    // exactly zero (no other source mixes in).
    for (float v : b) assert(v == 0.0f);
    std::puts("OK mute_silences");
}

// Regression: at 60× turbo the boot PROM's full phase sweep (~80 step
// events at ~10 emulated ms cadence) lands inside ONE audio buffer.
// Pre-fix, two things went wrong in sequence:
//
//   1. The original code computed inter-step rate from
//      `audioFrameCounter_` (wall-clock). All same-buffer events shared
//      the same `now`, so `pitch = nominalMs / 0` → INF → mixLoop spun
//      forever on `pos += INF` (INF - len == INF in IEEE 754).
//   2. A first band-aid forced `lastStepFrame_ = now + framesPerMs` to
//      coerce a 1 ms synthetic gap on the next event. Wrong sign:
//      `(now - lastStepFrame_)` underflowed as uint64 → ~1e17 ms gap,
//      classified as out-of-seek → single STEP_1_1 click per event
//      with stepPos_=0 reset each time → user heard step_1_1's attack
//      repeated buffer-after-buffer ("haché" / buzzy).
//
// Real fix: measure step cadence in emulated CPU cycles (MAME-parity
// with floppy_sound_device::step using `machine().time()`), passed
// explicitly by DiskIICard. Same-buffer events still have monotonic
// cycle stamps, so SEEK classification is honest regardless of how
// many wall-clock buffers the audio thread sees.
void testRapidStepsNoHang(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.setVolume(1.0f);

    // Drain.
    std::vector<float> drain(256, 0.0f);
    fs.fillAudioBuffer(drain.data(), 256);

    // Fire 140 step events spaced ~10 emulated ms apart but queued with
    // NO fillAudioBuffer in between (the turbo case: CPU advances its
    // emulated time fast in wall-clock). Pre-fix this would hang or
    // degenerate to a buzz. After the fix the events classify based on
    // their cycle stamps → SEEK_12MS at near-native pitch.
    uint64_t cyc = cyclesForMs(100);
    for (int i = 0; i < 140; ++i) {
        fs.step(i, cyc);
        cyc += cyclesForMs(10);
    }

    // First buffer after the burst: must return promptly and produce
    // non-silent output (seek sample looping).
    std::vector<float> buf(2048, 0.0f);
    fs.fillAudioBuffer(buf.data(), 2048);
    assert(bufferEnergy(buf) > 0.01f);
    // audioInSeek_ must be true — pre-fix it ended up false (single-
    // click fall-through).
    assert(fs.audioInSeek());
    std::puts("OK rapid_steps_no_hang");
}

// Regression: at 60× turbo with same-cycle step events (worst case —
// CPU queued multiple steps within a single emulated cycle, e.g. a
// peripheral firing back-to-back). Gap is genuinely 0, must floor to
// 1 ms to keep pitch finite. mixLoop must not spin on INF.
void testSameCycleStepsClampGracefully(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.setVolume(1.0f);

    std::vector<float> drain(256, 0.0f);
    fs.fillAudioBuffer(drain.data(), 256);

    // 50 steps at the *same* emulated cycle — the pathological case the
    // original code hit with audio-frame deltas. Gap floors to 1 ms,
    // classifies as SEEK_2MS at pitch 2.0.
    const uint64_t cyc = cyclesForMs(100);
    for (int i = 0; i < 50; ++i) fs.step(i, cyc);

    std::vector<float> buf(2048, 0.0f);
    fs.fillAudioBuffer(buf.data(), 2048);
    assert(bufferEnergy(buf) > 0.01f);
    assert(fs.audioInSeek());
    std::puts("OK same_cycle_steps_clamp");
}

// Regression: at turbo speed, motor(false) follows motor(true) within
// milliseconds wall-clock. Pre-fix, the audio thread silenced the spin
// loop immediately and the user heard "start click → end click →
// silence" instead of the motor whirr. The hold-off in fillAudioBuffer
// should now keep the loop audible for ~800 ms wall-clock after the
// controller's motor(false), regardless of how fast the emulated CPU
// fired them.
void testRapidMotorTogglePreservesLoop(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    const uint32_t sr = 44100;
    fs.setSampleRate(sr);
    fs.setVolume(1.0f);

    fs.motor(true,  true);
    fs.motor(false, true);     // queued back-to-back like at 60× turbo

    // Step 100 ms of audio (~4410 frames). Hold-off is 800 ms — the loop
    // must still be playing here, audioMotorOn_ must still be true.
    const int frames = 4410;
    std::vector<float> a(frames, 0.0f);
    fs.fillAudioBuffer(a.data(), frames);
    assert(fs.audioMotorOn());
    const float earlyE = bufferEnergy(a);
    assert(earlyE > 0.01f);

    // Step 500 ms more (still inside the 800 ms hold-off window).
    std::vector<float> b(static_cast<size_t>(sr / 2), 0.0f);
    fs.fillAudioBuffer(b.data(), static_cast<int>(b.size()));
    assert(fs.audioMotorOn());
    assert(bufferEnergy(b) > 0.1f);

    // Step past the hold-off (another 400 ms takes us to ~1 s total).
    // audioMotorOn_ flips off when the next fillAudioBuffer sees
    // audioFrameCounter_ >= motorOffDeadline_ — that check happens at
    // the top of each buffer call, so we need a follow-up small buffer
    // after crossing the 800 ms deadline.
    std::vector<float> c(static_cast<size_t>(sr * 4 / 10), 0.0f);
    fs.fillAudioBuffer(c.data(), static_cast<int>(c.size()));
    std::vector<float> tiny(64, 0.0f);
    fs.fillAudioBuffer(tiny.data(), 64);
    assert(!fs.audioMotorOn());
    std::puts("OK rapid_motor_toggle_preserves_loop");
}

void testReset(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.motor(true, true);
    // Reset queues a MotorOff command. After one fillAudioBuffer the
    // queue is drained and audioMotorOn_ flips false — but the spin_end
    // one-shot is now active. We just verify no crash.
    fs.reset();
    std::vector<float> buf(512, 0.0f);
    fs.fillAudioBuffer(buf.data(), 512);
    assert(!fs.audioMotorOn());
    std::puts("OK reset");
}

// `drainCommands` swaps the producer queue into an audio-thread-owned
// member (`cmdScratch_`) and clear()s it instead of destroying a local
// vector — no malloc/free per realtime callback. The swap+clear protocol
// has to be exactly right: forget the clear and every command is replayed
// on every later callback; swap the wrong way and commands are lost. Pin
// both directions through the observable motor state.
void testDrainConsumesEachCommandOnce(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    std::vector<float> buf(512, 0.0f);

    fs.motor(true, true);
    assert(fs.queuedCommandCount() == 1);
    fs.fillAudioBuffer(buf.data(), 512);
    assert(fs.queuedCommandCount() == 0 && "drain must empty the queue");
    assert(fs.audioMotorOn());

    // Further callbacks carry no commands — a replayed MotorOn would keep
    // restarting the spin-up sample, and a replayed MotorOff would stop
    // the motor outright.
    for (int i = 0; i < 4; ++i) fs.fillAudioBuffer(buf.data(), 512);
    assert(fs.audioMotorOn() && "no command may be replayed after its drain");

    // A command queued after a drain still lands.
    fs.motor(false, true);
    assert(fs.queuedCommandCount() == 1);
    fs.fillAudioBuffer(buf.data(), 512);
    assert(fs.queuedCommandCount() == 0);
    std::puts("OK drain consumes each command exactly once");
}

}  // namespace

int main()
{
    std::puts("FloppySoundDevice smoke test starting…");

    testSilenceWithoutSamples();

    const std::string dir = findSampleDir();
    if (dir.empty()) {
        std::puts("WARN  roms/floppy_samples/ not found relative to CWD —");
        std::puts("      sample-dependent assertions skipped. The path probe");
        std::puts("      walks ., .., ../.., ../../.. — run from repo root or");
        std::puts("      build/.");
        return 0;
    }
    std::printf("Using sample dir: %s\n", dir.c_str());

    testLoadSamples525(dir);
    testLoadSamples35 (dir);
    testMotorAudible  (dir);
    testStepAndSeek   (dir);
    testMuteSilencesOutput(dir);
    testRapidMotorTogglePreservesLoop(dir);
    testRapidStepsNoHang(dir);
    testSameCycleStepsClampGracefully(dir);
    testReset         (dir);
    testDrainConsumesEachCommandOnce(dir);

    std::puts("OK floppy_sound_smoke");
    return 0;
}
