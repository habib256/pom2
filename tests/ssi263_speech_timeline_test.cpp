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

// Speech runs on the emuCycles timeline, like everything else — hunt #19.
//
// The SSI263's register writes took effect on the audio side the moment the
// CPU made them, while the card's PSG writes travel to the audio thread as
// cycle-stamped events replayed through a ~40 ms jitter buffer. On a Sound
// II — one card, one speaker, speech and music together — that put the
// speech a whole buffer AHEAD of the music it is synchronised with, and
// quantised every phoneme boundary to the CPU worker's chunk rather than
// the cycle the driver wrote it on.
//
// This pins the fix at chip level: a phoneme queued for a cycle in the
// middle of a buffer starts in the middle of that buffer. The control run
// is the old path (`fillAudio`), which voices the buffer from sample 0 —
// without it this test would pass on any implementation that makes noise.

#include "Ssi263.h"
#include "EchoPlusCard.h"
#include "Mockingboard.h"
#include "Memory.h"
#include "M6502.h"
#include "SlotBus.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;
void fail(const char* what) { std::printf("FAIL: %s\n", what); ++g_failures; }

constexpr uint32_t kSr     = 44100;
constexpr int      kFrames = 1024;
// One sample at 1.0227 MHz / 44100.
constexpr double   kCps    = 1022727.0 / 44100.0;

double energy(const std::vector<float>& b, int from, int to)
{
    double acc = 0.0;
    for (int i = from; i < to; ++i) acc += static_cast<double>(b[i]) * b[i];
    return acc;
}

// Latch a phoneme with the chip held in power-down, then prime the audio
// side by rendering one silent buffer.
void arm(pom2::Ssi263& chip, std::vector<float>& buf, double startCycle)
{
    chip.write(pom2::Ssi263::REG_CTTRAMP, 0x8F);   // CTL high: powered down
    chip.write(pom2::Ssi263::REG_FILFREQ, 0x80);
    chip.write(pom2::Ssi263::REG_DURPHON, 0x0A);   // a voiced phoneme
    std::fill(buf.begin(), buf.end(), 0.0f);
    chip.fillAudioTimed(buf.data(), kFrames, kSr, startCycle, kCps);
    if (energy(buf, 0, kFrames) != 0.0)
        fail("a powered-down chip must render silence");
}

void checkExactEventBoundary()
{
    // Each sample begins ten cycles apart. A store between sample starts
    // must affect the next sample, never the one preceding the store.
    // Include the buffer edge: splitting callbacks cannot move a store.
    for (uint64_t event : {40u, 41u, 49u, 379u, 380u, 381u}) {
        pom2::Ssi263 chip, reference;
        chip.reset();
        reference.reset();
        for (auto* target : {&chip, &reference}) {
            target->write(pom2::Ssi263::REG_CTTRAMP, 0x8F);
            target->write(pom2::Ssi263::REG_FILFREQ, 0x80);
            target->write(pom2::Ssi263::REG_DURPHON, 0x0A);
        }
        float prime = 0.0f;
        chip.fillAudioTimed(&prime, 1, kSr, 0.0, 10.0);
        chip.write(pom2::Ssi263::REG_CTTRAMP, 0x0F);
        chip.queuePlaybackEvent(pom2::Ssi263::REG_CTTRAMP, 0x0F, event);
        std::vector<float> actual(74), expected(74);
        chip.fillAudioTimed(actual.data(), 37, kSr, 10.0, 10.0);
        chip.fillAudioTimed(actual.data() + 37, 37, kSr, 380.0, 10.0);
        const auto first = static_cast<int>((event - 10 + 9) / 10);
        reference.write(pom2::Ssi263::REG_CTTRAMP, 0x0F);
        reference.fillAudio(expected.data() + first, 74 - first, kSr);
        if (actual != expected) {
            std::printf("  wrong sample boundary for event at cycle %llu\n",
                        static_cast<unsigned long long>(event));
            fail("speech store was not applied at the first sample starting on/after its cycle");
        }
    }
}

void checkTimedFrameModeLatch()
{
    pom2::Ssi263 chip, reference;
    chip.reset();
    reference.reset();
    for (auto* target : {&chip, &reference}) {
        target->write(pom2::Ssi263::REG_FILFREQ, 0x80);
        target->write(pom2::Ssi263::REG_DURPHON, 0xCA);
        target->write(pom2::Ssi263::REG_CTTRAMP, 0x0F); // function 3
        target->write(pom2::Ssi263::REG_DURPHON, 0x8A); // DUR=2
    }
    chip.queuePlaybackEvent(pom2::Ssi263::REG_DURPHON, 0x8A, 0);
    std::vector<float> prime(37), refPrime(37);
    chip.fillAudioTimed(prime.data(), 37, 22050, 0.0, 1.0);
    reference.fillAudio(refPrime.data(), 37, 22050);
    if (prime != refPrime) fail("DUR=2 priming differs between timed and immediate playback");
    // CPU latches frame mode now; the audio side must keep DUR=2 for
    // another 13 samples and latch frame mode only at the queued CTL edge.
    for (auto [reg, value] : {std::pair<uint8_t, uint8_t>{3, 0x8F}, {0, 0x4A}, {3, 0x0F}}) {
        chip.write(reg, value);
        chip.queuePlaybackEvent(reg, value, 50);
    }
    std::vector<float> actual(64), expected(64);
    chip.fillAudioTimed(actual.data(), 64, 22050, 37.0, 1.0);
    reference.fillAudio(expected.data(), 13, 22050);
    reference.write(3, 0x8F);
    reference.write(0, 0x4A);
    reference.write(3, 0x0F);
    reference.fillAudio(expected.data() + 13, 51, 22050);
    if (actual != expected) fail("frame-mode PCM speed changed before its stamped CTL edge");
}

template<class Card>
void checkModeSnapshot(Card& card, Card& restored, uint8_t base, int versionAt)
{
    card.slotRomWrite(base, 0x85);
    card.slotRomWrite(base + 3, 0x0F); // function 2 / IRQ enabled
    card.slotRomWrite(base, 0xC5);     // DUR now differs from active function
    std::vector<uint8_t> blob;
    card.appendSnapshotState(blob);
    restored.loadSnapshotState(blob.data(), blob.size());
    std::vector<uint8_t> actual;
    restored.appendSnapshotState(actual);
    if (actual != blob) fail("card snapshot lost the CTL-latched SSI263 mode");

    // Truncating only the new latch byte must reject the entire restore,
    // before any register, timer, VIA or AY has changed.
    restored.onReset();
    std::vector<uint8_t> before, after;
    restored.appendSnapshotState(before);
    restored.loadSnapshotState(blob.data(), blob.size() - 1);
    restored.appendSnapshotState(after);
    if (after != before) fail("truncated speech snapshot partially mutated the card");

    // The previous format had no latch byte. Reconstruct a genuine old
    // layout, which derives IRQ/function from DURPHON on migration.
    blob.pop_back();
    --blob[versionAt];
    restored.loadSnapshotState(blob.data(), blob.size());
    actual.clear();
    restored.appendSnapshotState(actual);
    std::vector<uint8_t> migrated = blob;
    ++migrated[versionAt];
    migrated.push_back(0x07); // function 3 / IRQ enabled from old DUR=$C5
    if (actual != migrated) fail("legacy speech snapshot was not migrated correctly");
    if (versionAt == 3) {
        // MBS v1 also omitted the trailing portAIn byte in each VIA.
        blob[3] = 1;
        blob.erase(blob.begin() + 55);
        blob.erase(blob.begin() + 30);
        restored.onReset();
        restored.loadSnapshotState(blob.data(), blob.size());
        actual.clear();
        restored.appendSnapshotState(actual);
        if (actual != migrated) fail("MBS v1 VIA/SSI offsets were not migrated correctly");
    }
}

// Muting belongs to the host mixer: a muted card must follow the same
// phoneme/PSG timeline as an audible twin, including writes during mute.
template<class Card>
void checkMuteTimeline(Card& card, Card& reference, uint8_t base,
                       uint32_t sr, bool stereo)
{
    constexpr int frames = 257;
    Memory memory;
    M6502 cpu(&memory);
    card.setCpu(&cpu);
    reference.setCpu(&cpu);
    std::vector<float> out(frames), expected(frames), right(frames), refRight(frames);
    card.setSampleRate(sr);
    reference.setSampleRate(sr);
    card.setVolume(1.0f);
    reference.setVolume(1.0f);
    auto write = [&](uint8_t reg, uint8_t value) {
        card.slotRomWrite(base + reg, value);
        reference.slotRomWrite(base + reg, value);
    };
    write(pom2::Ssi263::REG_CTTRAMP, 0x8F);
    write(pom2::Ssi263::REG_FILFREQ, 0x80);
    write(pom2::Ssi263::REG_DURPHON, 0x0A);
    write(pom2::Ssi263::REG_CTTRAMP, 0x0F);
    double cpuCycles = 0.0;
    uint64_t advanced = 0;
    float gain = 1.0f;
    const float step = 1.0f / (0.005f * sr);
    double referenceEnergy = 0.0;
    double fadeEnergy = 0.0;
    for (int block = 0; block < 22; ++block) {
        // Prime audibly, fade out, stay muted across writes, then fade in.
        const bool muted = block >= 2 && block < 16;
        card.setMuted(muted);
        if (block == 20) card.setVolume(0.4f);
        if (block == 7) write(pom2::Ssi263::REG_DURPHON, 0x12);
        if (block == 11) write(pom2::Ssi263::REG_DURPHON, 0x0A);
        cpuCycles += static_cast<double>(frames) * 1022727.0 / sr;
        const auto now = static_cast<uint64_t>(cpuCycles);
        memory.setCycleCounter(now);
        card.advanceCycles(static_cast<int>(now - advanced));
        reference.advanceCycles(static_cast<int>(now - advanced));
        advanced = now;
        if (stereo) {
            if (!card.audioSource()->fillAudioBufferStereo(out.data(), right.data(), frames) ||
                !reference.audioSource()->fillAudioBufferStereo(expected.data(), refRight.data(), frames)) {
                fail("speech card did not provide stereo output");
                return;
            }
        } else {
            card.audioSource()->fillAudioBuffer(out.data(), frames);
            reference.audioSource()->fillAudioBuffer(expected.data(), frames);
        }
        for (int i = 0; i < frames; ++i) {
            const float target = muted ? 0.0f : (block >= 20 ? 0.4f : 1.0f);
            if (gain < target) gain = std::min(target, gain + step);
            if (gain > target) gain = std::max(target, gain - step);
            if (std::fabs(out[i] - expected[i] * gain) > 1e-6f ||
                (stereo && std::fabs(right[i] - refRight[i] * gain) > 1e-6f)) {
                std::printf("  mute mismatch: rate %u, stereo %d, block %d, sample %d\n",
                            sr, stereo, block, i);
                fail("mute changed speech timing or bypassed the 5 ms gain ramp");
                return;
            }
            if (block >= 17) referenceEnergy += expected[i] * expected[i];
            if (block == 2) fadeEnergy += out[i] * out[i];
        }
    }
    if (!(referenceEnergy > 0.0 && fadeEnergy > 0.0))
        fail("mute control must contain audible speech during fade and after unmute");
}

}  // namespace

void checkSoundIIWarmReset(uint32_t sr)
{
    Memory memory;
    M6502 cpu(&memory);
    auto owned = std::make_unique<MockingboardCard>(4, MockingboardCard::Variant::SoundII);
    auto& card = *owned;
    memory.slotBus().plug(4, std::move(owned));
    MockingboardCard reference(4, MockingboardCard::Variant::SoundII);
    for (auto* target : {&card, &reference}) {
        target->setCpu(&cpu);
        target->setSampleRate(sr);
        target->setVolume(1.0f);
        target->slotRomWrite(0x43, 0x8F);
        target->slotRomWrite(0x44, 0x80);
        target->slotRomWrite(0x40, 0xCA);
        target->slotRomWrite(0x43, 0x0F);
    }
    constexpr int frames = 257;
    std::vector<float> actual(frames), expected(frames);
    uint64_t previous = 0;
    double audible = 0.0;
    for (int block = 0; block < 28; ++block) {
        const auto now = static_cast<uint64_t>((block + 1) * frames * 1022727.0 / sr);
        memory.setCycleCounter(now);
        for (auto* target : {&card, &reference}) {
            target->advanceCycles(static_cast<int>(now - previous));
            // This stays in the speech jitter buffer across Ctrl-Reset.
            if (block == 4) target->slotRomWrite(0x40, 0xD2);
        }
        previous = now;
        if (block == 6) {
            MockingboardCard::Ssi263Snap before, after;
            card.snapshotSsi263(&before);
            memory.slotBus().reset();
            memory.slotBus().reset(); // consecutive resets before an audio callback
            card.snapshotSsi263(&after);
            if (!std::equal(before.regs, before.regs + 5, after.regs) ||
                before.aRequest != after.aRequest ||
                before.phonemeRemainingCycles != after.phonemeRemainingCycles ||
                before.mode != after.mode || before.irqEnabled != after.irqEnabled)
                fail("Ctrl-Reset changed Sound II speech registers, request or timer");
            if (card.isIrqAsserted() || card.peekViaRegister(1, 0x0E) != 0x80)
                fail("Ctrl-Reset must still clear the Sound II VIA interrupt state");
        }
        card.audioSource()->fillAudioBuffer(actual.data(), frames);
        reference.audioSource()->fillAudioBuffer(expected.data(), frames);
        if (actual != expected) {
            fail("Ctrl-Reset changed continuing Sound II PCM or queued speech timing");
            return;
        }
        if (block > 6) audible += energy(actual, 0, frames);
    }
    if (!(audible > 0.0)) fail("warm-reset speech control was silent");
    memory.slotBus().reset(/*powerCycle=*/true);
    memory.slotBus().reset(); // a warm reset must retain the pending cold flush
    MockingboardCard::Ssi263Snap cold;
    card.snapshotSsi263(&cold);
    if (!cold.powerDown || cold.aRequest || cold.irqEnabled ||
        cold.phonemeRemainingCycles != 0 || cold.regs[0] != 0xC0 ||
        cold.regs[4] != 0xFF)
        fail("slot power cycle did not restore Sound II speech power-on state");
    for (int block = 0; block < 3; ++block) {
        card.audioSource()->fillAudioBuffer(actual.data(), frames);
        if (energy(actual, 0, frames) != 0.0)
            fail("slot power cycle retained queued or audible speech");
    }
}

void checkSoundIIWarmRequest()
{
    MockingboardCard card(4, MockingboardCard::Variant::SoundII);
    card.slotRomWrite(0x40, 0xCA);
    card.slotRomWrite(0x43, 0x0F);
    card.slotRomWrite(0x8E, 0x82); // enable VIA2.CA1
    MockingboardCard::Ssi263Snap state;
    card.snapshotSsi263(&state);
    card.advanceCycles(state.phonemeRemainingCycles);
    if (!card.isIrqAsserted()) fail("warm-request fixture did not assert VIA IRQ");
    card.onReset();
    card.snapshotSsi263(&state);
    if (!state.aRequest || card.isIrqAsserted() ||
        (card.peekViaRegister(1, 0x0D) & 0x02))
        fail("warm reset must retain SSI request while clearing VIA CA1/IRQ");
    card.slotRomWrite(0x8E, 0x82);
    if (card.isIrqAsserted()) fail("enabling CA1 fabricated a post-reset speech edge");
    card.slotRomWrite(0x41, 0); // acknowledge SSI without restarting its repeat phase
    card.snapshotSsi263(&state);
    card.advanceCycles(state.phonemeRemainingCycles - 1);
    if (card.isIrqAsserted()) fail("post-reset speech requested one cycle early");
    card.advanceCycles(1);
    if (!card.isIrqAsserted()) fail("post-reset speech lost the next repeat request");
}

int main()
{
    std::vector<float> buf(kFrames, 0.0f);
    checkExactEventBoundary();
    checkTimedFrameModeLatch();
    for (uint32_t sr : {22050u, 44100u, 48000u}) checkSoundIIWarmReset(sr);
    checkSoundIIWarmRequest();
    {
        EchoPlusCard card, restored;
        checkModeSnapshot(card, restored, 0, 2); // EP v1 -> v2
        MockingboardCard sound(4, MockingboardCard::Variant::SoundII);
        MockingboardCard soundRestored(4, MockingboardCard::Variant::SoundII);
        checkModeSnapshot(sound, soundRestored, 0x40, 3); // MBS v2 -> v3
    }

    for (uint32_t sr : {44100u, 48000u}) {
        EchoPlusCard echo, echoReference;
        checkMuteTimeline(echo, echoReference, 0, sr, false);
        for (bool stereo : {false, true}) {
            MockingboardCard sound(4, MockingboardCard::Variant::SoundII);
            MockingboardCard reference(4, MockingboardCard::Variant::SoundII);
            checkMuteTimeline(sound, reference, 0x40, sr, stereo);
        }
    }

    // ── The fix: the phoneme starts where the driver put it ──────────────
    {
        pom2::Ssi263 chip;
        chip.reset();
        arm(chip, buf, 0.0);

        // Second buffer covers cycles [B, B + kFrames*kCps). Start speaking
        // at the cycle that falls on sample 512 — the middle.
        const double bufStart = static_cast<double>(kFrames) * kCps;
        const double atCycle  = bufStart + 512.0 * kCps;
        chip.write(pom2::Ssi263::REG_CTTRAMP, 0x0F);            // CPU-NOW half
        chip.queuePlaybackEvent(pom2::Ssi263::REG_CTTRAMP, 0x0F,
                                static_cast<uint64_t>(atCycle));

        std::fill(buf.begin(), buf.end(), 0.0f);
        chip.fillAudioTimed(buf.data(), kFrames, kSr, bufStart, kCps);

        const double before = energy(buf, 0, 500);
        const double after  = energy(buf, 524, kFrames);
        std::printf("  energy before the stamped cycle %.6f, after %.6f\n",
                    before, after);
        if (before != 0.0)
            fail("speech began BEFORE the cycle the driver wrote it on");
        if (!(after > 0.0))
            fail("speech never began at all");
    }

    // ── The control: the untimed path voices the whole buffer ────────────
    //
    // Same chip, same writes, rendered the old way. If this were also
    // silent up to sample 500, the assertion above would be measuring
    // nothing.
    {
        pom2::Ssi263 chip;
        chip.reset();
        arm(chip, buf, 0.0);
        chip.write(pom2::Ssi263::REG_CTTRAMP, 0x0F);
        std::fill(buf.begin(), buf.end(), 0.0f);
        chip.fillAudio(buf.data(), kFrames, kSr);
        const double before = energy(buf, 0, 500);
        if (!(before > 0.0))
            fail("control: the untimed path should voice from sample 0");
        else
            std::printf("  ok: control — the untimed path voices from sample 0\n");
    }

    if (g_failures == 0)
        std::printf("  ok: a phoneme starts on the sample its cycle falls in\n");
    if (g_failures) return 1;
    std::puts("ssi263_speech_timeline OK");
    return 0;
}
