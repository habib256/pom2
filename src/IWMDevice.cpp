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

// Port of MAME `src/devices/machine/iwm.cpp` (`iwm_device`).
// Line refs throughout point back at the MAME source so future
// updates can be diff-checked. The MAME source-of-truth header is
// `/tmp/iwm.txt` in this checkout (a snapshot fetched via scrapling).
//
// Adaptations from MAME:
//   * `attotime` / `machine().time()` → POM2's CPU cycle counter
//     (`uint64_t`, advanced by the caller via `tick(nowCycles)`).
//   * `emu_timer` → POM2 has no timer subsystem, so the 1-emu-second
//     drive-disable delay (MAME `iwm.cpp:83-97 update_timer_tick`)
//     runs inline from `sync()` when the recorded `delayDeadline_`
//     is reached. EmulationController pulses `tick()` every video
//     frame so the deadline still fires when no $C0Ex traffic
//     arrives between operations.
//   * `floppy_image_device::get_next_transition(attotime)` →
//     `DiskImage::getNextTransition(qt, fromLssCycle)` for 5.25",
//     `Sony35Drive::nextTransition` for 3.5".
//   * `floppy_image_device::write_flux(start, end, count, transitions)` →
//     `DiskImage::writeFlux` / `Sony35Drive::writeFlux`.
//   * `m_devsel_cb` (host notifies drive select) → `devselCb_` fired
//     by `fireDevsel()` at MAME-faithful times: reset(0), MODE_DELAY
//     entry (1 or 2 before the timer), MODE_DELAY drain (0), and on
//     steady-state SEL transitions while active.
//   * `m_floppy->mon_w(motorOff)` → `notifyMonW(motorOff)`. For 5.25"
//     this is a no-op (DiskIICard owns motor + spin-down + audio for
//     the Disk II path). For 3.5" it fires `Sony35Drive::monW` so
//     the Sony stack's motorOn_ tracks the IWM as the master.
//
// Intentionally divergent (POM2-only):
//   * `read()` doesn't gate `control()` on `machine().side_effects_
//     disabled()` because POM2's debug surface (Memory viewer) reads
//     RAM directly and never goes through soft switches → no caller
//     needs a side-effect-free $C0Ex read today.
//   * Window sizes are scaled from MAME's IWM-clock ticks to POM2's
//     CPU-clock ticks (÷7 since //c+ runs the IWM off A2BUS_7M while
//     POM2 keeps a single cycle counter). See `windowSize` /
//     `halfWindowSize` / `readRegisterUpdateDelay` for the rounding
//     choices.
//
// Not yet ported (groundwork for a follow-up pass):
//   * `applefdintf_device::device_start/reset` base members.
//   * The Q3 fast clock (1.86 MHz) used on Mac/IIgs but not //c+.
//   * Full `set_write_splice` handling — the call site fires but
//     `DiskImage::setWriteSplice` is still a stub (TODO.md «WOZ1
//     splice point (TRK +6650) ignoré»).

#include "IWMDevice.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "CpuClock.h"
#include "Logger.h"
#include "Sony35Drive.h"

#include <algorithm>
#include <climits>
#include <cstdio>

namespace {

// MAME `iwm.cpp:204-206`: 1 << 23 = 8388608 ticks of the IWM's CLOCK —
// which on //c-class machines is the 7 MHz bus clock (`A2BUS_7M_CLOCK`,
// MAME apple2e.cpp IWM instantiation), NOT the CPU clock. 8388608 / 7 M
// ≈ 1.17 s; POM2 ticks IWM time in CPU cycles, so scale by CPU/7M (= 1/7
// of the 7M tick count). The previous "1 CPU-second" constant ran the
// spin-down ~15% short (and its comment claimed the IWM clock was the
// CPU clock, which POM2's own ÷7 window scaling elsewhere contradicts).
constexpr uint64_t kDriveDisableDelayCycles =
    (8388608ull * POM2_CPU_CLOCK_HZ) / (7159090ull);

}

namespace pom2 {

IWMDevice::IWMDevice()
{
    reset();
}

void IWMDevice::reset()
{
    // MAME `iwm.cpp:59-81` device_reset.
    lastSync_         = now_ * POM2_IWM_TICKS_PER_CPU_CYCLE;
    nextStateChange_  = 0;
    active_           = MODE_IDLE;
    rw_               = MODE_IDLE;
    rwState_          = S_IDLE;
    data_             = 0x00;
    whd_              = 0xBF;
    mode_             = 0x00;
    status_           = 0x00;
    control_          = 0x00;
    wsh_              = 0x00;
    rsh_              = 0x00;
    fluxWriteStart_   = 0;
    fluxWriteCount_   = 0;
    rwBitCount_       = 0;
    phases_           = 0;
    writeDataLoaded_  = false;
    q3ClockActive_    = false;
    syncUpdate_       = 0;
    asyncUpdate_      = 0;
    // MAME `iwm.cpp:78-79` fires `m_devsel_cb(0)` once during reset
    // so the host hub can drop any latched device-select state. POM2
    // mirrors via `fireDevsel(0)` (idempotent if already 0).
    fireDevsel(0);
    // The phase lines were just dropped too — tell the host hub, or a
    // Sony drive keeps acting on the pre-reset CA0-CA2/LSTRB levels.
    if (phasesCb_) phasesCb_(phases_);
}

void IWMDevice::fireDevsel(uint8_t value)
{
    if (devsel_ != value) {
        devsel_ = value;
        if (devselCb_) devselCb_(devsel_);
    }
}

void IWMDevice::notifyMonW(bool motorOff)
{
    if (sony_) {
        sony_->monW(motorOff);
    }
    // 5.25" DiskImage path: DiskIICard's motor + spin-down + audio
    // wiring is the source of truth (Theme 6 audit). The IWM here is
    // a shadow on //c+ (read path authoritative when iwmAuthoritative
    // is set), so we intentionally don't propagate mon_w to the disk
    // image — DiskIICard fires its own FloppySoundSink::motor on the
    // same $C0E8/$C0E9 access that drove this controlAccess() call.
}

void IWMDevice::setFloppy(DiskImage* disk, int qt)
{
    // MAME `iwm.cpp:99-115 set_floppy`. When the active floppy changes
    // while the motor is enabled, MAME drops mon_w on the OLD drive
    // then raises mon_w on the NEW drive — i.e. the new drive sees the
    // motor come on as part of the rebind. POM2 mirrors this for the
    // 3.5" Sony path (DiskIICard owns 5.25" motor sound).
    if (disk_ == disk && qt_ == qt && !sony_) return;
    sync(now_);
    flushWrite();
    const bool motorOn = (control_ & 0x10) != 0;
    if (motorOn) notifyMonW(true);     // stop old (if any 3.5" was active)
    disk_ = disk;
    qt_   = qt;
    sony_ = nullptr;                   // routing back to 5.25" path
    // No mon_w(false) here — the 5.25" Disk II path uses DiskIICard's
    // motor wiring exclusively.
}

void IWMDevice::setSony35(Sony35Drive* drive)
{
    if (sony_ == drive && !disk_) return;
    sync(now_);
    flushWrite();
    const bool motorOn = (control_ & 0x10) != 0;
    if (motorOn) notifyMonW(true);     // stop old drive's motor (Sony if any)
    sony_ = drive;
    disk_ = nullptr;
    if (sony_) {
        // Anchor the revolution: the freshly-attached drive's cell 0
        // is under the head at "now". MAME re-anchors on `mon_w(false)`
        // (motor spin-up); POM2 collapses both events to the moment
        // the SmartPort hub points the IWM at this drive.
        revStart35_ = now_;
        sony_->invalidateCache();
        if (motorOn) notifyMonW(false); // raise mon_w on new drive
    }
}

uint8_t IWMDevice::read(uint8_t offset)
{
    // MAME `iwm.cpp:103-114 read`. The `!machine().side_effects_disabled()`
    // guard there protects debugger peeks; POM2 doesn't expose a
    // side-effect-disabled read yet, so we always run `control()`.
    controlAccess(offset & 0xF, 0x00);
    switch (control_ & 0xC0) {
        case 0x00: return active_ ? data_ : 0xFF;
        case 0x40: {
            // (status & 0x7F) | wpt. MAME `iwm.cpp:107` reads
            // `m_floppy->wpt_r()`. For 3.5" Sony drives this is the
            // SENSE line on the currently-selected register (see
            // Sony35Drive::senseR comment block) — the //c+ firmware
            // probes /WPT, /TRACK0, /INSERTED, etc. via this same bit.
            // MAME `iwm.cpp:129`:
            //   (m_status & 0x7f) | ((!m_floppy || m_floppy->wpt_r()) ? 0x80 : 0)
            // — with NO selected floppy the SENSE line reads HIGH. POM2's
            // disk_ is permanently attached by DiskIICard, so it must be
            // gated on a drive actually being enabled (devsel != 0):
            // the //c+ firmware's boot drive-scan polls status with
            // devsel=0 and waited forever on the 5.25" image's
            // write-protect bit (writable disk → 0 → $F0FF BPL loop,
            // no banner, no boot).
            bool wpt;
            const char* src;
            if (sony_)      { wpt = sony_->senseR();           src = "sony"; }
            else if (disk_ && devsel_ != 0)
                            { wpt = disk_->isWriteProtected(); src = "disk525"; }
            else            { wpt = true;                      src = "none"; }
            static const bool trace =
                std::getenv("POM2_TRACE_IWM_SENSE") != nullptr;
            if (trace) {
                static int lastKey = -1;
                const int key = (wpt ? 1 : 0) | (sony_ ? 2 : 0) | (disk_ ? 4 : 0);
                if (key != lastKey) {
                    std::fprintf(stderr,
                                 "[IWMST] src=%s wpt=%d control=%02X devsel=%d\n",
                                 src, wpt ? 1 : 0, control_, devsel_);
                    lastKey = key;
                }
            }
            return static_cast<uint8_t>((status_ & 0x7F) | (wpt ? 0x80 : 0x00));
        }
        case 0x80: return whd_;
        case 0xC0: return 0xFF;
    }
    return 0xFF;     // unreachable
}

void IWMDevice::write(uint8_t offset, uint8_t data)
{
    // MAME `iwm.cpp:115-118 write`.
    controlAccess(offset & 0xF, data);
}

void IWMDevice::flushWrite(uint64_t when)
{
    // MAME `iwm.cpp:119-143 flush_write`. Slice the buffered transitions
    // into the backing disk image, leaving the bit-cell write splice
    // pinned at the next event.
    if (!fluxWriteStart_) return;
    if (!when) when = lastSync_;
    if (when > fluxWriteStart_) {
        bool lastOnEdge = (fluxWriteCount_ > 0 &&
                           fluxWrite_[fluxWriteCount_ - 1] == when);
        if (lastOnEdge) --fluxWriteCount_;
        // The flux transition values in MAME live in `attotime`; POM2
        // already speaks raw cycle counts at the DiskImage boundary, so
        // we pass them through as-is.
        // 5.25" (disk_): NO flux write-back from this device — ever.
        // MAME's IWM is the only controller on its bus, but POM2 wires
        // the slot-6 DiskIICard in parallel on //c+ (Memory::memWrite
        // feeds $C0Ex to ioWriteIWM AND slots.deviceSelectWrite), and
        // DiskIICard's LSS is the 5.25" write authority everywhere else
        // in POM2. Both state machines pushing flux into the same
        // DiskImage double-wrote every 5.25" sector on the //c+ (the
        // dual-controller hazard from the 2026-07 bug hunt). The IWM's
        // write handshake state (whd_, MODE_WRITE) still runs so the
        // firmware's probes behave; only the flux landing is suppressed.
        // The 3.5" Sony path below IS this device's to write — no other
        // controller sees those accesses.
        if (sony_) {
            // 3.5" Sony write-back. Sony35Drive's writeFlux splices
            // the new transitions into its cached cell stream, then
            // runs MAME's `extract_sectors_from_track_mac_gcr6`
            // decoder over the result and pushes any complete
            // sector's 512-byte payload back into the attached
            // Disk35Image. Write protection is enforced inside
            // writeFlux (early-return on `isWriteProtected()`).
            std::vector<int64_t> fluxes;
            fluxes.reserve(fluxWriteCount_);
            for (uint32_t i = 0; i < fluxWriteCount_; ++i) {
                fluxes.push_back(static_cast<int64_t>(fluxWrite_[i]));
            }
            // Every timestamp here is already in IWM ticks except the
            // revolution anchor, which is latched from the host clock.
            sony_->writeFlux(static_cast<int64_t>(fluxWriteStart_),
                             static_cast<int64_t>(when),
                             fluxes.empty() ? nullptr : fluxes.data(),
                             static_cast<int>(fluxes.size()),
                             static_cast<int64_t>(revStart35_) *
                                 POM2_IWM_TICKS_PER_CPU_CYCLE);
        }
        fluxWriteCount_ = 0;
        if (lastOnEdge) {
            fluxWrite_[fluxWriteCount_++] = when;
        }
        fluxWriteStart_ = when;
    } else {
        fluxWriteCount_ = 0;
    }
}

void IWMDevice::controlAccess(int offset, uint8_t data)
{
    // MAME `iwm.cpp:144-254 control`. Long function — split into:
    //   1. Phase 0-3 vs control-bit update (low 8 vs 8-15 offsets)
    //   2. Active state transition (motor-enable bit 4 of m_control)
    //   3. Read/write transition (Q7 bit 7 of m_control)
    //   4. Async-update scheduling for read-side timing
    //   5. mode_w / data_w dispatch on Q7H+Q6H+odd offset writes
    //
    // The phases (low 4 offsets) drive the head stepper on a real
    // Disk II. POM2's DiskIICard already owns the head — we keep the
    // raw bit tracking here so the IWM's `m_phases` mirrors MAME, but
    // don't re-invoke the stepper here (DiskIICard does it).

    sync(now_);
    if (offset < 8) {
        // Phases (offset 0..7 → phase bits 0..3, even=clear odd=set).
        // POM2's DiskIICard handles the 5.25" head-stepper effect of
        // these; in addition, we forward the 4-bit pattern to the
        // host-installed `phasesCb_` so a Sony 3.5" drive (the //c+
        // SmartPort target) can interpret them as a CA0/CA1/CA2/LSTRB
        // command bus. MAME `iwm.cpp:147-152` updates `m_phases` then
        // calls `update_phases()` which in turn fires `phases_cb`.
        const uint8_t bit = static_cast<uint8_t>(1u << ((offset >> 1) & 3));
        const uint8_t prev = phases_;
        if (offset & 1) phases_ |=  bit;
        else            phases_ &= ~bit;
        if (phases_ != prev && phasesCb_) phasesCb_(phases_);
        (void)data;
    } else {
        const uint8_t prevControl = control_;
        if (offset & 1) control_ |=  (1u << ((offset >> 1) & 7));
        else            control_ &= ~(1u << ((offset >> 1) & 7));
        // SEL (bit 5) is exposed to the host so 3.5" drives can fold
        // it into their register-select bus on the next phase strobe.
        // MAME `iwm.cpp` doesn't expose SEL via a dedicated callback —
        // it's read by `m_floppy` on every `seek_phase_w` from the
        // host's `update_phases()`. POM2 mirrors the same effect by
        // notifying `phasesCb_` whenever SEL transitions, so the
        // active 3.5" drive can re-evaluate its sense address.
        if (((prevControl ^ control_) & 0x20) && phasesCb_) {
            phasesCb_(phases_);
        }
    }

    // Activate / deactivate based on m_control bit 4 (motor enable).
    // MAME line 190-241.
    {
        // POM2_TRACE_IWM_SENSE=1 also logs drive-enable edges — pairs
        // with the [SENSE]/[IWMST] lines to show whether a firmware
        // wait-for-spin-down can ever terminate.
        static const bool traceMot =
            std::getenv("POM2_TRACE_IWM_SENSE") != nullptr;
        const bool wantOn = (control_ & 0x10) != 0;
        const bool isOn   = active_ == MODE_ACTIVE;
        if (traceMot && wantOn != isOn)
            std::fprintf(stderr, "[IWMMODE] enable %s now=%llu mode=%02X\n",
                         wantOn ? "ON " : "OFF",
                         static_cast<unsigned long long>(now_), mode_);
    }
    if (control_ & 0x10) {
        if (active_ != MODE_ACTIVE) {
            active_      = MODE_ACTIVE;
            status_     |= 0x20;
            // MAME `iwm.cpp:194-195`: `m_floppy->mon_w(false)`. For 5.25"
            // DiskIICard fires the motor sample on its own $C0E9 path
            // and stays source of truth; the 3.5" Sony drive needs the
            // signal forwarded so its motorOn_ tracks the IWM rather
            // than depending on strobe-register motor commands alone.
            notifyMonW(false);
        }
        if ((control_ & 0x80) == 0x00) {
            // Q7 = 0 → read mode
            if (rw_ != MODE_READ) {
                if (rw_ == MODE_WRITE) {
                    flushWrite();
                    writeClockStop();
                }
                rw_              = MODE_READ;
                rwState_         = S_IDLE;
                nextStateChange_ = 0;
                syncUpdate_      = 0;
                asyncUpdate_     = 0;
                data_            = 0x00;
            }
        } else {
            // Q7 = 1 → write mode
            if (rw_ != MODE_WRITE) {
                rw_              = MODE_WRITE;
                rwState_         = S_IDLE;
                whd_            |= 0x40;
                nextStateChange_ = 0;
                writeDataLoaded_ = false;
                writeClockStart();
                // MAME `iwm.cpp:218-221`:
                //   m_floppy->set_write_splice(
                //       cycles_to_time(m_flux_write_start));
                // The splice position pins the bit cell where a WOZ
                // re-master should start writing — Applesauce uses it
                // to keep round-trip parity. POM2's DiskImage exposes
                // a stub `setWriteSplice` (see DiskImage.h); see
                // TODO.md's WOZ1 splice-point entry (TRK +6650) for
                // the full plumbing. Call site is here so the day the
                // stub gets a body, the splice arrives at the right
                // moment automatically. No-op on Sony35Drive — 3.5"
                // images don't carry a splice position.
                if (disk_) {
                    // DiskImage speaks LSS cycles (2 per CPU cycle);
                    // fluxWriteStart_ is in IWM ticks (7 per CPU cycle).
                    disk_->setWriteSplice(qt_,
                        (static_cast<int64_t>(fluxWriteStart_) * 2) /
                            POM2_IWM_TICKS_PER_CPU_CYCLE);
                }
            }
        }
    } else {
        if (active_ == MODE_ACTIVE) {
            flushWrite();
            if (mode_ & 0x04) {
                // Timer mode: drop immediately to idle (MAME line
                // 226-234). `m_floppy->mon_w(true)` is fired here.
                writeClockStop();
                active_   = MODE_IDLE;
                rw_       = MODE_IDLE;
                rwState_  = S_IDLE;
                status_  &= ~0x20;
                whd_     &= ~0x40;
                notifyMonW(true);
            } else {
                // Normal mode: 1 emulated-second drive-disable delay.
                // MAME `iwm.cpp:235-239` schedules an emu_timer for
                // `cycles_to_time(8388608)` and fires `m_devsel_cb`
                // with the current drive number BEFORE the timer
                // fires (so the host hub can pre-emptively recalc the
                // active device while the motor coasts to a stop).
                // POM2 records a deadline; `sync()` checks it on
                // entry and runs the drain when reached. The drive-
                // disable fire of `m_floppy->mon_w(true)` happens
                // when the timer expires (see sync()), NOT here.
                fireDevsel(static_cast<uint8_t>(
                    (control_ & 0x20) ? 2 : 1));
                active_         = MODE_DELAY;
                delayDeadline_  = now_ + kDriveDisableDelayCycles;
            }
        }
    }

    // Steady-state devsel update (MAME line 243-247). Captures motor-
    // active drive-select transitions that aren't covered by the
    // MODE_DELAY-entry fire above (e.g. SEL bit flip while still
    // spinning).
    const uint8_t newDevsel = (active_ != MODE_IDLE)
        ? ((control_ & 0x20) ? 2 : 1)
        : 0;
    fireDevsel(newDevsel);

    // Read-side state reset (MAME line 214-215).
    if ((control_ & 0xC0) == 0x40 &&
        active_ == MODE_ACTIVE && rw_ == MODE_READ) {
        rsh_ = 0;
    }

    // Asynchronous mode update scheduling (MAME line 246-247). `14` is in
    // IWM ticks — half a default 28-tick window — which is now the unit the
    // FSM runs in, so it is MAME's constant unchanged. (It survived the
    // CPU-cycle era by accident: left raw at 14 it fired 7× late, then
    // "fixed" to 2, and neither was the hardware's number.)
    if (active_ && !(control_ & 0x80) && !isSync() && (data_ & 0x80)) {
        asyncUpdate_ = lastSync_ + 14;
    }

    // Mode register / data register write (MAME line 248-254).
    if ((control_ & 0xC0) == 0xC0 && (offset & 1)) {
        if (active_) { if (!busCapture_) dataW(data); }
        else         modeW(data);
    }
}

void IWMDevice::modeW(uint8_t data)
{
    // MAME `iwm.cpp:256-269 mode_w`.
    mode_   = data;
    status_ = static_cast<uint8_t>((status_ & 0xE0) | (data & 0x1F));
}

void IWMDevice::dataW(uint8_t data)
{
    // MAME `iwm.cpp:311-318 data_w`. Three side effects in order:
    //   1. Always latch the data byte (visible via $C0nF read once the
    //      controller is back in MODE_IDLE).
    //   2. In sync write mode, mirror the byte into the write shift
    //      register IMMEDIATELY (the FSM also copies data_ into wsh_ at
    //      SW_WINDOW_LOAD, but a CPU write that lands between two cells
    //      should be seen by the next bit-out without an extra round
    //      trip — MAME parity).
    //   3. If "latched handshake" mode is selected (mode bit 0 = 1),
    //      clear WHD bit 7 to signal "data loaded". When that mode bit
    //      is 0, the IWM does NOT auto-clear the handshake — the CPU is
    //      expected to use a different write-pacing protocol. (This is
    //      the gate POM2 originally got wrong: it cleared whd bit 7 on
    //      every sync+write data_w regardless of mode bit 0, which
    //      ignored the mode register entirely.)
    data_ = data;
    if (isSync() && rw_ == MODE_WRITE) {
        wsh_ = data;
    }
    if (mode_ & 0x01) {
        whd_ &= 0x7F;
        writeDataLoaded_ = true;
    }
}

// MAME's IWM window sizes are in IWM-clock ticks (the //c / //c+ runs
// the IWM off A2BUS_7M ≈ 7.16 MHz — see `apple2e.cpp` machine config).
// POM2 ticks the IWM with the CPU clock (POM2_CPU_CLOCK_HZ ≈ 1.023
// MHz) to keep one cycle counter for the whole machine. Scale the
// MAME constants by the clock ratio (≈ 7) so a "bit cell" window
// still spans ≈ 4 µs of emulated time, which is what GCR-encoded
// 5.25" flux transitions assume. The constants below preserve MAME's
// relative ratios across the four mode-bit-4-3 combinations.

uint64_t IWMDevice::halfWindowSize() const
{
    // MAME `iwm.cpp:290-301 half_window_size`, VERBATIM — the state machine
    // is clocked in IWM ticks now (7 per CPU cycle, CpuClock.h), which is
    // the unit these numbers were always in. They used to be divided by 7
    // and rounded, which collapsed 14/16 to 2 and 7/8 to 1: two of the four
    // window settings became indistinguishable, and none of them could place
    // an edge inside a 14.2-tick Sony cell.
    if (q3ClockActive_) {
        return (mode_ & 0x08) ? 2 : 4;
    }
    switch (mode_ & 0x18) {
        case 0x00: return 14;
        case 0x08: return 7;
        case 0x10: return 16;
        case 0x18: return 8;
    }
    return 14;
}

uint64_t IWMDevice::windowSize() const
{
    // MAME `iwm.cpp:302-313 window_size`, VERBATIM. See halfWindowSize.
    if (q3ClockActive_) {
        return (mode_ & 0x08) ? 4 : 8;
    }
    switch (mode_ & 0x18) {
        case 0x00: return 28;
        case 0x08: return 14;
        case 0x10: return 36;
        case 0x18: return 18;
    }
    return 28;
}

uint64_t IWMDevice::readRegisterUpdateDelay() const
{
    // MAME `iwm.cpp:363-366`: 4 IWM ticks when mode bit 3 is set, 8
    // otherwise. Both used to be rounded up into 1 and 2 CPU cycles, which
    // is a 14 % and a 75 % error on a sub-cycle delay; in ticks they are
    // simply themselves.
    return (mode_ & 0x08) ? 4 : 8;
}

void IWMDevice::writeClockStart()
{
    // MAME `iwm.cpp:318-326`. With Q3 inactive on //c+, just records
    // the splice cycle.
    if (isSync() && q3Clock_) {
        q3ClockActive_ = true;
        lastSync_      = now_ * POM2_IWM_TICKS_PER_CPU_CYCLE;
    }
    fluxWriteStart_ = lastSync_;
    fluxWriteCount_ = 0;
}

void IWMDevice::writeClockStop()
{
    // MAME `iwm.cpp:327-334`.
    if (q3ClockActive_) {
        q3ClockActive_ = false;
        lastSync_      = now_ * POM2_IWM_TICKS_PER_CPU_CYCLE;
    }
    fluxWriteStart_ = 0;
}

int64_t IWMDevice::nextTransition(int64_t from) const
{
    // `from` and the return value are IWM ticks. Dispatch by form factor,
    // and convert at each backend's own boundary:
    //  * 3.5" Sony: `Sony35Drive` speaks the same tick timeline (that is
    //    what makes a 2 µs cell 14.2 units wide instead of 2). Only the
    //    revolution anchor has to be scaled, since it is latched from the
    //    host's CPU-cycle clock.
    //  * 5.25" Disk II: `DiskImage`'s flux events live in LSS-cycle space
    //    (= 2× CPU cycles) — see DiskIICard `lssCycle = cpuCycleTotal * 2`.
    //    So an LSS cycle is 3.5 ticks. The halving is exact in the
    //    tick→LSS direction (7 ticks = 2 LSS cycles); the return trip
    //    rounds down by at most half a tick, which is 1/224 of a 5.25"
    //    bit cell.
    if (sony_) {
        const int64_t t = sony_->nextTransition(
            from, static_cast<int64_t>(revStart35_) *
                      POM2_IWM_TICKS_PER_CPU_CYCLE);
        if (t != INT64_MAX) return t;
        return noiseTransition(from);
    }
    if (disk_) {
        const int64_t fromLss = (from * 2) / POM2_IWM_TICKS_PER_CPU_CYCLE;
        const int64_t t = disk_->getNextTransition(qt_, fromLss);
        if (t != DiskImage::kFluxNever)
            return (t * POM2_IWM_TICKS_PER_CPU_CYCLE) / 2;
    }
    return noiseTransition(from);
}

int64_t IWMDevice::noiseTransition(int64_t from) const
{
    // No media (or an unformatted track) — but the head is parked over a
    // spinning, empty drive. A real Apple drive feeds the IWM a stream of
    // noise flux in this state, so the read shift register keeps assembling
    // garbage bytes with bit-7 ("byte ready") set. The boot firmware relies
    // on that: its wait-for-byte loop ($C0EC bit 7) must keep advancing so
    // the per-read retry counter can drain and the machine can fall through
    // to its "no disk" path (//c "Check Disk Drive", //c+ "UNABLE TO FIND A
    // BOOTABLE DISK ONLINE."). If we instead returned INT64_MAX the FSM only
    // ever shifts in 0-bits, bit-7 never asserts, and the firmware spins
    // forever on the un-cleared power-up screen. MAME models this implicitly:
    // the floppy reports no transition but the IWM's window timer still
    // cycles the SR; POM2 collapsed that timer into nextTransition(), so the
    // garbage has to be injected here.
    const uint64_t w = windowSize() ? windowSize() : 1;
    // Deterministic LCG keyed on the window index: reproducible for tests,
    // yet straddles window boundaries so the SR accumulates a mix of 1s and
    // 0s (rsh_ reaches 0x80 within a few windows -> byte ready).
    uint64_t s = static_cast<uint64_t>(from) / w;
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const int64_t span = static_cast<int64_t>(w + 1 + ((s >> 33) % (2 * w + 1)));
    return from + span;
}

void IWMDevice::sync(uint64_t nowCycles)
{
    // MAME `iwm.cpp:335-480 sync`. Verbatim port of the read/write
    // state-machine walker. The read side rebuilds the SR data byte
    // by sliding a window over flux transitions from the disk image;
    // the write side drains the CPU's loaded data byte into the IWM
    // shift register, scheduling flux events into `fluxWrite_`.

    // Drive-disable delay drain (MAME `iwm.cpp:83-97 update_timer_tick`).
    // The MAME implementation runs this from an emu_timer at the
    // scheduled deadline; POM2 has no timer system so we check the
    // deadline at every sync() entry instead. Idempotent: zeroing
    // `delayDeadline_` after the transition stops it from re-firing.
    //
    // Side effects (in MAME order, line 86-95):
    //   * flush_write — drain any pending flux events
    //   * m_active = MODE_IDLE; m_rw = MODE_IDLE; m_rw_state = S_IDLE
    //   * m_floppy->mon_w(true)  — motor off, real on the 3.5" Sony
    //                              path; 5.25" Disk II spin-down is
    //                              owned by DiskIICard (see notifyMonW)
    //   * m_devsel_cb(0); m_devsel = 0
    //   * m_status &= ~0x20; m_whd &= ~0x40
    if (active_ == MODE_DELAY &&
        delayDeadline_ != 0 &&
        nowCycles >= delayDeadline_) {
        flushWrite();
        active_     = MODE_IDLE;
        rw_         = MODE_IDLE;
        rwState_    = S_IDLE;
        notifyMonW(true);
        fireDevsel(0);
        status_    &= ~0x20;
        whd_       &= ~0x40;
        delayDeadline_ = 0;
    }
    if (!active_) return;
    // From here down the clock is IWM TICKS, seven per CPU cycle
    // (CpuClock.h). Everything crossing back out — the delay deadline
    // above, `now_`, the snapshot — stays in CPU cycles.
    const uint64_t nextSync = nowCycles * POM2_IWM_TICKS_PER_CPU_CYCLE;
    switch (rw_) {
        case MODE_IDLE:
            lastSync_ = nextSync;
            break;

        case MODE_READ: {
            int64_t nextFluxChange = 0;
            while (nextSync > lastSync_) {
                if (nextFluxChange <= static_cast<int64_t>(lastSync_)) {
                    // MAME asks the floppy for the first transition strictly
                    // after `m_last_sync` — not after `m_last_sync + 1`. The
                    // extra tick POM2 used to add silently DROPPED any flux
                    // landing exactly one tick past the last sync point, and
                    // that is not a rare alignment: `lastSync_` is parked on
                    // the caller's poll boundary every time a sync() call runs
                    // out of time mid-window, so it lands one tick short of a
                    // transition regularly. `nextFluxChange` is a local reset
                    // to 0 on every entry, so the recompute happens on every
                    // call and the same transition is skipped for good — a 1
                    // bit read as a 0, roughly once every 35 bytes. Invisible
                    // while the clock was whole CPU cycles (the resolution hid
                    // it in bigger errors); the dominant fault once the tick
                    // clock made everything else line up.
                    nextFluxChange = nextTransition(static_cast<int64_t>(lastSync_));
                    if (nextFluxChange <= static_cast<int64_t>(lastSync_)) {
                        nextFluxChange = static_cast<int64_t>(lastSync_ + 1);
                    }
                }
                if (nextSync < nextStateChange_) {
                    lastSync_ = nextSync;
                    break;
                }
                if (lastSync_ < nextStateChange_) {
                    lastSync_ = nextStateChange_;
                }
                switch (rwState_) {
                    case S_IDLE:
                        rsh_              = 0x00;
                        rwState_          = SR_WINDOW_EDGE_0;
                        nextStateChange_  = lastSync_ + windowSize();
                        syncUpdate_       = 0;
                        asyncUpdate_      = 0;
                        break;
                    case SR_WINDOW_EDGE_0:
                    case SR_WINDOW_EDGE_1: {
                        const uint64_t endw = nextStateChange_ +
                            (rwState_ == SR_WINDOW_EDGE_0 ? windowSize() : halfWindowSize());
                        if (rwState_ == SR_WINDOW_EDGE_0 &&
                            static_cast<int64_t>(endw) >= nextFluxChange &&
                            static_cast<int64_t>(nextSync) >= nextFluxChange) {
                            lastSync_        = static_cast<uint64_t>(nextFluxChange);
                            nextStateChange_ = lastSync_;
                            rwState_         = SR_WINDOW_EDGE_1;
                            break;
                        }
                        if (nextSync < endw) {
                            lastSync_ = nextSync;
                            break;
                        }
                        rsh_ = static_cast<uint8_t>(
                            (rsh_ << 1) | (rwState_ == SR_WINDOW_EDGE_1 ? 1 : 0));
                        nextStateChange_ = lastSync_ = endw;
                        rwState_         = SR_WINDOW_EDGE_0;
                        if (isSync()) {
                            if (rsh_ >= 0x80) {
                                data_ = rsh_;
                                rsh_  = 0;
                            } else if (rsh_ >= 0x04) {
                                data_       = rsh_;
                                syncUpdate_ = 0;
                            } else if (rsh_ >= 0x02) {
                                syncUpdate_ = lastSync_ + readRegisterUpdateDelay();
                            }
                        } else if (rsh_ >= 0x80) {
                            data_         = rsh_;
                            asyncUpdate_  = 0;
                            rsh_          = 0;
                        }
                        break;
                    }
                }
            }
            if (syncUpdate_ && syncUpdate_ <= lastSync_) {
                if (isSync()) data_ = rsh_;
                syncUpdate_ = 0;
            }
            if (asyncUpdate_ && asyncUpdate_ <= lastSync_) {
                if (!isSync()) data_ = 0;
                asyncUpdate_ = 0;
            }
            break;
        }

        case MODE_WRITE: {
            while (nextSync > lastSync_) {
                if (nextSync < nextStateChange_ || !(whd_ & 0x40)) {
                    lastSync_ = nextSync;
                    break;
                }
                if (lastSync_ < nextStateChange_) {
                    lastSync_ = nextStateChange_;
                }
                switch (rwState_) {
                    case S_IDLE:
                        fluxWriteCount_ = 0;
                        if (mode_ & 0x02) {
                            rwState_         = SW_WINDOW_LOAD;
                            rwBitCount_      = 8;
                            nextStateChange_ = lastSync_ + 7;  // MAME's 7 ticks
                        } else {
                            wsh_             = data_;
                            rwState_         = SW_WINDOW_MIDDLE;
                            nextStateChange_ = lastSync_ + halfWindowSize();
                        }
                        break;
                    case SW_WINDOW_LOAD:
                        if (whd_ & 0x80) {
                            // Underrun — CPU didn't load next byte in
                            // time. Only warn if the CPU had actually
                            // started a write sequence (≥1 dataW since
                            // entering MODE_WRITE); the spurious case
                            // where firmware probes Q7=1 with no
                            // intent to write would otherwise fire
                            // this every boot/media-change because
                            // whd_'s cold value (0xBF) has bit 7 set.
                            if (writeDataLoaded_) {
                                pom2::log().warn("IWM", "write underrun");
                            }
                            flushWrite(nextSync);
                            writeClockStop();
                            whd_      &= ~0x40;
                            lastSync_  = nextSync;
                            rwState_   = SW_UNDERRUN;
                        } else {
                            wsh_             = data_;
                            rwState_         = SW_WINDOW_MIDDLE;
                            whd_            |= 0x80;
                            // MAME `half_window_size() - 7`, both terms in IWM
                            // ticks — which is now the unit, so the constant is
                            // MAME's again. (Under the old CPU-cycle clock
                            // halfWindowSize() was 1 or 2 and this underflowed
                            // in uint64_t until the 7 was scaled to 1 as well.)
                            nextStateChange_ = lastSync_ + halfWindowSize() - 7;
                        }
                        break;
                    case SW_WINDOW_MIDDLE:
                        if (wsh_ & 0x80) {
                            if (fluxWriteCount_ < fluxWrite_.size()) {
                                fluxWrite_[fluxWriteCount_++] = lastSync_;
                            }
                        }
                        wsh_             = static_cast<uint8_t>(wsh_ << 1);
                        rwState_         = SW_WINDOW_END;
                        nextStateChange_ = lastSync_ + halfWindowSize();
                        break;
                    case SW_WINDOW_END:
                        if (fluxWriteCount_ == fluxWrite_.size()) {
                            flushWrite();
                        }
                        if (mode_ & 0x02) {
                            --rwBitCount_;
                            if (rwBitCount_ == 0) {
                                rwState_         = SW_WINDOW_LOAD;
                                rwBitCount_      = 8;
                                nextStateChange_ = lastSync_ + 7;  // MAME's 7 ticks
                            } else {
                                rwState_         = SW_WINDOW_MIDDLE;
                                nextStateChange_ = lastSync_ + halfWindowSize();
                            }
                        } else {
                            nextStateChange_ = lastSync_ + halfWindowSize();
                            rwState_         = SW_WINDOW_MIDDLE;
                        }
                        break;
                    case SW_UNDERRUN:
                        lastSync_ = nextSync;
                        break;
                }
            }
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Snapshot / rewind — see the header for why the timestamps matter
// ─────────────────────────────────────────────────────────────────────────

namespace {
// 'IWM1' held the state-machine timestamps in CPU cycles. They are IWM ticks
// since 2026-09-01 (seven per CPU cycle, CpuClock.h), which is a different
// number for the same instant — restoring a v1 blob verbatim would park the
// walker seven times too early and freeze the device until emulated time
// caught up. The loader accepts both and scales.
constexpr uint8_t kIwmBlobMagic[4]   = { 'I', 'W', 'M', '2' };
constexpr uint8_t kIwmBlobMagicV1[4] = { 'I', 'W', 'M', '1' };
}  // namespace

void IWMDevice::appendSnapshotState(std::vector<uint8_t>& out) const
{
    auto putU8  = [&](uint8_t v) { out.push_back(v); };
    auto putU32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto putU64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };

    out.insert(out.end(), kIwmBlobMagic, kIwmBlobMagic + 4);

    // emuCycles timestamps — the whole point of this blob.
    putU64(now_);
    putU64(revStart35_);
    putU64(lastSync_);
    putU64(nextStateChange_);
    putU64(syncUpdate_);
    putU64(asyncUpdate_);
    putU64(fluxWriteStart_);
    putU64(delayDeadline_);

    // Pending flux-write window: count-prefixed, live entries only.
    const uint32_t n = (fluxWriteCount_ <= fluxWrite_.size())
                           ? fluxWriteCount_
                           : static_cast<uint32_t>(fluxWrite_.size());
    putU32(n);
    for (uint32_t i = 0; i < n; ++i) putU64(fluxWrite_[i]);

    putU32(q3Clock_);
    putU8(q3ClockActive_ ? 1 : 0);

    putU32(static_cast<uint32_t>(qt_));
    putU32(static_cast<uint32_t>(active_));
    putU32(static_cast<uint32_t>(rw_));
    putU32(static_cast<uint32_t>(rwState_));

    putU8(data_);
    putU8(whd_);
    putU8(mode_);
    putU8(status_);
    putU8(control_);
    putU8(rwBitCount_);
    putU8(rsh_);
    putU8(wsh_);
    putU8(devsel_);

    // Appended after the v1 layout (old blobs simply end here — the
    // loader treats them as optional): the CA0-CA2/LSTRB phase lines and
    // the write-underrun-warn latch. Rewinding mid-3.5"-command-strobe
    // kept the *live* phases, so the next LSTRB decoded the wrong Sony
    // register.
    putU8(phases_);
    putU8(writeDataLoaded_ ? 1 : 0);
}

bool IWMDevice::loadSnapshotState(const uint8_t* data, size_t n)
{
    if (data == nullptr || n < 4) return false;
    const bool v1 = std::memcmp(data, kIwmBlobMagicV1, 4) == 0;
    if (!v1 && std::memcmp(data, kIwmBlobMagic, 4) != 0) return false;

    size_t pos = 4;
    bool   ok  = true;
    auto need = [&](size_t k) { if (n - pos < k) { ok = false; return false; } return true; };
    auto getU8 = [&]() -> uint8_t {
        if (!need(1)) return 0;
        return data[pos++];
    };
    auto getU32 = [&]() -> uint32_t {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[pos++]) << (8 * i);
        return v;
    };
    auto getU64 = [&]() -> uint64_t {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data[pos++]) << (8 * i);
        return v;
    };

    // Decode into locals first: a truncated blob must not leave the device
    // half-restored (that would be worse than not restoring at all — the
    // timestamps would be a mix of two different points in time).
    const uint64_t nowV      = getU64();
    const uint64_t revV      = getU64();
    const uint64_t lastV     = getU64();
    const uint64_t nextV     = getU64();
    const uint64_t syncV     = getU64();
    const uint64_t asyncV    = getU64();
    const uint64_t fluxStart = getU64();
    const uint64_t delayV    = getU64();
    if (!ok) return false;

    const uint32_t fwCount = getU32();
    if (!ok || fwCount > fluxWrite_.size()) return false;
    std::vector<uint64_t> flux;
    flux.reserve(fwCount);
    for (uint32_t i = 0; i < fwCount; ++i) {
        const uint64_t e = getU64();
        if (!ok) return false;
        flux.push_back(e);
    }

    const uint32_t q3      = getU32();
    const uint8_t  q3Act   = getU8();
    const uint32_t qtV     = getU32();
    const uint32_t activeV = getU32();
    const uint32_t rwV     = getU32();
    const uint32_t rwStV   = getU32();
    const uint8_t  dataV   = getU8();
    const uint8_t  whdV    = getU8();
    const uint8_t  modeV   = getU8();
    const uint8_t  statV   = getU8();
    const uint8_t  ctrlV   = getU8();
    const uint8_t  bitsV   = getU8();
    const uint8_t  rshV    = getU8();
    const uint8_t  wshV    = getU8();
    const uint8_t  devselV = getU8();
    if (!ok) return false;

    // Optional v1.1 tail — absent from blobs written before phases_ and
    // writeDataLoaded_ were serialized; those keep the live values. A
    // single leftover byte is neither format: that is a truncated v1.1
    // blob, and a truncated blob is rejected, never half-applied.
    if (n - pos == 1) return false;
    const bool    hasPhaseTail = (n - pos >= 2);
    const uint8_t phasesV      = hasPhaseTail ? data[pos]     : 0;
    const uint8_t wdlV         = hasPhaseTail ? data[pos + 1] : 0;

    // v1 wrote the FSM clock in CPU cycles; scale those fields into ticks.
    // `now_`, `revStart35_` and `delayDeadline_` were and remain CPU cycles.
    const uint64_t k = v1 ? POM2_IWM_TICKS_PER_CPU_CYCLE : 1;
    now_             = nowV;
    revStart35_      = revV;
    lastSync_        = lastV * k;
    nextStateChange_ = nextV * k;
    syncUpdate_      = syncV * k;
    asyncUpdate_     = asyncV * k;
    fluxWriteStart_  = fluxStart * k;
    delayDeadline_   = delayV;

    fluxWrite_.fill(0);
    for (uint32_t i = 0; i < fwCount; ++i) fluxWrite_[i] = flux[i] * k;
    fluxWriteCount_ = fwCount;

    q3Clock_       = q3;
    q3ClockActive_ = q3Act != 0;
    qt_            = static_cast<int>(qtV);
    active_        = static_cast<int>(activeV);
    rw_            = static_cast<int>(rwV);
    rwState_       = static_cast<int>(rwStV);
    data_          = dataV;
    whd_           = whdV;
    mode_          = modeV;
    status_        = statV;
    control_       = ctrlV;
    rwBitCount_    = bitsV;
    rsh_           = rshV;
    wsh_           = wshV;
    devsel_        = devselV;
    if (hasPhaseTail) {
        phases_          = phasesV;
        writeDataLoaded_ = wdlV != 0;
    }

    // Re-synchronise the host side. The hub / Sony drives / MIG mirrors
    // were NOT part of this blob and still hold their live state; firing
    // the callbacks unconditionally (fireDevsel's transition check would
    // stay silent when the restored value happens to equal the live one)
    // pushes the restored phases / SEL / drive-select down the wire.
    if (phasesCb_) phasesCb_(phases_);
    if (devselCb_) devselCb_(devsel_);
    return true;
}

}  // namespace pom2
