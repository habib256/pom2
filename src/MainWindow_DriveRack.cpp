// POM2 — GPL-3.0-or-later
#include "MainWindow.h"
#include "DiskIICard.h"
#include "DriveRack_ImGui.h"
#include "DriveRackMount.h"
#include "EmulationController.h"
#include "IconsFontAwesome6.h"
#include "Memory.h"
#include "MountableMediaCard.h"
#include "StorageCoordinator.h"
#include "SystemProfile.h"
#include <algorithm>
#include <filesystem>
#include <utility>

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

bool MainWindow::insertDriveRackImage(const pom2::DrivePicture& target, const std::string& path) {
    std::string error;
    const bool ok = pom2::mountEmptyDrive(*controller, *settings, *storageCoordinator_, target, path, error);
    tapeStatusMessage = ok ? "Inserted into " + target.label + ": " + path : error;
    tapeStatusUntil = lastFrameTime + 5.0;
    return ok;
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
            const bool ok = insertDriveRackImage(selected->target, selected->path);
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
