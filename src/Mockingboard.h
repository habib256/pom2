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

// MockingboardCard — Sweet Microsystems Mockingboard A/C-compatible sound
// card. Two 6522 VIAs, each driving an AY-3-8910 PSG (3 tone channels +
// noise + envelope). The cards plug into any free slot (4 by convention);
// most software hard-codes slot 4. Ported in spirit from MAME
// `bus/a2bus/mockingboard.cpp` + `machine/6522via.cpp` + `sound/ay8910.cpp`,
// but trimmed to the subset music drivers and SFX engines actually use.
//
// Address map (all in the slot ROM window; the device-select range
// $C0(8+N)X is unused):
//
//   $Cn00..$Cn0F   VIA #1  (16 registers, partial decode — high bits 0..7
//                           mirror across $Cn10..$Cn7F)
//   $Cn80..$Cn8F   VIA #2  (16 registers, partial decode — mirrors across
//                           $Cn90..$CnFF)
//
// 6522 → AY wiring (Sweet Microsystems Mockingboard A/C schematic;
// AppleWin `Mockingboard.cpp:193` matches MAME `mockingboard.cpp`):
//
//   VIA Port A (8 bits)  → AY data bus  D0..D7
//   VIA Port B bit 0     → AY BC1
//   VIA Port B bit 1     → AY BDIR
//   VIA Port B bit 2     → AY !RESET (active low: PB2=0 zeroes AY regs)
//
// (Earlier POM2 had PB0=/RESET and PB2=BC1 — every INACTIVE strobe a
// music driver emitted between LATCH and WRITE looked like /RESET-
// asserted and wiped the AY bank, silencing Nox Archaist, Ultima IV
// and Total Replay. Fixed 2026-05-14.)
//
// Control sequence (BDIR, BC1):
//
//     00  inactive   (no bus action)
//     01  read       (rare — drivers don't usually read AY back)
//     10  write      (write Port A to the latched register)
//     11  latch addr (latch Port A as the next register address)
//
// IRQ routing. Each VIA's IRQ line (IFR.bit7) is OR'd onto the slot IRQ.
// The Mockingboard music driver convention is to clock VIA #1 timer 1
// (T1) in continuous mode at the music tick rate (~50 Hz, 25 Hz or some
// multiple) and update AY registers from the IRQ handler. This makes T1
// the single most important VIA feature — the timer + IFR + IER subset
// is what we model precisely; CB1/CB2/SR/PCR are stubbed.
//
// Audio path. The card owns an inner AudioSource that AudioDevice mixes
// into the same float32 buffer as the speaker / cassette. The audio
// callback runs on miniaudio's thread; it takes the card's mutex to splice
// the cycle-stamped register-write queue and snapshot the banks, releases,
// then synthesises n samples using its own (audio-thread-resident) tone /
// noise / envelope state, replaying each write at its true sample offset.
// The card is STEREO, as the hardware is: AY1 (VIA1) goes left, AY2 (VIA2)
// right, each through its own DC blocker — the mono fold-down this comment
// used to describe went away with the stereo bus on 2026-08-01.
//
// What's NOT modelled (deliberate, scope-bounded omissions):
//
//   * 6522 shift register (SR). No software in the wild uses it on a
//     Mockingboard.
//   * 6522 CA2/CB1/CB2 outputs + handshake/pulse modes. CA1 *input*
//     edges (Sound II SSI263 A/!R wiring) and the MAME reg-1/ORA
//     IFR-clear rule ARE modelled — see Via6522::clearPaInt().
//   * AY-3-8910 I/O ports A/B (R14/R15). Mockingboard wires them as
//     unused; some Phasor / SuperMusicSynth boards use them for
//     channel routing but those are out of scope here.
//
// (T2 one-shot phase-2 mode IS modelled since the Via6522 extraction —
// see Via6522.h; Echo+/Ultima IV speech drivers and French Touch demos
// rely on it.)

#ifndef POM2_MOCKINGBOARD_H
#define POM2_MOCKINGBOARD_H

#include "Ay3_8910.h"
#include "AyEventRing.h"
#include "AudioSource.h"
#include "SlotPeripheral.h"
#include "Ssi263.h"
#include "Via6522.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>

class M6502;

class MockingboardCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;

    /// Hardware variant:
    ///   AC      = vanilla Mockingboard A or C (2× 6522 + 2× AY-3-8910, no
    ///             speech). Slot ROM is just the two VIAs with partial
    ///             address decode mirroring.
    ///   SoundII = Mockingboard "C" / Sound II — adds an SSI263A speech
    ///             synth selected by address bit A6 ($Cs40-$Cs7F and
    ///             $CsC0-$CsFF; the register is the low 3 bits, and the
    ///             write also lands in the VIA the address selects). Its
    ///             A/!R signal wires (inverted) to the SECOND 6522's CA1
    ///             — AppleWin: "SSI263's IRQ (A/!R) is routed via the 2nd
    ///             6522's CA1 input (at $Cn80)" — so a phoneme-end edge
    ///             latches IFR.CA1 in VIA2 and (if IER.CA1 is enabled by
    ///             the host) drives the slot IRQ. Stock Sound II software
    ///             configures PCR.0 = 0 (negative-edge active) to match
    ///             the inverted wiring.
    enum class Variant { AC, SoundII };

    explicit MockingboardCard(int slot = kDefaultSlot, Variant variant = Variant::AC);
    ~MockingboardCard() override;

    Variant getVariant() const { return variant_; }
    bool    hasSsi263()  const { return ssi_ != nullptr; }

    int getSlot() const { return slot_; }

    /// Inject the host CPU. Used for the lazy-sync timer back-channel
    /// (`getCycleCountNow()` — VIA T1/T2 counters need sub-instruction
    /// catch-up between MMIO touches; see `syncToCpuCycle()`). IRQ
    /// routing does NOT go through this pointer — `SlotPeripheral::
    /// assertIrq()` fans out via SlotBus's installed router. Safe to
    /// leave null in headless tests (the card falls back to legacy
    /// batched timer advance).
    void setCpu(M6502* cpu) { cpu_ = cpu; }

    /// Pointer to the inner AudioSource. The caller (MainWindow) is
    /// responsible for registering / deregistering it with AudioDevice
    /// and for keeping it alive past the last audio callback — the
    /// AudioSource lives inside the card, so it must be removed from
    /// AudioDevice before the card is destroyed.
    AudioSource* audioSource();

    /// Audio output sample rate negotiated with the OS device. Default
    /// is AudioDevice::kSampleRate; override before plugging if your
    /// device picked a different rate (Apple Silicon often picks 48 kHz).
    void setSampleRate(uint32_t hz);

    /// Volume in [0, 2]. UI thread sets, audio thread reads.
    void  setVolume(float v);
    float getVolume() const;

    void setMuted(bool m);
    bool isMuted() const;

    /// Emulated CPU clock, for the audio thread's emuCycles replay cursor.
    /// MUST follow the video standard: under PAL the CPU produces
    /// 1 015 625 cycles/s, and a cursor advancing at the NTSC nominal
    /// outruns the producer by 0.7 % — every queued AY write then lands
    /// at the START of its buffer instead of its true sample offset,
    /// silently undoing the sub-buffer timing on exactly the PAL demos
    /// (French Touch / DIX) that need it. Wired from
    /// `EmulationController::setVideoStandard`, same as the speaker and
    /// cassette. NOTE this does NOT retune the AY tone/noise generators
    /// themselves — those stay at the NTSC nominal, the project's
    /// documented audio-pitch approximation (see CLAUDE.md § profiles).
    void setCpuClock(double hz) override;

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Mockingboard"; }
    uint8_t slotRomRead (uint8_t low8) override;
    void    slotRomWrite(uint8_t low8, uint8_t v) override;
    /// The card decodes no DEVICE SELECT: $C0(8+n)X is open bus, the
    /// floating video byte — MAME's ayboard base ends its `read_c0nx` on
    /// `return get_open_bus();`, and CLAUDE.md's standing rule says the
    /// same. A hard $FF (the base class default) froze the floating bus for
    /// any guest sampling it through this window: VBL detection by comparing
    /// successive reads never sees a change.
    uint8_t deviceSelectRead(uint8_t) override { return openBus(); }
    /// The Mockingboard 4c: the //c-class build of this card, on the
    /// machine's internal expansion connector, answering at $C400-$C4FF.
    /// DIGIDREAM ("SPECIAL IIc/MB4C") wakes it with two writes to
    /// $C403/$C404 and then DETECTS it by reading the 6522's T1 counter
    /// there, so the window has to carry reads as well as writes.
    int iicRomWindowPage() const override { return 4; }
    void    advanceCycles(int cycles) override;
    void    onReset() override;
    void    onUnplug() override;
    // Rewind/snapshot: VIA + AY register/timer state (the audible music
    // state). SSI263 speech state is NOT captured (rare during a rewind).
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── Test hooks ──────────────────────────────────────────────────────
    /// Direct read of an AY register (peeks the latched register bank).
    /// Test-only — production code goes through the VIA control sequence.
    uint8_t getAyRegister(int chip, int reg) const;
    /// Direct read of a VIA register without any side effect (no T1 IFR
    /// clear, no shift). Test-only.
    uint8_t peekViaRegister(int chip, int reg) const;
    /// Whether the slot IRQ line is currently asserted (cumulative across
    /// both VIAs). Test-only — forwards to the base-class state cached by
    /// `SlotPeripheral::assertIrq`.
    bool isIrqAsserted() const { return slotIrqAsserted(); }

    /// Telemetry for the Mockingboard UI panel: how many MMIO writes the
    /// card has accepted on each chip's VIA, and how many of those
    /// progressed to an AY-3-8910 register write (the second number
    /// stays 0 if the music driver is running but never completes a
    /// LATCH→WRITE strobe, which would point at a bus-protocol bug).
    /// Cleared on `onReset()`.
    uint32_t getViaWriteCount(int chip) const {
        return (chip == 0 || chip == 1) ? viaWriteCount_[chip] : 0;
    }
    uint32_t getAyWriteCount(int chip) const {
        return (chip == 0 || chip == 1) ? ayWriteCount_[chip] : 0;
    }
    uint32_t getAyResetCount(int chip) const {
        return (chip == 0 || chip == 1) ? ayResetCount_[chip] : 0;
    }

    /// Snapshot the SSI263 state for the UI panel. Returns false if this
    /// card variant has no SSI263 (vanilla AC). When true, `*out` is
    /// populated with the chip's current register banks + playback flags.
    struct Ssi263Snap {
        uint8_t  regs[5];
        uint8_t  currentPhoneme;
        uint8_t  mode;
        bool     aRequest;
        bool     powerDown;
        bool     irqEnabled;
        int      phonemeRemainingCycles;
        uint32_t phonemeWriteCount;
    };
    bool snapshotSsi263(Ssi263Snap* out) const;
    /// Per-AY-command transition counters. `cmd` is the 2-bit
    /// {BDIR,BC1} encoding: 0=INACTIVE, 1=READ, 2=WRITE, 3=LATCH.
    /// Returning 0 for an out-of-range `chip` or `cmd` keeps the
    /// diagnostic panel safe.
    uint32_t getAyCommandCount(int chip, int cmd) const;

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
        struct Chip {
            uint8_t  via[16];      ///< indexed by VIA register number
            uint8_t  ay[16];
            uint32_t viaWrites = 0, ayWrites = 0, ayResets = 0;
            uint32_t cmd[4]{};     ///< {BDIR,BC1}: INACTIVE/READ/WRITE/LATCH
        };
        Chip       chip[2]{};
        bool       irqAsserted = false;
        bool       hasSsi      = false;
        Ssi263Snap ssi{};
    };
    Diagnostics captureDiagnostics() const;

private:
    /// Caller holds `mtx` — the body of `peekViaRegister`, split out so the
    /// whole card can be captured under one acquisition.
    uint8_t peekViaRegisterLocked(int chip, int reg) const;

    // Forward declarations. `Via6522` and `Ay3_8910` are shared with
    // PhasorCard (see `Via6522.h` / `Ay3_8910.h`); only AudioSrc remains
    // private to this card.
    struct AudioSrc;

    int     slot_;
    Variant variant_ = Variant::AC;
    M6502*  cpu_ = nullptr;

    // VIAs and AYs — each VIA drives the AY at the same index. Held by
    // unique_ptr so the inner types can stay opaque in this header.
    std::unique_ptr<pom2::Via6522>  via_[2];
    std::unique_ptr<pom2::Ay3_8910> ay_[2];
    // Optional SSI263 speech synth — non-null only on Variant::SoundII.
    // Lives at slot ROM offsets $40-$4F: register = low 3 address bits
    // (regs 0-7; 5 real + 3 unmapped), so $48-$4F mirror $40-$47. See
    // the decode note in MockingboardCard::slotRomRead for the
    // MAME/AppleWin wiring this simplifies.
    std::unique_ptr<pom2::Ssi263>   ssi_;
    std::unique_ptr<AudioSrc>       audio_;

    // Combined slot IRQ state — `via_[0].irqOut() || via_[1].irqOut()`.
    // Edge debouncing lives in SlotPeripheral::assertIrq, so no local
    // cache here.

    // Cycle the VIA timers were last advanced to. Both `slotRomRead/Write`
    // and the externally driven `advanceCycles()` catch up to "now"
    // (`cpu_->getCycleCountNow()`) before touching state, so a detection
    // routine that writes T1 and reads IFR a few cycles later sees the
    // up-to-date IFR instead of the stale batch-slice value. See the
    // CLAUDE.md "Mockingboard" section for the Nox Archaist / Skyfox /
    // Broadside detection-failure class this fixes.
    uint64_t lastSyncCycle_ = 0;
    void syncToCpuCycle();
    void syncToCpuCycleAt(uint64_t now);

    // Telemetry counters, bumped by slotRomWrite / onViaPortBChange.
    // Read by the Mockingboard ImGui panel; never affect emulation
    // semantics. Reset by onReset().
    uint32_t viaWriteCount_[2] = {0, 0};
    uint32_t ayWriteCount_[2]  = {0, 0};
    uint32_t ayResetCount_[2]  = {0, 0};
    // Bumped on every write to R13 (envelope shape) so the audio thread can
    // restart the envelope even when the shape value is unchanged.
    uint32_t ayEnvWriteCount_[2] = {0, 0};

    // ── emuCycles-stamped AY register-write queue ──────────────────────
    //
    // The audio thread used to snapshot both register banks ONCE per
    // audio buffer, so every write inside one buffer window collapsed to
    // the last value: an arpeggio or fast envelope written at ~1 ms
    // intervals came out quantised to the ~10 ms buffer (notes merged or
    // dropped). That is the emuCycles violation — CPU→audio events must
    // carry a CPU-cycle stamp, not land at buffer granularity.
    //
    // The CPU thread stamps each accepted AY register store with the
    // VIA's synced cycle; the audio thread replays them at their exact
    // sample offset inside the buffer (same cursor idiom as
    // SpeakerDevice::audioCpuCursor, including the catch-up snap).
    struct AyRegEvent {
        uint64_t cycle;
        uint8_t  chip;
        uint8_t  reg;
        uint8_t  val;
    };
    /// Pseudo-register marking an AY /RESET strobe (VIA PB2 low) in the
    /// event stream. A wholesale bank wipe carries no per-register write
    /// to stamp, so before 2026-08-02 the audio thread learned about it
    /// out-of-band through `ayResetCount_` — i.e. at CPU-NOW, ~40 ms
    /// ahead of the replay cursor, after which the still-queued pre-reset
    /// writes replayed on top and resurrected the note. Stamping it like
    /// any other write puts the silence exactly where the driver put it.
    static constexpr uint8_t kRegAyReset = 0xFF;
    /// Fixed capacity, allocated once: this queue is spliced on the
    /// REALTIME audio thread, where the deque this replaces called the
    /// allocator on every block boundary (bug hunt #19, `AyEventRing.h`).
    pom2::AyEventRing<AyRegEvent> ayEvents_{kMaxAyEvents};   // guarded by mtx
    std::atomic<uint64_t>  latestAyEventCycle_{0};
    /// Hard cap so a paused audio device can't grow the queue without
    /// bound (the audio thread drains it; nothing else does).
    static constexpr size_t kMaxAyEvents = 16384;

    /// Timeline generation, guarded by `mtx`. Bumped whenever the
    /// cycle-stamped stream becomes DISCONTINUOUS — the stamps already
    /// queued no longer describe the machine's future. The audio thread
    /// compares it against its own copy and, on a change, drops its
    /// backlog and re-primes from the live register bank.
    ///
    /// The cursor cannot do this job. A rewind drags `lastSyncCycle_` and
    /// `latestAyEventCycle_` backwards, so the cursor's own re-anchor
    /// follows them down — but `pending` still holds events stamped in
    /// the pre-rewind FUTURE, and the render loop consumes strictly
    /// front-ordered, so those block every write behind them and the AY
    /// bank freezes. Measured silence before this counter existed: 0.49 s
    /// / 2.00 s / >3 s for rewind depths of 0.5 / 2 / 5 s, bounded only
    /// by `kMaxAyEvents`.
    uint32_t ayQueueGen_ = 0;
    /// Did the CHIPS go with the last timeline break? See
    /// invalidateAyTimeline(bool). Audio thread reads it under `mtx`.
    bool     ayBreakResetsGens_ = true;
    /// Edge state for the /RESET strobe, per chip. `applyControl` reports
    /// `ResetOnly` on EVERY VIA write while PB2 is held low, and a driver
    /// can leave it low for many writes; only the falling edge is an
    /// event.
    bool     ayResetHeld_[2] = { false, false };

    /// Queue one cycle-stamped AY event (register store or `kRegAyReset`)
    /// for the audio thread. Caller must hold `mtx`.
    void queueAyEvent(int chip, uint8_t reg, uint8_t val);
    /// Drop both sides of the event stream and bump `ayQueueGen_`.
    /// Caller must hold `mtx`.
    /// Declare the cycle-stamped event stream discontinuous. Caller holds
    /// `mtx`. `resetChips` says whether the CHIPS went with it: true for a
    /// card reset or a queue overflow, false for a rewind / snapshot
    /// restore, where the registers are restored and the generators must
    /// keep running — re-seeding them there re-attacked every envelope the
    /// guest had not re-triggered, once per seek of a scrub.
    void invalidateAyTimeline(bool resetChips = true);

    // Cross-thread guard. CPU thread takes it for VIA reads/writes and
    // for `advanceCycles`; audio thread takes it briefly to snapshot
    // the AY register banks. `mutable` so the test peek accessors can
    // be const.
    mutable std::mutex mtx;

    // Updates AY[i] in response to a VIA[i] Port B / Port A change.
    void onViaPortBChange(int chip);

    // Re-evaluates slot IRQ (OR of both VIAs) and forwards to the CPU.
    void updateIrq();
};

#endif // POM2_MOCKINGBOARD_H
