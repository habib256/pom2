// POM2 — GPL-3.0-or-later
#ifndef POM2_SNES_MAX_CARD_H
#define POM2_SNES_MAX_CARD_H
#include "SlotPeripheral.h"
#include "CpuClock.h"
#include <array>
#include <atomic>
#include <cstdint>
namespace pom2 {
// MAME bus/a2bus/snesmax.cpp: read_c0nx, write_c0nx, latch callbacks;
// bus/snes_ctrl/joypad.cpp:84-119 supplies the serial button protocol.
// Reference implementations: BSD-3-Clause, Vas Crabb and Fabio Priuli.
// Host state is live input; latched data, shifters and the seven-clock pulse
// are guest-visible state and therefore belong in rewind snapshots.
class SnesMaxCard final : public SlotPeripheral {
public:
    explicit SnesMaxCard(int slot) : slot_(slot) {
        for (auto& pad : buttons_) pad.store(0, std::memory_order_relaxed);
    }
    std::string_view name() const override { return "SNES MAX"; }
    int getSlot() const { return slot_; }
    enum Button : uint16_t {
        B = 1, Y = 2, Select = 4, Start = 8,
        Up = 16, Down = 32, Left = 64, Right = 128,
        A = 256, X = 512, L = 1024, R = 2048
    };
    void setButtons(int player, uint16_t pressed);
    uint8_t deviceSelectRead(uint8_t) override { return data_; }
    void deviceSelectWrite(uint8_t offset, uint8_t) override;
    void advanceCycles(int cycles) override;
    void setCpuClock(double hz) override;
    void setStandardClock(double hz) override;
    void onPowerCycle() override;
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t size) override;
private:
    void clock();
    int slot_;
    std::array<std::atomic<uint16_t>, 2> buttons_{};
    std::array<uint32_t, 2> shift_{};
    uint8_t data_ = 0xFF;
    double latchRemaining_ = 0.0; // slot-bus ticks, independent of acceleration
    double cpuHz_ = POM2_CPU_CLOCK_HZ;
    double busHz_ = POM2_CPU_CLOCK_HZ;
};
}
#endif
