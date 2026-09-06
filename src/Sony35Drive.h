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

// Sony35Drive — 3.5" Sony floppy drive model attached to the IWM. Port
// of MAME's `applefdintf_device::add_35` device (the Apple "Sony 3.5"
// 800 KB drive used in the Macintosh 128 / Mac Plus / Apple //c+ /
// Apple //gs).
//
// The 3.5" drive's interface to the IWM is **fundamentally different**
// from the Disk II 5.25" drive's stepper-motor wiring:
//
//   * On 5.25", IWM phase lines φ0..φ3 are connected to four solenoids
//     that step the head one quarter-track per pulse pattern.
//
//   * On 3.5" Sony, the same four phase pins become a tiny command bus:
//     CA0, CA1, CA2(=PH2/HD), LSTRB(=PH3). The IWM also exposes SEL
//     (drive select) — together they address an 8-line "register"
//     space inside the drive.
//
//       Read register select  = { HDSEL, CA2, CA1, CA0 }  (MAME m_reg —
//       bit 3 is the head-select LINE via ssW, not the IWM drive select)
//       Write register select = same bits, latched by an LSTRB pulse
//
//     Reads come back on the SENSE line, which the IWM samples on
//     `read_register` (control = $40) reads. Writes are toggles —
//     pulsing LSTRB high with a given register address performs a
//     drive action (motor on, motor off, eject, step, direction set).
//
//     Documented in *Inside the Apple //gs* (hardware reference) and
//     mirrored in MAME `src/devices/imagedev/mac_floppy.cpp`. The
//     register table below lists the bits POM2 currently honours.
//
// Phase 1 scope (this file):
//   * `seekPhaseW(uint8_t phases)` — interprets the phase bits as a
//     read-register select OR a strobed write-register, drives the
//     internal state (motor on/off, eject, direction, head select).
//   * `senseR()` — returns the current SENSE bit for the selected
//     read register. The //c+ firmware probes the drive via this
//     register set during the cold-boot dispatch (`$C8xx` SmartPort
//     handler in bank 1) — without this, the probe reads back garbage
//     and the firmware loops indefinitely.
//   * `monW(bool stop)` — motor-enable / monitor wire-OR'd from the
//     IWM. Tracks the floppy spin state for SENSE reads.
//   * Disk image attach (`setImage(Disk35Image*)`).
//
// Phase 2 (deferred, separate session):
//   * `nextTransition(int from)` — return the next flux event timestamp.
//     For 3.5" this requires the zoned Sony GCR encoder
//     (`src/lib/formats/ap_dsk35.cpp`) to convert the loaded block
//     payload into a flux stream the IWM walker can clock out.
//   * `writeFlux(start, end, count, transitions)` — splice IWM-emitted
//     flux into the loaded image (write-back path).

#ifndef POM2_SONY35_DRIVE_H
#define POM2_SONY35_DRIVE_H

#include "Disk35Image.h"

#include <cstdint>
#include <vector>

class FloppySoundSink;

namespace pom2 {

class Sony35Drive
{
public:
    Sony35Drive();

    /// Power-on / cold-reset state. Motor off, no disk-change pending,
    /// direction = inward, track 0, head 0.
    void reset();

    /// Attach (or detach with nullptr) the Disk35Image slot the drive
    /// reads from. The slot is a stable container owned by the host;
    /// loading/ejecting media inside that container is signalled by
    /// `notifyMediaChange()` (drives the disk-change flip-flop).
    void setImage(Disk35Image* image);
    Disk35Image* image() const { return image_; }

    /// Pulse the disk-change flip-flop. Call after Disk35Image::loadFile
    /// or eject() so the //c+ firmware's "media changed" SmartPort
    /// probe latches the new state.
    void notifyMediaChange();

    /// True iff a disk is currently inserted (slot wired AND image
    /// loaded inside it). Out-of-line to avoid pulling Disk35Image's
    /// full definition into every includer of this header.
    bool isInserted() const;

    /// IWM motor-enable line (m_floppy->mon_w in MAME). When the IWM
    /// enters MODE_ACTIVE it pulls this low (motor on); MODE_DELAY /
    /// MODE_IDLE pull it high (motor off).
    void monW(bool motorOffHigh);

    /// IWM head-select wire (m_floppy->ss_w). MIG routes this from the
    /// 3.5" $C240/$C260 toggles in the bank-1 expansion ROM area. Side
    /// 0 = false, side 1 = true.
    void ssW(bool side1);

    /// IWM phase bus (m_floppy->seek_phase_w). Bits 0..3 = CA0, CA1,
    /// CA2 (or PH2 / HD), LSTRB. SEL is a separate IWM control bit
    /// `m_control & 0x20` → device select 1 vs 2; we don't have it
    /// here so we feed it through `setSel`. `emuCycles` is the host's
    /// emulated CPU cycle counter at the moment the IWM strobed —
    /// forwarded to the FloppySoundSink for head-step / motor-on
    /// cadence (mirrors `DiskIICard::seekPhaseW` on the 5.25" side).
    void seekPhaseW(uint8_t phases, uint64_t emuCycles = 0);

    /// Inject the mechanical-sound source (head step, motor spin-up /
    /// down, disk insert / eject click). Optional — when nullptr the
    /// drive is silent. Same plumbing as `DiskIICard::setFloppySound`
    /// but per-drive so internal vs external can route to different
    /// pitch / volume profiles if needed later.
    void setFloppySound(FloppySoundSink* fs) { sound_ = fs; }

    /// Where the firmware-issued eject (register 7) sends its write-back.
    ///
    /// That register is strobed from the IWM path, i.e. on the CPU worker
    /// with `stateMutex` held — and CLAUDE.md's rule is that the lock is
    /// never held across file I/O: an 800 KB rewrite plus two fsyncs there
    /// freezes the machine and the window together, cancel button included.
    /// With a sink wired the drive captures the payload (a memcpy) and the
    /// host writes it with the lock released. Without one it saves inline,
    /// which is what a single-threaded host (CLI, tests) wants.
    void setWriteBackSink(Disk35WriteBackSink* sink) { writeBackSink_ = sink; }

    /// User-facing one-shot — host calls this after a successful mount /
    /// eject so the drive emits a click via the sound sink without going
    /// through the IWM strobe path. Idempotent: nullptr sink → no-op.
    void emitInsertClick();

    /// IWM SEL wire — bit 5 of IWM control register. Distinguishes
    /// "external" vs "internal" drive on the //c+ but also doubles as
    /// the high bit of the 3.5" register select.
    void setSel(bool sel);

    /// SENSE line readout — the IWM samples this when the host reads
    /// the IWM status register (control = $40). MAME returns it via
    /// `m_floppy->wpt_r()` on the IWM read path.
    /// Returns true if SENSE is HIGH (=> IWM read-side reports WP bit
    /// SET, i.e. the high bit of the status byte is 1).
    bool senseR() const;

private:
    /// Raw register table behind senseR() — split out so the env-gated
    /// diagnostic trace (`POM2_TRACE_IWM_SENSE=1`) can log the (reg,
    /// value) pair without duplicating the switch.
    bool senseValue(uint8_t reg) const;

public:

    /// Convenience accessors for inspectors / save state.
    bool isMotorOn()        const { return motorOn_; }
    bool isWriteProtected() const { return writeProtect_; }
    int  track()            const { return track_; }
    bool side1()            const { return side1_; }

    // ── Phase 2: flux-source side of the drive ─────────────────────────
    //
    // The IWM consults `nextTransition(fromCycle, revStart)` to find
    // the next flux event under the head. Mirrors MAME's
    // `floppy_image_device::get_next_transition` for the 3.5" Sony
    // drive, but the bit-cell stream is generated on demand from the
    // attached `Disk35Image` blocks (no on-disk WOZ analogue today).
    //
    // The first call for a given (track, side) lazily builds the bit-
    // cell cache via the Sony 4:4 GCR encoder (port of MAME
    // `flopimg.cpp::build_mac_track_gcr`). Cache lifetime: cleared on
    // `setImage`, `eject`, `notifyMediaChange`, or motor turn-on with
    // a freshly-loaded image.

    /// Cells per revolution for the current track's speed zone. MAME
    /// `flopimg.cpp:2019 cells_per_speed_zone[]`.
    int cellsPerRev() const;

    /// CPU cycles per revolution at the current zone's RPM (394 / 429 /
    /// 472 / 525 / 590 RPM for outermost to innermost). Derived from
    /// `60 × POM2_CPU_CLOCK_HZ / RPM`.
    int64_t cyclesPerRev() const;

    /// The same period in IWM ticks (`POM2_IWM_TICKS_PER_CPU_CYCLE`), which
    /// is the unit the flux timeline below speaks. A cell is ~14.2 ticks
    /// here against ~2.02 CPU cycles; the whole reason for the finer unit is
    /// that a window edge has to be placeable inside a cell. See CpuClock.h.
    int64_t ticksPerRev() const;

    /// IWM tick of the next flux transition strictly after `fromTick`.
    /// `revStartTick` anchors the head position (zero = cell 0 was under the
    /// head at tick 0). Returns `INT64_MAX` if no disk is inserted or the
    /// track is unformatted (no transitions).
    ///
    /// Ticks, not CPU cycles: at CPU-cycle resolution consecutive Sony cells
    /// land 2 or 3 cycles apart (they are 2.02 apart) and the IWM's walker
    /// cannot hold alignment through a 700-byte data field — measured at ~4
    /// address fields per revolution and zero sectors. Pinned by
    /// `sony35_iwm_read_path` (POM2_GCR_IWM=1).
    int64_t nextTransition(int64_t fromTick, int64_t revStartTick) const;

    /// Drop the cached bit-cell stream. Called on media swap / head
    /// step so the next access rebuilds.
    void invalidateCache() const;

    /// **TEST-ONLY** dump of the raw bit-cell stream for the current
    /// (track, side) — one byte per cell, value 0 or 1. Returns an
    /// empty vector when no disk is inserted. Used by
    /// `smartport_35_smoke_test.cpp` to validate the Sony GCR encoder
    /// output against MAME's reference layout. The production code
    /// path never touches this; the IWM walker queries
    /// `nextTransition()` instead.
    std::vector<uint8_t> debugCellStream() const;

private:
    Disk35Image* image_         = nullptr;
    bool         motorOn_       = false;
    bool         writeProtect_  = true;   // safe default until image probed
    bool         side1_         = false;
    bool         sel_           = false;
    bool         directionIn_   = false;  // true → step toward track 0
                                          // (MAME floppy.cpp:290 m_dir(0))
    /// MAME `m_dskchg` polarity (floppy.cpp:560/672/723): HIGH = disk in
    /// place or latch cleared, LOW = empty / ejected since last clear.
    /// Sense register 3 returns it NEGATED (mac wpt_r `!m_dskchg`).
    bool         dskchg_        = false;
    int          track_         = 0;
    uint8_t      phases_        = 0;
    uint8_t      prevPhases_    = 0;

    /// Optional mechanical-sound sink. Non-owning. Set by
    /// EmulationController at construction time.
    FloppySoundSink* sound_     = nullptr;
    /// Optional deferred-write-back sink (see setWriteBackSink). Non-owning.
    Disk35WriteBackSink* writeBackSink_ = nullptr;
    /// Emulated CPU cycle of the last IWM strobe — used to stamp
    /// sound_->step() and sound_->motor() calls so cadence is measured
    /// in emulated time (matches the FloppySoundDevice's `emuCycles`
    /// expectation; see FloppySoundSink::step).
    uint64_t     lastStrobeCycle_ = 0;

    /// 4-bit register address: { HDSEL, CA2, CA1, CA0 } — MAME
    /// `mac_floppy_device`: `(phases & 7) | (m_actual_ss ? 8 : 0)`.
    /// Read addresses are the same bit pattern as writes but sampled in
    /// real time.
    uint8_t regSelect() const;

    /// Strobe a write-register address (called on LSTRB rising edge).
    void strobeWriteRegister(uint8_t reg);

    // Phase 2 bit-cell cache. Populated lazily by `ensureCache()` for
    // the current (track_, side1_) tuple; `invalidateCache()` clears
    // it on media swap / head step.
    //
    // `cells_` is the source of truth — one byte per cell, value 0/1.
    // `transitionCells_` is a sorted index list derived from cells_,
    // kept in sync for the IWM walker's O(log n) `nextTransition`
    // binary search. Phase 4 write-back mutates `cells_` then
    // rebuilds the list.
    mutable int cachedTrack_ = -1;
    mutable int cachedHead_  = -1;
    mutable std::vector<uint8_t> cells_;          // 1 byte per cell
    mutable std::vector<int>     transitionCells_;
    mutable int cachedCellsPerRev_ = 0;
    mutable int64_t cachedCyclesPerRev_ = 0;

    void ensureCache() const;
    void rebuildTransitionsFromCells() const;

public:
    // ── Phase 4: flux write-back ───────────────────────────────────────
    //
    // The IWM hands us a window of flux events recorded while it was
    // in MODE_WRITE on this drive (MAME `iwm_device::flush_write`,
    // POM2 `IWMDevice::flushWrite`). We splice the new transitions
    // into the cached cell stream — overwriting whatever the encoder
    // produced from the image — then run a GCR decoder over the
    // current cells to recover any complete sectors and push their
    // payload back into the attached `Disk35Image`. Port of MAME
    // `flopimg.cpp::extract_sectors_from_track_mac_gcr6` (line 2107).
    //
    // `startTick` / `endTick` bracket the window the IWM was writing for;
    // `fluxes` is the sorted list of flux-transition timestamps inside it.
    // `revStartTick` is the same anchor `nextTransition` reads against. All
    // in IWM ticks, like the read side — the two have to share one timeline
    // or a write lands on a different cell than the read that verifies it.
    void writeFlux(int64_t startTick,
                   int64_t endTick,
                   const int64_t* fluxes,
                   int            count,
                   int64_t        revStartTick);

private:
    /// Decode the current `cells_` stream for any complete sectors
    /// and write changed blocks to the attached image. Called after
    /// each writeFlux splice. Returns the number of sectors that
    /// actually got written back.
    int decodeAndCommit() const;
};

}  // namespace pom2

#endif // POM2_SONY35_DRIVE_H
