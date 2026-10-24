// POM2 — GPL-3.0-or-later
#include "IIcMouse.h"
#include <algorithm>
#include <cstring>

void IIcMouse::setHostMouse(uint8_t x, uint8_t y, bool button)
{
    // Caller holds the machine lock. Host counters are relative inputs,
    // never guest coordinates: the unmodified ROM integrates/clamps them.
    auto delta = [](int v) { return v > 128 ? v-256 : v < -128 ? v+256 : v; };
    // TWO queued steps per commanded cursor unit. `step()` spends one queued
    // unit per X0/Y0 TRANSITION, and the ROM's IRQ handler advances the screen
    // hole once per complete PERIOD — two transitions. Queuing one per unit
    // therefore moved the guest cursor exactly half as far as the host asked,
    // so the same drag that crossed the screen on a //e with `mouseaw` reached
    // the middle on a //c. MAME scales this at its input port instead; POM2's
    // host seam is here, and this is the only place that knows a host unit is
    // meant to be one cursor unit.
    countX_ = std::clamp(countX_ + 2 * delta(int(x)-hostX_), -32768, 32767);
    countY_ = std::clamp(countY_ + 2 * delta(int(y)-hostY_), -32768, 32767);
    hostX_ = x; hostY_ = y; button_ = button;
}

void IIcMouse::onReset()
{
    enabled_ = xEdge_ = yEdge_ = false;
    x0_ = y0_ = x1_ = y1_ = xIrq_ = yIrq_ = false;
    countX_ = countY_ = phase_ = 0;
    releasedFor_ = 0;
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
    //
    // Back-pressure: while an axis' interrupt latch is still pending, that
    // axis holds its next transition. One transition per scanline (65
    // cycles) is far faster than the ROM's IRQ handler, so a fast host move
    // used to produce edges the firmware never saw — A2DeskTop received ~42%
    // of a full-screen drag and the cursor could not reach the screen edges
    // even with the pointer captured. A real mouse never outruns the handler;
    // holding the queue until $C048 clears the latch keeps every unit.
    // Only in IRQ mode: with the interrupt off nothing latches, and software
    // polling X0/X1 directly keeps the plain per-scanline pace.
    // Deliberate divergence from MAME, whose update_iic_mouse emits a
    // transition every scanline unconditionally (and loses the same edges):
    // the host pointer can move faster than any physical mouse, so the
    // shaping belongs on this side of the quadrature (GitHub issue #17).
    //
    // The hold ends if the line was released with the latch still set and
    // nobody clears it — a guest that reads $C015/$C017 with interrupts
    // masked (READMOUSE under SEI) acknowledges the line without $C048, the
    // ROM's handler never runs, and a held queue then froze the mouse for
    // good. A real mouse keeps moving: the next edge raises the line again.
    // A handler goes from $C015 to $C048 in well under a scanline, so a few
    // scanlines of a released line mean no handler is coming.
    const bool latched = xIrq_ || yIrq_;
    releasedFor_ = (latched && !slotIrqAsserted()) ? releasedFor_ + 1 : 0;
    const bool stale = releasedFor_ >= kStaleLatchLines;
    bool irq = false;
    if (countX_ && !(enabled_ && xIrq_ && !stale)) {
        x1_ = countX_ > 0;
        countX_ += countX_ > 0 ? -1 : 1;
        if (enabled_ && x0_ == xEdge_) { xIrq_ = true; irq = true; }
        x0_ = !x0_;
    }
    if (countY_ && !(enabled_ && yIrq_ && !stale)) {
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

bool IIcMouse::validateIicMouseState(const uint8_t* p, size_t n) const
{
    if (!p || n != 19 || std::memcmp(p, "IOU1", 4) || p[14] >= 65) return false;
    for (int i=4; i<14; ++i) if (p[i] > 1) return false;
    return true;
}

bool IIcMouse::loadIicMouseState(const uint8_t* p, size_t n)
{
    if (!validateIicMouseState(p, n)) return false;
    enabled_=p[4]; xEdge_=p[5]; yEdge_=p[6]; x0_=p[7]; y0_=p[8];
    x1_=p[9]; y1_=p[10]; xIrq_=p[11]; yIrq_=p[12]; phase_=p[14];
    auto signed16 = [](int v) { return v >= 32768 ? v-65536 : v; };
    countX_=signed16(p[15] | (p[16]<<8)); countY_=signed16(p[17] | (p[18]<<8));
    assertIrq(p[13]);
    // Current host position/button stay live across rewind. Only queued
    // emulated edges and chip state travel back in time.
    return true;
}
