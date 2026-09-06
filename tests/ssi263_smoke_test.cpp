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

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using pom2::Ssi263;

void testResetState()
{
    Ssi263 chip;
    chip.reset();
    // Reset puts the chip in power-down (CTL=1) with everything else
    // cleared. A/!R low; no playback in progress.
    assert(chip.powerDown());
    assert(!chip.aRequest());
    assert(chip.peekRegister(Ssi263::REG_DURPHON) == 0);
    assert(chip.peekRegister(Ssi263::REG_INFLECT) == 0);
    assert(chip.peekRegister(Ssi263::REG_RATEINF) == 0);
    assert(chip.peekRegister(Ssi263::REG_CTTRAMP) == Ssi263::CONTROL_MASK);
    assert(chip.peekRegister(Ssi263::REG_FILFREQ) == 0);
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

    // Configure a fast phoneme: rate = 15, dur mode = 3 → ~4 ms.
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
    assert(chip.phonemeRemainingCycles() == 0);

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
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);
    // Mode = MODE_IRQ_DISABLED (bits 7:6 = 00).
    chip.write(Ssi263::REG_DURPHON, 0x05);
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

void testDurationFormulaBounds()
{
    Ssi263 chip;
    chip.reset();
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);

    // Fastest: rate=15, dur mode=3 → (16-15)*4096/1023 * (4-3) ≈ 4 ms ≈ 4090 cyc.
    chip.write(Ssi263::REG_RATEINF, 0xF0);
    chip.write(Ssi263::REG_DURPHON, 0xC0);     // mode=11
    const int fastest = chip.phonemeRemainingCycles();
    assert(fastest > 0 && fastest < 10000);

    // Slowest: rate=0, dur mode=0 → 16*4096/1023 * 4 ≈ 256 ms ≈ 262k cyc.
    chip.write(Ssi263::REG_RATEINF, 0x00);
    // dur mode = 00 also means MODE_IRQ_DISABLED — that's fine for
    // duration math; we're just checking the formula not IRQ.
    chip.write(Ssi263::REG_DURPHON, 0x00);
    const int slowest = chip.phonemeRemainingCycles();
    assert(slowest > 200000 && slowest < 300000);

    // Slowest must be many times longer than fastest.
    assert(slowest > fastest * 20);

    std::printf("  ok: duration formula bounds (%d cyc fastest, %d cyc slowest)\n",
                fastest, slowest);
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

    // Live chip, mid-way through the LONGEST phoneme.
    Ssi263 live;
    live.reset();
    live.write(Ssi263::REG_CTTRAMP, 0x0F);       // powered up, amp 15
    live.write(Ssi263::REG_RATEINF, 0x00);
    live.write(Ssi263::REG_DURPHON,
               static_cast<uint8_t>(0x80 | (longest & 0x3F)));
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
    blob[14] = static_cast<uint8_t>(shortest);
    blob[15] = blob[16] = blob[17] = 0;
    // Also point DURPHON at it so the restored chip is otherwise coherent.
    blob[0] = static_cast<uint8_t>(0x80 | (shortest & 0x3F));

    Ssi263 restored;
    restored.reset();
    restored.loadSnapshot(blob.data());

    constexpr int N = 512;
    std::vector<float> got(N, 0.0f);
    restored.fillAudio(got.data(), N, 22050);

    // Reference: the same phoneme rendered from its start.
    Ssi263 ref;
    ref.reset();
    ref.write(Ssi263::REG_CTTRAMP, 0x0F);
    ref.write(Ssi263::REG_RATEINF, 0x00);
    ref.write(Ssi263::REG_DURPHON,
              static_cast<uint8_t>(0x80 | (shortest & 0x3F)));
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

int main()
{
    std::printf("Ssi263 chip smoke test\n");
    testResetState();
    testPhonemePlaysAndIrqs();
    testAckClearsRequest();
    testCtlPowerDownAndRestart();
    testCtlRestartRewindsPlaybackCursor();
    testIrqDisabledMode();
    testAudioRenderNonSilent();
    testDurationFormulaBounds();
    testSnapshotClampsPlaybackCursor();
    std::printf("PASS\n");
    return 0;
}
