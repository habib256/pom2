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
#include <cmath>
#include <limits>

namespace pom2 {

namespace {

// SSI263 phoneme CODE ($00-$3F) → index into the 62-entry PCM table.
//
// The blob in `Ssi263PhonemeData.cpp` is AppleWin's `g_nPhonemeInfo[62]`
// byte for byte, and AppleWin indexes it by code MINUS TWO
// (`SSI263.cpp SSI263::Play`):
//
//     if (nPhoneme == 1) nPhoneme = 2;   // "Missing this sample, so map to phoneme-2"
//     if (nPhoneme == 0) bPause = true;  // PA — silence
//     else               nPhoneme -= 2;  // "Missing phoneme-1"
//
// So code $00 is the PA pause (SILENCE — it is what a speech driver emits
// between words), code $01 folds onto entry 0, and codes $02..$3F are
// entries 0..61. Indexing the table by the raw code played every phoneme
// as the sound two codes higher, turned every pause into an audible
// vowel, and left codes $3E/$3F silent because they fell off the end of
// the 62-entry table.
//
// Returns -1 for "render silence".
constexpr int kPhonemePause = -1;
inline int phonemePcmIndex(int code)
{
    code &= 0x3F;
    if (code == 0) return kPhonemePause;
    if (code == 1) return 0;
    return code - 2;
}

int playbackDuration(uint8_t durPhon, Ssi263::SsiMode mode)
{
    // AppleWin 6c579da5bd94 SSI263.cpp:613-617, the primary audio path.
    return mode == Ssi263::MODE_FRAME_IMMEDIATE_INFLECTION
        ? 3 : (durPhon >> Ssi263::DURATION_MODE_SHIFT);
}

uint32_t nativeFrameCount(uint32_t sourceSamples, int dur)
{
    // Update():645-649 skips one source sample after every fourth READ
    // whose mod4 counter is 3. The skip itself does not increment mod4:
    // source offsets 3, 8, 13, ... are omitted (5 source samples / 4 frames).
    if (dur == 1) return sourceSamples - (sourceSamples + 1) / 5;
    const unsigned group = dur < 2 ? 1 : dur == 2 ? 2 : 4;
    return (sourceSamples + group - 1) / group;
}

} // namespace

void Ssi263::reset(bool powerCycle)
{
    // AppleWin SSI263.h ResetState(): typical power-on phoneme/mode and
    // observed filter silence (6c579da5bd94:56-68). The active function
    // and interrupt enable are not latched until the first CTL H->L.
    durPhon_ = 0xC0;
    inflect_ = 0;
    rateInf_ = 0;
    cttrAmp_ = CONTROL_MASK;     // CTL=1 → power-down at reset
    filFreq_ = powerCycle ? FILTER_FREQ_SILENCE : 0;
    mode_ = MODE_IRQ_DISABLED;
    irqEnabled_ = false;
    phonemeRemainingCycles_ = 0;
    aRequest_ = false;
    phonemeWriteCount_ = 0;
    playbackPhoneme_ = 0;
    playbackOffset_  = 0;
    resampleAccum_   = 0.0f;
    // The audio-side timeline is not guest state, but it MUST die with
    // the chip: a Sound II queuePlaybackEvent + F12 otherwise leaves
    // ctlEvents_ and aPrimed_ armed, so fillAudioTimed keeps rendering
    // the abandoned phoneme (and applyPlaybackEvent rewinds the cursor
    // the blob just restored — the double-rewind write() no longer does).
    ctlEvents_.clear();
    aPrimed_        = false;
    timedPlayback_  = false;
    aCttrAmp_       = CONTROL_MASK;
    aDurPhon_       = 0;
    aFilFreq_       = 0;
}

uint8_t Ssi263::peekRegister(uint8_t reg) const
{
    switch (reg & 0x07) {
    case REG_DURPHON: return durPhon_;
    case REG_INFLECT: return inflect_;
    case REG_RATEINF: return rateInf_;
    case REG_CTTRAMP: return cttrAmp_;
    case REG_FILFREQ:
    default:          return filFreq_;   // A2 alone decodes FILFREQ: 4..7
    }
}

uint8_t Ssi263::read(uint8_t /*reg*/) const
{
    // AppleWin: every register read returns a status byte whose bit 7
    // mirrors the A/!R signal. The other bits are documented as "don't
    // care" by Silicon Systems. Return only the driven bit; the card
    // supplies floating-bus bits where its read decode exposes them.
    return aRequest_ ? 0x80 : 0x00;
}

int Ssi263::computePhonemeDurationCycles() const
{
    // Use exactly the native output-frame count the audio path consumes,
    // not SSI_Output()'s debug formula or UpdateAccurateLength()'s fallback.
    // AppleWin Play(): the silent PA uses entry 0's length too.
    const int index = std::max(0, phonemePcmIndex(currentPhoneme()));
    const auto samples = ssi263_data::kPhonemeInfo[index].length;
    const auto frames = nativeFrameCount(samples, playbackDuration(durPhon_, mode_));
    const int64_t rate = ssi263_data::kPhonemeSampleRateHz;
    if (cpuClockHz_ > static_cast<int64_t>(std::numeric_limits<int>::max()) * rate / frames)
        return std::numeric_limits<int>::max();
    const int64_t cycles = (static_cast<int64_t>(frames) * cpuClockHz_ + rate - 1) / rate;
    return static_cast<int>(std::clamp<int64_t>(cycles, 1, std::numeric_limits<int>::max()));
}

void Ssi263::setCpuClockHz(double hz)
{
    if (!(hz > 0.0) || !std::isfinite(hz) ||
        hz >= static_cast<double>(std::numeric_limits<int64_t>::max())) return;
    const auto next = static_cast<int64_t>(hz + 0.5);
    if (next <= 0 || next == cpuClockHz_) return;
    // The chip oscillator does not accelerate with the 6502. Preserve the
    // remaining real time when its CPU-cycle representation changes.
    if (phonemeRemainingCycles_ > 0) {
        const long double scaled = std::ceil(
            static_cast<long double>(phonemeRemainingCycles_) * next / cpuClockHz_);
        phonemeRemainingCycles_ = static_cast<int>(std::min<long double>(
            scaled, std::numeric_limits<int>::max()));
    }
    cpuClockHz_ = next;
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
            //
            // ONLY on the untimed path (EchoPlusCard's own fillAudio). When
            // the owner stamps its stores, `applyPlaybackEvent` does this at
            // the audio cursor instead, and doing it here as well switched the
            // phoneme ~40 ms early and then replayed its first 40 ms.
            if (!timedPlayback_) {
                playbackPhoneme_ = currentPhoneme();
                playbackOffset_  = 0;
                resampleAccum_   = 0.0f;
            }
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
            // AppleWin SSI263.cpp SetDeviceModeAndInts():234-243.
            // DURPHON writes while running do not alter these latches.
            // Mode 00 disables IRQ while retaining the previous function.
            const auto requested = static_cast<SsiMode>(
                (durPhon_ & DURATION_MODE_MASK) >> DURATION_MODE_SHIFT);
            irqEnabled_ = requested != MODE_IRQ_DISABLED;
            if (irqEnabled_) mode_ = requested;
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
            //
            // Untimed path only — see the DURPHON case above.
            if (!timedPlayback_) {
                playbackPhoneme_ = currentPhoneme();
                playbackOffset_  = 0;
                resampleAccum_   = 0.0f;
            }
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
    case REG_FILFREQ:
    default:
        // AppleWin `SSI263.cpp`: `case SSI_FILFREQ: // RegAddr.b2=1 (b1 &
        // b0 are: don't care)` followed by `default:` — the chip decodes
        // FILFREQ on A2 alone, so register addresses 4, 5, 6 and 7 all
        // land on it. Dropping 5-7 meant a Sound II driver writing the
        // $FF silence sentinel through `$Cs45`/`$Cs46`/`$Cs47` (which
        // `MockingboardCard::slotRomWrite` forwards as `low8 & 0x07`) was
        // ignored and the chip kept talking.
        filFreq_ = val;
        break;
    }
    return aRequestCleared;
}

bool Ssi263::advance(int cycles)
{
    if (cycles <= 0) return false;
    if (phonemeRemainingCycles_ <= 0) return false;
    if (powerDown()) return false;
    const int64_t remaining = static_cast<int64_t>(phonemeRemainingCycles_) - cycles;
    if (remaining > 0) {
        phonemeRemainingCycles_ = static_cast<int>(remaining);
        return false;
    }
    // Phonemes repeat until replaced or powered down (AppleWin SSI263.cpp
    // RepeatPhoneme/SetSpeechIRQ). Keep the repeat phase even while D7 is
    // held high, so acknowledging via INFLECT/RATEINF permits another
    // request at the next boundary without restarting the phoneme.
    const int period = std::max(1, computePhonemeDurationCycles());
    phonemeRemainingCycles_ = period - static_cast<int>((-remaining) % period);
    // Phoneme just finished. Per AppleWin SSI263.cpp SetSpeechIRQ, the
    // A/!R pin is raised on phoneme completion regardless of the DR1:0
    // mode bits (only power-down — already handled above — suppresses it).
    // The mode bits only gate whether the *host IRQ* fires. The return
    // value signals an IRQ-worthy edge, so in MODE_IRQ_DISABLED (00) we
    // still set D7 (pollable via read()) but report no IRQ edge to the host.
    if (aRequest_) return false;   // already requesting; no new edge
    aRequest_ = true;
    return irqEnabled();
}

// ─── Audio render — native PCM grouping + host-rate sample hold ──────────
//
// Native output rate is 22050 Hz, as in AppleWin. DUR selects source
// consumption/averaging on that grid. The host (44100/48000 typical)
// holds each native output frame until resampleAccum_ advances to the
// next one. Inflection, articulation and analog filtering are not modelled.
//
// Power-down + amp=0 + filFreq=$FF (silence sentinel) all squelch
// the contribution. Otherwise we sum (not overwrite) into `output`
// so the host's AudioSrc can mix multiple sources.

void Ssi263::prepareTimedPlayback()
{
    if (!aPrimed_) {
        aDurPhon_ = durPhon_;
        aCttrAmp_ = cttrAmp_;
        aFilFreq_ = filFreq_;
        aMode_ = mode_;
        aPrimed_ = true;
    }
    timedPlayback_ = true;
}

void Ssi263::queuePlaybackEvent(uint8_t reg, uint8_t val, uint64_t cycle)
{
    // From here on the PCM cursor belongs to `applyPlaybackEvent`, which runs
    // at the audio cursor. See `timedPlayback_` in the header.
    timedPlayback_ = true;
    if (!ctlEvents_.push_back(CtlEvent{cycle, static_cast<uint8_t>(reg & 0x07),
                                       val})) {
        // Full — the audio device is stalled or absent, and a stale backlog
        // is worse than none: drop it and re-prime from the live registers
        // on the next buffer, the same policy the PSG timeline uses.
        ctlEvents_.clear();
        aPrimed_ = false;
    }
}

void Ssi263::applyPlaybackEvent(uint8_t reg, uint8_t val)
{
    switch (reg & 0x07) {
    case REG_DURPHON:
        aDurPhon_ = val;
        // Mirrors `write()`: a DURPHON store rewinds the PCM cursor to the
        // start of the newly latched phoneme — unless the chip is down.
        if ((aCttrAmp_ & CONTROL_MASK) == 0) {
            playbackPhoneme_ = aDurPhon_ & PHONEME_MASK;
            playbackOffset_  = 0;
            resampleAccum_   = 0.0f;
        }
        break;
    case REG_CTTRAMP: {
        const uint8_t prevCtl = aCttrAmp_ & CONTROL_MASK;
        aCttrAmp_ = val;
        // CTL H→L: leave power-down and restart the latched phoneme.
        if (prevCtl != 0 && (aCttrAmp_ & CONTROL_MASK) == 0) {
            const auto requested = static_cast<SsiMode>(aDurPhon_ >> DURATION_MODE_SHIFT);
            if (requested != MODE_IRQ_DISABLED) aMode_ = requested;
            playbackPhoneme_ = aDurPhon_ & PHONEME_MASK;
            playbackOffset_  = 0;
            resampleAccum_   = 0.0f;
        }
        break;
    }
    case REG_FILFREQ:
    case 5: case 6: case 7: aFilFreq_ = val; break;
    default: break;   // INFLECT / RATEINF synthesis is outside the fixed PCM model
    }
}

void Ssi263::fillAudioTimed(float* output, int frameCount, uint32_t sampleRate,
                            double startCycle, double cyclesPerSample)
{
    if (frameCount <= 0 || sampleRate == 0) return;
    if (!(cyclesPerSample > 0.0)) {           // no timeline: render as-is
        fillAudio(output, frameCount, sampleRate);
        return;
    }
    if (!aPrimed_) {                          // first buffer, or after a drop
        aDurPhon_ = durPhon_;
        aCttrAmp_ = cttrAmp_;
        aFilFreq_ = filFreq_;
        aMode_    = mode_;
        aPrimed_  = true;
    }

    int    i     = 0;
    double cycle = startCycle;
    while (i < frameCount) {
        // Everything already due happens BEFORE this sample.
        while (!ctlEvents_.empty() &&
               static_cast<double>(ctlEvents_.front().cycle) <= cycle) {
            applyPlaybackEvent(ctlEvents_.front().reg, ctlEvents_.front().val);
            ctlEvents_.pop_front();
        }
        // Render up to the next event, so a phoneme boundary lands on the
        // sample the driver's write falls in rather than at a buffer edge.
        int n = frameCount - i;
        if (!ctlEvents_.empty()) {
            const double ahead =
                (static_cast<double>(ctlEvents_.front().cycle) - cycle) /
                cyclesPerSample;
            if (ahead < static_cast<double>(n))
                n = std::max(1, static_cast<int>(ahead));
        }
        renderSamples(output + i, n, sampleRate, aCttrAmp_, aFilFreq_, aDurPhon_, aMode_);
        i     += n;
        cycle += static_cast<double>(n) * cyclesPerSample;
    }
}

void Ssi263::fillAudio(float* output, int frameCount, uint32_t sampleRate)
{
    renderSamples(output, frameCount, sampleRate, cttrAmp_, filFreq_, durPhon_, mode_);
}

void Ssi263::renderSamples(float* output, int frameCount, uint32_t sampleRate,
                           uint8_t cttrAmp, uint8_t filFreq,
                           uint8_t durPhon, SsiMode mode)
{
    if (frameCount <= 0 || sampleRate == 0) return;
    if ((cttrAmp & CONTROL_MASK) != 0) return;      // power-down
    const uint8_t amp = cttrAmp & AMPLITUDE_MASK;

    // Phoneme CODE → PCM table index (AppleWin `Play()`; see the mapper).
    // `kPhonemePause` ($00, the PA phoneme) renders nothing, which is
    // exactly what summing a silent buffer does.
    const int   ph = phonemePcmIndex(playbackPhoneme_);
    if (ph == kPhonemePause)      return;
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

    // Silence is an output gain, not a pause. AppleWin SSI263.cpp at
    // 6c579da5bd94, Update():605-654 zeros amplitude for AMP=0 or
    // FILFREQ=$FF but still consumes PCM. Freezing here resumed the old
    // phoneme position when a guest speech driver restored its gain.
    const int level = filFreq == FILTER_FREQ_SILENCE ? 0 : amp;
    const double gain = static_cast<double>(level) / 15.0;
    const int dur = playbackDuration(durPhon, mode);
    const unsigned group = dur < 2 ? 1 : dur == 2 ? 2 : 4;
    const float step = static_cast<float>(ssi263_data::kPhonemeSampleRateHz)
                     / static_cast<float>(sampleRate);

    for (int i = 0; i < frameCount; ++i) {
        // AppleWin Update(): scale/truncate each source sample, then
        // average groups of 1/2/4. A partial final group produces silence.
        const size_t idx = info.offset + playbackOffset_;
        if (idx >= ssi263_data::kPhonemeDataLen) break;
        int sum = 0;
        if (playbackOffset_ + group <= info.length) {
            for (unsigned n = 0; n < group; ++n)
                sum += static_cast<int>(
                    static_cast<int16_t>(ssi263_data::kPhonemeData[idx + n]) * gain);
        }
        output[i] += static_cast<float>(sum / static_cast<int>(group)) / 32768.0f;

        // Advance the source cursor by `step` (typically < 1 at host
        // rates ≥ source rate).
        resampleAccum_ += step;
        while (resampleAccum_ >= 1.0f) {
            resampleAccum_ -= 1.0f;
            playbackOffset_ += dur == 1 && playbackOffset_ % 5 == 2 ? 2 : group;
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
