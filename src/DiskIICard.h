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

// DiskIICard — Apple Disk II Interface card (slot 6 by convention).
// Two-drive controller, 16-sector DOS 3.3 (.dsk / .do) and ProDOS-order
// (.po) images, plus raw .nib. Read-back via the bit-level LSS (default)
// or the legacy 32-cycle gate (fallback when no P6 PROM is loaded).
//
// Soft-switch map (slot N at $C080+N*16; for slot 6 → $C0E0-$C0EF):
//
//   $C0n0/n1   Phase 0 off / on    head stepper coil 0
//   $C0n2/n3   Phase 1 off / on
//   $C0n4/n5   Phase 2 off / on
//   $C0n6/n7   Phase 3 off / on
//   $C0n8      Drive (motor) off
//   $C0n9      Drive (motor) on
//   $C0nA      Select drive 1   (activeDrive ← 0 → uses images[0])
//   $C0nB      Select drive 2   (activeDrive ← 1 → uses images[1])
//   $C0nC      Q6L  — shift / read data register
//   $C0nD      Q6H  — load / write-protect probe
//   $C0nE      Q7L  — read mode
//   $C0nF      Q7H  — write mode (we acknowledge but never alter media)
//
// Each drive owns its own DiskImage, head position (in quarter-tracks),
// and nibble-buffer cursor. Phase magnet energization is controller
// state and shared between the two drives — same as real hardware,
// where only the selected drive's head responds to phase pulses.
// Motor on/off is also a single controller signal; in real hardware it
// reaches the selected drive only, but POM2 models it as global since
// a stopped drive sees no LSS activity anyway (active==MODE_IDLE skips
// sync).
//
// Slot ROM ($Cs00-$CsFF, s=6 → $C600-$C6FF) is the Apple-Disk-II "P5A"
// 256-byte boot PROM. The PROM autodetects its slot via the standard
// JSR-$FF58 / TSX / LDA $0100,X trick, then steps the head to track 0,
// reads the first sector via the address-field/data-field state machine,
// and JMPs to the loaded boot1 at $0801.
//
// Head stepping: real Disk II hardware pulls the head magnet coils in a
// 4-phase rotational pattern. The head's mechanical position is at any
// quarter-track offset; each phase magnet has a "well" at a quarter-track
// position whose index (mod 4) matches the phase number. With one magnet
// energized the head settles at that magnet's well; with two adjacent
// magnets energized the head settles between them; opposing magnets
// (0+2 or 1+3) cancel and the head holds. State is held in
// *quarter-tracks* (0..139 = 35 tracks × 4) so disks with quarter-track
// copy protection step accurately. The same algorithm is what MAME's
// `apple2_floppy_image_device` uses (see its phase-to-target lookup).
//
// Timing: at 1.0227 MHz with 4 µs bit cells, the LSS shift register
// outputs one nibble every ~32 CPU cycles. We accumulate cycles via
// advanceCycles() and step the track-buffer cursor accordingly. Reads of
// $C0nC return the current nibble; bit-7 is implicitly always set
// because every valid GCR nibble has it set.

#ifndef POM2_DISK_II_CARD_H
#define POM2_DISK_II_CARD_H

#include "DiskImage.h"
#include "SlotPeripheral.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class M6502;
class FloppySoundSink;

namespace pom2 { class IWMDevice; }

class DiskIICard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 6;

    /// Construct with the slot number this card will be plugged into.
    /// The Disk II boot PROM is slot-agnostic at runtime — the P5A PROM
    /// auto-detects its own slot via the standard `JSR $FF58 / TSX /
    /// LDA $0100,X` trick — so the slot number is held only for
    /// diagnostics / UI display.
    explicit DiskIICard(int slot = kDefaultSlot);
    /// Flushes both drives' pending write-back (see `flushPendingWrites`)
    /// so tearing the card down — process exit, profile switch — can't
    /// drop writes the user opted in to.
    ~DiskIICard();

    int getSlot() const { return slot_; }

    /// Inject the host CPU pointer so the LSS can resolve the precise
    /// sub-instruction cycle of every $C0EX MMIO access. Without this,
    /// the LSS treats every MMIO access as happening at the START of the
    /// instruction, when in reality (per MAME's per-cycle state machine)
    /// the data fetch of `LDA $C0EC` happens on the 4th cycle (sub-cycle
    /// 3) and the LSS state at that exact moment is what software sees.
    /// Cycle-precise copy-protection schemes (Spiradisc, Locksmith, some
    /// Sirius / Sierra titles) rely on this. Stock DOS / ProDOS RWTS
    /// doesn't notice the difference. Safe to leave unset (nullptr) —
    /// behaviour falls back to instruction-aligned access timing.
    void setCpu(M6502* cpu) { cpu_ = cpu; }

    /// Inject the mechanical-sound source (head step, motor spin-up/down,
    /// disk insert/eject click). Optional — when nullptr the card is
    /// silent on the floppy-sound side (the data path is unchanged).
    /// Sound calls are sparse (mutex-guarded command queue inside the
    /// source) so this stays cheap even on read-heavy workloads.
    void setFloppySound(FloppySoundSink* fs) { sound_ = fs; }

    /// Force every drive's head back to track 0 and reset the LSS state.
    /// Used by the "Boot disk" UI shortcut so the boot PROM finds
    /// D5 AA 96 quickly even if a head wandered while waiting for a disk
    /// insert.
    void seekTrack0() {
        for (int d = 0; d < kDriveCount; ++d) {
            headQuarterTrack[d] = 0;
            trackPos[d]         = 0;
        }
        cycleAccum = 0;
        // The //c / //c+ on-board IWM caches (image, quarter-track); every
        // other head-moving path re-pushes it. Without this the IWM kept
        // reading the pre-boot quarter-track after the UI's "Boot disk"
        // shortcut yanked the head back to 0.
        pushIwmFloppy();
    }

    /// Load the 256-byte Disk II boot PROM from disk. Must succeed before
    /// the card is useful — without the PROM, $C600-$C6FF reads back
    /// $FF and `PR#6` jumps into nothing.
    bool loadBootRom(const std::string& path);
    bool hasBootRom() const { return bootRomLoaded; }
    /// 13-sector boot PROM (Apple 341-0009, "P5" 13-sector) + LSS PROM
    /// (341-0010, "P6" 13-sector). Served instead of the 16-sector pair
    /// only while a 13-sector (pre-DOS-3.3) disk is mounted. Optional —
    /// without them a 13-sector disk mounts but can't boot.
    bool loadBootRom13(const std::string& path);
    bool loadLssRom13(const std::string& path);

    /// Load the 256-byte Disk II Logic State Sequencer PROM (Apple part
    /// 341-0028-A, "P6"). When loaded, the card switches to a cycle-
    /// accurate bit-level LSS that drives Q6/Q7 and the data register
    /// per the PROM's state-table — a faithful port of MAME's
    /// `wozfdc.cpp lss_sync()`. Without this PROM, reads fall back to
    /// the simplified 32-cycle byteReady gate (which is good enough for
    /// stock DOS 3.3 / ProDOS RWTS but not for software like Copy II
    /// Plus that scans for sync-gap-shifted byte boundaries).
    bool loadLssRom(const std::string& path);
    bool hasLssRom() const { return p6RomLoaded; }
    /// Is the bit-level LSS actually driving the head right now? Distinct
    /// from `hasLssRom()`: a mounted WOZ forces the bit-level path even with
    /// no `diskii_p6.rom` on disk (the embedded default P6 suffices), and
    /// with neither the card runs the legacy 32-cycle nibble gate. The
    /// Abstraction Levels panel reports exactly this difference — it is the
    /// L0-vs-fallback line for the whole 5.25" stack.
    bool usingBitLss() const { return useBitLss; }

    /// Number of drives the controller models. The Disk II Interface
    /// has two slots in the daisy chain (drive 1 = images[0], drive 2 =
    /// images[1]).
    static constexpr int kDriveCount = 2;

    /// True iff `drive` is a valid 0-based drive index. Used by the per-drive
    /// getters below to reject an out-of-range index instead of indexing the
    /// fixed-size images[]/headQuarterTrack[]/trackPos[] arrays out of bounds.
    static constexpr bool validDrive(int drive) {
        return drive >= 0 && drive < kDriveCount;
    }

    /// Insert / eject a disk image. The single-arg variants target
    /// drive 1 (= index 0) for backwards compatibility with existing UI
    /// and test call sites; pass an explicit drive index (0 or 1) for
    /// the two-arg form.
    ///
    /// insertDisk() reads and decodes the file INLINE, so a caller holding
    /// `stateMutex` holds it across the whole file read — 12.8 ms for a 32 MB
    /// image with a warm cache, most of a PAL frame, during which the CPU
    /// worker and the UI paint are both blocked. Callers on the UI thread
    /// should use the two-phase form below instead; this one stays for the
    /// single-threaded callers (CLI, headless, tests) where it reads better.
    bool insertDisk(int drive, const std::string& path);
    bool insertDisk(const std::string& path) { return insertDisk(0, path); }

    /// ── Two-phase mount ──────────────────────────────────────────────────
    /// Phase 1, to be called WITHOUT `stateMutex`: read and decode `path`
    /// into a detached image. This is the expensive half — all of the file
    /// I/O lives here and none of it touches card state, so nothing needs
    /// serialising against the CPU worker.
    ///
    /// Returns false with `error` filled in if the file cannot be used; the
    /// caller should report it and NOT proceed to phase 2.
    static bool prepareDisk(const std::string& path, bool writeBackEnabled,
                            DiskImage& out, std::string& error);

    /// Phase 2, to be called WITH `stateMutex` held: take over an image
    /// prepared by phase 1. Cheap — a move plus the LSS re-anchor — with one
    /// documented exception: if the OUTGOING medium has unsaved changes it is
    /// still flushed inline here, and a failed flush still refuses the swap.
    /// That is deliberate. Moving the flush out of the lock would mean either
    /// swapping before knowing whether the old medium could be written (which
    /// loses the user's changes when it cannot) or handing the dirty image
    /// back for the caller to commit (which loses them if the caller drops
    /// it). Latency is worth less than the only copy of somebody's disk. The
    /// case is rare in practice: write-back is opt-in, so a clean medium —
    /// the default — takes the fully unlocked path.
    bool installDisk(int drive, DiskImage&& prepared);

private:
    /// Commit a drive's pending write-back before its medium is dropped.
    /// False (with `mediaErrors[drive]` set) means the swap must be refused.
    bool flushOutgoingForSwap(int drive);
    /// The state mutation shared by insertDisk() and installDisk(): adopt the
    /// image and re-anchor the LSS, track position and derived media state.
    bool installPreparedLocked(int drive, DiskImage&& replacement);

public:
    /// Eject `drive`, saving its write-back first. ONE-PHASE: the save runs
    /// inline, so a caller holding `stateMutex` freezes the machine and the
    /// window for the whole read-modify-write + rename. Composed from the
    /// three-step form below — kept for single-threaded callers (CLI, tests,
    /// `Pom2Core`) that have no lock to get out from under.
    bool ejectDisk(int drive);
    bool ejectDisk() { return ejectDisk(0); }

    /// ── Two-phase eject ──────────────────────────────────────────────────
    /// The mirror of `prepareDisk` + `installDisk`. Mount got its split in
    /// v0.8.5 (`MediaMount.h`); eject kept doing a full sector re-encode plus
    /// a write and two fsyncs with `stateMutex` held — the lock the CPU worker
    /// takes every 4096 cycles and the UI thread takes to paint every frame.
    ///
    /// Phase 1, WITH the lock: splice the in-flight write burst in, lift the
    /// whole medium out (a move — no syscall) and leave the bay empty. Returns
    /// null when there was nothing to write back; the bay is emptied either
    /// way. A `DiskImage` is ~242 KB, hence the heap.
    std::unique_ptr<DiskImage> takeEjectWriteBack(int drive);

    /// Phase 2, with NO lock held: write the lifted medium out. Static — by
    /// now the card may have been unplugged, and the payload is self-contained.
    static bool commitEjectWriteBack(DiskImage& pending, std::string& error);

    /// Phase-2 FAILURE undo, with the lock held: put the medium back so the
    /// user can fix the cause and retry, which is what the inline path always
    /// did. Refuses (returns false) if something was mounted into the bay
    /// while phase 2 ran unlocked — that disk wins, and the caller reports the
    /// loss rather than silently overwriting it.
    bool restoreEjected(int drive, std::unique_ptr<DiskImage> pending);

    /// Persist any pending write-back for both drives WITHOUT ejecting.
    /// insertDisk / ejectDisk already flush on the swap, but shutdown and
    /// profile switching tear the card down through neither — so a session's
    /// worth of guest writes used to die with the process even with
    /// write-back enabled. A no-op when write-back is off or the medium is
    /// physically write-protected.
    bool flushPendingWrites();

    // All per-drive getters bound-check `drive` (0..kDriveCount-1) and return
    // a safe default out of range — insertDisk/ejectDisk already validate, so
    // these match that contract instead of indexing images[]/headQuarterTrack[]
    // out of bounds.
    bool isDiskLoaded(int drive = 0) const {
        return validDrive(drive) && images[drive].isLoaded();
    }
    const std::string& getDiskPath (int drive = 0) const {
        static const std::string kEmpty;
        return validDrive(drive) ? images[drive].getPath() : kEmpty;
    }
    const std::string& getLastError(int drive = 0) const {
        static const std::string kEmpty;
        if (!validDrive(drive)) return kEmpty;
        return mediaErrors[drive].empty() ? images[drive].getLastError()
                                          : mediaErrors[drive];
    }

    int  getCurrentTrack(int drive = 0) const { return validDrive(drive) ? headQuarterTrack[drive] / 4 : 0; }
    int  getHalfTrack   (int drive = 0) const { return validDrive(drive) ? headQuarterTrack[drive] / 2 : 0; }
    int  getQuarterTrack(int drive = 0) const { return validDrive(drive) ? headQuarterTrack[drive] : 0; }
    bool isMotorOn() const { return motorOn; }
    /// Index of the drive currently selected by the most recent
    /// $C0nA / $C0nB access (0 = drive 1, 1 = drive 2).
    int  getActiveDrive() const { return activeDrive; }
    int  getTrackPosition(int drive = 0) const { return validDrive(drive) ? trackPos[drive] : 0; }
    bool hasUnsavedChanges(int drive = 0) const { return validDrive(drive) && images[drive].hasUnsavedChanges(); }
    /// PHYSICAL write-protect of the medium (WOZ INFO+2 / 2IMG WP flag) —
    /// distinct from the write-back opt-in. The UI needs both to tell the
    /// user *why* the guest is seeing a write-protected disk.
    bool isFileWriteProtected(int drive = 0) const {
        return validDrive(drive) && images[drive].isFileWriteProtected();
    }
    /// Total nibble write flushes (across both drives) since last reset.
    /// Used by the dos33_save smoke test to confirm SAVE actually
    /// exercised the write pipeline (vs. erroring out before any write).
    uint64_t getWriteFlushCount() const { return writeFlushCount; }

    /// Bind the //c / //c+ on-board IWM. When set, DiskIICard pushes
    /// `setFloppy(image, qt)` updates to the IWM on every disk insert /
    /// eject / drive-select / seek, so the standalone IWMDevice state
    /// machine (MAME `iwm.cpp` port, see `IWMDevice.{h,cpp}`) can walk
    /// the same flux stream as DiskIICard. Non-owning pointer set by
    /// EmulationController.
    void setIWM(pom2::IWMDevice* iwm) {
        iwm_ = iwm;
        pushIwmFloppy();
    }

    /// User opt-in for write-back. When true, eject (and explicit save)
    /// rewrites the source file with any modified sectors. Default off
    /// to avoid silently mutating the user's image file. The toggle is
    /// card-wide and applies to both drives.
    void setWriteBackEnabled(bool on) {
        for (int d = 0; d < kDriveCount; ++d) images[d].setWriteBackEnabled(on);
        writeBackEnabled = on;
    }
    bool isWriteBackEnabled() const { return writeBackEnabled; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Disk II"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    void    advanceCycles(int cycles) override;
    void    onReset() override;

    // Rewind/snapshot: serialize the mechanical + LSS runtime state (head
    // position, motor, phase magnets, data register, sequencer, timing) so a
    // rewind doesn't leave an in-progress read on the wrong nibble. The
    // mounted media and the boot/P6 PROMs are NOT captured — they're
    // reconstructed identically on the restored machine. See DiskIICard.cpp.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

private:
    int slot_;
    /// Optional CPU pointer for sub-instruction cycle resolution at MMIO
    /// access points. See `setCpu()` doc above.
    M6502* cpu_ = nullptr;
    FloppySoundSink* sound_ = nullptr;
    std::array<DiskImage, kDriveCount> images{};
    std::array<std::string, kDriveCount> mediaErrors{};
    /// Drive currently routed to the LSS / legacy gate. Set by control()
    /// in response to $C0nA ($activeDrive=0) or $C0nB ($activeDrive=1).
    /// Persists across onReset() — matches the 74LS259 latch on real
    /// hardware which is not cleared by 6502 RES.
    int activeDrive = 0;
    std::array<uint8_t, 256> bootRom{};
    bool bootRomLoaded = false;
    // 13-sector (341-0009) boot PROM — served only when serving13_.
    std::array<uint8_t, 256> bootRom13{};
    bool bootRom13Loaded = false;

    // Atomic: read by the UI thread (updateAutoTurbo's disk-activity poll)
    // while the worker writes it from soft-switch handling — same reason
    // Block512Backing::isBusy is atomic. Relaxed-equivalent plain ops are
    // fine (a one-frame-late turbo decision is harmless; the flag carries
    // no dependent data).
    std::atomic<bool> motorOn { false };
    // MAME `wozfdc_device::active` — MODE_IDLE / MODE_ACTIVE / MODE_DELAY.
    // MODE_DELAY: a motor-off ($C0E8) command does NOT stop the drive
    // immediately; `delay_timer` holds the LSS active for ~1 second of
    // CPU cycles before the head transitions to MODE_IDLE. A fresh
    // motor-on ($C0E9) during the delay cancels it (back to MODE_ACTIVE).
    enum ActiveMode { MODE_IDLE = 0, MODE_ACTIVE, MODE_DELAY };
    ActiveMode active = MODE_IDLE;
    int  motorOffDelay = 0;
    bool writeMode = false;     // Q7 latch: false=read, true=write
    bool loadMode  = false;     // Q6 latch: false=shift, true=load

    // IWM (Apple Integrated Wozniak Machine) register shadows. The
    // Apple //c / //c+ internal firmware drives slot 6 as an IWM, not
    // as a Disk II / wozfdc. The four "real" registers MAME tracks
    // (iwm.cpp:36-46) plus the data path are unified into the LSS
    // state above; the bits that diverge from a plain Disk II are:
    //
    //   * mode   — written by `STA $C0EF` when Q6 is already high
    //              (MAME `iwm.cpp:248-253 mode_w`). Low 5 bits are
    //              echoed in the status register at $C0EE+Q6 (line 259).
    //   * whd    — write-handshake register. Returned by reads of
    //              $C0EC when Q7 is high + Q6 is low (control = 0x80;
    //              MAME `iwm.cpp:110`). Bit 7 = "ready" (1 = idle/OK),
    //              bit 6 = "in write mode" (set on Q7 rising edge,
    //              cleared on idle — `iwm.cpp:183 / 199 / 82`). Cold
    //              value 0xBF (`iwm.cpp:57`); we mirror that so the
    //              //c+ alt firmware's `BIT $C0EC / BPL` ready loop
    //              at `$C8A6-$C8A9` falls through on the first read.
    //
    // Existing Disk II smoke tests don't read $C0EC under Q7=1 (write
    // mode reads are atypical for the standard Disk II boot), so
    // returning whd there is a no-op for them.
    uint8_t iwmMode = 0;
    uint8_t iwmWhd  = 0xBF;
    bool writeBackEnabled = false;     // forwarded to DiskImage on toggle
    uint8_t writeLatch = 0xFF;         // latched data nibble for next bit-cell flush

    // Head stepper. phaseOn[i] = magnet i currently energized. The phase
    // signals are controller state and shared between the two drives —
    // only the selected drive's head physically moves in response.
    std::array<bool, 4> phaseOn{};
    // Head position in quarter-tracks, per drive. 35 tracks × 4 qt = 140;
    // the head can sit at any qt from 0 (track 0) to 4*(kTracks-1) = 136
    // (track 34). Quarter-tracks are needed for some copy protections;
    // the standard DOS 3.3 / ProDOS skew uses whole tracks (qt mod 4 == 0).
    int headQuarterTrack[kDriveCount] = {0, 0};

    // Position into the active drive's track nibble buffer (0..6655).
    // Wraps continuously while the motor is on. Used by the legacy
    // 32-cycle gate and the LSS write path (where a complete write
    // nibble lands at trackPos[activeDrive]).
    int trackPos[kDriveCount] = {0, 0};
    int cycleAccum = 0;       // CPU cycles since the last nibble advance

    // LSS shift-register model (legacy 32-cycle gate). While a new
    // nibble is assembling, the data register's bit 7 reads as 0
    // (intermediate shift state), so the host CPU's `LDA $C0EC ; BPL
    // loop` busy-wait holds until the full byte is in.
    uint8_t dataLatch = 0;
    bool    byteReady = false;

    // ── Bit-level LSS state (when useBitLss == true) ─────────────────
    //
    // Port of the Apple Disk II Logic State Sequencer, faithful to MAME's
    // `wozfdc.cpp lss_sync()`. The 256-byte P6 PROM (Apple part 341-0028-A)
    // is indexed by a persistent 8-bit address register whose bits are:
    //
    //   bit 7  state[3]  (= prev opcode bit 7)
    //   bit 6  state[2]  (= prev opcode bit 6)
    //   bit 5  state[0]  (= prev opcode bit 4)  ← scrambled with state[1]
    //   bit 4  !PULSE    (no flux transition this sub-cell)
    //   bit 3  Q7        (mode_write — 0=read, 1=write)
    //   bit 2  Q6        (mode_load — 0=shift, 1=load)
    //   bit 1  QA        (data register MSB, sampled AFTER each opcode)
    //   bit 0  state[1]  (= prev opcode bit 5)  ← scrambled with state[0]
    //
    // The PROM byte read at that address is the opcode for the current
    // LSS tick. Its high nibble becomes the *new* state (re-scrambled into
    // address bits 7,6,5,0 via MAME's exact pack: `(opcode & 0xC0) |
    // ((opcode & 0x20) >> 5) | ((opcode & 0x10) << 1)`). Its low nibble is
    // the ALU op on the data register, dispatched per MAME's full range:
    //
    //     0x0..0x7      CLR  data ← 0
    //     0x8, 0xC      NOP
    //     0x9           SL0  data ← (data << 1)
    //     0xA, 0xE      SR   data ← (data >> 1) | (write-protect ? 0x80 : 0)
    //     0xB, 0xF      LD   data ← writeLatch (CPU bus)
    //     0xD           SL1  data ← (data << 1) | 1
    //
    // In write mode, bit 7 of the new address (= opcode bit 7 = new
    // state[3]) is the WRITE_DATA bit driven onto the disk surface for
    // the next cell.
    //
    // The PROM bytes embedded as kP6RomDefault[] are pre-permuted into
    // MAME's address layout by scripts/permute_p6_rom.py from the
    // apple2js `SEQUENCER_ROM_16` table; same 256 logical bytes, indexed
    // differently. roms/diskii_p6.rom on disk is also MAME-layout.
    std::array<uint8_t, 256> p6Rom{};
    bool    p6RomLoaded = false;
    // 13-sector (341-0010) LSS PROM + the "this card is currently a
    // 13-sector controller" latch. serving13_ is recomputed on every
    // insertDisk from whether any mounted disk is 13-sector AND both
    // 13-sector PROMs are present; slotRomRead / lssSync then index the
    // 13-sector pair instead of the 16-sector one.
    std::array<uint8_t, 256> p6Rom13{};
    bool    p6Rom13Loaded = false;
    bool    serving13_    = false;
    bool    useBitLss   = false;        // false → legacy 32-cycle gate
    uint8_t address     = 0x10;         // MAME's persistent LSS address reg
    uint8_t lssData     = 0;            // 8-bit shift / data register

    // ── MAME wozfdc cycle bookkeeping ───────────────────────────────────
    //
    // MAME's `wozfdc_device::cycles` is the absolute LSS-cycle counter
    // (clock = 2× CPU clock; one PROM lookup per LSS cycle). The CPU side
    // calls `lss_sync()` whose loop runs from `cycles` up to a `cycles_limit`
    // computed from `time_to_cycles(machine().time())`. POM2 doesn't have
    // a global emu time; we recover the equivalent via `cpuCycleTotal` —
    // a running CPU cycle counter that `advanceCycles(n)` bumps before
    // calling `lss_sync(0)`. The LSS limit is `cpuCycleTotal*2 + extra`.
    //
    // This is a verbatim port of MAME's algorithm; only the time-base
    // substrate changes (LSS cycles directly, instead of attotime
    // converted via clock()*2).
    uint64_t lssCycle       = 0;
    uint64_t cpuCycleTotal  = 0;

    // MAME `floppy_image_device::m_revolution_start_time`, one per drive.
    // Set to the current `lssCycle` on a motor-on transition (MAME's
    // `mon_w(false)` — fires from `control() case 0x9 MODE_IDLE→ACTIVE`
    // and on the *new* drive in `selectDrive()` when the controller is
    // already spinning). Kept `kNeverRev` while the drive's motor is off
    // (MAME stores `attotime::never`). The disk angular position is
    // `(lssCycle - revolutionStartLssCycle[d]) mod track_period_lsscycles`,
    // computed inside `DiskImage::getNextTransition`.
    //
    // Why per-drive: MAME's `revolution_start_time` lives on the
    // `floppy_image_device`, not on the `wozfdc_device`. When the user
    // does `$C0EA / $C0EB` to switch drives mid-spin, each drive's disk
    // remembers its own angular position; the previously-selected drive's
    // disk does NOT freeze when the controller looks away. POM2 used to
    // collapse this into the single global `lssCycle`, which fell apart
    // when a head step on the active drive made the new track's period
    // differ from the old one (the modulo wrap suddenly snapped angular
    // position to a stale slot). See `DiskImage::getNextTransition` for
    // the angular-position computation.
    static constexpr int64_t kNeverRev = -1;
    int64_t revolutionStartLssCycle[kDriveCount] = { kNeverRev, kNeverRev };

    // MAME `write_buffer[32]` — flux event timestamps (LSS cycles)
    // captured from WRITE_DATA edge transitions during the active write
    // session. Flushed via `image.writeFlux(...)` either on Q7 falling
    // edge (control() $C0nE) or pre-emptively when write_position ≥ 30
    // (avoids ever hitting the assert at 32, mirroring MAME's flush
    // condition exactly).
    static constexpr int  kWriteBufferSize = 32;
    int64_t  writeBuffer[kWriteBufferSize] = {};
    int      writePosition  = 0;
    int64_t  writeStartTime = 0;       // LSS cycle of last splice start
    bool     writeLineActive = false;  // tracks WRITE_DATA edge state

    uint64_t writeFlushCount = 0;

    /// Recompute the card-wide latches derived from the currently mounted
    /// media: `serving13_` (13-sector boot PROM at $Cn00) and `useBitLss`
    /// (WOZ / 13-sector force the bit-level LSS). Called from BOTH
    /// insertDisk and ejectDisk — they describe the mounted set, so an
    /// eject has to be able to clear them again.
    void refreshMediaDerivedState(bool warnMissing13Rom);

    /// Splice the in-flight LSS write burst into the ACTIVE drive's image and
    /// clear the buffer. `selectDrive` does this on a drive swap; the media
    /// paths that abandon a write outside the normal Q7 falling edge —
    /// `insertDisk`, `flushPendingWrites` — must do it too, or the sector the
    /// controller is mid-way through writing is lost. No-op unless a write
    /// session is actually open on a loaded, write-back-enabled drive.
    void commitInFlightWrite();

    void handleSwitchAccess(uint8_t low4);
    /// MAME `floppy_image_device::seek_phase_w`: settle the head into the
    /// well of the current 4-bit magnet pattern, capped at ±4 quarter-
    /// tracks per call. Called from handleSwitchAccess after every phase
    /// soft-switch hit (rising AND falling).
    void seekPhaseW(int phases);
    // Legacy 32-cycle gate body, retained as a fallback when no P6 PROM
    // is loaded.
    void legacyAdvance(int cycles);

    // //c / //c+ on-board IWM. Non-owning. When set, DiskIICard
    // forwards floppy + head-position changes via `iwm_->setFloppy`
    // so the IWM state machine stays in lock-step with the active
    // drive (matches MAME `apple2e.cpp:1180-1185 phases_w` chain,
    // which calls `m_iwm->set_floppy(...)` on every reseat).
    pom2::IWMDevice* iwm_ = nullptr;
    void pushIwmFloppy();

    // ── MAME wozfdc API ─────────────────────────────────────────────────
    //
    // Verbatim ports of `wozfdc_device::lss_start()`, `lss_sync(extra)`,
    // and `control(offset)`. Together these implement the full Disk II
    // controller behaviour as MAME models it: per-LSS-cycle PROM lookup,
    // per-cycle WRITE_DATA toggle detection, and flux event splicing on
    // mode transitions.
    void lssStart();
    void lssSync(uint64_t extraCycles = 0);
    void control(int offset);

public:
    /// Debug-only: dump the last N $C0EC reads with their cycle stamp +
    /// byte value to stderr. Intended for `hero_probe` / boot-dump
    /// diagnostics — set `POM2_DEBUG_DISK_READS=1` to enable capture.
    void dumpRecentReads(size_t maxEntries = 256) const;
private:
    // Ring of (cpuCycleTotal, lssData, qt) at each $C0EC read.
    // Populated only when `POM2_DEBUG_DISK_READS=1`.
    struct ReadLog {
        uint64_t cycle;
        uint8_t  data;
        uint8_t  qt;
    };
    mutable std::vector<ReadLog> readLog_;
    mutable size_t               readLogCursor_ = 0;
    static constexpr size_t      kReadLogCap    = 4096;

    /// MAME `wozfdc_device::control()` cases 0xa/0xb — drive_select
    /// switch. When the motor is on (`active != MODE_IDLE`), MAME calls
    /// `floppy->mon_w(true)` on the old drive (commits in-flight writes
    /// and freezes the flux cursor) and `floppy->mon_w(false)` on the
    /// new drive (sets `revolution_start_time = now`, so the new drive
    /// resumes reading from position 0 of its current track). POM2's
    /// equivalent: flush the write buffer to the old drive, then reset
    /// the LSS-cycle base so the new drive's flux stream starts fresh.
    /// No-op if `newDrive == activeDrive`.
    void selectDrive(int newDrive);

    // Boot-trace one-shot flags (POM2_DEBUG_DISK=1). Reset at onReset().
    struct {
        bool sawSlotRom     = false;
        bool sawDevSelect   = false;
        bool sawMotorOn     = false;
        bool sawFirstNibble = false;
    } trace;
};

#endif // POM2_DISK_II_CARD_H
