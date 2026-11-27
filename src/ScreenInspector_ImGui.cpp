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
#include "BeamPosition.h"
#include "Disassembler6502.h"
#include "EmulationController.h"
#include "FetchedPagesFile.h"
#include "M6502.h"
#include "Memory.h"
#include "Pom2Theme.h"
#include "ResourcePaths.h"
#include "ScreenProbe.h"

#include "imgui.h"
#include "Pom2GL.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace pom2 {

ScreenInspector_ImGui::~ScreenInspector_ImGui()
{
    // The GL context may already be gone at shutdown; leaking one texture
    // name then is harmless, deleting through a dead context is not.
}

namespace {

// One colour per video mode, shared by the gutter and its legend.
struct ModeColour { const char* mode; ImU32 colour; };
const ModeColour kModeColours[] = {
    {"TEXT",   IM_COL32(150, 150, 150, 255)},
    {"TEXT80", IM_COL32(215, 215, 215, 255)},
    {"LORES",  IM_COL32(230, 130,  40, 255)},
    {"DLORES", IM_COL32(240, 205,  60, 255)},
    {"HIRES",  IM_COL32( 70, 190,  90, 255)},
    {"DHGR",   IM_COL32( 60, 190, 220, 255)},
};
const ImU32 kPage1Colour = IM_COL32(60, 70, 110, 255);
const ImU32 kPage2Colour = IM_COL32(110, 150, 255, 255);
const ImU32 kSplitColour = IM_COL32(255, 255, 255, 255);

ImU32 modeColour(const char* mode)
{
    for (const ModeColour& m : kModeColours)
        if (std::strcmp(m.mode, mode) == 0) return m.colour;
    return IM_COL32(80, 80, 80, 255);
}

ScreenProbe probeAt(const Memory::DisplayState& s, int y, double fx)
{
    return probeScreen(s.textMode, s.mixedMode, s.hiRes, s.page2,
                       s.eightyCol, s.dhgr, s.eightyStore, y, fx);
}

void legendSwatch(ImU32 colour, const char* label)
{
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + h - 4, p.y + h - 2), colour);
    ImGui::Dummy(ImVec2(h - 2, h));
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0.0f, 14.0f);
}

// The beam, drawn to be FOUND: a cross over the whole picture, in two tones.
// One colour is never enough here — whatever it is, an Apple II picture can
// be that colour where the beam stands (the first marker was a red-orange
// line, lost on hi-res orange). A dark edge under a bright core shows on
// both. The byte under the beam keeps a frame around it, not a fill: it is
// the byte the user wants to SEE.
void drawBeamMarker(ImDrawList* draw, ImVec2 at, ImVec2 size, const BeamPosition& beam,
                    int lines, ImU32 colour)
{
    const ImU32 edge = IM_COL32(0, 0, 0, 230);
    const auto line = [&](ImVec2 a, ImVec2 b) {
        draw->AddLine(a, b, edge, 4.0f);
        draw->AddLine(a, b, colour, 2.0f);
    };
    const auto arrow = [&](ImVec2 tip, ImVec2 b, ImVec2 c) {
        draw->AddTriangleFilled(tip, b, c, colour);
        draw->AddTriangle(tip, b, c, edge, 1.5f);
    };
    const auto label = [&](ImVec2 where, const char* text) {
        draw->AddText(ImVec2(where.x + 1.0f, where.y + 1.0f), edge, text);
        draw->AddText(where, colour, text);
    };
    char text[48];
    if (beam.vbl) {
        // Below the picture: an arrow on the bottom edge, under the cycle
        // the beam is at in its line, and how far into blanking it is.
        const float x = at.x + size.x * std::clamp(static_cast<float>(beam.h - kBeamHblCycles) + 0.5f,
                                                   0.0f, 40.0f) / 40.0f;
        const float y = at.y + size.y;
        arrow(ImVec2(x, y - 1.0f), ImVec2(x - 7.0f, y - 13.0f), ImVec2(x + 7.0f, y - 13.0f));
        std::snprintf(text, sizeof text, "VBL  V %d of %d  H %d", beam.line, lines - 1, beam.h);
        const float wide = ImGui::CalcTextSize(text).x;
        // (max first: a label wider than the picture would hand clamp a
        // high bound under its low one.)
        const float left = at.x + 2.0f;
        label(ImVec2(std::clamp(x - wide * 0.5f, left, std::max(left, at.x + size.x - wide - 2.0f)),
                     y - 15.0f - ImGui::GetTextLineHeight()), text);
        return;
    }
    const float lineH = size.y / 192.0f, byteW = size.x / 40.0f;
    const float y0 = at.y + lineH * static_cast<float>(beam.line), y1 = y0 + lineH;
    const float ym = (y0 + y1) * 0.5f;
    if (beam.column < 0) {
        // Horizontal blanking: the beam is to the LEFT of this line's first
        // byte. The line it is about to draw, and an arrow on the left edge.
        line(ImVec2(at.x, ym), ImVec2(at.x + size.x, ym));
        arrow(ImVec2(at.x + 13.0f, ym), ImVec2(at.x + 1.0f, ym - 7.0f), ImVec2(at.x + 1.0f, ym + 7.0f));
        std::snprintf(text, sizeof text, "HBL  H %d", beam.h);
        label(ImVec2(at.x + 17.0f, ym < at.y + 20.0f ? ym + 4.0f : ym - 4.0f - ImGui::GetTextLineHeight()), text);
        return;
    }
    const float x0 = at.x + byteW * static_cast<float>(beam.column), x1 = x0 + byteW;
    const float xm = (x0 + x1) * 0.5f;
    line(ImVec2(at.x, ym), ImVec2(x0 - 3.0f, ym));
    line(ImVec2(x1 + 3.0f, ym), ImVec2(at.x + size.x, ym));
    line(ImVec2(xm, at.y), ImVec2(xm, y0 - 3.0f));
    line(ImVec2(xm, y1 + 3.0f), ImVec2(xm, at.y + size.y));
    draw->AddRect(ImVec2(x0 - 3.0f, y0 - 3.0f), ImVec2(x1 + 3.0f, y1 + 3.0f), edge, 0.0f, 0, 4.0f);
    draw->AddRect(ImVec2(x0 - 3.0f, y0 - 3.0f), ImVec2(x1 + 3.0f, y1 + 3.0f), colour, 0.0f, 0, 2.0f);
}

/// "PAGE2", "TEXT", "MIXED off" — one soft-switch throw, as a programmer
/// names it. Null for the display's own band cuts, which are no switch.
const char* switchText(Memory::VideoEventKind kind, bool on)
{
    using K = Memory::VideoEventKind;
    switch (kind) {
        case K::TextMode:    return on ? "TEXT" : "GRAPHICS";
        case K::MixedMode:   return on ? "MIXED on" : "MIXED off";
        case K::Page2:       return on ? "PAGE2" : "PAGE1";
        case K::HiRes:       return on ? "HIRES" : "LORES";
        case K::EightyCol:   return on ? "80COL on" : "80COL off";
        case K::Dhgr:        return on ? "DHGR on" : "DHGR off";
        case K::An3:         return on ? "AN3 on" : "AN3 off";
        case K::EightyStore: return on ? "80STORE on" : "80STORE off";
        case K::AltChar:     return on ? "ALTCHAR on" : "ALTCHAR off";
        case K::An2:         return on ? "AN2 on" : "AN2 off";
        case K::Cut:         return nullptr;
    }
    return nullptr;
}

std::string timeStamp()
{
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    return stamp;
}

} // namespace

void ScreenInspector_ImGui::render(EmulationController& ctrl, Apple2Display& display,
                                   bool* open, const std::function<void()>& savePng,
                                   const std::function<void()>& refreshDisplay,
                                   const std::function<void(uint16_t)>& showInListing)
{
    if (open && !*open) return;
    // Docked by the layout presets (MainWindow::applyDockLayout: the Debug
    // preset puts it on the left, beside the Debugger). This size only serves
    // a window the user has pulled out to float.
    ImGui::SetNextWindowSize(ImVec2(900, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Screen Inspector", open)) { ImGui::End(); return; }

    // The display is rendered by whoever shows it first this frame; with the
    // Apple II Screen hidden behind this panel's tab, that is us. No lock
    // held here: the refresh takes the state lock itself.
    if (refreshDisplay) refreshDisplay();

    // ── One lock: the frame's switches, the beam, and the picture ────────
    Memory::DisplayFrame frame;
    BeamPosition beam;
    bool beamView = false;
    bool beamComposite = false;     // the events ARE the beam view's two-frame composite
    int w = 0, h = 0;
    int frameLines = 262;
    VideoStandard standard = VideoStandard::NTSC;
    const bool running = ctrl.getMode() == EmulationController::Mode::Running;
    {
        auto st = ctrl.lockState();
        std::lock_guard<std::mutex> demod(display.demodMutex());   // state → demod
        Memory& mem = st.memory();
        frame    = mem.displayFrame();
        beam     = beamPositionAt(mem.videoNow(), mem.videoStandard());
        standard = mem.videoStandard();
        frameLines = pom2VideoTiming(standard).scanlinesPerFrame;
        beamView = mem.beamView();
        beamComposite = mem.beamViewActive();
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
    {
        ImVec4 colour = ImGui::ColorConvertU32ToFloat4(beamColour_);
        if (ImGui::ColorEdit3("##beamcolour", &colour.x,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            setBeamColour(ImGui::ColorConvertFloat4ToU32(colour));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The colour of the beam's cross (it keeps a dark edge, so it shows\n"
                              "on any picture). The cross is drawn while the beam view is on,\n"
                              "and whenever the machine is stopped.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Fit", &fit_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Size the picture to the panel, like the Apple II Screen.\nUntick to choose the zoom.");
    ImGui::SameLine();
    ImGui::BeginDisabled(fit_);
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Zoom", &zoom_, 1.0f, 6.0f, "x%.1f");
    ImGui::EndDisabled();
    // The two saves stay on the row while they fit (half a window is common).
    const auto next = [](const char* label) {
        ImGui::SameLine();
        const float wide = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetCursorPosX() + wide > ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x)
            ImGui::NewLine();
    };
    // Run until the beam is somewhere: the next line, the top of the next
    // frame, the start of vertical blanking. A right click on the picture
    // runs to that point.
    next("Next line");
    if (ImGui::Button("Next line")) ctrl.debugRunToBeam((beam.line + 1) % frameLines, 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run until the beam starts the next line (H 0), then stop.\n"
                          "Right-click the picture to run to a point of it.");
    next("Next frame");
    if (ImGui::Button("Next frame")) ctrl.debugRunToBeam(0, 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run until the beam is back at the top (V 0, H 0), then stop.");
    next("VBL");
    if (ImGui::Button("VBL")) ctrl.debugRunToBeam(192, 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run until vertical blanking starts (V 192, H 0), then stop.");
    next("Switches");
    ImGui::Checkbox("Switches", &showSwitches_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("List every video soft switch the program threw during the frame,\n"
                          "with the beam position (V, H) of each and the instruction that\n"
                          "did it. Click a row: the Debugger's listing goes to it.");
    next("Save PNG");
    if (ImGui::Button("Save PNG") && savePng) savePng();
    next("Save pages as fetched");
    ImGui::BeginDisabled(!applePicture);
    if (ImGui::Button("Save pages as fetched")) {
        auto pages = std::make_unique<Memory::FetchedPages>();
        {
            auto st = ctrl.lockState();
            st.memory().captureFetchedPages(*pages);
        }
        // File work with the lock released (CLAUDE.md: never across I/O).
        const std::filesystem::path dir = userDataDir() / "screenshots";
        const std::string base = "SCREEN-" + timeStamp();
        std::string error;
        const auto written = saveFetchedPages(*pages, dir, base, error);
        status_ = !error.empty() ? "Save failed: " + error
                                 : "Saved " + std::to_string(written.size()) + " page(s) as " +
                                   base + "-* in " + dir.string();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The video pages built from what the beam read: every byte from the\n"
                          "page its screen position displayed, with what RAM held at that moment.\n"
                          "One file per page the frame used: hi-res (8 KB), text / lo-res (1 KB),\n"
                          "and their AUX halves for 80 columns and the double modes.\n"
                          "They open in the Paint editor and in any Apple II tool.");
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    // The readout sits ABOVE the picture so it stays visible whatever the
    // window's height; it describes the picture drawn below it, so its lines
    // are reserved here and filled once the picture's geometry is known.
    const ImVec2 readoutAt = ImGui::GetCursorPos();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 5.0f));

    // ── The picture, with the mode gutter on its left ────────────────────
    // 280 x 192 Apple pixels at zoom 1, whatever the framebuffer's width.
    const float gutterW = 30.0f, bandH = 30.0f;
    const float switchesH = showSwitches_ && applePicture
        ? ImGui::GetTextLineHeightWithSpacing() * 8.0f + 8.0f : 0.0f;
    ImGui::BeginChild("##picture", ImVec2(0, switchesH > 0.0f ? -switchesH : 0.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (fit_) {
        // The largest picture the panel holds, edges included.
        const ImVec2 room = ImGui::GetContentRegionAvail();
        zoom_ = std::clamp(std::min((room.x - gutterW) / 280.0f, (room.y - bandH) / 192.0f), 1.0f, 12.0f);
    }
    const ImVec2 size(280.0f * zoom_, 192.0f * zoom_);
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const ImVec2 origin(corner.x, corner.y + bandH);
    const ImVec2 at(origin.x + gutterW, origin.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (applePicture) {
        // The band on top is ONE line read byte by byte — the line under the
        // mouse, kept when the mouse leaves: mode, page, and a white mark on
        // each byte where a switch is thrown. Under it, dimmer, the bytes
        // where ANY line of the frame is split.
        // Until the mouse has chosen a line, read the first split one: that
        // is the line a newcomer to the panel wants explained.
        if (!bandChosen_) {
            bandLine_ = 0;
            for (int y = 0; y < 192; ++y)
                if (frame.splitInside(y)) { bandLine_ = y; break; }
        }
        const float byteW = size.x / 40.0f;
        const std::array<bool, 40> here = frame.splitColumns(bandLine_);
        const std::array<bool, 40> anywhere = frame.splitColumns(-1);
        for (int c = 0; c < 40; ++c) {
            const Memory::DisplayState s = frame.at(bandLine_, c);
            const ScreenProbe p = probeAt(s, bandLine_, (static_cast<double>(c) + 0.5) / 40.0);
            const float x0 = at.x + byteW * static_cast<float>(c);
            draw->AddRectFilled(ImVec2(x0, corner.y), ImVec2(x0 + byteW - 1.0f, corner.y + 10.0f), modeColour(p.mode));
            draw->AddRectFilled(ImVec2(x0, corner.y + 11.0f), ImVec2(x0 + byteW - 1.0f, corner.y + 18.0f),
                                p.page2 ? kPage2Colour : kPage1Colour);
            if (here[static_cast<std::size_t>(c)])
                draw->AddRectFilled(ImVec2(x0 - 1.0f, corner.y), ImVec2(x0 + 2.0f, corner.y + 24.0f), kSplitColour);
            else if (anywhere[static_cast<std::size_t>(c)])
                draw->AddRectFilled(ImVec2(x0 - 1.0f, corner.y + 20.0f), ImVec2(x0 + 2.0f, corner.y + 24.0f),
                                    IM_COL32(150, 150, 150, 255));
        }
        // Which line the band reads: a tick in the gutter's corner side.
        const float ly = origin.y + size.y * (static_cast<float>(bandLine_) + 0.5f) / 192.0f;
        draw->AddTriangleFilled(ImVec2(at.x - 1.0f, ly), ImVec2(at.x - 5.0f, ly - 4.0f),
                                ImVec2(at.x - 5.0f, ly + 4.0f), IM_COL32(255, 255, 0, 255));
    }
    if (applePicture) {
        // Three stripes per line: the mode, the page, and a white mark where
        // a switch is thrown INSIDE the line's 40 bytes.
        const float lineH = size.y / 192.0f;
        for (int y = 0; y < 192; ++y) {
            const Memory::DisplayState s = frame.at(y, 0);
            const ScreenProbe p = probeAt(s, y, 0.0);
            const float y0 = origin.y + lineH * static_cast<float>(y);
            draw->AddRectFilled(ImVec2(origin.x, y0), ImVec2(origin.x + 12.0f, y0 + lineH), modeColour(p.mode));
            draw->AddRectFilled(ImVec2(origin.x + 13.0f, y0), ImVec2(origin.x + 20.0f, y0 + lineH),
                                p.page2 ? kPage2Colour : kPage1Colour);
            if (frame.splitInside(y))
                draw->AddRectFilled(ImVec2(origin.x + 21.0f, y0), ImVec2(origin.x + 26.0f, y0 + lineH), kSplitColour);
        }
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::Image(static_cast<ImTextureID>(texture_), size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // Where the beam is. With the beam view on the picture IS the tube at
    // this cycle; with it off the picture is the last whole frame, and the
    // cross still says where the stopped machine stands in the next one.
    if (applePicture && (beamView || !running))
        drawBeamMarker(draw, at, size, beam, frameLines, beamColour_);
    // Right click: run until the beam gets to that byte.
    if (applePicture && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && size.x > 0) {
        runLine_   = std::clamp(static_cast<int>((mouse.y - at.y) / size.y * 192.0f), 0, 191);
        runColumn_ = std::clamp(static_cast<int>((mouse.x - at.x) / size.x * 40.0f), 0, 39);
        ImGui::OpenPopup("##runtobeam");
    }
    if (ImGui::BeginPopup("##runtobeam")) {
        char item[80];
        std::snprintf(item, sizeof item, "Run until the beam is here  (V %d, H %d)",
                      runLine_, runColumn_ + kBeamHblCycles);
        if (ImGui::MenuItem(item)) ctrl.debugRunToBeam(runLine_, runColumn_ + kBeamHblCycles);
        std::snprintf(item, sizeof item, "Run to the first byte of line %d  (H %d)", runLine_, kBeamHblCycles);
        if (ImGui::MenuItem(item)) ctrl.debugRunToBeam(runLine_, kBeamHblCycles);
        std::snprintf(item, sizeof item, "Run to the start of line %d  (H 0, in blanking)", runLine_);
        if (ImGui::MenuItem(item)) ctrl.debugRunToBeam(runLine_, 0);
        ImGui::Separator();
        ImGui::TextDisabled("Stops at the first instruction that starts at or\nafter that cycle: up to one instruction past it.");
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    const ImVec2 afterPicture = ImGui::GetCursorPos();
    ImGui::SetCursorPos(readoutAt);

    // ── Readout ──────────────────────────────────────────────────────────
    {
        ScopedMonoFont mono;
        if (beam.vbl)      ImGui::Text("Beam    V %3d  H %2d   vertical blanking", beam.line, beam.h);
        else if (beam.hbl) ImGui::Text("Beam    V %3d  H %2d   horizontal blanking", beam.line, beam.h);
        else               ImGui::Text("Beam    V %3d  H %2d   on byte %d", beam.line, beam.h, beam.column);
        if (!applePicture) {
            ImGui::TextDisabled("A card's own picture is on screen: nothing to probe.");
            ImGui::TextUnformatted("");
        } else if (hovered && size.x > 0 && size.y > 0) {
            const double fx = std::clamp(static_cast<double>((mouse.x - at.x) / size.x), 0.0, 0.999999);
            const int y = std::clamp(static_cast<int>((mouse.y - at.y) / size.y * 192.0f), 0, 191);
            bandLine_ = y;      // the band on top reads this line from now on
            bandChosen_ = true;
            // The switches of THAT byte: a line can change mode or page inside.
            const Memory::DisplayState s = frame.at(y, static_cast<int>(fx * 40.0));
            const ScreenProbe p = probeAt(s, y, fx);
            uint8_t value = 0;
            {
                auto st = ctrl.lockState();
                value = st.memory().fetchedVideoByte(y, p.address, p.aux);
            }
            ImGui::Text("Pixel   X %3d of 0-%d   Y %3d   %s, page %d%s",
                        p.x, p.xMax, y, p.mode, p.page2 ? 2 : 1,
                        s.mixedMode && !s.textMode ? " (MIXED on)" : "");
            char what[48];
            if (p.bit >= 0)
                std::snprintf(what, sizeof what, "byte %d of the line, bit %d", p.byteColumn, p.bit);
            else
                std::snprintf(what, sizeof what, "row %d, column %d", p.row, p.byteColumn);
            const char printable = static_cast<char>(value & 0x7F);
            if (p.bit < 0 && std::strncmp(p.mode, "TEXT", 4) == 0 && printable >= 0x20 && printable < 0x7F)
                ImGui::Text("Memory  $%04X%s = $%02X '%c'   %s", p.address, p.aux ? " (aux)" : "",
                            value, printable, what);
            else
                ImGui::Text("Memory  $%04X%s = $%02X   %s", p.address, p.aux ? " (aux)" : "", value, what);
            // Outline the byte under the mouse.
            const float bx = at.x + size.x * static_cast<float>(p.byteColumn) / 40.0f;
            const float by = at.y + size.y * static_cast<float>(y) / 192.0f;
            ImGui::GetForegroundDrawList()->AddRect(
                ImVec2(bx, by), ImVec2(bx + size.x / 40.0f, by + size.y / 192.0f),
                IM_COL32(255, 255, 0, 200));
        } else {
            ImGui::TextDisabled("Pixel   move the mouse over the picture");
            ImGui::TextDisabled("Memory  the byte the beam read there, and its address");
        }
    }
    // Legend of the two edges, on two short lines so it fits a half window.
    const char* const edges =
        "Left edge: every line - its mode, its page, and a white mark when a\n"
        "switch is thrown inside the line.\n"
        "Top edge: ONE line read byte by byte - the line last under the mouse.";
    for (const ModeColour& m : kModeColours) legendSwatch(m.colour, m.mode);
    ImGui::NewLine();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", edges);
    legendSwatch(kPage1Colour, "page 1");
    legendSwatch(kPage2Colour, "page 2");
    legendSwatch(kSplitColour, "switch inside a line");
    ImGui::TextDisabled("  top edge: line %d", bandLine_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", edges);
    ImGui::SetCursorPos(afterPicture);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));   // an item, so the cursor move is legal

    // ── The frame's soft-switch throws ───────────────────────────────────
    // What the gutter and the band draw, as a list a program can be checked
    // against: each throw with the V and H of the access that made it, the
    // byte of the line it takes effect from — the PAINTER's own figure,
    // which differs by switch (Apple2Display::eventByteColumn) — and the
    // instruction that made it.
    if (switchesH > 0.0f) {
        struct Thrown { const Memory::VideoEvent* event; int from; };
        std::vector<Thrown> thrown;
        {
            // 80COL as it stood at each event decides AN3's column.
            Memory::DisplayState state = frame.start;
            for (const Memory::VideoEvent& e : frame.events) {
                // Not the display's band cuts, and not the beam view's seam
                // between its two frames: neither is a switch a program threw.
                if (switchText(e.kind, e.value) && e.pc != Memory::VideoEvent::kComposite)
                    thrown.push_back({&e, Apple2Display::eventByteColumn(e, standard, state.eightyCol)});
                Memory::foldVideoEvent(state, e.kind, e.value);
            }
        }
        // The bytes of each throwing instruction, as memory holds them NOW
        // (a program that rewrites itself may have moved on since).
        Cpu6502Set isa = Cpu6502Set::Nmos6502;
        if (!thrown.empty()) {
            if (codeScratch_.size() != 0x10000) codeScratch_.assign(0x10000, 0);
            auto st = ctrl.lockState();
            isa = st.cpu().instructionSet();
            for (const Thrown& t : thrown) {
                if (t.event->pc < 0) continue;
                for (int b = 0; b < 3; ++b) {
                    const uint16_t a = static_cast<uint16_t>(t.event->pc + b);
                    codeScratch_[a] = st.memory().peekCpuView(a);
                }
            }
        }
        ScopedMonoFont mono;
        bool openMenu = false;
        ImGui::BeginChild("##switches", ImVec2(0, 0), true);
        ImGui::TextDisabled("%s", beamComposite
            ? "Above the beam: the frame in progress. Below it: the previous frame. (No vertical-blanking throws.)"
            : running ? "The last complete frame."
                      : "The last COMPLETE frame - tick Beam view for the one in progress.");
        if (thrown.empty()) {
            ImGui::TextDisabled("No video soft switch was thrown.");
        } else if (ImGui::BeginTable("##switchlist", 5, ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("V");
            ImGui::TableSetupColumn("H");
            ImGui::TableSetupColumn("from byte");
            ImGui::TableSetupColumn("switch");
            ImGui::TableSetupColumn("thrown by", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(thrown.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                    const Memory::VideoEvent& e = *thrown[static_cast<std::size_t>(i)].event;
                    const int from = thrown[static_cast<std::size_t>(i)].from;
                    const int eh = static_cast<int>(e.emuCycle % 65);
                    const bool visible = e.scanline < 192;
                    const bool known = e.pc >= 0;
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    char v[16];
                    if (visible) std::snprintf(v, sizeof v, "%3d", e.scanline);
                    else         std::snprintf(v, sizeof v, "VBL");
                    if (ImGui::Selectable(v, visible && e.scanline == bandLine_,
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        if (visible) { bandLine_ = e.scanline; bandChosen_ = true; }
                        if (known && showInListing) showInListing(static_cast<uint16_t>(e.pc));
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        menuPc_ = known ? e.pc : -1;
                        menuLine_ = visible ? e.scanline : -1;
                        menuH_ = eh;
                        openMenu = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%2d", eh);
                    ImGui::TableNextColumn();
                    if (!visible)        ImGui::TextDisabled("-");
                    else if (from <= 0)  ImGui::TextUnformatted("whole line");
                    else if (from >= 40) ImGui::TextUnformatted("next line");
                    else                 ImGui::Text("%d", from);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(switchText(e.kind, e.value));
                    ImGui::TableNextColumn();
                    if (!known) {
                        ImGui::TextDisabled("-");
                    } else {
                        int len = 1;
                        const std::string text = disassemble6502(codeScratch_.data(),
                            static_cast<uint16_t>(e.pc), len, isa);
                        ImGui::Text("$%04X  %s", e.pc, text.c_str());
                    }
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
        // One menu for the list, on the throw the click chose.
        if (openMenu) ImGui::OpenPopup("##switchmenu");
        if (ImGui::BeginPopup("##switchmenu")) {
            char item[64];
            std::snprintf(item, sizeof item, "Show $%04X in the Debugger's listing",
                          menuPc_ >= 0 ? menuPc_ : 0);
            if (ImGui::MenuItem(item, nullptr, false, menuPc_ >= 0 && showInListing))
                showInListing(static_cast<uint16_t>(menuPc_));
            std::snprintf(item, sizeof item, "Run until the beam is there  (V %d, H %d)",
                          menuLine_ < 0 ? 0 : menuLine_, menuH_);
            if (ImGui::MenuItem(item, nullptr, false, menuLine_ >= 0))
                ctrl.debugRunToBeam(menuLine_, menuH_);
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace pom2
