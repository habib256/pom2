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

// paint_ui_probe — the HGR / DHGR Paint editor WINDOW, drawn off screen at
// several window sizes and UI scales, saved as PNGs, and checked for
// clipping.
//
// A real EmulationController and POM2's own host (Pom2HgrPaintHost: the
// canvas goes through the same scratch display as the app) feed the editor
// a picture on HGR page 1, a text screen and a few pages of colour. For each
// window size (1500x900, 1180x760, 960x640, 800x560) and each UI scale (1.0,
// 1.5 — Pom2Theme's zoom, fonts AND geometry) it draws the editor in every
// state that changes the layout: the Pencil, Select (with a clip), Text and
// Palette-shift tools, DHGR with flipbook + ghost + onion skin, GR with its
// 16-colour bar, the page monitor on TEXT, a long status line, and the two
// modals (file browser, import preview).
//
// After each frame it walks the editor's ImGui windows and reports any whose
// content is wider or taller than what it shows: "CUT" when there is no
// scroll bar to reach the rest (the content is clipped), "scroll" when there
// is one. The canvas itself may scroll (that is its zoom). The run fails if
// anything is CUT, or if a window outside the canvas needs a horizontal bar.
//
// Not a ctest test: it needs a GL context, and its PNGs are for eyes.
// `cmake --build build --target paint_ui_probe`, then look in
// $POM2_PROBE_OUT (or $TMPDIR/pom2_probes)/paint_ui/ — or pass --out DIR.

#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "PinchZoom_ImGui.h"
#include "Pom2HgrPaintHost.h"
#include "Pom2Theme.h"
#include "ProbeOutDir.h"
#include "UiFonts.h"
#include "hgrpaint/HgrPaintEditor.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>
#include "Pom2GL.h"

#include "stb_image_write.h"   // implementation: Pom2HgrPaintHost.cpp

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hgrpaint {

// The editor's private state, set directly (the friend declared in
// HgrPaintEditor.h): a probe that clicked its way to each state would have
// to know where every widget is, which is the very thing it measures.
struct HgrPaintEditorTestAccess {
    using E = HgrPaintEditor;
    static void tool(E& e, int t) { e.tool = static_cast<E::Tool>(t); }
    static void page(E& e, int mode, bool p2) { e.switchPage(mode, p2); }
    static void clip(E& e)
    {
        e.clip.w = 24; e.clip.h = 16; e.clip.sixteen = false; e.clip.blockMode = false;
        e.clip.px.assign(24 * 16, HgrColor::White);
        e.hasSel = true; e.selX0 = 10; e.selY0 = 10; e.selX1 = 34; e.selY1 = 26;
    }
    static void text(E& e) { e.textPlaced = true; e.textX = 40; e.textY = 60; std::strcpy(e.textBuf, "HELLO"); }
    static void anim(E& e, void* onion)
    {
        e.flipShow = false; e.ghostOther = true;
        e.onionTex = onion; e.onionShow = true;
    }
    static void flip(E& e) { e.flipShow = true; }
    static void monitor(E& e, bool on, int mode, int follow)
    {
        e.monitor = on; e.monMode = mode; e.monFollow = follow;
    }
    static void zoom(E& e, int idx) { e.zoomIdx = idx; }
    static void fit(E& e) { e.wantFit = true; }
    static void status(E& e, const std::string& s) { e.status = s; }
    static void browser(E& e, const std::string& dir, bool save)
    {
        e.browserDir = dir; e.browserForSave = save; e.browserImport = false;
        e.browserSaveKind = 0; std::strcpy(e.browserSaveName, "PICTURE#062000");
        e.browserOpen = true;
    }
    static void importPreview(E& e, const std::string& path) { e.openImportPreview(path); }
    static void dhgrModel(E& e, int m) { e.importDhgrModel = m; }
    static void reset(E& e)
    {
        e.flipShow = e.ghostOther = e.onionShow = false;
        e.onionTex = nullptr;   // the probe owns that texture
        e.clip = {}; e.hasSel = false; e.textPlaced = false; e.status.clear();
        e.monitor = false;
    }
};

} // namespace hgrpaint

namespace {

using TA = hgrpaint::HgrPaintEditorTestAccess;
enum ToolIdx { Pencil = 0, Eraser, Line, Rect, Ellipse, Fill, Eyedrop, Select, Palette, Text };

constexpr int kMaxW = 1500, kMaxH = 900;

struct Overflow { std::string window; float overX, overY; bool sbX, sbY; };

// Every window of the editor whose content does not fit what it shows.
std::vector<Overflow> overflows()
{
    std::vector<Overflow> out;
    ImGuiContext& g = *GImGui;
    for (ImGuiWindow* w : g.Windows) {
        if (!w->Active || w->Hidden) continue;
        const std::string name = w->Name;
        const bool popup = name.rfind("##Popup_", 0) == 0;
        const bool ours = popup || name.rfind("HGR Paint Editor", 0) == 0 ||
                          name.rfind("Import preview##hgr", 0) == 0 ||
                          name.rfind("HGR File##browser", 0) == 0;
        if (!ours) continue;
        // A window (a menu, a dialog) that hangs off the screen is cut too.
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        if (w->Pos.x < -0.5f || w->Pos.y < -0.5f ||
            w->Pos.x + w->Size.x > d.x + 0.5f || w->Pos.y + w->Size.y > d.y + 0.5f)
            out.push_back({name + " (off screen)", std::max(0.0f, w->Pos.x + w->Size.x - d.x),
                           std::max(0.0f, w->Pos.y + w->Size.y - d.y), false, false});
        // The canvas child scrolls by design (zoom).
        if (name.find("/hgrcanvas_") != std::string::npos) continue;
        const float visW = w->InnerRect.GetWidth() - 2.0f * w->WindowPadding.x;
        const float visH = w->InnerRect.GetHeight() - 2.0f * w->WindowPadding.y;
        const float ox = w->ContentSize.x - visW, oy = w->ContentSize.y - visH;
        if (ox > 1.5f || oy > 1.5f) out.push_back({name, ox, oy, w->ScrollbarX, w->ScrollbarY});
    }
    return out;
}

void writeTestPng(const std::string& path, int w, int h)
{
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            const int dx = x - w / 2, dy = y - h / 2;
            const bool disc = dx * dx + dy * dy < (h / 3) * (h / 3);
            p[0] = static_cast<uint8_t>(disc ? 240 : x * 255 / w);
            p[1] = static_cast<uint8_t>(disc ? 200 : y * 255 / h);
            p[2] = static_cast<uint8_t>(disc ? 40 : 160);
            p[3] = 255;
        }
    stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4);
}

} // namespace

int main(int argc, char** argv)
{
    const std::string out = pom2test::probeOutDir(argc > 2 && std::string(argv[1]) == "--out" ? argv[2] : "") +
                            "/paint_ui";
    std::filesystem::create_directories(out);
    // A folder for the file browser: a picture, a page dump, a long name.
    const std::string files = out + "/files";
    std::filesystem::create_directories(files);
    writeTestPng(files + "/sunset.png", 320, 200);
    {
        std::ofstream(files + "/PICTURE#062000", std::ios::binary) << std::string(8192, '\0');
        std::ofstream(files + "/A_RATHER_LONG_FILE_NAME_FOR_A_DOUBLE_HIRES_PAGE#062000.A2FC",
                      std::ios::binary) << std::string(16384, '\0');
        std::filesystem::create_directories(files + "/subfolder");
    }

    if (!glfwInit()) { std::printf("SKIP: no GLFW\n"); return 77; }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    GLFWwindow* win = glfwCreateWindow(kMaxW, kMaxH, "paint_ui_probe", nullptr, nullptr);
    if (!win) { std::printf("SKIP: no GL context\n"); glfwTerminate(); return 77; }
    glfwMakeContextCurrent(win);

    GLuint fbo = 0, tex = 0;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kMaxW, kMaxH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigMacOSXBehaviors = false;
    io.DisplayFramebufferScale = ImVec2(1, 1);
    pom2::applyTheme(pom2::UiAccent::Amber, 1.0f, 1.0f);
    pom2::setMonoFont(pom2::loadUiFonts(io));
    ImGui_ImplOpenGL3_Init("#version 150");

    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        Memory& mem = st.memory();
        mem.setIIEMode(true);
        // HGR1: a disc in two colours over stripes; HGR2: bars; the text
        // pages: a line of text; aux: a DHGR pattern.
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 40; ++x) {
                const int dx = x * 7 - 140, dy = y - 96;
                const bool disc = dx * dx + dy * dy < 70 * 70;
                const uint8_t v = disc ? static_cast<uint8_t>(((x + y / 8) & 1) ? 0xD5 : 0xAA)
                                       : static_cast<uint8_t>((y & 8) ? 0x2A : 0x00);
                mem.memWrite(static_cast<uint16_t>(0x2000 + Memory::hiResLineOffset(y) + x), v);
                mem.memWrite(static_cast<uint16_t>(0x4000 + Memory::hiResLineOffset(y) + x),
                             static_cast<uint8_t>((x / 5) & 1 ? 0x7F : 0x55));
            }
        const char* text = "POM2 PAINT EDITOR LAYOUT PROBE - TEXT PAGE ";
        for (uint32_t a = 0x0400; a < 0x0C00; ++a)
            mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(text[a % 43] | 0x80));
        st.cpu().setProgramCounter(0x0300);
        mem.memWrite(0x0300, 0x4C); mem.memWrite(0x0301, 0x00); mem.memWrite(0x0302, 0x03);
    }

    Pom2HgrPaintHost host(&ctrl);
    hgrpaint::HgrPaintEditor ed(&host);
    // A stand-in for the onion-skin source (any texture: only its presence
    // changes the layout).
    std::vector<uint32_t> onionPx(64 * 64, 0xFF3366CCu);
    void* onionTex = host.uploadTexture(nullptr, onionPx.data(), 64, 64, true);

    std::vector<uint8_t> memSnap, auxSnap;
    int failures = 0, frames = 0;
    std::vector<std::string> cutLines;

    auto snapshot = [&] {
        auto st = ctrl.lockState();
        const Memory& mem = st.memory();
        memSnap.assign(mem.data(), mem.data() + 0x10000);
        auxSnap.assign(mem.videoAuxData(), mem.videoAuxData() + 0x10000);
    };

    bool closeAll = false, openView = false;
    auto frame = [&](int W, int H, const char* shot, const std::string& tag) {
        snapshot();
        io.DisplaySize = ImVec2(static_cast<float>(W), static_cast<float>(H));
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(-1, -1);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        pom2::pinchZoomNewFrame();
        if (closeAll) { closeAll = false; if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false); }
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(W), static_cast<float>(H)), ImGuiCond_Always);
        bool open = true;
        if (ImGui::Begin("HGR Paint Editor", &open)) {
            // The toolbar's View menu, opened as its button does (same ID scope).
            if (openView) { openView = false; ImGui::OpenPopup("##paintview"); }
            ed.render(memSnap, &auxSnap);
        }
        ImGui::End();
        ImGui::Render();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, W, H);
        glClearColor(0.08f, 0.08f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ++frames;
        if (!shot) return;
        for (const Overflow& o : overflows()) {
            const bool cut = (o.overX > 1.5f && !o.sbX) || (o.overY > 1.5f && !o.sbY);
            const bool hscroll = o.overX > 1.5f && o.sbX;
            char line[512];
            std::snprintf(line, sizeof line, "  %s %-24s %-44s over x %+6.0f y %+6.0f%s%s",
                          cut ? "CUT   " : hscroll ? "HSCRL " : "scroll", tag.c_str(), o.window.c_str(),
                          o.overX, o.overY, o.sbX ? " [x bar]" : "", o.sbY ? " [y bar]" : "");
            std::printf("%s\n", line);
            if (cut || hscroll) { ++failures; cutLines.push_back(line); }
        }
        std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::vector<uint8_t> flipped(px.size());
        for (int y = 0; y < H; ++y)
            std::copy_n(&px[static_cast<size_t>(H - 1 - y) * W * 4], W * 4,
                        &flipped[static_cast<size_t>(y) * W * 4]);
        const std::string path = out + "/" + shot + ".png";
        stbi_write_png(path.c_str(), W, H, 4, flipped.data(), W * 4);
    };
    // A state takes a few frames to settle (fit, popups opening, auto-resize).
    auto settle = [&](int W, int H, const std::string& name) {
        for (int i = 0; i < 4; ++i) frame(W, H, nullptr, name);
        frame(W, H, name.c_str(), name);
    };
    auto closePopups = [&](int W, int H) {
        closeAll = true;
        frame(W, H, nullptr, "");
        frame(W, H, nullptr, "");
    };

    struct Size { int w, h; };
    const Size sizes[] = { {1500, 900}, {1180, 760}, {960, 640}, {800, 560} };
    const float scales[] = { 1.0f, 1.5f };

    for (float sc : scales) {
        pom2::applyTheme(pom2::UiAccent::Amber, sc, 1.0f);
        for (const Size& s : sizes) {
            const int W = s.w, H = s.h;
            char pre[64];
            std::snprintf(pre, sizeof pre, "%dx%d_s%02d_", W, H, static_cast<int>(sc * 10 + 0.5f));
            auto name = [&](const char* what) { return std::string(pre) + what; };

            TA::reset(ed);
            TA::page(ed, 0, false);
            TA::tool(ed, Pencil);
            TA::fit(ed);
            settle(W, H, name("a_hgr_pencil"));

            TA::tool(ed, Select);
            TA::clip(ed);
            TA::status(ed, "Saved /Users/someone/Documents/Apple II/pictures/A_RATHER_LONG_NAME#062000");
            settle(W, H, name("b_select_clip_status"));

            TA::reset(ed);
            TA::tool(ed, Text);
            TA::text(ed);
            TA::zoom(ed, 4);       // zoomed past the fit: the navigator shows
            settle(W, H, name("c_text_zoomed"));

            TA::tool(ed, Palette);
            TA::fit(ed);
            settle(W, H, name("d_palette_shift"));

            TA::reset(ed);
            TA::page(ed, 2, false);
            TA::tool(ed, Rect);
            TA::anim(ed, onionTex);
            TA::fit(ed);
            settle(W, H, name("e_dhgr_rect_ghost_onion"));
            TA::flip(ed);
            settle(W, H, name("e2_dhgr_flipbook"));
            openView = true;
            settle(W, H, name("e3_view_menu"));
            closePopups(W, H);

            TA::reset(ed);
            TA::page(ed, 1, true);
            TA::tool(ed, Fill);
            TA::fit(ed);
            settle(W, H, name("f_gr2_fill"));

            TA::reset(ed);
            TA::monitor(ed, true, 4, 0);
            settle(W, H, name("g_monitor_text"));
            TA::monitor(ed, true, 0, 1);
            settle(W, H, name("g2_monitor_follow"));

            TA::reset(ed);
            TA::page(ed, 0, false);
            TA::browser(ed, files, true);
            settle(W, H, name("h_browser_save"));
            closePopups(W, H);

            TA::page(ed, 2, false);
            TA::dhgrModel(ed, 3);
            TA::importPreview(ed, files + "/sunset.png");
            settle(W, H, name("i_import_dhgr"));
            closePopups(W, H);
            TA::page(ed, 0, false);
        }
    }

    host.destroyTexture(onionTex);
    ed.releaseGL();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    std::printf("\npaint_ui_probe: %d frames, PNGs in %s\n", frames, out.c_str());
    if (!cutLines.empty()) {
        std::printf("clipped or horizontally scrolled (%zu):\n", cutLines.size());
        for (const std::string& l : cutLines) std::printf("%s\n", l.c_str());
    }
    std::printf("paint_ui_probe: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
