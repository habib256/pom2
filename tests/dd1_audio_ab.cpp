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

// DIGIDREAM 1 (French Touch) Mockingboard audio A/B probe — diagnostic,
// NOT a pinned test (needs disks_5.4/demo/digidream/DD.dsk + roms/).
//
// Why
// ---
// DD1 drives BOTH AY chips, adds a ~6.8 kHz 4-bit PCM "digidrum" written
// into a volume register from a VIA1 T1 IRQ, and runs a hardware-envelope
// buzzer on channel A. That is the densest register traffic in the corpus,
// so it is the material that exposes any regression in the audio-thread
// replay path. This probe renders DD1's real boot to a .wav so two builds
// of `Mockingboard.cpp` (worktree vs `git show HEAD:`) can be compared
// sample-for-sample.
//
// What it does
// ------------
// Boots DD.dsk on //e Enhanced PAL (128 K) with a Disk II in slot 6 and a
// MockingboardCard in slot 4, then drives the machine in the SAME cadence
// the real emulator uses:
//
//   per wall-clock 50 Hz frame:
//     * run one video frame of CPU cycles (20313 PAL — or 1'000'000 when
//       the Disk II motor is on, which is exactly what MainWindow's
//       `disk_turbo` does, MainWindow.cpp:6789-6797);
//     * pull 882 audio samples (44100/50) from the card's AudioSource in
//       256-frame buffers, carrying the fractional remainder — the same
//       granularity mismatch the real AudioDevice callback has.
//
// While stepping it decodes the AY control bus through `peekViaRegister`
// exactly as `dd2_ay_trace` does, so the register write log carries a CPU
// cycle stamp and can be correlated against the rendered audio.
//
// Outputs (prefix from argv[3], default ./dd1):
//   <prefix>.wav   16-bit mono 44.1 kHz render
//   <prefix>.csv   AY write log: cycle,chip,reg,val
//   <prefix>.frames.csv  per-frame: frame,cycle,motor,ayWrites,samples
//
// Env POM2_MB_TRACE=<path> is honoured by the *instrumented* copy of
// Mockingboard.cpp only (see tests/CMakeLists.txt dd1_audio_instr).
//
// Usage: build/tests/dd1_audio_ab [wall-seconds] [disk] [out-prefix] [turbo0|1]
//                                 [jitter-samples] [buffer-frames]
//
// `jitter-samples` models the fact that the audio callback and the CPU
// worker frame are driven by two INDEPENDENT clocks (the sound card's and
// steady_clock + OS scheduling). Their relative phase random-walks; the
// argument bounds that walk in samples (441 = ±10 ms, a normal scheduling
// hiccup). 0 = the idealised, perfectly locked cadence.
// `buffer-frames` is the device period size (AudioDevice.cpp:187 asks for
// 256, but the backend may hand back 480/512/1024).

#include "AyPsgSynth.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "CpuClock.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string findFirst(std::initializer_list<const char*> cands)
{
    std::error_code ec;
    for (const char* c : cands)
        if (std::filesystem::is_regular_file(c, ec)) return c;
    return {};
}

struct AyWrite { uint64_t cycle; uint8_t chip; uint8_t reg; uint8_t val; };

void writeWav(const std::string& path, const std::vector<float>& s, uint32_t sr)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return; }
    const uint32_t n = static_cast<uint32_t>(s.size());
    const uint32_t dataBytes = n * 2u;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
    u32(sr); u32(sr * 2); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    std::vector<int16_t> pcm(n);
    for (uint32_t i = 0; i < n; ++i) {
        float v = s[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = static_cast<int16_t>(v * 32767.0f);
    }
    std::fwrite(pcm.data(), 2, n, f);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv)
{
    const double wallSecs = (argc > 1) ? std::atof(argv[1]) : 45.0;
    const std::string rom  = findFirst({"../roms/apple2e.rom", "roms/apple2e.rom"});
    const std::string boot = findFirst({"../roms/disk2.rom",   "roms/disk2.rom"});
    const std::string p6   = findFirst({"../roms/diskii_p6.rom", "roms/diskii_p6.rom"});
    const std::string dsk  = (argc > 2 && argv[2][0]) ? std::string(argv[2]) : findFirst({
        "../disks_5.4/demo/digidream/DD.dsk",
        "disks_5.4/demo/digidream/DD.dsk"});
    const std::string prefix = (argc > 3) ? std::string(argv[3]) : std::string("dd1");
    const bool turbo = (argc > 4) ? (std::atoi(argv[4]) != 0) : true;
    const int  jitter = (argc > 5) ? std::atoi(argv[5]) : 0;
    const int  bufFrames = (argc > 6) ? std::atoi(argv[6]) : 256;
    // Models a host that cannot quite hold 50 Hz: <1.0 shortens the CPU
    // budget every frame, so the producer falls behind the audio clock.
    const double speed = (argc > 7) ? std::atof(argv[7]) : 1.0;
    // Models an occasional scheduling stall: every Nth frame runs no CPU
    // cycles at all while the audio device keeps pulling.
    const int dropEvery = (argc > 8) ? std::atoi(argv[8]) : 0;
    if (rom.empty() || boot.empty() || dsk.empty()) {
        std::printf("dd1_audio_ab SKIP: missing apple2e.rom / disk2.rom / DD.dsk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/false)) {
        std::fprintf(stderr, "loadAppleIIRom failed\n"); return 1;
    }

    auto diskOwned = std::make_unique<DiskIICard>();
    DiskIICard* disk = diskOwned.get();
    if (!disk->loadBootRom(boot) || !disk->insertDisk(dsk)) {
        std::fprintf(stderr, "Disk II setup failed\n"); return 1;
    }
    if (!p6.empty()) disk->loadLssRom(p6);
    mem.slotBus().plug(6, std::move(diskOwned));

    auto mbOwned = std::make_unique<MockingboardCard>(4);
    MockingboardCard* mb = mbOwned.get();
    mem.slotBus().plug(4, std::move(mbOwned));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    // MUST mirror MainWindow::plugMockingboard (MainWindow.cpp:1530): without
    // it `cpu_` stays null, `lastSyncCycle_` never leaves 0, EVERY AY event is
    // stamped cycle 0 and `latestAyEventCycle_` never rises — which silently
    // disables the whole cycle-stamped replay path being measured here.
    mb->setCpu(&cpu);
    cpu.hardReset();
    mem.slotBus().reset();

    const auto vt = pom2VideoTiming(VideoStandard::PAL);
    const double cpuHz = static_cast<double>(vt.cpuClockHz);
    constexpr uint32_t kSampleRate = 44100;
    const int          kBufFrames  = bufFrames;

    // Same wiring MainWindow does: the card learns the live CPU clock and
    // the device sample rate before any audio is pulled.
    mb->setSampleRate(kSampleRate);
    mb->setCpuClock(cpuHz);
    mb->setVolume(1.0f);            // unity so the render is the raw mix
    AudioSource* src = mb->audioSource();

    const int    totalFrames    = static_cast<int>(wallSecs * vt.refreshHz);
    const double samplesPerFrame =
        static_cast<double>(kSampleRate) / static_cast<double>(vt.refreshHz);

    std::vector<AyWrite> log;
    log.reserve(4u << 20);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(wallSecs * kSampleRate) + 4096);
    std::vector<float> buf(kBufFrames);

    uint8_t prevPb[2] = {0xFF, 0xFF};
    uint8_t latch[2]  = {0, 0};

    FILE* fr = std::fopen((prefix + ".frames.csv").c_str(), "w");
    if (fr) std::fprintf(fr, "frame,cycle,motor,ayWritesCum,samplesCum\n");

    double samplesOwed = 0.0;
    // Bounded random walk of the audio/CPU phase — see the usage note.
    uint32_t rng = 12345;
    double   phase = 0.0;
    uint64_t turboFrames = 0;
    for (int fnum = 0; fnum < totalFrames; ++fnum) {
        const bool motor = disk->isMotorOn();
        int64_t budget = (turbo && motor) ? 1'000'000
                       : static_cast<int64_t>(vt.cyclesPerFrame * speed);
        if (dropEvery > 0 && !motor && (fnum % dropEvery) == 0) budget = 0;
        if (turbo && motor) ++turboFrames;
        const uint64_t target = cpu.getCycleCountNow() + static_cast<uint64_t>(budget);
        while (cpu.getCycleCountNow() < target) {
            cpu.step();
            for (int ci = 0; ci < 2; ++ci) {
                const uint8_t pb = static_cast<uint8_t>(mb->peekViaRegister(ci, 0) & 0x07);
                if (pb == prevPb[ci]) continue;
                prevPb[ci] = pb;
                if (pb == 0x07) {
                    latch[ci] = static_cast<uint8_t>(mb->peekViaRegister(ci, 1) & 0x0F);
                } else if (pb == 0x06) {
                    log.push_back({cpu.getCycleCountNow(), static_cast<uint8_t>(ci),
                                   latch[ci], mb->peekViaRegister(ci, 1)});
                }
            }
        }
        double dPhase = 0.0;
        if (jitter > 0) {
            rng = rng * 1103515245u + 12345u;
            const double r = ((rng >> 16) & 0x7FFF) / 32767.0 * 2.0 - 1.0;
            double np = phase + r * (jitter / 8.0);
            if (np >  jitter) np =  jitter;
            if (np < -jitter) np = -jitter;
            dPhase = np - phase;
            phase  = np;
        }
        samplesOwed += samplesPerFrame + dPhase;
        while (samplesOwed >= kBufFrames) {
            src->fillAudioBuffer(buf.data(), kBufFrames);
            out.insert(out.end(), buf.begin(), buf.end());
            samplesOwed -= kBufFrames;
        }
        if (fr) std::fprintf(fr, "%d,%llu,%d,%zu,%zu\n", fnum,
                             (unsigned long long)cpu.getCycleCountNow(),
                             motor ? 1 : 0, log.size(), out.size());
    }
    if (fr) std::fclose(fr);

    writeWav(prefix + ".wav", out, kSampleRate);

    // ── ORACLE render ────────────────────────────────────────────────────
    // Ground truth for the replay path: the SAME synthesis core driven
    // straight off the cycle-stamped write log, with no queue, no cursor
    // lag and no buffer granularity. Whichever build's output correlates
    // better with this is the one whose register timing is more faithful.
    // `point=true` swaps the box integrator for the pre-2026-08-01 point
    // sampler so the old build can be scored against a like-for-like oracle.
    auto oracle = [&](bool point, bool dcBlock) {
        std::vector<float> o;
        o.reserve(out.size());
        pom2::ay::ChipSynthState cs[2];
        pom2::ay::DcBlocker      db;
        db.setRate(kSampleRate);
        uint8_t regs[2][16] = {};
        const double cps = cpuHz / static_cast<double>(kSampleRate);
        const float  tps = static_cast<float>(cpuHz / 8.0 / kSampleRate);
        const float  itps = 1.0f / tps;
        double   frac = 0.0;
        uint64_t cur  = 0;
        size_t   ei   = 0;
        for (size_t i = 0; i < out.size(); ++i) {
            frac += cps;
            const uint64_t whole = static_cast<uint64_t>(frac);
            cur += whole; frac -= static_cast<double>(whole);
            while (ei < log.size() && log[ei].cycle <= cur) {
                const auto& e = log[ei++];
                regs[e.chip][e.reg] = e.val;
                if (e.reg == 13) cs[e.chip].envRetrigger = true;
            }
            float smp = 0.0f;
            for (int ci = 0; ci < 2; ++ci) {
                if (point) {
                    pom2::ay::applyEnvShape(cs[ci], regs[ci]);
                    cs[ci].tickPhase += tps;
                    while (cs[ci].tickPhase >= 1.0f) {
                        cs[ci].tickPhase -= 1.0f;
                        pom2::ay::stepTick(cs[ci], regs[ci]);
                    }
                    smp += pom2::ay::chipLevel(cs[ci], regs[ci]);
                } else {
                    smp += pom2::ay::renderChipSample(cs[ci], regs[ci], tps, itps);
                }
            }
            smp *= (1.0f / 6.0f);
            o.push_back(dcBlock ? db.process(smp) : smp);
        }
        return o;
    };
    writeWav(prefix + "_oracle_box.wav", oracle(false, true), kSampleRate);
    writeWav(prefix + "_oracle_pt.wav",  oracle(true,  true), kSampleRate);
    if (FILE* f = std::fopen((prefix + ".csv").c_str(), "w")) {
        for (const auto& w : log)
            std::fprintf(f, "%llu,%u,%u,%u\n", (unsigned long long)w.cycle,
                         w.chip, w.reg, w.val);
        std::fclose(f);
    }

    std::printf("DD1 A/B probe: %.1f wall s (%d frames, %llu in disk turbo), "
                "%llu emulated CPU cycles (%.1f emulated s), PC=$%04X\n",
                wallSecs, totalFrames, (unsigned long long)turboFrames,
                (unsigned long long)cpu.getCycleCountNow(),
                cpu.getCycleCountNow() / cpuHz, cpu.getProgramCounter());
    std::printf("AY writes: %zu (chip0=%u chip1=%u), samples rendered: %zu (%.2f s)\n",
                log.size(), mb->getAyWriteCount(0), mb->getAyWriteCount(1),
                out.size(), out.size() / double(kSampleRate));

    // ── register histogram per chip ──────────────────────────────────────
    uint32_t hist[2][16] = {};
    for (const auto& w : log) hist[w.chip][w.reg]++;
    std::printf("\nregister write histogram\n  reg   AY1($C400)   AY2($C480)\n");
    for (int r = 0; r < 16; ++r)
        if (hist[0][r] || hist[1][r])
            std::printf("  R%-2d %10u %12u\n", r, hist[0][r], hist[1][r]);

    // ── R13 (envelope shape) histogram — hypothesis (e) ─────────────────
    std::map<int, uint32_t> shape[2];
    for (const auto& w : log) if (w.reg == 13) shape[w.chip][w.val & 0x0F]++;
    for (int c = 0; c < 2; ++c) {
        if (shape[c].empty()) continue;
        std::printf("\nR13 shapes on AY%d: ", c + 1);
        for (auto& [s, n] : shape[c]) {
            const bool cont = (s & 0x08) != 0;
            const bool hold = cont ? ((s & 0x01) != 0) : true;
            std::printf("$%X(%u,%s) ", s, n, hold ? "HOLD" : "cont");
        }
        std::putchar('\n');
    }

    // ── digidrum: amplitude-register writes closer than one replay tick ──
    for (int c = 0; c < 2; ++c) {
        for (int rg = 8; rg <= 10; ++rg) {
            std::vector<uint64_t> dg;
            uint64_t p = 0;
            for (const auto& w : log)
                if (w.chip == c && w.reg == rg) {
                    if (p && w.cycle - p < 1000) dg.push_back(w.cycle - p);
                    p = w.cycle;
                }
            if (dg.size() > 50) {
                std::sort(dg.begin(), dg.end());
                std::printf("digidrum AY%d R%d: n=%zu median gap %llu cycles"
                            " -> %.0f Hz\n", c + 1, rg, dg.size(),
                            (unsigned long long)dg[dg.size() / 2],
                            cpuHz / double(dg[dg.size() / 2]));
            }
        }
    }

    // ── short-time RMS envelope (10 ms hops), printed sparsely ──────────
    {
        const size_t hop = kSampleRate / 100;
        std::printf("\nshort-time RMS (100 Hz hop) — first non-silent hop and "
                    "per-second mean/peak\n");
        size_t firstNz = 0;
        for (size_t i = 0; i + hop <= out.size(); i += hop) {
            double e = 0; for (size_t k = 0; k < hop; ++k) e += out[i+k]*out[i+k];
            if (std::sqrt(e / hop) > 1e-4) { firstNz = i; break; }
        }
        std::printf("  first audio at %.2f s\n", firstNz / double(kSampleRate));
        for (size_t s0 = 0; s0 + kSampleRate <= out.size(); s0 += kSampleRate) {
            double mean = 0, peak = 0; int nh = 0;
            for (size_t i = s0; i + hop <= s0 + kSampleRate; i += hop) {
                double e = 0; for (size_t k = 0; k < hop; ++k) e += out[i+k]*out[i+k];
                const double r = std::sqrt(e / hop);
                mean += r; peak = std::max(peak, r); ++nh;
            }
            std::printf("  t=%3.0fs rms_mean=%.5f rms_peak=%.5f\n",
                        s0 / double(kSampleRate), mean / nh, peak);
        }
    }
    return 0;
}
