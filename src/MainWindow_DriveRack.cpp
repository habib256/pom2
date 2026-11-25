// POM2 — GPL-3.0-or-later
#include "MainWindow.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "DriveRack_ImGui.h"
#include "DriveRackMount.h"
#include "EmulationController.h"
#include "IconsFontAwesome6.h"
#include "MediaSource.h"
#include "Memory.h"
#include "MountableMediaCard.h"
#include "Settings.h"
#include "StorageCoordinator.h"
#include "SystemProfile.h"
#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <utility>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Every drive of the machine, in the rack's order, as plain values. The Drives
// window and the File menu both list from here, so the two can never show a
// different machine.
std::vector<pom2::DrivePicture> MainWindow::collectDrivePictures() {
    using pom2::DrivePicture;
    using Kind = pom2::DrivePictureKind;
    const bool compact = isCompactDriveProfile();
    std::vector<DrivePicture> drives;
    // Copy values under the machine lock. Card pointers never survive it;
    // artwork decode/upload and drawing take place afterwards.
    {
        auto state = controller->lockState();
        auto& bus = state.memory().slotBus();
        for (int slot = 0; slot < SlotBus::kSlotCount; ++slot) {
            auto* card = bus.peripheral(slot);
            if (auto* disk = dynamic_cast<DiskIICard*>(card)) {
                for (int bay = 0; bay < DiskIICard::kDriveCount; ++bay) {
                    DrivePicture row;
                    row.kind = Kind::DiskII; row.slot = slot; row.bay = bay;
                    row.label = "Slot " + std::to_string(slot) + "," + std::to_string(bay + 1) + " · Disk II";
                    if (compact) {
                        row.label = "Slot " + std::to_string(slot) + "," + std::to_string(bay + 1) + " · Disk //c 5.25\"";
                        row.pictureAspect = 0.43f;
                    }
                    row.loaded = disk->isDiskLoaded(bay);
                    row.writeProtected = disk->isFileWriteProtected(bay);
                    row.busy = disk->isMotorOn() && disk->getActiveDrive() == bay;
                    row.path = disk->getDiskPath(bay);
                    drives.push_back(std::move(row));
                }
            } else if (auto* media = dynamic_cast<pom2::MountableMediaCard*>(card)) {
                for (int bay = 0; bay < media->bayCount(); ++bay) {
                    const auto info = media->bayInfo(bay);
                    // SmartPort's unassigned unit positions have no drive
                    // yet; configured mechanisms still appear when empty.
                    if (info.kindLabel.empty() && info.typeKey.empty() && !info.loaded) continue;
                    DrivePicture row;
                    row.slot = slot; row.bay = bay;
                    row.kind = info.acceptsSony35OrHdv ? Kind::Generic :
                               info.typeKey == "35" || info.kindLabel.find("3.5") != std::string::npos
                        ? Kind::Floppy35 : info.typeKey == "hdv" || info.blockCount > 1600 ||
                          info.kindLabel.find("HD") != std::string::npos ? Kind::HardDisk : Kind::Generic;
                    if (row.kind == Kind::Floppy35) row.pictureAspect = 0.45f;
                    if (row.kind == Kind::HardDisk) row.pictureAspect = 0.40f;
                    const char* name = row.kind == Kind::Floppy35 ? "UniDisk 3.5\"" :
                                       row.kind == Kind::HardDisk ? "ProDOS HD" : "Drive";
                    // Slot,drive the way ProDOS names a device (S6,D1 → "Slot 6,1").
                    row.label = "Slot " + std::to_string(slot) + "," + std::to_string(bay + 1) + " · " + name;
                    row.loaded = info.loaded; row.busy = info.busy; row.path = info.path;
                    row.writeProtected = info.fileWriteProtected;
                    drives.push_back(std::move(row));
                }
            }
        }
    }
    // Physical Sony drives aren't slot-card bays. Include only connected
    // mechanisms, so //c+ external connection changes update the stack live.
    if (activeProfile == pom2::SystemProfile::AppleIIcPlus) {
        const auto sony = storageCoordinator_->captureDisk35(*controller, true);
        std::vector<DrivePicture> onboard;
        for (int bay = 0; bay < 3; ++bay) {
            const auto& info = sony.drives[bay];
            if (!info.connected) continue;
            DrivePicture row;
            row.kind = Kind::Floppy35; row.slot = -1; row.bay = bay;
            row.pictureAspect = 0.45f;
            row.label = bay == 0 ? "Apple 3.5\" · Internal" : "Apple 3.5\" · External " + std::to_string(bay);
            row.loaded = info.loaded; row.busy = info.motorOn; row.path = info.path;
            row.writeProtected = info.fileWriteProtected;
            onboard.push_back(std::move(row));
        }
        drives.insert(drives.begin(), onboard.begin(), onboard.end());
    }
    // List slots 7 down to 1, then each card's drives/units in ascending
    // order. This follows the //e slot scan, but is not a per-drive boot
    // sequence: the //c tries S6D1 then SmartPort S5, not S6D2.
    // The //c+ Sony mechanisms have no slot-card bay; retain their leading
    // position and physical order (internal, then connected externals).
    std::stable_sort(drives.begin(), drives.end(), [](const DrivePicture& a, const DrivePicture& b) {
        if ((a.slot < 0) != (b.slot < 0)) return a.slot < 0;
        if (a.slot != b.slot) return a.slot > b.slot;
        return a.bay < b.bay;
    });
    // The other disks of a loaded disk's archive (a game's sides). Read once
    // per image: this list is rebuilt every frame and the set never changes.
    for (auto& drive : drives) {
        if (!drive.loaded || drive.path.empty()) continue;
        auto it = diskSetCache_.find(drive.path);
        if (it == diskSetCache_.end())
            it = diskSetCache_.emplace(drive.path, pom2::mediaSetOf(drive.path, pom2::mediaCacheDir())).first;
        if (it->second.size() < 2) continue;
        drive.diskSet = it->second;
        for (const auto& disk : drive.diskSet) drive.diskSetNames.push_back(pom2::mediaDisplayName(disk));
    }
    return drives;
}

bool MainWindow::isCompactDriveProfile() const {
    return activeProfile == pom2::SystemProfile::AppleIIc ||
           activeProfile == pom2::SystemProfile::AppleIIcPAL ||
           activeProfile == pom2::SystemProfile::AppleIIcPlus;
}

void MainWindow::renderDriveRackWindow() {
    using Kind = pom2::DrivePictureKind;
    const bool compact = isCompactDriveProfile();
    auto drives = collectDrivePictures();
    for (auto& drive : drives)
        if (drive.kind == Kind::DiskII)
            drive.texture = compact ? retroArtwork_.driveIIc(drive.loaded, drive.busy)
                                    : retroArtwork_.drive(drive.bay, drive.loaded, drive.busy);
        else if (drive.kind == Kind::Floppy35)
            drive.texture = retroArtwork_.drive35(drive.busy);
        else if (drive.kind == Kind::HardDisk)
            drive.texture = retroArtwork_.hardDisk(drive.busy);
    if (focusDefaultDrives_) { ImGui::SetNextWindowFocus(); focusDefaultDrives_ = false; }
    const auto result = pom2::renderDriveRack(show(pom2::PanelId::Drives), drives);
    driveRackHits_ = result.hits;
    driveRackHitFrame_ = ImGui::GetFrameCount();
    if (result.manage)
        show(pom2::PanelId::Media) = true;
    for (const auto& action : result.actions)
        runDriveRackAction(drives.at(action.driveIndex), action.type, action.path);
}

void MainWindow::renderFileMenuDrives() {
    using Kind = pom2::DrivePictureKind;
    const auto drives = collectDrivePictures();
    if (drives.empty()) {
        ImGui::TextDisabled("No drive in this machine");
        return;
    }
    for (const auto& drive : drives) {
        const char* icon = drive.kind == Kind::HardDisk ? ICON_FA_HARD_DRIVE
                                                        : ICON_FA_FLOPPY_DISK;
        const std::string id = "##drive" + std::to_string(drive.slot) + "_" +
                               std::to_string(drive.bay);
        if (!drive.loaded) {
            const std::string label = std::string(icon) + "  " + drive.label +
                                      "  —  Insert…" + id;
            if (ImGui::MenuItem(label.c_str()))
                runDriveRackAction(drive, pom2::DriveRackAction::Type::Insert);
            continue;
        }
        const std::string name = std::filesystem::path(drive.path).filename().string();
        const std::string label = std::string(icon) + "  " + drive.label + "  —  " +
                                  name + id;
        if (ImGui::BeginMenu(label.c_str())) {
            if (ImGui::MenuItem(ICON_FA_EJECT "  Eject"))
                runDriveRackAction(drive, pom2::DriveRackAction::Type::Eject);
            if (ImGui::MenuItem(ICON_FA_LOCK "  Write-protected", nullptr, drive.writeProtected))
                runDriveRackAction(drive, pom2::DriveRackAction::Type::ToggleWrite);
            if (drive.diskSet.size() > 1) {
                ImGui::Separator();
                ImGui::TextDisabled(ICON_FA_SHUFFLE "  Change disk");
                for (std::size_t i = 0; i < drive.diskSet.size(); ++i) {
                    const bool current = drive.diskSet[i] == drive.path;
                    const std::string item = "    " + drive.diskSetNames[i] + "##set" + std::to_string(i);
                    if (ImGui::MenuItem(item.c_str(), nullptr, current, !current))
                        runDriveRackAction(drive, pom2::DriveRackAction::Type::Swap, drive.diskSet[i]);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", drive.path.c_str());
    }
}

// One drive action, wherever it was asked for (the rack's picture, the File
// menu). Insert opens the rack's file picker on that drive.
void MainWindow::runDriveRackAction(const pom2::DrivePicture& drive,
                                    pom2::DriveRackAction::Type type,
                                    const std::string& path) {
    using Kind = pom2::DrivePictureKind;
    if (type == pom2::DriveRackAction::Type::Insert) {
#ifdef __EMSCRIPTEN__
        chooseBrowserDisk(&drive);
#else
        driveRackPickerProfile_ = activeProfile;
        driveRackFilePicker_.open(drive);
#endif
        return;
    }
    if (type == pom2::DriveRackAction::Type::Mount) {
        insertDriveRackImage(drive, path);
        return;
    }
    if (type == pom2::DriveRackAction::Type::Swap) {
        swapDriveDisk(drive, path);
        return;
    }
    pom2::StorageCoordinator::MediaCommandResult command;
    if (type == pom2::DriveRackAction::Type::ToggleWrite)
        command = storageCoordinator_->setMediaNotch(*controller, drive.path, !drive.writeProtected);
    else if (drive.slot < 0)
        command = storageCoordinator_->ejectDisk35(*controller, *settings, drive.bay, true);
    else if (drive.kind == Kind::DiskII)
        command = storageCoordinator_->ejectDiskII(*controller, *settings, drive.slot, drive.bay);
    else
        command = storageCoordinator_->ejectMediaBay(*controller, *settings, drive.slot, drive.bay);
    tapeStatusMessage = command.ok
        ? (type == pom2::DriveRackAction::Type::Eject ? "Disk ejected" : "Disk write protection updated")
        : command.error;
    tapeStatusUntil = lastFrameTime + 4.0;
}

bool MainWindow::insertDriveRackImage(const pom2::DrivePicture& target, const std::string& path,
                                      bool explain) {
    std::string error;
    DiskSlotClass fileKind = DiskSlotClass::Unknown;
    const bool ok = pom2::mountEmptyDrive(*controller, *settings, *storageCoordinator_, target, path,
                                          error, &fileKind);
    tapeStatusMessage = ok ? "Inserted into " + target.label + ": " + path : error;
    tapeStatusUntil = lastFrameTime + 5.0;
    if (!ok && explain) {
        showDiskRefusal(path, error);
        // A disk of the wrong kind for this drive: offer the empty drive of
        // this machine that does take it (Stephane's 5.25-inch image dropped
        // on the 3.5-inch drive of the web demo, 2026-10-20). A button, not a
        // silent redirection: the user picked a drive and is told why not.
        // Only for a file of the wrong KIND: a right-kind file the loader
        // refused (a truncated .dsk, an HDV too large) would fail the same
        // way in the other drive.
        if (fileKind != DiskSlotClass::Unknown && !pom2::driveTakes(target.kind, fileKind)) {
            const auto drives = collectDrivePictures();
            if (const auto* other = pom2::emptyDriveFor(drives, fileKind);
                other && !(other->slot == target.slot && other->bay == target.bay && other->kind == target.kind)) {
                diskRefusalAlternative_ = *other;
                diskRefusalPath_ = path;
            }
        }
    }
    return ok;
}

// Another disk of the same archive into a loaded drive: the outgoing disk is
// flushed first, as for any replacement. A slotted drive replaces in place
// (mountIntoDrive); the //c+'s own 3.5" mechanisms have no slot, so theirs is
// ejected, then the new one inserted.
bool MainWindow::swapDriveDisk(const pom2::DrivePicture& drive, const std::string& path)
{
    std::string error;
    bool ok = false;
    if (drive.slot >= 0) {
        ok = mountIntoDrive(drive.slot, drive.bay, path, error, std::string());
    } else {
        const auto ejected = storageCoordinator_->ejectDisk35(*controller, *settings, drive.bay, true);
        if (!ejected.ok) {
            error = ejected.error;
        } else {
            pom2::DrivePicture empty = drive;
            empty.loaded = false;
            ok = pom2::mountEmptyDrive(*controller, *settings, *storageCoordinator_, empty, path, error);
        }
    }
    tapeStatusMessage = ok ? drive.label + ": " + pom2::mediaDisplayName(path) : error;
    tapeStatusUntil = lastFrameTime + 5.0;
    if (!ok) showDiskRefusal(path, error);
    return ok;
}

void MainWindow::showDiskRefusal(const std::string& path, const std::string& why)
{
    diskRefusalAlternative_.reset();
    diskRefusalPath_.clear();
    diskRefusalFile_ = std::filesystem::path(path).filename().string();
    diskRefusalText_ = why;
    diskRefusalPending_ = true;
}

void MainWindow::renderDiskRefusal()
{
    constexpr const char* kTitle = "Disk not inserted###diskrefusal";
    if (diskRefusalPending_) {
        diskRefusalPending_ = false;
        ImGui::OpenPopup(kTitle);
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 26, 0),
                                        ImVec2(ImGui::GetFontSize() * 34, FLT_MAX));
    if (!ImGui::BeginPopupModal(kTitle, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
    ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.30f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION
                       "  %s was not inserted", diskRefusalFile_.empty()
                           ? "The disk" : diskRefusalFile_.c_str());
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted(diskRefusalText_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const float w = ImGui::GetFontSize() * 6;
    bool insertElsewhere = false;
    if (diskRefusalAlternative_) {
        const std::string label = ICON_FA_ARROW_RIGHT "  Insert into " + diskRefusalAlternative_->label;
        if (ImGui::Button(label.c_str()) || ImGui::IsKeyPressed(ImGuiKey_Enter)) insertElsewhere = true;
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x);
    if (insertElsewhere || ImGui::Button(diskRefusalAlternative_ ? "Cancel" : "OK", ImVec2(w, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        (!diskRefusalAlternative_ && ImGui::IsKeyPressed(ImGuiKey_Enter)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (insertElsewhere) {
        const auto target = *diskRefusalAlternative_;
        const auto path = diskRefusalPath_;
        diskRefusalAlternative_.reset();
        insertDriveRackImage(target, path);
    }
}

void MainWindow::processDriveRackFileDrop() {
    if (const auto selected = driveRackFilePicker_.render()) {
        if (driveRackPickerProfile_ != activeProfile) {
            driveRackFilePicker_.complete(false, "Machine changed. Cancel and select a drive again.");
        } else if (selected->boot) {
            std::string error;
            const bool ok = insertAndBootImage(selected->path, error);
            tapeStatusMessage = ok ? "Disk inserted and booted: " + selected->path
                                   : "Insert and boot failed: " + error;
            tapeStatusUntil = lastFrameTime + (ok ? 5.0 : 10.0);
            driveRackFilePicker_.complete(ok, error);
        } else {
            const bool ok = insertDriveRackImage(selected->target, selected->path, /*explain=*/false);
            driveRackFilePicker_.complete(ok, ok ? "" : tapeStatusMessage);
        }
    }
    if (driveRackPendingDrop_.empty()) return;
    auto paths = std::move(driveRackPendingDrop_);
    driveRackPendingDrop_.clear();
    if (driveRackHitFrame_ == ImGui::GetFrameCount()) {
        for (const auto& hit : driveRackHits_) {
            if (driveRackDropPosition_.x < hit.x0 || driveRackDropPosition_.x >= hit.x1 ||
                driveRackDropPosition_.y < hit.y0 || driveRackDropPosition_.y >= hit.y1) continue;
            insertDriveRackImage(hit.drive, paths.front());
            return; // No fallback to another drive, including on error or an occupied bay.
        }
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        tapeStatusMessage = "Close the dialog before dropping a disk onto a drive.";
        tapeStatusUntil = lastFrameTime + 4.0;
        return;
    }
    std::vector<const char*> raw;
    for (const auto& path : paths) raw.push_back(path.c_str());
    onFileDrop(static_cast<int>(raw.size()), raw.data());
}

// `--mount sNdM:path` (WASM-004): a drive named by slot and drive, as the
// browser's per-drive link parameters name it, filled through the rack's own
// mount — the same refusals (occupied drive, wrong medium) and no boot.
bool MainWindow::mountIntoDrive(int slot, int drive, const std::string& path, std::string& errOut,
                                const std::string& bootDisk)
{
    pom2::DrivePicture target;
    target.slot = slot;
    target.bay = drive;
    target.label = "S" + std::to_string(slot) + ",D" + std::to_string(drive + 1);
    bool occupied = false;
    std::string occupant;
    {
        auto st = controller->lockState();
        SlotPeripheral* card = st.memory().slotBus().peripheral(slot);
        if (auto* disk = dynamic_cast<DiskIICard*>(card)) {
            if (drive < 0 || drive >= DiskIICard::kDriveCount) {
                errOut = target.label + ": a Disk II has drives 1 and 2";
                return false;
            }
            target.kind = pom2::DrivePictureKind::DiskII;
            occupied = disk->isDiskLoaded(drive);
            if (occupied) occupant = disk->getDiskPath(drive);
        } else if (auto* media = dynamic_cast<pom2::MountableMediaCard*>(card)) {
            if (drive < 0 || drive >= media->bayCount()) {
                errOut = target.label + ": the card in slot " + std::to_string(slot) + " has " +
                         std::to_string(media->bayCount()) + " drive(s)";
                return false;
            }
            const auto info = media->bayInfo(drive);
            target.kind = info.typeKey == "35"  ? pom2::DrivePictureKind::Floppy35
                        : info.typeKey == "hdv" ? pom2::DrivePictureKind::HardDisk
                                                : pom2::DrivePictureKind::Generic;
            occupied = info.loaded;
            occupant = info.path;
        } else {
            errOut = target.label + ": no drive in slot " + std::to_string(slot);
            return false;
        }
    }
    // The mount itself records nothing: the storage layer writes into a
    // read-only scratch store. (Like every mounted disk, the drive's path is
    // still remembered at quit, as the positional disk's always was.)
    pom2::Settings transient;
    transient.setReadOnly(true);
    if (!occupied)
        return pom2::mountEmptyDrive(*controller, transient, *storageCoordinator_, target, path, errOut);
    // A drive the startup restore already filled is REPLACED, as the
    // positional boot disk replaces its own: refusing it made a link or
    // `--mount s6d2:` fail silently whenever S6D2 had a remembered disk. The
    // storage layer flushes a dirty outgoing medium first.
    if (!bootDisk.empty() && occupant == bootDisk) {
        errOut = target.label + " holds the disk that was just booted";
        return false;
    }
    std::string local = path;
    if (pom2::needsMediaResolve(path)) {
        const auto got = pom2::resolveMediaSource(path, pom2::mediaCacheDir());
        if (!got.ok) { errOut = path + ": " + got.error; return false; }
        local = got.localPath;
    }
    // The same rule as an empty drive (mountEmptyDrive): a disk only goes
    // into a drive made for it. An HDV bay adopts any 512-byte-block image,
    // so without this a 140 KB .dsk replaced its hard disk (round 2).
    const DiskSlotClass kind = classifyDiskForSlot(local);
    const bool fits =
        target.kind == pom2::DrivePictureKind::DiskII   ? kind == DiskSlotClass::Floppy525 :
        target.kind == pom2::DrivePictureKind::Floppy35 ? kind == DiskSlotClass::Sony35 :
        target.kind == pom2::DrivePictureKind::HardDisk ? kind == DiskSlotClass::Hdv :
                                                          (kind == DiskSlotClass::Sony35 || kind == DiskSlotClass::Hdv);
    if (!fits) {
        errOut = target.label + " does not take " + std::filesystem::u8path(local).filename().u8string();
        return false;
    }
    const auto result = target.kind == pom2::DrivePictureKind::DiskII
        ? storageCoordinator_->mountDiskII(*controller, transient, slot, drive, local, false)
        : storageCoordinator_->mountMediaBay(*controller, transient, slot, drive, local);
    errOut = result.error;
    return result.ok;
}

#ifdef __EMSCRIPTEN__
void MainWindow::copyBrowserSetupLink()
{
    // [{"key":"s6d1","path":"/…"}] — MEMFS paths are POM2's own (bundle
    // paths, /uploads/…), but a quote or a backslash is escaped all the same.
    auto quote = [](const std::string& v) {
        std::string o = "\"";
        for (char c : v) {
            if (c == '"' || c == '\\') o.push_back('\\');
            if (static_cast<unsigned char>(c) >= 0x20) o.push_back(c);
        }
        return o + "\"";
    };
    std::string json = "[";
    {
        auto st = controller->lockState();
        auto& bus = st.memory().slotBus();
        for (int s = 7; s >= 1; --s) {
            SlotPeripheral* card = bus.peripheral(s);
            std::vector<std::pair<int, std::string>> held;
            if (auto* disk = dynamic_cast<DiskIICard*>(card)) {
                for (int d = 0; d < DiskIICard::kDriveCount; ++d)
                    if (disk->isDiskLoaded(d)) held.emplace_back(d, disk->getDiskPath(d));
            } else if (auto* media = dynamic_cast<pom2::MountableMediaCard*>(card)) {
                for (int b = 0; b < media->bayCount() && b < 14; ++b) {
                    const auto info = media->bayInfo(b);
                    if (!info.path.empty()) held.emplace_back(b, info.path);
                }
            }
            for (const auto& [d, p] : held) {
                if (json.size() > 1) json += ",";
                json += "{\"key\":" + quote("s" + std::to_string(s) + "d" + std::to_string(d + 1)) +
                        ",\"path\":" + quote(p) + "}";
            }
        }
    }
    json += "]";
    const std::string preset(pom2::profileConfig(activeProfile).key);
    const int skipped = EM_ASM_INT({
        return Module.pom2CopySetupLink ? Module.pom2CopySetupLink(UTF8ToString($0), UTF8ToString($1)) : -1;
    }, preset.c_str(), json.c_str());
    tapeStatusMessage = skipped < 0 ? std::string("Could not build the link")
        : skipped == 0 ? std::string("Link to this setup copied (also in the address bar)")
        : "Link copied; " + std::to_string(skipped) +
          " disk(s) imported from this computer cannot be linked";
    tapeStatusUntil = lastFrameTime + 6.0;
}
#endif
