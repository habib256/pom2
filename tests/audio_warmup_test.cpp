// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio_warmup — wide at start-up, then adjusted (AudioWarmup.h), and the
// speaker lead's upkeep a real-time host turns on (SpeakerDevice::
// setRealtimeLead). No device, no clock:
//
//  1. the policy: the warm rung per platform; the lead held wide until
//     kSteadySec of steady frames, a slow frame or a late callback starting
//     the count again; the device brought down only at a quiet moment;
//     a ladder climb or a pin ends the device's part
//  2. the speaker, default: a sound 20 ms ahead of the cursor plays 20 ms
//     later — the eroded lead nothing repaired (the SDK keeps this)
//  3. the speaker, real-time: in a silence, a lead (the CPU's now minus the
//     cursor) that eroded is set back to the target; one too long is trimmed
//  4. lowering the lead while a sound plays purges nothing of it
//  5. a tune of notes and short rests keeps its tempo to the millisecond —
//     the first upkeep slowed and jumped Micro Sokoban's music
//  6. the invariant, swept over rests of 5-300 ms and device buffers of
//     256-2048 frames with a worker that makes a frame at once: once the
//     lead is set, no cursor move, no toggle dropped, an exact tempo
//  7. the three threads of a real-time host at once — the CPU worker
//     (toggles, its now), the audio callback, the UI (the lead, the
//     counters): nothing asserted on the sound, ThreadSanitizer is the judge
//     (the nightly tsan leg runs this test for that section)

#include "AudioWarmup.h"
#include "SpeakerDevice.h"

#include <cmath>
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

using pom2::AudioWarmup;
using Platform = pom2::AudioBufferPolicy::Platform;

void policy()
{
    // The warm rung: one up, at least 1024, within the ladder; none in a browser.
    CHECK(AudioWarmup::warmPeriodFrames(Platform::Desktop, 256) == 1024);
    CHECK(AudioWarmup::warmPeriodFrames(Platform::Desktop, 1024) == 2048);
    CHECK(AudioWarmup::warmPeriodFrames(Platform::Desktop, 2048) == 2048);   // the top
    CHECK(AudioWarmup::warmPeriodFrames(Platform::LinuxArm, 1024) == 2048);
    CHECK(AudioWarmup::warmPeriodFrames(Platform::Browser, 2048) == 0);
    CHECK(AudioWarmup::warmPeriodFrames(Platform::BrowserMobile, 4096) == 0);

    AudioWarmup w;
    CHECK(!w.warm());
    auto a = w.begin(10.0, 256, 1024);
    CHECK(a.leadSec == AudioWarmup::kWarmLeadSec && w.warm() && w.deviceWaitsForQuiet());

    // Steady frames, but a late callback at 11 s and a slow frame at 12 s:
    // the three steady seconds count from 12 s.
    double t = 10.0;
    std::uint64_t late = 5;
    auto step = [&](double dt, float peak) { t += dt; return w.poll(t, dt, late, peak); };
    while (t < 11.0) CHECK(step(0.016, 0.5f).leadSec == 0.0);
    ++late;
    CHECK(step(0.016, 0.5f).leadSec == 0.0);
    while (t < 12.0) CHECK(step(0.016, 0.5f).leadSec == 0.0);
    CHECK(step(0.20, 0.5f).leadSec == 0.0);                          // a slow frame
    while (t < 14.9) CHECK(step(0.016, 0.5f).leadSec == 0.0);
    bool lowered = false;
    while (t < 15.3 && !lowered) {
        a = step(0.016, 0.5f);
        lowered = a.leadSec == AudioWarmup::kSteadyLeadSec;
        CHECK(a.deviceFrames == 0);                                  // still loud
    }
    CHECK(lowered && t >= 15.2 - 0.02);
    CHECK(w.warm() && w.deviceWaitsForQuiet());

    // The device waits for kQuietSec of silence; a sound restarts the wait.
    for (int i = 0; i < 10; ++i) CHECK(step(0.016, 0.0f).deviceFrames == 0);   // 0.16 s
    CHECK(step(0.016, 0.3f).deviceFrames == 0);
    std::uint32_t down = 0;
    double quietFrom = t;
    while (t < quietFrom + 1.0 && down == 0) down = step(0.016, 0.0f).deviceFrames;
    CHECK(down == 256 && t - quietFrom >= AudioWarmup::kQuietSec - 1e-9);
    CHECK(!w.warm());
    CHECK(step(0.016, 0.0f).deviceFrames == 0 && step(0.016, 0.0f).leadSec == 0.0);

    // Never silent (a demo's music from the first second): the threshold
    // relaxes, and after kMaxQuietWaitSec the device comes down regardless.
    CHECK(AudioWarmup::quietPeakAfter(0.0) == AudioWarmup::kQuietPeak);
    CHECK(AudioWarmup::quietPeakAfter(5.0) == 2.0f * AudioWarmup::kQuietPeak);
    CHECK(AudioWarmup::quietPeakAfter(1000.0) == AudioWarmup::kLoudestQuiet);
    {
        AudioWarmup m;
        m.begin(0.0, 256, 1024);
        double v = 0.0;
        while (v < 3.5) { v += 0.016; m.poll(v, 0.016, 0, 0.5f); }   // loud: lead down only
        CHECK(m.leadSec() == AudioWarmup::kSteadyLeadSec && m.deviceWaitsForQuiet());
        // A quieter passage (peak 0.01) passes once the threshold has
        // relaxed past it (about 12 s), not before.
        std::uint32_t got = 0;
        double when = 0.0;
        while (v < 40.0 && got == 0) { v += 0.016; got = m.poll(v, 0.016, 0, 0.01f).deviceFrames; when = v; }
        CHECK(got == 256 && when > 3.0 + 10.0 && when < 3.0 + 20.0);
        // Loud throughout: down at kMaxQuietWaitSec.
        AudioWarmup l;
        l.begin(0.0, 256, 1024);
        v = 0.0;
        got = 0;
        while (v < 60.0 && got == 0) { v += 0.016; got = l.poll(v, 0.016, 0, 0.9f).deviceFrames; when = v; }
        CHECK(got == 256 && when >= 3.0 + AudioWarmup::kMaxQuietWaitSec - 0.1 && when < 3.0 + 31.0);
    }

    // A re-warm (a profile switch) widens the lead only.
    a = w.begin(t, 256, 0);
    CHECK(a.leadSec == AudioWarmup::kWarmLeadSec && w.warm() && !w.deviceWaitsForQuiet());

    // A climb or a pin while the device waits: it is not brought down.
    AudioWarmup v;
    v.begin(0.0, 256, 1024);
    v.deviceSettled(2048);
    double u = 0.0;
    std::uint32_t frames = 0;
    while (u < 5.0) { u += 0.016; frames |= v.poll(u, 0.016, 0, 0.0f).deviceFrames; }
    CHECK(frames == 0 && !v.warm());
}

// Speaker: 48 kHz, NTSC clock. The cursor starts at cycle 0.
constexpr std::uint32_t kRate = 48000;
const double kClock = static_cast<double>(POM2_CPU_CLOCK_HZ);
constexpr int kBuffer = 480;   // 10 ms

std::uint64_t at(double seconds) { return static_cast<std::uint64_t>(seconds * kClock); }

// Silence for `seconds`: the cursor walks there with nothing queued.
void advance(SpeakerDevice& s, double seconds)
{
    std::vector<float> out(kBuffer);
    for (int n = static_cast<int>(seconds * kRate / kBuffer); n > 0; --n)
        s.fillAudioBuffer(out.data(), kBuffer);
}

// A 1 kHz square tone from `start` lasting `seconds`.
void tone(SpeakerDevice& s, std::uint64_t start, double seconds)
{
    const std::uint64_t half = at(0.0005);
    for (std::uint64_t c = start; c < start + at(seconds); c += half) s.recordToggle(c);
}

// Milliseconds of output until the tone is heard.
double onsetMs(SpeakerDevice& s)
{
    std::vector<float> out(kBuffer);
    for (int b = 0; b < 100; ++b) {
        s.fillAudioBuffer(out.data(), kBuffer);
        for (int i = 0; i < kBuffer; ++i)
            if (std::fabs(out[i]) > 0.05f) return 1000.0 * (b * kBuffer + i) / kRate;
    }
    return -1.0;
}

void speaker()
{
    // 2. Default (the SDK): a sound queued 20 ms ahead of the cursor plays
    //    20 ms later, and the CPU's now is not even kept.
    {
        SpeakerDevice s;
        s.setSampleRate(kRate);
        advance(s, 0.5);
        s.noteProducerNow(at(0.5) + at(0.02));
        tone(s, at(0.5) + at(0.015), 0.005);
        const double ms = onsetMs(s);
        if (!(ms > 5.0 && ms < 30.0)) std::fprintf(stderr, "default: onset after %.1f ms\n", ms);
        CHECK(ms > 5.0 && ms < 30.0);
        CHECK(s.playbackLeadSeconds() == 0.10);
    }
    // The producer and the consumer in step, 10 ms at a time: `make` queues
    // the toggles of [from, to) and the CPU's now is published, as the
    // worker does, before the buffer is rendered.
    struct Run {
        SpeakerDevice& s;
        std::uint64_t cpu;                 // the CPU's now
        std::vector<float> all;            // everything rendered
        void step(const std::vector<std::uint64_t>& toggles) {
            for (const auto c : toggles) s.recordToggle(c);
            cpu += at(0.01);
            s.noteProducerNow(cpu);
            std::vector<float> out(kBuffer);
            s.fillAudioBuffer(out.data(), kBuffer);
            all.insert(all.end(), out.begin(), out.end());
        }
    };
    // The toggles of a tune in [from, to): 80 ms of 1 kHz, 60 ms of rest.
    auto tune = [](std::uint64_t origin, std::uint64_t from, std::uint64_t to) {
        std::vector<std::uint64_t> out;
        const std::uint64_t half = at(0.0005), period = at(0.14), note = at(0.08);
        for (std::uint64_t c = from - (from - origin) % half; c < to; c += half)
            if (c >= from && (c - origin) % period < note) out.push_back(c);
        return out;
    };
    // The start of each note in the rendered output, in ms.
    auto onsets = [](const std::vector<float>& all) {
        std::vector<double> at_;
        int quiet = 1000000;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (std::fabs(all[i]) > 0.05f) {
                if (quiet > static_cast<int>(kRate) * 3 / 100) at_.push_back(1000.0 * double(i) / kRate);   // after 30 ms of rest
                quiet = 0;
            } else ++quiet;
        }
        return at_;
    };

    // 3. Real-time, the CPU running on (its now published every 10 ms): a
    //    short sound made 5 ms before the CPU's now, after a long silence,
    //    with the CPU `leadMs` ahead of the cursor. Returns when it is heard.
    auto heardAfter = [&](double leadMs) {
        SpeakerDevice s;
        s.setSampleRate(kRate);
        s.setRealtimeLead(0.10);
        advance(s, 0.5);
        Run run{s, at(0.5) + at(leadMs / 1000.0) - at(0.01), {}};
        std::vector<std::uint64_t> sound;
        for (std::uint64_t c = run.cpu + at(0.005); c < run.cpu + at(0.01); c += at(0.0005)) sound.push_back(c);
        run.step(sound);
        for (int b = 0; b < 60; ++b) run.step({});
        CHECK(s.staleDropCount() == 0);
        for (std::size_t n = 0; n < run.all.size(); ++n)
            if (std::fabs(run.all[n]) > 0.05f) return 1000.0 * double(n) / kRate;
        return -1.0;
    };
    // 3a. An eroded lead (20 ms): set back to 100 ms in the silence, so the
    //     sound made 5 ms before the CPU's now plays about 95 ms later.
    {
        const double ms = heardAfter(20.0);
        if (!(ms > 80.0 && ms < 110.0)) std::fprintf(stderr, "eroded lead: heard after %.1f ms\n", ms);
        CHECK(ms > 80.0 && ms < 110.0);
    }
    // 3b. A lead over one and a half times the target (the start-up 300 ms
    //     once the lead came down): trimmed in the silence, same 95 ms.
    {
        const double ms = heardAfter(300.0);
        if (!(ms > 80.0 && ms < 110.0)) std::fprintf(stderr, "long lead: heard after %.1f ms\n", ms);
        CHECK(ms > 80.0 && ms < 110.0);
    }
    // ...and one inside the tolerance (120 ms) is left alone: 115 ms.
    {
        const double ms = heardAfter(120.0);
        if (!(ms > 100.0 && ms < 130.0)) std::fprintf(stderr, "healthy lead: heard after %.1f ms\n", ms);
        CHECK(ms > 100.0 && ms < 130.0);
    }

    // 5. Micro Sokoban's shape: notes with 60 ms rests, a healthy 100 ms
    //    lead. The rests are silences longer than kIdleSecs, and the first
    //    toggle of each note is then only a few ms ahead of the cursor: the
    //    first upkeep read that as a lead of a few ms, pushed every note
    //    back 95 ms, and purged the backlog in a lump every few notes — the
    //    tempo slowed, then jumped. Here every note starts 140 ms after the
    //    last, and nothing is purged.
    {
        SpeakerDevice s;
        s.setSampleRate(kRate);
        s.setRealtimeLead(0.10);
        advance(s, 0.5);
        const std::uint64_t origin = at(0.5) + at(0.10);
        Run run{s, origin, {}};
        for (int b = 0; b < 400; ++b) run.step(tune(origin, run.cpu, run.cpu + at(0.01)));
        const auto starts = onsets(run.all);
        CHECK(starts.size() >= 26);
        double worst = 0.0;
        for (std::size_t i = 1; i < starts.size(); ++i)
            worst = std::fmax(worst, std::fabs(starts[i] - starts[i - 1] - 140.0));
        if (worst > 1.0) std::fprintf(stderr, "tempo: a note %.1f ms off\n", worst);
        CHECK(worst <= 1.0);
        CHECK(s.staleDropCount() == 0);
        CHECK(s.getQueuedEventCount() < 250);   // ~100 ms of toggles, no backlog
    }

    // 6. THE INVARIANT, swept: a worker that makes a whole 20 ms frame at
    //    once (as POM2's does), a device that asks for 256, 1024 or 2048
    //    frames at 44.1 kHz, and tunes whose rests run from 5 to 300 ms —
    //    shorter than kIdleSecs, around it, around the lead, beyond it.
    //    Once the lead is set (the first second), the cursor never moves
    //    again other than by playing, no toggle is ever dropped, and every
    //    note starts exactly one period after the last.
    for (const int frames : { 256, 1024, 2048 }) {
        for (const double restMs : { 5.0, 20.0, 45.0, 55.0, 60.0, 95.0, 150.0, 300.0 }) {
            SpeakerDevice s;
            s.setSampleRate(44100);
            s.setRealtimeLead(0.10);
            const std::uint64_t half = at(0.0005), note = at(0.08);
            const std::uint64_t period = note + at(restMs / 1000.0);
            std::vector<float> all, out(static_cast<std::size_t>(frames));
            std::uint64_t cpu = 0;
            double nextFrame = 0.0, nextCallback = 0.0;
            std::uint64_t movesAfterStart = 0;
            for (double t = 0.0; t < 21.0; t += 0.001) {
                if (t >= nextFrame) {                       // the worker: one frame, at once
                    const std::uint64_t to = cpu + at(0.02);
                    // Silent for the first second, then the tune.
                    for (std::uint64_t c = cpu - cpu % half + half; c < to; c += half)
                        if (c >= at(1.0) && (c - at(1.0)) % period < note) s.recordToggle(c);
                    cpu = to;
                    s.noteProducerNow(cpu);
                    nextFrame += 0.02;
                }
                if (t >= nextCallback) {                    // the device
                    s.fillAudioBuffer(out.data(), frames);
                    all.insert(all.end(), out.begin(), out.end());
                    nextCallback += frames / 44100.0;
                }
                if (t < 1.0) movesAfterStart = s.cursorMoveCount();
            }
            const bool still = s.cursorMoveCount() == movesAfterStart;
            if (!still || s.droppedToggleCount() != 0)
                std::fprintf(stderr, "%d frames, %.0f ms rests: %llu cursor moves, %llu toggles dropped\n", frames, restMs,
                             static_cast<unsigned long long>(s.cursorMoveCount() - movesAfterStart),
                             static_cast<unsigned long long>(s.droppedToggleCount()));
            CHECK(still);
            CHECK(s.droppedToggleCount() == 0);
            if (restMs >= 45.0) {                           // rests the onset finder can see
                std::vector<double> starts;
                int quiet = 1000000;
                for (std::size_t i = 0; i < all.size(); ++i) {
                    if (std::fabs(all[i]) > 0.05f) {
                        if (quiet > 44100 * 3 / 100) starts.push_back(1000.0 * double(i) / 44100.0);
                        quiet = 0;
                    } else ++quiet;
                }
                const double want = 1000.0 * double(period) / kClock;
                double worst = 0.0;
                for (std::size_t i = 1; i < starts.size(); ++i)
                    worst = std::fmax(worst, std::fabs(starts[i] - starts[i - 1] - want));
                if (worst > 1.0 || starts.size() < 20)
                    std::fprintf(stderr, "%d frames, %.0f ms rests: %zu notes, one %.1f ms off\n",
                                 frames, restMs, starts.size(), worst);
                CHECK(starts.size() >= 20 && worst <= 1.0);
            }
        }
    }

    // 4. A tone playing 300 ms behind the CPU when the lead comes down to
    //    100 ms: nothing of it is purged or moved while it sounds; the
    //    300 ms is trimmed in the first silence, and the next tone plays
    //    100 ms behind the CPU.
    {
        SpeakerDevice s;
        s.setSampleRate(kRate);
        s.setRealtimeLead(0.30);
        advance(s, 0.5);
        const std::uint64_t half = at(0.0005);
        Run run{s, at(0.5) + at(0.30), {}};
        std::uint64_t next = run.cpu;
        auto toneUntil = [&](std::uint64_t until) {
            std::vector<std::uint64_t> out;
            for (; next < until; next += half) out.push_back(next);
            return out;
        };
        for (int b = 0; b < 100; ++b) {
            if (b == 50) s.setRealtimeLead(0.10);
            run.step(toneUntil(run.cpu + at(0.01)));
            if (b >= 40) CHECK(s.getQueuedEventCount() > 500);   // ~600 toggles in 0.3 s, before and after
        }
        for (int b = 0; b < 60; ++b) run.step({});              // the tone ends; 0.6 s of rest
        CHECK(s.getQueuedEventCount() == 0);
        const std::size_t before = run.all.size();
        const std::uint64_t second = run.cpu;
        next = second;
        for (int b = 0; b < 30; ++b) run.step(toneUntil(run.cpu + at(0.01)));
        double heard = -1.0;
        for (std::size_t i = before; i < run.all.size() && heard < 0.0; ++i)
            if (std::fabs(run.all[i]) > 0.05f) heard = 1000.0 * double(i - before) / kRate;
        if (!(heard > 85.0 && heard < 115.0)) std::fprintf(stderr, "after the trim: %.1f ms\n", heard);
        CHECK(heard > 85.0 && heard < 115.0);
        CHECK(s.staleDropCount() == 0);
    }
}

// 7. The worker, the audio callback and the UI on one speaker, as in POM2:
//    EmulationController::runCpuSlice records toggles and publishes its now,
//    the device thread fills, AudioCoordinator moves the lead and the Audio
//    panel reads the counters. The worker also resets once mid-run (a hard
//    reset while a tune plays).
void threads()
{
    SpeakerDevice s;
    s.setSampleRate(kRate);
    s.setRealtimeLead(0.30);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> filled{0};

    std::thread audio([&] {
        std::vector<float> out(512);
        while (!stop.load(std::memory_order_relaxed)) {
            s.fillAudioBuffer(out.data(), static_cast<int>(out.size()));
            filled.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    std::thread ui([&] {
        std::uint64_t seen = 0;
        for (int i = 0; !stop.load(std::memory_order_relaxed); ++i) {
            s.setRealtimeLead(i % 2 ? 0.10 : 0.30);
            seen += s.cursorMoveCount() + s.droppedToggleCount() + s.staleDropCount()
                  + s.getQueuedEventCount();
            seen += static_cast<std::uint64_t>(s.playbackLeadSeconds() * 1000.0);
            std::this_thread::yield();
        }
        (void)seen;
    });

    // The worker: 400 slices of 20 ms, notes of 60 ms and rests of 40 ms.
    const std::uint64_t slice = static_cast<std::uint64_t>(0.02 * kClock);
    const std::uint64_t half  = static_cast<std::uint64_t>(0.0005 * kClock);
    std::uint64_t cpu = 0;
    for (int n = 0; n < 400; ++n) {
        if (n == 200) s.reset();
        if (n % 5 < 3)
            for (std::uint64_t c = cpu; c < cpu + slice; c += half) s.recordToggle(c);
        cpu += slice;
        s.noteProducerNow(cpu);
        std::this_thread::yield();
    }
    while (filled.load(std::memory_order_relaxed) < 50) std::this_thread::yield();
    stop.store(true, std::memory_order_relaxed);
    audio.join();
    ui.join();
    CHECK(filled.load() >= 50);
}

} // namespace

int main()
{
    policy();
    speaker();
    threads();
    if (g_fail) {
        std::fprintf(stderr, "audio_warmup: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("audio_warmup: PASS\n");
    return 0;
}
