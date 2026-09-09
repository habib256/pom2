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

// SSI263 phoneme CODE -> PCM table INDEX, and the FILFREQ A2 decode.
//
// The blob in `Ssi263PhonemeData.cpp` is AppleWin's `g_nPhonemeInfo[62]`
// byte for byte, and AppleWin indexes it by phoneme code MINUS TWO
// (`source/SSI263.cpp`, `SSI263::Play`):
//
//     if (nPhoneme == 1) nPhoneme = 2;    // "Missing this sample, so map to phoneme-2"
//     if (nPhoneme == 0) bPause = true;   // PA — silence
//     else               nPhoneme -= 2;   // "Missing phoneme-1"
//     m_phonemeLengthRemaining = g_nPhonemeInfo[nPhoneme].nLength;
//
// POM2 indexed the table by the RAW code, so every one of the 62 phonemes
// rendered as the sound two codes higher, the PA pause ($00 — what a
// speech driver emits between words) came out as an audible vowel at
// RMS 0.19, and codes $3E/$3F fell off the end of the table and were
// silent. Bug hunt 16.
//
// Second pin in the same file: the SSI263 decodes FILFREQ on A2 alone
// (AppleWin `case SSI_FILFREQ: // RegAddr.b2=1 (b1 & b0 are: don't care)`
// followed by `default:`), so register addresses 4..7 are all FILFREQ.
// `MockingboardCard::slotRomWrite` forwards `$Cs40-$Cs4F` as
// `low8 & 0x07`, so a Sound II driver reaches registers 5, 6 and 7; POM2
// dropped those writes, and the $FF silence sentinel with them.

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
namespace pd = pom2::ssi263_data;

// Render `n` samples of phoneme `code` at the blob's own rate, so the
// resampler is a 1:1 copy and the output is exactly gain * PCM.
std::vector<float> renderCode(uint8_t code, int n)
{
    Ssi263 chip;
    chip.reset();
    chip.write(Ssi263::REG_CTTRAMP, 0x0F);                        // CTL=0, amp=15
    chip.write(Ssi263::REG_RATEINF, 0x00);                        // slowest rate
    chip.write(Ssi263::REG_DURPHON,
               static_cast<uint8_t>(0x80 | (code & 0x3F)));       // mode 10
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);
    chip.fillAudio(buf.data(), n, pd::kPhonemeSampleRateHz);
    return buf;
}

// The first `n` samples of PCM table entry `idx`, scaled as fillAudio does.
std::vector<float> entrySamples(int idx, int n)
{
    assert(idx >= 0 && static_cast<size_t>(idx) < pd::kNumPhonemes);
    const auto& info = pd::kPhonemeInfo[idx];
    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        const size_t off = static_cast<size_t>(i) % info.length;
        out[static_cast<size_t>(i)] =
            static_cast<float>(static_cast<int16_t>(
                pd::kPhonemeData[info.offset + off])) * (1.0f / 32768.0f);
    }
    return out;
}

double rms(const std::vector<float>& v)
{
    double s = 0.0;
    for (float x : v) s += static_cast<double>(x) * x;
    return std::sqrt(s / static_cast<double>(v.size()));
}

// AppleWin's mapping, restated.
int wantEntry(int code)
{
    if (code == 0) return -1;      // PA — silence
    if (code == 1) return 0;
    return code - 2;
}

void testEveryCodeMapsToAppleWinsEntry()
{
    constexpr int N = 256;
    for (int code = 0; code < 64; ++code) {
        const auto got  = renderCode(static_cast<uint8_t>(code), N);
        const int  want = wantEntry(code);
        if (want < 0) {
            // $00 is the PA pause. Every inter-word gap in every utterance
            // goes through it; it must be SILENT.
            if (rms(got) != 0.0) {
                std::fprintf(stderr,
                    "ssi263 phoneme map: code $00 (PA, pause) rendered rms=%.5f, "
                    "expected silence (AppleWin SSI263::Play bPause branch)\n",
                    rms(got));
                std::abort();
            }
            continue;
        }
        const auto exp = entrySamples(want, N);
        for (int i = 0; i < N; ++i) {
            if (got[i] != exp[i]) {
                // Report which entry it DID come from, so the off-by-two
                // reads straight off the failure.
                int from = -1;
                for (size_t e = 0; e < pd::kNumPhonemes; ++e) {
                    const auto cand = entrySamples(static_cast<int>(e), N);
                    if (cand[0] == got[0] && cand[1] == got[1]) { from = static_cast<int>(e); break; }
                }
                std::fprintf(stderr,
                    "ssi263 phoneme map: code $%02X rendered PCM entry %d, "
                    "expected entry %d (AppleWin indexes g_nPhonemeInfo[] by "
                    "code-2); sample %d = %.6f vs %.6f\n",
                    code, from, want, i, got[i], exp[i]);
                std::abort();
            }
        }
    }
    std::printf("  ok: all 64 phoneme codes index the AppleWin entry (code-2); "
                "$00 = PA silence\n");
}

void testTopTwoCodesAreNotSilent()
{
    // Codes $3E/$3F are entries 60/61. Indexing by the raw code ran off the
    // end of the 62-entry table and rendered nothing at all.
    for (int code : {0x3E, 0x3F}) {
        const auto buf = renderCode(static_cast<uint8_t>(code), 4096);
        if (rms(buf) <= 0.005) {
            std::fprintf(stderr,
                "ssi263 phoneme map: code $%02X rms=%.5f — the top two phoneme "
                "codes fell off the end of the 62-entry table\n", code, rms(buf));
            std::abort();
        }
    }
    std::printf("  ok: codes $3E/$3F render entries 60/61 (not silence)\n");
}

void testFilterFreqDecodesOnA2Alone()
{
    // Registers 4..7 are all FILFREQ. Writing the $FF silence sentinel
    // through register 7 (reachable as `$Cs47` on a Sound II) must squelch.
    for (uint8_t reg = 4; reg <= 7; ++reg) {
        Ssi263 chip;
        chip.reset();
        chip.write(Ssi263::REG_CTTRAMP, 0x0F);
        chip.write(Ssi263::REG_RATEINF, 0x00);
        chip.write(Ssi263::REG_DURPHON, 0x80 | 0x05);
        // Sanity: audible before the sentinel.
        std::vector<float> pre(1024, 0.0f);
        chip.fillAudio(pre.data(), 1024, 44100);
        assert(rms(pre) > 0.005);

        const bool acked = chip.write(reg, Ssi263::FILTER_FREQ_SILENCE);
        if (acked) {
            std::fprintf(stderr, "ssi263: write to register %u must not ack A/!R\n", reg);
            std::abort();
        }
        std::vector<float> post(1024, 0.0f);
        chip.fillAudio(post.data(), 1024, 44100);
        if (rms(post) != 0.0) {
            std::fprintf(stderr,
                "ssi263 FILFREQ decode: writing $FF to register %u left the chip "
                "audible (rms=%.5f). AppleWin decodes FILFREQ on A2 alone — "
                "registers 4..7 are all FILFREQ.\n", reg, rms(post));
            std::abort();
        }
        if (chip.peekRegister(reg) != Ssi263::FILTER_FREQ_SILENCE) {
            std::fprintf(stderr,
                "ssi263 FILFREQ decode: peekRegister(%u) = $%02X, expected $FF\n",
                reg, chip.peekRegister(reg));
            std::abort();
        }
    }
    std::printf("  ok: registers 4..7 all decode as FILFREQ (A2-only decode)\n");
}

} // namespace

int main()
{
    std::printf("Ssi263 phoneme-map / FILFREQ-decode test\n");
    testEveryCodeMapsToAppleWinsEntry();
    testTopTwoCodesAreNotSilent();
    testFilterFreqDecodesOnA2Alone();
    std::printf("PASS\n");
    return 0;
}
