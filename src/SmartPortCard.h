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

// SmartPortCard — Apple II expansion card that gives a //e (or II+) access
// to ProDOS block-level disks the same way the //c+ on-board SmartPort
// does. Modelled after the real "Apple II 3.5 Disk Controller Card" (a.k.a.
// the "Liron" card, Apple part 670-0186), slot 5 by convention.
//
// Architecture choice — block-level vs IWM-level:
//
// The real card carries a full IWM plus a tiny 6502 ROM with the SmartPort
// dispatcher; ProDOS calls the dispatcher and the dispatcher talks GCR to
// whichever drive the user clicked. POM2's //c+ profile already emulates
// that full stack (`IWMDevice`, `SmartPortHub`, `Sony35Drive`). For a slot-
// plugged card on //e we don't need the bit-level emulation — no game does
// flux-level tricks through the Liron — so this class skips the IWM and
// exposes the underlying volume blocks directly through a streaming
// protocol identical to `ProDOSHardDiskCard`'s.
//
// Per-unit polymorphism:
//
// The original v0.5 incarnation hardcoded two `Disk35Image*` slots. This
// design replaces them with a `std::array<unique_ptr<SmartPortUnit>, kMaxUnits>`
// where each slot can hold any concrete `SmartPortUnit` (3.5", HDV, future
// types). ProDOS sees the first two units as drive 1 / drive 2 of slot N;
// SmartPort calls enumerate additional units. There are up to 14 bays,
// with 8 visible by default; setUnitCount selects an even count from 2-14.
//
//   $C0n0 write = drive select (value modulo the configured unit count)
//   $C0n1 write = block LO byte (selected unit)
//   $C0n2 write = block HI byte (selected unit)
//   $C0n3 read  = next byte of selected block (auto-incrementing)
//   $C0n3 write = next byte INTO selected block (write-back gated)
//   $C0n4 read  = status: bit 7 = no media, bit 6 = write-protected,
//                 bit 0 = last block transfer hit an I/O error (out-of-range
//                 / rejected block) — the ROM driver tests this after a
//                 read/write stream and returns carry-set (ProDOS $27) so a
//                 failed transfer is never reported as success.
//
// Slot ROM ($Cn00-$CnFF) holds a ProDOS block driver. The synthetic
// fallback has the signature below; loadLironRom preserves the real ROM
// identity ($Cn07=$00, $CnFE=$BF, $CnFF=$0A) under HLE service overlays:
//
//   $Cn01 = $20     (ProDOS signature)
//   $Cn03 = $00
//   $Cn05 = $03
//   $Cn07 = $01     (ProDOS block device — see buildRom's rationale; the
//                    $3C "full SmartPort" signature broke //c-class boot)
//   $CnFE = $13     (read+write+status, 2 logical units)
//   $CnFF = $50     (driver entry at $Cn50)
//
// The driver examines ProDOS's $43 unit byte (bit 7 = drive 2) and routes
// to the corresponding pair of soft switches. Boot routine at $Cn20
// reads block 0 of unit 0 to $0800 and JMPs $0801 — identical pattern to
// the HDV card so existing ProDOS bootstraps work unchanged.

#ifndef POM2_SMARTPORT_CARD_H
#define POM2_SMARTPORT_CARD_H

#include "MountableMediaCard.h"
#include "SlotPeripheral.h"
#include "SmartPortBusDevice.h"
#include "SmartPortUnit.h"

#include <array>
#include <optional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class FloppySoundSink;

namespace pom2 {

class SmartPortCard : public SlotPeripheral, public MountableMediaCard
{
public:
    static constexpr int    kDefaultSlot   = 5;
    static constexpr size_t kBlockBytes    = SmartPortUnit::kBlockBytes;
    /// Fourteen addressable bays; eight are exposed by default. Units 1-2
    /// are ProDOS's drive 1/2 of the slot; additional units are enumerated
    /// through SmartPort STATUS and remapped by ProDOS 8 2.4+ onto empty
    /// slots. `unitCount()` selects the visible even count from 2 to 14;
    /// a saved `smartport_slotN_units` value overrides the default.
    static constexpr size_t kMaxUnits      = 14;
    static constexpr int    kDefaultUnits  = 8;

    /// Snapshot blob version. v1 = transfer state (+ an optional v1.1 call
    /// engine tail); v2 appends one media-identity hash per unit so a primed
    /// write block is never flushed into a disk that was swapped in after
    /// the capture. Older blobs still load. v3 (2026-09-08) carries
    /// eight per-unit records instead of two; v4 carries fourteen.
    /// Older blobs keep their original record count; extra bays are reset.
    static constexpr uint8_t kSnapVersion  = 4;

    /// `slot` is baked into the slot ROM (signature byte, driver
    /// address, soft-switch trampolines). All units start empty;
    /// the host plugs / replaces units via `setUnit`.
    explicit SmartPortCard(int slot);
    void setSuperDriveMode(bool enabled);
    bool isSuperDriveMode() const { return superDriveMode_; }

    int getSlot() const { return slot_; }

    /// Replace the unit at `idx` (0..kMaxUnits-1). Pass nullptr to
    /// clear the slot. Returns the old unit so the caller can
    /// inspect / destroy it (typical use: just let unique_ptr drop).
    /// Idempotent on an already-empty slot.
    std::unique_ptr<SmartPortUnit> setUnit(size_t idx,
                                           std::unique_ptr<SmartPortUnit> u);

    /// A host folder mounted on an empty bay turns it into an HDV unit for
    /// the session only (StorageCoordinator::mountHostFolderOnSmartPort).
    /// `borrowBayType` remembers what the bay was; `persistedBayKind` is the
    /// kind the settings must keep — the borrowed one while the loan lasts,
    /// so ejecting the folder no longer saves `_type=hdv` over the user's
    /// 3.5" unit. Any later setUnit (a user type change) ends the loan.
    void borrowBayType(int bay, std::string originalKind);
    std::string persistedBayKind(int bay) const;

    /// The same units, as SmartPort **bus** devices for a //c's external
    /// port (SlotPeripheral::smartPortBusUnit). On the 32 KB //c this card's
    /// $C500 page is never punched in: the machine's own firmware speaks to
    /// these over the IWM, enumerates them and boots from them.
    /// The number of units the card answers for — STATUS unit 0's device
    /// count, the SmartPort unit range, the media rows. 2, 4, ..., 14; any
    /// other value is rounded to the next even number in that range. Bays
    /// beyond it keep their media but are invisible to the guest.
    void setUnitCount(int n);
    int  unitCount() const { return unitCount_; }

    int                    smartPortBusUnitCount() const override;
    pom2::SmartPortBusUnit* smartPortBusUnit(int index) override;

    /// Borrowed access for the panel UI. Nullable.
    SmartPortUnit*       unit(size_t idx)
    { return idx < kMaxUnits ? units_[idx].get() : nullptr; }
    const SmartPortUnit* unit(size_t idx) const
    { return idx < kMaxUnits ? units_[idx].get() : nullptr; }

    std::vector<pom2::Block512Backing*> blockBackings() override
    {
        std::vector<pom2::Block512Backing*> result;
        for (auto& u : units_) result.push_back(u ? u->blockBacking() : nullptr);
        return result;
    }
    std::vector<pom2::AutosavedMedium*> autosavedMedia() override
    {
        std::vector<pom2::AutosavedMedium*> result;
        for (auto& u : units_) result.push_back(u ? u->autosavedMedium() : nullptr);
        return result;
    }

    /// Currently selected unit (set by $C0n0 writes; resets to 0 on
    /// reset). The ProDOS driver re-latches it on every dispatch.
    size_t activeUnit() const { return activeUnit_; }

    /// Mechanical sound sink (head step / motor whirr / click). Optional;
    /// null = silent. Block-level transfers only, so we emit `step()`
    /// once per actual block read/write and gate `motor()` with a
    /// wall-clock-ish spin-down derived from `advanceCycles`.
    /// MainWindow wires this to EmulationController::floppySound35().
    void setFloppySound(FloppySoundSink* fs) { sound_ = fs; }

    /// Load the REAL Liron controller ROM (BMOW/Yellowstone dump,
    /// roms/liron.rom, 4 KB = 8 per-slot $Cn00 pages at n×256 + the 2 KB
    /// $C800 bank at 2048). When loaded, buildRom() serves the real page
    /// for this slot — authentic identity bytes ($Cn07=$00 SmartPort
    /// class, $CnFB=$00, $CnFE=$BF, $CnFF=$0A → ProDOS entry $Cn0A) —
    /// with POM2's HLE service entries overlaid at $Cn00/$Cn0A/$Cn0D and
    /// $Cn20-$CnE2 (the real firmware's IWM/UniDisk code cannot run
    /// without the drive-side 65C02). Do NOT load on //c-class machines:
    /// the on-board $C500 stub keeps the synthetic $Cn07=$01 identity so
    /// the //c boot scan never SmartPort-enumerates it (see
    /// project_iic_smartport_boot). Call before the first slotRomRead.
    bool loadLironRom(const std::string& path);
    bool isLironRomLoaded() const { return lironLoaded_; }

    // ── SlotPeripheral interface ────────────────────────────────────────
    std::string_view name() const override {
        return superDriveMode_ ? "Apple IIe SuperDrive (block interface)" : "SmartPort";
    }

    /// True when the hand-assembled slot ROM did not assemble — a region
    /// over budget, two regions overlapping, a branch that cannot reach its
    /// target, a label nobody defined. Only a test should look at this; a
    /// correct build is always false. See SlotRomAsm.h.
    bool romLayoutError() const { return romLayoutError_; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead      (uint8_t low8) override;
    uint8_t expansionRomRead (uint16_t offset) override;
    bool    peekSlotRom(uint8_t low8, uint8_t& out) const override
    { out = rom_[low8]; return true; }
    bool    peekExpansionRom(uint16_t offset, uint8_t& out) const override
    { out = offset < c800_.size() ? c800_[offset] : 0xFF; return true; }
    bool    takesC800() const override { return true; }  // MAME a2bus.h:145 take_c800
    bool    exposesIicOnboardRom() const override;
    void    onReset() override;
    void    advanceCycles(int cycles) override;

    // Rewind/snapshot: transfer-in-progress state (unit select, block
    // registers, 512-byte stream position, half-filled write buffer,
    // latched I/O errors). Without these, a rewind landing mid-block
    // desynced streamOffset_ against the restored CPU loop → one corrupted
    // block transfer after every such rewind. Media/paths are NOT
    // serialized (host job); the read cache is just invalidated on load —
    // it re-fills from the same block.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ── MountableMediaCard: one bay per unit, with per-bay type select ──
    // (empty / 3.5" / HDV). Lets the Slot Manager drive each unit
    // generically. NOTE: persistence (smartport_slotN_unitK_*) is the
    // host's (MainWindow's) job — these only touch in-memory unit state.
    int  bayCount() const override { return unitCount_; }
    MediaBayInfo bayInfo(int bay) const override;
    uint64_t bayMediaIdentity(int bay) const override;
    bool mountBay(int bay, const std::string& path, std::string& errOut) override;
    bool adoptBay(int bay, Block512Backing::PreparedImage&& prepared,
                  std::string& errOut) override;
    bool ejectBay(int bay) override;
    /// Write this bay's pending changes back WITHOUT ejecting. The card had
    /// no override, so it inherited `MountableMediaCard`'s default — which
    /// REFUSES any dirty bay ("no flush path"). That default is a guard for
    /// cards that genuinely cannot flush; a SmartPort unit can
    /// (`SmartPortUnit::saveDirty`), and the missing override is what left
    /// `StorageCoordinator::setMediaBayType` with nothing safe to call before
    /// it destroyed a dirty unit (bug hunt 4 #3).
    bool flushBay(int bay, std::string& errOut) override;
    bool prepareFlushBay(int bay, PendingBayFlush& out,
                         std::string& errOut) override;
    void restoreFlushBayDirty(int bay, uint64_t lineage) override;
    bool prepareEjectBay(int bay, Block512Backing::PendingWriteBack& out,
                         std::string& errOut) override;
    void restoreBayDirty(int bay,
                         const std::vector<uint32_t>& indices) override;
    void setBayWriteBack(int bay, bool on) override;
    void setBayHostWriteProtected(int bay, bool on) override;
    std::vector<std::pair<std::string, std::string>>
         bayTypeOptions(int bay) const override;
    void setBayType(int bay, const std::string& kindKey) override;

private:
    int  slot_;
    std::array<std::unique_ptr<SmartPortUnit>, kMaxUnits> units_{};
    std::array<std::optional<std::string>, kMaxUnits> borrowedKind_{};

    /// SmartPortBusUnit over a SmartPortUnit. Bound to a bay, not a unit
    /// object, so a swap through setUnit() is seen on the next call.
    /// The unit's mount identity (Block512Backing / Disk35Image mountId).
    static uint64_t unitMediaId(const SmartPortUnit* u);
    /// What the GUEST sees: a medium in bay `idx`, unless the guest ejected
    /// it and the UI thread has not taken it out yet (requestGuestEject).
    bool guestLoaded(size_t idx) const
    {
        if (idx >= kMaxUnits) return false;
        const SmartPortUnit* u = units_[idx].get();
        if (!u || !u->isLoaded()) return false;
        const int bay = static_cast<int>(idx);
        return !guestDoorArmed(bay) || !guestDoorOpen(bay, unitMediaId(u));
    }

    class BusUnit final : public pom2::SmartPortBusUnit {
    public:
        void bind(SmartPortCard* card, size_t bay) { card_ = card; bay_ = bay; }
        bool     hasMedia()       const override;
        uint32_t blockCount()     const override;
        bool     writeProtected() const override;
        bool     readBlock (uint32_t block, uint8_t out[512]) override;
        bool     writeBlock(uint32_t block, const uint8_t in[512]) override;
        bool     isUnidisk35()    const override;
        uint64_t mediaIdentity()  const override;
        void     requestEject()         override;
    private:
        SmartPortUnit* target() const
        { return card_ ? card_->units_[bay_].get() : nullptr; }
        SmartPortCard* card_ = nullptr;
        size_t         bay_  = 0;
    };
    std::array<BusUnit, kMaxUnits> busUnits_{};
    std::array<uint8_t, 256> rom_{};
    /// True when a hand-assembled ROM region did not fit its budget. Always
    /// false in a correct build; pinned by `smartport_rom_layout`, which is
    /// the check the silent $CnC0 overwrite got past.
    bool                     romLayoutError_ = false;

    // Per-unit transfer state. The byte-stream protocol auto-increments
    // `streamOffset_[u]` per access, wrapping every 512 B. Drive select
    // ($C0n0) latches `activeUnit_` for $C0n3 (data) / $C0n4 (status)
    // — block setup ($C0n1/2) writes to the active unit's register pair.
    size_t   activeUnit_                 = 0;
    int      unitCount_                  = kDefaultUnits;   // see setUnitCount
    bool     superDriveMode_             = false;
    std::array<uint16_t, kMaxUnits> selectedBlock_{};
    std::array<size_t,   kMaxUnits> streamOffset_{};
    std::array<std::array<uint8_t, kBlockBytes>, kMaxUnits> readCache_{};
    std::array<bool,     kMaxUnits> readCacheValid_{};
    std::array<uint16_t, kMaxUnits> readCacheBlock_{};
    std::array<std::array<uint8_t, kBlockBytes>, kMaxUnits> writeBuf_{};
    std::array<bool,     kMaxUnits> writeBufPrimed_{};
    // Set when the active unit's readBlock/writeBlock fails (out-of-range /
    // rejected). Surfaced as $C0n4 bit 0; cleared at the start of each
    // transfer (unit-select / block-register write). See buildRom().
    std::array<bool,     kMaxUnits> ioError_{};

    // ── Sound state ─────────────────────────────────────────────────────
    FloppySoundSink* sound_           = nullptr;
    /// Rising edge of `ioError_[u]`. The byte stream calls this once per
    /// failed block, not once per byte — a 512-byte read must not machine-gun
    /// the Virtual ][ grunt.
    void noteIoError(size_t unit);
    uint64_t         cpuCycleTotal_   = 0;
    uint64_t         lastAccessCycle_ = 0;
    bool             audibleMotorOn_  = false;
    static constexpr uint64_t kSpinDownCycles = 500'000;  // ~0.5 s @ 1 MHz

    // ── Real Liron ROM (optional) + $C800 bank ─────────────────────────
    std::vector<uint8_t>      lironRom_;       // 4 KB when loaded
    bool                      lironLoaded_ = false;
    std::array<uint8_t, 2048> c800_{};         // expansion bank ($C800-$CFFE)

    // ── SmartPort-protocol call engine (entry $Cn0D → $CE00 handler) ────
    // The 6502 stub in c800_ collects [cmd + the first 10 param-list
    // bytes] through reg 0x7 after a BEGIN (write reg 0xE), then a read
    // of reg 0xE executes the call here and returns the SmartPort error
    // code ($00 = ok). Results (STATUS payloads, READ data) stream out of
    // reg 0x9, sized by regs 0xB/0xC; WRITE data streams into the legacy
    // reg 0x3 machinery (count pages via reg 0xD; post-commit error via
    // reg 0xF). See buildC800() for the stub and spExecute() for the
    // command semantics (STATUS $00 + DIB $03, READ $01, WRITE $02,
    // FORMAT $03, CONTROL $04, INIT $05; cmd >= $0A → $01, same as the
    // Liron dump's CPX #$0A at $CC5B).
    std::array<uint8_t, 11> spCollect_{};      // [0]=cmd, [1..10]=param list
    size_t                  spCollectN_ = 0;
    std::vector<uint8_t>    spResult_;
    size_t                  spResultPos_ = 0;
    uint8_t                 spPushPages_ = 0;  // 512-byte WRITE → 2
    uint8_t                 spError_     = 0;

    void    buildRom();
    void    buildC800();
    uint8_t spExecute();
    uint8_t readDataByte();
    void    writeDataByte(uint8_t v);
    uint8_t statusByte() const;

    uint8_t blockCountByte(int which) const;   // 0 = low, 1 = high (STATUS)
    /// One block access on `unit` (null = the active one): lights that
    /// unit's access LED and, when a floppy sound device is attached,
    /// drives the audible motor/seek. The unit is explicit because the
    /// SmartPort `$Cn0D` dispatch addresses a unit by number, which need
    /// not be the one the legacy streaming registers have selected.
    void    noteAccess(SmartPortUnit* unit = nullptr);
};

} // namespace pom2

#endif // POM2_SMARTPORT_CARD_H
