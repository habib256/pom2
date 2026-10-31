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

// File ▸ Open disk from URL: a link to a disk image (or to a .zip / .gz of
// one), fetched by MediaSource into the media cache on a thread of its own —
// the window keeps painting and the download can be cancelled — then booted
// through insertAndBootImage like a dropped file.

#include "MainWindow.h"
#include "Logger.h"
#include "MediaSource.h"
#include "ThreadGuard.h"

#include "imgui.h"

#include <filesystem>

void MainWindow::renderOpenUrlPopup()
{
    // The download finished: boot what it gave, on this (the UI) thread.
    if (urlFetch_ && urlFetch_->done.load()) {
        if (urlFetch_->t.joinable()) urlFetch_->t.join();
        std::string err = urlFetch_->error;
        if (err.empty() && insertAndBootImage(urlFetch_->localPath, err)) {
            tapeStatusMessage = "Opened from URL: " +
                std::filesystem::path(urlFetch_->localPath).filename().string();
            pom2::log().info("URL", urlFetch_->url + " -> " + urlFetch_->localPath);
        } else {
            tapeStatusMessage = "URL failed: " + err;
            pom2::log().warn("URL", urlFetch_->url + ": " + err);
            showDiskRefusal(urlFetch_->url, err);
        }
        tapeStatusUntil = lastFrameTime + 5.0;
        urlFetch_.reset();
    }

    if (openUrlRequested_) {
        openUrlRequested_ = false;
        ImGui::OpenPopup("Open disk from URL");
    }
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Open disk from URL", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const bool busy = urlFetch_ != nullptr;
    ImGui::TextWrapped("http:// or https:// link to a disk image, or to a .zip or .gz holding one "
                       "(Asimov, archive.org…). It is downloaded into the media cache and booted.");
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputText("##url", urlInput_, sizeof urlInput_,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string url = urlInput_;
    const bool valid = pom2::isHttpUrl(url);
    ImGui::BeginDisabled(!valid);
    const bool open = ImGui::Button("Download and boot") || (enter && valid);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (open && !busy) {
        urlFetch_ = std::make_unique<UrlFetch>();
        urlFetch_->url = url;
        UrlFetch* f = urlFetch_.get();
        f->t = pom2::guardedThread("MediaURL", [f] {
            const auto got = pom2::resolveMediaSource(f->url, pom2::mediaCacheDir(), &f->abort);
            if (got.ok) f->localPath = got.localPath;
            else        f->error = got.error;
            f->done = true;
        });
    }
    ImGui::SameLine();
    if (busy) {
        ImGui::TextDisabled("downloading…");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) urlFetch_->abort = true;
    } else if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
    }
    // Close the dialog once the result is in; the boot happens above.
    if (busy && urlFetch_->done.load()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
