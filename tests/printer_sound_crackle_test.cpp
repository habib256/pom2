// The printer sound must not crackle — 2026-09-09 (A2 File Cmd report:
// "the crackling comes from the printer sound").
//
// Same instrument as `floppy_sound_crackle`: render a realistic job and
// count the frames whose |Δ| exceeds a bar. The grains here are bandpassed
// NOISE, so the natural frame-to-frame step is large; the bar is therefore
// set from the device's own steady-state behaviour — the largest step seen
// in the middle of a sustained buzz — and what is counted is anything
// bigger than that: a grain cut dead, a voice restarted mid-decay, a burst
// of grains summing past full scale.

#include "PrinterSoundDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void fail(const std::string& what) { std::printf("FAIL: %s\n", what.c_str()); ++g_failures; }

constexpr uint32_t kRate  = 44100;
constexpr int      kBlock = 256;

struct Render {
    int   jumps = 0;
    float maxJump = 0.0f;
    float peak = 0.0f;
    float rms = 0.0f;
    int   clipped = 0;
};

// Feed `events` (time in ms → action) while pulling audio in 256-frame blocks.
struct Event { double atMs; int kind; double arg; };   // kind: 0 strike(pins) 1 feed(inches) 2 return(inches)

Render render(pom2::PrinterSoundDevice& d, double ms, std::vector<Event> ev, float bar)
{
    Render r;
    std::sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.atMs < b.atMs; });
    std::vector<float> buf(kBlock);
    float prev = 0.0f;
    double energy = 0.0; size_t count = 0; size_t next = 0;
    const double blockMs = 1000.0 * kBlock / kRate;
    for (double t = 0.0; t < ms; t += blockMs) {
        while (next < ev.size() && ev[next].atMs < t + blockMs) {
            const Event& e = ev[next++];
            if (e.kind == 0) d.strike(static_cast<int>(e.arg));
            else if (e.kind == 1) d.paperFeed(e.arg);
            else d.carriageReturn(e.arg);
        }
        std::fill(buf.begin(), buf.end(), 0.0f);
        d.fillAudioBuffer(buf.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const float v = buf[static_cast<size_t>(i)];
            const float dv = std::fabs(v - prev);
            if (dv > bar) { if (r.jumps < 5) std::printf("    jump %.3f at %.1f ms\n", dv, t + 1000.0 * i / kRate); ++r.jumps; }
            if (dv > r.maxJump) r.maxJump = dv;
            if (std::fabs(v) > r.peak) r.peak = std::fabs(v);
            if (std::fabs(v) > 1.0f) ++r.clipped;
            prev = v;
            energy += static_cast<double>(v) * v; ++count;
        }
    }
    r.rms = count ? static_cast<float>(std::sqrt(energy / static_cast<double>(count))) : 0.0f;
    return r;
}

}  // namespace

int main()
{
    // Calibrate the bar on a sustained 180 cps buzz: the largest step inside
    // steady state, times 1.5.
    float bar = 0.0f;
    {
        pom2::PrinterSoundDevice d;
        d.setSampleRate(kRate); d.setVolume(1.0f);
        std::vector<Event> ev;
        for (double t = 0.0; t < 3000.0; t += 1000.0 / 180.0) ev.push_back({ t, 0, 7 });
        const Render r = render(d, 3000.0, ev, 1e9f);
        bar = r.maxJump * 1.5f;
        std::printf("  steady buzz: max step %.3f, peak %.3f, rms %.3f -> bar %.3f\n", r.maxJump, r.peak, r.rms, bar);
    }
    // 1. A page: 80 characters at 180 cps, carriage return, line feed, ×40.
    {
        pom2::PrinterSoundDevice d;
        d.setSampleRate(kRate); d.setVolume(1.0f);
        std::vector<Event> ev;
        double t = 50.0;
        for (int line = 0; line < 40; ++line) {
            for (int c = 0; c < 80; ++c) { ev.push_back({ t, 0, static_cast<double>(5 + (c % 5)) }); t += 1000.0 / 180.0; }
            ev.push_back({ t, 2, 8.0 });
            ev.push_back({ t, 1, 1.0 / 6.0 });
            t += 250.0;
        }
        const Render r = render(d, t + 600.0, ev, bar);
        std::printf("  a page of text: jumps %d (max %.3f), peak %.3f, clipped frames %d\n", r.jumps, r.maxJump, r.peak, r.clipped);
        if (r.jumps) fail("a page of text: " + std::to_string(r.jumps) + " discontinuities");
        if (r.clipped) fail("a page of text: " + std::to_string(r.clipped) + " frames past full scale");
    }
    // 2. A screen dump: 9-pin graphics at the printer's full rate, 3 s.
    {
        pom2::PrinterSoundDevice d;
        d.setSampleRate(kRate); d.setVolume(1.0f);
        std::vector<Event> ev;
        for (double t = 0.0; t < 3000.0; t += 1000.0 / 600.0) ev.push_back({ t, 0, 9 });
        const Render r = render(d, 3600.0, ev, bar);
        std::printf("  a screen dump (600 strikes/s): jumps %d (max %.3f), peak %.3f, clipped %d\n", r.jumps, r.maxJump, r.peak, r.clipped);
        if (r.jumps) fail("screen dump: " + std::to_string(r.jumps) + " discontinuities");
        if (r.clipped) fail("screen dump: " + std::to_string(r.clipped) + " frames past full scale");
    }
    // 3. Idle: nothing printed, five seconds — must be digital silence.
    {
        pom2::PrinterSoundDevice d;
        d.setSampleRate(kRate); d.setVolume(1.0f);
        const Render r = render(d, 5000.0, {}, bar);
        std::printf("  idle: peak %.6f\n", r.peak);
        if (r.peak > 0.0f) fail("the idle printer is not silent");
    }
    if (g_failures) return 1;
    std::puts("printer_sound_crackle OK");
    return 0;
}
