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

// IWMDevice — port of MAME's `src/devices/machine/iwm.{h,cpp}` (Apple
// Integrated Wozniak Machine, the floppy disk controller in the //c,
// //c+, Mac 128/512/Plus/SE/Classic, and IIgs). The IWM is a
// successor to the Disk II wozfdc and extends it with:
//
//   * A mode register selectable from CPU (timing presets, sync mode,
//     bit-cell width)
//   * Three additional read-side registers (status, write-handshake,
//     "almost-always 0xFF") selected by the Q6/Q7 control bits
//   * A bit-cell-accurate read/write state machine that walks the
//     flux transition stream from the disk image instead of the
//     32-cycle nibble gate of the original Disk II.
//
// The //c+ alt firmware (bank 1, see DEV.md § Storage) drives this
// controller for both the on-board 5.25" Sony drive and the SmartPort
// 3.5" interface. POM2's current `DiskIICard` carries lightweight
// IWM-mode + IWM-WHD shadows that get the //c+ cold-reset path through
// banner display, but a full SmartPort 3.5" path needs the real
// `sync()` state machine — which is what this file ports.
//
// Why a separate device rather than expanding `DiskIICard`?
//   * The IWM state machine is genuinely different from the Disk II
//     LSS (P6 PROM) — different timing, different mode dispatch,
//     flux instead of nibbles. Trying to overlay it on the existing
//     LSS path would tangle two state machines that share no edges.
//   * MAME models the IWM as a standalone device that plugs into the
//     Apple II main board; mirroring that structure makes the port
//     line-for-line traceable to `mame/src/devices/machine/iwm.cpp`.
//   * Wiring this device behind the //c+ profile's $C0E0-$C0EF slot
//     mux (the equivalent of MAME `apple2e.cpp:2430-2432` where
//     `m_isiicplus && slot == 6` routes to `m_iwm`) is a thin shim
//     that doesn't disturb the II / II+ / //e Disk II path.
//
// Time model: POM2 measures everything in 1.023 MHz CPU cycles
// (`POM2_CPU_CLOCK_HZ`). MAME's IWM uses `attotime` plus an optional
// fast Q3 clock (~2 MHz) for synchronous mode. We collapse this into
// one cycle counter. MAME passes `q3_clock = 1021800*2` to every
// //c-class IWM (apple2e.cpp) — exactly 2x the CPU clock, so MAME's
// q3-domain write windows are numerically equal to POM2's scaled
// constants; sub-CPU-cycle q3 phase is the only thing not modelled.

#ifndef POM2_IWM_DEVICE_H
#define POM2_IWM_DEVICE_H

#include "DiskImage.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace pom2 {

class Sony35Drive;

class IWMDevice
{
public:
    /// Host callback shapes — mirror MAME `iwm.cpp::phases_cb` /
    /// `devsel_cb` / `sel35_cb`. Wired by EmulationController (via the
    /// SmartPort hub) so the active 3.5" Sony drive receives phase
    /// strobes and the active drive can be reselected when SEL bit
    /// flips. Each may be left nullptr; the IWM behaves as if the host
    /// didn't connect that pin.
    using PhasesCb = std::function<void(uint8_t phases)>;
    using DevselCb = std::function<void(uint8_t devsel)>;
    using Sel35Cb  = std::function<void(bool sel35)>;

    IWMDevice();

    /// Power-on reset. Mirrors MAME `iwm_device::device_reset` (iwm.cpp:48).
    void reset();

    /// CPU access at $C0E0-$C0EF. `offset` is the low nibble (0..15).
    /// The single-arg `read` matches MAME `iwm_device::read(offs_t)`;
    /// `write` matches `iwm_device::write(offs_t, u8)`.
    /// Both call `sync()` internally if a `nowCycles` was previously
    /// established via `tick(nowCycles)`.
    uint8_t read (uint8_t offset);
    void    write(uint8_t offset, uint8_t data);

    /// Associate the IWM with a backing 5.25" disk image and its
    /// currently-selected quarter-track. Mirrors MAME
    /// `iwm_device::set_floppy` (iwm.cpp:85) — POM2 splits the two
    /// floppy form factors into separate overloads because the flux-
    /// source backends speak different APIs (`DiskImage` for 5.25",
    /// `Sony35Drive` for 3.5"). Setting one clears the other.
    void setFloppy(DiskImage* disk, int qt);
    DiskImage* getFloppy() const { return disk_; }
    void       setQuarterTrack(int qt) { qt_ = qt; }

    /// 3.5" Sony drive variant. Called by `SmartPortHub::recalcActiveDevice`
    /// after MAME `apple2e.cpp:638-679 recalc_active_device` resolves
    /// the active floppy to one of the two Sony 3.5" drives. Passing
    /// nullptr returns the IWM to the 5.25" path.
    void setSony35(class Sony35Drive* drive);
    /// Detach the 3.5" drive WITHOUT touching the 5.25" binding.
    /// MAME ends `recalc_active_device` with an unconditional
    /// `m_iwm->set_floppy(m_cur_floppy)`, nullptr included; POM2 splits the
    /// two form factors across two setters, so the hub had no way to say
    /// "no 3.5" drive is selected" — `setSony35(nullptr)` would also drop
    /// `disk_`, which DiskIICard owns. Without this the IWM kept the LAST
    /// Sony attached while the hub routed to a 5.25" drive (or to nothing),
    /// and `flushWrite` spliced the burst into that drive's cells.
    void releaseSony35();
    Sony35Drive* getSony35() const { return sony_; }

    /// Advance the internal state machine up to the current CPU cycle
    /// timestamp. Mirrors MAME `iwm_device::sync` (iwm.cpp:335). The
    /// caller must invoke this on every $C0Ex access AND periodically
    /// during long idle stretches (the MAME timer fires once per
    /// `update_timer_tick` interval = 8 388 608 ticks at 1.023 MHz).
    void sync(uint64_t nowCycles);

    /// Refresh the "now" cycle and also run the state machine to that
    /// timestamp. Cheap when the IWM is fully idle (the `!active_`
    /// early-out in `sync` covers it). EmulationController calls this
    /// once per video frame so the 1-emulated-second drive-disable
    /// timer drain fires even when the //c+ alt firmware stops
    /// poking $C0Ex between disk operations.
    void tick(uint64_t nowCycles) { now_ = nowCycles; sync(nowCycles); }

    /// True iff the IWM is currently in MODE_ACTIVE (motor enabled +
    /// drive engaged). DiskIICard mirrors this when the //c+ profile
    /// is active so its motor-off spin-down stays in lock-step with
    /// the IWM's MODE_DELAY drain.
    bool isActive() const { return active_ == MODE_ACTIVE; }
    bool isIdle()   const { return active_ == MODE_IDLE; }

    /// While set, a data-register write is taken by an external SmartPort
    /// bus responder instead of the shifter: the access still moves Q6/Q7
    /// and the enable, so the chip's control state stays live, but no byte
    /// enters the write path — and so no "write underrun" is ever raised
    /// for traffic that was never meant for a disk. See SmartPortBusDevice.
    void setBusCapture(bool on) { busCapture_ = on; }

    /// Current cycle counter snapshot — last `tick()` value. Exposed so
    /// the SmartPort hub can stamp Sony 3.5" mechanical-sound events
    /// (head step, motor on/off) with an emulated-CPU timestamp the
    /// FloppySoundDevice can measure cadence against. Same role as
    /// `DiskIICard::cpuCycleTotal` on the 5.25" side.
    uint64_t emuCycles() const { return now_; }

    // ─── Snapshot / rewind ───────────────────────────────────────────────
    /// Serialize the IWM's registers, state machine and — critically —
    /// its **emuCycles timestamps**. Every one of `now_`, `lastSync_`,
    /// `nextStateChange_`, `syncUpdate_`, `asyncUpdate_`, `revStart35_`,
    /// `fluxWriteStart_` and `delayDeadline_` is an absolute CPU-cycle
    /// stamp. Leaving them out of the snapshot meant a rewind rolled the
    /// machine's `cycleCounter` backwards while the IWM kept its old,
    /// larger `lastSync_`; `sync()`'s `while (nextSync > lastSync_)` walker
    /// then did nothing and the IWM sat **frozen until emulated time
    /// caught back up** to where it had been. Reachable on every //c-class
    /// profile: `ioReadIWM` ticks the IWM on each $C0E0-$C0EF access even
    /// in shadow mode, where the 5.25" data itself comes from DiskIICard.
    ///
    /// `fluxWrite_` is 64 K entries (512 KB) for MAME parity, but only the
    /// first `fluxWriteCount_` are live — usually zero. Only those are
    /// written, or a single rewind ring would cost hundreds of megabytes.
    void appendSnapshotState(std::vector<uint8_t>& out) const;
    /// Returns false (and leaves the device untouched) on a truncated or
    /// unrecognised blob. `disk_`, `sony_` and the callbacks are host
    /// wiring — never serialized, re-installed by the caller.
    bool loadSnapshotState(const uint8_t* data, size_t n);

    /// Wire the MAME-style callbacks. `EmulationController` installs
    /// these once at construction; tests may install their own.
    void setPhasesCallback(PhasesCb cb) { phasesCb_ = std::move(cb); }
    void setDevselCallback(DevselCb cb) { devselCb_ = std::move(cb); }
    void setSel35Callback (Sel35Cb  cb) { sel35Cb_  = std::move(cb); }

    /// SEL bit (m_control bit 5) exposed to the host so 3.5" drives
    /// can fold it into their register-select bus. Mirrors MAME
    /// `iwm_device::sel_w` query (it lives inside `control` there;
    /// here we expose the snapshot for `Sony35Drive::setSel`).
    bool sel() const { return (control_ & 0x20) != 0; }

    /// 4-bit phase register snapshot (m_phases in MAME). Bits 0..3 =
    /// CA0/CA1/CA2/LSTRB on a 3.5" Sony drive.
    uint8_t phases() const { return phases_; }

    // Register accessors for inspectors / save state (`m_data`,
    // `m_mode`, etc. in MAME).
    uint8_t mode()    const { return mode_; }
    uint8_t status()  const { return status_; }
    uint8_t whd()     const { return whd_; }
    uint8_t control() const { return control_; }
    uint8_t data()    const { return data_; }

    /// The bit-cell window table MAME selects on mode bits 4-3
    /// (`iwm.cpp:303-329 window_size` / `half_window_size`), in IWM ticks.
    /// Pure functions of `mode_` + the Q3 clock flag. Public so the smoke
    /// test can diff all four rows against upstream: a slip in a row Apple
    /// firmware never selects (0x18 carried 18 instead of 16 until the
    /// 2026-09-07 audit) is invisible from every other angle.
    uint64_t windowSize()     const;                        // MAME line 317
    uint64_t halfWindowSize() const;                        // MAME line 303

private:
    // MAME `iwm.cpp:31-35` — active modes (m_active) and r/w modes
    // (m_rw) packed into one enum with a partition.
    enum {
        MODE_IDLE,                    // m_active = idle  OR  m_rw = idle
        MODE_ACTIVE, MODE_DELAY,      // m_active sub-states
        MODE_READ,   MODE_WRITE,      // m_rw sub-states
    };

    // MAME `iwm.cpp:38-46` — state-machine states for the bit-cell
    // window walker. Used by both read (SR_*) and write (SW_*) paths.
    enum {
        S_IDLE,
        SR_WINDOW_EDGE_0,
        SR_WINDOW_EDGE_1,
        SW_WINDOW_LOAD,
        SW_WINDOW_MIDDLE,
        SW_WINDOW_END,
        SW_UNDERRUN,
    };

    DiskImage*    disk_  = nullptr;
    Sony35Drive*  sony_  = nullptr;
    int           qt_    = 0;        // active quarter-track (5.25")
    uint64_t      now_   = 0;        // last `tick()` timestamp

    /// Revolution anchor cycle for 3.5" Sony bit-cell time → CPU cycle
    /// mapping. Set to the moment motor spin-up completed (= the cycle
    /// at which cell 0 was nominally under the head). MAME tracks this
    /// in `floppy_image_device::m_revolution_start_time`. POM2 latches
    /// it on the `MODE_IDLE → MODE_ACTIVE` transition.
    uint64_t      revStart35_ = 0;

    // Time-tracking state (all in CPU cycles). Mirrors MAME's
    // `m_last_sync`, `m_next_state_change`, `m_sync_update`,
    // `m_async_update`, `m_flux_write_start`.
    uint64_t lastSync_         = 0;
    uint64_t nextStateChange_  = 0;
    uint64_t syncUpdate_       = 0;
    uint64_t asyncUpdate_      = 0;
    uint64_t fluxWriteStart_   = 0;

    // Write buffer. MAME uses `std::array<u64, 65536> m_flux_write` — a
    // 64 K-entry window so the IWM can stash an entire track's worth
    // of flux events before the host code calls `flush_write`. POM2
    // reuses the same upper bound for parity.
    std::array<uint64_t, 65536> fluxWrite_{};
    uint32_t                    fluxWriteCount_ = 0;

    // Q3 clock support. The //c / //c+ leaves this at 0 (no fast
    // clock), so we keep the field but treat `q3ClockActive_=false`
    // throughout.
    uint32_t q3Clock_       = 0;
    bool     q3ClockActive_ = false;

    // Drive-disable delay deadline (CPU cycle). MAME schedules an
    // `emu_timer` for `cycles_to_time(8388608)` when the motor enable
    // bit drops in non-timer mode (`iwm.cpp:202-206`); the timer's
    // expiry runs `update_timer_tick` (line 70-84) which:
    //   * flushes any pending write events
    //   * drops m_active = MODE_IDLE
    //   * clears m_status bit 5 and m_whd bit 6
    //   * deasserts m_floppy->mon_w
    // POM2 doesn't run a real timer; instead `sync(now)` checks this
    // deadline on every entry and runs the same drain when reached.
    // EmulationController also calls `tick(now)` once per video frame
    // so the drain fires even if no further $C0Ex accesses arrive.
    // Zero means "no delay pending" (active is ACTIVE or IDLE).
    uint64_t delayDeadline_ = 0;

    // Mode / read-or-write / state-machine state.
    int active_  = MODE_IDLE;
    int rw_      = MODE_IDLE;
    int rwState_ = S_IDLE;

    // Register set (MAME `iwm.cpp:55-65`).
    uint8_t data_      = 0x00;
    uint8_t whd_       = 0xBF;        // cold value per MAME line 57
    uint8_t mode_      = 0x00;
    uint8_t status_    = 0x00;
    uint8_t control_   = 0x00;
    bool    busCapture_ = false;
    uint8_t rwBitCount_ = 0;
    uint8_t rsh_       = 0x00;
    uint8_t wsh_       = 0x00;
    uint8_t devsel_    = 0;
    uint8_t phases_    = 0;

    // Set by dataW() when the CPU loads a fresh byte while we are in
    // MODE_WRITE. Cleared on every MODE_WRITE entry. Lets the FSM tell
    // a real mid-write underrun (CPU was loading bytes then ran out)
    // apart from the spurious-on-entry case where the firmware probes
    // the IWM in write mode without intending to write — the latter
    // would otherwise fire warn() on every boot/media-change because
    // whd_'s cold value (0xBF) has bit 7 set.
    bool    writeDataLoaded_ = false;

    /// Host callbacks — set by the SmartPort hub. Lazy nullptr-check
    /// at fire site keeps the hot path branch-free when unwired (the
    /// SmartPort hub is only attached on //c+ profiles).
    PhasesCb phasesCb_;
    DevselCb devselCb_;
    Sel35Cb  sel35Cb_;

    // ── Internal helpers (MAME line refs in the .cpp port) ──────────

    void controlAccess(int offset, uint8_t data);
    void modeW (uint8_t data);
    void dataW (uint8_t data);
    void flushWrite(uint64_t when = 0);
    void writeClockStart();
    void writeClockStop();

    /// Propagate the IWM motor-enable line to the active floppy
    /// (`m_floppy->mon_w(motorOff)` in MAME). For 5.25" `DiskImage*`
    /// this is currently a no-op — DiskIICard owns the 5.25" motor +
    /// audio + spin-down delay. For 3.5" `Sony35Drive*` it fires
    /// `Sony35Drive::monW`, which is the only authoritative motor
    /// state for the Sony stack (the strobe-register motor commands
    /// the //c+ alt firmware also issues run on top of this).
    /// `motorOff` follows MAME's polarity: true = motor stopped,
    /// false = motor running.
    void notifyMonW(bool motorOff);

    /// Fire the host devsel callback with `value`. Cached so we only
    /// fire the callback when devsel transitions. MAME-faithful
    /// timing requires firing at three extra moments POM2 originally
    /// missed: device_reset (cb(0)), MODE_DELAY entry in non-timer
    /// mode (cb(1 or 2)), and update_timer_tick exit (cb(0)).
    void fireDevsel(uint8_t value);

    bool     isSync() const { return !(mode_ & 0x02); }     // MAME line 286
    uint64_t readRegisterUpdateDelay() const;               // MAME line 331

    // POM2-specific: fetch the next flux transition cycle from the
    // backing disk image. `from` is exclusive (we want a transition
    // strictly after `from`). Falls back to noiseTransition() when no
    // real media transition is pending so an empty spinning drive still
    // feeds the read SR (see noiseTransition).
    int64_t nextTransition(int64_t from) const;

    // Synthesize a noise-flux transition for an empty/unformatted track so
    // the firmware boot read-loop keeps assembling garbage bytes (byte-ready
    // asserts) instead of spinning forever. Deterministic in `from`.
    int64_t noiseTransition(int64_t from) const;
};

}  // namespace pom2

#endif // POM2_IWM_DEVICE_H
