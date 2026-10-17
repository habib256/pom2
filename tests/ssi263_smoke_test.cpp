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

// Ssi263 chip-model smoke test — pins the register state machine and
// IRQ timing that every SSI263 host card (Mockingboard C, Echo+,
// Phasor speech) depends on, plus the PCM audio render (phoneme blob
// in Ssi263PhonemeData.cpp) and its playback-cursor protocol.

#include "Ssi263.h"
#include "Ssi263PhonemeData.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using pom2::Ssi263;

void testControlLatchesModeAndIrq()
{
    Ssi263 chip;
    chip.reset();
    // Hardware-observed initial DURPHON=$C0; IRQ mode is latched only
    // when CTL falls. A detector can simply wake the chip and await D7.
    assert(chip.peekRegister(Ssi263::REG_DURPHON) == 0xC0);
    assert(chip.peekRegister(Ssi263::REG_FILFREQ) == 0xFF);
    assert(!chip.irqEnabled());
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    assert(chip.irqEnabled());
    assert(chip.currentMode() == Ssi263::MODE_PHONEME_TRANSITIONED_INFLECTION);
    assert(chip.advance(chip.phonemeRemainingCycles()));

    // Changing DUR while running changes phoneme timing, not the latched
    // interrupt enable or function. A subsequent CTL falling edge with
    // DUR=00 disables interrupts while preserving the previous function.
    chip.write(Ssi263::REG_DURPHON, 0x05);
    assert(chip.irqEnabled());
    assert(chip.currentMode() == Ssi263::MODE_PHONEME_TRANSITIONED_INFLECTION);
    assert(chip.advance(chip.phonemeRemainingCycles()));
    assert(chip.write(Ssi263::REG_CTTRAMP, 0x8F));
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    assert(!chip.irqEnabled());
    assert(chip.currentMode() == Ssi263::MODE_PHONEME_TRANSITIONED_INFLECTION);
    assert(!chip.advance(chip.phonemeRemainingCycles()));
    assert(chip.aRequest());  // D7 is pollable even with IRQ disabled.

    chip.write(Ssi263::REG_DURPHON, 0x85);
    assert(!chip.irqEnabled());
    chip.write(Ssi263::REG_CTTRAMP, 0x8F);
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    assert(chip.irqEnabled());
    assert(chip.currentMode() == Ssi263::MODE_PHONEME_IMMEDIATE_INFLECTION);
    std::puts("  ok: CTL falling edge latches mode/IRQ, DUR=00 retains function");
}

void testRepeatedRequestsKeepTheirPhase()
{
    Ssi263 chip;
    chip.reset();
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    chip.write(Ssi263::REG_DURPHON, 0xC5);
    const int period = chip.phonemeRemainingCycles();
    assert(chip.advance(period + 7));
    assert(chip.phonemeRemainingCycles() == period - 7);
    // Without acknowledgement the request is sticky, but playback and
    // the repeat timer keep running. Skip several complete periods.
    assert(!chip.advance(period * 3 + 11));
    assert(chip.aRequest());
    assert(chip.phonemeRemainingCycles() == period - 18);
    assert(chip.write(Ssi263::REG_INFLECT, 0x00));
    assert(!chip.advance(period - 19));
    assert(!chip.aRequest());
    assert(chip.advance(1));
    assert(chip.aRequest());
    // RATEINF also acknowledges without restarting the active phoneme.
    assert(chip.write(Ssi263::REG_RATEINF, 0x00));
    assert(!chip.advance(period - 1));
    assert(chip.advance(1));
    chip.write(Ssi263::REG_CTTRAMP, 0x8F);
    assert(!chip.advance(period * 10));
    assert(!chip.aRequest());
    std::puts("  ok: repeated phoneme requests survive INFLECT/RATEINF acknowledgement");
}

void testLatchedModeSnapshot()
{
    Ssi263 chip;
    chip.reset();
    chip.write(Ssi263::REG_DURPHON, 0x85);
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);  // latch function 2 / IRQ enabled
    chip.write(Ssi263::REG_DURPHON, 0x05); // register no longer describes mode
    chip.advance(17);
    std::vector<uint8_t> blob;
    chip.appendSnapshot(blob);
    Ssi263 restored;
    restored.reset();
    restored.loadSnapshot(blob.data());
    assert(restored.currentMode() == Ssi263::MODE_PHONEME_IMMEDIATE_INFLECTION);
    assert(restored.irqEnabled());
    assert(restored.phonemeRemainingCycles() == chip.phonemeRemainingCycles());
    assert(restored.advance(restored.phonemeRemainingCycles()));
    chip.write(Ssi263::REG_CTTRAMP, 0x8F);
    chip.write(Ssi263::REG_CTTRAMP, 0x0F); // latch DUR=00, preserve function 2
    chip.write(Ssi263::REG_DURPHON, 0xC5);
    blob.clear();
    chip.appendSnapshot(blob);
    restored.loadSnapshot(blob.data());
    assert(!restored.irqEnabled());
    assert(restored.currentMode() == Ssi263::MODE_PHONEME_IMMEDIATE_INFLECTION);
    assert(!restored.advance(restored.phonemeRemainingCycles()));

    // A legacy one-shot completion has no repeat phase or mode latch.
    // Migrate its DUR-derived IRQ policy and restart a complete period.
    blob.resize(Ssi263::kSnapshotBytesV1);
    std::fill(blob.begin() + 5, blob.begin() + 9, 0); // old completed timer
    blob[9] = 1; // request already pending
    restored.loadSnapshot(blob.data(), blob.size());
    assert(restored.irqEnabled());
    assert(restored.currentMode() == Ssi263::MODE_PHONEME_TRANSITIONED_INFLECTION);
    assert(restored.aRequest());
    assert(restored.phonemeRemainingCycles() > 0);
    restored.write(Ssi263::REG_INFLECT, 0);
    assert(restored.advance(restored.phonemeRemainingCycles()));
    std::puts("  ok: snapshot preserves a mode different from DURPHON bits");
}

void testResetState()
{
    Ssi263 chip;
    chip.reset();
    // Typical power-on: CTL=1, DUR=$C0, FILFREQ=$FF. A/!R is low.
    assert(chip.powerDown());
    assert(!chip.aRequest());
    assert(chip.peekRegister(Ssi263::REG_DURPHON) == 0xC0);
    assert(chip.peekRegister(Ssi263::REG_INFLECT) == 0);
    assert(chip.peekRegister(Ssi263::REG_RATEINF) == 0);
    assert(chip.peekRegister(Ssi263::REG_CTTRAMP) == Ssi263::CONTROL_MASK);
    assert(chip.peekRegister(Ssi263::REG_FILFREQ) == 0xFF);
    assert(chip.phonemeWriteCount() == 0);
    assert(chip.phonemeRemainingCycles() == 0);

    // Read in reset state: A/!R is low → status byte = 0x00.
    assert(chip.read(0) == 0x00);

    std::printf("  ok: reset state\n");
}

void testPhonemePlaysAndIrqs()
{
    Ssi263 chip;
    chip.reset();

    // Exit power-down: clear CTL, set amplitude = max.
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    assert(!chip.powerDown());

    // Configure DUR=3 (four source samples per native output frame).
    chip.write(Ssi263::REG_RATEINF, Ssi263::RATE_MASK);   // rate=15
    // Write DURPHON: mode=11 (transitioned inflection), phoneme=$05.
    chip.write(Ssi263::REG_DURPHON,
               static_cast<uint8_t>((0x3 << Ssi263::DURATION_MODE_SHIFT) | 0x05));
    assert(chip.currentPhoneme() == 0x05);
    assert(chip.currentMode() == Ssi263::MODE_PHONEME_TRANSITIONED_INFLECTION);
    assert(chip.irqEnabled());
    assert(chip.phonemeWriteCount() == 1);
    assert(chip.phonemeRemainingCycles() > 0);
    assert(!chip.aRequest());

    // Tick partially through the duration — A/!R should still be low.
    const int half = chip.phonemeRemainingCycles() / 2;
    bool edge = chip.advance(half);
    assert(!edge);
    assert(!chip.aRequest());
    assert(chip.phonemeRemainingCycles() > 0);

    // Tick past the remaining duration — A/!R goes high, edge=true.
    const int rest = chip.phonemeRemainingCycles() + 100;
    edge = chip.advance(rest);
    assert(edge);
    assert(chip.aRequest());
    assert(chip.phonemeRemainingCycles() > 0); // next repeating period

    // Reads see A/!R = 1.
    assert(chip.read(0) == 0x80);
    assert(chip.read(Ssi263::REG_CTTRAMP) == 0x80);

    // Further advance() doesn't fire a second edge (sticky until cleared).
    edge = chip.advance(10000);
    assert(!edge);
    assert(chip.aRequest());

    std::printf("  ok: phoneme write → cycle countdown → A/!R edge\n");
}

void testAckClearsRequest()
{
    Ssi263 chip;
    chip.reset();
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);    // exit power-down
    chip.write(Ssi263::REG_DURPHON, 0xC1);    // mode=11, phoneme=$01
    (void)chip.advance(chip.phonemeRemainingCycles() + 10);
    assert(chip.aRequest());

    // Write to $00 (DURPHON) clears the request and loads a new phoneme.
    bool cleared = chip.write(Ssi263::REG_DURPHON, 0xC2);
    assert(cleared);
    assert(!chip.aRequest());
    assert(chip.currentPhoneme() == 2);
    assert(chip.phonemeWriteCount() == 2);

    // Get another A/!R, then ack via $01 (INFLECT).
    (void)chip.advance(chip.phonemeRemainingCycles() + 10);
    assert(chip.aRequest());
    cleared = chip.write(Ssi263::REG_INFLECT, 0x00);
    assert(cleared);
    assert(!chip.aRequest());

    // And via $02 (RATEINF).
    chip.write(Ssi263::REG_DURPHON, 0xC3);
    (void)chip.advance(chip.phonemeRemainingCycles() + 10);
    assert(chip.aRequest());
    cleared = chip.write(Ssi263::REG_RATEINF, Ssi263::RATE_MASK);
    assert(cleared);
    assert(!chip.aRequest());

    // Writing $03 (CTTRAMP) WITHOUT CTL transition does NOT clear A/!R.
    chip.write(Ssi263::REG_DURPHON, 0xC4);
    (void)chip.advance(chip.phonemeRemainingCycles() + 10);
    assert(chip.aRequest());
    cleared = chip.write(Ssi263::REG_CTTRAMP, 0x0F);   // CTL=0, amp=15
    assert(!cleared);
    assert(chip.aRequest());

    // Writing $04 (FILFREQ) does NOT clear A/!R.
    cleared = chip.write(Ssi263::REG_FILFREQ, 0x80);
    assert(!cleared);
    assert(chip.aRequest());

    std::printf("  ok: writes to $00/$01/$02 ack A/!R; $03/$04 do not\n");
}

void testCtlPowerDownAndRestart()
{
    Ssi263 chip;
    chip.reset();
    // Exit power-down + start phoneme.
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    chip.write(Ssi263::REG_DURPHON, 0xC1);
    assert(chip.phonemeRemainingCycles() > 0);

    // CTL L→H (set bit 7): power-down silences + zeroes the remaining
    // timer + clears any pending request.
    (void)chip.advance(chip.phonemeRemainingCycles() + 10);
    assert(chip.aRequest());
    const bool cleared = chip.write(Ssi263::REG_CTTRAMP, 0x80 | 0x0F);
    assert(cleared);
    assert(chip.powerDown());
    assert(!chip.aRequest());
    assert(chip.phonemeRemainingCycles() == 0);

    // advance() during power-down is a no-op.
    const bool edge = chip.advance(100000);
    assert(!edge);

    // CTL H→L (clear bit 7): re-load the previously latched phoneme +
    // start a fresh countdown. Doesn't bump phonemeWriteCount_ (no new
    // DURPHON write).
    const uint32_t before = chip.phonemeWriteCount();
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    assert(!chip.powerDown());
    assert(chip.phonemeRemainingCycles() > 0);
    assert(chip.phonemeWriteCount() == before);

    std::printf("  ok: CTL L→H restarts loaded phoneme; H→L silences + clears\n");
}

// Decode the audio playback cursor out of the (stable, little-endian)
// snapshot blob — appendSnapshot layout per Ssi263.h: 5 register bytes,
// u32 phonemeRemainingCycles, u8 aRequest, u32 phonemeWriteCount, then
// u32 playbackPhoneme @14, u64 playbackOffset @18, u32 resampleAccum
// bits @26.
struct PlaybackCursor { uint32_t phoneme; uint64_t offset; uint32_t accumBits; };
PlaybackCursor snapshotCursor(const Ssi263& chip)
{
    std::vector<uint8_t> b;
    chip.appendSnapshot(b);
    assert(b.size() == Ssi263::kSnapshotBytes);
    auto u32 = [&](size_t at) {
        return static_cast<uint32_t>(b[at]) |
               (static_cast<uint32_t>(b[at + 1]) << 8) |
               (static_cast<uint32_t>(b[at + 2]) << 16) |
               (static_cast<uint32_t>(b[at + 3]) << 24);
    };
    PlaybackCursor c{};
    c.phoneme   = u32(14);
    c.offset    = u32(18) | (static_cast<uint64_t>(u32(22)) << 32);
    c.accumBits = u32(26);
    return c;
}

// CTL 1→0 (power-down exit) must restart AUDIO playback at the latched
// phoneme, not only the IRQ countdown. AppleWin SSI263.cpp:200-209 runs
// `Play(m_durationPhoneme & PHONEME_MASK)` on the H→L edge, which
// rewinds the PCM cursor; pre-fix POM2 only re-armed
// phonemeRemainingCycles_ and resumed mid-sample at the stale cursor.
void testCtlRestartRewindsPlaybackCursor()
{
    Ssi263 chip;
    chip.reset();
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);          // power up, amp=15
    chip.write(Ssi263::REG_RATEINF, 0x00);          // rate=0 (slow)
    chip.write(Ssi263::REG_DURPHON, 0x80 | 0x05);   // mode=10, phoneme $05

    // Render some audio so the playback cursor advances mid-phoneme.
    std::vector<float> buf(600, 0.0f);
    chip.fillAudio(buf.data(), static_cast<int>(buf.size()), 44100);
    const PlaybackCursor mid = snapshotCursor(chip);
    assert(mid.phoneme == 0x05);
    assert(mid.offset > 0 && "fillAudio should have advanced the cursor");

    // Power down (CTL 0→1), then exit power-down (CTL 1→0).
    chip.write(Ssi263::REG_CTTRAMP, 0x80 | 0x0F);
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    const PlaybackCursor restarted = snapshotCursor(chip);
    assert(restarted.phoneme == 0x05 &&
           "CTL H→L must (re)latch the playback phoneme");
    assert(restarted.offset == 0 &&
           "CTL H→L must rewind the playback cursor to the phoneme start");
    assert(restarted.accumBits == 0 &&
           "CTL H→L must clear the resampler accumulator");
    assert(chip.phonemeRemainingCycles() > 0);

    std::printf("  ok: CTL 1→0 rewinds the PCM playback cursor (AppleWin Play())\n");
}

void testIrqDisabledMode()
{
    Ssi263 chip;
    chip.reset();
    // Mode = MODE_IRQ_DISABLED (bits 7:6 = 00).
    chip.write(Ssi263::REG_DURPHON, 0x05);
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    assert(chip.currentMode() == Ssi263::MODE_IRQ_DISABLED);
    assert(!chip.irqEnabled());

    // Phoneme runs to completion. Per AppleWin (SSI263.cpp ~line 724) the
    // A/!R pin (D7) is asserted on completion regardless of DR1:0 mode, so
    // aRequest()/read() see it — but MODE_IRQ_DISABLED (00) suppresses the
    // host IRQ, so advance() reports no IRQ edge. Polling drivers that select
    // mode 00 and watch D7 for phoneme completion rely on this.
    const bool edge = chip.advance(chip.phonemeRemainingCycles() + 10);
    assert(!edge);                 // no host IRQ edge in mode 00
    assert(chip.aRequest());       // ...but D7 (A/!R) is set
    assert(chip.read(0) == 0x80);  // ...and pollable via a status read

    std::printf("  ok: MODE_IRQ_DISABLED suppresses host IRQ but still sets A/!R (D7)\n");
}

// Audio render — with the AppleWin-ported phoneme PCM blob now linked
// in (Ssi263PhonemeData.cpp), fillAudio() must emit non-silent samples
// when the chip is configured to play a phoneme.
void testAudioRenderNonSilent()
{
    Ssi263 chip;
    chip.reset();
    chip.write(Ssi263::REG_FILFREQ, 0x80);
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);     // exit power-down, amp=15
    // Slow phoneme so playback runs throughout the buffer.
    chip.write(Ssi263::REG_RATEINF, 0x00);     // rate=0
    chip.write(Ssi263::REG_DURPHON, 0x80 | 0x05);    // mode=10, phon $05

    constexpr int N = 4096;
    constexpr uint32_t SR = 44100;
    std::vector<float> buf(N, 0.0f);
    chip.fillAudio(buf.data(), N, SR);

    double sumSq = 0.0;
    float vmin = +1e9f, vmax = -1e9f;
    for (float s : buf) {
        sumSq += static_cast<double>(s) * s;
        if (s < vmin) vmin = s;
        if (s > vmax) vmax = s;
    }
    const double rms = std::sqrt(sumSq / N);
    std::printf("  phoneme $05 audio rms=%.4f vmin=%.4f vmax=%.4f\n",
                rms, vmin, vmax);
    // Expect non-trivial energy — a real speech phoneme has RMS in the
    // 0.05-0.3 range at amp=15 depending on its envelope. Set the floor
    // low enough to survive any phoneme.
    assert(rms > 0.005);

    // Power-down silences.
    chip.write(Ssi263::REG_CTTRAMP, 0x80);
    std::fill(buf.begin(), buf.end(), 0.0f);
    chip.fillAudio(buf.data(), N, SR);
    double sumSq2 = 0.0;
    for (float s : buf) sumSq2 += static_cast<double>(s) * s;
    assert(sumSq2 == 0.0);

    // FILTER_FREQ_SILENCE sentinel silences too.
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);                // unblock
    chip.write(Ssi263::REG_FILFREQ, Ssi263::FILTER_FREQ_SILENCE);
    chip.write(Ssi263::REG_DURPHON, 0x80 | 0x05);         // new phoneme
    std::fill(buf.begin(), buf.end(), 0.0f);
    chip.fillAudio(buf.data(), N, SR);
    double sumSq3 = 0.0;
    for (float s : buf) sumSq3 += static_cast<double>(s) * s;
    assert(sumSq3 == 0.0);

    std::printf("  ok: phoneme audio non-silent; power-down + $FF filter silence\n");
}

void testDurationFollowsPcmLength()
{
    Ssi263 chip;
    chip.reset();
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);

    // Pause uses entry 0's PCM length, even though it renders silence.
    // DUR=3 consumes groups of four; RATEINF does not drive PCM speed.
    chip.write(Ssi263::REG_RATEINF, 0xF0);
    chip.write(Ssi263::REG_DURPHON, 0xC0);     // mode=11
    const int fastest = chip.phonemeRemainingCycles();
    assert(fastest > 30000 && fastest < 32000);

    // DUR=0 consumes one source sample per native output frame.
    chip.write(Ssi263::REG_RATEINF, 0x00);
    // The IRQ mode is still latched as 3; this changes duration only.
    chip.write(Ssi263::REG_DURPHON, 0x00);
    const int slowest = chip.phonemeRemainingCycles();
    assert(slowest > 120000 && slowest < 125000);

    assert(std::abs(slowest - fastest * 4) <= 3); // cycle-rounding bound

    std::printf("  ok: PCM duration (%d cyc DUR=3, %d cyc DUR=0)\n",
                fastest, slowest);
}

void testClockChangePreservesRemainingTime()
{
    for (int nextClock : {1015625, 3579545}) {
        Ssi263 chip;
        chip.reset();
        chip.setCpuClockHz(1022727);
        chip.write(Ssi263::REG_CTTRAMP, 0x0F);
        chip.write(Ssi263::REG_DURPHON, 0xCA);
        chip.advance(chip.phonemeRemainingCycles() / 3);
        const int before = chip.phonemeRemainingCycles();
        chip.setCpuClockHz(nextClock);
        const int expected = static_cast<int>((static_cast<int64_t>(before) * nextClock + 1022726) / 1022727);
        assert(chip.phonemeRemainingCycles() == expected);
        assert(!chip.advance(expected - 1));
        assert(chip.advance(1));
    }
    std::puts("  ok: CPU clock changes preserve the active phoneme's remaining real time");
}

} // namespace

// A snapshot restores (playbackPhoneme_, playbackOffset_) as an
// unvalidated PAIR, so a cursor captured deep inside a long phoneme can
// come back attached to a short one — after a rewind that crosses a
// DURPHON write, or from a snapshot written by another build. Pre-fix,
// `info.offset + playbackOffset_` then indexed PCM belonging to the NEXT
// phoneme in the blob (an audible burst of the wrong sound) or ran past
// the end of the blob and silently truncated the buffer at the
// `idx >= kPhonemeDataLen` break. fillAudio must re-clamp the cursor to
// the phoneme it is actually rendering.
void testSnapshotClampsPlaybackCursor()
{
    namespace pd = pom2::ssi263_data;
    // Shortest and longest phoneme in the blob — the worst-case pair.
    int shortest = 0, longest = 0;
    for (size_t i = 1; i < pd::kNumPhonemes; ++i) {
        if (pd::kPhonemeInfo[i].length < pd::kPhonemeInfo[shortest].length)
            shortest = static_cast<int>(i);
        if (pd::kPhonemeInfo[i].length > pd::kPhonemeInfo[longest].length)
            longest = static_cast<int>(i);
    }
    assert(pd::kPhonemeInfo[shortest].length > 0);
    assert(pd::kPhonemeInfo[longest].length > pd::kPhonemeInfo[shortest].length);

    // A PCM table index is NOT a phoneme code: AppleWin indexes
    // `g_nPhonemeInfo[]` by code MINUS TWO (`SSI263::Play`), so the code
    // that plays entry `e` is `e + 2` (see `ssi263_phoneme_map`). Feeding
    // the raw index as the DURPHON code used to pick a phoneme two
    // entries away and the "deep cursor onto a short phoneme" premise
    // silently stopped holding.
    const int shortCode = shortest + 2;
    const int longCode  = longest  + 2;
    assert(shortCode <= 0x3F && longCode <= 0x3F);

    // Live chip, mid-way through the LONGEST phoneme.
    Ssi263 live;
    live.reset();
    live.write(Ssi263::REG_FILFREQ, 0x80);
    live.write(Ssi263::REG_CTTRAMP, 0x0F);       // powered up, amp 15
    live.write(Ssi263::REG_RATEINF, 0x00);
    live.write(Ssi263::REG_DURPHON,
               static_cast<uint8_t>(0x80 | (longCode & 0x3F)));
    std::vector<float> scratch(pd::kPhonemeInfo[longest].length / 2, 0.0f);
    live.fillAudio(scratch.data(), static_cast<int>(scratch.size()), 22050);
    std::vector<uint8_t> blob;
    live.appendSnapshot(blob);
    assert(blob.size() == Ssi263::kSnapshotBytes);
    const uint64_t deepOffset =
        static_cast<uint64_t>(blob[18]) |
        (static_cast<uint64_t>(blob[19]) << 8) |
        (static_cast<uint64_t>(blob[20]) << 16) |
        (static_cast<uint64_t>(blob[21]) << 24);
    assert(deepOffset > pd::kPhonemeInfo[shortest].length &&
           "need a cursor deeper than the short phoneme is long");

    // Re-point the cursor's PHONEME at the shortest one, offset untouched:
    // the exact mismatch a rewind can hand back.
    blob[14] = static_cast<uint8_t>(shortCode);
    blob[15] = blob[16] = blob[17] = 0;
    // Also point DURPHON at it so the restored chip is otherwise coherent.
    blob[0] = static_cast<uint8_t>(0x80 | (shortCode & 0x3F));

    Ssi263 restored;
    restored.reset();
    restored.loadSnapshot(blob.data());

    constexpr int N = 512;
    std::vector<float> got(N, 0.0f);
    restored.fillAudio(got.data(), N, 22050);

    // Reference: the same phoneme rendered from its start.
    Ssi263 ref;
    ref.reset();
    ref.write(Ssi263::REG_FILFREQ, 0x80);
    ref.write(Ssi263::REG_CTTRAMP, 0x0F);
    ref.write(Ssi263::REG_RATEINF, 0x00);
    ref.write(Ssi263::REG_DURPHON,
              static_cast<uint8_t>(0x80 | (shortCode & 0x3F)));
    std::vector<float> want(N, 0.0f);
    ref.fillAudio(want.data(), N, 22050);

    for (int i = 0; i < N; ++i) {
        if (got[i] != want[i]) {
            std::fprintf(stderr,
                "ssi263 snapshot cursor: sample %d = %.6f, expected %.6f "
                "(phoneme %d, restored offset %llu > length %u — the clamp "
                "is missing)\n",
                i, got[i], want[i], shortest,
                (unsigned long long)deepOffset,
                pd::kPhonemeInfo[shortest].length);
            std::abort();
        }
    }

    // And a corrupt resampler accumulator must not spin the audio thread:
    // fillAudio's `while (accum >= 1) accum -= 1` would iterate ~2^30 times.
    blob[26] = 0x00; blob[27] = 0x00; blob[28] = 0x80; blob[29] = 0x4F; // 1e9f
    Ssi263 wild;
    wild.reset();
    wild.loadSnapshot(blob.data());
    wild.fillAudio(got.data(), N, 22050);

    std::printf("  ok: snapshot cursor re-clamped to the restored phoneme "
                "(phoneme %d, offset %llu -> 0)\n",
                shortest, (unsigned long long)deepOffset);
}

void testResetDropsQueuedPlayback()
{
    // Hunt #21: reset() restored the CPU latches but left ctlEvents_ and
    // aPrimed_ armed, so fillAudioTimed kept rendering a phoneme the
    // guest had just powered down (and applyPlaybackEvent rewound the
    // cursor a snapshot restore had just written).
    Ssi263 chip;
    chip.reset();
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    chip.write(Ssi263::REG_DURPHON, 0x80 | 0x05);
    chip.queuePlaybackEvent(Ssi263::REG_CTTRAMP, 0x0F, 0);
    chip.queuePlaybackEvent(Ssi263::REG_DURPHON, 0x80 | 0x05, 0);
    chip.reset();
    std::vector<float> buf(256, 0.0f);
    chip.fillAudioTimed(buf.data(), 256, 44100, 0.0, 1022727.0 / 44100.0);
    float peak = 0.0f;
    for (float s : buf) peak = std::max(peak, std::fabs(s));
    assert(peak == 0.0f &&
           "reset must drop queued speech; leftover events would voice the buffer");
    std::printf("  ok: reset drops queued speech events\n");
}

int main()
{
    std::printf("Ssi263 chip smoke test\n");
    testControlLatchesModeAndIrq();
    testRepeatedRequestsKeepTheirPhase();
    testLatchedModeSnapshot();
    testResetState();
    testPhonemePlaysAndIrqs();
    testAckClearsRequest();
    testCtlPowerDownAndRestart();
    testCtlRestartRewindsPlaybackCursor();
    testIrqDisabledMode();
    testAudioRenderNonSilent();
    testDurationFollowsPcmLength();
    testClockChangePreservesRemainingTime();
    testSnapshotClampsPlaybackCursor();
    testResetDropsQueuedPlayback();
    std::printf("PASS\n");
    return 0;
}
