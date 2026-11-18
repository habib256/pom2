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

// MainWindow_RomOffer — first-launch offer and on-screen firmware recovery.
//
// A desktop package carries no firmware at all (packaging/bundle.manifest:
// every dump is copyrighted); they are fetched — POM2's archive.org archive,
// RetroBIOS as the fallback — by ROM Status → Download missing. A new user does not know that panel exists,
// so the desktop build asks ONCE: the first launch that finds catalogued dumps
// missing opens this modal, and either answer sets `rom_download_offered`.
// "Download" opens ROM Status and starts its fetch there, so progress, cancel
// and the outcome live where the user will look for them again. Never in the
// browser build (no helper processes, no fetch) and never in a settings-read-
// only (kiosk) session, which could not remember the answer.

#include "MainWindow.h"

#include "IconsFontAwesome6.h"
#include "EmulationController.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SystemProfile.h"
#include "Logger.h"
#include "PanelCatalog.h"
#include "RomFetch.h"
#include "RomStatus_ImGui.h"
#include "Settings.h"

#include "imgui.h"

#include <optional>
#include <string>

#ifndef __EMSCRIPTEN__
namespace {
constexpr const char* kRomOfferTitle = "Download the Apple II ROMs?";
}
#endif

void MainWindow::renderRomDownloadOffer()
{
#ifdef __EMSCRIPTEN__
    return;
#else
    if (!romOfferChecked_) {
        // First frame, not the constructor: main() turns kiosk on after
        // constructing the window, and a kiosk run must not be asked.
        romOfferChecked_ = true;
        if (!settings || settingsReadOnly() ||
            settings->getBool("rom_download_offered", false))
            return;
        // The same planner the button runs: missing, or present but not
        // the dump POM2 vouches for. A full tree (a source build) is never
        // asked, and is asked later if dumps go missing.
        romOfferMissing_ = static_cast<int>(pom2::romsToFetch().size());
        if (romOfferMissing_ == 0) return;
        pom2::log().info("ROM", "first-launch offer: " +
                         std::to_string(romOfferMissing_) +
                         " catalogued dumps are not installed; a download "
                         "would go to " + pom2::writableRomsDir().string());
        ImGui::OpenPopup(kRomOfferTitle);
    }
    if (!ImGui::BeginPopupModal(kRomOfferTitle, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextWrapped(
        "POM2 does not ship Apple's firmware: it is copyrighted. The "
        "machines (][, ][+, //e, //c, //c Plus), the card firmware and the "
        "international character sets are %d ROM files.", romOfferMissing_);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "They can be downloaded now from POM2's ROM archive on archive.org, "
        "with the RetroBIOS collection (github.com/Abdess/retrobios) as a "
        "fallback. Every file is checked against its SHA-256 before it is "
        "installed.");
    ImGui::TextDisabled("Saving into: %s", pom2::displayPath(pom2::writableRomsDir()).c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("Later: Help \xe2\x96\xb8 ROM Status \xe2\x96\xb8 Download missing ROMs.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    auto remember = [this] {
        settings->setBool("rom_download_offered", true);
        settings->save();
    };
    if (ImGui::Button(ICON_FA_DOWNLOAD " Download", ImVec2(160, 0))) {
        openRomSetup(true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Not now", ImVec2(160, 0))) {
        remember();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
#endif
}

void MainWindow::openRomSetup(bool download)
{
    // Panels are not rendered in kiosk mode. Leave it before opening setup;
    // the session's settings-read-only policy remains in force.
    if (kiosk_) toggleKioskMode();
    if (!romStatusPanel)
        romStatusPanel = std::make_unique<pom2::RomStatus_ImGui>();
    show(pom2::PanelId::RomStatus) = true;
    if (download) {
        // An explicit screen-button click also answers the first-launch offer.
        romOfferChecked_ = true;
        if (settings && !settingsReadOnly() &&
            !settings->getBool("rom_download_offered", false)) {
            settings->setBool("rom_download_offered", true);
            settings->save();
        }
        romStatusPanel->startDownload();
    }
}

void MainWindow::reloadMissingFirmware()
{
    // "Loads", not "exists" — the real loader's staging step, no lock held.
    const auto mainRomLoads = [](const std::string& candidate) {
        Memory::AppleIIRomImage image;
        std::string error;
        return Memory::prepareAppleIIRom(candidate.c_str(), image, error);
    };
    const auto profileRomLoads = [&](pom2::SystemProfile p) {
        return !pom2::findFirstResourceThat(
            pom2::profileConfig(p).romProbeOrder, mainRomLoads).empty();
    };

    // No main ROM is loaded: the machine on screen is whatever a ROM-less
    // start fell back to (a bare ][+), not the one this install will be at
    // its next launch. Reloading that profile's main ROM alone — what this
    // did — gave a ][+ with no character ROM and none of the ROM-gated cards,
    // right after a download whose next launch is a //e Enhanced PAL with
    // the full slot map (bug hunt 2026-11-18 round 2). So resolve the
    // machine the way the constructor does — the saved `system_profile`
    // when there is one and its firmware loads, else the default order
    // (pom2::defaultProfileForRoms) — and run the full profile switch: ROM,
    // character ROM, slots, video standard. A restore, not a choice, so
    // nothing is persisted here (the normal exit does that).
    if (!romLoaded_) {
        std::optional<pom2::SystemProfile> target;
        // The command line named the machine (`--ii-plus`, `--preset`): that
        // one or none — a session asked to be a ][+ does not come up a //e
        // because the download also brought the //e's firmware.
        if (launchProfile_ && profileRomLoads(*launchProfile_)) target = launchProfile_;
        const std::string saved = (launchProfile_ || !settings)
            ? std::string() : settings->getString("system_profile", "");
        if (!saved.empty()) {
            const pom2::SystemProfile p = pom2::profileFromKey(saved);
            if (profileRomLoads(p)) target = p;
        }
        // A profile other than the ROM-less fallback was put there on
        // purpose (a session, a failed File ▸ Reload ROM): keep it if it can
        // be loaded now.
        if (!target && !launchProfile_ &&
            activeProfile != pom2::SystemProfile::AppleIIPlus &&
            profileRomLoads(activeProfile))
            target = activeProfile;
        if (!target && !launchProfile_)
            target = pom2::defaultProfileForRoms(profileRomLoads);
        if (target) {
            restoringSavedProfile_ = true;
            applyProfile(*target);
            restoringSavedProfile_ = false;
            // A refusal (a card that could not save its disk) is reported by
            // applyProfile on the status line; say so here too, where the
            // user is looking.
            if (!romLoaded_) romStatus = "Reload refused — see the status line";
            return;
        }
        // Nothing loads: fall through for the loader's own error text.
    }

    // First candidate that loads; when none does, the first that exists (or
    // the last path tried) so the message names a real file.
    std::string path = pom2::findFirstResourceThat(
        pom2::profileConfig(activeProfile).romProbeOrder, mainRomLoads);
    if (path.empty())
        path = pom2::findFirstResource(pom2::profileConfig(activeProfile).romProbeOrder);
    if (path.empty()) path = romPath;
    // Stage the bytes unlocked, then install the SAME bytes under the lock.
    // No second file open or filesystem probe can stall the CPU/UI here.
    Memory::AppleIIRomImage image;
    std::string error;
    if (!Memory::prepareAppleIIRom(path.c_str(), image, error)) {
        romStatus = error;
        return;
    }
    bool loaded;
    {
        auto state = controller->lockState();
        loaded = state.memory().loadAppleIIRom(
            image, pom2::profileUsesLowerRomHalf(activeProfile));
        if (!loaded) error = state.memory().getLastError();
    }
    if (!loaded) {
        romStatus = error;
        return;
    }
    romPath = path;
    romLoaded_ = true;
    romStatus = "loaded: " + path;
    controller->hardReset();
}

void MainWindow::renderMissingFirmwareNotice()
{
    const auto& profile = pom2::profileConfig(activeProfile);
    // A child remains part of the screen's dock/window ordering, unlike a
    // top-level overlay that would cover menus. It scrolls on small screens.
    if (ImGui::BeginChild("##missingFirmware", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGui::TextUnformatted("Firmware missing");
        ImGui::TextWrapped("%.*s", static_cast<int>(profile.displayName.size()),
                           profile.displayName.data());
        ImGui::Spacing();
        if (kiosk_) {
            ImGui::TextWrapped("This machine needs a system ROM before it can start.");
            if (ImGui::Button("Open firmware setup")) openRomSetup(false);
        } else {
#ifdef __EMSCRIPTEN__
            ImGui::TextWrapped("Select a machine whose firmware is bundled, or restore "
                               "a supported session. ROM downloads are unavailable in the browser.");
#else
            const bool busy = romStatusPanel && romStatusPanel->downloadRunning();
            ImGui::TextWrapped("Download the ROMs or drop your ROM file onto this window. "
                               "After downloading, choose Reload ROM.");
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Download missing ROMs")) openRomSetup(true);
            if (ImGui::Button("Reload ROM")) reloadMissingFirmware();
            ImGui::EndDisabled();
            if (busy) ImGui::TextWrapped("Downloading ROMs. Progress and Cancel are in ROM Status.");
#endif
            if (ImGui::Button("ROM status / details")) openRomSetup(false);
        }
        ImGui::Spacing();
        ImGui::TextWrapped("%s", romStatus.c_str());
    }
    ImGui::EndChild();
}
