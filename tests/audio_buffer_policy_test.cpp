// POM2 — GPL-3.0-or-later
//
// audio_buffer_policy — the host audio device's buffer ladder
// (AudioBufferPolicy.h) and the device's hot re-open (AudioDevice::
// reconfigurePeriod).
//
// Why it exists (2026-11-01): the Raspberry Pi build crackled unless the
// user raised scheduler priorities by hand, and the WASM build crackled on
// a phone. Both are the same defect seen from the device side: POM2 opened
// the host device with 3 × 256 frames (16 ms) everywhere, and a host whose
// audio thread — or, in the browser, whose main thread — is not scheduled
// within 16 ms lets the device run dry. The sources' own jitter buffers
// (100 ms on the speaker, two frames on the Mockingboard) cannot help with
// that; only the device buffer can. The policy is a ladder of period sizes
// with a platform first rung, climbed on LATE CALLBACKS the device measures
// itself. Pure functions here, so the rules are pinned without a sound
// card; the re-open runs on whatever backend the host has (CI: miniaudio's
// null device, which still drives the callback).
//
//   1. The rungs: platform defaults, the top of each ladder, doubling.
//   2. A late callback is a gap longer than the whole buffer, and not a
//      laptop lid.
//   3. The climb: three late callbacks move one rung; two do not; the top
//      rung stays.
//   4. The device re-opens at another rung with its sources still
//      registered, told the rate again.

#include "AudioBufferPolicy.h"
#include "AudioDevice.h"
#include "AudioMix.h"
#include "EventRing.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {

using P = pom2::AudioBufferPolicy;

void testRungs()
{
    static_assert(P::defaultPeriodFrames(P::Platform::Desktop) == 256);
    static_assert(P::defaultPeriodFrames(P::Platform::LinuxArm) == 1024);
    static_assert(P::defaultPeriodFrames(P::Platform::Browser) == 2048);
    static_assert(P::defaultPeriodFrames(P::Platform::BrowserMobile) == 4096);
    static_assert(P::maxPeriodFrames(P::Platform::Desktop) == 2048);
    static_assert(P::maxPeriodFrames(P::Platform::LinuxArm) == 2048);
    static_assert(P::maxPeriodFrames(P::Platform::Browser) == 8192);
    static_assert(P::maxPeriodFrames(P::Platform::BrowserMobile) == 8192);
    // Every default is a rung of its own ladder.
    for (auto p : {P::Platform::Desktop, P::Platform::LinuxArm,
                   P::Platform::Browser, P::Platform::BrowserMobile})
        assert(P::isRung(P::defaultPeriodFrames(p), p));
    // Powers of two inside the range, nothing else.
    assert(P::isRung(256, P::Platform::Desktop));
    assert(P::isRung(2048, P::Platform::Desktop));
    assert(!P::isRung(4096, P::Platform::Desktop) && "above the desktop's top");
    assert(P::isRung(4096, P::Platform::Browser));
    assert(!P::isRung(128, P::Platform::Browser) && "below the ladder");
    assert(!P::isRung(300, P::Platform::Desktop) && "not a power of two");
    assert(!P::isRung(0, P::Platform::Desktop));
    // Doubling, clamped at the top.
    assert(P::nextPeriodFrames(256, P::Platform::Desktop) == 512);
    assert(P::nextPeriodFrames(1024, P::Platform::Desktop) == 2048);
    assert(P::nextPeriodFrames(2048, P::Platform::Desktop) == 2048);
    assert(P::nextPeriodFrames(2048, P::Platform::Browser) == 4096);
    assert(P::nextPeriodFrames(8192, P::Platform::BrowserMobile) == 8192);
    // 3 × 256 at 48 kHz = 16 ms; 3 × 2048 = 128 ms.
    const double b256 = P::bufferSeconds(256, 48000);
    assert(b256 > 0.0159 && b256 < 0.0161);
    const double b2048 = P::bufferSeconds(2048, 48000);
    assert(b2048 > 0.1279 && b2048 < 0.1281);
    assert(P::bufferSeconds(256, 0) == 0.0);
    std::printf("[ OK ] the rungs: defaults, tops, doubling\n");
}

void testLateCallback()
{
    // 16 ms buffer: a 15 ms gap is a normal callback, 21 ms (> 1.25 ×) is a
    // hole the listener heard, 2 s is the lid closing — not a rung's worth.
    assert(!P::callbackIsLate(0.015, 256, 48000));
    assert(!P::callbackIsLate(0.019, 256, 48000) && "inside the burst factor");
    assert(P::callbackIsLate(0.021, 256, 48000));
    assert(P::callbackIsLate(0.300, 256, 48000));
    assert(!P::callbackIsLate(2.000, 256, 48000) && "a sleep is not an underrun");
    // The threshold scales with the rung: 21 ms is on time at 1024 frames.
    assert(!P::callbackIsLate(0.021, 1024, 48000));
    assert(P::callbackIsLate(0.090, 1024, 48000));
    // No rate, no judgement.
    assert(!P::callbackIsLate(0.5, 256, 0));
    std::printf("[ OK ] a late callback is a gap longer than the buffer\n");
}

void testClimb()
{
    assert(P::escalatedPeriodFrames(256, 0, P::Platform::Desktop) == 256);
    assert(P::escalatedPeriodFrames(256, 2, P::Platform::Desktop) == 256 &&
           "two late callbacks in a window are noise");
    assert(P::escalatedPeriodFrames(256, 3, P::Platform::Desktop) == 512);
    assert(P::escalatedPeriodFrames(256, 40, P::Platform::Desktop) == 512 &&
           "one rung per window, however bad it was");
    assert(P::escalatedPeriodFrames(2048, 10, P::Platform::Desktop) == 2048 &&
           "the top rung stays");
    assert(P::escalatedPeriodFrames(2048, 10, P::Platform::Browser) == 4096);
    std::printf("[ OK ] the climb: three late callbacks, one rung\n");
}

// A source that records what the device tells it.
struct ProbeSource final : AudioSource, RateAware {
    std::atomic<int>      fills{0};
    std::atomic<uint32_t> rate{0};
    std::atomic<int>      rateCalls{0};
    void fillAudioBuffer(float* out, int n) override
    {
        for (int i = 0; i < n; ++i) out[i] = 0.0f;
        fills.fetch_add(1, std::memory_order_relaxed);
    }
    void setSampleRate(uint32_t hz) override
    {
        rate.store(hz, std::memory_order_relaxed);
        rateCalls.fetch_add(1, std::memory_order_relaxed);
    }
};

constexpr float kSentinel = 4.0f;

bool stillRegistered(AudioDevice& dev, AudioSource* src)
{
    src->lastBufferPeak.store(kSentinel, std::memory_order_relaxed);
    std::vector<float> out(256 * AudioDevice::kChannels, 0.0f);
    dev.mixSources(out.data(), 256);
    return src->lastBufferPeak.load(std::memory_order_relaxed) != kSentinel;
}

void testReopenKeepsSources()
{
    AudioDevice dev;   // Host: a real backend, or miniaudio's null device
    const bool wasAvailable = dev.isAvailable();
    const uint32_t first = dev.periodFrames();
    assert(P::isRung(first, dev.platform()) && "opened on a rung");
    assert(first == P::defaultPeriodFrames(dev.platform()));

    ProbeSource a, b;
    dev.addSource(&a);
    dev.addSource(&b);
    assert(a.rateCalls.load() == 1 && "addSource told the source the rate");
    assert(a.rate.load() == dev.getActualSampleRate());

    // Not a rung: refused, nothing changes.
    assert(!dev.reconfigurePeriod(300));
    assert(dev.periodFrames() == first);

    // One rung up: the device re-opens, both sources are still on the bus.
    const uint32_t next = P::nextPeriodFrames(first, dev.platform());
    assert(next != first);
    assert(dev.reconfigurePeriod(next));
    assert(dev.periodFrames() == next);
    assert(dev.isAvailable() == wasAvailable && "re-open changed availability");
    assert(stillRegistered(dev, &a) && stillRegistered(dev, &b) &&
           "the re-open dropped a source");
    assert(a.rate.load() == dev.getActualSampleRate() &&
           "a source left on the old rate would drift");
    assert(dev.bufferSeconds() ==
           P::bufferSeconds(next, dev.getActualSampleRate()));

    // The same rung again is a no-op that succeeds.
    assert(dev.reconfigurePeriod(next));
    assert(stillRegistered(dev, &a));

    // Back down, and the tally survives as a counter (it is since launch).
    assert(dev.reconfigurePeriod(first));
    assert(dev.periodFrames() == first);
    (void)dev.lateCallbacks();

    // Offline: no device, the rung is recorded for a later open.
    AudioDevice offline(AudioDevice::OutputMode::Offline);
    assert(!offline.isAvailable());
    assert(offline.reconfigurePeriod(P::maxPeriodFrames(offline.platform())));
    assert(offline.periodFrames() == P::maxPeriodFrames(offline.platform()));

    dev.removeSource(&a);
    dev.removeSource(&b);
    std::printf("[ OK ] the device re-opens at another rung with its sources\n");
}

}  // namespace

int main()
{
    // The audio thread never allocates (bug hunt 2026-11-07): the mixer's
    // scratch covers the largest rung, and an EventRing hands back a popped
    // tail with push_front whatever the producer added in between.
    {
        pom2::MixBusState mix;
        assert(mix.tmpL.size() >= pom2::AudioBufferPolicy::kMaxBrowserPeriodFrames);
        pom2::EventRing<int> ring(4);
        for (int i = 0; i < 100; ++i) ring.push_back(i);          // grows: producer side
        std::vector<int> window;
        while (!ring.empty() && ring.front() < 60) { window.push_back(ring.front()); ring.pop_front(); }
        for (int i = 100; i < 140; ++i) ring.push_back(i);         // the producer runs on
        for (size_t k = window.size(); k-- > 10; )                 // consumer reached 10
            assert(ring.push_front(window[k]) && "a popped tail always fits back");
        for (int expect = 10; expect < 140; ++expect) {
            assert(!ring.empty() && ring.front() == expect);
            ring.pop_front();
        }
        assert(ring.empty());
    }
    // WASM-5 (bug hunt 2026-11-01 round 3): a PINNED rung below the browser
    // default is kept at the next load; a LEARNED one below it is stale.
    {
        const auto B = P::Platform::Browser;
        assert(P::startRung(B, 512, true) == 512);
        assert(P::startRung(B, 512, false) == P::defaultPeriodFrames(B));
        assert(P::startRung(B, 4096, false) == 4096);
        assert(P::startRung(B, 4096, true) == 4096);
        assert(P::startRung(B, 300, true) == P::defaultPeriodFrames(B));   // not a rung
        assert(P::startRung(B, 0, false) == P::defaultPeriodFrames(B));
    }

    testRungs();
    testLateCallback();
    testClimb();
    testReopenKeepsSources();
    std::printf("audio_buffer_policy: all assertions passed\n");
    return 0;
}
