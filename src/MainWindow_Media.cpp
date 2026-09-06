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

// MainWindow_Media — mount, eject and boot policy, plus the slot-bus queries
// the rest of the frontend asks its questions through.
//
// The split from MainWindow_StoragePanels is deliberate and is the one worth
// keeping: everything here decides WHAT happens to media (which slot an image
// belongs in, whether a card has to be auto-plugged to boot it, what an eject
// commits), and nothing here draws anything. The panels call in; nothing here
// calls back out to ImGui.
//
// Two rules govern the whole file, both learned the hard way and both
// documented in CLAUDE.md:
//
//   * Never hold `stateMutex` across file I/O. Every mount goes through the
//     two-phase form in MediaMount.h — read and decode unlocked, take the
//     lock only to swap the finished object in. A 32 MB image is 12.8 ms of
//     read against a 20 ms PAL frame, and the lock is held by the CPU worker
//     and the UI thread both.
//   * The `primaryX()` / `xCards()` queries walk SlotBus TOPOLOGY only, which
//     is UI-thread-confined, so they do not take the lock. They must stay
//     that way: adding a read of emulated state to one of them would make
//     every caller wrong at once.

#include "MainWindow.h"

#include "Block512Backing.h"
#include "CffaCard.h"
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "Logger.h"
#include "MediaMount.h"
#include "Memory.h"
#include "MountableMediaCard.h"
#include "ProDOSBlockCard.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SlotCardCatalog.h"
#include "SlotProvisioningCoordinator.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "StorageCoordinator.h"
#include "SuperSerialCard.h"
#include "SystemProfile.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

void MainWindow::bootHdvImage()
{
    pom2::ProDOSBlockCard* dev = hdvDevice();
    if (!dev || !dev->isImageLoaded()) {
        tapeStatusMessage = "HDV boot failed: no image loaded";
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    // Under the lock, same reason as kioskRescanDisks: this is a reference
    // into live card state that another thread can reassign.
    std::string p;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        p = dev->getImagePath();
    }
    // Boot from the slot the HDV/CFFA card is actually plugged in — the user
    // can move it to slot 2 / 7 / etc. via Slot Configuration and the
    // boot path follows. The card's slot ROM bakes its slot number into
    // the ProDOS dispatcher trampolines, so `bootFromSlot(N)` lands on
    // the right $C(N)00 entry point automatically.
    controller->bootFromSlot(dev->getSlot());
    tapeStatusMessage = "Booting HDV (slot " +
        std::to_string(dev->getSlot()) + "): " + p;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

bool MainWindow::ejectMediaBay(int slot, int index, bool diskII)
{
    // Addressed by SLOT + bay rather than by a card pointer: the status bar
    // builds its rows from a value snapshot taken under the lock and released
    // before drawing, so by the time the user clicks, any pointer captured
    // back then could belong to a card a Slot-Config Apply has since deleted.
    // Re-resolving through the bus is what makes the click safe — and the
    // coordinator re-resolves under its OWN lock, which is why delegating
    // keeps that property instead of losing it.
    //
    // DELEGATED, not hand-rolled. The duplicate this replaces was the
    // most-clicked eject in the UI and got persistence wrong three ways the
    // coordinator already has covered:
    //   * it built `disk_path_slot<N>` for BOTH drives, so ejecting drive 2
    //     cleared drive 1's path and left `_drive2` set — after a crash or a
    //     kill, drive 1 came back empty and the disk the user had just
    //     ejected came back mounted (`diskIIPathSettingKey` appends
    //     `_drive2` for drive 1, and `restoreMediaFromSettings` reads both).
    //   * it had no `genericMediaCard` branch, so ejecting a Liron 3.5" left
    //     `media_slot<N>_bay<B>_path` set — verbatim the bug
    //     tests/storage_coordinator_test.cpp records as fixed in the
    //     coordinator.
    //   * it skipped the auto-provision / host-folder guards.
    // The eject itself is also a whole-file settings write plus a possible
    // write-back commit; the coordinator splits that into three critical
    // sections so neither runs under `stateMutex` (this file's own header
    // rule, and CLAUDE.md's).
    std::string label;
    if (diskII) {
        label = "slot " + std::to_string(slot) + " drive " +
                std::to_string(index + 1);
    } else {
        label = "slot " + std::to_string(slot);
        // Topology-only read for the label. Taken under the lock because the
        // card can be deleted by a Slot-Config Apply, released before the
        // command runs: `lockState()` is non-recursive and the coordinator
        // takes it itself.
        int bays = 1;
        {
            auto state = controller->lockState();
            auto* media = dynamic_cast<pom2::MountableMediaCard*>(
                state.memory().slotBus().peripheral(slot));
            if (!media) return false;
            bays = media->bayCount();
        }
        if (bays > 1) label += " bay " + std::to_string(index + 1);
    }

    const auto r =
        diskII ? storageCoordinator_->ejectDiskII(*controller, *settings,
                                                  slot, index)
               : storageCoordinator_->ejectMediaBay(*controller, *settings,
                                                    slot, index);

    // A refused eject leaves the medium MOUNTED on purpose: the write-back
    // failed, so dropping it would lose the only copy of the writes.
    tapeStatusMessage = r.ok
        ? ("Ejected " + label)
        : ("Eject refused for " + label + " — " +
           (r.error.empty() ? std::string("the image could not be saved")
                            : r.error) +
           ", so it stays mounted");
    tapeStatusUntil = lastFrameTime + (r.ok ? 3.0 : 8.0);
    if (!r.ok) pom2::log().warn("Media", tapeStatusMessage);
    return r.ok;
}

void MainWindow::ejectAllDisks()
{
    // One locked pass over the whole topology — Disk II (BOTH drives), every
    // block device, every SmartPort unit and the on-board 3.5" pair — with the
    // settings writes applied after the lock is released.
    //
    // Two things the hand-rolled version got wrong. It called `ejectDisk()`
    // with the default argument, so it only ever ejected drive 1 and left
    // drive 2's medium mounted with its path still persisted. And it ignored
    // every eject's return value, so a medium whose write-back failed was
    // reported as ejected: the failure is exactly when the user needs to know,
    // because the card keeps that disk mounted so the write can be retried.
    const auto result = storageCoordinator_->ejectAllMedia(*controller, *settings);

    if (!result.ok()) {
        std::string msg = "Eject failed (media left mounted): ";
        for (size_t i = 0; i < result.failures.size(); ++i)
            msg += (i ? "; " : "") + result.failures[i];
        tapeStatusMessage = std::move(msg);
    } else {
        tapeStatusMessage = result.changed ? "Eject completed"
                                           : "Nothing was mounted";
    }
    tapeStatusUntil = lastFrameTime + 3.0;
}

bool MainWindow::routeMount35(int driveIdx, const std::string& path,
                              std::string& errOut)
{
    // One routing rule for 3.5" media, in the coordinator: a plugged SmartPort
    // card's units own it (auto-creating a SmartPort35Unit on the target bay,
    // flushing whatever was there first so a failed write-back aborts the
    // mount rather than losing the writes), and only without one does it fall
    // through to the //c+ on-board hub. Callers keep this signature; the CLI
    // insert+boot path and the Library share it.
    const auto r = storageCoordinator_->mountDisk35(*controller, *settings,
                                                    driveIdx, path);
    if (!r.ok) errOut = r.error;
    return r.ok;
}

bool MainWindow::routeMountHdv(const std::string& path, int& bootSlotOut,
                               std::string& errOut)
{
    // On //c-class the cffa/hdv slot cards are ROM-masked by the forced
    // INTCXROM and can't boot ($Cn00 reads internal ROM, not the card) —
    // the only bootable block device is the on-board SmartPort (slot 5),
    // so skip the dedicated-HDV-card branch there and route HDV to the
    // SmartPort unit. See project_iic_smartport_boot.
    const bool iicClass =
        (activeProfile == pom2::SystemProfile::AppleIIc ||
         activeProfile == pom2::SystemProfile::AppleIIcPlus ||
         activeProfile == pom2::SystemProfile::AppleIIcPAL);
    // Prefer a dedicated HDV-class card — the MAME-faithful CffaCard if
    // plugged, else the synthetic ProDOSHardDiskCard; else route to a
    // SmartPort card's unit 0 (auto-creating a SmartPortHdvUnit). Promoted
    // from a lambda in renderDiskLibraryWindow.
    if (!iicClass) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            // Two-phase: up to 32 MiB read with no lock held, then the lock
            // only for the 2IMG parse and the swap. This was the largest
            // single stall in the tree — 12.8 ms warm-cache, most of a PAL
            // frame with the machine and the window both stopped.
            // Same two-phase read, and the coordinator writes hdv_path with
            // the mount instead of leaving it for the next shutdown.
            const int slot = dev->getSlot();
            const auto r = storageCoordinator_->mountMediaBay(
                *controller, *settings, slot, 0, path);
            if (!r.ok) {
                errOut = r.error;
                hdvStatus = "no image mounted";
                return false;
            }
            hdvPath = path;
            hdvStatus = "loaded: " + path;
            bootSlotOut = slot;
            return true;
        }
    }
    if (primarySmartPortCard()) {
        // Through the coordinator: it flushes the outgoing unit before the
        // type swap destroys it, reads the image with no lock held, and
        // writes the unit keys through `appendMediaBaySettingUpdates` — which
        // is where the auto-provisioned-slot guard lives. The hand-rolled
        // block this replaces wrote `smartport_slotN_unit0_*` unconditionally,
        // so a card `ensureSmartPortCardForBoot` plugged for a one-shot
        // drag-and-drop boot — session-local by contract — persisted its
        // media over the user's real configuration.
        const auto mounted = storageCoordinator_->mountHdv(
            *controller, *settings, path, /*smartPortOnly=*/true);
        if (!mounted.ok) {
            errOut = mounted.error;
            return false;
        }
        bootSlotOut = mounted.bootSlot;
        return true;
    }
    errOut = "no HDV or SmartPort card plugged";
    return false;
}

pom2::ProDOSBlockCard* MainWindow::hdvDevice() const
{
    // The CFFA-outranks-HDV preference is the coordinator's rule now, so the
    // menus, the AI server and the boot path cannot drift apart on it.
    return storageCoordinator_->topology(controller->memory().slotBus())
        .preferredBlock();
}

// Bus *topology* reads (which slot holds which card) are UI-thread-confined:
// every writer — plugSlotsFromSettings, applyProfile, the slot-config
// rebuild — runs on this thread, and the worker only ever reads the table.
// A lock here would protect nothing while reading as though it did; per-card
// *state* is a different matter and does go through lockState().
std::vector<pom2::ProDOSBlockCard*> MainWindow::blockCards() const
{
    // Walk the bus (slots 1..7) and cross-cast each plugged peripheral to
    // the ProDOSBlockCard mix-in. Both implementers (ProDOSHardDiskCard,
    // CffaCard) inherit SlotPeripheral *and* ProDOSBlockCard, so the
    // dynamic_cast side-cast succeeds for exactly those and yields nullptr
    // for everything else. Slot order is ascending, matching the "lowest
    // slot is primary" convention used for primaryDiskII()/primaryHdvCard().
    return storageCoordinator_->topology(controller->memory().slotBus())
        .blockCards;
}

std::vector<pom2::SmartPortCard*> MainWindow::smartPortCards() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .smartPortCards;
}

std::vector<DiskIICard*> MainWindow::diskIICards() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .diskIICards;
}

std::vector<SuperSerialCard*> MainWindow::serialCards() const
{
    // Slot-ascending, which is what makes "the primary is the first" true.
    std::vector<SuperSerialCard*> out;
    SlotBus& bus = controller->memory().slotBus();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot)
        if (auto* ssc = dynamic_cast<SuperSerialCard*>(bus.peripheral(slot)))
            out.push_back(ssc);
    return out;
}

SuperSerialCard* MainWindow::primarySerialCard() const
{
    const auto cards = serialCards();
    return cards.empty() ? nullptr : cards.front();
}

DiskIICard* MainWindow::primaryDiskII() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primaryDiskII;
}

ProDOSHardDiskCard* MainWindow::primaryHdvCard() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primaryHdv;
}

pom2::CffaCard* MainWindow::primaryCffaCard() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primaryCffa;
}

pom2::SmartPortCard* MainWindow::primarySmartPortCard() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primarySmartPort;
}

bool MainWindow::flushSlotMedia(std::string& err)
{
    // The coordinator walks the same three families (Disk II pending writes,
    // block devices, SmartPort units) and aggregates the same failure text.
    // It is the flush half of the rebuild transaction — only a successful one
    // may prepare a teardown — so it has to be the same code as the one the
    // rebuild path uses, not a second copy that can drift.
    //
    // Two-phase for the bays that offer it (a Liron's 800 KB 3.5" images):
    // captured under the lock, written with it released. Everything else still
    // writes inline — those cards have no capture path yet, and their callers
    // (quit, profile switch, Apply) all have the CPU worker stopped.
    std::vector<pom2::StorageCoordinator::DeferredFlush> deferred;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        ok = storageCoordinator_->flushAll(controller->memory().slotBus(), err,
                                           &deferred);
    }
    if (!storageCoordinator_->commitDeferredFlushes(*controller, deferred, err))
        ok = false;
    return ok;
}

int MainWindow::ensureHdvCardForBoot()
{
    // Preference order and the session-only marking both live in the
    // coordinator, so the CLI, drag-and-drop and Floppy Emu paths cannot
    // disagree about which target a boot should use.
    //
    // Behaviour change, deliberate: on a //c-class profile this now REFUSES
    // when no SmartPort target exists, instead of falling through to plug an
    // HDV card. Slot cards there are ROM-masked by the forced INTCXROM and
    // cannot boot ($Cn00 reads internal ROM, not the card), so the old
    // fallback conjured a card that could never be booted from and then
    // reported success.
    const auto r = slotProvisioningCoordinator_->ensureHdvBootTarget(
        *controller, *settings, activeProfile);
    if (!r && !r.error.empty()) pom2::log().warn("Slots", r.error);
    return r.slot;
}

int MainWindow::ensureSmartPortCardForBoot()
{
    const auto r = slotProvisioningCoordinator_->ensureSmartPortBootTarget(
        *controller, activeProfile);
    if (!r && !r.error.empty()) pom2::log().warn("Slots", r.error);
    return r.slot;
}

bool MainWindow::insertAndBootImage(const std::string& path, std::string& errOut)
{
    // Classify by extension + size, route into the matching slot under the
    // active profile/slot config, then cold-boot. Shared by the CLI
    // positional-disk / --kiosk launcher and (potentially) any future
    // single-call boot entry point. Mirrors the Disk Library "insert +
    // boot" buttons but with no UI surface.
    //
    // No ROM means no Monitor and no Applesoft: the boot PROM would still
    // pull sector 0 and jump into a loader that calls firmware entry points
    // backed by nothing, hanging on a garbage screen. Say so instead of
    // mounting and then reporting a boot that cannot happen.
    if (!romLoaded_) {
        errOut = "no Apple II ROM loaded — see Help > Welcome / Quick Start";
        return false;
    }
    switch (classifyDiskForSlot(path)) {
        case DiskSlotClass::Floppy525: {
            // Prefer the Disk II in the conventional boot slot 6; fall back
            // to the primary (lowest-slot) card. Booting a single floppy
            // from a non-6 slot is unconventional and breaks software that
            // hardcodes slot 6 for its loader — matters when the config has
            // Disk II in several slots (primary = lowest = e.g. slot 5).
            DiskIICard* target = nullptr;
            for (auto* c : diskIICards()) if (c && c->getSlot() == 6) { target = c; break; }
            if (!target) target = primaryDiskII();
            if (!target) { errOut = "no Disk II card in the current config"; return false; }
            const bool ok = pom2::mountDiskII(*controller, *target, 0, path,
                                              errOut, /*seekTrack0=*/true);
            if (!ok) return false;
            if (!controller->bootFromSlot(target->getSlot())) {
                errOut = "slot " + std::to_string(target->getSlot()) +
                         " did not boot the image (cold-booted instead)";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Sony35: {
            // Without a SmartPort card, routeMount35 falls through to the
            // //c+ on-board Sony hub — a device that only exists on the
            // //c+. On any other machine the image would "mount" into
            // hardware the guest can't see and the cold boot below would
            // land at the BASIC prompt with no error at all.
            //
            // When neither exists, auto-plug a Liron-class SmartPort the
            // same way the HDV branch below auto-plugs a block card: a
            // dropped 800K .po/.2mg is explicit "boot this" intent, and
            // failing it on the stock II+/IIe config (which ships no
            // SmartPort) made drag-and-drop refuse the single most common
            // 3.5" distribution format. Session-local, never persisted.
            if (!primarySmartPortCard() &&
                activeProfile != pom2::SystemProfile::AppleIIcPlus &&
                ensureSmartPortCardForBoot() < 0) {
                errOut = "no 3.5\" device in this config, and no free slot "
                         "to plug a SmartPort 3.5\" card into";
                return false;
            }
            if (!routeMount35(0, path, errOut)) return false;
            // SmartPort card present (incl. //c-class built-in slot 5) →
            // boot it explicitly; otherwise cold-boot (//c+ on-board hub).
            if (primarySmartPortCard()) {
                if (!controller->bootFromSlot(primarySmartPortCard()->getSlot())) {
                    errOut = "slot " + std::to_string(primarySmartPortCard()->getSlot()) +
                             " did not boot the image (cold-booted instead)";
                    return false;
                }
            } else {
                // //c+ on-board Sony hub. The IWM bit-shift state machine is
                // deliberately unmodelled (CLAUDE.md), so this cold boot does
                // NOT reach the mounted 3.5" disk — the image is mounted and
                // the machine restarted, nothing more. Don't call it a boot.
                controller->coldBoot();
                controller->setMode(EmulationController::Mode::Running);
                errOut = "mounted on the //c+ on-board 3.5\" drive, which "
                         "POM2 cannot boot from (unmodelled IWM) — use a "
                         "SmartPort 3.5\" card to boot this image";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Hdv: {
            // Make sure a card exists to host the HDV (auto-plug one if the
            // saved config has none), then route + boot.
            if (ensureHdvCardForBoot() < 0) {
                errOut = "no free slot to plug an HDV card into";
                return false;
            }
            int bootSlot = 0;
            if (!routeMountHdv(path, bootSlot, errOut)) return false;
            if (!controller->bootFromSlot(bootSlot)) {
                errOut = "slot " + std::to_string(bootSlot) +
                         " did not boot the image (cold-booted instead)";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Unknown:
        default:
            errOut = "unrecognised disk image (extension/size): " + path;
            return false;
    }
}

bool MainWindow::mountProDOSFolder(const std::string& path, std::string& errOut)
{
    if (ensureHdvCardForBoot() < 0 || !primaryHdvCard()) {
        errOut = "no free slot to plug an HDV card into";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    const auto built = pom2::buildVolumeFromFolder(path, "HOST", bytes);
    if (!built.ok) {
        errOut = built.error;
        return false;
    }

    const auto mounted = storageCoordinator_->mountBlockBytes(
        *controller, *settings, primaryHdvCard()->getSlot(), std::move(bytes),
        std::string("[host folder] ") + path, path);
    if (!mounted.ok) {
        errOut = mounted.error;
        return false;
    }

    pom2::log().info("CLI", "mounted /HOST/ from " + path + " (" +
        std::to_string(built.filesIncluded) + " files, " +
        std::to_string(built.totalBlocks) + " blocks)");
    return true;
}
