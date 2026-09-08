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

// TranswarpCard — the Applied Engineering **TransWarp** accelerator (1986).
// Ported from MAME `src/devices/bus/a2bus/transwarp.cpp` (R. Belmont,
// BSD-3-Clause), with ONE deliberate structural divergence, explained below.
//
// ─── What the hardware is ────────────────────────────────────────────────
//
// A 65C02 running at 3.58 MHz on a card, plus its own 4 KB ROM. On reset it
// takes the bus away from the Apple's own 6502 and runs the SAME program out
// of the SAME memory — it is not a coprocessor doing separate work, it is
// the machine's CPU relocated onto a faster clock. Everything else in the
// Apple still runs at 1 MHz, which is where all of its complications come
// from.
//
// ─── The divergence from MAME, and why ───────────────────────────────────
//
// MAME instantiates a SECOND `W65C02` on the card and has it DMA the Apple's
// bus for every access (`dma_r`/`dma_w` → `slot_dma_read`/`slot_dma_write`).
// It does that because a MAME a2bus card cannot retime the host CPU — the
// only way to make the machine run faster is to substitute a faster CPU.
//
// POM2 can retime the host CPU: the emulation worker's per-frame budget is
// `cyclesPerFrame`, and multiplying it IS running the 6502 faster. So this
// card publishes a multiplier (`cpuSpeedMultiplier`) and keeps the Apple's
// own `M6502`, which is closer to what the board does — same program, same
// memory, faster clock — and costs nothing on the hot path. The price is
// that the multiplier is SAMPLED once per frame; see "Sampling" below for
// why that is exact in aggregate rather than merely close.
//
// The multiplier values are exact rationals, not fitted constants. The card
// is clocked from the Apple's 7M line (14.31818 / 2 MHz):
//
//     full acceleration = 7.159 / 2 = 3.579545 MHz    (MAME `clock() / 2`)
//     half acceleration = 7.159 / 4 = 1.789773 MHz    (MAME `clock() / 4`)
//     Apple II          = 14.31818 / 14 = 1.022727 MHz
//
// giving 14/4 = **3.5×** and 14/8 = **1.75×** exactly.
//
// ─── Software interface (MAME `transwarp.cpp:16-19`, `dma_w`) ────────────
//
//   write $C074 = 0   full speed (per DSW1 bit 7)
//   write $C074 = 1   drop to 1 MHz
//   write $C074 = 3   halt the card's CPU, hand the bus back to the Apple's
//                     — the machine stays at 1 MHz until the next reset
//   write $C072       stop shadowing $F000-$FFFF with the card's ROM
//
// $C074 is CONSUMED by the card (MAME's `dma_w` returns before
// `slot_dma_write`), so it never reaches the Apple's paddle-reset latch;
// $C072 is snooped and passed through, so it rearms the paddles like any
// other $C07x access. That asymmetry is in the MAME source and is kept.
//
// There is deliberately NO way to detect the card by reading a register —
// MAME's header comment says so outright ("There's no way I can tell to
// detect it besides maybe measuring how many cycles between vblanks"). It
// has no slot ROM and no $C0nX window; it occupies a slot for power and bus
// access only.
//
// ─── The slowdowns ───────────────────────────────────────────────────────
//
// A card at 3.58 MHz talking to 1 MHz peripherals breaks their timing, so
// the board watches the bus and drops itself to 1 MHz around the accesses
// that care (MAME `hit_slot` / `hit_slot_joy`):
//
//   $C090-$C0FF   slot 1-7 device select  → 20 µs at 1 MHz
//   $C100-$C7FF   slot 1-7 ROM window     → 20 µs at 1 MHz
//   $C070         paddle strobe           → 11 × 257 µs (a full PREAD)
//
// per-slot, gated on DSW2. Note POM2 does not NEED these to keep peripherals
// correct: its whole time base is CPU cycles, so a Disk II at 3.5× spins 3.5×
// faster in wall-clock and the nibble pacing per CPU cycle is unchanged —
// which is exactly the same reason the //c Plus profile can run at 4× with a
// working drive. They are modelled because they are what the board does, and
// because they are visible to anything anchored to the video frame.
//
// ─── Sampling ────────────────────────────────────────────────────────────
//
// `cpuSpeedMultiplier()` is read once per 4096-cycle CHUNK of the frame
// (2026-09-09; once per frame before), and the slowdown windows are ~20
// cycles. Sampling a duty cycle at a rate uncorrelated with it is an
// unbiased estimator of it: guest code hammering a slot in a tight loop
// keeps the window permanently open and is sampled at 1× every chunk;
// code touching a slot once a frame is sampled at 1× on a fraction of
// chunks; anything in between converges to the true average over a few
// frames. What per-chunk sampling adds is WHERE in a frame the slow cycles
// fall, to a quarter of a 1 MHz frame: a program that starts hammering a
// slot mid-frame used to get the whole frame at 3.5× because the frame's
// budget had been fixed at its start (`transwarp_chunk_sampling` measures
// 59 657 CPU cycles before, ~22 000 after, for a window opening a third of
// the way in — and 59 658 before for a window open from the first cycle).
// Finer than a chunk would put a branch on the bus hot path, which
// docs/PERFORMANCE.md forbids.
//
// ─── ROM shadow ──────────────────────────────────────────────────────────
//
// The board overlays $F000-$FFFF with its own 4 KB ROM until software writes
// $C072. AE shipped speed-corrected Monitor routines there — the stock F8
// ROM's delay loops (WAIT, the beep) are calibrated for 1 MHz and come out
// 3.5× short otherwise. POM2 implements this as a straight 4 KB swap in the
// ROM mirror (`Memory::loadRomBytes`), which is free at run time, keeping a
// copy of the displaced Apple ROM to put back.
//
// ROM-GATED: needs `roms/ae_transwarp_1.4.bin` (4096 bytes, CRC32
// afe37f55 — MAME `ROM_START(warprom)`). POM2 does not ship the dump. With
// no ROM the card plugs and accelerates normally and simply never shadows,
// which is also what DSW-driven software expects after a $C072 write.

#ifndef POM2_TRANSWARP_CARD_H
#define POM2_TRANSWARP_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Memory;

namespace pom2 {

class TranswarpCard : public SlotPeripheral
{
public:
    /// Exact ratios against the Apple's 1.022727 MHz — see the header.
    static constexpr double kFullSpeed = 3.5;
    static constexpr double kHalfSpeed = 1.75;

    /// MAME `transwarp.cpp` INPUT_PORTS defaults. DSW1 bits 0-6 declare each
    /// slot's card as a language card or not (the board never reads them;
    /// they exist on the physical switch block) and bit 7 picks full or half
    /// acceleration. DSW2 bits 0-6 say whether each slot may run
    /// accelerated, and bit 7 disables acceleration outright.
    ///
    /// Note DSW2 bit 5 defaults to 0 — slot 6 ships set to STOCK SPEED.
    /// That is Apple's Disk II, and it is the one slot AE did not trust to
    /// survive 3.5×.
    static constexpr uint8_t kDsw1Default = 0x7F;
    static constexpr uint8_t kDsw2Default = 0x5F;

    static constexpr uint8_t kDsw1HalfSpeed   = 0x80;
    static constexpr uint8_t kDsw2AccelOff    = 0x80;

    /// MAME `hit_slot`: "slow down for 20 uSec, should be more than enough".
    static constexpr int kSlotSlowMicros = 20;
    /// MAME `hit_slot_joy`: "PREAD main loop counts up to 11*256 uSec, add 1
    /// to cover the setup".
    static constexpr int kJoySlowMicros  = 11 * 257;

    /// The dump the shadow needs, and what it must be.
    static constexpr const char* kRomPath  = "roms/ae_transwarp_1.4.bin";
    static constexpr std::size_t kRomSize  = 4096;

    explicit TranswarpCard(int slot);

    std::string_view name() const override { return "TransWarp (Applied Engineering)"; }

    // ─── Configuration (the physical DIP blocks) ─────────────────────────
    void    setDsw1(uint8_t v) { dsw1_ = v; }
    void    setDsw2(uint8_t v) { dsw2_ = v; }
    uint8_t dsw1() const { return dsw1_; }
    uint8_t dsw2() const { return dsw2_; }

    /// Convenience over the two switches that users actually reach for.
    void setFullAcceleration(bool full);
    bool fullAcceleration() const { return (dsw1_ & kDsw1HalfSpeed) == 0; }
    void setAccelerationEnabled(bool on);
    bool accelerationEnabled() const { return (dsw2_ & kDsw2AccelOff) == 0; }
    /// Whether slot `s` (1..7) is allowed to run accelerated.
    void setSlotAccelerated(int s, bool on);
    bool slotAccelerated(int s) const;

    /// Adopt the card's own 4 KB ROM. Takes effect immediately if the card
    /// is already plugged and still shadowing. Returns false (and shadows
    /// nothing) unless `bytes.size() == kRomSize`.
    bool setRom(std::vector<uint8_t> bytes);
    bool hasRom() const { return rom_.size() == kRomSize; }
    /// True while $F000-$FFFF is the card's ROM rather than the Apple's.
    bool shadowActive() const { return shadowing_; }

    /// The Memory whose $F000-$FFFF mirror the shadow swaps. Must be set
    /// before plugging for the shadow to engage; the accelerator itself
    /// works without it.
    void setMemory(Memory* m) { memory_ = m; }

    // ─── What the rest of the machine asks the card ──────────────────────
    double cpuSpeedMultiplier() const override;

    /// Bus snoop. Returns true when the card CONSUMED the access and the
    /// Apple must not also act on it — true only for a $C074 write, exactly
    /// as MAME's `dma_w` returns early there. Called from
    /// `Memory::softSwitchAccess` for $C070-$C07F and from `SlotBus` for the
    /// slot windows.
    bool snoopsBus() const override { return true; }
    bool busSnoop(uint16_t addr, bool isWrite, uint8_t value) override;

    void advanceCycles(int cycles) override;
    void onPlug()   override;
    void onUnplug() override;
    void onReset()  override;

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    /// Diagnostics for the UI / tests.
    bool     inOneMhzMode() const { return in1MHz_; }
    bool     cpuHalted()    const { return halted_; }
    bool     readsAppleRom() const { return readA2Rom_; }
    int      slowCyclesRemaining() const { return slowCycles_; }

    /// Load `kRomPath` (probing the usual relative bases) into a fresh card.
    /// Returns an empty string on success, or a human-readable reason the
    /// shadow will be inactive.
    std::string loadRomFromDisk();

private:
    void hitSlot(int slot);
    void hitJoystick();
    void engageShadow();
    void releaseShadow();

    uint8_t dsw1_ = kDsw1Default;
    uint8_t dsw2_ = kDsw2Default;

    // MAME's m_bEnabled / m_bReadA2ROM / m_bIn1MHzMode, plus the halt that
    // MAME expresses as INPUT_LINE_HALT + lower_slot_dma().
    bool enabled_   = true;
    bool readA2Rom_ = false;
    bool in1MHz_    = false;
    bool halted_    = false;

    // MAME's emu_timer, in CPU cycles instead of attotime: counted down by
    // advanceCycles, which is the only clock a slot card gets here.
    int slowCycles_ = 0;

    bool    shadowing_ = false;
    Memory* memory_    = nullptr;
    std::vector<uint8_t>            rom_;
    std::array<uint8_t, kRomSize>   displaced_{};   // the Apple ROM we cover
};

} // namespace pom2

#endif // POM2_TRANSWARP_CARD_H
