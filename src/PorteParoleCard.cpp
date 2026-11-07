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
// clock / 480 x 8: 66 667 output ticks a second at 4 MHz.
constexpr double kTickHz = PorteParoleCard::kChipClockHz * Mea8000::kSuperSampling /
                           Mea8000::kClockDivider;
constexpr std::size_t kMaxQueue = 8192;
constexpr std::size_t kSnapshotBytes = 4 + Mea8000::kSnapshotBytes + 8 + 4;
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
    : audio_(std::make_unique<AudioSrc>(*this)), slot_(slot) {}

PorteParoleCard::~PorteParoleCard() = default;

AudioSource* PorteParoleCard::audioSource() { return audio_.get(); }

void PorteParoleCard::setCpu(M6502* cpu)
{
    std::lock_guard<std::mutex> lock(mtx_);
    cpu_ = cpu;
    lastCycle_ = cpu ? cpu->getCycleCountNow() : 0;
    reqLowUntil_ = 0;
}

void PorteParoleCard::setCpuClock(double hz)
{
    std::lock_guard<std::mutex> lock(mtx_);
    // No sync() here, for the Echo+'s reason: the setters run between
    // instructions, after advanceCycles has brought the card to the boundary.
    if (std::isfinite(hz) && hz >= 100000 && hz <= 1000000000) cpuHz_ = hz;
}

void PorteParoleCard::setSampleRate(uint32_t hz)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (hz < 8000 || hz > 192000 || hz == rate_) return;
    rate_ = hz;
    samplePhase_ = 0.0;
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
    tickPhase_ = samplePhase_ = tickSum_ = 0.0;
    tickCount_ = 0;
    reqLowUntil_ = 0;
    lastCycle_ = cpu_ ? cpu_->getCycleCountNow() : lastCycle_;
}

void PorteParoleCard::onUnplug() { onPowerCycle(); }

void PorteParoleCard::onTimeJump()
{
    // The chip state rides in the snapshot; what was queued for the mixer
    // belongs to the timeline that was left.
    std::lock_guard<std::mutex> lock(mtx_);
    pcm_.clear();
    tickSum_ = 0.0;
    tickCount_ = 0;
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
        // the queue and the REQ window from here.
        pcm_.clear();
        reqLowUntil_ = 0;
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

// The chip's ticks are counted from CPU cycles ALONE, and the host samples
// are derived from the chip's ticks. Splitting one advance at host-sample
// boundaries (the Echo+'s shape) made the floating-point tick phase — and so
// the cycle a frame ended on — depend on the host sample rate's phase: two
// cards in the same machine state drifted apart (porte_parole_card #7).
void PorteParoleCard::advance(uint64_t cycles)
{
    const double t = tickPhase_ + double(cycles) * kTickHz / cpuHz_;
    const double whole = std::floor(t);
    tickPhase_ = t - whole;
    uint64_t ticks = static_cast<uint64_t>(whole);
    while (ticks > 0 && chip_.running()) {
        pushTick(chip_.tick());
        --ticks;
    }
    if (ticks > 0) {
        // Idle: only the chip's free-running 8 ms grid moves (Mea8000.h,
        // departure 7), and the host hears silence.
        chip_.idle(ticks);
        pushSilence(ticks);
    }
}

void PorteParoleCard::pushTick(int16_t level)
{
    tickSum_ += level;
    ++tickCount_;
    samplePhase_ += double(rate_) / kTickHz;
    if (samplePhase_ >= 1.0) {
        samplePhase_ -= 1.0;
        // Box-average the ~1.5 chip ticks of each host sample: the chip's
        // 64 kHz stream point-sampled at 44.1 kHz would fold its x8
        // interpolation images into the audible band.
        pcm_.push_back(float(tickSum_ / tickCount_) / 32768.0f);
        tickSum_ = 0.0; tickCount_ = 0;
        if (pcm_.size() > kMaxQueue) pcm_.pop_front();
    }
}

void PorteParoleCard::pushSilence(uint64_t ticks)
{
    const double s = samplePhase_ + double(ticks) * double(rate_) / kTickHz;
    auto whole = static_cast<uint64_t>(s);
    samplePhase_ = s - double(whole);
    whole = std::min<uint64_t>(whole, kMaxQueue);
    for (uint64_t i = 0; i < whole; ++i) {
        // The first sample after an utterance closes its last partial box.
        pcm_.push_back(tickCount_ ? float(tickSum_ / tickCount_) / 32768.0f : 0.0f);
        tickSum_ = 0.0; tickCount_ = 0;
    }
    while (pcm_.size() > kMaxQueue) pcm_.pop_front();
}

bool PorteParoleCard::reqVisible() const
{
    return chip_.acceptsByte() && lastCycle_ >= reqLowUntil_;
}

uint8_t PorteParoleCard::deviceSelectRead(uint8_t)
{
    std::lock_guard<std::mutex> lock(mtx_);
    sync();
    // [B] D7 = REQ; D0-D6 float (header).
    return static_cast<uint8_t>((reqVisible() ? 0x80 : 0x00) | (openBus() & 0x7F));
}

void PorteParoleCard::deviceSelectWrite(uint8_t low4, uint8_t value)
{
    std::lock_guard<std::mutex> lock(mtx_);
    sync();
    if (low4 & 1) {
        chip_.writeCommand(value);
    } else {
        chip_.writeData(value);
        reqLowUntil_ = lastCycle_ +
            uint64_t(std::ceil(kReqRecoverySeconds * cpuHz_));
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
// 'P' 'P' 'A' 1, the chip, the tick phase (the chip's place between two of
// its own ticks) and what is left of the REQ recovery window, in cycles.
// The host-rate queue and phase are host state and are not saved.

void PorteParoleCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    out.insert(out.end(), {'P', 'P', 'A', 1});
    chip_.appendSnapshot(out);
    uint64_t bits; std::memcpy(&bits, &tickPhase_, 8);
    byteio::putU64(out, bits);
    const uint64_t left = reqLowUntil_ > lastCycle_ ? reqLowUntil_ - lastCycle_ : 0;
    byteio::putU32(out, static_cast<uint32_t>(std::min<uint64_t>(left, 0xFFFFFFFFu)));
}

void PorteParoleCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // A versioned prefix: bytes past it are a later version's tail (the
    // contract's P3b), so a longer blob loads its known part.
    if (!data || len < kSnapshotBytes || data[0] != 'P' || data[1] != 'P' ||
        data[2] != 'A' || data[3] != 1)
        return;
    len = kSnapshotBytes;
    Mea8000 chip;
    if (!chip.loadSnapshot(data + 4, Mea8000::kSnapshotBytes)) return;
    byteio::Reader r(data, len);
    r.pos = 4 + Mea8000::kSnapshotBytes;
    const uint64_t bits = r.u64();
    double phase; std::memcpy(&phase, &bits, 8);
    const uint32_t reqLeft = r.u32();
    // 2 µs at the fastest CPU clock setCpuClock accepts is 2000 cycles.
    if (!std::isfinite(phase) || phase < 0.0 || phase >= 1.0 || reqLeft > 2000) return;
    std::lock_guard<std::mutex> lock(mtx_);
    chip_ = chip;
    tickPhase_ = phase;
    if (cpu_) lastCycle_ = cpu_->getCycleCountNow();
    reqLowUntil_ = lastCycle_ + reqLeft;
    pcm_.clear();
    tickSum_ = 0.0;
    tickCount_ = 0;
}

} // namespace pom2
