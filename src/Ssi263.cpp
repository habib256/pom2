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

// Ssi263 — see header for register layout + protocol notes.

#include "Ssi263.h"
#include "Ssi263PhonemeData.h"

#include <algorithm>

namespace pom2 {

namespace {

// 4-bit AMP register → linear gain. The SSI263 datasheet doesn't
// publish a normalised amplitude table, but the chip's published
// behaviour is "monotonic, perceptually log-like". AppleWin uses a
// straight linear ramp (amp/15.0). We do the same.
inline float ampToGain(uint8_t amp4)
{
    return static_cast<float>(amp4 & 0x0F) / 15.0f;
}

} // namespace

void Ssi263::reset()
{
    durPhon_ = 0;
    inflect_ = 0;
    rateInf_ = 0;
    cttrAmp_ = CONTROL_MASK;     // CTL=1 → power-down at reset
    filFreq_ = 0;
    phonemeRemainingCycles_ = 0;
    aRequest_ = false;
    phonemeWriteCount_ = 0;
    playbackPhoneme_ = 0;
    playbackOffset_  = 0;
    resampleAccum_   = 0.0f;
}

uint8_t Ssi263::peekRegister(uint8_t reg) const
{
    switch (reg & 0x07) {
    case REG_DURPHON: return durPhon_;
    case REG_INFLECT: return inflect_;
    case REG_RATEINF: return rateInf_;
    case REG_CTTRAMP: return cttrAmp_;
    case REG_FILFREQ: return filFreq_;
    default:          return 0xFF;
    }
}

uint8_t Ssi263::read(uint8_t /*reg*/) const
{
    // AppleWin: every register read returns a status byte whose bit 7
    // mirrors the A/!R signal. The other bits are documented as "don't
    // care" by Silicon Systems; we return 0 to match AppleWin's
    // observable behaviour (no driver depends on the low 7 bits).
    return aRequest_ ? 0x80 : 0x00;
}

int Ssi263::computePhonemeDurationCycles() const
{
    // AppleWin SSI263.cpp ~line 290:
    //   phonemeDuration_ms = ((16 - (rate>>4)) * 4096 / 1023) * (4 - (dur>>6))
    //
    // (rate>>4) ∈ [0..15], so (16 - rate) ∈ [1..16] → up to 64 ms base.
    // (dur>>6) ∈ [0..3], so (4 - dur) ∈ [1..4] → up to 4× multiplier.
    // Total range: ~4 ms (rate=15, dur=3) → ~256 ms (rate=0, dur=0).
    const int rate = (rateInf_ & RATE_MASK) >> 4;
    const int dur  = (durPhon_ & DURATION_MODE_MASK) >> DURATION_MODE_SHIFT;
    const int ms   = ((16 - rate) * 4096 / 1023) * (4 - dur);
    // Cycles per millisecond ≈ 1022.727. Use integer math for
    // determinism (no float).
    return (ms * static_cast<int>(POM2_CPU_CLOCK_HZ)) / 1000;
}

bool Ssi263::write(uint8_t reg, uint8_t val)
{
    bool aRequestCleared = false;

    // Writes to $00..$02 clear A/!R and the chip's pending IRQ
    // (AppleWin: any of the first 3 registers' writes count as the
    // host "acknowledging" the previous phoneme request).
    if (reg <= REG_RATEINF) {
        if (aRequest_) aRequestCleared = true;
        aRequest_ = false;
    }

    switch (reg & 0x07) {
    case REG_DURPHON: {
        durPhon_ = val;
        // Start a fresh phoneme unless the chip is in power-down.
        if (!powerDown()) {
            phonemeRemainingCycles_ = computePhonemeDurationCycles();
            ++phonemeWriteCount_;
            // Reset audio playback cursor to the start of the newly
            // latched phoneme. Without this, a new phoneme would
            // resume mid-sample at the previous cursor.
            playbackPhoneme_ = currentPhoneme();
            playbackOffset_  = 0;
            resampleAccum_   = 0.0f;
        }
        break;
    }
    case REG_INFLECT: inflect_ = val; break;
    case REG_RATEINF: rateInf_ = val; break;
    case REG_CTTRAMP: {
        const uint8_t prevCtl = cttrAmp_ & CONTROL_MASK;
        cttrAmp_ = val;
        const uint8_t newCtl  = cttrAmp_ & CONTROL_MASK;
        // CTL H→L (1→0): exit power-down, restart the loaded phoneme
        // (if any). This is how a freshly-powered chip starts speaking
        // — datasheet "Phoneme Initiation".
        if (prevCtl != 0 && newCtl == 0) {
            phonemeRemainingCycles_ = computePhonemeDurationCycles();
            // Power-up doesn't itself bump phonemeWriteCount_ — only
            // an explicit DURPHON write does.
            //
            // AppleWin SSI263.cpp:200-209 — the CTL H→L branch runs
            // `Play(m_durationPhoneme & PHONEME_MASK)`, which rewinds
            // the PCM playback cursor to the start of the LATCHED
            // phoneme (not just the IRQ countdown above). Without this
            // the audio side resumed mid-sample at whatever cursor the
            // pre-power-down phoneme left behind.
            playbackPhoneme_ = currentPhoneme();
            playbackOffset_  = 0;
            resampleAccum_   = 0.0f;
        }
        // CTL L→H (0→1): power-down silences audio + clears A/!R + drops
        // any pending IRQ (AppleWin SSI263.cpp ~line 165).
        if (prevCtl == 0 && newCtl != 0) {
            if (aRequest_) aRequestCleared = true;
            aRequest_ = false;
            phonemeRemainingCycles_ = 0;
        }
        break;
    }
    case REG_FILFREQ: filFreq_ = val; break;
    default: break;
    }
    return aRequestCleared;
}

bool Ssi263::advance(int cycles)
{
    if (cycles <= 0) return false;
    if (phonemeRemainingCycles_ <= 0) return false;
    if (powerDown()) return false;
    phonemeRemainingCycles_ -= cycles;
    if (phonemeRemainingCycles_ > 0) return false;
    // Phoneme just finished. Don't re-tick over zero; leave the counter
    // at 0 and assert A/!R (D7). Per AppleWin SSI263.cpp ~line 724, the
    // A/!R pin is raised on phoneme completion regardless of the DR1:0
    // mode bits (only power-down — already handled above — suppresses it).
    // The mode bits only gate whether the *host IRQ* fires. The return
    // value signals an IRQ-worthy edge, so in MODE_IRQ_DISABLED (00) we
    // still set D7 (pollable via read()) but report no IRQ edge to the host.
    phonemeRemainingCycles_ = 0;
    if (aRequest_) return false;   // already requesting; no new edge
    aRequest_ = true;
    return irqEnabled();
}

// ─── Audio render — PCM phoneme lookup + linear resample ─────────────────
//
// Source rate = 22050 Hz (Ssi263PhonemeData::kPhonemeSampleRateHz).
// Host rate is whatever miniaudio negotiated (44100 or 48000 typical).
// We step the source cursor by `kPhonemeSampleRateHz / sampleRate`
// per output sample; the integer part advances the offset, the
// fractional remainder is held in `resampleAccum_`. No filtering —
// the source material is already band-limited by the original
// SSI263 chip's analog output. Quality is fine for speech.
//
// Power-down + amp=0 + filFreq=$FF (silence sentinel) all squelch
// the contribution. Otherwise we sum (not overwrite) into `output`
// so the host's AudioSrc can mix multiple sources.

void Ssi263::fillAudio(float* output, int frameCount, uint32_t sampleRate)
{
    if (frameCount <= 0 || sampleRate == 0) return;
    if (powerDown())              return;
    if (filFreq_ == FILTER_FREQ_SILENCE) return;
    const uint8_t amp = amplitude();
    if (amp == 0)                 return;

    const int   ph = playbackPhoneme_ & 0x3F;
    if (static_cast<size_t>(ph) >= ssi263_data::kNumPhonemes) return;
    const auto& info = ssi263_data::kPhonemeInfo[ph];
    if (info.length == 0)         return;

    // Re-clamp the cursor to THIS phoneme. `playbackPhoneme_` and
    // `playbackOffset_` are restored as an unvalidated pair by
    // `loadSnapshot` (rewind, or a hand-edited/older .pom2snap), so an
    // offset captured mid-way through a long phoneme can come back
    // attached to a short one. `info.offset + playbackOffset_` then reads
    // PCM belonging to the NEXT phoneme in the blob — an audible burst of
    // the wrong sound until the driver latches its next DURPHON — or
    // walks off the end of the blob and silently truncates the buffer at
    // the `idx >= kPhonemeDataLen` break below. Cheap enough to keep in
    // the steady-state path (one compare per callback), which also covers
    // any future writer of these members.
    if (playbackOffset_ >= info.length) {
        playbackOffset_ = 0;
        resampleAccum_  = 0.0f;
    }

    const float gain = ampToGain(amp) * (1.0f / 32768.0f);
    const float step = static_cast<float>(ssi263_data::kPhonemeSampleRateHz)
                     / static_cast<float>(sampleRate);

    for (int i = 0; i < frameCount; ++i) {
        // Read sample at current offset (signed 16-bit).
        const size_t idx = info.offset + playbackOffset_;
        if (idx >= ssi263_data::kPhonemeDataLen) break;
        const int16_t s = static_cast<int16_t>(ssi263_data::kPhonemeData[idx]);
        output[i] += static_cast<float>(s) * gain;

        // Advance the source cursor by `step` (typically < 1 at host
        // rates ≥ source rate).
        resampleAccum_ += step;
        while (resampleAccum_ >= 1.0f) {
            resampleAccum_ -= 1.0f;
            ++playbackOffset_;
            if (playbackOffset_ >= info.length) {
                // Phoneme ran out of samples — loop it back to start.
                // The aRequest/phonemeRemainingCycles state machine on
                // the CPU thread decides when to stop calling fillAudio
                // for this phoneme (by writing a new DURPHON or going
                // power-down); audio-side just keeps the chip "voiced"
                // until then.
                playbackOffset_ = 0;
            }
        }
    }
}

} // namespace pom2
