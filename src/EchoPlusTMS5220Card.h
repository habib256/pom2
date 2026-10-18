// POM2 — GPL-3.0-or-later
// Street Electronics Echo+: MAME a2mockingboard.cpp:210-219,307-310,
// 360-363,453-474,585-607. One VIA drives two AY8913s; TSP5220C at C0n0.
#pragma once
#include "SlotPeripheral.h"
#include "AudioSource.h"
#include "Ay3_8910.h"
#include "AyPsgSynth.h"
#include "Via6522.h"
#include "Tms5220.h"
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
class M6502;
class EchoPlusTMS5220Card : public SlotPeripheral {
public:
    static constexpr int kDefaultSlot = 2;
    explicit EchoPlusTMS5220Card(int slot = kDefaultSlot);
    ~EchoPlusTMS5220Card() override;
    int getSlot() const { return slot_; }
    std::string_view name() const override { return "Echo+ (TMS5220 + 2×AY)"; }
    uint8_t slotRomRead(uint8_t offset) override;
    void slotRomWrite(uint8_t offset, uint8_t value) override;
    uint8_t deviceSelectRead(uint8_t offset) override;
    void deviceSelectWrite(uint8_t offset, uint8_t value) override;
    AudioSource* audioSource() override;
    void setCpu(M6502* cpu);
    void setCpuClock(double hz) override;
    void setStandardClock(double hz) override;
    void advanceCycles(int cycles) override;
    void onReset() override;
    void onUnplug() override;
    void setSampleRate(uint32_t hz);
    void setVolume(float value);
    float getVolume() const;
    void setMuted(bool value);
    bool isMuted() const;
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;
    struct Snap { uint8_t tmsStatus, tmsLastWrite, ayRegs[2][16]; };
    Snap snapshot() const;
private:
    struct AudioSrc;
    std::unique_ptr<AudioSrc> audio_;
    int slot_;
    mutable std::mutex mtx_;
    M6502* cpu_ = nullptr;
    uint64_t lastCycle_ = 0;
    pom2::Tms5220 speech_;
    pom2::Via6522 via_;
    pom2::Ay3_8910 ay_[2];
    pom2::ay::ChipSynthState synth_[2];
    std::deque<std::array<float, 2>> pcm_;
    double cpuHz_ = 1022727, busHz_ = 1022727;
    bool busClockSet_ = false;
    double viaPhase_ = 0, speechPhase_ = 0, samplePhase_ = 0;
    uint32_t rate_ = kAudioSampleRate;
    int16_t lastSpeech_ = -1;
    uint8_t lastWrite_ = 0;
    float volume_ = 0.7f;
    bool muted_ = false;
    void sync();
    void syncAt(uint64_t now);
    void advance(uint64_t cycles);
    void applyPsgControl();
};
