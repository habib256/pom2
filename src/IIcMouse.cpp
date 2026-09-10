// POM2 — GPL-3.0-or-later
#include "IIcMouse.h"
#include <algorithm>
#include <cstring>

void IIcMouse::setHostMouse(uint8_t x, uint8_t y, bool button)
{
    // Caller holds the machine lock. Host counters are relative inputs,
    // never guest coordinates: the unmodified ROM integrates/clamps them.
    auto delta = [](int v) { return v > 128 ? v-256 : v < -128 ? v+256 : v; };
    countX_ = std::clamp(countX_ + delta(int(x)-hostX_), -32768, 32767);
    countY_ = std::clamp(countY_ + delta(int(y)-hostY_), -32768, 32767);
    hostX_ = x; hostY_ = y; button_ = button;
}

void IIcMouse::onReset()
{
    enabled_ = xEdge_ = yEdge_ = false;
    x0_ = y0_ = x1_ = y1_ = xIrq_ = yIrq_ = false;
    countX_ = countY_ = phase_ = 0;
    assertIrq(false);
}

bool IIcMouse::iicMouseAccess(uint8_t low, bool write, bool ioudis,
                             uint8_t bus, uint8_t& out)
{
    // MAME apple2e.cpp c000_iic_r (:2290-2345 in the reference copy),
    // do_io (:1835-1885, :1958): the axis latches survive IRQ acknowledge;
    // $C048 clears both latches. $C015/$C017 release the shared IRQ only.
    out = bus;
    if (low == 0x48) { xIrq_ = yIrq_ = false; assertIrq(false); return true; }
    if (low >= 0x58 && low <= 0x5F && !ioudis) {
        switch (low) {
        case 0x58: enabled_ = false; break;
        case 0x59: enabled_ = true; break;
        case 0x5C: xEdge_ = false; break;
        case 0x5D: xEdge_ = true; break;
        case 0x5E: yEdge_ = false; break;
        case 0x5F: yEdge_ = true; break;
        default: return false; // Memory owns VBL mask/ack
        }
        return true;
    }
    if (write) return false;
    bool bit;
    switch (low) {
    case 0x15: bit = xIrq_; assertIrq(false); break;
    case 0x17: bit = yIrq_; assertIrq(false); break;
    case 0x40: bit = enabled_; break;
    case 0x42: bit = xEdge_; break;
    case 0x43: bit = yEdge_; break;
    case 0x63: case 0x6B: bit = !button_; break;
    case 0x66: case 0x6E: bit = x1_; break;
    case 0x67: case 0x6F: bit = y1_; break;
    default: return false;
    }
    out = (bus & 0x7F) | (bit ? 0x80 : 0);
    return true;
}

void IIcMouse::advanceCycles(int cycles)
{
    // MAME apple2e.cpp apple2_interrupt (:1375-1383) clocks the IOU
    // input model once per scanline, 65 CPU cycles, NTSC and PAL alike.
    if (cycles <= 0) return;
    const uint64_t total = uint64_t(phase_) + unsigned(cycles);
    auto ticks = total / 65;
    phase_ = int(total % 65);
    while (ticks-- && (countX_ || countY_)) step();
}

void IIcMouse::step()
{
    // MAME apple2e.cpp update_iic_mouse (:2700-2810). Each pending step
    // toggles X0/Y0. X1/Y1 encode direction with opposite Y polarity;
    // only the selected X0/Y0 edge raises the shared mouse interrupt.
    bool irq = false;
    if (countX_) {
        x1_ = countX_ > 0;
        countX_ += countX_ > 0 ? -1 : 1;
        if (enabled_ && x0_ == xEdge_) { xIrq_ = true; irq = true; }
        x0_ = !x0_;
    }
    if (countY_) {
        y1_ = countY_ < 0;
        countY_ += countY_ > 0 ? -1 : 1;
        if (enabled_ && y0_ == yEdge_) { yIrq_ = true; irq = true; }
        y0_ = !y0_;
    }
    if (irq) assertIrq(true);
}

void IIcMouse::appendSnapshotState(std::vector<uint8_t>& out) const
{
    const uint8_t flags[] = {'I','O','U','1', uint8_t(enabled_), uint8_t(xEdge_),
        uint8_t(yEdge_), uint8_t(x0_), uint8_t(y0_), uint8_t(x1_), uint8_t(y1_),
        uint8_t(xIrq_), uint8_t(yIrq_), uint8_t(slotIrqAsserted()), uint8_t(phase_)};
    out.insert(out.end(), std::begin(flags), std::end(flags));
    for (int v : {countX_, countY_}) {
        out.push_back(uint8_t(v)); out.push_back(uint8_t(uint16_t(v) >> 8));
    }
}

void IIcMouse::loadSnapshotState(const uint8_t* p, size_t n)
{
    (void)loadIicMouseState(p, n);
}

bool IIcMouse::loadIicMouseState(const uint8_t* p, size_t n)
{
    if (!p || n != 19 || std::memcmp(p, "IOU1", 4) || p[14] >= 65) return false;
    for (int i=4; i<14; ++i) if (p[i] > 1) return false;
    enabled_=p[4]; xEdge_=p[5]; yEdge_=p[6]; x0_=p[7]; y0_=p[8];
    x1_=p[9]; y1_=p[10]; xIrq_=p[11]; yIrq_=p[12]; phase_=p[14];
    auto signed16 = [](int v) { return v >= 32768 ? v-65536 : v; };
    countX_=signed16(p[15] | (p[16]<<8)); countY_=signed16(p[17] | (p[18]<<8));
    assertIrq(p[13]);
    // Current host position/button stay live across rewind. Only queued
    // emulated edges and chip state travel back in time.
    return true;
}
