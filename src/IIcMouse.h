// POM2 — GPL-3.0-or-later
#pragma once
#include "SlotPeripheral.h"

// Motherboard IOU mouse circuit. Slot 4 is only its lifecycle/IRQ/snapshot
// owner in POM2; this device supplies NO slot ROM and NO card PIA/MCU.
class IIcMouse final : public SlotPeripheral {
public:
    explicit IIcMouse(int firmwareSlot = 4) : firmwareSlot_(firmwareSlot) {}
    int getSlot() const { return firmwareSlot_; }
    std::string_view name() const override { return "Apple //c IOU mouse"; }
    uint8_t deviceSelectRead(uint8_t) override { return openBus(); }
    void setHostMouse(uint8_t x, uint8_t y, bool button);
    bool iicMouseAccess(uint8_t low, bool write, bool ioudis,
                        uint8_t bus, uint8_t& out) override;
    void advanceCycles(int cycles) override;
    void onReset() override;
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, size_t n) override;
    void saveIicMouseState(std::vector<uint8_t>& out) const override { appendSnapshotState(out); }
    bool loadIicMouseState(const uint8_t* data, size_t n) override;
    bool hostDrained() const { return countX_ == 0 && countY_ == 0; }
private:
    int firmwareSlot_ = 4; // //c+ moves the firmware entry table/screen holes to port 7
    void step();
    bool enabled_ = false, xEdge_ = false, yEdge_ = false;
    bool x0_ = false, y0_ = false, x1_ = false, y1_ = false;
    bool xIrq_ = false, yIrq_ = false;
    bool button_ = false;
    uint8_t hostX_ = 0, hostY_ = 0;
    int countX_ = 0, countY_ = 0, phase_ = 0;
};
