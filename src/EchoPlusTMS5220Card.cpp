// POM2 — GPL-3.0-or-later. Hardware map: MAME a2mockingboard.cpp:453-474,585-607.
#include "EchoPlusTMS5220Card.h"
#include "M6502.h"
#include "ByteIO.h"
#include <algorithm>
#include <cmath>
#include <cstring>

struct EchoPlusTMS5220Card::AudioSrc : AudioSource, RateAware {
    EchoPlusTMS5220Card& card;
    explicit AudioSrc(EchoPlusTMS5220Card& c) : card(c) {}
    void setSampleRate(uint32_t hz) override { card.setSampleRate(hz); }
    void fillAudioBuffer(float* out, int count) override {
        std::lock_guard<std::mutex> lock(card.mtx_);
        for (int i = 0; i < count; ++i) {
            const float sample = card.pcm_.empty() ? 0 : (card.pcm_.front()[0] + card.pcm_.front()[1]) * 0.5f;
            if (!card.pcm_.empty()) card.pcm_.pop_front();
            out[i] = card.muted_ ? 0 : sample * card.volume_;
        }
    }
    bool fillAudioBufferStereo(float* left, float* right, int count) override {
        std::lock_guard<std::mutex> lock(card.mtx_);
        for (int i = 0; i < count; ++i) {
            const auto sample = card.pcm_.empty() ? std::array<float, 2>{} : card.pcm_.front();
            if (!card.pcm_.empty()) card.pcm_.pop_front();
            left[i] = card.muted_ ? 0 : sample[0] * card.volume_;
            right[i] = card.muted_ ? 0 : sample[1] * card.volume_;
        }
        return true;
    }
};
EchoPlusTMS5220Card::EchoPlusTMS5220Card(int slot) : audio_(std::make_unique<AudioSrc>(*this)), slot_(slot) { onReset(); }
EchoPlusTMS5220Card::~EchoPlusTMS5220Card() = default;
AudioSource* EchoPlusTMS5220Card::audioSource() { return audio_.get(); }
void EchoPlusTMS5220Card::onReset() {
    std::lock_guard<std::mutex> lock(mtx_);
    speech_.reset(); via_.reset();
    for (int i = 0; i < 2; ++i) { ay_[i].reset(); synth_[i] = {}; }
    pcm_.clear(); viaPhase_ = speechPhase_ = samplePhase_ = 0;
    lastSpeech_ = -1; lastWrite_ = 0;
    lastCycle_ = cpu_ ? cpu_->getCycleCountNow() : 0;
    assertIrq(false);
}
void EchoPlusTMS5220Card::onUnplug() { onReset(); }
void EchoPlusTMS5220Card::setCpu(M6502* cpu) {
    std::lock_guard<std::mutex> lock(mtx_);
    cpu_ = cpu; lastCycle_ = cpu ? cpu->getCycleCountNow() : 0;
}
void EchoPlusTMS5220Card::setCpuClock(double hz) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (std::isfinite(hz) && hz >= 100000 && hz <= 1000000000) {
        sync(); cpuHz_ = hz; if (!busClockSet_) busHz_ = hz;
    }
}
void EchoPlusTMS5220Card::setStandardClock(double hz) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (std::isfinite(hz) && hz >= 100000 && hz <= 10000000) {
        sync(); busHz_ = hz; busClockSet_ = true;
    }
}
void EchoPlusTMS5220Card::setSampleRate(uint32_t hz) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (hz < 8000 || hz > 192000 || hz == rate_) return;
    rate_ = hz; samplePhase_ = 0; pcm_.clear();
}
void EchoPlusTMS5220Card::setVolume(float v) { std::lock_guard<std::mutex> l(mtx_); volume_ = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0; }
float EchoPlusTMS5220Card::getVolume() const { std::lock_guard<std::mutex> l(mtx_); return volume_; }
void EchoPlusTMS5220Card::setMuted(bool v) { std::lock_guard<std::mutex> l(mtx_); muted_ = v; }
bool EchoPlusTMS5220Card::isMuted() const { std::lock_guard<std::mutex> l(mtx_); return muted_; }
void EchoPlusTMS5220Card::sync() {
    if (!cpu_) return;
    syncAt(cpu_->getCycleCountNow() + 1);
}
void EchoPlusTMS5220Card::syncAt(uint64_t now) {
    if (now > lastCycle_) advance(now - lastCycle_);
    else if (lastCycle_ - now > 1024) pcm_.clear();
    lastCycle_ = now;
}
void EchoPlusTMS5220Card::advanceCycles(int cycles) {
    std::lock_guard<std::mutex> lock(mtx_);
    // Memory has advanced its counter, while M6502 still carries the
    // instruction's cycles. Apply the same correction as Mockingboard.
    if (cpu_ && cycles > 0) {
        const uint64_t now = cpu_->getCycleCountNow();
        syncAt(now >= uint64_t(cycles) ? now - uint64_t(cycles) : 0);
    }
    else if (cycles > 0) { advance(uint64_t(cycles)); lastCycle_ += uint64_t(cycles); }
}
void EchoPlusTMS5220Card::advance(uint64_t cycles) {
    const double ticks = double(cycles) * busHz_ / cpuHz_ + viaPhase_;
    const auto whole = uint64_t(ticks);
    viaPhase_ = ticks - double(whole);
    for (uint64_t left = whole; left;) {
        const int batch = int(std::min<uint64_t>(left, 1000000));
        via_.advance(batch); left -= uint64_t(batch);
    }
    assertIrq(via_.irqOut());
    double left = double(cycles);
    while (left > 0) {
        const double delta = std::min({left, (1 - speechPhase_) * cpuHz_ / 8000,
                                            (1 - samplePhase_) * cpuHz_ / rate_});
        speechPhase_ += delta * 8000 / cpuHz_;
        samplePhase_ += delta * rate_ / cpuHz_;
        left -= delta;
        if (speechPhase_ >= 1 - 1e-10) {
            speechPhase_ = std::max(0.0, speechPhase_ - 1);
            speech_.process(&lastSpeech_, 1);
        }
        if (samplePhase_ >= 1 - 1e-10) {
            samplePhase_ = std::max(0.0, samplePhase_ - 1);
            const float step = float(busHz_ / (8 * rate_));
            const float psgLeft = pom2::ay::renderChipSample(synth_[0], ay_[0].regs, step, 1 / step);
            const float psgRight = pom2::ay::renderChipSample(synth_[1], ay_[1].regs, step, 1 / step);
            const float speech = lastSpeech_ == -1 ? 0 : float(lastSpeech_) / 32768;
            pcm_.push_back({std::clamp(speech * 0.7f + psgLeft / 6, -1.0f, 1.0f),
                            std::clamp(speech * 0.7f + psgRight / 6, -1.0f, 1.0f)});
            if (pcm_.size() > 8192) pcm_.pop_front();
        }
    }
}
uint8_t EchoPlusTMS5220Card::deviceSelectRead(uint8_t offset) {
    std::lock_guard<std::mutex> lock(mtx_); sync();
    return offset == 0 ? uint8_t(0x1F | speech_.status()) : 0;
}
void EchoPlusTMS5220Card::deviceSelectWrite(uint8_t offset, uint8_t value) {
    std::lock_guard<std::mutex> lock(mtx_); sync();
    if (!offset) { lastWrite_ = value; speech_.write(value); }
}
uint8_t EchoPlusTMS5220Card::slotRomRead(uint8_t offset) {
    std::lock_guard<std::mutex> lock(mtx_); sync();
    const auto result = via_.read(offset & 15); assertIrq(via_.irqOut()); return result;
}
void EchoPlusTMS5220Card::slotRomWrite(uint8_t offset, uint8_t value) {
    std::lock_guard<std::mutex> lock(mtx_); sync();
    if (via_.write(offset & 15, value) & 3) applyPsgControl();
    assertIrq(via_.irqOut());
}
void EchoPlusTMS5220Card::applyPsgControl() {
    const auto pb = via_.readPortB(), pa = via_.readPortA();
    for (int i = 0; i < 2; ++i) {
        if ((pb & 4) && !(pb & (8 << i))) continue;
        const auto result = ay_[i].applyControl(pa, pb);
        if (result == pom2::Ay3_8910::ResetOnly) synth_[i] = {};
        if (result == pom2::Ay3_8910::Wrote && ay_[i].latchedAddr == 13) synth_[i].envRetrigger = true;
        if (result == pom2::Ay3_8910::Read) via_.setPortAInput(ay_[i].busOut);
    }
}
EchoPlusTMS5220Card::Snap EchoPlusTMS5220Card::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto copy = speech_;
    Snap result{}; result.tmsStatus = copy.status(); result.tmsLastWrite = lastWrite_;
    for (int i = 0; i < 2; ++i) std::memcpy(result.ayRegs[i], ay_[i].regs, 16);
    return result;
}
