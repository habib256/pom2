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

// EchoPlusCard — see header for address map + protocol notes.

#include "EchoPlusCard.h"

#include "M6502.h"

#include <algorithm>
#include <vector>

// ─── AudioSrc — silent placeholder until phoneme PCM data is imported ──

struct EchoPlusCard::AudioSrc : public AudioSource, public RateAware
{
    explicit AudioSrc(EchoPlusCard* p) : parent(p) {}

    EchoPlusCard* parent;

    std::atomic<uint32_t> sampleRate { kAudioSampleRate };
    std::atomic<float>    volume     { 0.7f };
    std::atomic<bool>     muted      { false };

    // Render scratch — member, not a per-callback local: this runs on the
    // realtime audio thread, where a heap allocation per buffer tick is
    // the canonical source of underruns. Audio-thread-only.
    std::vector<float> scratch;

    void setSampleRate(uint32_t hz) override
    {
        if (hz == 0) hz = kAudioSampleRate;
        sampleRate.store(hz, std::memory_order_relaxed);
    }

    void fillAudioBuffer(float* output, int frameCount) override
    {
        if (frameCount <= 0) return;
        std::fill_n(output, frameCount, 0.0f);
        if (muted.load(std::memory_order_relaxed)) return;
        const float vol = volume.load(std::memory_order_relaxed);
        const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
        // Render under the parent mutex so the chip's playback cursor
        // is consistent with CPU-thread register writes. Render into a
        // temp buffer + scale by master volume so the chip's own
        // amplitude register stays in `Ssi263::fillAudio` (single
        // source of truth for chip-side scaling).
        if (scratch.size() < static_cast<size_t>(frameCount))
            scratch.resize(static_cast<size_t>(frameCount));
        std::fill_n(scratch.begin(), frameCount, 0.0f);
        {
            std::lock_guard<std::mutex> lk(parent->mtx_);
            parent->ssi_.fillAudio(scratch.data(), frameCount, sr);
        }
        for (int i = 0; i < frameCount; ++i) output[i] = scratch[i] * vol;
    }
};

// ─── EchoPlusCard ─────────────────────────────────────────────────────────

EchoPlusCard::EchoPlusCard(int slotNum)
    : slot_(slotNum)
{
    audio_ = std::make_unique<AudioSrc>(this);
    onReset();
}

EchoPlusCard::~EchoPlusCard() = default;

void EchoPlusCard::onUnplug()
{
    // SlotBus auto-releases pending IRQ on detach.
}

void EchoPlusCard::onReset()
{
    std::lock_guard<std::mutex> lk(mtx_);
    ssi_.reset();
    assertIrq(false);
}

AudioSource* EchoPlusCard::audioSource() { return audio_.get(); }

void EchoPlusCard::setSampleRate(uint32_t hz)
{
    if (hz == 0) hz = kAudioSampleRate;
    audio_->sampleRate.store(hz, std::memory_order_relaxed);
}

void EchoPlusCard::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    audio_->volume.store(v, std::memory_order_relaxed);
}

float EchoPlusCard::getVolume() const
{
    return audio_->volume.load(std::memory_order_relaxed);
}

void EchoPlusCard::setMuted(bool m) { audio_->muted.store(m, std::memory_order_relaxed); }
bool EchoPlusCard::isMuted()  const { return audio_->muted.load(std::memory_order_relaxed); }

uint8_t EchoPlusCard::slotRomRead(uint8_t low8)
{
    std::lock_guard<std::mutex> lk(mtx_);
    // Five registers decoded, and the rest of the page is not this card's:
    // an undecoded slot-ROM read is the FLOATING BUS, not a hard $FF
    // (CLAUDE.md; `openBus()` answers $FF anyway on a bus with no source,
    // which is what the harnesses see).
    //
    // OPEN QUESTION (bug hunt #18): the CHIP aliases registers 4-7 onto
    // FILFREQ (A2 alone — hunt #16, `ssi263_phoneme_map`), and the
    // Mockingboard path reaches those aliases because it forwards
    // `low8 & 0x07`. Whether the Cricket/Echo card's own decode passes A0-A2
    // through — making $Cs05-$Cs07 three more FILFREQs rather than open bus
    // — is not settled by anything POM2 has: no card schematic, no driver
    // in the corpus that writes there. Left as it was, deliberately.
    if (low8 > pom2::Ssi263::REG_FILFREQ) return openBus();
    return ssi_.read(low8);
}

void EchoPlusCard::slotRomWrite(uint8_t low8, uint8_t v)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (low8 > pom2::Ssi263::REG_FILFREQ) return;
    const bool aRequestCleared = ssi_.write(low8, v);
    if (aRequestCleared) {
        // Host ack'd the previous phoneme request → drop slot IRQ.
        assertIrq(false);
    }
}

void EchoPlusCard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (ssi_.advance(cycles)) {
        // Phoneme just finished + chip is requesting next → assert IRQ.
        assertIrq(true);
    }
}

void EchoPlusCard::updateIrqFromChip()
{
    // Called by external test code if needed; production path goes
    // through advanceCycles / slotRomWrite which already manage the
    // IRQ line transitions.
    assertIrq(ssi_.aRequest() && ssi_.irqEnabled());
}

EchoPlusCard::ChipSnap EchoPlusCard::snapshotChip() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    ChipSnap s{};
    for (int r = 0; r <= pom2::Ssi263::REG_FILFREQ; ++r) {
        s.regs[r] = ssi_.peekRegister(static_cast<uint8_t>(r));
    }
    s.currentPhoneme         = ssi_.currentPhoneme();
    s.mode                   = static_cast<uint8_t>(ssi_.currentMode());
    s.aRequest               = ssi_.aRequest();
    s.powerDown              = ssi_.powerDown();
    s.irqEnabled             = ssi_.irqEnabled();
    s.phonemeRemainingCycles = ssi_.phonemeRemainingCycles();
    s.phonemeWriteCount      = ssi_.phonemeWriteCount();
    return s;
}

void EchoPlusCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    // 3-byte tag + the chip's fixed blob, so a foreign/old blob is
    // rejected cleanly in load (length + tag gate).
    out.push_back('E'); out.push_back('P'); out.push_back(1);
    ssi_.appendSnapshot(out);
}

void EchoPlusCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (len < 3 + pom2::Ssi263::kSnapshotBytes) return;
    if (data[0] != 'E' || data[1] != 'P' || data[2] != 1) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ssi_.loadSnapshot(data + 3);
    }
    // Re-derive the slot IRQ line from the restored A/!R + enable state
    // (same post-restore refresh Mockingboard does for its chips).
    updateIrqFromChip();
}
