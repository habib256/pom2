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

// Printer sound test — pins src/PrinterSoundDevice.cpp.
//
// The device has no samples to compare against and the test has no ears, so
// what is asserted is the STRUCTURE of the grain scheduler — which is where
// the bugs that matter live:
//
//   1. THE CURSOR CAP IS LOAD-BEARING. Grains are spaced along the audio
//      timeline so they overlap into a buzz instead of collapsing onto one
//      instant. Without a ceiling on how far ahead the cursor may run, a
//      dense burst — a full-black screen dump is tens of thousands of pin
//      strikes in a fraction of a second — queues MINUTES of buzz, and the
//      printer then keeps rattling long after the page is done. The cap makes
//      the burst thin instead. This test fires 20 000 strikes and asserts the
//      noise stops in well under a second.
//   2. DENSE PRINT SUSTAINS, A LONE CHARACTER TICKS. That is the whole point
//      of grains-with-spacing over one-shot-per-event, and it is the
//      difference between "sounds like a printer" and "sounds like a
//      typewriter".
//   3. POWER OFF IS IMMEDIATE. The front-panel switch kills the mechanism; a
//      printer that keeps buzzing after you switch it off is worse than one
//      with no sound at all.

#include "PrinterSoundDevice.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using pom2::PrinterSoundDevice;

constexpr int kRate   = 44100;
constexpr int kFrames = 256;          // a typical mixer buffer

/// Pull one buffer and report its peak. The mixer pre-zeroes, so the test
/// must too — the source ADDS into the buffer.
float pullPeak(PrinterSoundDevice& d)
{
    std::vector<float> buf(kFrames, 0.0f);
    d.fillAudioBuffer(buf.data(), kFrames);
    float peak = 0.0f;
    for (float v : buf) peak = std::max(peak, std::fabs(v));
    return peak;
}

/// Pull until silent (or the deadline), returning how many buffers carried
/// sound. One buffer is ~5.8 ms at 44.1 kHz.
int buffersUntilSilent(PrinterSoundDevice& d, int maxBuffers)
{
    int sounding = 0;
    int quietRun = 0;
    for (int i = 0; i < maxBuffers; ++i) {
        if (pullPeak(d) > 1e-5f) { sounding = i + 1; quietRun = 0; }
        else if (++quietRun > 8) break;      // eight quiet buffers = done
    }
    return sounding;
}

// ── 1. Silence is silent ─────────────────────────────────────────────────
void testIdleIsSilent()
{
    PrinterSoundDevice d;
    d.setSampleRate(kRate);
    for (int i = 0; i < 20; ++i) assert(pullPeak(d) == 0.0f);
    assert(d.activeGrains() == 0);
    std::printf("  ok: an idle printer is silent\n");
}

// ── 2. A strike makes a sound, and it ends ───────────────────────────────
void testSingleStrike()
{
    PrinterSoundDevice d;
    d.setSampleRate(kRate);
    d.strike(9);
    assert(d.activeGrains() == 1);

    const int sounding = buffersUntilSilent(d, 200);
    assert(sounding > 0);
    // An 11 ms grain is ~2 buffers. Allow slack for the scheduling offset,
    // but a single character must not ring for a tenth of a second.
    assert(sounding <= 8);
    assert(d.activeGrains() == 0);
    std::printf("  ok: one strike sounds for %d buffer(s), then stops\n",
                sounding);
}

// ── 3. Loudness follows the pin count ────────────────────────────────────
void testPinCountScalesLevel()
{
    auto peakFor = [](int pins) {
        PrinterSoundDevice d;
        d.setSampleRate(kRate);
        d.strike(pins);
        float peak = 0.0f;
        for (int i = 0; i < 8; ++i) peak = std::max(peak, pullPeak(d));
        return peak;
    };
    const float light = peakFor(1);
    const float heavy = peakFor(9);
    assert(light > 0.0f && heavy > 0.0f);
    // A full stop and a 'W' are not the same impact.
    assert(heavy > light * 1.5f);
    std::printf("  ok: 9 pins is louder than 1 (%.4f vs %.4f)\n", heavy, light);
}

// ── 4. THE cap: a burst thins, it does not queue ─────────────────────────
void testBurstThinsRatherThanQueues()
{
    PrinterSoundDevice d;
    d.setSampleRate(kRate);

    // A full-black screen dump: tens of thousands of strikes arriving in one
    // UI frame. Scheduled naively at 5 ms apart this is 100 SECONDS of buzz.
    for (int i = 0; i < 20000; ++i) d.strike(9);

    const int sounding = buffersUntilSilent(d, 4000);
    assert(sounding > 0);                        // it did make a noise...
    // ...but it stopped. The cursor cap is 0.2 s ahead; add the grain length
    // and generous slack, and 100 buffers is ~0.58 s. Naive queueing would
    // need 17 000.
    assert(sounding < 100);
    std::printf("  ok: 20 000 strikes thin to %d buffers, not 17 000\n",
                sounding);
}

// ── 5. Dense print sustains where a lone character ticks ─────────────────
void testDensePrintSustains()
{
    PrinterSoundDevice d;
    d.setSampleRate(kRate);

    // Feed strikes the way a printing head does: a few per buffer, buffer
    // after buffer. The grains must overlap into continuous sound.
    int sounding = 0;
    for (int b = 0; b < 30; ++b) {
        for (int i = 0; i < 4; ++i) d.strike(6);
        if (pullPeak(d) > 1e-5f) ++sounding;
    }
    // Essentially every buffer should carry sound — that is the buzz.
    assert(sounding >= 27);
    std::printf("  ok: sustained printing sounds in %d/30 buffers\n", sounding);
}

// ── 6. Carriage return and paper feed are their own voices ───────────────
void testCarriageAndFeed()
{
    {   // A long return sweeps for longer than a short one.
        PrinterSoundDevice a, b;
        a.setSampleRate(kRate);
        b.setSampleRate(kRate);
        a.carriageReturn(1.0);
        b.carriageReturn(8.0);
        const int shortSweep = buffersUntilSilent(a, 400);
        const int longSweep  = buffersUntilSilent(b, 400);
        assert(shortSweep > 0 && longSweep > shortSweep);
    }
    {   // A hop shorter than the threshold is not a sweep at all.
        PrinterSoundDevice d;
        d.setSampleRate(kRate);
        d.carriageReturn(0.01);
        assert(d.activeGrains() == 0);
    }
    {   // Paper feed makes a sound.
        PrinterSoundDevice d;
        d.setSampleRate(kRate);
        d.paperFeed(1.0 / 6.0);
        assert(d.activeGrains() == 1);
        assert(buffersUntilSilent(d, 200) > 0);
    }
    std::printf("  ok: carriage sweep scales with distance; feed voices\n");
}

// ── 7. Power off is immediate ────────────────────────────────────────────
void testPowerOffSilences()
{
    PrinterSoundDevice d;
    d.setSampleRate(kRate);
    for (int i = 0; i < 50; ++i) d.strike(9);
    assert(d.activeGrains() > 0);

    d.power(false);
    assert(d.activeGrains() == 0);
    for (int i = 0; i < 10; ++i) assert(pullPeak(d) == 0.0f);

    // And it stays silent while off, however much is printed at it.
    for (int i = 0; i < 50; ++i) d.strike(9);
    assert(d.activeGrains() == 0);
    assert(pullPeak(d) == 0.0f);

    // Back on, it works again — with no backlog of what it missed.
    d.power(true);
    d.strike(9);
    assert(d.activeGrains() == 1);
    std::printf("  ok: power off silences at once and buffers nothing\n");
}

// ── 8. Mute and volume ───────────────────────────────────────────────────
void testMuteAndVolume()
{
    PrinterSoundDevice d;
    d.setSampleRate(kRate);
    d.setMuted(true);
    for (int i = 0; i < 20; ++i) d.strike(9);
    for (int i = 0; i < 10; ++i) assert(pullPeak(d) == 0.0f);

    // Unmuting must not dump the whole muted backlog at once: the grains
    // aged while silent, so most are already over.
    d.setMuted(false);
    const int after = buffersUntilSilent(d, 200);
    assert(after < 100);

    PrinterSoundDevice loud, quiet;
    loud.setSampleRate(kRate);
    quiet.setSampleRate(kRate);
    loud.setVolume(1.0f);
    quiet.setVolume(0.1f);
    loud.strike(9);
    quiet.strike(9);
    float lp = 0.0f, qp = 0.0f;
    for (int i = 0; i < 8; ++i) {
        lp = std::max(lp, pullPeak(loud));
        qp = std::max(qp, pullPeak(quiet));
    }
    assert(lp > qp * 3.0f);
    std::printf("  ok: mute silences, volume scales\n");
}

// ── 9. A sample-rate change must not leave stale grains ──────────────────
void testSampleRateChange()
{
    PrinterSoundDevice d;
    d.setSampleRate(kRate);
    for (int i = 0; i < 10; ++i) d.strike(9);
    assert(d.activeGrains() > 0);

    // Grains carry frame counts computed for the old rate; keeping them would
    // play them at the wrong pitch and length.
    d.setSampleRate(22050);
    assert(d.activeGrains() == 0);
    d.strike(9);
    assert(d.activeGrains() == 1);
    assert(buffersUntilSilent(d, 200) > 0);
    std::printf("  ok: a rate change drops grains built for the old rate\n");
}

// ── 10. A grain DECAYS; it is not a flat-top burst ───────────────────────
//
// The envelope is documented as "attack to the peak, then exponential decay
// to the floor", but the phase used to be inferred from `env < peak` with no
// flag. Every grain shape here has an attack step larger than one decay step
// (a char grain gains 0.0198 per frame and loses 0.0136), so the comparison
// flipped straight back to attack the instant a decay step lowered `env`: the
// envelope locked into a two-frame oscillation just under the peak and the
// voice was cut off dead at `end`. Nothing else in this file could see it —
// the grain still stops, still scales with pin count, still sustains — so it
// takes a shape assertion.
void testGrainDecays()
{
    PrinterSoundDevice d;
    d.setSampleRate(kRate);
    d.strike(9);

    // Collect the whole grain, buffer by buffer, as RMS per block.
    std::vector<float> rms;
    for (int i = 0; i < 200; ++i) {
        std::vector<float> buf(32, 0.0f);
        d.fillAudioBuffer(buf.data(), 32);
        double acc = 0.0;
        for (float v : buf) acc += static_cast<double>(v) * v;
        const float r = static_cast<float>(std::sqrt(acc / buf.size()));
        if (r > 1e-6f) rms.push_back(r);
        else if (!rms.empty()) break;
    }
    assert(rms.size() >= 8 && "the grain should span several buffers");

    // The last quarter must be a small fraction of the first quarter. With the
    // plateau this ratio sat at ~1.0; with a real decay it is well under 0.5.
    const size_t q = rms.size() / 4;
    double head = 0.0, tail = 0.0;
    for (size_t i = 0; i < q; ++i)                 head += rms[i];
    for (size_t i = rms.size() - q; i < rms.size(); ++i) tail += rms[i];
    head /= static_cast<double>(q);
    tail /= static_cast<double>(q);
    assert(tail < head * 0.5 && "the grain envelope never decays");

    std::printf("  ok: grain decays (head RMS %.4f -> tail %.4f)\n", head, tail);
}

// ── 11. The recorded ImageWriter II bank ─────────────────────────────────
// Scott Lawrence's takes (roms/printer_samples, CC BY 4.0) replace the grains
// when selected: a carriage LOOP per print quality held while strikes arrive,
// one-shot takes for the platen, the tear and the power switch.
std::vector<float> render(PrinterSoundDevice& d, int buffers)
{
    std::vector<float> out(static_cast<size_t>(buffers) * kFrames, 0.0f);
    for (int i = 0; i < buffers; ++i)
        d.fillAudioBuffer(out.data() + static_cast<size_t>(i) * kFrames, kFrames);
    return out;
}

void testRecordedBank()
{
    {
        PrinterSoundDevice none;
        none.setSampleRate(kRate);
        assert(!none.loadRecordedBank("/nonexistent/pom2/printer_samples"));
        assert(!none.recordedReady());
        none.setRecorded(true);              // selected, but not there:
        none.strike(9);                      // …the synthesiser still answers
        assert(none.activeGrains() == 1);
    }

    PrinterSoundDevice d;
    d.setSampleRate(kRate);
    assert(d.loadRecordedBank(POM2_PRINTER_SAMPLES_DIR) &&
           "roms/printer_samples is tracked and must decode");
    assert(d.recordedReady());

    // Loaded is not selected: the grains stay until the user (or the saved
    // setting) picks the recordings.
    d.strike(9);
    assert(d.activeGrains() == 1);
    d.setRecorded(true);
    assert(d.activeGrains() == 0 && "switching banks drops the grains in flight");
    for (int i = 0; i < 20; ++i) assert(pullPeak(d) == 0.0f);

    // A strike holds the carriage loop for a moment, with no grain at all,
    // and the loop is released when the strikes stop.
    d.strike(9);
    assert(d.activeGrains() == 0);
    const int held = buffersUntilSilent(d, 400);
    assert(held >= 8 && held <= 40 && "one strike is a short pass of the head");
    // Strikes that keep coming keep it running.
    int sounding = 0;
    for (int i = 0; i < 120; ++i) {          // ~0.7 s of steady printing
        d.strike(5);
        if (pullPeak(d) > 1e-5f) ++sounding;
    }
    assert(sounding > 100 && "steady printing is one continuous take");
    (void)buffersUntilSilent(d, 400);

    // The three qualities are three different takes.
    const auto passOf = [&d](int tier) {
        d.setRecorded(false); d.setRecorded(true);      // rewind the voices
        d.printQuality(tier);
        std::vector<float> all;
        for (int i = 0; i < 40; ++i) {
            d.strike(9);
            const auto b = render(d, 1);
            all.insert(all.end(), b.begin(), b.end());
        }
        (void)buffersUntilSilent(d, 400);
        return all;
    };
    const auto standard = passOf(0), draft = passOf(1), nlq = passOf(2);
    const auto differ = [](const std::vector<float>& a, const std::vector<float>& b) {
        double sum = 0.0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) sum += std::fabs(a[i] - b[i]);
        return sum > 1.0;
    };
    assert(differ(draft, nlq) && differ(draft, standard) && differ(standard, nlq));

    // A line feed is one platen take (~0.4-0.6 s); a run of micro-feeds is
    // still one motion, not a machine gun.
    d.paperFeed(1.0 / 6.0);
    assert(d.activeShots() == 1);
    for (int i = 0; i < 20; ++i) d.paperFeed(1.0 / 144.0);
    assert(d.activeShots() == 1 && "feeds inside 90 ms are one clack");
    const int clack = buffersUntilSilent(d, 600);
    assert(clack >= 40 && clack <= 130);
    assert(d.activeShots() == 0);

    // A form feed is a page-feed take, and a longer travel a longer one.
    d.paperFeed(2.0);
    const int shortFeed = buffersUntilSilent(d, 1200);
    d.paperFeed(9.0);
    const int longFeed = buffersUntilSilent(d, 1200);
    assert(shortFeed >= 60 && longFeed > shortFeed * 2);

    // Tearing the sheet off.
    d.tearOff();
    assert(d.activeShots() == 1);
    assert(buffersUntilSilent(d, 600) > 20);

    // The power switch: off stops the mechanism and is itself heard; nothing
    // prints while it is off; on is the head homing. Attaching a printer that
    // is already on (no change of state) makes no noise.
    d.power(true);
    assert(d.activeShots() == 0);
    d.strike(9);
    d.power(false);
    assert(d.activeShots() == 1 && "the switch is heard");
    (void)buffersUntilSilent(d, 600);
    d.strike(9);
    d.paperFeed(1.0 / 6.0);
    assert(d.activeShots() == 0 && pullPeak(d) == 0.0f && "a printer that is off is silent");
    d.power(true);
    assert(d.activeShots() == 1);
    assert(buffersUntilSilent(d, 1500) > 200 && "the power-up take is seconds long");

    // Back to the synthesiser on request.
    d.setRecorded(false);
    d.strike(9);
    assert(d.activeGrains() == 1);
    std::printf("  ok: recorded bank — strike %d buffers, clack %d, feeds %d/%d\n",
                held, clack, shortFeed, longFeed);
}

} // namespace

int main()
{
    testIdleIsSilent();
    testSingleStrike();
    testPinCountScalesLevel();
    testBurstThinsRatherThanQueues();
    testDensePrintSustains();
    testCarriageAndFeed();
    testPowerOffSilences();
    testMuteAndVolume();
    testSampleRateChange();
    testGrainDecays();
    testRecordedBank();

    std::puts("printer_sound: OK");
    return 0;
}
