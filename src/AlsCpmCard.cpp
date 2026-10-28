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

// AlsCpmCard implementation — see the header for where each rule comes from.

#include "AlsCpmCard.h"

#include "M6502.h"
#include "Memory.h"
#include "SoftCardZ80.h"

#include <cmath>
#include <cstring>

namespace {
// Snapshot blob: "ALS1", flags (bit0 enabled, bit1 firstBoot, bit2 bank,
// bit3 tick pending), the Z80 register file in SoftCardZ80's layout, the
// next tick's cycle (8 bytes), then the card's 64 KB.
constexpr char kMagic[4] = { 'A', 'L', 'S', '1' };
constexpr double kStockHz = 1'022'727.0;
} // namespace

AlsCpmCard::AlsCpmCard() : z80_(*this)
{
    z80_.reset();
}

// Bank 0 = the SoftCard's window with Z80 A15 inverted (header).
uint16_t AlsCpmCard::xlateBank0(uint16_t a)
{
    return SoftCardZ80::xlate(uint16_t(a ^ 0x8000));
}

void AlsCpmCard::slotRomWrite(uint8_t /*low8*/, uint8_t /*v*/)
{
    if (!enabled_) {
        if (firstZ80Boot_) {
            firstZ80Boot_ = false;
            z80_.reset();
        }
        enabled_ = true;
        if (cpu_)
            cpu_->stop();   // end the 6502's chunk after this STA
    } else {
        enabled_ = false;
    }
}

void AlsCpmCard::onReset()
{
    z80_.reset();
    z80_.setIrqLine(false);
    enabled_ = false;
    firstZ80Boot_ = true;
    bank_ = 0;
    tickPending_ = false;
    nextTick_ = 0;
    busPos_ = 0.0;
    busCarry_ = 0.0;
}

uint8_t AlsCpmCard::z80MemRead(uint16_t addr)
{
    if (!enabled_ || !mem_)
        return 0xFF;
    if (appleWindow(addr)) {
        appleAccess();
        return mem_->memRead(xlateBank0(addr));
    }
    return ram_[addr];
}

void AlsCpmCard::z80MemWrite(uint16_t addr, uint8_t v)
{
    if (!enabled_ || !mem_)
        return;
    if (appleWindow(addr)) {
        appleAccess();
        mem_->memWrite(xlateBank0(addr), v);   // $6n00 here is the toggle
        return;
    }
    ram_[addr] = v;
}

void AlsCpmCard::z80IoWrite(uint16_t /*port*/, uint8_t v)
{
    bank_ = v & 1;
}

// An access to the Apple: the Z80 has reached `pos` bus cycles into this
// instruction (counted from the cycle Memory's counter stands at); the DMA
// cycle starts on the next edge and lasts one bus cycle, against the 3
// T-states the access would have taken on the card. The 6502's in-flight
// count is set to that edge, so devices stamp the access where it lands
// (SoftCardZ80::stampAccess, at this card's clock).
double AlsCpmCard::appleAccess()
{
    const double pos = busPos_ + z80_.busT() * busPerT_ + syncExtra_;
    const double edge = std::ceil(pos);
    syncExtra_ += (edge - pos) + 1.0 - 3.0 * busPerT_;
    if (cpu_) cpu_->setInFlightCycles(static_cast<int>(edge * ratio_));
    return edge;
}

// 60 Hz from the emulated clock. While the 6502 has the bus the Z80 cannot
// take the interrupt, so ticks then collapse into one pending request.
void AlsCpmCard::clockTicks()
{
    if (!mem_) return;
    const double hz = cpuHz_ > 0.0 ? cpuHz_ : (stockHz_ > 0.0 ? stockHz_ : kStockHz);
    const uint64_t period = static_cast<uint64_t>(hz / kTickHz);
    const uint64_t now = mem_->getCycleCounter();
    if (nextTick_ == 0) nextTick_ = now + period;
    if (now >= nextTick_) {
        nextTick_ = now + period;
        tickPending_ = true;
        z80_.setIrqLine(true, kRst30);
    }
}

int AlsCpmCard::dmaRun(int cycles6502)
{
    if (cpu_) cpu_->clearInFlightCycles();
    const double stock = stockHz_ > 0.0 ? stockHz_ : kStockHz;
    ratio_ = (cpuHz_ > 0.0 && stockHz_ > 0.0 && cpuHz_ > stockHz_)
        ? cpuHz_ / stockHz_ : 1.0;
    busPerT_ = stock / kZ80Hz;
    int spent = 0;
    while (spent < cycles6502 && enabled_) {
        clockTicks();
        const bool couldTake = z80_.getState().iff1;
        syncExtra_ = 0.0;
        const int t = z80_.step();
        // The acknowledge cycle: interrupts were on and the Z80 is now at
        // the RST 30 vector with them off.
        if (tickPending_ && couldTake && !z80_.getState().iff1
            && z80_.getState().pc == (kRst30 & 0x38)) {
            tickPending_ = false;
            z80_.setIrqLine(false);
        }
        const double bus = busPos_ + t * busPerT_ + syncExtra_;
        int whole = static_cast<int>(bus);
        busPos_ = bus - whole;
        if (ratio_ != 1.0) {
            const double a = whole * ratio_ + busCarry_;
            whole = static_cast<int>(a);
            busCarry_ = a - whole;
        }
        if (cpu_) cpu_->clearInFlightCycles();
        if (whole > 0) {
            if (mem_) mem_->advanceCycles(whole);
            spent += whole;
        }
    }
    return spent;
}

void AlsCpmCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    const pom2::Z80::State& s = z80_.getState();
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(uint8_t((enabled_ ? 1 : 0) | (firstZ80Boot_ ? 2 : 0)
                          | (bank_ ? 4 : 0) | (tickPending_ ? 8 : 0)));
    const uint8_t regs8[] = {
        s.a, s.f, s.b, s.c, s.d, s.e, s.h, s.l,
        s.a2, s.f2, s.b2, s.c2, s.d2, s.e2, s.h2, s.l2,
        s.i, s.r, s.im,
        uint8_t((s.iff1 ? 1 : 0) | (s.iff2 ? 2 : 0) | (s.halted ? 4 : 0)
                | (s.afterEi ? 8 : 0) | (s.nmiPending ? 16 : 0)
                | (s.irqLine ? 32 : 0)),
        s.irqData, s.pendingPrefix,
    };
    out.insert(out.end(), regs8, regs8 + sizeof(regs8));
    for (uint16_t w : { s.ix, s.iy, s.sp, s.pc, s.wz }) {
        out.push_back(uint8_t(w));
        out.push_back(uint8_t(w >> 8));
    }
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(nextTick_ >> (8 * i)));
    out.insert(out.end(), ram_.begin(), ram_.end());
}

void AlsCpmCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    constexpr std::size_t kLen = 4 + 1 + 22 + 10 + 8 + 0x10000;
    if (len < kLen || std::memcmp(data, kMagic, 4) != 0)
        return;
    const uint8_t* p = data + 4;
    const uint8_t flags = *p++;
    enabled_      = (flags & 1) != 0;
    firstZ80Boot_ = (flags & 2) != 0;
    bank_         = (flags & 4) ? 1 : 0;
    tickPending_  = (flags & 8) != 0;

    pom2::Z80::State s;
    s.a = p[0];  s.f = p[1];  s.b = p[2];  s.c = p[3];
    s.d = p[4];  s.e = p[5];  s.h = p[6];  s.l = p[7];
    s.a2 = p[8]; s.f2 = p[9]; s.b2 = p[10]; s.c2 = p[11];
    s.d2 = p[12]; s.e2 = p[13]; s.h2 = p[14]; s.l2 = p[15];
    s.i = p[16]; s.r = p[17]; s.im = p[18];
    const uint8_t misc = p[19];
    s.iff1       = (misc & 1) != 0;
    s.iff2       = (misc & 2) != 0;
    s.halted     = (misc & 4) != 0;
    s.afterEi    = (misc & 8) != 0;
    s.nmiPending = (misc & 16) != 0;
    s.irqLine    = (misc & 32) != 0;
    s.irqData = p[20];
    s.pendingPrefix = p[21];
    p += 22;
    uint16_t* words[] = { &s.ix, &s.iy, &s.sp, &s.pc, &s.wz };
    for (uint16_t* w : words) {
        *w = uint16_t(p[0] | (p[1] << 8));
        p += 2;
    }
    z80_.setState(s);
    nextTick_ = 0;
    for (int i = 0; i < 8; ++i) nextTick_ |= uint64_t(p[i]) << (8 * i);
    p += 8;
    std::memcpy(ram_.data(), p, ram_.size());
}
