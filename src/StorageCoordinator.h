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

// Storage topology and lifecycle coordinator for the host application.
// Emulated media remains owned by its cards; this class owns cross-card
// discovery, safe flush policy and session-only auto-provisioning state.

#ifndef POM2_STORAGE_COORDINATOR_H
#define POM2_STORAGE_COORDINATOR_H

#include "MountableMediaCard.h"
#include "SmartPort_ImGui.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class DiskIICard;
class EmulationController;
class ProDOSHardDiskCard;
class SlotBus;

namespace pom2 {

class CffaCard;
class ProDOSBlockCard;
class Settings;
class SmartPortCard;

/// The ONE definition of a Disk II path settings key: `disk_path_slot<N>`
/// for drive 1, `disk_path_slot<N>_drive2` for drive 2. Exported because the
/// profile-switch remount has to build it inline — it runs inside the
/// stateMutex scope that keeps the SlotBus rebuild atomic, so it cannot call
/// a coordinator command (those take the lock themselves). Every hand-rolled
/// copy of the "_drive2" rule so far has been a bug.
std::string diskIIPathSettingKey(int slot, std::size_t drive);

class StorageCoordinator
{
public:
    /// Ephemeral view of the cards currently owned by SlotBus. The pointers
    /// are never retained by StorageCoordinator and are valid only while the
    /// caller keeps the machine topology stable (normally by holding the
    /// EmulationController state lock).
    struct Topology {
        std::vector<DiskIICard*> diskIICards;
        std::vector<ProDOSBlockCard*> blockCards;
        std::vector<SmartPortCard*> smartPortCards;
        DiskIICard* primaryDiskII = nullptr;
        ProDOSHardDiskCard* primaryHdv = nullptr;
        CffaCard* primaryCffa = nullptr;
        SmartPortCard* primarySmartPort = nullptr;

        DiskIICard* diskIIAt(int slot) const noexcept;
        /// Legacy single-target HDV policy: prefer CFFA, then synthetic HDV.
        ProDOSBlockCard* preferredBlock() const noexcept;
    };

    struct InventorySnapshot {
        std::vector<int> diskIISlots;
        std::vector<int> blockSlots;
        std::vector<int> smartPortSlots;
        int primaryDiskIISlot = -1;
        int primaryHdvSlot = -1;
        int primaryCffaSlot = -1;
        int primarySmartPortSlot = -1;
        bool primaryDiskUsingBitLss = false;
        bool primaryDiskLoaded = false;
        bool primaryHdvLoaded = false;
        bool primarySmartPortLironRomLoaded = false;

        bool hasDiskII() const noexcept { return primaryDiskIISlot >= 0; }
        bool hasBlockDevice() const noexcept
        {
            return primaryCffaSlot >= 0 || primaryHdvSlot >= 0;
        }
        bool hasSmartPort() const noexcept
        {
            return primarySmartPortSlot >= 0;
        }
    };

    /// Value-only single-medium state captured while SlotBus is stable.
    struct SlotMediumSnapshot {
        int slot = -1;
        bool loaded = false;
        std::string path;
        bool writeBackEnabled = false;
    };

    static constexpr std::size_t kDiskIIDriveCount = 2;

    struct DiskIIDriveSnapshot {
        bool loaded = false;
        std::string path;
    };

    /// Disk II write-back is card-wide, while mounted media are per drive.
    struct DiskIISnapshot {
        int slot = -1;
        std::array<DiskIIDriveSnapshot, kDiskIIDriveCount> drives{};
        bool writeBackEnabled = false;
    };

    struct RebuildSnapshot {
        /// Every Disk II card and both of its drives, including empty media.
        std::vector<DiskIISnapshot> diskII;
        /// The legacy HDV policy has one primary synthetic HDV target.
        std::optional<SlotMediumSnapshot> primaryHdv;
        /// CFFA is multi-instance and therefore restored by slot.
        std::vector<SlotMediumSnapshot> cffa;
    };

    struct PanelCommandStatus {
        std::string message;
        double visibleSeconds = 0.0;
    };

    struct RestoreSettingsResult {
        std::vector<std::string> warnings;
        bool ok() const noexcept { return warnings.empty(); }
    };

    /// Result of one immediate frontend storage command. Commands resolve
    /// their target by slot while holding the machine lock; no card alias is
    /// retained across calls.
    struct MediaCommandResult {
        bool ok = false;
        std::string error;
    };

    struct EjectAllResult {
        bool changed = false;
        std::vector<std::string> failures;
        bool ok() const noexcept { return failures.empty(); }
    };

    struct RoutedMediaCommandResult : MediaCommandResult {
        /// Slot to boot after the mount; -1 means the //c+ on-board path.
        int bootSlot = -1;
        bool usesSmartPort = false;
        /// Filled by WOZ conversion with the newly-created writable image.
        std::string outputPath;
    };

    struct Disk35DriveSnapshot {
        bool loaded = false;
        bool motorOn = false;
        int track = 0;
        bool side1 = false;
        bool writeProtected = false;
        std::string path;
        std::string lastError;
        bool hasUnsavedChanges = false;
        bool writeBackEnabled = false;
        bool isWoz = false;
        std::string convertTargetPath;
    };

    struct Disk35Snapshot {
        std::array<Disk35DriveSnapshot, 2> drives{};
        /// Non-negative when a SmartPort card owns the two logical drives;
        /// otherwise the EmulationController on-board pair is authoritative.
        int smartPortSlot = -1;
        bool usesSmartPort() const noexcept { return smartPortSlot >= 0; }
    };

    Topology topology(const SlotBus& bus) const;
    InventorySnapshot captureInventory(EmulationController& controller) const;

    /// Caller must hold the emulation state lock. Every path is copied by
    /// value because card getters expose references into mutable live media.
    RebuildSnapshot captureRebuildSnapshot(const SlotBus& bus) const;

    /// Synchronize the settings consumed by a slot-config rebuild. This does
    /// not save the settings file. Session-only auto-provisioned HDV cards and
    /// synthetic host-folder volumes deliberately do not replace hdv_path.
    void persistRebuildSettings(Settings& settings,
                                const RebuildSnapshot& snapshot) const;

    /// Persist the complete shutdown-facing storage state. Unlike rebuild
    /// synchronization, this clears an absent/ejected HDV, writes its live
    /// write-back policy, and updates the legacy primary Disk II aliases.
    /// The caller is responsible for Settings::save().
    void persistSessionSettings(Settings& settings,
                                const RebuildSnapshot& snapshot) const;

    /// Persist one live Disk II drive immediately after a successful mount or
    /// eject. Caller must hold the emulation state lock.
    bool persistDiskIIDrive(Settings& settings, const SlotBus& bus,
                            int slot, int drive) const;

    /// Immediate Disk II commands used by every frontend surface. Each
    /// operation resolves `(slot, drive)` under the state lock, updates the
    /// compatible per-slot/legacy settings keys on success, then saves the
    /// settings outside the machine critical section.
    MediaCommandResult mountDiskII(EmulationController& controller,
                                   Settings& settings, int slot, int drive,
                                   const std::string& path,
                                   bool seekTrackZero = false) const;
    MediaCommandResult ejectDiskII(EmulationController& controller,
                                   Settings& settings, int slot,
                                   int drive) const;
    MediaCommandResult setDiskIIWriteBack(
        EmulationController& controller, Settings& settings, int slot,
        bool enabled) const;

    /// Generic commands for MountableMediaCard bays (HDV, CFFA and
    /// SmartPort). Persistence routing is owned here rather than duplicated
    /// in MainWindow. Block bytes cover synthetic host-folder volumes.
    MediaCommandResult mountMediaBay(EmulationController& controller,
                                     Settings& settings, int slot, int bay,
                                     const std::string& path) const;
    MediaCommandResult mountBlockBytes(
        EmulationController& controller, Settings& settings, int slot,
        std::vector<std::uint8_t> bytes, const std::string& label,
        const std::string& hostFolder) const;
    MediaCommandResult ejectMediaBay(EmulationController& controller,
                                     Settings& settings, int slot,
                                     int bay) const;
    MediaCommandResult setMediaBayWriteBack(
        EmulationController& controller, Settings& settings, int slot,
        int bay, bool enabled) const;
    MediaCommandResult setMediaBayType(EmulationController& controller,
                                       Settings& settings, int slot, int bay,
                                       const std::string& kind) const;

    /// Authoritative 3.5-inch routing. If a SmartPort card exists its two
    /// units own the drives; otherwise commands target the controller's
    /// on-board pair. A mount safely flushes and replaces a unit of another
    /// kind before loading the requested 3.5-inch image.
    Disk35Snapshot captureDisk35(EmulationController& controller) const;
    RoutedMediaCommandResult mountDisk35(
        EmulationController& controller, Settings& settings, int drive,
        const std::string& path) const;
    MediaCommandResult ejectDisk35(EmulationController& controller,
                                   Settings& settings, int drive) const;
    MediaCommandResult setDisk35WriteBack(
        EmulationController& controller, Settings& settings, int drive,
        bool enabled) const;
    RoutedMediaCommandResult convertDisk35WozToPo(
        EmulationController& controller, Settings& settings,
        int drive) const;

    /// Route an HDV either to the preferred dedicated block card or, when
    /// `smartPortOnly` is true/no block card exists, to SmartPort unit 0.
    /// The SmartPort unit-type replacement follows the same flush-before-
    /// destroy rule as the 3.5-inch command.
    RoutedMediaCommandResult mountHdv(
        EmulationController& controller, Settings& settings,
        const std::string& path, bool smartPortOnly) const;

    /// Eject every slot-owned medium (both Disk II drives, every HDV/CFFA,
    /// every SmartPort unit). Failed write-back leaves the affected medium
    /// mounted and is reported without aborting the other targets.
    EjectAllResult ejectAllMedia(EmulationController& controller,
                                 Settings& settings) const;

    /// Restore all newly-built Disk II, HDV, CFFA and SmartPort media from
    /// settings.
    /// The lowest-slot Disk II accepts the legacy unsuffixed fallback. Missing
    /// files are intentionally ignored; valid files rejected by a card are
    /// reported without aborting restoration of the remaining media. Caller
    /// must hold the emulation state lock.
    RestoreSettingsResult restoreMediaFromSettings(
        SlotBus& bus, const Settings& settings) const;

    /// Caller must hold the emulation state lock. Missing files and card-type
    /// changes are ignored, matching the startup/profile restoration policy.
    void restoreRebuildSnapshot(SlotBus& bus,
                                const RebuildSnapshot& snapshot) const;

    /// One bay's flush payload, lifted out under the lock and committed
    /// without it. See `flushAll`.
    struct DeferredFlush {
        int         slot = -1;
        int         bay  = 0;
        std::string label;      ///< how the failure names the bay
        MountableMediaCard::PendingBayFlush payload;
    };

    /// Caller must hold the emulation state lock. Failure leaves every card
    /// alive and reports all media which remain dirty/retryable.
    ///
    /// `deferred` non-null opts into the two-phase form: bays that support it
    /// (a `LironCard`'s 800 KB 3.5" images) are CAPTURED here instead of
    /// written, and the caller must hand the vector to `commitDeferredFlushes`
    /// once it has released the lock. Null keeps the inline behaviour, which
    /// is what a single-threaded caller wants.
    bool flushAll(const SlotBus& bus, std::string& error,
                  std::vector<DeferredFlush>* deferred = nullptr) const;

    /// Phase 2 of `flushAll`, with NO lock held: write what it captured. A
    /// failed payload re-marks its bay dirty (phase 3, which re-takes the
    /// lock through `controller`) so the next flush tries again, and is
    /// appended to `error` with the same aggregation as phase 1.
    bool commitDeferredFlushes(EmulationController& controller,
                               std::vector<DeferredFlush>& deferred,
                               std::string& error) const;

    /// Frontend boundary for the slot SmartPort panel. Both operations
    /// resolve the card from SlotBus while the machine lock is held; callers
    /// never retain SmartPortCard/SmartPortUnit aliases between frames.
    SmartPort_ImGui::CardSnapshot
    captureSmartPortPanel(EmulationController& controller) const;
    PanelCommandStatus applySmartPortPanel(
        EmulationController& controller, Settings& settings, int slot,
        const SmartPort_ImGui::Result& command) const;

    int autoProvisionedHdvSlot() const noexcept { return autoHdvSlot_; }
    int autoProvisionedSmartPortSlot() const noexcept { return autoSmartPortSlot_; }
    void markAutoProvisionedHdv(int slot) noexcept { autoHdvSlot_ = slot; }
    void markAutoProvisionedSmartPort(int slot) noexcept { autoSmartPortSlot_ = slot; }
    void clearAutoProvisioned() noexcept
    {
        autoHdvSlot_ = -1;
        autoSmartPortSlot_ = -1;
    }

private:
    int autoHdvSlot_ = -1;
    int autoSmartPortSlot_ = -1;
};

} // namespace pom2

#endif // POM2_STORAGE_COORDINATOR_H
