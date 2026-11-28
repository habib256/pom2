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

// heatmap_ui_probe — the Memory Heatmap WINDOW, driven by a mouse of its own,
// drawn off screen and saved frame by frame as PNGs (UI-022).
//
// A real EmulationController runs a //e program that clears HGR page 1 and
// writes a counter into the language card's $D000 bank 1; the probe hovers a
// byte (crosshair + loupe), zooms on it (values in the cells), clicks it (the
// inspector), shift-drags a range, opens the right-click menu, pinches with
// two fingers (the portable event a touchpad / touch screen source pushes,
// PinchZoom.h), and checks what the window says it selected. Not a ctest test: it needs a GL context
// and its output is for eyes. `make heatmap_ui_probe`, then look in
// $POM2_PROBE_OUT (or $TMPDIR/pom2_probes)/heatmap_ui/.

#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryHeatmap_ImGui.h"
#include "PinchZoom_ImGui.h"
#include "Pom2Theme.h"
#include "ProbeOutDir.h"
#include "UiFonts.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>
#include "Pom2GL.h"

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kW = 1500, kH = 860;

// $0300: clear HGR1, then loop: time a stretch with French Touch's markers
// ($C067 ... $C066, ~12 x 1285 cycles) around a bump of $D000 (bank 1,
// RAM) and $10, then a 3-NOP "handler" between $C069 and $C068, forever.
//   0300 AD 8B C0  LDA $C08B        ; LC bank 1, read/write RAM (twice)
//   0303 AD 8B C0  LDA $C08B
//   0306 A9 20     LDA #$20
//   0308 85 07     STA $07
//   030A A9 00     LDA #$00
//   030C 85 06     STA $06
//   030E A2 20     LDX #$20
//   0310 A0 00     LDY #$00
//   0312 91 06     STA ($06),Y      ; CLEAR_LOOP
//   0314 C8        INY
//   0315 D0 FB     BNE $0312
//   0317 E6 07     INC $07
//   0319 CA        DEX
//   031A D0 F6     BNE $0312
//   031C 8D 67 C0  STA $C067        ; MAIN_LOOP: start the section
//   031F EE 00 D0  INC $D000
//   0322 E6 10     INC $10
//   0324 A0 0C     LDY #$0C
//   0326 A2 00     LDX #$00
//   0328 CA        DEX
//   0329 D0 FD     BNE $0328
//   032B 88        DEY
//   032C D0 F8     BNE $0326
//   032E 8D 66 C0  STA $C066        ; stop it
//   0331 8D 69 C0  STA $C069        ; the "IRQ" span
//   0334 EA EA EA  NOP x 3
//   0337 8D 68 C0  STA $C068
//   033A AD 30 C0  LDA $C030
//   033D 4C 1C 03  JMP $031C
const uint8_t kProgram[] = {
    0xAD, 0x8B, 0xC0, 0xAD, 0x8B, 0xC0, 0xA9, 0x20, 0x85, 0x07, 0xA9, 0x00,
    0x85, 0x06, 0xA2, 0x20, 0xA0, 0x00, 0x91, 0x06, 0xC8, 0xD0, 0xFB, 0xE6,
    0x07, 0xCA, 0xD0, 0xF6, 0x8D, 0x67, 0xC0, 0xEE, 0x00, 0xD0, 0xE6, 0x10,
    0xA0, 0x0C, 0xA2, 0x00, 0xCA, 0xD0, 0xFD, 0x88, 0xD0, 0xF8, 0x8D, 0x66,
    0xC0, 0x8D, 0x69, 0xC0, 0xEA, 0xEA, 0xEA, 0x8D, 0x68, 0xC0, 0xAD, 0x30,
    0xC0, 0x4C, 0x1C, 0x03,
};

std::string probeLabel(uint16_t a, pom2::DebugBank)
{
    if (a == 0x0312) return "CLEAR_LOOP";
    if (a == 0x031C) return "MAIN_LOOP";
    if (a == 0x0010) return "frame_count";
    return {};
}

struct Mouse {
    float x = -1, y = -1;
    bool left = false, right = false, shift = false;
    float wheel = 0;
};

}  // namespace

int main(int argc, char** argv)
{
    const std::string out = pom2test::probeOutDir(argc > 2 && std::string(argv[1]) == "--out" ? argv[2] : "") +
                            "/heatmap_ui";
    std::filesystem::create_directories(out);

    if (!glfwInit()) { std::printf("SKIP: no GLFW\n"); return 77; }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    GLFWwindow* win = glfwCreateWindow(kW, kH, "heatmap_ui_probe", nullptr, nullptr);
    if (!win) { std::printf("SKIP: no GL context\n"); glfwTerminate(); return 77; }
    glfwMakeContextCurrent(win);

    GLuint fbo = 0, tex = 0;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // The probe presses Ctrl itself: no macOS Cmd <-> Ctrl swap.
    io.ConfigMacOSXBehaviors = false;
    io.DisplaySize = ImVec2(kW, kH);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    // The app's own look: its theme, and its fonts with the icons merged
    // (UiFonts.cpp, shared with main.cpp) — or the PNGs would lie.
    pom2::applyTheme(pom2::UiAccent::Amber, 1.0f, 1.0f);
    pom2::setMonoFont(pom2::loadUiFonts(io));
    ImGui_ImplOpenGL3_Init("#version 150");

    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        Memory& mem = st.memory();
        mem.setIIEMode(true);
        for (unsigned i = 0; i < sizeof kProgram; ++i)
            mem.memWrite(static_cast<uint16_t>(0x0300 + i), kProgram[i]);
        for (uint32_t a = 0x2000; a < 0x4000; ++a) mem.memWrite(static_cast<uint16_t>(a), 0xA5);
        // A machine with real contents, not only the 00 FF power-on fill,
        // so the bits-set background and the region lines are judged on
        // what a program leaves in memory: the //e ROM when it is there,
        // a text screen on page 2, a picture on HGR2, a code-like block at
        // $6000 (a fixed pseudo-random stream) and a zeroed buffer after it.
        if (mem.loadAppleIIRom("roms/apple2e.rom") <= 0) std::printf("  (no roms/apple2e.rom: ROM column empty)\n");
        const char* text = "MEMORY HEATMAP PROBE - FRENCH TOUCH MONITOR MARKS $C066-$C069 ";
        for (uint32_t a = 0x0800; a < 0x0C00; ++a)
            mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(text[a % 62] | 0x80));
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 40; ++x) {
                const int dx = x * 7 - 140, dy = y - 96;
                const bool disc = dx * dx + dy * dy < 80 * 80;
                const uint8_t v = disc ? static_cast<uint8_t>(((x + y / 8) & 1) ? 0xD5 : 0xAA) : uint8_t{0};
                mem.memWrite(static_cast<uint16_t>(0x4000 + Memory::hiResLineOffset(y) + x), v);
            }
        uint32_t lcg = 12345;
        for (uint32_t a = 0x6000; a < 0x8800; ++a) {
            lcg = lcg * 1103515245u + 12345u;
            mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(lcg >> 16));
        }
        for (uint32_t a = 0x8800; a < 0x9800; ++a) mem.memWrite(static_cast<uint16_t>(a), 0);
        st.cpu().setProgramCounter(0x0300);
    }

    pom2::MemoryHeatmap_ImGui heat;
    // Both are off by default; the probe drives them, so it turns them on
    // the way a user's saved choice would.
    heat.setOptionsText(heat.optionsText() + " loupe=1 inspector=1");
    bool open = true;
    Mouse mouse;
    int frame = 0, failures = 0;
    // The machine runs through the controller's own frame loop, as in the
    // app (tickFrame is one worker tick): its debugged loop is what reports
    // each instruction to the heatmap.
    auto step = [&](int frames) { for (int i = 0; i < frames; ++i) ctrl.tickFrame(); };
    auto draw = [&](const char* shot) {
        io.DeltaTime = 1.0f / 60.0f;
        io.AddKeyEvent(ImGuiMod_Shift, mouse.shift);
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, mouse.left);
        io.AddMouseButtonEvent(1, mouse.right);
        if (mouse.wheel != 0) { io.AddMouseWheelEvent(0, mouse.wheel); mouse.wheel = 0; }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        pom2::pinchZoomNewFrame();   // as main.cpp: Ctrl+wheel and pinches → the view under them
        // The whole frame (the window sizes itself on first use only).
        ImGui::SetWindowPos("Memory Heatmap", ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetWindowSize("Memory Heatmap", ImVec2(kW, kH), ImGuiCond_Always);
        heat.render(ctrl, &open, probeLabel, [](std::string_view t, pom2::DebugAddress& out) {
            if (t == "CLEAR_LOOP") { out = {0x0312, {}}; return true; }
            if (t == "MAIN_LOOP") { out = {0x031C, {}}; return true; }
            return false;
        });
        pom2::MemoryHeatmap_ImGui::Request r;
        while (heat.takeRequest(r)) std::printf("  request %d at %s\n", static_cast<int>(r.kind), r.at.text().c_str());
        ImGui::Render();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, kW, kH);
        glClearColor(0.08f, 0.08f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ++frame;
        if (!shot) return;
        std::vector<uint8_t> px(static_cast<std::size_t>(kW) * kH * 4);
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::vector<uint8_t> flipped(px.size());
        for (int y = 0; y < kH; ++y)
            std::copy_n(&px[static_cast<std::size_t>(kH - 1 - y) * kW * 4], kW * 4,
                        &flipped[static_cast<std::size_t>(y) * kW * 4]);
        const std::string path = out + "/" + shot + ".png";
        stbi_write_png(path.c_str(), kW, kH, 4, flipped.data(), kW * 4);
        std::printf("  wrote %s\n", path.c_str());
    };
    auto expect = [&](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    };

    // Warm up: the window attaches the recording, then the program runs.
    draw(nullptr);
    ctrl.setMode(EmulationController::Mode::Running);
    for (int i = 0; i < 12; ++i) { step(2); draw(nullptr); }
    {
        auto st = ctrl.lockState();
        std::printf("  PC $%04X  $2000=%02X  heat armed %d  cycles %llu\n", st.cpu().getProgramCounter(),
                    st.memory().data()[0x2000], st.memory().heatArmed() ? 1 : 0,
                    static_cast<unsigned long long>(st.memory().getCycleCounter()));
    }
    draw("1_overview");
    {   // What a frame of the open window costs the UI thread, and how long
        // it holds the machine's lock (its refresh copies under it).
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) draw(nullptr);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30;
        std::printf("  a frame of the window at the fit: %.2f ms (UI thread, GL included; "
                    "refresh + paint %.2f ms)\n", ms, heat.lastPaintMs());
    }

    // Hover $2A13 in MAIN: crosshair, loupe.
    float x = 0, y = 0;
    expect(heat.screenPointOf(pom2::HeatData::kMain, 0x2A13, x, y), "MAIN is drawn");
    mouse.x = x; mouse.y = y;
    draw(nullptr);
    draw("2_hover_loupe");

    // Zoom in on it (wheel), until the values show.
    for (int i = 0; i < 20; ++i) {
        heat.screenPointOf(pom2::HeatData::kMain, 0x2A13, x, y);
        mouse.x = x; mouse.y = y; mouse.wheel = 1.0f;
        draw(nullptr);
    }
    expect(heat.zoom() > 12.0f, "the wheel zooms");
    heat.screenPointOf(pom2::HeatData::kMain, 0x2A13, x, y);
    mouse.x = x; mouse.y = y;
    draw("3_zoomed");
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) draw(nullptr);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30;
        std::printf("  a frame of the window zoomed in: %.2f ms (refresh + paint %.2f ms)\n", ms,
                    heat.lastPaintMs());
    }

    // Click: select it, the inspector shows it.
    mouse.left = true;  draw(nullptr);
    mouse.left = false; draw(nullptr);
    int plane = -1;
    uint16_t phys = 0;
    expect(heat.selectedByte(plane, phys) && plane == pom2::HeatData::kMain && phys == 0x2A13,
           "a click selects the byte under the pointer");
    step(3);
    draw("4_selected");

    // Shift-drag four bytes to the right: a range.
    float x2 = 0, y2 = 0;
    heat.screenPointOf(pom2::HeatData::kMain, 0x2A17, x2, y2);
    mouse.shift = true; mouse.left = true; draw(nullptr);
    for (int i = 1; i <= 6; ++i) { mouse.x = x + (x2 - x) * i / 6.0f; draw(nullptr); }
    mouse.left = false; draw(nullptr);
    mouse.shift = false; draw(nullptr);
    uint16_t lo = 0, hi = 0;
    expect(heat.selectedRange(lo, hi) && lo == 0x2A13 && hi == 0x2A17, "shift-drag selects $2A13-$2A17");
    draw("5_range");

    // Right click: the menu, with the range's breakpoints.
    mouse.right = true; draw(nullptr);
    mouse.right = false; draw(nullptr);
    draw("6_menu");

    // Close the menu with a click on the toolbar's empty space.
    mouse.x = 760; mouse.y = 40;
    draw(nullptr);
    mouse.left = true; draw(nullptr);
    mouse.left = false; draw(nullptr);

    // Home: fit; the $D000 bank-1 counter is in MAIN's $C0 rows.
    const float zoomBeforeHome = heat.zoom();
    mouse.x = 5; mouse.y = kH - 5;
    draw(nullptr);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Home, true);  draw(nullptr);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Home, false); draw(nullptr);
    expect(heat.zoom() < zoomBeforeHome, "Home fits the map");
    expect(heat.screenPointOf(pom2::HeatData::kMain, 0xC000, x, y), "bank 1 of $D000 is drawn");
    mouse.x = x; mouse.y = y;
    for (int i = 0; i < 2; ++i) { step(2); draw(nullptr); }
    draw("7_lc_bank1");

    // Two fingers apart over $2A13, the pointer parked elsewhere (a touch
    // screen's focal point is between the fingers): zooms in about them, and
    // the byte stays under them. Then fingers together: back out.
    {
        const float zFit = heat.zoom();
        float fx = 0, fy = 0;
        heat.screenPointOf(pom2::HeatData::kMain, 0x2A13, fx, fy);
        mouse.x = 5; mouse.y = kH - 5;
        for (int i = 0; i < 6; ++i) {
            pom2::pinch::router().pushAt(pom2::pinch::ratioToLog(1.5), {fx, fy});
            draw(nullptr);
        }
        float ax = 0, ay = 0;
        heat.screenPointOf(pom2::HeatData::kMain, 0x2A13, ax, ay);
        expect(heat.zoom() > zFit * 4.0f, "a pinch (fingers apart) zooms in");
        expect(std::fabs(ax - fx) < heat.zoom() * 1.5f && std::fabs(ay - fy) < heat.zoom() * 1.5f,
               "the pinched byte stays under the fingers");
        draw("7b_pinched");
        const float zIn = heat.zoom();
        pom2::pinch::router().pushAt(pom2::pinch::ratioToLog(0.5), {fx, fy});
        draw(nullptr);
        expect(heat.zoom() < zIn, "fingers together zoom out");
        // A pinch over the toolbar is not the map's.
        const float zBefore = heat.zoom();
        pom2::pinch::router().pushAt(pom2::pinch::ratioToLog(2.0), {760.0f, 40.0f});
        draw(nullptr);
        expect(heat.zoom() == zBefore, "a pinch off the map leaves it alone");
        // Ctrl+wheel (Cmd+wheel on macOS: ImGui swaps the two, so the key
        // that reaches io.KeyCtrl is sent as Super there) is the same zoom,
        // ONCE: ×1.25 a notch, not also the map's own wheel step on top.
        const ImGuiKey ctrlMod = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        mouse.x = fx; mouse.y = fy;
        ImGui::GetIO().AddKeyEvent(ctrlMod, true); draw(nullptr);   // the key, then the wheel
        mouse.wheel = 1.0f; draw(nullptr);
        ImGui::GetIO().AddKeyEvent(ctrlMod, false); draw(nullptr);
        const float k = heat.zoom() / zBefore;
        std::printf("  Ctrl+wheel: x%.3f\n", k);
        expect(k > 1.2f && k < 1.3f, "Ctrl+wheel zooms like a pinch, once (x1.25)");
        // A tablet: the first finger presses the map (the browser's GLFW
        // makes it the left button), the second lands and the pinch begins.
        // Lifting must click nothing — the press is cancelled off every
        // widget — so the selection stays what it was.
        int selPlane = -1, nowPlane = -1;
        uint16_t selPhys = 0, nowPhys = 0;
        heat.selectedByte(selPlane, selPhys);
        float tx = 0, ty = 0;
        heat.screenPointOf(pom2::HeatData::kMain, 0x2A40, tx, ty);
        mouse.x = tx; mouse.y = ty; draw(nullptr);
        mouse.left = true; draw(nullptr);
        pom2::pinch::router().cancelPress();
        draw(nullptr);                    // the finger still down: POM2 lets go
        mouse.left = false;               // GLFW's release, later: already up
        for (int i = 0; i < 4; ++i) draw(nullptr);
        expect(heat.selectedByte(nowPlane, nowPhys) && nowPlane == selPlane && nowPhys == selPhys,
               "a pinch that began over a press does not end as a click");
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Home, true);  draw(nullptr);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Home, false); draw(nullptr);
    }

    // Go to a label (Ctrl+G, type, Enter): selected, zoomed to readable,
    // ringed; the labels on the map; the minimap.
    mouse.x = 5; mouse.y = kH - 5;
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_G, true);  draw(nullptr);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_G, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); draw(nullptr);
    draw(nullptr);
    ImGui::GetIO().AddInputCharactersUTF8("CLEAR_LOOP"); draw(nullptr);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);  draw(nullptr);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); draw(nullptr);
    for (int i = 0; i < 3; ++i) { step(1); draw(nullptr); }
    expect(heat.selectedByte(plane, phys) && plane == pom2::HeatData::kMain && phys == 0x0312,
           "Go to CLEAR_LOOP selects $0312");
    expect(heat.zoom() >= 20.0f, "Go to zooms until the values show");
    draw("8_goto_label");

    // The minimap: shown while zoomed in; a drag on it moves the view.
    float m0 = 0, m1 = 0, m2 = 0, m3 = 0;
    expect(heat.minimapRect(m0, m1, m2, m3), "the minimap is shown while zoomed in");
    float c0x = 0, c0y = 0, c1x = 0, c1y = 0;
    heat.viewCentre(c0x, c0y);
    mouse.x = m0 + (m2 - m0) * 0.80f; mouse.y = m1 + (m3 - m1) * 0.15f;
    draw(nullptr);
    mouse.left = true; draw(nullptr);
    mouse.x = m0 + (m2 - m0) * 0.85f; draw(nullptr);
    mouse.left = false; draw(nullptr);
    heat.viewCentre(c1x, c1y);
    expect(std::abs(c1x - c0x) > 50.0f, "a drag on the minimap moves the view");
    draw("9_minimap");

    // Back (Alt+Left): to where the selection was before the jump.
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, true);  draw(nullptr);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false); draw(nullptr);
    draw(nullptr);
    expect(heat.selectedByte(plane, phys) && plane == pom2::HeatData::kMain && phys == 0x2A17,
           "Alt+Left goes back to the byte selected before the jump");

    // The minimap folds away and comes back.
    float bxm = 0, bym = 0;
    expect(heat.minimapButtonCentre(bxm, bym), "the minimap has a collapse button");
    mouse.x = bxm; mouse.y = bym; draw(nullptr);
    mouse.left = true; draw(nullptr);
    mouse.left = false; draw(nullptr);
    expect(!heat.minimapRect(m0, m1, m2, m3), "the collapse button folds the minimap");
    draw("9b_minimap_collapsed");
    expect(heat.minimapButtonCentre(bxm, bym), "a collapsed minimap leaves a pill");
    mouse.x = bxm; mouse.y = bym; draw(nullptr);
    mouse.left = true; draw(nullptr);
    mouse.left = false; draw(nullptr);
    expect(heat.minimapRect(m0, m1, m2, m3), "the pill brings it back");

    // The View popup: everything the toolbar does not show.
    float vbx = 0, vby = 0;
    heat.viewButtonCentre(vbx, vby);
    mouse.x = vbx; mouse.y = vby; draw(nullptr);
    mouse.left = true; draw(nullptr);
    mouse.left = false; draw(nullptr);
    draw(nullptr);
    float p0x = 0, p0y = 0, p1x = 0, p1y = 0;
    expect(heat.viewPopupRect(p0x, p0y, p1x, p1y) && p0x >= 0 && p0y >= 0 && p1x <= kW && p1y <= kH,
           "the View popup opens, whole, on screen");
    draw("11_view_popup");
    mouse.x = p1x + 40.0f; mouse.y = 300.0f; draw(nullptr);   // close it: a click beside it
    mouse.x = 760; mouse.y = 40; draw(nullptr);
    mouse.left = true; draw(nullptr);
    mouse.left = false; draw(nullptr);

    // The look is fixed: a saved FT palette from an older version is ignored.
    heat.setOptionsText(heat.optionsText() + " palette=ft");
    expect(heat.optionsText().find("palette=ft") == std::string::npos,
           "a saved palette choice is ignored (no Look section)");

    ctrl.setMode(EmulationController::Mode::Stopped);
    open = false;
    draw(nullptr);                       // detaches
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    std::printf("heatmap_ui_probe: %s (%d frames)\n", failures ? "FAILED" : "OK", frame);
    return failures ? 1 : 0;
}
