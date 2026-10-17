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

// ZipChip — Zip Technologies' Zip Chip, the 6502-socket accelerator, as
// MAME models it in `src/mame/apple/apple2e.cpp` (the "4 MHz Zip Chip" CPU
// type of `apple2_sysconfig_accel`, :3862-3872 — the same code that runs
// the //c Plus's licensed built-in Zip). Line numbers below are mamedev/mame
// master as fetched 2026-10-16.
//
// ─── Register interface ($C05A-$C05F) ───────────────────────────────────
//
// Writes (apple2e.cpp:2562-2632, write side of c000_w):
//   $C05A  $5A ×4 in a row     unlock. Any OTHER write between the four
//                              cancels (MAME installs a write tap on the
//                              whole address space, :2570-2590)
//          $A5                 lock
//          anything else       (unlocked) disable acceleration
//   $C05B  any                 (unlocked) enable acceleration
//   $C05C  slot/speaker mask   (unlocked) bit n = slow down on slot n's
//                              $C0nX, bit 0 = slow down on the speaker
//   $C05E  bit 7               (unlocked) 1 = disable the slow-down delays
//   $C05F  game I/O flags      (unlocked) bit 6 = paddle delay
//   Every write ALSO reaches the motherboard (`do_io(offset)` on each), so
//   AN0-AN3 keep their meaning whether the chip is locked or not.
//
// Reads while UNLOCKED (apple2e.cpp:2237-2281): $C058 = $C0, $C059 = $20,
// $C05A = $C05D = $C05F = $00, $C05B = status (bit 7 PH0/1024 tap, bit 5
// LC-cache off, bit 4 Zip disabled, bit 3 delay active, bits 0-1 cache
// size = 64 KB), $C05C = the slot/speaker mask, $C05E = a STATEREG-like
// paging summary. The annunciator side effect of the read is SKIPPED when
// unlocked AND fast ("executes from cache", which MAME approximates by
// m_accel_fast; :2237-2244). Locked, $C058-$C05F are plain annunciators.
//
// ─── Speed and slow-downs ───────────────────────────────────────────────
//
// Fast = 4 MHz (MAME `m_accel_speed = 4000000`, :1315), stock otherwise
// (:1515-1525). Temporary 1 MHz windows (accel_temp_delay, :1547-1556):
// 52 ms after a $C0nX access to a slot whose mask bit is set (accel_slot,
// :1565-1569, called from c080_r/w :2898/:2934), 5 ms after a speaker
// toggle if mask bit 0 is set (:1914), 5 ms after PTRIG $C070 if game-I/O
// bit 6 is set (:2044). Reset (accel_reset, :1527-1545): locked, mask $E4
// (slots 7, 6, 5, 2), game-I/O $40, delays enabled, and fast iff "Bootup
// speed" says so.
//
// ─── POM2 divergences, stated ───────────────────────────────────────────
//
// * MAME cancels an unlock on ANY intervening write. POM2 sees writes at
//   and above $C000 only — RAM writes below it never leave the fast path,
//   and watching them would put a branch there (PERFORMANCE §§ 8.2/8.5).
//   An intervening write to the I/O page still cancels; a write to RAM does
//   not. Real unlock code (four consecutive `STA $C05A`) is unaffected.
// * The multiplier is sampled once per 4096-cycle chunk, as for the
//   TransWarp (TranswarpCard.h § Sampling) — exact in aggregate.
// * $C05B bit 7 toggles every 512 EMULATED cycles, not every 512 stock
//   cycles of wall time; equal while slow, ~3.9× faster while fast.
// * The "Bootup speed" default is FAST here (MAME's config defaults to
//   Standard). MAME says the real chip's own firmware decides at power-up
//   from the Esc/space keys and that its config is an approximation (:1537-
//   1538); the chip boots accelerated unless told otherwise, and a user who
//   installs one expects it to accelerate. `setBootFast(false)` gives MAME's
//   default.
// * $C05D writes (the Zip's documented speed-select register) are NOT
//   modelled: MAME has no case for them, so there is no source to port.
//   The chip runs at 4 MHz or stock.
//
// Present only on //e-class machines with slots (Memory gates it); the
// //c Plus keeps its fixed 4× profile budget.

#ifndef POM2_ZIP_CHIP_H
#define POM2_ZIP_CHIP_H

#include "CpuSocketAccelerator.h"
#include "CpuClock.h"

namespace pom2 {

class ZipChip final : public CpuSocketAccelerator
{
public:
    static constexpr double  kFastHz        = 4000000.0;   // apple2e.cpp:1315
    static constexpr uint8_t kResetSlotSpk  = 0xE4;        // :1533
    static constexpr uint8_t kResetGameIo   = 0x40;        // :1534
    static constexpr int     kSlotDelayMs   = 52;          // :1568
    static constexpr int     kShortDelayMs  = 5;           // :1914, :2044

    ReadResult ioRead(uint8_t low, uint64_t now, uint8_t stateByte) override;
    void   ioWrite(uint8_t low, uint8_t value, uint64_t now) override;
    double cpuSpeedMultiplier(uint64_t now) const override;
    void   onReset(uint64_t now) override;
    void   appendSnapshotState(std::vector<uint8_t>& out) const override;
    bool   loadSnapshotState(const uint8_t* data, std::size_t len) override;
    bool   validateSnapshotState(const uint8_t* data, std::size_t len) const override;

    /// The machine's stock clock (NTSC 1.0227 / PAL 1.0156 MHz): the 4 MHz
    /// multiplier and the millisecond delays are derived from it.
    void   setStockClockHz(double hz) { if (hz > 0) stockHz_ = hz; }
    void   setBootFast(bool on) { bootFast_ = on; }
    bool   bootFast() const { return bootFast_; }

    // Diagnostics / tests.
    bool    unlocked() const { return unlocked_; }
    bool    fast() const { return fast_; }
    uint8_t slotSpeakerMask() const { return slotSpk_; }
    uint8_t gameIo() const { return gameIo_; }
    bool    delayActive(uint64_t now) const { return now < slowUntil_; }

private:
    void tempDelay(int ms, bool condition, uint64_t now);

    double   stockHz_   = static_cast<double>(POM2_CPU_CLOCK_HZ);
    bool     bootFast_  = true;
    bool     unlocked_  = false;
    int      stage_     = 0;       // MAME m_accel_stage: $5A writes seen
    bool     fast_      = true;
    bool     disableDelay_ = false;
    uint8_t  slotSpk_   = kResetSlotSpk;
    uint8_t  gameIo_    = kResetGameIo;
    uint64_t slowUntil_ = 0;       // MAME m_acceltimer, as an emuCycles deadline
};

} // namespace pom2

#endif // POM2_ZIP_CHIP_H
