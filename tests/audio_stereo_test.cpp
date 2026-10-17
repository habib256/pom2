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

// Stereo audio bus test — pins the 2026-08-01 mono → stereo conversion.
//
// The bus carries interleaved stereo since the Mockingboard and the
// Phasor are stereo cards: MAME gives the Mockingboard one 2-channel
// speaker with AY1 on channel 0 and AY2 on channel 1
// (`a2mockingboard.cpp:159-165`), routes the card's speech chip to BOTH
// channels at unity (`:186-189`), and gives the Phasor a second speaker
// so that left = ay1+ay2 (the VIA1 pair) and right = ay3+ay4 (the VIA2
// pair) (`:192-208`).
//
// What must not regress, in order of how expensive the mistake would be:
//
//   1. LEVEL. Making the bus stereo must not move any existing source.
//      A mono source sits at the default centre pan, which is unity on
//      BOTH channels (a balance law, not constant power), and the mono
//      fold-down of a stereo card reproduces its old summed render
//      exactly — `/3` per side folds back to the `/6` the Mockingboard
//      used, `/6` per side back to the Phasor's `/12`.
//   2. PLACEMENT. Chip 0 must be audible on the left and silent on the
//      right, and vice versa. This is the whole point: Digidream 1
//      writes a deliberate A/B/C pan that the mono sum destroyed.
//   3. The mono-downmix switch must give back a centred image.
//
// Self-contained: drives AudioDevice::mixSources and the cards'
// AudioSource directly, no Memory / M6502.

#include "AudioDevice.h"
#include "Mockingboard.h"
#include "PhasorCard.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kFrames = 1024;
constexpr int kSamples = kFrames * static_cast<int>(AudioDevice::kChannels);

// ─── Synthetic sources ────────────────────────────────────────────────

// Mono source emitting a constant — the pan law is easiest to read off
// a DC level.
class ConstMono : public AudioSource
{
public:
    explicit ConstMono(float v) : v_(v) {}
    void fillAudioBuffer(float* out, int n) override
    {
        for (int i = 0; i < n; ++i) out[i] = v_;
    }
private:
    float v_;
};

// Native-stereo source: writes different constants to each plane, so a
// swapped or collapsed channel is immediately visible.
class ConstStereo : public AudioSource
{
public:
    ConstStereo(float l, float r) : l_(l), r_(r) {}
    void fillAudioBuffer(float* out, int n) override
    {
        // Mono fold-down, same convention as the cards.
        for (int i = 0; i < n; ++i) out[i] = 0.5f * (l_ + r_);
    }
    bool fillAudioBufferStereo(float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i) { left[i] = l_; right[i] = r_; }
        return true;
    }
private:
    float l_, r_;
};

double rms(const std::vector<float>& buf, int stride, int offset)
{
    double s = 0.0;
    int n = 0;
    for (size_t i = static_cast<size_t>(offset); i < buf.size();
         i += static_cast<size_t>(stride)) {
        s += static_cast<double>(buf[i]) * buf[i];
        ++n;
    }
    return (n > 0) ? std::sqrt(s / n) : 0.0;
}

// ─── 1: pan law on the mono path ──────────────────────────────────────
void testMonoPanLaw()
{
    AudioDevice dev;
    ConstMono src(0.5f);
    dev.addSource(&src);
    std::vector<float> out(kSamples, 0.0f);

    // Centre (the default) is UNITY on both channels. If this ever
    // becomes 1/sqrt(2), every mono source in POM2 silently loses 3 dB.
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        assert(std::fabs(out[2 * i]     - 0.5f) < 1e-6f);
        assert(std::fabs(out[2 * i + 1] - 0.5f) < 1e-6f);
    }

    // Hard left / hard right.
    src.pan.store(-1.0f);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        assert(std::fabs(out[2 * i] - 0.5f) < 1e-6f);
        assert(out[2 * i + 1] == 0.0f);
    }
    src.pan.store(1.0f);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        assert(out[2 * i] == 0.0f);
        assert(std::fabs(out[2 * i + 1] - 0.5f) < 1e-6f);
    }

    // Half right: left drops, right holds unity.
    src.pan.store(0.5f);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        assert(std::fabs(out[2 * i]     - 0.25f) < 1e-6f);
        assert(std::fabs(out[2 * i + 1] - 0.50f) < 1e-6f);
    }

    dev.removeSource(&src);
    std::puts("OK mono_pan_law");
}

// ─── 2: native-stereo sources bypass the pan law ──────────────────────
void testStereoSourcePassthrough()
{
    AudioDevice dev;
    ConstStereo src(0.6f, -0.2f);
    // A pan value must be IGNORED for a source that placed itself: the
    // card's wiring is the authority, not a mixer knob.
    src.pan.store(-1.0f);
    dev.addSource(&src);
    std::vector<float> out(kSamples, 0.0f);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        assert(std::fabs(out[2 * i]     - 0.6f) < 1e-6f);
        assert(std::fabs(out[2 * i + 1] + 0.2f) < 1e-6f);
    }
    // Per-source meter reads the louder side, so a card feeding one
    // speaker only still lights the mixer panel.
    assert(std::fabs(src.lastBufferPeak.load() - 0.6f) < 1e-6f);
    // Master peaks are per channel.
    assert(std::fabs(dev.getMasterPeakL() - 0.6f) < 1e-6f);
    assert(std::fabs(dev.getMasterPeakR() - 0.2f) < 1e-6f);

    dev.removeSource(&src);
    std::puts("OK stereo_source_passthrough");
}

// ─── 3: mono downmix ──────────────────────────────────────────────────
void testMonoDownmix()
{
    AudioDevice dev;
    ConstStereo src(0.6f, -0.2f);
    dev.addSource(&src);
    std::vector<float> out(kSamples, 0.0f);

    assert(!dev.isMonoDownmix());          // stereo is the default
    dev.setMonoDownmix(true);
    dev.mixSources(out.data(), kFrames);
    // 0.5 * (0.6 - 0.2) = 0.2 on both channels — the AVERAGE, so a
    // centred source keeps its level instead of gaining 6 dB.
    for (int i = 0; i < kFrames; ++i) {
        assert(std::fabs(out[2 * i]     - 0.2f) < 1e-6f);
        assert(std::fabs(out[2 * i + 1] - 0.2f) < 1e-6f);
    }
    dev.removeSource(&src);
    std::puts("OK mono_downmix");
}

// ─── Mockingboard helpers (same idiom as mockingboard_smoke_test) ─────

constexpr uint8_t kPbInactive = 0x04;   // {BC1=0, BDIR=0, /RESET=1}
constexpr uint8_t kPbWrite    = 0x06;   // {BC1=0, BDIR=1, /RESET=1}
constexpr uint8_t kPbLatch    = 0x07;   // {BC1=1, BDIR=1, /RESET=1}

void mbWriteVia(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    const uint8_t base = (chip == 0) ? 0x00 : 0x80;
    card.slotRomWrite(static_cast<uint8_t>(base | (reg & 0x0F)), v);
}

void mbAyWrite(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    mbWriteVia(card, chip, pom2::Via6522::VIA_DDRA, 0xFF);
    mbWriteVia(card, chip, pom2::Via6522::VIA_DDRB, 0xFF);
    mbWriteVia(card, chip, pom2::Via6522::VIA_ORA,  reg);
    mbWriteVia(card, chip, pom2::Via6522::VIA_ORB,  kPbLatch);
    mbWriteVia(card, chip, pom2::Via6522::VIA_ORB,  kPbInactive);
    mbWriteVia(card, chip, pom2::Via6522::VIA_ORA,  v);
    mbWriteVia(card, chip, pom2::Via6522::VIA_ORB,  kPbWrite);
    mbWriteVia(card, chip, pom2::Via6522::VIA_ORB,  kPbInactive);
}

// Channel A tone at `period`, amplitude 15, tone A only.
void mbToneOnChip(MockingboardCard& card, int chip, uint16_t period)
{
    mbAyWrite(card, chip, 0, static_cast<uint8_t>(period & 0xFF));
    mbAyWrite(card, chip, 1, static_cast<uint8_t>((period >> 8) & 0x0F));
    mbAyWrite(card, chip, 7, 0x3E);       // mixer: tone A on, rest off
    mbAyWrite(card, chip, 8, 0x0F);       // channel A amplitude
}

// ─── 4: Mockingboard AY1 → left, AY2 → right ──────────────────────────
void testMockingboardChipPan()
{
    for (int chip = 0; chip < 2; ++chip) {
        MockingboardCard card(4);
        card.setSampleRate(44100);
        card.setVolume(1.0f);
        card.setMuted(false);
        mbToneOnChip(card, chip, 0x0100);

        std::vector<float> left(kFrames, 0.0f), right(kFrames, 0.0f);
        AudioSource* src = card.audioSource();
        assert(src);
        assert(src->fillAudioBufferStereo(left.data(), right.data(), kFrames));

        double sumL = 0.0, sumR = 0.0;
        for (int i = 0; i < kFrames; ++i) {
            sumL += static_cast<double>(left[i])  * left[i];
            sumR += static_cast<double>(right[i]) * right[i];
        }
        const double rmsL = std::sqrt(sumL / kFrames);
        const double rmsR = std::sqrt(sumR / kFrames);
        std::printf("  MB chip %d: rmsL=%.4f rmsR=%.4f\n", chip, rmsL, rmsR);

        // The silent side must be EXACTLY silent — a chip with no
        // registers written produces no samples at all, so any leak
        // means the channels are being summed somewhere.
        if (chip == 0) { assert(rmsL > 0.05); assert(rmsR == 0.0); }
        else           { assert(rmsR > 0.05); assert(rmsL == 0.0); }
    }
    std::puts("OK mockingboard_chip_pan");
}

// ─── 5: Mockingboard mono fold-down == 0.5*(L+R) ──────────────────────
//
// This is the level guarantee. Two cards configured identically: one
// rendered through the stereo path, one through the mono path. The mono
// render must equal the average of the two stereo channels sample for
// sample — i.e. exactly what the pre-stereo `/6` summed render produced.
void testMockingboardMonoFoldDown()
{
    MockingboardCard stereoCard(4), monoCard(4);
    for (MockingboardCard* c : { &stereoCard, &monoCard }) {
        c->setSampleRate(44100);
        c->setVolume(1.0f);
        c->setMuted(false);
        // BOTH chips driven, at different periods, so the fold-down has
        // something asymmetric to average.
        mbToneOnChip(*c, 0, 0x0100);
        mbToneOnChip(*c, 1, 0x0180);
    }

    std::vector<float> left(kFrames, 0.0f), right(kFrames, 0.0f),
                       mono(kFrames, 0.0f);
    assert(stereoCard.audioSource()->fillAudioBufferStereo(
        left.data(), right.data(), kFrames));
    monoCard.audioSource()->fillAudioBuffer(mono.data(), kFrames);

    double worst = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        const double d =
            std::fabs(0.5 * (left[i] + right[i]) - mono[i]);
        if (d > worst) worst = d;
    }
    std::printf("  MB mono fold-down worst |0.5*(L+R) - mono| = %.3e\n", worst);
    assert(worst < 1e-6);
    std::puts("OK mockingboard_mono_fold_down");
}

// ─── Phasor helpers (same idiom as phasor_card_smoke_test) ────────────

uint8_t phMakePb(bool selPrimary, bool selSecondary)
{
    uint8_t pb = 0xFF;
    pb &= ~0x01;                                    // BC1 = 0
    pb &= ~0x02;                                    // BDIR = 0
    pb |= 0x04;                                     // /RESET = 1
    if (selPrimary)   pb &= ~0x10; else pb |= 0x10; // PB4 active-low
    if (selSecondary) pb &= ~0x08; else pb |= 0x08; // PB3 active-low
    return pb;
}

void phLatchWrite(PhasorCard& card, int viaIdx, uint8_t pb,
                  uint8_t reg, uint8_t data)
{
    // Native mode: VIA1's exclusive window is $10, VIA2's is $80.
    const uint8_t base = (viaIdx == 0) ? 0x10 : 0x80;
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRA, 0xFF);
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRB, 0xFF);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  reg);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>((pb & ~0x03) | 0x03));
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>(pb & ~0x03));
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  data);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>((pb & ~0x03) | 0x02));
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>(pb & ~0x03));
}

// ─── 6: Phasor pans by VIA pair ───────────────────────────────────────
void testPhasorPairPan()
{
    // chipIdx 0..3 → expected side: 0,1 left (VIA1 pair), 2,3 right.
    for (int chipIdx = 0; chipIdx < 4; ++chipIdx) {
        PhasorCard card(4);
        card.setSampleRate(44100);
        card.setVolume(1.0f);
        card.setMuted(false);
        card.deviceSelectWrite(0xD, 0);          // → PH_Phasor (4 chips)

        const int  viaIdx = chipIdx >> 1;
        const bool primary = ((chipIdx & 1) == 0);
        const uint8_t pb = phMakePb(primary, !primary);
        phLatchWrite(card, viaIdx, pb, 0, 0x00); // R0 period low
        phLatchWrite(card, viaIdx, pb, 1, 0x01); // R1 period high ($100)
        phLatchWrite(card, viaIdx, pb, 7, 0x3E); // mixer: tone A only
        phLatchWrite(card, viaIdx, pb, 8, 0x0F); // amplitude 15

        std::vector<float> left(kFrames, 0.0f), right(kFrames, 0.0f);
        assert(card.audioSource()->fillAudioBufferStereo(
            left.data(), right.data(), kFrames));

        double sumL = 0.0, sumR = 0.0;
        for (int i = 0; i < kFrames; ++i) {
            sumL += static_cast<double>(left[i])  * left[i];
            sumR += static_cast<double>(right[i]) * right[i];
        }
        const double rmsL = std::sqrt(sumL / kFrames);
        const double rmsR = std::sqrt(sumR / kFrames);
        std::printf("  Phasor AY%d: rmsL=%.4f rmsR=%.4f\n",
                    chipIdx, rmsL, rmsR);
        if (chipIdx < 2) { assert(rmsL > 0.02); assert(rmsR == 0.0); }
        else             { assert(rmsR > 0.02); assert(rmsL == 0.0); }
    }
    std::puts("OK phasor_pair_pan");
}

// ─── 7: end-to-end through the mixer ──────────────────────────────────
//
// A Mockingboard playing on chip 0 only, mixed by AudioDevice next to a
// centred mono source. Left must carry both, right only the mono one —
// the failure this whole change exists to prevent is the card's pan
// being flattened by the bus after the card got it right.
void testCardThroughMixer()
{
    MockingboardCard card(4);
    card.setSampleRate(44100);
    card.setVolume(1.0f);
    card.setMuted(false);
    mbToneOnChip(card, 0, 0x0100);

    AudioDevice dev;
    ConstMono centre(0.1f);
    dev.addSource(card.audioSource());
    dev.addSource(&centre);

    std::vector<float> out(kSamples, 0.0f);
    dev.mixSources(out.data(), kFrames);

    // Right channel is the centred DC source alone: constant 0.1.
    for (int i = 0; i < kFrames; ++i)
        assert(std::fabs(out[2 * i + 1] - 0.1f) < 1e-6f);
    // Left carries the AY on top of it, so it must deviate.
    const double rL = rms(out, 2, 0);
    const double rR = rms(out, 2, 1);
    std::printf("  through mixer: rmsL=%.4f rmsR=%.4f\n", rL, rR);
    assert(rL > rR * 1.5);

    dev.removeSource(card.audioSource());
    dev.removeSource(&centre);
    std::puts("OK card_through_mixer");
}

}  // namespace

int main()
{
    std::puts("=== stereo audio bus test ===");
    testMonoPanLaw();
    testStereoSourcePassthrough();
    testMonoDownmix();
    testMockingboardChipPan();
    testMockingboardMonoFoldDown();
    testPhasorPairPan();
    testCardThroughMixer();
    std::puts("All stereo audio tests passed.");
    return 0;
}
