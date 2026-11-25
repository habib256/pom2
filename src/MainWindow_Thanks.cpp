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

// MainWindow — Help ▸ Thank you: the credits (ThanksCredits.h) rolled like
// the end of a film. A long, slow, centred scroll on black that starts from
// the bottom, loops, and can be taken in hand: the wheel moves it, a click
// pauses it.

#include "MainWindow.h"

#include "Pom2SfxHost.h"
#include "Pom2Theme.h"
#include "sfxbeep/SfxEditor.h"
#include "ThanksCredits.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

void MainWindow::renderThanksWindow()
{
    if (!showThanks_) { thanksScroll_ = -1.0f; return; }
    ImGui::SetNextWindowSize(ImVec2(640, 620), ImGuiCond_FirstUseEver);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    const bool visible = ImGui::Begin("Thank you###thanksWindow", &showThanks_,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleColor();
    if (!visible) { ImGui::End(); return; }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area   = ImGui::GetContentRegionAvail();
    const float  base   = ImGui::GetFontSize();
    // Line heights by kind: a name, a title, a note, a blank.
    const float size[4]   = {base * 1.15f, base * 1.55f, base * 1.0f, base};
    const float height[4] = {size[0] * 1.45f, size[1] * 2.1f, size[2] * 1.5f, base * 1.2f};
    // The credits open on the Apple II photograph, the About box's, centred.
    ensureAboutImageLoaded();
    const bool  hasPhoto = aboutImageTex_ && aboutImageW_ > 0 && aboutImageH_ > 0;
    const float photoW   = hasPhoto ? std::min(300.0f, area.x * 0.6f) : 0.0f;
    const float photoH   = hasPhoto ? photoW * static_cast<float>(aboutImageH_) /
                                               static_cast<float>(aboutImageW_) : 0.0f;
    const float photoBlock = hasPhoto ? photoH + base * 3.0f : 0.0f;
    float total = photoBlock;
    for (const pom2::CreditLine& line : pom2::kThanksCredits) total += height[line.kind];

    // The whole area takes the mouse: wheel to move, click to pause.
    ImGui::InvisibleButton("##credits", ImVec2(std::max(area.x, 1.0f), std::max(area.y, 1.0f)));
    if (ImGui::IsItemClicked()) thanksPaused_ = !thanksPaused_;
    const ImGuiIO& io = ImGui::GetIO();
    if (thanksScroll_ < 0.0f) {              // just opened
        // Open ON the photograph, centred in the window, and hold it a
        // moment before the credits start to roll.
        thanksScroll_ = hasPhoto ? (area.y + photoH) * 0.5f : 0.0f;
        thanksHold_   = hasPhoto ? 3.0f : 0.0f;
        thanksPaused_ = false;
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
        thanksScroll_ -= io.MouseWheel * base * 4.0f;
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) thanksHold_ = 0.0f;
    if (thanksHold_ > 0.0f)  thanksHold_ -= io.DeltaTime;
    else if (!thanksPaused_) thanksScroll_ += io.DeltaTime * base * 1.5f;   // a film's pace: about three minutes
    // One lap: the text enters from the bottom edge and leaves by the top.
    const float lap = total + area.y;
    thanksScroll_ = std::fmod(thanksScroll_, lap);
    if (thanksScroll_ < 0.0f) thanksScroll_ += lap;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, ImVec2(origin.x + area.x, origin.y + area.y), true);
    const pom2::Palette& colours = pom2::palette();
    const ImU32 nameColour  = IM_COL32(235, 235, 235, 255);
    const ImU32 titleColour = colours.accent;
    const ImU32 noteColour  = IM_COL32(150, 150, 150, 255);
    ImFont* font = ImGui::GetFont();
    float y = origin.y + area.y - thanksScroll_;
    if (hasPhoto) {
        if (y + photoH > origin.y && y < origin.y + area.y) {
            const float x = origin.x + (area.x - photoW) * 0.5f;
            draw->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(aboutImageTex_)),
                           ImVec2(x, y), ImVec2(x + photoW, y + photoH));
        }
        y += photoBlock;
    }
    for (const pom2::CreditLine& line : pom2::kThanksCredits) {
        const float h = height[line.kind];
        if (line.kind != 3 && y + h > origin.y && y < origin.y + area.y) {
            const float s = size[line.kind];
            const ImU32 colour = line.kind == 1 ? titleColour : line.kind == 2 ? noteColour : nameColour;
            // "Name|role": the name, then the role dimmer and a little
            // smaller, the pair centred as one.
            const char* bar = line.kind == 0 ? std::strchr(line.text, '|') : nullptr;
            const float roleSize = s * 0.82f, gap = bar ? s * 0.7f : 0.0f;
            const ImVec2 nameExtent = font->CalcTextSizeA(s, 10000.0f, 0.0f, line.text, bar);
            const ImVec2 roleExtent = bar ? font->CalcTextSizeA(roleSize, 10000.0f, 0.0f, bar + 1)
                                          : ImVec2(0.0f, 0.0f);
            const float x = origin.x + (area.x - (nameExtent.x + gap + roleExtent.x)) * 0.5f;
            draw->AddText(font, s, ImVec2(x, y + (h - s) * 0.5f), colour, line.text, bar);
            if (bar)
                draw->AddText(font, roleSize, ImVec2(x + nameExtent.x + gap, y + (h - roleSize) * 0.5f),
                              noteColour, bar + 1);
        }
        y += h;
    }
    // Fade the top and bottom edges into the black, as on a screen.
    const float fade = std::min(60.0f, area.y * 0.2f);
    const ImU32 black = IM_COL32(0, 0, 0, 255), clear = IM_COL32(0, 0, 0, 0);
    draw->AddRectFilledMultiColor(origin, ImVec2(origin.x + area.x, origin.y + fade),
                                  black, black, clear, clear);
    draw->AddRectFilledMultiColor(ImVec2(origin.x, origin.y + area.y - fade),
                                  ImVec2(origin.x + area.x, origin.y + area.y),
                                  clear, clear, black, black);
    draw->PopClipRect();
    if (thanksPaused_)
        draw->AddText(ImVec2(origin.x + 8.0f, origin.y + area.y - base - 6.0f), noteColour,
                      "paused - click to resume, wheel to move");
    ImGui::End();
}

// Tools > Speaker SFX editor: POM1's beeper editor, on POM2's speaker.
void MainWindow::renderSfxEditorWindow()
{
    if (!showSfxEditor_) return;
    // Registered on the audio bus the first time the window opens; the
    // registration is idempotent (MainWindow::registerAudioSource).
    registerAudioSource(&sfxHost_->audio());
    ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Speaker SFX Editor", &showSfxEditor_)) sfxEditor_->render();
    ImGui::End();
    if (!showSfxEditor_) sfxHost_->stopPreview();
}
