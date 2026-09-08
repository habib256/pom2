// The floppy sounds must not crackle — 2026-09-08 (A2 File Cmd report:
// "crackling, and only the disk sounds are playing").
//
// A crackle is a discontinuity in the rendered stream: a jump between two
// consecutive output frames far larger than anything inside the samples
// themselves. Three places produced one:
//   1. the LOOP WRAP of the motor and seek samples — MAME's recordings are
//      not loop-clean (525_spin_loaded: the wrap jumps 409 where the sample's
//      largest internal step is 105), so a spinning motor clicked at the
//      sample's period, 5 Hz for the loaded spin;
//   2. a RETRIGGERED click — an isolated step arriving while the previous
//      step_1_1 (75 ms) was still playing restarted it at frame 0, and a
//      file manager reading scattered blocks does exactly that;
//   3. a SEEK sample switch or the seek→landing transition, which reset the
//      cursor of a loop mid-wave.
// The metric: count the frames whose |Δ| exceeds kJump, a bar set at twice
// the largest intra-sample step of any 5.25" sample (582/32768).

#include "CpuClock.h"
#include "FloppySoundDevice.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
void fail(const std::string& what) { std::printf("FAIL: %s\n", what.c_str()); ++g_failures; }

std::string findSampleDir()
{
    for (const char* p : { "roms/floppy_samples", "../roms/floppy_samples",
                           "../../roms/floppy_samples" })
        if (fs::is_directory(p)) return p;
    return {};
}

constexpr uint32_t kRate  = 44100;
constexpr int      kBlock = 256;
constexpr float    kJump  = 2.0f * 582.0f / 32768.0f;   // twice the worst intra-sample step

uint64_t cyclesForMs(double ms) { return static_cast<uint64_t>(ms * POM2_CPU_CLOCK_HZ / 1000.0); }

struct Render {
    int   jumps = 0;
    float maxJump = 0.0f;
    float rms = 0.0f;
};

// Render `ms` of audio, feeding `events` (emulated-ms → step) as their time
// comes: the device classifies steps by emulated gap, and its wall-clock
// hooks (seek timeout, motor hold-off) run on the frame counter.
Render render(FloppySoundDevice& d, double ms, const std::vector<double>& stepAtMs)
{
    Render r;
    std::vector<float> buf(kBlock);
    float prev = 0.0f;
    double energy = 0.0; size_t count = 0;
    size_t next = 0;
    const double blockMs = 1000.0 * kBlock / kRate;
    for (double t = 0.0; t < ms; t += blockMs) {
        while (next < stepAtMs.size() && stepAtMs[next] < t + blockMs) {
            d.step(0, cyclesForMs(stepAtMs[next]));
            ++next;
        }
        std::fill(buf.begin(), buf.end(), 0.0f);
        d.fillAudioBuffer(buf.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const float v = buf[static_cast<size_t>(i)];
            const float dv = std::fabs(v - prev);
            if (dv > kJump) {
                if (r.jumps < 6)
                    std::printf("    jump %.4f at %.1f ms (%.4f -> %.4f)\n", dv,
                                t + 1000.0 * i / kRate, prev, v);
                ++r.jumps;
            }
            if (dv > r.maxJump) r.maxJump = dv;
            prev = v;
            energy += static_cast<double>(v) * v; ++count;
        }
    }
    r.rms = count ? static_cast<float>(std::sqrt(energy / static_cast<double>(count))) : 0.0f;
    return r;
}

void report(const char* what, const Render& r, int allowed)
{
    std::printf("  %-44s jumps %4d (max Δ %.4f, rms %.4f)\n", what, r.jumps, r.maxJump, r.rms);
    if (r.jumps > allowed)
        fail(std::string(what) + ": " + std::to_string(r.jumps) + " discontinuities, allowed " +
             std::to_string(allowed));
}

}  // namespace

int main()
{
    const std::string dir = findSampleDir();
    if (dir.empty()) { std::puts("SKIP floppy_sound_crackle: no roms/floppy_samples"); return 77; }

    // 1. A spinning motor, alone, for two seconds: no click at the loop period.
    {
        FloppySoundDevice d;
        if (!d.loadSamples(dir)) { fail("loadSamples"); return 1; }
        d.setSampleRate(kRate); d.setVolume(1.0f);
        d.motor(true, true);
        (void)render(d, 400.0, {});          // past the spin-up one-shot
        report("motor loop, 2 s", render(d, 2000.0, {}), 0);
    }
    // 2. Isolated clicks every 60 ms — past the 50 ms seek window, inside the
    //    75 ms click sample: each one used to restart the previous mid-wave.
    {
        FloppySoundDevice d;
        d.loadSamples(dir); d.setSampleRate(kRate); d.setVolume(1.0f);
        std::vector<double> at;
        for (double t = 100.0; t < 1900.0; t += 60.0) at.push_back(t);
        report("30 clicks 60 ms apart (retrigger)", render(d, 2000.0, at), 0);
    }
    // 3. A seek whose cadence changes class (2 → 6 → 12 ms) and then lands.
    {
        FloppySoundDevice d;
        d.loadSamples(dir); d.setSampleRate(kRate); d.setVolume(1.0f);
        std::vector<double> at;
        double t = 100.0;
        for (int i = 0; i < 60; ++i) { at.push_back(t); t += 2.0; }
        for (int i = 0; i < 30; ++i) { at.push_back(t); t += 6.0; }
        for (int i = 0; i < 20; ++i) { at.push_back(t); t += 12.0; }
        report("seek 2→6→12 ms, then landing", render(d, 1200.0, at), 0);
    }
    // 4. Motor + a file manager's mix: bursts of block reads (3 ms apart)
    //    separated by 80 ms of thinking, for two seconds.
    {
        FloppySoundDevice d;
        d.loadSamples(dir); d.setSampleRate(kRate); d.setVolume(1.0f);
        d.motor(true, true);
        std::vector<double> at;
        double t = 400.0;
        while (t < 2300.0) { for (int i = 0; i < 6; ++i) { at.push_back(t); t += 3.0; } t += 80.0; }
        report("motor + block bursts (file manager)", render(d, 2400.0, at), 0);
    }
    if (g_failures) return 1;
    std::puts("floppy_sound_crackle OK");
    return 0;
}
