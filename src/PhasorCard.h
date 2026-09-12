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

// PhasorCard — Applied Engineering Phasor sound card. Dual-mode
// successor to the Mockingboard: starts in Mockingboard-compat mode
// (so existing MB software works untouched) and software-switches to
// Phasor-native mode (4× AY-3-8913 → 12 voices, doubled chip clock).
//
// Hardware (matches MAME `a2bus/phasor.cpp` + AppleWin `Mockingboard.cpp`):
//
//   * 2 × 6522 VIAs (identical to Mockingboard's 2-VIA layout)
//   * 4 × AY-3-8913 PSGs (vs Mockingboard's 2)
//   * Mode register (3 bits) driven by reads/writes to $C0(8+s)X
//
// Address map (s = slot number) — MAME `a2bus_phasor_device::read_cnxx`
// / `write_cnxx` (`a2mockingboard.cpp:312-337` / `:365-390`): the decode
// is gated to offsets $00-$1F and $80-$9F in BOTH modes; the VIA select
// depends on the mode (see PhasorCard::viaSelect):
//
//   Mockingboard-compat mode:
//     $Cs00..$Cs1F   VIA1   (reg = low 4 bits; $10-$1F mirror)
//     $Cs80..$Cs9F   VIA2   (reg = low 4 bits; $90-$9F mirror)
//   Phasor-native mode (via_sel = ((off & $80) >> 6) | ((off & $10) >> 4)):
//     $Cs00..$Cs0F   no VIA (undecoded)
//     $Cs10..$Cs1F   VIA1
//     $Cs80..$Cs8F   VIA2
//     $Cs90..$Cs9F   BOTH   (write broadcast; read = OR of both bytes)
//   Everything else ($20-$7F, $A0-$FF): undecoded — writes dropped,
//   reads return 0 (MAME parity).
//   $C0(8+s)0..F     Mode soft-switch (see below)
//
// 6522 → AY wiring (per VIA, same as Mockingboard):
//
//   Port A (8 bits)   → AY data bus D0..D7
//   Port B bit 0      → AY BC1
//   Port B bit 1      → AY BDIR
//   Port B bit 2      → AY /RESET (active LOW)
//   Port B bit 3..4   → CHIP-SELECT (Phasor-native only — see below)
//
// Chip-select decode (Phasor-native mode, MAME `phasor.cpp`):
//
//     chip_sel = (~(port_b >> 3)) & 3
//
//   chip_sel:
//      0  none selected   (e.g. PB[4:3] = 0b11 → both bits high → nothing)
//      1  primary AY      (VIA1 → AY1; VIA2 → AY3)
//      2  secondary AY    (VIA1 → AY2; VIA2 → AY4)
//      3  BOTH AYs        (broadcast — writes go to both, reads ambiguous)
//
// In Mockingboard-compat mode, PB3..7 are ignored: VIA1 always drives
// AY1, VIA2 always drives AY3 (AY2 / AY4 stay silent — they're the
// "extra" Phasor chips). This matches the real card's compat default.
//
// Mode soft-switch (AppleWin `Mockingboard.cpp` rules, distilled):
//
//   On any read OR write to $C0(8+s)X with offset bits = [b3, b2, b1, b0]:
//     if b3 == 1:                clear mode bits 2:0
//     mode |= (offset & 0b111)
//
//   Resulting mode values:
//     PH_Mockingboard = 0      (compat)
//     PH_Phasor       = 5      (native; written via $C0(8+s)D after clear)
//     PH_EchoPlus     = 7      (single-VIA / dual-AY broadcast variant —
//                              ACKNOWLEDGED but treated as Phasor for v1)
//
//   Initial mode at reset = PH_Mockingboard. Power-up software can
//   switch via the canonical $C0(8+s)8 (force MB) → $C0(8+s)5 (set bits
//   to 5 = Phasor) pattern, or the simpler single-write $C0(8+s)D
//   (clear-then-set-5).
//
// Clock scaling. In Phasor-native mode the AY chip clock is doubled
// (kAyClockHz * 2) — real Phasor halves the AY divider in native mode
// so periods produce notes one octave higher than the same register
// values would on a Mockingboard. The chip-clock multiplier is
// surfaced as `clockScale()` for the audio synthesiser to apply.
//
// Audio synth: full 4-AY mono mix
// -------------------------------
// The `AudioSrc` runs all 4 AY-3-8913s in parallel on the audio thread:
// snapshot the 4 register banks under the parent mutex, then synthesise
// tone (3 ch/chip × integer counter + fractional accum), noise (17-bit
// LFSR), and envelope (MAME 4-flag state machine) per chip. The mono
// mix divides by 12 (= 4 chips × 3 channels × peak 1.0) so a maxed-out
// Phasor-native signal sits at 1.0 before the volume knob. `clockScale`
// (×2 in PH_Phasor) multiplies the per-sample step rate so the same
// register values produce notes one octave higher in native mode — the
// AY register periods don't change, the chip clock does.
//
// MB-compat note: in PH_Mockingboard only 2 of the 4 chips receive
// strobes (AY1 + AY3 — primary of each VIA), so the mix sits ~6 dB
// lower than a real Mockingboard plug at the same volume. The user
// can crank Phasor's slider to compensate; the alternative
// (dynamic divisor) would clip in Phasor-native mode.

#ifndef POM2_PHASOR_CARD_H
#define POM2_PHASOR_CARD_H

#include "Ay3_8910.h"
#include "AyEventRing.h"
#include "AudioSource.h"
#include "SlotPeripheral.h"
#include "Via6522.h"

#include <cstdint>
#include <memory>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include <string_view>

class M6502;

class PhasorCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;     // same as Mockingboard

    enum Mode : uint8_t {
        PH_Mockingboard = 0,
        PH_Phasor       = 5,
        PH_EchoPlus     = 7,
    };

    explicit PhasorCard(int slot = kDefaultSlot);
    ~PhasorCard() override;

    int getSlot() const { return slot_; }

    /// Inject the host CPU for the lazy-sync VIA timer back-channel
    /// (`getCycleCountNow()`). Safe to leave null in headless tests.
    void setCpu(M6502* cpu) { cpu_ = cpu; }

    /// Inner AudioSource — the caller (MainWindow) registers it with
    /// AudioDevice. v1 emits silence; the actual 4-AY mix is a
    /// follow-up.
    AudioSource* audioSource() override;

    void setSampleRate(uint32_t hz);
    void  setVolume(float v);
    float getVolume() const;
    void setMuted(bool m);
    bool isMuted()  const;

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Phasor"; }
    uint8_t slotRomRead     (uint8_t low8) override;
    void    slotRomWrite    (uint8_t low8, uint8_t v) override;
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    void    advanceCycles(int cycles) override;
    /// Retune the four AYs' input clock (slot phase 0) on a PAL/NTSC
    /// switch — mirrors MockingboardCard::setCpuClock.
    void    setCpuClock(double hz) override;
    void    onReset()  override;
    void    onUnplug() override;
    // Rewind/snapshot: the 2 VIAs + 4 AYs register/timer state.
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── Test hooks ──────────────────────────────────────────────────────
    /// Current Phasor mode (see Mode enum).
    Mode mode() const { return mode_; }
    /// Audio-clock multiplier: 1 in MB / EchoPlus, 2 in Phasor native.
    int  clockScale() const { return mode_ == PH_Phasor ? 2 : 1; }

    /// Direct peek into an AY register bank (chip 0..3).
    uint8_t getAyRegister(int chip, int reg) const;
    /// Direct peek into a VIA register without side effects (chip 0..1).
    uint8_t peekViaRegister(int chip, int reg) const;
    /// Slot IRQ contribution (OR of both VIAs).
    bool isIrqAsserted() const { return slotIrqAsserted(); }

    /// Telemetry: per-VIA write count, per-AY write count (incremented
    /// every time the VIA delivers a WRITE strobe that landed on the AY
    /// via chip-select), per-AY reset count.
    uint32_t getViaWriteCount(int chip) const {
        return (chip >= 0 && chip < 2) ? viaWriteCount_[chip] : 0;
    }
    uint32_t getAyWriteCount(int chip) const {
        return (chip >= 0 && chip < 4) ? ayWriteCount_[chip] : 0;
    }
    uint32_t getAyResetCount(int chip) const {
        return (chip >= 0 && chip < 4) ? ayResetCount_[chip] : 0;
    }

    /// Capture everything the diagnostic panel shows, under ONE acquisition
    /// of the card mutex (bug hunt #19).
    ///
    /// The panel used to assemble its view one accessor at a time:
    /// `peekViaRegister` (9 per VIA) + `getAyRegister` (16 per chip) +
    /// `snapshotSsi263` = **51 separate locks of `mtx` per rendered frame**
    /// on the Mockingboard, 82 on the Phasor — the same mutex the realtime
    /// audio thread takes on every callback and the CPU thread takes on
    /// every MMIO access. Two costs, and the second is the real defect:
    ///   * the panel interleaved with the running machine 51 times a frame,
    ///     at 60 Hz, on the lock that gates audio rendering;
    ///   * the values it displayed came from 51 DIFFERENT machine states, so
    ///     VIA1 and VIA2 — or an AY register bank and the IFR that explains
    ///     it — never described the same instant. A timing investigation was
    ///     reading a composite that never existed.
    struct Diagnostics {
        uint8_t  via[2][16]{};     ///< indexed by VIA register number
        uint32_t viaWrites[2]{};
        uint8_t  ay[4][16]{};
        uint32_t ayWrites[4]{}, ayResets[4]{};
        bool     irqAsserted = false;
        uint8_t  mode        = 0;
        int      clockScale  = 1;
    };
    Diagnostics captureDiagnostics() const;

private:
    /// Caller holds `mtx_` — see `captureDiagnostics`.
    uint8_t peekViaRegisterLocked(int chip, int reg) const;

    struct AudioSrc;

    int    slot_;
    M6502* cpu_ = nullptr;

    Mode   mode_ = PH_Mockingboard;

    // 2 VIAs + 4 AYs. VIA0 drives AY0/AY1, VIA1 drives AY2/AY3.
    std::unique_ptr<pom2::Via6522>  via_[2];
    std::unique_ptr<pom2::Ay3_8910> ay_[4];
    std::unique_ptr<AudioSrc>       audio_;

    // Last-sync cycle for lazy VIA timer catch-up. Same protocol as
    // MockingboardCard::syncToCpuCycle.
    uint64_t lastSyncCycle_ = 0;
    void syncToCpuCycle();
    void syncToCpuCycleAt(uint64_t now);

    // Telemetry counters.
    uint32_t viaWriteCount_[2]  = {0, 0};
    uint32_t ayWriteCount_[4]   = {0, 0, 0, 0};
    uint32_t ayResetCount_[4]   = {0, 0, 0, 0};
    // Bumped on every R13 write (envelope shape) so the audio thread
    // can restart the envelope even when the same value is re-stored —
    // real AY-3-8913 behaviour (set_shape runs on every R13 store).
    uint32_t ayEnvWriteCount_[4] = {0, 0, 0, 0};
    /// PB2 (/RESET) held low, per VIA pair. `onViaPortBChange` runs on every
    /// port-A/B output change, and holding /RESET low across a run of writes
    /// is legal, so an un-gated push queued one idempotent reset event per
    /// write — the flood MockingboardCard::ayResetHeld_ exists to stop
    /// (`Mockingboard.cpp`, the ResetOnly branch). Only the FALLING edge is
    /// stamped.
    bool ayResetHeld_[4] = {false, false, false, false};

    mutable std::mutex mtx_;

    // ── emuCycles-stamped AY register-write queue (2026-09-09) ─────────
    // The Mockingboard's design, verbatim, for four chips: every accepted
    // AY store (and every /RESET strobe) is stamped with the CPU cycle it
    // happened at and replayed by the audio thread at that cycle, instead
    // of the audio thread taking a snapshot of the register banks per
    // buffer — which quantised every beam-raced register change to the
    // buffer (5-10 ms), the one thing that kept the card *Partial* in the
    // MAME parity dashboard. See MockingboardCard for the timeline rules
    // (jitter buffer, re-anchor, breaks on rewind / reset / overflow).
    static constexpr uint8_t kRegAyReset = 0xFF;
    /// Pseudo-register carrying the AY CLOCK SCALE (1 in MB-compat /
    /// EchoPlus, 2 in Phasor-native). The scale is a property of the
    /// TIMELINE, not of "now": the render cursor deliberately trails the
    /// producer by two 20 ms bursts, so reading `clockScale()` per fill
    /// re-tuned the ~40 ms of not-yet-rendered backlog an octave up the
    /// instant the guest touched $C0(8+s)D — 27.8 ms of a 1997 Hz tone came
    /// out at 3996 Hz BEFORE the switch's own cycle was reached. Stamped
    /// like every other AY event, it lands where it happened.
    static constexpr uint8_t kRegClockScale = 0xFE;
    struct AyRegEvent {
        uint64_t cycle;
        uint8_t  chip;
        uint8_t  reg;
        uint8_t  val;
    };
    /// Fixed capacity, allocated once — see AyEventRing.h and the note on
    /// MockingboardCard's queue.
    pom2::AyEventRing<AyRegEvent> ayEvents_{kMaxAyEvents};  // guarded by mtx_
    std::atomic<uint64_t>  latestAyEventCycle_{0};
    static constexpr size_t kMaxAyEvents = 16384;
    uint32_t ayQueueGen_ = 0;
    /// Did the CHIPS go with the last timeline break? See
    /// invalidateAyTimeline(bool). Audio thread reads it under `mtx_`.
    bool     ayBreakResetsGens_ = true;
    /// Queue one cycle-stamped AY event. Caller holds mtx_.
    void queueAyEvent(int chip, uint8_t reg, uint8_t val);
    /// Declare the stamped stream discontinuous. Caller holds mtx_.
    /// `resetChips` says whether the CHIPS went with it: true for a card
    /// reset or a queue overflow, false for a rewind / snapshot restore,
    /// where the registers are restored and the generators must keep
    /// running — re-seeding them there re-attacked every envelope the guest
    /// had not re-triggered, once per seek of a scrub.
    void invalidateAyTimeline(bool resetChips = true);

    // MAME-parity slot-ROM VIA select: bit 0 = VIA1, bit 1 = VIA2, 0 =
    // undecoded. See the comment block above the definition for the
    // exact MAME `via_sel` + range-gate semantics. Caller holds mtx_.
    int viaSelect(uint8_t low8) const;

    // Dispatch a VIA Port B change to the AY pair attached to that VIA,
    // honouring the current mode + chip-select decode.
    void onViaPortBChange(int viaIdx);

    // Re-evaluate slot IRQ (OR of both VIAs' irqOut).
    void updateIrq();

    // Apply the soft-switch mode-update rule to `offset` (low 4 bits of
    // the device-select address). Called by both deviceSelectRead and
    // deviceSelectWrite — real Phasor responds to either side.
    void applyModeSwitch(uint8_t offset);
};

#endif // POM2_PHASOR_CARD_H
