// The Phasor's AY writes land at their cycle stamp — 2026-09-09.
//
// Until today the Phasor's audio thread took a snapshot of the four
// register banks per buffer, so a register change landed wherever the
// buffer boundary fell (5-10 ms of jitter) — the one thing that kept the
// card *Partial* in the MAME parity dashboard while the Mockingboard has
// replayed a cycle-stamped queue since 2026-08-01. Same pin as the
// Mockingboard's: toggle channel A's volume every 1000 cycles, a 511 Hz
// square wave written by the CPU; measure the frequency the audio thread
// renders. Quantised to the buffer, the writes collapse onto buffer edges
// and the measured frequency is off by a wide margin; stamped, it is the
// written one. Mockingboard-compat mode (VIA 0 → AY 0) and Phasor-native
// mode (both chips of VIA 0, doubled clock) are both pinned.

#include "M6502.h"
#include "Memory.h"
#include "PhasorCard.h"
#include "Via6522.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kSr = 44100;

uint8_t makePb(bool bc1, bool bdir, bool selPrimary, bool selSecondary)
{
    uint8_t pb = 0xFF;
    if (bc1)  pb |= 0x01; else pb &= ~0x01;
    if (bdir) pb |= 0x02; else pb &= ~0x02;
    pb |= 0x04;                                   // /RESET released
    if (selPrimary)   pb &= ~0x10; else pb |= 0x10;
    if (selSecondary) pb &= ~0x08; else pb |= 0x08;
    return pb;
}

uint8_t viaBase(const PhasorCard& card, int viaIdx)
{
    if (viaIdx != 0) return 0x80;
    return (card.mode() == PhasorCard::PH_Mockingboard) ? 0x00 : 0x10;
}

void ayWrite(PhasorCard& card, int viaIdx, uint8_t regAddr, uint8_t data)
{
    const uint8_t base = viaBase(card, viaIdx);
    const uint8_t pb = makePb(false, false, true, false);
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRA, 0xFF);
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRB, 0xFF);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  regAddr);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB, static_cast<uint8_t>((pb & ~0x03) | 0x03));
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB, static_cast<uint8_t>(pb & ~0x03));
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  data);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB, static_cast<uint8_t>((pb & ~0x03) | 0x02));
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB, static_cast<uint8_t>(pb & ~0x03));
}

double measurePwm(bool native)
{
    Memory mem;
    M6502  cpu(&mem);
    PhasorCard card(4);
    card.setCpu(&cpu);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);
    if (native) {
        card.deviceSelectWrite(0x08, 0);      // force Mockingboard mode…
        card.deviceSelectWrite(0x05, 0);      // …then set bits to 5 = Phasor
        assert(card.mode() == PhasorCard::PH_Phasor);
    }
    ayWrite(card, 0, 7, 0x3F);                // all channels off (tone+noise)
    ayWrite(card, 0, 8, 0x00);                // channel A volume 0

    constexpr int kHalfPeriodCycles = 1000;
    const double kExpectedHz = 1022727.0 / (2.0 * kHalfPeriodCycles);
    constexpr int kFrameCycles  = 17045;
    constexpr int kFrameSamples = 735;
    constexpr int kChunk        = 245;
    constexpr int kFrames       = 40;

    AudioSource* src = card.audioSource();
    assert(src);
    std::vector<float> all;
    std::vector<float> chunk(kChunk);
    int  cyclesIntoHalf = 0;
    bool high = false;
    for (int f = 0; f < kFrames; ++f) {
        int remaining = kFrameCycles;
        while (remaining > 0) {
            const int step = std::min(remaining, kHalfPeriodCycles - cyclesIntoHalf);
            mem.advanceCycles(step);
            cyclesIntoHalf += step;
            remaining      -= step;
            if (cyclesIntoHalf >= kHalfPeriodCycles) {
                cyclesIntoHalf = 0;
                high = !high;
                ayWrite(card, 0, 8, high ? 0x0F : 0x00);
            }
        }
        for (int c = 0; c < kFrameSamples / kChunk; ++c) {
            src->fillAudioBuffer(chunk.data(), kChunk);
            all.insert(all.end(), chunk.begin(), chunk.end());
        }
    }
    const size_t skip = all.size() / 2;
    std::vector<float> tail(all.begin() + static_cast<ptrdiff_t>(skip), all.end());
    double mean = 0.0;
    for (float s : tail) mean += s;
    mean /= static_cast<double>(tail.size());
    int crossings = 0;
    for (size_t i = 1; i < tail.size(); ++i)
        if (tail[i - 1] <= mean && tail[i] > mean) ++crossings;
    const double measured = crossings * static_cast<double>(kSr) / static_cast<double>(tail.size());
    double sumSq = 0.0;
    for (float s : tail) sumSq += static_cast<double>(s) * s;
    const double rms = std::sqrt(sumSq / static_cast<double>(tail.size()));
    std::printf("  %s: volume PWM measured=%.1f Hz expected=%.1f Hz rms=%.4f\n",
                native ? "Phasor-native" : "MB-compat", measured, kExpectedHz, rms);
    assert(rms > 0.01 && "the PWM was not audible");
    return measured / kExpectedHz;
}

void testCompatibilityResetClearsHiddenAudio()
{
    Memory mem;
    M6502 cpu(&mem);
    PhasorCard card(4), reference(4);
    for (auto* target : {&card, &reference}) {
        target->setCpu(&cpu);
        target->setSampleRate(kSr);
        target->setVolume(1.0f);
        target->deviceSelectWrite(0x05, 0);
        // Broadcast writes to both AYs: PB3=PB4=0, PB2=1.
        target->slotRomWrite(0x13, 0xFF);
        target->slotRomWrite(0x12, 0xFF);
        for (auto [reg, value] : {std::pair<uint8_t, uint8_t>{0, 73},
                                  {1, 0}, {7, 0x3E}, {8, 15}}) {
            target->slotRomWrite(0x11, reg);
            target->slotRomWrite(0x10, 7);
            target->slotRomWrite(0x10, 4);
            target->slotRomWrite(0x11, value);
            target->slotRomWrite(0x10, 6);
            target->slotRomWrite(0x10, 4);
        }
    }
    std::vector<float> actual(245), expected(245);
    double beforeEnergy = 0.0;
    for (int block = 0; block < 30; ++block) {
        mem.advanceCycles(5682);
        // Keep both producers synced without altering a timer/IRQ latch.
        (void)card.slotRomRead(0x12);
        (void)reference.slotRomRead(0x12);
        if (block == 6) {
            // Equal-cycle reset on the same pins, one while AY2 is hidden.
            card.deviceSelectWrite(0x08, 0);
            card.slotRomWrite(0x00, 0x18); // /RESET low, neither AY selected
            reference.slotRomWrite(0x10, 0x18);
            card.deviceSelectWrite(0x05, 0);
        }
        card.audioSource()->fillAudioBuffer(actual.data(), 245);
        reference.audioSource()->fillAudioBuffer(expected.data(), 245);
        assert(actual == expected && "compatibility reset retained hidden AY audio");
        if (block < 6)
            for (float v : actual) beforeEnergy += v * v;
    }
    assert(beforeEnergy > 0.01 && "reset audio fixture must start audible");
}

void testRejectedSnapshotPreservesAudio(bool native)
{
    for (int invalid = 0; invalid < 4; ++invalid) {
        Memory mem;
        M6502 cpu(&mem);
        PhasorCard card(4), reference(4);
        for (auto* target : {&card, &reference}) {
            target->setCpu(&cpu);
            target->setSampleRate(kSr);
            target->setVolume(1.0f);
            if (native) {
                target->deviceSelectWrite(0x08, 0);
                target->deviceSelectWrite(0x05, 0);
            }
            ayWrite(*target, 0, 0, 100);
            ayWrite(*target, 0, 7, 0x3E);
            ayWrite(*target, 0, 8, 15);
        }
        std::vector<float> actual(245), expected(245);
        card.audioSource()->fillAudioBuffer(actual.data(), 245);
        reference.audioSource()->fillAudioBuffer(expected.data(), 245);
        assert(actual == expected);
        // Leave a future volume write queued while the audio cursor is
        // behind CPU-now. Rejecting a blob must preserve that backlog and
        // the currently audible tone, not re-prime from CPU's zero volume.
        mem.advanceCycles(60000);
        ayWrite(card, 0, 8, 0);
        ayWrite(reference, 0, 8, 0);
        std::vector<uint8_t> blob;
        card.appendSnapshotState(blob);
        switch (invalid) {
            case 0: blob.clear(); break;
            case 1: blob[0] = 'X'; break;
            case 2: blob[3] = 99; break;
            case 3: blob.pop_back(); break;
        }
        card.loadSnapshotState(blob.data(), blob.size());
        double energy = 0.0;
        for (int block = 0; block < 8; ++block) {
            card.audioSource()->fillAudioBuffer(actual.data(), 245);
            reference.audioSource()->fillAudioBuffer(expected.data(), 245);
            for (float sample : expected) energy += sample * sample;
            if (actual != expected) {
                std::fprintf(stderr, "rejected Phasor snapshot changed audio: native=%d case=%d block=%d\n",
                             native, invalid, block);
                std::abort();
            }
        }
        assert(energy > 0.01);
    }
}

}  // namespace

bool testEchoStereo()
{
    PhasorCard echo(4);
    echo.setSampleRate(kSr);
    echo.setVolume(1.0f);
    echo.deviceSelectWrite(0x0F, 0);
    ayWrite(echo, 1, 0, 73);
    ayWrite(echo, 1, 7, 0x3E);
    ayWrite(echo, 1, 8, 15);
    std::vector<float> left(2048), right(2048);
    echo.audioSource()->fillAudioBufferStereo(left.data(), right.data(), 2048);
    double energy = 0;
    for (float v : right) energy += v * v;
    if (left != right || energy < 0.01) {
        std::fprintf(stderr, "Phasor Echo+ must output the second VIA pair to BOTH speakers\n");
        return false;
    }
    // A future compat->Echo change has the SAME clock scale, but changes
    // routing at its stamp; it must not change earlier audio buffers.
    Memory mem;
    M6502 cpu(&mem);
    PhasorCard queued(4);
    queued.setCpu(&cpu);
    queued.setSampleRate(kSr);
    queued.setVolume(1.0f);
    ayWrite(queued, 1, 0, 73);
    ayWrite(queued, 1, 7, 0x3E);
    ayWrite(queued, 1, 8, 15);
    left.resize(245); right.resize(245);
    queued.audioSource()->fillAudioBufferStereo(left.data(), right.data(), 245);
    mem.advanceCycles(60000);
    queued.deviceSelectWrite(0x0F, 0);
    double lateEnergy = 0;
    for (int block = 0; block < 18; ++block) {
        // Feed the producer too: a stopped CPU intentionally leaves the
        // jitter buffer's final half-burst pending rather than outrunning it.
        mem.advanceCycles(5682);
        (void)queued.slotRomRead(0x82);
        queued.audioSource()->fillAudioBufferStereo(left.data(), right.data(), 245);
        for (int i = 0; i < 245; ++i) {
            if (block < 7 && left[i] != 0) {
                std::fprintf(stderr, "Echo+ routing arrived early: block=%d sample=%d value=%g\n", block, i, left[i]);
                return false;
            }
            if (block > 12) lateEnergy += left[i] * left[i];
        }
    }
    if (lateEnergy <= 0.01) std::fprintf(stderr, "queued Echo+ routing never arrived: energy=%g\n", lateEnergy);
    return lateEnergy > 0.01;
}

bool testMutedPhasorContinuity()
{
    for (int mode : {0, 5, 7}) {
        Memory mem;
        M6502 cpu(&mem);
        PhasorCard card(4), reference(4);
        for (auto* target : {&card, &reference}) {
            target->setCpu(&cpu);
            target->setSampleRate(kSr);
            target->setVolume(1.0f);
            target->deviceSelectWrite(0x08 | mode, 0);
            ayWrite(*target, 0, 0, 73);
            ayWrite(*target, 0, 7, 0x3E);
            ayWrite(*target, 0, 8, 15);
        }
        std::vector<float> actual(257), expected(257);
        float gain = 1;
        const float step = 1.0f / (0.005f * kSr);
        for (int block = 0; block < 26; ++block) {
            mem.advanceCycles(5960);
            const auto address = viaBase(card, 0) + 2;
            (void)card.slotRomRead(address);
            (void)reference.slotRomRead(address);
            if (block == 7) {
                ayWrite(card, 0, 0, 51);
                ayWrite(reference, 0, 0, 51);
            }
            const bool muted = block >= 2 && block < 15;
            card.setMuted(muted);
            card.audioSource()->fillAudioBuffer(actual.data(), 257);
            reference.audioSource()->fillAudioBuffer(expected.data(), 257);
            for (int i = 0; i < 257; ++i) {
                gain = muted ? std::max(0.0f, gain - step) : std::min(1.0f, gain + step);
                if (std::abs(actual[i] - expected[i] * gain) > 1e-7f) {
                    std::fprintf(stderr, "Phasor mute paused audio: mode=%d block=%d sample=%d\n", mode, block, i);
                    return false;
                }
            }
        }
    }
    return true;
}

int main()
{
    const bool mute = testMutedPhasorContinuity();
    const bool echo = testEchoStereo();
    assert(mute && echo);
    testCompatibilityResetClearsHiddenAudio();
    testRejectedSnapshotPreservesAudio(false);
    testRejectedSnapshotPreservesAudio(true);
    const double a = measurePwm(false);
    const double b = measurePwm(true);
    assert(a > 0.90 && a < 1.10 && "MB-compat: the writes did not land at their cycle stamp");
    assert(b > 0.90 && b < 1.10 && "Phasor-native: the writes did not land at their cycle stamp");
    std::puts("phasor_timeline OK");
    return 0;
}
