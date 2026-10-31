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

// The card monitor: what reaches the screen when a card has its own picture
// (Videx Videoterm, UltraTerm, VidHD), and a second monitor that always shows
// that picture.
//
// A Videoterm reaches the monitor through Videx's Soft Video Switch: the
// card's 80 columns while annunciator 0 is high and the machine is in text,
// the Apple's video otherwise (SVS-000 schematic, VidexVideotermCard.h).
// Software that leaves 80 columns without turning AN0 off — Merlin's VID 0 —
// keeps the stale 80-column screen up on the real machine. Owners without
// the switch used a manual one or two monitors. The four choices here are
// those set-ups, as sbabcoc's ReplicApple2Plus offers them: the switch,
// Apple video only, card video only, and two monitors (the Apple's video in
// the main window, the card's in this panel).

#include "MainWindow.h"
#include "Apple2Display.h"
#include "CardVideoSource.h"
#include "CpuClock.h"
#include "EmulationController.h"
#include "Pom2GL.h"
#include "Settings.h"

#include "imgui.h"

#include <algorithm>
#include <vector>

void MainWindow::applyCardMonitorSetting()
{
    const int m = std::clamp(settings->getInt("card_monitor_switch", 0), 0, 2);
    display->setCardMonitor(static_cast<Apple2Display::CardMonitor>(m));
}

void MainWindow::renderCardMonitorPanel()
{
    if (!show(pom2::PanelId::CardMonitor)) return;
    ImGui::SetNextWindowSize(ImVec2(760, 330), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Card Monitor (80-column card)###cardMonitor",
                      &show(pom2::PanelId::CardMonitor))) {
        ImGui::End();
        return;
    }

    int sel = static_cast<int>(display->cardMonitor());
    bool changed = false;
    changed |= ImGui::RadioButton("Soft video switch", &sel, 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The card's own rule: a Videoterm shows its 80 columns while\n"
                          "AN0 is on ($C059) and the machine is in text, the Apple's\n"
                          "video otherwise. Videx's Soft Video Switch.");
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Apple video", &sel, 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The main screen always shows the Apple's video; with this\n"
                          "panel open, that is two monitors.");
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Card video", &sel, 2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The main screen always shows the card's picture, as a\n"
                          "monitor cabled to the card alone.");
    if (changed) {
        settings->setInt("card_monitor_switch", sel);
        if (!settingsReadOnly()) settings->save();
        auto st = controller->lockState();
        display->setCardMonitor(static_cast<Apple2Display::CardMonitor>(sel));
    }

    // Paint under the lock — paintPicture reads the card's VRAM and CRTC —
    // and upload after.
    int w = 0, h = 0;
    {
        auto st = controller->lockState();
        if (const pom2::CardVideoSource* card = display->cardVideoSource()) {
            w = card->pictureWidth();
            h = card->pictureHeight();
            if (w > 0 && h > 0) {
                cardMonitorPixels_.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
                card->paintPicture(cardMonitorPixels_.data(), 0xFFFFFFFFu, 0xFF000000u,
                                   st.memory().videoNow(),
                                   pom2VideoTiming(st.memory().videoStandard()).cpuClockHz);
            }
        }
    }
    if (w <= 0 || h <= 0) {
        ImGui::TextDisabled("No card with its own picture is plugged (Videx Videoterm\n"
                            "on a ][ / ][+, UltraTerm, VidHD).");
        ImGui::End();
        return;
    }
    if (!cardMonitorTex_) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        cardMonitorTex_ = tex;
    }
    glBindTexture(GL_TEXTURE_2D, cardMonitorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 cardMonitorPixels_.data());

    // Fit, keeping the card's aspect (its pixels are not square: a
    // Videoterm is 720 × 216 for a 4:3 tube).
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float aspect = 4.0f / 3.0f;
    float dw = avail.x, dh = avail.x / aspect;
    if (dh > avail.y) { dh = avail.y; dw = dh * aspect; }
    if (dw > 1.0f && dh > 1.0f)
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(cardMonitorTex_)),
                     ImVec2(dw, dh));
    ImGui::End();
}
