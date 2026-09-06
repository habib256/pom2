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

// MainWindow_SettingsPanels — the ImGui bodies for the settings/input panels:
// No-Slot Clock, the 3D voxel view settings, the NTSC shader settings, the
// joystick/paddle panel and the mouse inspector. Moved out of MainWindow.cpp
// verbatim; none touch the anonymous-namespace helpers that keep the storage
// panels there. See the file-size ratchet.

#include "MainWindow.h"
#include "EmulationController.h"

#include "JoystickInput.h"
#include "JoystickPanel_ImGui.h"
#include "Logger.h"
#include "Voxel3DRenderer.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "MouseCoordinator.h"
#include "NoSlotClock.h"
#include "NtscPostProcessor.h"
#include "Settings.h"

#include "imgui.h"

void MainWindow::renderNoSlotClockPanelWindow()
{
    if (!show(pom2::PanelId::NoSlotClock)) return;

    ImGui::SetNextWindowSize(ImVec2(420, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("No-Slot Clock (Dallas DS1216E)###nsclockPanel",
                      &show(pom2::PanelId::NoSlotClock))) {
        ImGui::End();
        return;
    }

    pom2::NoSlotClock& nsc = controller->noSlotClock();
    bool enabled = nsc.isEnabled();
    if (ImGui::Checkbox("Enabled", &enabled)) {
        nsc.setEnabled(enabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Dallas DS1216E SmartWatch — virtual chip under the\n"
            "internal ROM. AppleWin-parity placement:\n"
            "  II / II+   : under Monitor ROM at $F800-$FFFF\n"
            "  //e / //c  : under $C300 + $C800 internal ROM\n"
            "ProDOS 2.0.3+ / GS-OS walk the 64-bit magic key\n"
            "0x5CA33AC55CA33AC5 (A2=0 reads, A0 = next bit),\n"
            "then read 64 clock bits via A2=1 reads on D0.");
    }

    ImGui::Separator();
    const auto phase = nsc.phase();
    const char* phaseName = (phase == pom2::NoSlotClock::Phase::Idle)
        ? "idle (pass-through)"
        : (phase == pom2::NoSlotClock::Phase::MatchingKey)
            ? "matching magic key"
            : "reading clock register";
    ImGui::Text("Phase: %s", phaseName);
    ImGui::Text("Key bits matched : %d / 64", nsc.keyBitsMatched());
    ImGui::Text("Clock bits read  : %d / 64", nsc.clockBitsRead());

    ImGui::Separator();
    ImGui::TextWrapped(
        "Place a free clock card in a slot for older software, or "
        "leave this enabled for ProDOS 2.0.3+/GS-OS auto-detection "
        "on any profile (incl. //c, where no slot card can exist).");

    ImGui::End();
}

// ─── 3D voxel view settings (MicroM8 "Voxel Cube") ───────────────────────
//
// Live sliders for the geometry knobs the Voxel3DRenderer exposes: cube
// thickness, the per-colour forward "pop", footprint fill, supersample
// (anti-alias) factor and the ambient floor. The grid resolution is NOT a
// knob — it always tracks the live display (one voxel per Apple II pixel).
// Values persist under the `voxel_*` keys; they bind straight to `voxel3d_`
// (owned up-front at settings-load, so the panel works before the view is on).
void MainWindow::renderVoxelSettingsWindow()
{
    if (!show(pom2::PanelId::VoxelSettings)) return;
    if (!voxel3d_) voxel3d_ = std::make_unique<pom2::Voxel3DRenderer>();

    ImGui::SetNextWindowSize(ImVec2(360, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("3D Voxel View", &show(pom2::PanelId::VoxelSettings))) {
        ImGui::End();
        return;
    }

    // Quick enable toggle, mirroring the View-menu item so the panel is usable
    // stand-alone. Greys out the knobs while the 3D view is off.
    ImGui::Checkbox("Enable 3D voxel view", &show(pom2::PanelId::Voxel));
    ImGui::SameLine();
    ImGui::TextDisabled("(left-drag orbit · middle-drag pan · wheel zoom)");
    ImGui::Separator();

    ImGui::BeginDisabled(!show(pom2::PanelId::Voxel));

    pom2::Voxel3DRenderer& v = *voxel3d_;
    ImGui::SliderFloat("Voxel depth",  &v.voxelDepth, 0.0f, 12.0f, "%.1f cells", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uniform Z-thickness of every cube (in pixel units).");
    ImGui::SliderFloat("Colour pop",   &v.colorShift, 0.0f, 24.0f, "%.1f cells", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("MicroM8 'Z-axis 3D offset': brighter pixels push\n"
                          "toward the viewer for pin-art relief. 0 = flat slab.");
    ImGui::SliderFloat("Cube fill",    &v.cubeFill,   0.2f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Footprint as a fraction of the cell. 1.0 = contiguous\n"
                          "(no gap grid — best against moiré); lower = visible gaps.");
    ImGui::SliderInt  ("Anti-alias",   &v.superSample, 1, 4, "%dx supersample", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("FBO render scale, minify-downsampled. Higher = smoother\n"
                          "edges (kills moiré) but more GPU. 1 = off.");
    ImGui::SliderFloat("Ambient",      &v.ambient,    0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lighting floor so no cube face goes pure black.");

    ImGui::Checkbox("Mono", &v.mono);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("MicroM8 'Voxel Cube Mono' — grey output, relief kept.");
    ImGui::SameLine();
    ImGui::Checkbox("Per-colour depth", &v.perColorDepth);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap each pixel to the nearest lo-res palette colour and\n"
                          "drive 'Colour pop' from that — discrete, blocky relief\n"
                          "(MicroM8 per-index Z) instead of the smooth luminance field.");

    ImGui::Separator();
    if (ImGui::Button("Reset view")) {
        voxelCam_ = pom2::OrbitCamera{};
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset settings")) {
        pom2::Voxel3DRenderer def;
        v.voxelDepth    = def.voxelDepth;
        v.colorShift    = def.colorShift;
        v.cubeFill      = def.cubeFill;
        v.superSample   = def.superSample;
        v.ambient       = def.ambient;
        v.mono          = def.mono;
        v.perColorDepth = def.perColorDepth;
    }

    ImGui::EndDisabled();
    ImGui::End();
}

// ─── CRT Settings (Composite NTSC mode) ──────────────────────────────────
//
// Sliders that drive the OpenEmulator-style shader: standard four TV knobs
// (B/C/S/H), sharpness (chroma bandwidth), persistence (CRT afterglow), and
// the pure post-effects (scanlines, barrel, vignette, shadow mask). All
// values are persisted to settings.json under the `ntsc_*` keys so the look
// survives across sessions. No look presets: they overwrote the whole glass
// block in one click, which made the panel hard to reason about — the
// defaults plus "Reset to defaults" are the only starting points now.
void MainWindow::renderNtscSettingsWindow()
{
    if (!show(pom2::PanelId::Crt)) return;

    ImGui::SetNextWindowSize(ImVec2(380, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CRT Settings (Composite NTSC)",
                      &show(pom2::PanelId::Crt))) {
        ImGui::End();
        return;
    }

    const pom2::Palette& pal = pom2::palette();
    const auto u32 = ImGui::ColorConvertU32ToFloat4;

    // Master ON/OFF for every CRT effect, full-width at the top of the window.
    // Off bypasses the whole effect stack (the colour pipeline still runs);
    // the controls below grey out so it's clear they have no effect.
    {
        const bool on = crtEffectsEnabled;
        const ImVec4 col = u32(on ? pal.ok : pal.danger);
        // Tint the face at low alpha and put the full-strength colour on the
        // text: a saturated full-width slab was the loudest thing in the UI.
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(col.x, col.y, col.z, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(col.x, col.y, col.z, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(col.x, col.y, col.z, 0.44f));
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        if (ImGui::Button(on ? "CRT effects: ON  —  click to disable"
                             : "CRT effects: OFF  —  click to enable",
                          ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            crtEffectsEnabled = !crtEffectsEnabled;
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::BeginDisabled(!crtEffectsEnabled);

    pom2::NtscParams p = ntscFx ? ntscFx->getParams() : pom2::NtscParams{};
    bool changed = false;

    // ── Scope notes ──────────────────────────────────────────────────────
    // What actually applies right now. Kept terse and dim: it is reference
    // material, not a warning.
    const bool oeFamily =
        display->getHiResMode() == Apple2Display::HiResMode::ColorCompositeOE ||
        display->getHiResMode() == Apple2Display::HiResMode::ColorCompositeOECpu;
    if (!oeFamily) {
        ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
        ImGui::TextWrapped(
            "Every glass control below applies on this mode. PAL composite and "
            "Sharp text are demodulation-only — they affect just the two "
            "'Composite (OpenEmulator)' pipelines.");
        ImGui::PopStyleColor();
    }

    if (ntscFx && !ntscFx->available()) {
        // Previously this read as a flat contradiction: a green "CRT Effects:
        // ON" banner immediately above a red "Shader unavailable", leaving the
        // user unable to tell whether the controls below did anything. Scope
        // it explicitly — only the OpenEmulator *demodulation* shader is
        // missing; the CRT glass stack is a separate pass and still runs.
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.warn));
        ImGui::TextWrapped(
            "OpenEmulator demodulation shader unavailable — the two "
            "'Composite (OpenEmulator)' pipelines fall back to the NTSC LUT. "
            "The glass controls below are a separate pass and still apply.");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ntscFx->lastError().c_str());
    }

    // ── Advanced ─────────────────────────────────────────────────────────
    // Open by default: the look presets that used to be the panel's primary
    // control are gone, so the sliders are the only controls left and hiding
    // them behind a collapsed header would leave the panel empty on open.
    // Labels lead, sliders fill the rest of the row:
    // ImGui's native SliderFloat puts its label on the RIGHT, which made the
    // panel read "bar → number → name" and clipped the longest label
    // ("Phosphor curve (ga…"). Two decimals, not three — these are perceptual
    // knobs and 0.055 was false precision.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        // Widest label sets the column, so it can never clip. Measured rather
        // than hardcoded so it survives the UI zoom.
        const float labelW = ImGui::CalcTextSize("Analog bandwidth").x +
                             ImGui::GetStyle().ItemSpacing.x * 2.0f;
        auto slider = [&](const char* label, const char* id, float* v,
                          float lo, float hi, const char* tip,
                          const char* fmt = "%.2f") {
            ImGui::TextUnformatted(label);
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat(id, v, lo, hi, fmt,
                                   ImGuiSliderFlags_AlwaysClamp))
                changed = true;
        };

        ImGui::SeparatorText("Picture");
        slider("Brightness", "##bright", &p.brightness, -0.5f, 0.5f,
               "Added to luma.");
        slider("Contrast",   "##contrast", &p.contrast,  0.5f, 1.5f,
               "Scaling around mid-grey.");
        slider("Saturation", "##sat", &p.saturation, 0.0f, 2.0f,
               "Chroma multiplier. 0 = monochrome.");
        slider("Hue",        "##hue", &p.hue, -0.5f, 0.5f,
               "I/Q rotation. Full turn at +/-0.5.");

        ImGui::SeparatorText("Phosphor");
        slider("Sharpness",   "##sharp", &p.sharpness, 0.0f, 1.0f,
               "Chroma bandwidth. Lower = more composite bleed.");
        slider("Persistence", "##persist", &p.persistence, 0.0f, 0.95f,
               "Temporal decay — the phosphor's afterglow.");
        slider("Phosphor gamma", "##gamma", &p.phosphorGamma, 0.6f, 2.6f,
               "Response curve. 1.0 = flat, >1 deepens shadows.\n"
               "Pairs with Persistence as the phosphor model.");

        ImGui::SeparatorText("Glass");
        slider("Scanlines", "##scan", &p.scanlines, 0.0f, 1.0f,
               "0 = off, 1 = black between every line.");
        slider("Barrel",    "##barrel", &p.barrel, 0.0f, 0.30f,
               "Tube curvature. 0 = flat.");
        slider("Vignette",  "##vign", &p.centerLighting, 0.5f, 1.0f,
               "Center lighting. 1.0 = flat, lower darkens the edges.");

        // Shadow mask: combo + strength. Procedural — no texture upload, no
        // perf cost when Off.
        static const char* kMaskNames[] = {
            "Off", "Triad (3-stripe)", "Aperture grille (Trinitron)",
            "Dot mask (offset triads)"
        };
        int maskIdx = static_cast<int>(p.shadowMask);
        ImGui::TextUnformatted("Shadow mask");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##mask", &maskIdx, kMaskNames,
                         IM_ARRAYSIZE(kMaskNames))) {
            p.shadowMask = static_cast<pom2::NtscParams::ShadowMask>(maskIdx);
            changed = true;
        }
        ImGui::BeginDisabled(p.shadowMask == pom2::NtscParams::ShadowMask::Off);
        slider("Mask strength", "##maskstr", &p.shadowMaskStrength, 0.0f, 1.0f,
               nullptr);
        ImGui::EndDisabled();
        slider("Luminance gain", "##lumgain", &p.luminanceGain, 1.0f, 2.0f,
               "Post-glass re-brighten — compensates the dimming\n"
               "from scanlines and the shadow mask.");

        // The cable between the machine and the tube. OpenEmulator models
        // this as its "connection" type; POM2 exposes it as one figure in MHz
        // because the interesting case — Le Chat Mauve — is literally a choice
        // of connector: the TTL RGB header (square dots, 0 = off) or the
        // analog Péritel socket (resistor ladders, trim pots and a cable,
        // ~5-6 MHz). The filter runs on the source sample grid, so the same
        // figure lands differently per video mode and needs no per-mode knob.
        ImGui::SeparatorText("Analog link");
        slider("Analog bandwidth", "##rgbbw", &p.rgbBandwidthMHz, 0.0f, 8.0f,
               "Bandwidth of the analog video chain, in MHz.\n"
               "0 = off — a direct/TTL link, square dots.\n"
               "~5-6 = an analog RGB (Peritel/SCART) cable: 560-dot\n"
               "detail (DHGR, COL280) softens, 280-dot HGR barely moves.\n"
               "Above a mode's Nyquist the filter is skipped entirely.",
               "%.1f MHz");

        ImGui::SeparatorText("Demodulation (OpenEmulator pipelines only)");
        // PAL composite — line-phase alternation. Off by default (POM2 ships
        // with the NTSC look most users associate with the Apple II). It
        // describes the machine being emulated, not a look.
        changed |= ImGui::Checkbox("PAL composite (line-phase alternation)",
                                   &p.palMode);
        // Sharp-text bypass: keep glyphs crisp in TEXT mode by skipping the
        // shader for the whole text screen. HGR/DHGR/lo-res still run through
        // the demodulator.
        changed |= ImGui::Checkbox("Sharp text (bypass shader in TEXT mode)",
                                   &p.textSharp);

        ImGui::Spacing();
        if (ImGui::Button("Reset to defaults")) {
            p = pom2::NtscParams{};
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Saved to ntsc_* keys");
    }

    ImGui::EndDisabled();

    if (changed) {
        if (!ntscFx) ntscFx = std::make_unique<pom2::NtscPostProcessor>();
        ntscFx->setParams(p);
    }

    ImGui::End();
}

void MainWindow::renderJoystickPanelWindow()
{
    if (!show(pom2::PanelId::Joystick)) return;

    pom2::JoystickPanel_ImGui::Snapshot snap;
    for (int h = 0; h < JoystickInput::kHostCount; ++h) {
        const auto& d = joystick->deviceState(h);
        if (!d.present) continue;
        pom2::JoystickPanel_ImGui::HostDevice hd;
        hd.index   = h;
        hd.name    = d.name;
        hd.axis    = d.axis;
        hd.buttons = d.buttons;
        snap.hosts.push_back(std::move(hd));
    }
    const auto& cf = joystick->binding();
    snap.hostIdx    = cf.hostIdx;
    snap.deadzone   = cf.deadzone;
    snap.invert     = cf.invert;
    snap.squareGate = cf.squareGate;
    for (int i = 0; i < 4; ++i) snap.appleIIPaddle[i] = joystick->paddleValue(i);
    for (int i = 0; i < 3; ++i) snap.appleIIButton[i] = joystick->buttonDown(i);

    auto result = joystickPanel->render("Joystick", show(pom2::PanelId::Joystick), snap);
    if (result.changed) {
        auto& bind = joystick->binding();
        const bool hostPicked = (result.hostIdx != bind.hostIdx);
        bind.hostIdx    = result.hostIdx;
        bind.deadzone   = result.deadzone;
        bind.invert     = result.invert;
        bind.squareGate = result.squareGate;
        // Picking a device — "(none)" included — is a decision, and the
        // auto-binder must stop overruling it. See markBindingExplicit().
        if (hostPicked) joystick->markBindingExplicit();
        // The whole binding persists, not just the square gate: deadzone,
        // invert and the chosen pad were re-derived from defaults on every
        // launch, so a user with drift dialled out got it back each session.
        if (settings) {
            settings->setBool ("joystick_square_gate", bind.squareGate);
            settings->setInt  ("joystick_host",        bind.hostIdx);
            settings->setFloat("joystick_deadzone",    bind.deadzone);
            settings->setBool ("joystick_invert_x",    bind.invert[0]);
            settings->setBool ("joystick_invert_y",    bind.invert[1]);
        }
    }
}

// ─── Mouse Inspector ─────────────────────────────────────────────────────
//
// Diagnostic panel for tuning Apple II Mouse Card alignment. Live readout
// of: host cursor (window coords + widget-local + in-widget fraction),
// Apple II Screen widget rect + per-axis logical→host scale, MouseCard's
// 8-bit running counter + sub-pixel accumulator, AppleWin HLE firmware
// state (clamp window, current iX/iY, MOUSE_READ snapshot, mode/state
// bits, PIA port latches, last command), and the AppleMouse firmware
// screen holes for the active slot. Optional CSV log at ~30 Hz so a
// session of cursor motion can be replayed offline.

void MainWindow::renderMouseInspectorWindow()
{
    if (!show(pom2::PanelId::Mouse)) return;
    ImGui::SetNextWindowPos (ImVec2(40, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mouse Inspector", &show(pom2::PanelId::Mouse))) {
        ImGui::End();
        return;
    }

    const float widgetW = screenRectMax.x - screenRectMin.x;
    const float widgetH = screenRectMax.y - screenRectMin.y;
    const double hostLocalX = lastMouseHostX - double(screenRectMin.x);
    const double hostLocalY = lastMouseHostY - double(screenRectMin.y);
    const bool hostInside =
        widgetW > 0.0f && widgetH > 0.0f &&
        lastMouseHostX >= double(screenRectMin.x) &&
        lastMouseHostX <= double(screenRectMax.x) &&
        lastMouseHostY >= double(screenRectMin.y) &&
        lastMouseHostY <= double(screenRectMax.y);
    const double fracX = widgetW > 0.0f ? hostLocalX / double(widgetW) : 0.0;
    const double fracY = widgetH > 0.0f ? hostLocalY / double(widgetH) : 0.0;
    const int dispW = display->width();
    const int dispH = display->height();
    // Apple-cursor pixels per host pixel — what onMouseMove uses to scale
    // host deltas to MCU 8-bit counts. Always derived from the constant
    // kWidth/kHeight (the widget is rendered at that aspect, not at the
    // current display resolution — see the comment in onMouseMove).
    const double sxRatio =
        widgetW > 0.0f ? double(Apple2Display::kWidth)  / double(widgetW) : 0.0;
    const double syRatio =
        widgetH > 0.0f ? double(Apple2Display::kHeight) / double(widgetH) : 0.0;

    // ── Host cursor ────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Host cursor", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Window coords : (%.1f, %.1f)", lastMouseHostX, lastMouseHostY);
        ImGui::Text("Widget-local  : (%.1f, %.1f)", hostLocalX, hostLocalY);
        ImGui::Text("Fraction      : (%.3f, %.3f)", fracX, fracY);
        ImGui::Text("Button held   : %s", mouseButtonHeld ? "YES" : "no");
        ImGui::TextColored(
            hostInside ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                       : ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
            "Inside Apple II Screen widget: %s", hostInside ? "YES" : "no");
        // Captured, "Window coords" above is GLFW's *virtual* unbounded
        // position — it walks past the window edges and the widget-local /
        // fraction rows below it stop meaning anything. Say so rather than
        // letting the numbers read as a bug.
        ImGui::TextColored(
            mouseGrabbed_ ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                          : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "Pointer captured (grab)     : %s",
            mouseGrabbed_ ? "YES — coords above are virtual" : "no");
    }

    // ── Apple II Screen widget rect ───────────────────────────────────
    if (ImGui::CollapsingHeader("Apple II Screen widget",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Rect min      : (%.1f, %.1f)", screenRectMin.x, screenRectMin.y);
        ImGui::Text("Rect max      : (%.1f, %.1f)", screenRectMax.x, screenRectMax.y);
        ImGui::Text("Size          : %.1f x %.1f host px", widgetW, widgetH);
        ImGui::Text("Display res   : %d x %d (kWidth=%d kHeight=%d)",
                    dispW, dispH, Apple2Display::kWidth, Apple2Display::kHeight);
        ImGui::Text("Apple px/host : %.4f x %.4f (used by onMouseMove)",
                    sxRatio, syRatio);
    }

    // One immutable snapshot for the whole panel body. It resolves both card
    // implementations AND reads the AppleMouse screen holes inside a single
    // lockState(), where this panel used to hold a card alias across the frame
    // and then take the lock a second time just for the holes.
    const auto mouseInventory = mouseCoordinator_->capture();

    // ── MouseCard 8-bit running counter (MainWindow side) ─────────────
    if (ImGui::CollapsingHeader("MouseCard input (8-bit counter)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Apple counter : (%3u, %3u)  [0x%02X, 0x%02X]",
                    mouseAppleX, mouseAppleY, mouseAppleX, mouseAppleY);
        ImGui::Text("Sub-pixel acc : (%.3f, %.3f)",
                    mouseSubAppleX, mouseSubAppleY);
        const char* cardName =
            mouseInventory.appleWinPlugged ? "AppleWin HLE (mouseaw)" :
            mouseInventory.mamePlugged     ? "MAME-faithful (mouse)" :
                                             "(no card plugged)";
        ImGui::Text("Active card   : %s", cardName);
    }

    // ── AppleWin HLE card-internal state ──────────────────────────────
    if (mouseInventory.appleWinPlugged) {
        if (ImGui::CollapsingHeader("AppleWin HLE — firmware state",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& s = mouseInventory.appleWin;
            ImGui::Text("Clamp X       : [%d .. %d]", s.iMinX, s.iMaxX);
            ImGui::Text("Clamp Y       : [%d .. %d]", s.iMinY, s.iMaxY);
            ImGui::Text("Cursor iX/iY  : (%d, %d)", s.iX, s.iY);
            ImGui::Text("Read   nX/nY  : (%d, %d)  (last MOUSE_READ snap)",
                        s.nX, s.nY);
            ImGui::Text("Buttons curr  : btn0=%d btn1=%d   prev: btn0=%d btn1=%d",
                        s.bBtn0, s.bBtn1, s.bPrevBtn0, s.bPrevBtn1);
            ImGui::Text("MODE  ($00)   : 0x%02X  on=%d intMove=%d intBtn=%d intVBL=%d",
                        s.byMode,
                        (s.byMode & 0x01) ? 1 : 0,
                        (s.byMode & 0x02) ? 1 : 0,
                        (s.byMode & 0x04) ? 1 : 0,
                        (s.byMode & 0x08) ? 1 : 0);
            ImGui::Text("STATE byte    : 0x%02X  curBtn0=%d curBtn1=%d moved=%d",
                        s.byState,
                        (s.byState & 0x80) ? 1 : 0,
                        (s.byState & 0x10) ? 1 : 0,
                        (s.byState & 0x20) ? 1 : 0);
            const char* cmdName = "(unknown)";
            switch (s.lastCmd & 0xF0) {
                case 0x00: cmdName = "MOUSE_SET";   break;
                case 0x10: cmdName = "MOUSE_READ";  break;
                case 0x20: cmdName = "MOUSE_SERV";  break;
                case 0x30: cmdName = "MOUSE_CLEAR"; break;
                case 0x40: cmdName = "MOUSE_POS";   break;
                case 0x50: cmdName = "MOUSE_INIT";  break;
                case 0x60: cmdName = "MOUSE_CLAMP"; break;
                case 0x70: cmdName = "MOUSE_HOME";  break;
                case 0x90: cmdName = "MOUSE_TIME";  break;
            }
            ImGui::Text("Last cmd byte : 0x%02X (%s)  buffPos=%d dataLen=%d",
                        s.lastCmd, cmdName, s.buffPos, s.dataLen);
            ImGui::Text("PIA latches   : A=0x%02X  B=0x%02X", s.by6821A, s.by6821B);
        }
    } else if (mouseInventory.mamePlugged) {
        if (ImGui::CollapsingHeader("MAME-faithful — card state",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled(
                "Firmware position lives inside the 68705P3 MCU RAM —");
            ImGui::TextDisabled(
                "use the screen-hole readout below for the cursor state.");
        }
    }

    // ── AppleMouse firmware screen holes (per Apple II Mouse FAQ) ─────
    if (ImGui::CollapsingHeader("Screen holes (AppleMouse firmware)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const int activeSlot = mouseInventory.slot > 0 ? mouseInventory.slot : 0;
        if (activeSlot < 1 || activeSlot > 7) {
            ImGui::TextDisabled("(no mouse card plugged)");
        } else {
            const auto& holes = mouseInventory.holes;
            const int holeXlo = holes.xLo, holeXhi = holes.xHi;
            const int holeYlo = holes.yLo, holeYhi = holes.yHi;
            const int holeMode = holes.mode, holeStatus = holes.status;
            const int holeX = holes.x();
            const int holeY = holes.y();
            ImGui::Text("Slot          : %d", activeSlot);
            ImGui::Text("X = $%04X     : %d  (lo $%04X=0x%02X  hi $%04X=0x%02X)",
                        0x0478 + activeSlot, holeX,
                        0x0478 + activeSlot, holeXlo,
                        0x0578 + activeSlot, holeXhi);
            ImGui::Text("Y = $%04X     : %d  (lo $%04X=0x%02X  hi $%04X=0x%02X)",
                        0x04F8 + activeSlot, holeY,
                        0x04F8 + activeSlot, holeYlo,
                        0x05F8 + activeSlot, holeYhi);
            ImGui::Text("Mode  $%04X   : 0x%02X (bit0=mouseOn=%d)",
                        0x07F8 + activeSlot, holeMode, holeMode & 0x01);
            ImGui::Text("Status $%04X  : 0x%02X (bit7=btnDown bit5=moved)",
                        0x0778 + activeSlot, holeStatus);
        }
    }

    // ── CSV logging ───────────────────────────────────────────────────
    ImGui::Separator();
    const bool logging = mouseInspectorLogStream != nullptr;
    if (!logging) {
        if (ImGui::Button("Start logging to CSV")) {
            mouseInspectorLogPath = "mouse_inspector.csv";
            mouseInspectorLogStream =
                std::make_unique<std::ofstream>(mouseInspectorLogPath);
            if (*mouseInspectorLogStream) {
                *mouseInspectorLogStream
                    << "t_s,hostX,hostY,inside,widgetMinX,widgetMinY,"
                       "widgetW,widgetH,appleCntX,appleCntY,btn,"
                       "awIX,awIY,awMinX,awMaxX,awMinY,awMaxY,"
                       "awMode,awState,holeX,holeY,holeMode\n";
                mouseInspectorLastLogTime = 0.0;
                pom2::log().info("MouseInspector",
                    "Logging to " + mouseInspectorLogPath);
            } else {
                mouseInspectorLogStream.reset();
                pom2::log().warn("MouseInspector",
                    "Cannot open " + mouseInspectorLogPath);
            }
        }
    } else {
        if (ImGui::Button("Stop logging")) {
            mouseInspectorLogStream.reset();
            pom2::log().info("MouseInspector",
                "Stopped logging to " + mouseInspectorLogPath);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("→ %s", mouseInspectorLogPath.c_str());
    }
    ImGui::TextDisabled(
        "CSV row per ~33 ms (panel-driven); flushed after each row.");

    // Rate-limit to ~30 Hz so a 5-minute capture stays small. Use
    // ImGui's frame time (monotonic, decoupled from emulated CPU
    // cycles) — the panel is paced by the UI loop, not the worker.
    if (mouseInspectorLogStream) {
        const double now = ImGui::GetTime();
        if (now - mouseInspectorLastLogTime >= 1.0 / 30.0) {
            mouseInspectorLastLogTime = now;
            const int holeX = mouseInventory.holes.x();
            const int holeY = mouseInventory.holes.y();
            const int holeMode = mouseInventory.holes.mode;
            const auto& s = mouseInventory.appleWin;
            auto& os = *mouseInspectorLogStream;
            os << now << ','
               << lastMouseHostX << ',' << lastMouseHostY << ','
               << (hostInside ? 1 : 0) << ','
               << screenRectMin.x << ',' << screenRectMin.y << ','
               << widgetW << ',' << widgetH << ','
               << int(mouseAppleX) << ',' << int(mouseAppleY) << ','
               << (mouseButtonHeld ? 1 : 0) << ','
               << s.iX << ',' << s.iY << ','
               << s.iMinX << ',' << s.iMaxX << ','
               << s.iMinY << ',' << s.iMaxY << ','
               << int(s.byMode) << ',' << int(s.byState) << ','
               << holeX << ',' << holeY << ',' << holeMode << '\n';
            os.flush();
        }
    }

    ImGui::End();
}
