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

// PorteParoleCard — see the header. The timing shape (sync to the access
// cycle, an internal host-rate queue the mixer drains) is EchoPlusTMS5220Card's.

#include "PorteParoleCard.h"

#include "ByteIO.h"
#include "M6502.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pom2 {

namespace {
constexpr uint32_t kNativeRate = Mea8000::kOutputRate;
constexpr std::size_t kMaxQueue = 8192;
constexpr std::size_t kSnapshotV2Bytes = 4 + Mea8000::kSnapshotBytes + 8;
constexpr std::size_t kSnapshotBytes = kSnapshotV2Bytes + 8;

// Outside an instruction, getCycleCountNow still includes the last opcode
// scratch count. All lifecycle methods run at a completed bus boundary.
uint64_t boundaryCycle(const M6502* cpu)
{
    return cpu->getCycleCountNow() - uint64_t(cpu->getCurrentInstructionCycles());
}
} // namespace

struct PorteParoleCard::AudioSrc : AudioSource, RateAware {
    PorteParoleCard& card;
    explicit AudioSrc(PorteParoleCard& c) : card(c) {}
    void setSampleRate(uint32_t hz) override { card.setSampleRate(hz); }
    void fillAudioBuffer(float* out, int count) override {
        std::lock_guard<std::mutex> lock(card.mtx_);
        for (int i = 0; i < count; ++i) {
            float s = 0.0f;
            if (!card.pcm_.empty()) { s = card.pcm_.front(); card.pcm_.pop_front(); }
            out[i] = card.muted_ ? 0.0f : s * card.volume_;
        }
        // A backlog built while emulation ran ahead (turbo, speed > 1x) would
        // keep the voice late for the rest of the session; snap anything past
        // 100 ms back to 50 ms, as the Echo+ does.
        const std::size_t maxLead = card.rate_ / 10, keep = card.rate_ / 20;
        if (card.pcm_.size() > maxLead)
            card.pcm_.erase(card.pcm_.begin(),
                            card.pcm_.begin() + std::ptrdiff_t(card.pcm_.size() - keep));
    }
};

PorteParoleCard::PorteParoleCard(int slot)
    : audio_(std::make_unique<AudioSrc>(*this)), slot_(slot) { scratch_.reserve(481); }

PorteParoleCard::~PorteParoleCard() = default;

AudioSource* PorteParoleCard::audioSource() { return audio_.get(); }

void PorteParoleCard::setCpu(M6502* cpu)
{
    std::lock_guard<std::mutex> lock(mtx_);
    cpu_ = cpu;
    lastCycle_ = cpu ? boundaryCycle(cpu) : 0;
}

void PorteParoleCard::setCpuClock(double hz)
{
    std::lock_guard<std::mutex> lock(mtx_);
    // No sync() here, for the Echo+'s reason: the setters run between
    // instructions, after advanceCycles has brought the card to the boundary.
    if (std::isfinite(hz) && hz >= 100000 && hz <= 1000000000) {
        clockRemainder_ *= hz / cpuHz_;
        cpuHz_ = hz;
    }
}

void PorteParoleCard::setSampleRate(uint32_t hz)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (hz < 8000 || hz > 192000 || hz == rate_) return;
    rate_ = hz;
    samplePhase_ = 0;
    previousSample_ = 0;
    pcm_.clear();
}

void PorteParoleCard::setVolume(float v)
{
    std::lock_guard<std::mutex> l(mtx_);
    volume_ = std::isfinite(v) ? std::clamp(v, 0.0f, 2.0f) : 0.0f;
}
float PorteParoleCard::getVolume() const { std::lock_guard<std::mutex> l(mtx_); return volume_; }
void PorteParoleCard::setMuted(bool v) { std::lock_guard<std::mutex> l(mtx_); muted_ = v; }
bool PorteParoleCard::isMuted() const { std::lock_guard<std::mutex> l(mtx_); return muted_; }

void PorteParoleCard::onPowerCycle()
{
    std::lock_guard<std::mutex> lock(mtx_);
    chip_.reset();
    pcm_.clear();
    clockRemainder_ = 0.0;
    samplePhase_ = 0;
    previousSample_ = 0;
    lastCycle_ = cpu_ ? boundaryCycle(cpu_) : lastCycle_;
}

void PorteParoleCard::onUnplug() { onPowerCycle(); }

void PorteParoleCard::onTimeJump()
{
    // The chip state rides in the snapshot; what was queued for the mixer
    // belongs to the timeline that was left.
    std::lock_guard<std::mutex> lock(mtx_);
    pcm_.clear();
    samplePhase_ = 0;
    previousSample_ = 0;
    if (cpu_) lastCycle_ = boundaryCycle(cpu_);
}

void PorteParoleCard::sync()
{
    if (!cpu_) return;
    // The access cycle: the bus counter plus the in-flight instruction, the
    // Echo+'s convention.
    syncAt(cpu_->getCycleCountNow() + 1);
}

void PorteParoleCard::syncAt(uint64_t now)
{
    if (now > lastCycle_) {
        advance(now - lastCycle_);
    } else if (lastCycle_ - now > 1024) {
        // Time ran backwards (a rewind the card was not told of): restart
        // the host resampler from here.
        pcm_.clear();
        samplePhase_ = 0;
        previousSample_ = 0;
    }
    lastCycle_ = now;
}

void PorteParoleCard::advanceCycles(int cycles)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (cycles <= 0) return;
    if (cpu_) {
        // Memory has advanced its counter while the CPU still carries the
        // instruction's cycles — the same correction as the Echo+.
        const uint64_t now = cpu_->getCycleCountNow();
        syncAt(now >= uint64_t(cycles) ? now - uint64_t(cycles) : 0);
    } else {
        advance(uint64_t(cycles));
        lastCycle_ += uint64_t(cycles);
    }
}

// Convert CPU cycles to quartz clocks once, independently of host audio rate.
void PorteParoleCard::advance(uint64_t cycles)
{
    // Keep the numerator remainder, not a repeatedly divided fraction of a
    // quartz clock. Integer CPU Hz then uses exact integer-valued doubles:
    // 65 PAL cycles always produce 256 clocks, even when advanced one by one.
    const double exact = clockRemainder_ + double(cycles) * kChipClockHz;
    const auto clocks = static_cast<uint64_t>(std::floor(exact / cpuHz_));
    clockRemainder_ = exact - double(clocks) * cpuHz_;
    // Bound temporary PCM storage even for large headless/turbo advances.
    uint64_t left = clocks;
    while (left) {
        const uint64_t count = std::min<uint64_t>(left, 40000);
        scratch_.clear();
        chip_.advanceClocks(count, &scratch_);
        for (int16_t sample : scratch_) pushSample(sample);
        left -= count;
    }
}

void PorteParoleCard::pushSample(int16_t level)
{
    samplePhase_ += rate_;
    while (samplePhase_ >= kNativeRate) {
        samplePhase_ -= kNativeRate;
        const double t = 1.0 - double(samplePhase_) / rate_;
        pcm_.push_back(float(previousSample_ + (level - previousSample_) * t) / 32768.0f);
        if (pcm_.size() > kMaxQueue) pcm_.pop_front();
    }
    previousSample_ = level;
}

uint8_t PorteParoleCard::deviceSelectRead(uint8_t)
{
    std::lock_guard<std::mutex> lock(mtx_);
    sync();
    // [B] D7 = REQ; D0-D6 float (header).
    return static_cast<uint8_t>((chip_.acceptsByte() ? 0x80 : 0x00) | (openBus() & 0x7F));
}

void PorteParoleCard::deviceSelectWrite(uint8_t low4, uint8_t value)
{
    std::lock_guard<std::mutex> lock(mtx_);
    sync();
    if (low4 & 1) {
        chip_.writeCommand(value);
    } else {
        chip_.writeData(value);
    }
}

Mea8000 PorteParoleCard::chipSnapshot() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return chip_;
}

std::size_t PorteParoleCard::queuedSamples() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return pcm_.size();
}

std::vector<float> PorteParoleCard::drainSamples(std::size_t count)
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<float> out;
    while (count-- && !pcm_.empty()) { out.push_back(pcm_.front()); pcm_.pop_front(); }
    return out;
}

// ─── Snapshot ────────────────────────────────────────────────────────────
// PPA v3: reference core (including its analogue histories and 48 kHz
// scheduler) plus the undivided quartz remainder and its CPU frequency. Host PCM is discarded on load.
// v1 used a different synthesis engine and cannot represent this state.

void PorteParoleCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    out.insert(out.end(), {'P', 'P', 'A', 3});
    chip_.appendSnapshot(out);
    uint64_t bits; std::memcpy(&bits, &clockRemainder_, 8);
    byteio::putU64(out, bits);
    std::memcpy(&bits, &cpuHz_, 8);
    byteio::putU64(out, bits);
}

void PorteParoleCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // A versioned prefix: bytes past it are a later version's tail (the
    // contract's P3b), so a longer blob loads its known part.
    if (!data || len < kSnapshotV2Bytes || data[0] != 'P' || data[1] != 'P' ||
        data[2] != 'A' || (data[3] != 2 && data[3] != 3)) return;
    const bool legacy = data[3] == 2;
    if (!legacy && len < kSnapshotBytes) return;
    Mea8000 chip;
    if (!chip.loadSnapshot(data + 4, Mea8000::kSnapshotBytes)) return;
    byteio::Reader r(data, len);
    r.pos = 4 + Mea8000::kSnapshotBytes;
    uint64_t bits = r.u64();
    double remainder; std::memcpy(&remainder, &bits, 8);
    double savedHz = 1.0; // v2 stored a fraction of one quartz clock
    if (!legacy) {
        bits = r.u64(); std::memcpy(&savedHz, &bits, 8);
        if (!std::isfinite(savedHz) || savedHz < 100000 || savedHz > 1000000000) return;
    }
    if (!std::isfinite(remainder) || remainder < 0 || remainder >= savedHz) return;
    std::lock_guard<std::mutex> lock(mtx_);
    chip_ = chip;
    clockRemainder_ = savedHz == cpuHz_ ? remainder : remainder * (cpuHz_ / savedHz);
    if (cpu_) lastCycle_ = boundaryCycle(cpu_);
    pcm_.clear();
    samplePhase_ = 0;
    previousSample_ = 0;
}

} // namespace pom2
