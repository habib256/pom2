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

#include "ScreenInspector_ImGui.h"

#include "Apple2Display.h"
#include "AtomicFileReplace.h"
#include "BeamPosition.h"
#include "EmulationController.h"
#include "HostFileTag.h"
#include "Memory.h"
#include "Pom2Theme.h"
#include "ResourcePaths.h"
#include "ScreenProbe.h"

#include "imgui.h"
#include "Pom2GL.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace pom2 {

ScreenInspector_ImGui::~ScreenInspector_ImGui()
{
    // The GL context may already be gone at shutdown; leaking one texture
    // name then is harmless, deleting through a dead context is not.
}

void ScreenInspector_ImGui::render(EmulationController& ctrl, Apple2Display& display,
                                   bool* open, const std::function<void()>& savePng)
{
    if (open && !*open) return;
    ImGui::SetNextWindowSize(ImVec2(900, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Screen Inspector", open)) { ImGui::End(); return; }

    // ── One lock: the per-line states, the beam, and the picture ─────────
    std::array<Memory::DisplayState, 192> lines;
    BeamPosition beam;
    bool beamView = false;
    int w = 0, h = 0;
    {
        auto st = ctrl.lockState();
        std::lock_guard<std::mutex> demod(display.demodMutex());   // state → demod
        Memory& mem = st.memory();
        lines    = mem.displayStatePerLine();
        beam     = beamPositionAt(mem.videoNow(), mem.videoStandard());
        beamView = mem.beamView();
        w = display.width();
        h = display.height();
        const uint32_t* src = display.pixels();
        pixels_.assign(src, src + static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    }
    // A card's own picture (Videoterm, VidHD) is not the Apple's 192 lines:
    // show it, but there is nothing to probe.
    const bool applePicture = h == 192;

    if (!texture_) glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    if (w != texW_ || h != texH_) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels_.data());
        texW_ = w; texH_ = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                        pixels_.data());
    }

    // ── Controls ─────────────────────────────────────────────────────────
    bool wantBeam = beamView;
    if (ImGui::Checkbox("Beam view", &wantBeam)) {
        auto st = ctrl.lockState();
        st.memory().setBeamView(wantBeam);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The tube at THIS cycle: the frame in progress above the beam,\n"
                          "the previous frame from the beam down. Off: the last completed frame.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("Zoom", &zoom_, 1.0f, 6.0f, "x%.1f");
    ImGui::SameLine();
    if (ImGui::Button("Save PNG") && savePng) savePng();
    ImGui::SameLine();
    ImGui::BeginDisabled(!applePicture);
    if (ImGui::Button("Save hi-res as fetched")) {
        std::array<uint8_t, 0x2000> page{};
        {
            auto st = ctrl.lockState();
            st.memory().captureFetchedHiRes(page.data());
        }
        // File work with the lock released (CLAUDE.md: never across I/O).
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        // BIN, load address $2000: the convention HostFileTag.h owns.
        const std::filesystem::path dir = userDataDir() / "screenshots";
        const std::filesystem::path out =
            dir / (std::string("SCREEN-") + stamp + hostFileTag(0x06, 0x2000));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (writeFileAtomic(out, page.data(), page.size(), ec))
            status_ = "Saved " + out.string();
        else
            status_ = "Save failed: " + ec.message();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("An 8 KB hi-res page built from what the beam read: each line from\n"
                          "the page it displayed, with the bytes RAM held at that moment.\n"
                          "Opens in the Paint editor and in any Apple II tool.");
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    // The readout sits ABOVE the picture so it stays visible whatever the
    // window's height; it describes the picture drawn below it, so its two
    // lines are reserved here and filled once the picture's geometry is known.
    const ImVec2 readoutAt = ImGui::GetCursorPos();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 3.0f));

    // ── The picture ──────────────────────────────────────────────────────
    // 560 x 384 at zoom 1, whatever the framebuffer's width (280 or 560).
    const ImVec2 size(280.0f * zoom_, 192.0f * zoom_);
    ImGui::BeginChild("##picture", ImVec2(0, 0),
                      true, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Image(static_cast<ImTextureID>(texture_), size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (applePicture && beamView && !beam.vbl) {
        // The beam: a line across its scanline, a tick at its byte.
        const float y0 = at.y + size.y * static_cast<float>(beam.line) / 192.0f;
        const float y1 = at.y + size.y * static_cast<float>(beam.line + 1) / 192.0f;
        draw->AddLine(ImVec2(at.x, y1), ImVec2(at.x + size.x, y1), IM_COL32(255, 80, 80, 160));
        if (beam.column >= 0) {
            const float x = at.x + size.x * static_cast<float>(beam.column) / 40.0f;
            draw->AddRectFilled(ImVec2(x, y0), ImVec2(x + size.x / 40.0f, y1),
                                IM_COL32(255, 80, 80, 220));
        }
    }
    ImGui::EndChild();

    const ImVec2 afterPicture = ImGui::GetCursorPos();
    ImGui::SetCursorPos(readoutAt);

    // ── Readout ──────────────────────────────────────────────────────────
    {
    ScopedMonoFont mono;
    if (beam.vbl)      ImGui::Text("Beam  V %3d  H %2d  VBL", beam.line, beam.h);
    else if (beam.hbl) ImGui::Text("Beam  V %3d  H %2d  HBL", beam.line, beam.h);
    else               ImGui::Text("Beam  V %3d  H %2d  byte %2d", beam.line, beam.h, beam.column);
    if (!applePicture) {
        ImGui::TextDisabled("A card's own picture is on screen: nothing to probe.");
    } else if (hovered && size.x > 0 && size.y > 0) {
        const double fx = std::clamp(static_cast<double>((mouse.x - at.x) / size.x), 0.0, 0.999999);
        const int y = std::clamp(static_cast<int>((mouse.y - at.y) / size.y * 192.0f), 0, 191);
        const Memory::DisplayState& s = lines[static_cast<std::size_t>(y)];
        const ScreenProbe p = probeScreen(s.textMode, s.mixedMode, s.hiRes, s.page2,
                                          s.eightyCol, s.dhgr, s.eightyStore, y, fx);
        uint8_t value = 0;
        {
            auto st = ctrl.lockState();
            const Memory& mem = st.memory();
            value = (p.aux && mem.videoAuxData()) ? mem.videoAuxData()[p.address]
                                                  : mem.data()[p.address];
        }
        ImGui::Text("X %3d / %3d   Y %3d   %s page %d   %s",
                    p.x, p.xMax, y, p.mode, p.page2 ? 2 : 1,
                    s.mixedMode && !s.textMode ? "(MIXED)" : "");
        if (p.bit >= 0)
            ImGui::Text("byte %2d bit %d   $%04X%s = $%02X (now)",
                        p.byteColumn, p.bit, p.address, p.aux ? " aux" : "", value);
        else
            ImGui::Text("row %2d col %2d    $%04X%s = $%02X (now)",
                        p.row, p.byteColumn, p.address, p.aux ? " aux" : "", value);
        // Outline the byte under the mouse.
        const float bx = at.x + size.x * static_cast<float>(p.byteColumn) / 40.0f;
        const float by = at.y + size.y * static_cast<float>(y) / 192.0f;
        ImGui::GetForegroundDrawList()->AddRect(
            ImVec2(bx, by), ImVec2(bx + size.x / 40.0f, by + size.y / 192.0f),
            IM_COL32(255, 255, 0, 200));
    } else {
        ImGui::TextDisabled("Move the mouse over the picture: coordinates, mode, byte and address.");
    }
    }
    ImGui::SetCursorPos(afterPicture);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));   // an item, so the cursor move is legal
    ImGui::End();
}

} // namespace pom2
