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

// MainWindow_StoragePanels — the ImGui bodies for every storage window: the
// Disk II controller, the disk library, SmartPort, HDV, the 3.5" controller,
// the BMOW Floppy Emu, and the four file dialogs that feed them.
//
// Moved out of MainWindow.cpp verbatim. Two file-scope helpers came with
// them because nothing else used either: the `@PRODOS_HOST_FOLDER@:` sentinel
// that lets a library entry stand for a host folder rather than a file, and
// `freePoNameFor`, which picks the name for a 3.5" WOZ's writable twin.
//
// These are PANELS, not policy: mounting, ejecting and boot routing live in
// MainWindow_Media.cpp, and the snapshot-render-dispatch shape they all use
// is the LeChatMauve_ImGui pattern — read state under the lock, render from
// the snapshot, act under the lock again.

#include "MainWindow.h"

#include "Block512Backing.h"
#include "CffaCard.h"
#include "Disk35Controller_ImGui.h"
#include "Disk35Image.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "DiskLibrary_ImGui.h"
#include "EmulationController.h"
#include "FloppyEmuDevice.h"
#include "FloppyEmu_ImGui.h"
#include "HdvController_ImGui.h"
#include "IconsFontAwesome6.h"
#include "Logger.h"
#include "MediaMount.h"
#include "Memory.h"
#include "MountableMediaCard.h"
#include "Pom2Theme.h"
#include "ProDOSBlockCard.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "SmartPort_ImGui.h"
#include "SystemProfile.h"
#include "StatusLed.h"
#include "StorageCoordinator.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {
// Sentinel prefix used in HdvController_ImGui::LibraryEntry::fullPath to
// flag the synthetic prodos_folder/ host-folder mount. The dispatcher in
// renderHdvPanelWindow detects this prefix and routes to the synthesiser
// instead of treating the path as a real .hdv file.
constexpr const char* kProDOSHostSentinel = "@PRODOS_HOST_FOLDER@:";
} // namespace

namespace {
// Where a 3.5" WOZ's writable twin should go: same directory, same stem,
// `.po`. Never overwrites — appends " (2)", " (3)" … until the name is free,
// and gives up rather than looping if a hundred already exist. Returns ""
// when no free name could be found, which greys the button out.
std::string freePoNameFor(const std::string& wozPath)
{
    if (wozPath.empty()) return {};
    namespace fs = std::filesystem;
    const fs::path src(wozPath);
    const fs::path dir  = src.parent_path();
    const std::string stem = src.stem().string();
    std::error_code ec;
    for (int n = 1; n <= 99; ++n) {
        const std::string name =
            (n == 1) ? stem + ".po"
                     : stem + " (" + std::to_string(n) + ").po";
        const fs::path cand = dir / name;
        if (!fs::exists(cand, ec)) return cand.string();
    }
    return {};
}
}  // namespace

void MainWindow::renderDiskPanelWindow()
{
    if (!show(pom2::PanelId::DiskII)) return;

    // Disk library is the same for every plugged DiskII (it's the
    // contents of disks_5.4/ on disk). Build it once and share via copy.
    std::vector<pom2::DiskController_ImGui::LibraryEntry> sharedLibrary;
    // Disk library — scan disks_5.4/ recursively for .dsk/.do/.po/.nib/.woz.
    // Sub-folders are honoured so users can shelve their library by
    // format (`disks_5.4/dsk/`, `disks_5.4/woz/`, …) or by collection
    // (`disks_5.4/games/`, `disks_5.4/dev/`, …) without losing the one-click
    // boot. Cheap (a few dirent reads per frame); sorted alphabetically
    // so the list doesn't reshuffle as the OS hands us a different
    // dirent order. `displayName` carries the path relative to the
    // scanned root so two files of the same name in different sub-
    // folders don't collide visually.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "disks_5.4", "../disks_5.4", "../../disks_5.4" };
        for (const char* dir : dirCandidates) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                // Skip dotfiles AND dotdirs (.git, .DS_Store, …). On a
                // dotdir we disable_recursion_pending so we don't walk
                // into it on the next ++.
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".dsk" && ext != ".do" && ext != ".po" &&
                    ext != ".nib" && ext != ".woz") continue;
                pom2::DiskController_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath    = entry.path().string();
                sharedLibrary.push_back(std::move(e));
            }
            break;     // first existing candidate dir wins
        }
        std::sort(sharedLibrary.begin(), sharedLibrary.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    // (Auto-turbo lives in updateAutoTurbo(), called every frame from
    // render() — it must run even when this panel window is hidden, which is
    // the default. See MainWindow::updateAutoTurbo.)

    // ── Render one window per plugged DiskII ──────────────────────────
    // Title carries the slot number so ImGui assigns each card its own
    // window state (position, size, dock). The primary (lowest-slot) card
    // gets the curated default position; subsequent cards cascade slightly
    // down/right so they don't perfectly overlap on first show.
    // Hoisted: each call walks the bus, so re-reading it per iteration would
    // make this quadratic in slot count for no gain — the topology cannot
    // change inside one UI-thread loop.
    const auto diskCardList = diskIICards();
    for (size_t idx = 0; idx < diskCardList.size() && idx < diskPanels.size(); ++idx) {
        DiskIICard*                       card  = diskCardList[idx];
        pom2::DiskController_ImGui*       panel = diskPanels[idx].get();
        if (!card || !panel) continue;

        pom2::DiskController_ImGui::DriveSnapshot snap;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            snap.bootRomLoaded     = card->hasBootRom();
            snap.diskLoaded        = card->isDiskLoaded();
            snap.motorOn           = card->isMotorOn();
            snap.track             = card->getCurrentTrack();
            snap.halfTrack         = card->getHalfTrack();
            snap.trackPos          = card->getTrackPosition();
            snap.diskPath          = card->getDiskPath();
            snap.lastError         = card->getLastError();
            snap.writeBackEnabled  = card->isWriteBackEnabled();
            snap.hasUnsavedChanges = card->hasUnsavedChanges();
            snap.fileWriteProtected = card->isFileWriteProtected();
        }
        snap.turboWhileMotor = diskTurboWhileMotor;
        snap.turboActive     = diskTurboActive;
        snap.library         = sharedLibrary;     // shared copy

        const float baseX = 1055.0f, baseY = 30.0f;
        ImGui::SetNextWindowPos (
            ImVec2(baseX - static_cast<float>(idx) * 30.0f,
                   baseY + static_cast<float>(idx) * 30.0f),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(705, 960), ImGuiCond_FirstUseEver);

        char title[64];
        std::snprintf(title, sizeof(title),
                      "Disk II (slot %d)", card->getSlot());
        // Only the primary card honours `show(pom2::PanelId::DiskII)` (the menu toggle).
        // Secondary cards share the same toggle for simplicity — the user
        // sees them appear/disappear together. We feed the same flag to
        // each render() call.
        auto result = panel->render(title, show(pom2::PanelId::DiskII), snap);

        if (result.turboToggleChanged) {
            diskTurboWhileMotor = result.turboNewValue;
        }
        if (result.writeBackToggleChanged) {
            // Persists disk_writeback_slotN with the change — the bare setter
            // did not, so the toggle did not survive a restart, and since
            // isWriteProtected() is `fileWriteProtected || !writeBack` the
            // guest then saw a write-protected disk again.
            (void)storageCoordinator_->setDiskIIWriteBack(
                *controller, *settings, card->getSlot(),
                result.writeBackNewValue);
            tapeStatusMessage = "slot " + std::to_string(card->getSlot()) +
                (result.writeBackNewValue
                    ? ": write-back ENABLED (saves on eject)"
                    : ": write-back disabled");
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        if (result.requestEject) {
            const auto r = storageCoordinator_->ejectDiskII(
                *controller, *settings, card->getSlot(), 0);
            tapeStatusMessage = r.ok
                ? ("Disk ejected (slot " + std::to_string(card->getSlot()) + ")")
                : ("Eject failed: " + r.error);
            tapeStatusUntil   = lastFrameTime + 4.0;
        }
        if (result.requestBoot) {
            auto st = controller->lockState();
            card->seekTrack0();
            const uint16_t pc = static_cast<uint16_t>(
                0xC000 + (card->getSlot() << 8));
            st.cpu().setProgramCounter(pc);
            controller->setMode(EmulationController::Mode::Running);
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Boot: PC → $%04X", pc);
            tapeStatusMessage = msg;
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        if (!result.requestInsertAndBoot.empty()) {
            const std::string path = result.requestInsertAndBoot;
            std::string err;
            const bool ok = pom2::mountDiskII(*controller, *card, 0, path, err,
                                              /*seekTrack0=*/true);
            if (ok) {
                controller->coldBoot();
                controller->setMode(EmulationController::Mode::Running);
                pom2::log().info("Disk II",
                    "slot " + std::to_string(card->getSlot()) +
                    " Library click → insert + boot: " + path);
                tapeStatusMessage = "Booting: " + path;
            } else {
                tapeStatusMessage = "Boot failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        if (!result.requestInsertOnly.empty()) {
            const std::string path = result.requestInsertOnly;
            std::string err;
            const bool ok = pom2::mountDiskII(*controller, *card, 0, path, err);
            if (ok) {
                pom2::log().info("Disk II",
                    "slot " + std::to_string(card->getSlot()) +
                    " Library right-click → insert only: " + path);
                tapeStatusMessage = "Inserted (no boot): " + path;
            } else {
                tapeStatusMessage = "Insert failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 4.0;
        }
    }
}

void MainWindow::renderDiskLibraryWindow()
{
    if (!show(pom2::PanelId::DiskLibrary)) return;

    // Default position: right column of the curated 1568×850 layout,
    // flush against the screen window. 435 px wide × 745 px tall =
    // enough for the 3-tab table + the search/sort header without
    // scroll overflow on a typical 800+ disk library.
    ImGui::SetNextWindowPos (ImVec2(1125, 90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(435,  745), ImGuiCond_FirstUseEver);

    pom2::DiskLibrary_ImGui::CurrentlyMounted mounted;
    // Build the mount snapshot under stateMutex, copying every path BY
    // VALUE — same rule as renderDiskPanelWindow / renderSmartPortPanelWindow
    // / renderFloppyEmuWindow. getDiskPath() & friends return a reference
    // into the card's live DiskImage, and the AI control server's HTTP
    // thread reassigns it from /disk insert + eject. The lock is released
    // before diskLibrary->render() below: that path re-locks.
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        // Currently-inserted Disk II images (per plugged card). The library
        // tags rows with `* ` when their path matches any of these — gives
        // the user a visual cue across all plugged cards at once.
        for (auto* c : diskIICards()) {
            if (!c) continue;
            pom2::DiskLibrary_ImGui::CurrentlyMounted::DiskIICardInfo info;
            info.slot = c->getSlot();
            if (c->isDiskLoaded(0)) {
                info.drive1 = c->getDiskPath(0);
                mounted.diskII.push_back(info.drive1);
            }
            if (c->isDiskLoaded(1)) {
                info.drive2 = c->getDiskPath(1);
                mounted.diskII.push_back(info.drive2);
            }
            mounted.diskIICards.push_back(info);
        }
        // 3.5" mount sources: the //c+ on-board hub OR a slot-plugged
        // SmartPort card's unit 0/1 (one or the other, never both on the
        // same profile). The library marks rows mounted on either, so the
        // user sees the `* ` cue regardless of which path is active.
        mounted.disk35Internal = controller->disk35Internal().isLoaded()
            ? controller->disk35Internal().path() : std::string();
        mounted.disk35External = controller->disk35External().isLoaded()
            ? controller->disk35External().path() : std::string();
        if (primarySmartPortCard()) {
            const pom2::SmartPortUnit* u0 = primarySmartPortCard()->unit(0);
            const pom2::SmartPortUnit* u1 = primarySmartPortCard()->unit(1);
            if (u0 && u0->isLoaded() &&
                u0->kindKey() == pom2::SmartPort35Unit::kKindKey &&
                mounted.disk35Internal.empty()) {
                mounted.disk35Internal = u0->path();
            }
            if (u1 && u1->isLoaded() &&
                u1->kindKey() == pom2::SmartPort35Unit::kKindKey &&
                mounted.disk35External.empty()) {
                mounted.disk35External = u1->path();
            }
        }
        if (pom2::ProDOSBlockCard* dev = hdvDevice(); dev && dev->isImageLoaded()) {
            mounted.hdv = dev->getImagePath();
        } else if (primarySmartPortCard()) {
            // SmartPort-routed HDV — show as mounted in the Library so the
            // `* ` marker matches reality regardless of which path holds it.
            const pom2::SmartPortUnit* u = primarySmartPortCard()->unit(0);
            if (u && u->isLoaded() &&
                u->kindKey() == pom2::SmartPortHdvUnit::kKindKey) {
                mounted.hdv = u->path();
            }
        }
    }

    // Favourites + recents are host state (persisted to state.cfg); the panel
    // only renders them and reports a toggle.
    pom2::DiskLibrary_ImGui::Lists lists;
    lists.favourites   = libraryFavourites_;
    lists.recents      = libraryRecents_;
    lists.hideSizeDate = libraryHideSizeDate_;

    const auto r = diskLibrary->render("Disk Library", show(pom2::PanelId::DiskLibrary),
                                       mounted, lists);

    if (r.toggleHideSizeDate) libraryHideSizeDate_ = !libraryHideSizeDate_;

    if (!r.toggleFavourite.empty()) {
        auto it = std::find(libraryFavourites_.begin(),
                            libraryFavourites_.end(), r.toggleFavourite);
        if (it != libraryFavourites_.end()) libraryFavourites_.erase(it);
        else libraryFavourites_.push_back(r.toggleFavourite);
    }

    // Anything the user actually mounted this frame becomes the newest recent.
    // Driven off the panel's requests rather than off the cards, so a mount
    // that came from the CLI or a drag-and-drop doesn't silently reorder the
    // list behind the user's back.
    for (const std::string* p : { &r.request525InsertAndBoot,
                                  &r.request525InsertOnly,
                                  &r.request35MountAndBoot,
                                  &r.request35MountOnly,
                                  &r.requestHdvMountAndBoot,
                                  &r.requestHdvMountOnly,
                                  &r.requestFloppyEmuMountAndBoot,
                                  &r.requestFloppyEmuMountOnly }) {
        if (p->empty()) continue;
        noteLibraryRecent(*p);
    }

    // ── Eject-all (header-row button, moved here from the toolbar) ─────
    if (r.requestEjectAllDisks) ejectAllDisks();

    // ── 5.25" actions → the DiskII card/drive the right-click menu picked ──
    // request525Slot = -1 means "primary card" (left-click default); a real
    // slot routes to that specific DiskII card. drive 0 = drive 1, 1 = drive 2.
    auto resolve525 = [&](int slot) -> DiskIICard* {
        if (slot < 0) return primaryDiskII();
        for (auto* c : diskIICards()) if (c && c->getSlot() == slot) return c;
        return primaryDiskII();
    };
    if (!r.request525InsertAndBoot.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        const std::string path = r.request525InsertAndBoot;
        std::string err;
        const bool ok = target && pom2::mountDiskII(*controller, *target, drive,
                                                    path, err,
                                                    /*seekTrack0=*/true);
        if (ok && target) {
            // Boot the target card's slot (its boot PROM boots drive 1).
            const bool booted = controller->bootFromSlot(target->getSlot());
            controller->setMode(EmulationController::Mode::Running);
            tapeStatusMessage = std::string("Library: inserted") +
                (booted ? " + booted" : " (slot did not boot — cold-booted)") +
                " (slot " + std::to_string(target->getSlot()) + " drive " +
                std::to_string(drive + 1) + "): " + path;
        } else {
            tapeStatusMessage = "Library: boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.request525InsertOnly.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        std::string err = "no DiskII card";
        if (target && pom2::mountDiskII(*controller, *target, drive,
                                        r.request525InsertOnly, err)) {
            tapeStatusMessage = "Library: inserted (slot " +
                std::to_string(target->getSlot()) + " drive " +
                std::to_string(drive + 1) + ", no boot): " +
                r.request525InsertOnly;
        } else {
            tapeStatusMessage = "Library: insert failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── 3.5" actions ─────────────────────────────────────────────────
    // Routing: on //c+ profile the on-board hub owns 3.5" media; on any
    // other profile with a SmartPort card plugged, the card's units do.
    // The Library click is explicit user intent to mount 3.5" here, so
    // we auto-create a SmartPort35Unit on the target index if the slot
    // is empty or holds a different kind (HDV) — the user can re-pick
    // the type later from the SmartPort Configuration panel.
    // routeMount35 / routeMountHdv are now member methods (shared with the
    // CLI insert+boot path) — see their definitions above.

    if (!r.request35MountAndBoot.empty()) {
        std::string err;
        if (routeMount35(r.request35BootDrive,
                         r.request35MountAndBoot, err)) {
            // Slot-aware boot: explicit `bootFromSlot(N)` whenever a
            // SmartPort card is plugged — now including the //c-class
            // built-in SmartPort (slot 5). No SmartPort card at all means
            // the //c+ on-board Sony hub, whose IWM boot path POM2
            // deliberately does not model: the cold boot below restarts the
            // machine but never reaches the disk, so don't call it a boot.
            bool booted = false;
            if (primarySmartPortCard()) {
                booted = controller->bootFromSlot(primarySmartPortCard()->getSlot());
            } else {
                controller->coldBoot();
            }
            tapeStatusMessage = "Library: 3.5\" drive "
                + std::string(r.request35BootDrive == 0 ? "1" : "2")
                + (booted ? " booted: " : " mounted (did not boot): ")
                + r.request35MountAndBoot;
        } else {
            tapeStatusMessage = "Library: 3.5\" boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.request35MountOnly.empty()) {
        std::string err;
        if (routeMount35(r.request35MountDrive,
                         r.request35MountOnly, err)) {
            tapeStatusMessage = "Library: 3.5\" drive "
                + std::string(r.request35MountDrive == 0 ? "1" : "2")
                + " mounted: " + r.request35MountOnly;
        } else {
            tapeStatusMessage = "Library: 3.5\" mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── HDV actions ──────────────────────────────────────────────────
    // Two routing targets: the legacy `ProDOSHardDiskCard` slot (if
    // plugged) OR a SmartPort card's unit 0 (if a SmartPort is plugged
    // but no HDV card is). The Library click is explicit intent to
    // mount HDV; on a SmartPort-only config, auto-create / replace
    // unit 0 with a SmartPortHdvUnit so users don't have to detour
    // through the SmartPort Configuration panel.
    if (!r.requestHdvMountAndBoot.empty()) {
        const std::string path = r.requestHdvMountAndBoot;
        int bootSlot = 0;
        std::string err;
        if (routeMountHdv(path, bootSlot, err)) {
            const bool booted = controller->bootFromSlot(bootSlot);
            tapeStatusMessage = "Library: HDV (slot " +
                std::to_string(bootSlot) +
                (booted ? ") booted: " : ") mounted, did not boot: ") + path;
        } else {
            tapeStatusMessage = "Library: HDV mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.requestHdvMountOnly.empty()) {
        int bootSlot = 0;
        std::string err;
        if (routeMountHdv(r.requestHdvMountOnly, bootSlot, err)) {
            tapeStatusMessage = "Library: HDV mounted: " + r.requestHdvMountOnly;
        } else {
            tapeStatusMessage = "Library: HDV mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── Floppy Emu SD card (floppyemu/) ───────────────────────────────
    // The SD card is not a bay: it holds 5.25", 3.5" and Smartport images
    // side by side, because the device emulates all of them. So a click
    // here is FILE-driven and goes through the same helper the CLI
    // positional disk uses — which also auto-plugs an HDV card when the
    // saved config has none. (The OLED panel stays MODE-driven: that is
    // what a Floppy Emu *is*. See DiskLibrary_ImGui.h.)
    if (!r.requestFloppyEmuMountAndBoot.empty()) {
        const std::string path = r.requestFloppyEmuMountAndBoot;
        std::string err;
        tapeStatusMessage = insertAndBootImage(path, err)
            ? ("Library: Floppy Emu booted: " + path)
            : ("Library: Floppy Emu boot failed: " + err);
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.requestFloppyEmuMountOnly.empty()) {
        const std::string path = r.requestFloppyEmuMountOnly;
        std::string err;
        bool ok = false;
        switch (classifyDiskForSlot(path)) {
            case DiskSlotClass::Floppy525:
                if (primaryDiskII()) {
                    ok = pom2::mountDiskII(*controller, *primaryDiskII(), 0, path, err);
                } else {
                    err = "no Disk II card in the current config";
                }
                break;
            case DiskSlotClass::Sony35:
                ok = routeMount35(0, path, err);
                break;
            case DiskSlotClass::Hdv: {
                int bootSlot = 0;
                ok = routeMountHdv(path, bootSlot, err);
                break;
            }
            case DiskSlotClass::Unknown:
            default:
                err = "unrecognised disk image (extension/size)";
                break;
        }
        tapeStatusMessage = ok
            ? ("Library: Floppy Emu mounted: " + path)
            : ("Library: Floppy Emu mount failed: " + err);
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── Eject actions ─────────────────────────────────────────────────
    // 5.25": eject from whichever plugged DiskII holds the clicked
    // image. Match by path so multi-instance DiskII setups (the same
    // image plugged into two slots) all clear together.
    if (!r.request525EjectPath.empty()) {
        // Two phases, for the same reason the 3.5" branch below delegates:
        // match under the lock, then eject through the coordinator with the
        // lock RELEASED. The hand-rolled loop this replaces ejected the
        // medium but cleared no settings key at all, so the image was
        // remounted on the next launch unless a clean quit happened to
        // rewrite the key from live state first. It also held `stateMutex`
        // across an eject that can commit a write-back to disk.
        std::vector<std::pair<int, int>> targets;   // (slot, drive)
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            for (auto* c : diskIICards()) {
                if (!c) continue;
                for (int d = 0; d < DiskIICard::kDriveCount; ++d) {
                    if (c->isDiskLoaded(d) &&
                        c->getDiskPath(d) == r.request525EjectPath)
                        targets.emplace_back(c->getSlot(), d);
                }
            }
        }
        bool ok = true;
        std::string err;
        for (const auto& [slot, drive] : targets) {
            const auto e = storageCoordinator_->ejectDiskII(
                *controller, *settings, slot, drive);
            if (!e.ok) {
                ok  = false;
                err = e.error;
            }
        }
        tapeStatusMessage = ok ? "Library: 5.25\" disk ejected"
                               : "Library: 5.25\" eject failed: " + err;
        tapeStatusUntil   = lastFrameTime + 4.0;
    }
    if (r.request35EjectDrive >= 0) {
        // Through the coordinator so the eject follows the SAME routing the
        // mount did. This called controller->eject35() unconditionally, which
        // only ever touches the on-board pair — so with a SmartPort card
        // owning the media the button silently did nothing while the panel
        // went on showing the disk.
        const auto e = storageCoordinator_->ejectDisk35(
            *controller, *settings, r.request35EjectDrive);
        tapeStatusMessage = e.ok
            ? ("Library: 3.5\" drive " +
               std::string(r.request35EjectDrive == 0 ? "1" : "2") + " ejected")
            : ("Library: 3.5\" eject failed: " + e.error);
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (r.requestHdvEject) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            // Through the coordinator, like every other eject on this panel:
            // `ejectImage()` under the lock ran the save-on-eject rewrite of
            // a volume up to 32 MiB with the machine and the window frozen
            // behind it, and cleared no settings key, so the image came back
            // on the next launch.
            const auto e = storageCoordinator_->ejectMediaBay(
                *controller, *settings, dev->getSlot(), 0);
            if (e.ok) {
                hdvPath.clear();
                hdvStatus = "no image mounted";
            }
            tapeStatusMessage = e.ok
                ? "Library: HDV ejected"
                : "Library: HDV eject failed: " + e.error;
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
    }
}

void MainWindow::renderSmartPortPanelWindow()
{
    if (!show(pom2::PanelId::SmartPort)) return;

    // One acquisition for every unit row, with the card resolved from the live
    // bus inside it. The old code fetched a SmartPortUnit* and then reused it
    // across several INDEPENDENT lock acquisitions — snapshot, then type swap,
    // then write-back, then mount, then eject — so a slot rebuild between any
    // two of them left the rest writing through a freed unit.
    const auto snap = storageCoordinator_->captureSmartPortPanel(*controller);

    char title[64];
    if (snap.plugged) {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration (slot %d)", snap.slot);
    } else {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration");
    }

    const auto r = smartPortPanel->render(title, show(pom2::PanelId::SmartPort), snap);

    if (!snap.plugged) return;

    // Re-resolves the card under the lock, applies the whole frame's request
    // in one critical section, and saves settings after unlocking. Unit-type
    // replacement flushes the outgoing unit first, so a failed write-back
    // aborts the swap instead of destroying the dirty medium with it.
    const auto status =
        storageCoordinator_->applySmartPortPanel(*controller, *settings,
                                                 snap.slot, r);
    if (!status.message.empty()) {
        tapeStatusMessage = status.message;
        tapeStatusUntil   = lastFrameTime + status.visibleSeconds;
    }
}

void MainWindow::renderFloppyEmuWindow()
{
    if (!show(pom2::PanelId::FloppyEmu)) return;
    namespace fs = std::filesystem;
    using Mode = pom2::FloppyEmuMode;
    const Mode mode = floppyEmu->mode();

    auto baseName = [](const std::string& p) {
        return fs::path(p).filename().string();
    };
    auto human = [](uint64_t b) -> std::string {
        if (b == 0)              return std::string();
        if (b >= 1024 * 1024)    return std::to_string(b / (1024 * 1024)) + "M";
        if (b >= 1024)           return std::to_string(b / 1024) + "K";
        return std::to_string(b) + "B";
    };
    auto controllerReady = [&](Mode m) -> bool {
        switch (m) {
            case Mode::Disk525:   return primaryDiskII() != nullptr;
            case Mode::Disk35:
            case Mode::Unidisk35: return primarySmartPortCard() != nullptr ||
                                         activeProfile == pom2::SystemProfile::AppleIIcPlus;
            case Mode::SmartportHD: return hdvDevice() != nullptr ||
                                           primarySmartPortCard() != nullptr;
        }
        return false;
    };
    auto controllerHint = [&](Mode m) -> std::string {
        switch (m) {
            case Mode::Disk525:
                return "No Disk II controller — add 'Disk II' in the Slot Manager.";
            case Mode::Disk35:
            case Mode::Unidisk35:
                return "No SmartPort/Liron controller for 3.5\" media.";
            case Mode::SmartportHD:
                return "No SmartPort or HDV controller for hard-disk media.";
        }
        return std::string();
    };
    auto insertedLabel = [&](Mode m) -> std::string {
        // Snapshot-under-lock: load state / paths are worker-mutable
        // (inserts can come from soft-switch-triggered write-back paths
        // and other panels). Lock released before any rendering.
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        switch (m) {
            case Mode::Disk525:
                return (primaryDiskII() && primaryDiskII()->isDiskLoaded())
                           ? baseName(primaryDiskII()->getDiskPath()) : std::string();
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (primarySmartPortCard()) {
                    const pom2::SmartPortUnit* u = primarySmartPortCard()->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return controller->disk35Internal().isLoaded()
                           ? baseName(controller->disk35Internal().path())
                           : std::string();
            case Mode::SmartportHD:
                if (pom2::ProDOSBlockCard* dev = hdvDevice())
                    return dev->isImageLoaded() ? baseName(dev->getImagePath())
                                                : std::string();
                if (primarySmartPortCard()) {
                    const pom2::SmartPortUnit* u = primarySmartPortCard()->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return std::string();
        }
        return std::string();
    };
    auto mountImage = [&](const std::string& path, Mode m) {
        std::string err;
        int bootSlot = 0;
        // Selecting an image BOOTS it, like a left-click in the Disk Library
        // ("left-click = insert + boot"). Mounting alone left the user
        // staring at whatever was already on screen with a status line
        // telling them to go and reboot the machine themselves — the image
        // was in the drive and nothing had happened, which reads as "the
        // click did nothing". Routing stays MODE-driven, not
        // extension-driven: the Floppy Emu emulates the device its mode
        // says, so a .2mg selected in Smartport mode must boot from the
        // SmartPort slot even though insertAndBootImage would classify the
        // same file by its extension. -1 = mount succeeded but no explicit
        // boot slot (cold-boot instead); -2 = nothing mounted.
        constexpr int kNoMount   = -2;
        constexpr int kColdBoot  = -1;
        int bootTarget = kNoMount;
        switch (m) {
            case Mode::Disk525: {
                if (!primaryDiskII()) { floppyEmuStatus = controllerHint(m); break; }
                // Two-phase (MediaMount): the decode runs unlocked, and the
                // lock is taken only to swap the track buffers the worker's
                // LSS streams from. seekTrack0 parks the head before the boot
                // PROM reads — the same step insertAndBootImage does.
                std::string mountErr;
                const bool ok = pom2::mountDiskII(*controller, *primaryDiskII(), 0,
                                                  path, mountErr,
                                                  /*seekTrack0=*/true);
                if (ok) bootTarget = primaryDiskII()->getSlot();
                floppyEmuStatus = ok
                    ? ("Booting " + baseName(path))
                    : ("5.25 mount failed: " + baseName(path));
                break;
            }
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (!controllerReady(m)) ensureSmartPortCardForBoot();
                if (routeMount35(0, path, err)) {
                    // With a SmartPort card, boot its slot explicitly;
                    // without one the mount landed on the //c+ on-board hub,
                    // which has no slot to jump to — cold boot instead.
                    bootTarget = primarySmartPortCard() ? primarySmartPortCard()->getSlot()
                                               : kColdBoot;
                    floppyEmuStatus = "Booting " + baseName(path);
                } else {
                    floppyEmuStatus = "3.5\" mount failed: " + err;
                }
                break;
            case Mode::SmartportHD:
                if (!controllerReady(m)) ensureSmartPortCardForBoot();
                if (routeMountHdv(path, bootSlot, err)) {
                    // bootSlot is what routeMountHdv resolved. It was being
                    // filled and then dropped on the floor here.
                    bootTarget = bootSlot;
                    floppyEmuStatus = "Booting " + baseName(path);
                } else {
                    floppyEmuStatus = "Smartport mount failed: " + err;
                }
                break;
        }
        if (bootTarget != kNoMount) {
            if (bootTarget == kColdBoot) controller->coldBoot();
            else                        controller->bootFromSlot(bootTarget);
            controller->setMode(EmulationController::Mode::Running);
        }
    };
    auto ejectCurrent = [&](Mode m) {
        bool ok = false;
        std::string err;
        switch (m) {
            case Mode::Disk525: {
                // Through the coordinator, like the 3.5" cases below: the
                // direct `ejectDisk()` this replaces cleared no settings key,
                // so the ejected image came back on the next launch. Note the
                // default argument was drive 1 only, which is the drive this
                // button means.
                if (auto* d2 = primaryDiskII()) {
                    const auto e = storageCoordinator_->ejectDiskII(
                        *controller, *settings, d2->getSlot(), 0);
                    ok = e.ok;
                    if (!ok) err = e.error;
                }
                break;
            }
            case Mode::Disk35:
            case Mode::Unidisk35: {
                // Through the coordinator, exactly like the 5.25" case above
                // — and for the same reason, which the comment there wrongly
                // claimed was already true here: the inline `u->eject()` /
                // `eject35()` cleared NO settings key, and the SmartPort unit
                // keys are only ever written by a mount or an eject, so the
                // disk this button removed was back on the next launch. It
                // also ran the save-on-eject write under the lock.
                const auto e = storageCoordinator_->ejectDisk35(
                    *controller, *settings, 0);
                ok = e.ok;
                if (!ok) err = e.error;
                break;
            }
            case Mode::SmartportHD: {
                // Same routing rule as the mount side: a dedicated block card
                // first, the SmartPort unit otherwise. Both are bays, so one
                // coordinator command covers them.
                int slot = -1;
                if (pom2::ProDOSBlockCard* dev = hdvDevice())
                    slot = dev->getSlot();
                else if (primarySmartPortCard())
                    slot = primarySmartPortCard()->getSlot();
                if (slot < 0) { err = "no HDV or SmartPort card plugged"; break; }
                const auto e = storageCoordinator_->ejectMediaBay(
                    *controller, *settings, slot, 0);
                ok = e.ok;
                if (!ok) err = e.error;
                break;
            }
        }
        floppyEmuStatus = ok ? "Ejected" : "Eject failed: " + err;
    };

    // ── Build the snapshot. ──────────────────────────────────────────────
    pom2::FloppyEmu_ImGui::Snapshot snap;
    snap.modeLabel = pom2::FloppyEmuDevice::modeLabel(mode);
    snap.sdPresent = floppyEmu->sdPresent();
    snap.sdRootDisplay = floppyEmu->sdRoot();
    {
        const std::string cur = floppyEmu->currentDir();
        const std::string root = floppyEmu->sdRoot();
        snap.dirLabel = (cur.size() >= root.size() &&
                         cur.compare(0, root.size(), root) == 0)
                            ? cur.substr(root.size()) : cur;
    }
    const auto fav = floppyEmu->favorites();
    snap.favoritesAvailable = fav.present;
    snap.favoritesActive    = floppyEmuFavActive_ && fav.present;
    if (snap.favoritesActive) {
        for (const auto& e : fav.entries) {
            pom2::FloppyEmu_ImGui::Item it;
            it.label = e.name; it.sublabel = human(e.sizeBytes);
            snap.items.push_back(std::move(it));
        }
    } else {
        for (const auto& e : floppyEmu->listing()) {
            pom2::FloppyEmu_ImGui::Item it;
            it.label = e.name; it.isDir = e.isDir; it.isUp = e.isUp;
            it.sublabel = e.isDir ? "DIR" : human(e.sizeBytes);
            snap.items.push_back(std::move(it));
        }
    }
    snap.controllerReady = controllerReady(mode);
    snap.controllerHint  = controllerHint(mode);
    snap.insertedLabel   = insertedLabel(mode);
    snap.statusLine      = floppyEmuStatus;
    for (Mode m : pom2::FloppyEmuDevice::allModes()) {
        snap.modeOptions.push_back(pom2::FloppyEmuDevice::modeLabel(m));
        if (m == mode) snap.currentModeIndex =
            static_cast<int>(snap.modeOptions.size()) - 1;
    }

    const auto r = floppyEmuPanel->render("Floppy Emu (BMOW)", show(pom2::PanelId::FloppyEmu), snap);

    // ── Apply actions. ───────────────────────────────────────────────────
    if (r.setModeIndex >= 0) {
        const auto modes = pom2::FloppyEmuDevice::allModes();
        if (r.setModeIndex < static_cast<int>(modes.size())) {
            floppyEmu->setMode(modes[r.setModeIndex]);
            floppyEmuFavActive_ = false;
            floppyEmuStatus = std::string("Mode: ") +
                pom2::FloppyEmuDevice::modeLabel(modes[r.setModeIndex]);
        }
    }
    if (r.toggleFavorites) floppyEmuFavActive_ = !floppyEmuFavActive_;
    if (r.requestConfigureController) {
        if (mode == Mode::Disk525)
            floppyEmuStatus = "Add a Disk II card via the Slot Manager (Apply restarts).";
        else {
            const int s = ensureSmartPortCardForBoot();
            floppyEmuStatus = (s >= 0)
                ? ("Added SmartPort card in slot " + std::to_string(s))
                : "No free slot for a SmartPort card.";
        }
    }
    if (r.requestEject) ejectCurrent(mode);
    if (r.activateIndex >= 0) {
        if (snap.favoritesActive) {
            if (r.activateIndex < static_cast<int>(fav.entries.size()))
                mountImage(fav.entries[r.activateIndex].fullPath, mode);
        } else {
            const auto items = floppyEmu->listing();
            if (r.activateIndex < static_cast<int>(items.size())) {
                const auto& e = items[r.activateIndex];
                if (e.isDir || e.isUp) floppyEmu->enterDir(e);
                else                   mountImage(e.fullPath, mode);
            }
        }
    }
}

const MainWindow::MediaDirListing& MainWindow::mediaDirListing(
    const std::string& cacheKey,
    std::initializer_list<const char*> candidates,
    std::initializer_list<const char*> extensions,
    bool recursive)
{
    namespace fs = std::filesystem;
    MediaDirListing& cache = mediaDirCaches_[cacheKey];
    if (lastFrameTime - cache.stamp < kMediaDirRescanSeconds) return cache;
    cache.stamp = lastFrameTime;
    cache.dir.clear();
    cache.entries.clear();

    // error_code on EVERY call, including the per-entry `is_regular_file` /
    // `is_directory` probes: those are the ones that throw, because they stat
    // a path the walk found a moment ago and that can be gone by now.
    std::error_code ec;
    for (const char* dir : candidates) {
        if (!fs::is_directory(dir, ec)) { ec.clear(); continue; }
        cache.dir = dir;
        break;
    }
    if (cache.dir.empty()) return cache;

    const auto matches = [&](const fs::path& file) {
        if (extensions.size() == 0) return true;
        const std::string ext = file.extension().string();
        for (const char* candidate : extensions)
            if (ext == candidate) return true;
        return false;
    };

    const fs::path root(cache.dir);
    if (recursive) {
        // Users shelve images by collection / OS / target machine, so the
        // deep walk is what makes one-click mount usable; dot-directories are
        // skipped whole.
        ec.clear();
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto& entry = *it;
            const std::string name = entry.path().filename().string();
            if (!name.empty() && name.front() == '.') {
                if (entry.is_directory(ec)) it.disable_recursion_pending();
                ec.clear();
                continue;
            }
            const bool regular = entry.is_regular_file(ec);
            ec.clear();
            if (!regular || !matches(entry.path())) continue;
            MediaDirEntry row;
            row.name = fs::relative(entry.path(), root, ec).string();
            ec.clear();
            if (row.name.empty()) row.name = name;
            row.path = entry.path().string();
            cache.entries.push_back(std::move(row));
        }
    } else {
        ec.clear();
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            const bool regular = entry.is_regular_file(ec);
            ec.clear();
            if (!regular || !matches(entry.path())) continue;
            MediaDirEntry row;
            row.name = entry.path().filename().string();
            row.path = entry.path().string();
            cache.entries.push_back(std::move(row));
        }
        ec.clear();
    }
    // Sorted so the list is stable across frames whatever order dirent
    // returns.
    std::sort(cache.entries.begin(), cache.entries.end(),
              [](const MediaDirEntry& a, const MediaDirEntry& b) {
                  return a.name < b.name;
              });
    return cache;
}

void MainWindow::renderHdvPanelWindow()
{
    if (!show(pom2::PanelId::Hdv)) return;

    pom2::HdvController_ImGui::DriveSnapshot snap;
    if (primaryHdvCard()) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        snap.imageLoaded       = primaryHdvCard()->isImageLoaded();
        snap.imagePath         = primaryHdvCard()->getImagePath();
        snap.blockCount        = primaryHdvCard()->getBlockCount();
        snap.writeBackEnabled  = primaryHdvCard()->isWriteBackEnabled();
        snap.hasUnsavedChanges = primaryHdvCard()->hasUnsavedChanges();
        snap.supportsWriteBack = primaryHdvCard()->canWriteBack();
        snap.isSynthVolume     = primaryHdvCard()->isSynthVolumeMounted();
    }

    // Library scan — hdv/ for .hdv and .2mg, sorted alphabetically so the
    // list stays stable across frames regardless of dirent order. Plus a
    // synthetic entry for prodos_folder/ if that folder exists (host-folder
    // mount: contents are synthesised into a read-only ProDOS volume on
    // click, see kProDOSHostSentinel below).
    {
        // Cached and non-throwing — this ran a RECURSIVE walk of hdv/ on
        // every frame the panel was open, with the throwing overloads. See
        // MainWindow::mediaDirListing.
        const auto& library = mediaDirListing(
            "hdv:panel", { "hdv", "../hdv", "../../hdv" },
            { ".hdv", ".2mg" }, /*recursive=*/true);
        for (const auto& row : library.entries) {
            pom2::HdvController_ImGui::LibraryEntry e;
            e.displayName = row.name;
            e.fullPath    = row.path;
            snap.library.push_back(std::move(e));
        }

        // Synthetic entry for the host-folder mount. Its file count is the
        // same cached listing (every regular file, no extension filter).
        const auto& hostFolder = mediaDirListing(
            "prodos_folder",
            { "prodos_folder", "../prodos_folder", "../../prodos_folder" },
            {}, /*recursive=*/false);
        if (!hostFolder.dir.empty()) {
            pom2::HdvController_ImGui::LibraryEntry e;
            e.displayName = "[host folder] " + hostFolder.dir + "/  ("
                          + std::to_string(hostFolder.entries.size())
                          + " files)";
            e.fullPath    = std::string(kProDOSHostSentinel) + hostFolder.dir;
            snap.library.push_back(std::move(e));
        }
    }

    // HDV = bottom-left panel in the curated layout (under the Screen).
    ImGui::SetNextWindowPos (ImVec2(5,    600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1040, 390), ImGuiCond_FirstUseEver);
    // Title reflects the actual slot the HDV card is plugged in.
    char hdvTitle[48];
    std::snprintf(hdvTitle, sizeof(hdvTitle), "HDV (slot %d)",
                  primaryHdvCard() ? primaryHdvCard()->getSlot() : 5);
    auto result = hdvPanel->render(hdvTitle, show(pom2::PanelId::Hdv), snap);

    if (result.writeBackToggleChanged && primaryHdvCard()) {
        // Persists the hdv_writeback key with the change; the bare setter did
        // not, so the toggle was forgotten at the next launch.
        (void)storageCoordinator_->setMediaBayWriteBack(
            *controller, *settings, primaryHdvCard()->getSlot(), 0,
            result.writeBackNewValue);
        tapeStatusMessage = result.writeBackNewValue
            ? "HDV: write-back ENABLED (saves on eject)"
            : "HDV: write-back disabled";
        tapeStatusUntil   = lastFrameTime + 4.0;
    }
    if (result.requestEject && primaryHdvCard()) {
        const auto r = storageCoordinator_->ejectMediaBay(
            *controller, *settings, primaryHdvCard()->getSlot(), 0);
        if (r.ok) hdvStatus = "no image mounted";
        tapeStatusMessage = r.ok ? "HDV ejected"
                                 : "HDV eject failed: " + r.error;
        tapeStatusUntil   = lastFrameTime + 4.0;
    }
    if (result.requestBoot && primaryHdvCard()) {
        bootHdvImage();
    }
    if (!result.requestMountAndBoot.empty() && primaryHdvCard()) {
        const std::string path = result.requestMountAndBoot;
        const std::string sentinel(kProDOSHostSentinel);

        if (path.rfind(sentinel, 0) == 0) {
            // Host-folder mount: synthesise a read-only ProDOS volume from
            // the folder contents and load it into the slot 5 card. We do
            // NOT auto-boot — block 0 is zero, so the volume isn't
            // bootable. The user boots ProDOS from elsewhere (Disk II or
            // an HDV) and ProDOS then sees /HOST/ as a second drive.
            const std::string hostDir = path.substr(sentinel.size());
            std::vector<std::uint8_t> bytes;
            auto br = pom2::buildVolumeFromFolder(hostDir, "HOST", bytes);
            if (!br.ok) {
                tapeStatusMessage = "ProDOS synth failed: " + br.error;
                tapeStatusUntil   = lastFrameTime + 5.0;
                return;
            }
            // Through the coordinator: `loadImageFromBytes` SAVES whatever
            // is mounted before replacing it, and this held `stateMutex`
            // across that write — up to 30 ms for a 32 MiB HDV, against a
            // 20 ms PAL frame. The coordinator flushes the outgoing image
            // two-phase and refuses the mount if it could not be saved.
            const auto mounted = storageCoordinator_->mountBlockBytes(
                *controller, *settings, primaryHdvCard()->getSlot(),
                std::move(bytes), std::string("[host folder] ") + hostDir,
                hostDir);
            const bool ok = mounted.ok;
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("synth from ") + hostDir +
                            " (" + std::to_string(br.filesIncluded) + " files)";
            } else {
                hdvStatus = mounted.error.empty() ? "synth load failed"
                                                  : mounted.error;
            }
            if (ok) {
                char msg[200];
                std::snprintf(msg, sizeof(msg),
                    "/HOST/ mounted from %s (%zu files, %zu skipped, %zu blocks). Boot ProDOS from another drive.",
                    hostDir.c_str(), br.filesIncluded, br.filesSkipped, br.totalBlocks);
                tapeStatusMessage = msg;
                pom2::log().info("HDV",
                    std::string("Synthesised volume from ") + hostDir +
                    " (" + std::to_string(br.filesIncluded) + " files, " +
                    std::to_string(br.totalBlocks) + " blocks)");
            } else {
                tapeStatusMessage = "Synth load failed";
            }
            tapeStatusUntil = lastFrameTime + 8.0;
            return;
        }

        // Real .hdv / .2mg / .po file: load under the lock so the card
        // has the right blocks before bootFromSlot wipes RAM and jumps
        // PC = $C(N)00 (where N is the slot the card actually lives in).
        // Two-step lock is safe — the CPU worker only resumes when
        // bootFromSlot flips mode to Running.
        bool ok = false;
        std::string err;
        {
            // Through the coordinator: same two-phase read (32 MiB off the
            // lock), plus the hdv_path key written with the mount. The bare
            // helper left the key stale, so the panel and settings disagreed
            // until the next shutdown.
            {
                const auto r = storageCoordinator_->mountMediaBay(
                    *controller, *settings, primaryHdvCard()->getSlot(), 0,
                    path);
                ok  = r.ok;
                err = r.error;
            }
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("loaded: ") + path;
            } else {
                hdvStatus = "no image mounted";
            }
        }
        if (ok) {
            controller->bootFromSlot(primaryHdvCard()->getSlot());
            pom2::log().info("HDV",
                "slot " + std::to_string(primaryHdvCard()->getSlot()) +
                " library click → mount + boot: " + path);
            tapeStatusMessage = "Mounting + booting HDV (slot " +
                std::to_string(primaryHdvCard()->getSlot()) + "): " + path;
        } else {
            tapeStatusMessage = "Boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!result.requestMountOnly.empty() && primaryHdvCard()) {
        // Right-click "mount only": swap the image without booting.
        // Host-folder sentinel is handled the same as mount-and-boot
        // above (it never auto-boots anyway), so funnel both branches
        // here when no Apple II reset is wanted.
        const std::string path = result.requestMountOnly;
        const std::string sentinel(kProDOSHostSentinel);

        bool ok = false;
        std::string err;
        if (path.rfind(sentinel, 0) == 0) {
            const std::string hostDir = path.substr(sentinel.size());
            std::vector<std::uint8_t> bytes;
            auto br = pom2::buildVolumeFromFolder(hostDir, "HOST", bytes);
            if (!br.ok) {
                tapeStatusMessage = "ProDOS synth failed: " + br.error;
                tapeStatusUntil   = lastFrameTime + 5.0;
                return;
            }
            // Same as the mount-and-boot branch above: the coordinator
            // flushes the outgoing image off the lock and refuses the mount
            // when that write fails.
            const auto mounted = storageCoordinator_->mountBlockBytes(
                *controller, *settings, primaryHdvCard()->getSlot(),
                std::move(bytes), std::string("[host folder] ") + hostDir,
                hostDir);
            ok = mounted.ok;
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("synth from ") + hostDir;
            } else {
                err = mounted.error.empty() ? "synth load failed"
                                            : mounted.error;
                hdvStatus = err;
            }
        } else {
            // Through the coordinator: same two-phase read (32 MiB off the
            // lock), plus the hdv_path key written with the mount. The bare
            // helper left the key stale, so the panel and settings disagreed
            // until the next shutdown.
            {
                const auto r = storageCoordinator_->mountMediaBay(
                    *controller, *settings, primaryHdvCard()->getSlot(), 0,
                    path);
                ok  = r.ok;
                err = r.error;
            }
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("loaded: ") + path;
            } else {
                hdvStatus = "no image mounted";
            }
        }
        if (ok) {
            pom2::log().info("HDV",
                std::string("Library right-click → mount only: ") + path);
            tapeStatusMessage = "Mounted (no boot): " + path;
        } else {
            tapeStatusMessage = "Mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}

void MainWindow::renderDiskFileDialog()
{
    // Find the panel that currently has its insertDialogOpen flag set.
    // With option C (multi-instance DiskII), any of the per-card panels
    // could have triggered the popup via its "Insert .dsk..." button —
    // we route the eventual insertDisk() to the corresponding card.
    pom2::DiskController_ImGui* triggeredPanel = nullptr;
    DiskIICard*                 triggeredCard  = nullptr;
    const auto diskCardList = diskIICards();
    for (size_t i = 0; i < diskPanels.size() && i < diskCardList.size(); ++i) {
        if (diskPanels[i] && diskPanels[i]->insertDialogOpen) {
            triggeredPanel = diskPanels[i].get();
            triggeredCard  = diskCardList[i];
            break;
        }
    }
    // Top-level "Insert disk image..." menu (no per-panel context) routes
    // to the primary card by convention.
    if (!triggeredPanel && diskPanel && diskPanel->insertDialogOpen) {
        triggeredPanel = diskPanel;
        triggeredCard  = primaryDiskII();
    }

    if (triggeredPanel) {
        ImGui::OpenPopup("Insert disk image");
        triggeredPanel->insertDialogOpen = false;
        // Remember which card the popup routes to until the user clicks
        // Insert / Cancel. ImGui modal state survives between frames so
        // the pointer needs to survive too.
        diskDialogTargetSlot = triggeredCard ? triggeredCard->getSlot() : -1;
    }
    if (!ImGui::BeginPopupModal("Insert disk image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    // Resolve the target card via the saved slot — the panel pointer may
    // have moved (rare profile-switch races), but the slot number is
    // stable until plugSlotsFromSettings rebuilds.
    DiskIICard*                 popupCard  = nullptr;
    pom2::DiskController_ImGui* popupPanel = nullptr;
    const auto popupCardList = diskIICards();
    for (size_t i = 0; i < popupCardList.size(); ++i) {
        if (popupCardList[i] && popupCardList[i]->getSlot() == diskDialogTargetSlot) {
            popupCard  = popupCardList[i];
            popupPanel = (i < diskPanels.size()) ? diskPanels[i].get() : nullptr;
            break;
        }
    }
    if (!popupPanel) popupPanel = diskPanel;
    if (!popupCard)  popupCard  = primaryDiskII();

    if (popupCard) {
        ImGui::Text("Target: Disk II slot %d", popupCard->getSlot());
    }
    ImGui::TextUnformatted("Path to a 5.25\" image —"
                           " .dsk / .do (DOS 3.3, 143 360 B) or"
                           " .po (ProDOS, 143 360 B) or .nib (raw"
                           " nibble stream, 232 960 B) or .woz"
                           " (bit-cell, copy-protected disks; read-only)."
                           " Write-back is opt-in via the panel checkbox.");
    if (popupPanel) {
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", popupPanel->dialogPath.c_str());
        if (ImGui::InputText("##DiskPath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            popupPanel->dialogPath = buf;
        else
            popupPanel->dialogPath = buf;
    }

    // Quick list of disk images in disks_5.4/ (mirrors the cassette dialog).
    // Cached: a modal is re-rendered every frame, and this walked the folder
    // each time, with the throwing filesystem overloads.
    {
        const auto& listing = mediaDirListing(
            "disks_5.4",
            { "disks_5.4", "../disks_5.4", "../../disks_5.4" },
            { ".dsk", ".do", ".po", ".nib", ".woz" }, /*recursive=*/false);
        if (!listing.dir.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("%s/", listing.dir.c_str());
            for (const auto& row : listing.entries) {
                if (ImGui::Selectable(row.name.c_str()) && popupPanel)
                    popupPanel->dialogPath = row.path;
            }
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Insert", ImVec2(120, 0))) {
        if (popupCard && popupPanel && !popupPanel->dialogPath.empty()) {
            std::string err;
            if (pom2::mountDiskII(*controller, *popupCard, 0,
                                  popupPanel->dialogPath, err)) {
                tapeStatusMessage = "Disk inserted (slot " +
                    std::to_string(popupCard->getSlot()) + "): " +
                    popupPanel->dialogPath;
            } else {
                tapeStatusMessage = "Insert failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 5.0;
        }
        diskDialogTargetSlot = -1;   // popup closed — clear the latched slot
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        diskDialogTargetSlot = -1;   // popup closed — clear the latched slot
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::renderHdvFileDialog()
{
    if (hdvPanel->mountDialogOpen) {
        ImGui::OpenPopup("Mount HDV image");
        hdvPanel->mountDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal("Mount HDV image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("ProDOS block-device image — .hdv (raw blocks)"
                           " or .2mg (with 2IMG header, ProDOS order)");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", hdvPanel->dialogPath.c_str());
    if (ImGui::InputText("##HdvPath", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        hdvPanel->dialogPath = buf;
    else
        hdvPanel->dialogPath = buf;

    {
        const auto& listing = mediaDirListing(
            "hdv:dialog", { "hdv", "../hdv", "../../hdv" },
            { ".hdv", ".2mg" }, /*recursive=*/false);
        if (!listing.dir.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("%s/", listing.dir.c_str());
            for (const auto& row : listing.entries) {
                if (ImGui::Selectable(row.name.c_str()))
                    hdvPanel->dialogPath = row.path;
            }
        }
    }

    ImGui::Separator();
    const bool canMount = primaryHdvCard() && !hdvPanel->dialogPath.empty();
    ImGui::BeginDisabled(!canMount);
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        std::string mountErr;
        const auto r = storageCoordinator_->mountMediaBay(
            *controller, *settings, primaryHdvCard()->getSlot(), 0,
            hdvPanel->dialogPath);
        mountErr = r.error;
        if (r.ok) {
            hdvPath   = hdvPanel->dialogPath;
            hdvStatus = std::string("loaded: ") + hdvPanel->dialogPath;
            tapeStatusMessage = "HDV mounted: " + hdvPanel->dialogPath;
        } else {
            hdvStatus = "no image mounted";
            tapeStatusMessage = "HDV mount failed: " + mountErr;
        }
        tapeStatusUntil = lastFrameTime + 5.0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Mount and Boot", ImVec2(160, 0))) {
        bool ok = false;
        {
            std::string mountErr;
            const auto r = storageCoordinator_->mountMediaBay(
                *controller, *settings, primaryHdvCard()->getSlot(), 0,
                hdvPanel->dialogPath);
            ok = r.ok;
            mountErr = r.error;
            if (ok) {
                hdvPath   = hdvPanel->dialogPath;
                hdvStatus = std::string("loaded: ") + hdvPanel->dialogPath;
            } else {
                hdvStatus = "no image mounted";
                tapeStatusMessage = "HDV mount failed: " + mountErr;
                tapeStatusUntil   = lastFrameTime + 5.0;
            }
        }
        if (ok) bootHdvImage();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

bool MainWindow::convertWoz35ToPo(int drive, bool /*useSmartPort35*/)
{
    // The way out of a read-only 3.5" WOZ, and the reason it exists: POM2
    // decodes Sony GCR but cannot encode it, so a `.woz` mounted at 800K can
    // never take a guest write — which breaks any program that keeps its
    // configuration on its own program disk (The New Print Shop's printer
    // setup is the canonical case). The decode already produced the exact
    // 1600 blocks a `.po` holds, so the conversion is a file write and
    // nothing more. The WOZ is never touched: it stays the archival master.
    //
    // The coordinator picks the source image by the same routing rule as
    // mount and eject, writes the `.po`, re-mounts it in place of the WOZ and
    // persists the new path — so the drive the panel is showing is the drive
    // that gets converted. The routing argument is ignored and kept only so
    // the call sites need not change.
    const auto r = storageCoordinator_->convertDisk35WozToPo(*controller,
                                                             *settings, drive);
    if (!r.ok) {
        tapeStatusMessage = "3.5\" convert failed: " + r.error;
        tapeStatusUntil   = lastFrameTime + 6.0;
        return false;
    }
    // The memoised convert-target name is stale the moment the medium
    // changes: the drive now holds the .po, not the WOZ it was computed from.
    convertSrc_[drive].clear();
    convertDst_[drive].clear();
    tapeStatusMessage = "3.5\" drive " + std::string(drive == 0 ? "1" : "2") +
                        " converted to " + r.outputPath + " (now writable)";
    tapeStatusUntil   = lastFrameTime + 6.0;
    return true;
}

void MainWindow::renderDisk35PanelWindow()
{
    if (!show(pom2::PanelId::Disk35)) return;

    pom2::Disk35Controller_ImGui::PanelSnapshot snap;
    // 3.5" is "supported" by the //c+ profile (on-board SmartPort + MIG) OR by
    // ANY profile where the user plugged a SmartPort 3.5" card (//e +
    // Liron-class). Both paths share the same Disk35Image objects, so the
    // panel does not have to care which mux is talking.
    snap.supportedByProfile =
        (activeProfile == pom2::SystemProfile::AppleIIcPlus) ||
        (primarySmartPortCard() != nullptr);

    // One acquisition, and — this is the behaviour change — the SAME source
    // rule the mount path uses: a plugged SmartPort card owns the 3.5" media,
    // whatever the profile. The panel used to exclude //c+ from that branch
    // and read the on-board hub instead, while routeMount35 sent the media to
    // the card's units regardless. So on //c+ the panel showed two empty
    // on-board drives over media that was really in the card, and eject and
    // write-back hit the wrong object.
    const auto d35 = storageCoordinator_->captureDisk35(*controller);
    for (int i = 0; i < 2; ++i) {
        const auto& src = d35.drives[i];
        auto& dst = snap.drives[i];
        dst.diskLoaded        = src.loaded;
        dst.motorOn           = src.motorOn;
        dst.track             = src.track;
        dst.side1             = src.side1;
        dst.writeProtected    = src.writeProtected;
        dst.diskPath          = src.path;
        dst.lastError         = src.lastError;
        dst.hasUnsavedChanges = src.hasUnsavedChanges;
        dst.writeBackEnabled  = src.writeBackEnabled;
        dst.isWoz             = src.isWoz;
    }

    // Convert-target names stay memoised HERE rather than taken from the
    // snapshot. The coordinator computes them outside its lock (so they never
    // block the CPU worker), but it recomputes on every capture, and
    // `freePoNameFor` stats the filesystem up to 99 times when earlier
    // candidates are taken — this panel re-snapshots every frame. The answer
    // only changes when the medium changes, so the path is the whole key.
    for (int i = 0; i < 2; ++i) {
        auto& s = snap.drives[i];
        if (!s.isWoz) { convertSrc_[i].clear(); convertDst_[i].clear(); continue; }
        if (convertSrc_[i] != s.diskPath) {
            convertSrc_[i] = s.diskPath;
            convertDst_[i] = freePoNameFor(s.diskPath);
        }
        s.convertTargetPath = convertDst_[i];
    }

    // Library scan — mirrors the Disk II library scan but only picks
    // up files large enough to be 800K (size sniff via filesystem).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const char* dir : { "disks_3.5", "../disks_3.5", "../../disks_3.5",
                                 "disks_5.4",   "../disks_5.4",   "../../disks_5.4" }) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".po" && ext != ".2mg" && ext != ".woz") continue;
                const auto sz = entry.file_size(ec);
                if (ec) continue;
                // 800K raw or 2IMG-wrapped (header + 819 200). A `.woz` is
                // FLUX, so its size says nothing about the payload — the
                // 3.5" loader decodes it and refuses a 5.25" one by name.
                if (ext != ".woz" &&
                    sz != 819200 && sz != 819200 + 64 &&
                    !(sz > 819200 && sz < 819200 + 4096)) continue;
                pom2::Disk35Controller_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath = entry.path().string();
                snap.library.push_back(std::move(e));
            }
            if (!snap.library.empty()) break;
        }
        std::sort(snap.library.begin(), snap.library.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    ImGui::SetNextWindowPos (ImVec2(1055, 30),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(705,  600), ImGuiCond_FirstUseEver);
    // Title reflects where the SmartPort path lives: on-board on //c+,
    // or the explicit slot of the plugged Liron-class card on other
    // profiles. Stable ImGui window-id per slot so the user's position/
    // size choices are remembered per-configuration.
    char disk35Title[64];
    if (primarySmartPortCard()) {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (slot %d)", primarySmartPortCard()->getSlot());
    } else {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (//c+ on-board)");
    }
    auto result = disk35Panel->render(
        disk35Title, show(pom2::PanelId::Disk35), snap);

    if (result.requestConvertDrive >= 0)
        convertWoz35ToPo(result.requestConvertDrive, d35.usesSmartPort());

    for (int d = 0; d < 2; ++d) {
        if (result.requestEject[d]) {
            // Routed like the mount: the coordinator decides whether the
            // medium lives in a SmartPort unit or the on-board pair, ejects
            // there and clears the matching settings key. The two branches
            // here duplicated that decision and only the SmartPort one
            // persisted, so an on-board eject came back on the next launch.
            const auto e = storageCoordinator_->ejectDisk35(*controller,
                                                            *settings, d);
            tapeStatusMessage = e.ok
                ? (std::string("3.5\" drive ") +
                   (d == 0 ? "1 (internal)" : "2 (external)") + " ejected")
                : ("3.5\" eject failed: " + e.error);
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        // Per-drive write-back toggle. The coordinator applies it under the
        // machine lock — a save-on-eject race against the worker must not
        // half-flip the flag — and persists after unlocking. The on-board
        // branch here never wrote a settings key at all, so that toggle was
        // forgotten every launch.
        if (result.requestWriteBackToggle[d]) {
            (void)storageCoordinator_->setDisk35WriteBack(
                *controller, *settings, d, result.newWriteBack[d]);
            tapeStatusMessage = std::string("3.5\" drive ")
                + (d == 0 ? "1" : "2")
                + (result.newWriteBack[d]
                    ? ": write-back ENABLED (saves on eject)"
                    : ": write-back disabled");
            tapeStatusUntil = lastFrameTime + 4.0;
        }
    }
    if (result.openMountDialog) {
        disk35Panel->mountDialogOpen     = true;
        disk35Panel->mountDialogForDrive = result.openMountDialogForDrive;
        if (disk35Panel->dialogPath.empty()) disk35Panel->dialogPath = "disks_3.5/";
    }
    if (!result.requestMountPath.empty()) {
        // routeMount35 sends the image to the SmartPort card's unit on
        // non-//c+ profiles, or to the on-board hub on //c+ — the same
        // routing the Disk Library + CLI use. Keeps the standalone panel
        // and the library in lock-step.
        std::string err;
        if (routeMount35(result.requestMountDrive, result.requestMountPath, err)) {
            tapeStatusMessage = "3.5\" mounted: " + result.requestMountPath;
        } else {
            tapeStatusMessage = "3.5\" mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    // Library left-click default = mount + cold boot. The //c+ ROM's
    // power-on probe scans SmartPort devices in order and boots the
    // first ready volume, so `coldBoot()` is enough — no need to
    // pre-set PC. On non-//c+ profiles `mount35` succeeds (the image
    // sits idle in Sony35Drive) but no device walker exists to read
    // it, so we still cold-boot but the user sees the Applesoft
    // prompt instead of the new image's loader.
    if (!result.requestInsertAndBoot.empty()) {
        const int d = result.insertAndBootDrive;
        std::string err;
        if (routeMount35(d, result.requestInsertAndBoot, err)) {
            // Prefer an explicit `bootFromSlot(N)` when the SmartPort
            // path is provided by a slot card on a non-//c+ profile —
            // the user picked the slot in Slot Configuration and the
            // PR#N landing should follow that. On //c+ on-board, fall
            // back to `coldBoot()` so the ROM autostart picks up the
            // built-in SmartPort firmware.
            if (d35.usesSmartPort()) {
                controller->bootFromSlot(d35.smartPortSlot);
                tapeStatusMessage = "3.5\" drive "
                    + std::string(d == 0 ? "1" : "2")
                    + " booted (slot " + std::to_string(d35.smartPortSlot)
                    + "): " + result.requestInsertAndBoot;
            } else {
                controller->coldBoot();
                tapeStatusMessage = "3.5\" drive "
                    + std::string(d == 0 ? "1" : "2")
                    + " booted: " + result.requestInsertAndBoot;
            }
        } else {
            tapeStatusMessage = "3.5\" boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}

void MainWindow::renderDisk35FileDialog()
{
    if (disk35Panel->mountDialogOpen) {
        ImGui::OpenPopup("Mount 3.5\" image");
        disk35Panel->mountDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal("Mount 3.5\" image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Target drive: %s",
                disk35Panel->mountDialogForDrive == 0
                    ? "1 (internal, //c+ on-board)"
                    : "2 (external, SmartPort daisy-chain)");
    ImGui::TextUnformatted("800K Sony 3.5\" image — .po (raw ProDOS blocks,"
                           " 819 200 B) or .2mg (with 2IMG header).");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", disk35Panel->dialogPath.c_str());
    if (ImGui::InputText("##Disk35Path", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        disk35Panel->dialogPath = buf;
    else
        disk35Panel->dialogPath = buf;

    {
        const auto& listing = mediaDirListing(
            "disks_3.5",
            { "disks_3.5", "../disks_3.5", "../../disks_3.5" },
            { ".po", ".2mg", ".woz" }, /*recursive=*/false);
        if (!listing.dir.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("%s/", listing.dir.c_str());
            for (const auto& row : listing.entries) {
                if (ImGui::Selectable(row.name.c_str()))
                    disk35Panel->dialogPath = row.path;
            }
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        if (!disk35Panel->dialogPath.empty()) {
            // routeMount35 dispatches to the SmartPort card unit (non-//c+)
            // or the on-board hub (//c+), matching the panel's read source.
            std::string err;
            if (routeMount35(disk35Panel->mountDialogForDrive,
                             disk35Panel->dialogPath, err)) {
                tapeStatusMessage = "3.5\" mounted: " + disk35Panel->dialogPath;
            } else {
                tapeStatusMessage = "3.5\" mount failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 5.0;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
