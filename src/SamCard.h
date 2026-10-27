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

// SamCard — Don't Ask Software's S.A.M. ("Software Automatic Mouth") card.
// Port of MAME `src/devices/bus/a2bus/a2sam.cpp` (R. Belmont, BSD-3-Clause).
//
// The whole card, per MAME: ANY write to its $C0nX window drives an 8-bit
// DAC (`write_c0nx(...) { m_dac->write(data); }`, a2sam.cpp:44), routed to
// its own speaker at 0.5 (a2sam.cpp:55-56, `DAC_8BIT_R2R`, "unknown DAC").
// No ROM, no reads (MAME defines no `read_c0nx`, so the bus sees the
// default — POM2's open bus), nothing in $Cn00 or $C800. The speech itself
// is software: S.A.M. computes the waveform on the 6502 and streams samples
// to the port, so the card's fidelity is entirely the write timing — every
// write is stamped with the exact CPU cycle it lands on (bus counter + the
// in-flight instruction's cycle) and reconstructed by `Dac8Audio`.
//
// The DAC level is not saved in snapshots on purpose: it is audio-only
// output with no state a program can read back, and the next sample the
// software writes restores it.

#ifndef POM2_SAM_CARD_H
#define POM2_SAM_CARD_H

#include "Dac8Audio.h"
#include "SlotPeripheral.h"

class M6502;

namespace pom2 {

class SamCard : public SlotPeripheral
{
public:
    explicit SamCard(int slot);

    std::string_view name() const override { return "S.A.M. (8-bit DAC)"; }

    /// For the sub-instruction cycle of a write. Optional: without it the
    /// stamp is the bus counter at the start of the instruction.
    void setCpu(M6502* cpu) { cpu_ = cpu; }

    void deviceSelectWrite(uint8_t low4, uint8_t v) override;
    /// Writes are stamped on the STOCK timeline (cycles of the unaccelerated
    /// clock), continuous across clock changes, and the DAC replays at the
    /// stock rate. Handing the DAC the accelerated clock re-timed writes
    /// already queued: a Zip dropping its 52 ms slot slow-down after the
    /// last write played the tail of every utterance ~4x fast.
    void setCpuClock(double hz) override;
    void setStandardClock(double hz) override;
    void onReset() override {}
    void onTimeJump() override { dac_.reset(); }

    AudioSource* audioSource() override { return &dac_; }
    Dac8Audio&   dac() { return dac_; }

    void  setSampleRate(uint32_t hz) { dac_.setSampleRate(hz); }
    void  setVolume(float v) { dac_.setVolume(v); }
    float getVolume() const  { return dac_.getVolume(); }
    void  setMuted(bool m)   { dac_.setMuted(m); }
    bool  isMuted() const    { return dac_.isMuted(); }

private:
    /// stock(cycle) = cycle * stockRatio_ + stockOffset_, re-based at every
    /// clock change so the axis never jumps. Identity on a stock machine.
    double toStock(uint64_t cycle) const;
    void   rebase(double cpuHz, double stockHz);

    M6502*    cpu_ = nullptr;
    Dac8Audio dac_;
    double    cpuHz_       = 0.0;   // 0 = never given
    double    stockHz_     = 0.0;   // 0 = never given: follow the CPU clock
    double    stockRatio_  = 1.0;
    double    stockOffset_ = 0.0;
};

} // namespace pom2

#endif // POM2_SAM_CARD_H
