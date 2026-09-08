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

// ClockCard — ThunderClock+-compatible real-time clock card. Plugs into
// a slot (4 by convention) and emulates the bit-banged uPD1990AC RTC
// chip well enough that ProDOS 8 / 2.4's hardcoded ThunderClock driver
// reads host wall-clock through it. With this in place ProDOS file
// timestamps and BASIC.SYSTEM `DATE` / `TIME` reflect real time.
//
// Detection signature
// -------------------
// ProDOS scans slots 1-7 at boot for the canonical clock-card bytes:
//
//   $Cs00 = $08   (PHP)
//   $Cs02 = $28   (PLP)
//   $Cs04 = $58   (CLI)
//   $Cs06 = $70   (BVS)
//
// On match ProDOS copies its hardcoded ThunderClock driver into RAM
// (around $D742) and rewrites its $BF06-$BF08 vector to JMP into that
// copy. The slot ROM itself is touched only for detection — every
// subsequent CLOCK call talks to the chip via the device-select range
// below, NOT by JMPing back to $Cs00.
//
// uPD1990AC bit-bang protocol
// ---------------------------
// All chip access goes through a single soft-switch register at $C0n0
// (slot 4 → $C0C0):
//
//   write $C0n0:
//     bit 0  DATA_IN     (serial command bit input)
//     bit 1  CLK         (rising edge = shift one bit)
//     bit 2  STB         (rising edge = latch mode bits)
//     bit 3  C0          mode bit 0
//     bit 4  C1          mode bit 1
//     bit 5  C2          mode bit 2
//
//   read  $C0n0:
//     bit 5  IRQ_ASSERTED (this card pulled the slot IRQ line — manual 5-3)
//     bit 7  DATA_OUT    (LSB of shift register, sampled after CLK)
//
// Interrupts (TP / Timing-Pulse)
// ------------------------------
// The uPD1990AC's TP output toggles at a software-selectable rate. The
// ThunderClock+ wires TP (gated by an enable latch) to the slot IRQ line,
// giving a periodic interrupt source (used by clock/scheduler utilities).
// Per the ThunderClock Plus manual ch. V:
//
//   write $C0n0 bit 6 ($40)  enable interrupts   ("INTERRUPT CONTROL REG")
//   write $C0n0 bit 6 = 0     disable + reset interrupt hardware
//   any device-select read/write   clears a pending request (manual 5-2)
//   RESET                          disables interrupts
//
// The rate is the chip's TP rate, selected via the C0/C1/C2 mode field:
// MODE_TP_64HZ/256/2048 (= codes 4, 5, 6). The parallel uPD1990AC's 3-bit
// C field can't reach the uPD4990A interval timers (1/10/30/60 s, enum
// 8..11), so those are not modelled — and it cannot reach MODE_TP_4096HZ
// either. Code 7 on the PARALLEL field is MODE_TEST: MAME
// `upd1990a.cpp:198-205` `stb_w` does `m_c = m_c_unlatched; if (m_c == 7)
// m_c = MODE_TEST;` unless `is_serial_mode()` (`:63-67`, a uPD4990A with
// C=7), and `a2thunderclock.cpp:94` fits a plain `UPD1990A`. So 4096 Hz is
// simply not a rate this card can produce; test mode stops the time counter
// and makes a following SHIFT / TIME_READ latch put TP at 32 Hz
// (`:240-242`, `:307-309`). The firmware exposes only 64/256/2048 Hz to the
// user ("," "." "/" write-mode chars), so nothing shipped ever writes 7.
//
// To read time, the host driver writes mode = 0b011 (MODE_TIME_READ),
// pulses STB to load the current time counter into the 40-bit shift
// register, then pulses CLK 40 times, sampling bit 7 of $C0n0 after
// each rising edge to assemble 5 BCD bytes:
//
//   sec, min, hr, day, (month<<4)|dow
//
// There is NO year on this chip: MAME's upd1990a shifts 5 bytes for a
// non-4990A part, and the shipped ThunderClock firmware clocks exactly
// 10 nibbles = 40 pulses (see the 40-bit note in deviceSelectWrite).
// The host driver converts BCD → binary and packs into ProDOS DATE/TIME,
// supplying the year itself.

#ifndef POM2_CLOCK_CARD_H
#define POM2_CLOCK_CARD_H

#include "SlotPeripheral.h"
#include "CpuClock.h"

#include <array>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

class ClockCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;

    /// Construct with the slot number this card will be plugged into.
    /// The slot bakes into the slot ROM signature offsets but does not
    /// affect the chip protocol.
    explicit ClockCard(int slot = kDefaultSlot);

    int  getSlot() const { return slot; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Clock (ThunderClock+)"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override { return rom[low8]; }
    // //c-class profiles force INTCXROM on, masking every slot ROM with
    // the internal IORom — which means ProDOS's slot scan never sees the
    // $08/$28/$58/$70 ThunderClock signature at $Cs00/02/04/06. Punch
    // the hole so a clock plugged into a free //c slot (typically sl7;
    // sl3 collides with the built-in 80-col firmware) is detectable.
    bool    exposesIicOnboardRom() const override { return true; }
    void    onReset() override;
    void    advanceCycles(int cycles) override;

    /// Emulated CPU clock, for converting the chip's own-crystal TP rate
    /// into emulated cycles. The uPD1990AC's TP derives from ITS crystal
    /// (a real-time reference), so the period in CPU cycles must follow
    /// the host standard: left at the NTSC constant, TP ran 0.7 % slow in
    /// wall-clock under PAL (64 Hz became 63.55 Hz for a guest using TP
    /// IRQs as a timebase). Wired from EmulationController::setVideoStandard.
    void setCpuClock(double hz) override;

    /// Snapshot/rewind: 'CLK1'-tagged blob with the uPD1990AC bit-bang
    /// state (40-bit shift register + edge-detect latches + mode), the
    /// user time offset and the TP/IRQ timer. Host wall-clock is NOT
    /// serialized — the clock keeps telling real time — but a rewind
    /// mid-shift-out used to resume against the LIVE shift register and
    /// hand ProDOS a garbled date.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── Interrupt / TP state (for debug panels + tests) ─────────────────
    /// True when the INTERRUPT CONTROL REGISTER enable bit ($C0n0 bit 6)
    /// is set, i.e. TP edges currently reach the slot IRQ line.
    bool interruptsEnabled() const { return irqEnabled_; }
    /// True while a TP-driven interrupt request is latched (cleared by any
    /// device-select access). Mirrors the bit-5 "interrupt asserted" flag.
    bool interruptPending() const { return irqPending_; }
    /// Currently-selected TP rate in Hz (0 = TP timer stopped). One of
    /// 64 / 256 / 2048 once a TP/REGISTER_HOLD mode has been latched, or 32
    /// after a SHIFT / TIME_READ latched while the chip is in MODE_TEST.
    /// Never 4096: that enum value is unreachable through the parallel C
    /// field (see the MODE_TEST note at the top of this file).
    int  tpRateHz() const { return tpRateHz_; }
    /// MAME `upd1990a.cpp` `m_testmode` — mode code 7 latched.
    bool testModeActive() const { return testMode_; }

    /// Time-source hook. Defaults to std::time + localtime_r (reentrant:
    /// this card converts on the CPU thread while the UI thread converts
    /// times of its own); the public test factory below swaps in a
    /// deterministic source.
    using TimeFn = std::tm (*)();

    /// Test-only factory: build a ClockCard that reads time through
    /// `fn` instead of the host clock. Returns a unique_ptr so the
    /// caller can transfer it to a SlotBus.
    /// `probeDump=false` skips the roms/ probe so a test can build the
    /// dump-less (synthetic ROM, empty expansion window) card on a tree
    /// that ships the EPROM.
    static std::unique_ptr<ClockCard> makeForTest(int slot, TimeFn fn,
                                                  bool probeDump = true);

    // ─── Expansion ROM ($C800-$CFFF) ────────────────────────────────────
    // Populated only when a 2 KB Thunderware U9 dump is loaded — the synth
    // ROM and the 256 B variant leave it empty (open bus).
    uint8_t expansionRomRead(uint16_t offset) override;
    /// MAME `a2thunderclock.cpp:73` `take_c800() const override { return true; }`
    /// — the ThunderClock+ drives /IOSTB, but only when the 2 KB Thunderware
    /// EPROM is actually loaded. With the synthetic ROM, or with the 256-byte
    /// slot-ROM-only dump `tryLoadDump` also accepts, `expansionRomRead`
    /// answers $FF for the whole window — latching it then starved whichever
    /// card really serves $C800 (bug hunt #9; same rule as the Grappler+).
    bool takesC800() const override { return expansionRomLoaded_; }

    /// Was the slot ROM sourced from a real Thunderware U9 dump (vs the
    /// synthetic ProDOS-signature stub)? Surfaced for the UI's About /
    /// hardware-status panel.
    bool romFromDump()           const { return romFromDump_; }
    /// Path the dump was loaded from (empty when synthetic).
    const std::string& romSource() const { return romSource_; }

private:
    int slot;
    std::array<uint8_t, 256>   rom{};
    std::array<uint8_t, 0x800> expansionRom_{};   // $C800-$CFFF (2 KB)
    bool                       expansionRomLoaded_ = false;
    bool                       romFromDump_        = false;
    std::string                romSource_;
    TimeFn timeFn;

    // ── uPD1990AC bit-bang chip state ─────────────────────────────────
    //
    // The 40-bit shift register lives in the FIRST 5 of these 6 bytes, in
    // shift-out order (LSB of byte 0 goes out first):
    //   shiftReg[0] = seconds (BCD)
    //   shiftReg[1] = minutes (BCD)
    //   shiftReg[2] = hours   (BCD)
    //   shiftReg[3] = day     (BCD)
    //   shiftReg[4] = (month << 4) | (day_of_week)
    //   shiftReg[5] = UNUSED — the chip has no year counter; kept only so
    //                 the array shape (and the snapshot blob) is stable.
    //
    // STB rising edge with MODE_TIME_READ snapshots host time and loads
    // these six bytes. Each subsequent CLK rising edge shifts the whole
    // bank right by one bit, with DATA_OUT exposing the new LSB of
    // shiftReg[0].
    std::array<uint8_t, 6> shiftReg{};
    uint8_t prevWrite = 0;        // last value seen at $C0n0 write
                                  // (for CLK / STB rising-edge detection)
    uint8_t lastMode  = 0;        // most recently latched C0/C1/C2 (0..7)

    // ── MODE_TIME_SET state ────────────────────────────────────────────
    //
    // When the host commits MODE_TIME_SET (STB rising edge after loading
    // the shift register via 40 CLK pulses in MODE_SHIFT), the resulting
    // BCD bytes are converted back to a `time_t` and the delta against
    // the host clock at that moment is stored as `userOffsetSeconds`.
    // Subsequent reads compose `timeFn() + userOffsetSeconds` so the
    // "set time" advances naturally with wall-clock (mirroring MAME's
    // internal time counter that ticks at 1 Hz from `m_timer_clock`).
    //
    // For a test injector returning a fixed timestamp, the offset is
    // captured once and the effective time stays pinned — matching the
    // existing fixed-snapshot behaviour the smoke test relies on.
    bool        userOffsetActive  = false;
    std::time_t userOffsetSeconds = 0;

    // ── uPD1990AC TP (Timing Pulse) timer ──────────────────────────────
    //
    // The chip toggles its TP output at 2× the labelled rate (MAME
    // `upd1990a.cpp:248-257` programs `m_timer_tp` at `(clock()/div)*2`).
    // We track the labelled rate plus the CPU cycles per toggle, and drive
    // the toggle from `advanceCycles()`. A TP rising edge is the IRQ-worthy
    // event, so the slot IRQ fires `tpRateHz_` times per second while
    // enabled. tpHalfPeriodCycles_ == 0 means the timer is stopped.
    double cpuClockHz_       = static_cast<double>(POM2_CPU_CLOCK_HZ);
    int  tpRateHz_           = 0;
    int  tpHalfPeriodCycles_ = 0;
    int  tpAccumCycles_      = 0;
    bool tpLevel_            = false;     // current TP output level

    // MAME `upd1990a.cpp:216-218` `m_testmode`. Mode code 7 on the PARALLEL
    // C0/C1/C2 field is MODE_TEST on a plain uPD1990A (`:202-204`), NOT
    // "TP 4096 Hz" — that enum value is only reachable through a uPD4990A's
    // serial command byte, and `a2thunderclock.cpp:94` fits the 1990A.
    // While set, MODE_SHIFT / MODE_TIME_READ program TP to 32 Hz
    // (`:240-242`, `:307-309`) instead of leaving it alone.
    bool testMode_           = false;

    // ── ThunderClock+ interrupt logic (card-level, POM2-original) ───────
    //
    // MAME's `a2thunderclock.cpp` never binds the chip's tp_callback, so
    // the TP→IRQ path is not modelled upstream; the wiring here follows
    // the ThunderClock Plus manual ch. V. `irqEnabled_` is the $C0n0 bit-6
    // enable latch; `irqPending_` is the request flip-flop clocked by TP
    // and cleared by any device-select access.
    bool irqEnabled_ = false;
    bool irqPending_ = false;

    void buildRom();

    /// Probe `roms/thunderclock_u9_v1.3.bin` (and a couple of name variants)
    /// and load it in place of the synthetic ROM if found. Accepts a 256 B
    /// slot-ROM-only dump or a 2 KB dump that also covers $C800-$CFFF. On
    /// failure (missing / wrong size / signature mismatch) the synthetic
    /// ROM stays in place. Called once from the constructor before
    /// `onReset`. See `Thunderware Thunderclock Plus` /
    /// `Thunderware_REV_1.3_ROM_U9.bin` in markadev/AppleII-RevEng.
    void tryLoadDump();

    /// (Re)program the TP timer for a freshly-latched C0/C1/C2 mode.
    /// Only REGISTER_HOLD and the four TP modes change the rate; SHIFT /
    /// TIME_SET / TIME_READ leave TP running at its previous rate (matching
    /// MAME normal-mode behaviour). Called on STB rising edge.
    void programTpTimer(uint8_t mode);

    /// Set the TP rate in Hz (0 = stop) and recompute the per-toggle cycle
    /// budget. Resets the accumulator so the rate change takes effect from
    /// the next edge.
    void setTpRate(int hz);

    /// Clear a pending interrupt request and release the slot IRQ line.
    /// The enable latch is left intact so a periodic source keeps firing.
    void clearIrqRequest();

    /// Resolve the time the next MODE_TIME_READ snapshot should emit.
    /// Without a prior TIME_SET this is just `timeFn()`; with a TIME_SET
    /// it is `timeFn()` adjusted by `userOffsetSeconds`.
    std::tm effectiveTime() const;

    /// Snapshot the effective time into shiftReg as 6 BCD bytes.
    /// Called on STB rising edge when MODE_TIME_READ is selected.
    void latchTimeToShiftReg();

    /// Commit the contents of shiftReg as the new "set time" — converts
    /// the 6 BCD bytes back to `time_t` (via `std::mktime`) and stores
    /// the delta vs the current host clock so future reads carry the
    /// adjustment. Called on STB rising edge when MODE_TIME_SET is
    /// selected.
    void commitTimeSetFromShiftReg();

    ClockCard(int slot, TimeFn fn, bool probeDump = true);
};

#endif // POM2_CLOCK_CARD_H
