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

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr uint32_t kSr = 44100;

uint8_t makePb(bool bc1, bool bdir, bool selPrimary, bool selSecondary)
{
    uint8_t pb = 0xFF;
    if (bc1)  pb |= 0x01; else pb &= ~0x01;
    if (bdir) pb |= 0x02; else pb &= ~0x02;
    pb |= 0x04;                                   // /RESET released
    if (selPrimary)   pb &= ~0x08; else pb |= 0x08;
    if (selSecondary) pb &= ~0x10; else pb |= 0x10;
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

}  // namespace

int main()
{
    const double a = measurePwm(false);
    const double b = measurePwm(true);
    assert(a > 0.90 && a < 1.10 && "MB-compat: the writes did not land at their cycle stamp");
    assert(b > 0.90 && b < 1.10 && "Phasor-native: the writes did not land at their cycle stamp");
    std::puts("phasor_timeline OK");
    return 0;
}
