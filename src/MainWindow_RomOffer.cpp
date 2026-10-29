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

// MainWindow_RomOffer — the first-launch "download the Apple II ROMs?" offer.
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
#include "Logger.h"
#include "PanelCatalog.h"
#include "RomFetch.h"
#include "RomStatus_ImGui.h"
#include "Settings.h"

#include "imgui.h"

#include <string>

namespace {
constexpr const char* kRomOfferTitle = "Download the Apple II ROMs?";
}

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
        remember();
        if (!romStatusPanel)
            romStatusPanel = std::make_unique<pom2::RomStatus_ImGui>();
        show(pom2::PanelId::RomStatus) = true;
        romStatusPanel->startDownload();
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
