// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// MainWindow_CrtPanel — the CRT Settings panel and the per-monitor glass
// settings it edits: load (with the one-time carry-over of the old global
// `ntsc_*` keys), save, the switch of look when the display mode changes, and
// the panel itself.
//
// The policy is `CrtPresets.h` and is tested there (`crt_presets`); this file
// is the part that needs a settings file and an ImGui context. One rule
// crosses the two: the parameters the processors run on (`ntscFx->getParams()`)
// are always `Overrides::resolve(current monitor)` — nothing writes a look
// into them directly, or the next change of display mode would lose it.

#include "MainWindow.h"

#include "Apple2Display.h"
#include "CrtPresets.h"
#include "NtscPostProcessor.h"
#include "Pom2Theme.h"
#include "Settings.h"

#include "imgui.h"
#include "imgui_internal.h" // TempInputIsActive: Ctrl-click edits must stay opaque

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

namespace {

using pom2::crt::Field;
using pom2::crt::Monitor;

Monitor monitorFor(Apple2Display::HiResMode mode)
{
    using M = Apple2Display::HiResMode;
    switch (mode) {
        case M::ColorNTSC:           return Monitor::ColorNtsc;
        case M::ColorCompMedium:     return Monitor::ColorMedium;
        case M::ColorComp4Bit:       return Monitor::Color4Bit;
        case M::ChatMauveRGB:        return Monitor::ChatMauve;
        case M::ColorCompositeOE:    return Monitor::CompositeOE;
        case M::ColorCompositeOECpu: return Monitor::CompositeOECpu;
        case M::MonoWhite:           return Monitor::MonoWhite;
        case M::MonoGreen:           return Monitor::MonoGreen;
        case M::MonoAmber:           return Monitor::MonoAmber;
        case M::ColorAppleWin:       return Monitor::AppleWin;
    }
    return Monitor::CompositeOE;
}

/// The browser build caps the tube curvature: the canvas is letterboxed by
/// the page, and a deep barrel leaves black corners the page cannot hide.
/// Applied to what is SHOWN, never to what is stored.
pom2::NtscParams shown(pom2::NtscParams p)
{
#ifdef __EMSCRIPTEN__
    p.barrel = std::min(p.barrel, 0.03f);
#endif
    return p;
}

}  // namespace

void MainWindow::loadCrtSettings()
{
    crtOverrides_ = std::make_unique<pom2::crt::Overrides>();
    pom2::crt::Overrides& ov = *crtOverrides_;
    const pom2::NtscParams defaults;

    // One-time carry-over. Until 2026-11-05 there was ONE look for every
    // display mode, in fourteen `ntsc_*` keys. A value the user had moved off
    // the default becomes an override on every monitor — the picture they
    // tuned is the picture they get, on whichever mode they were using — and
    // a value still at its default becomes nothing, so the presets apply.
    if (!settings->getBool("crt_per_monitor", false)) {
        for (std::size_t f = 0; f < pom2::crt::kFields; ++f) {
            const auto field = static_cast<Field>(f);
            const char* legacy = pom2::crt::info(field).legacy;
            if (!legacy || !settings->has(legacy)) continue;
            const float def = pom2::crt::get(defaults, field);
            const float v   = pom2::crt::clampField(field, settings->getFloat(legacy, def));
            if (std::fabs(v - def) > 1e-4f)
                for (std::size_t m = 0; m < pom2::crt::kMonitors; ++m)
                    ov.set(static_cast<Monitor>(m), field, v);
            settings->erase(legacy);
        }
    }

    for (std::size_t m = 0; m < pom2::crt::kMonitors; ++m)
        for (std::size_t f = 0; f < pom2::crt::kFields; ++f) {
            const auto mon   = static_cast<Monitor>(m);
            const auto field = static_cast<Field>(f);
            const std::string key = pom2::crt::settingKey(mon, field);
            // `Overrides::set` clamps: only values made in the panel are
            // slider-bounded, and a hand-edited `…_center_lighting=0` is
            // 1/0 in the glass shader — a black screen everywhere.
            if (settings->has(key)) ov.set(mon, field, settings->getFloat(key, 0.0f));
        }

    pom2::NtscParams global;
    global.palMode   = settings->getBool("ntsc_pal",        global.palMode);
    global.textSharp = settings->getBool("ntsc_text_sharp", global.textSharp);

    // The display mode may not be restored yet at this point of the
    // constructor; `syncCrtMonitor` corrects the choice on the first frame.
    const Monitor mon = display ? monitorFor(display->getHiResMode())
                                : Monitor::CompositeOE;
    crtMonitor_ = static_cast<int>(mon);
    // The postprocessor's constructor is GL-free; `initialize()` stays lazy.
    ntscFx = std::make_unique<pom2::NtscPostProcessor>();
    ntscFx->setParams(shown(ov.resolve(mon, global)));
}

void MainWindow::saveCrtSettings()
{
    if (!crtOverrides_) return;
    const pom2::crt::Overrides& ov = *crtOverrides_;
    settings->setBool("crt_per_monitor", true);
    for (std::size_t m = 0; m < pom2::crt::kMonitors; ++m)
        for (std::size_t f = 0; f < pom2::crt::kFields; ++f) {
            const auto mon   = static_cast<Monitor>(m);
            const auto field = static_cast<Field>(f);
            const std::string key = pom2::crt::settingKey(mon, field);
            // Absent means "the preset": a field put back to its preset
            // leaves no key, so a later preset reaches it.
            if (ov.has(mon, field)) settings->setFloat(key, ov.value(mon, field));
            else if (settings->has(key)) settings->erase(key);
        }
    if (ntscFx) {
        const pom2::NtscParams& p = ntscFx->getParams();
        settings->setBool("ntsc_pal",        p.palMode);
        settings->setBool("ntsc_text_sharp", p.textSharp);
    }
}

void MainWindow::syncCrtMonitor()
{
    if (!crtOverrides_ || !display) return;
    const int now = static_cast<int>(monitorFor(display->getHiResMode()));
    if (now == crtMonitor_ && ntscFx) return;
    pom2::NtscParams global;
    if (ntscFx) global = ntscFx->getParams();
    else        ntscFx = std::make_unique<pom2::NtscPostProcessor>();
    crtMonitor_ = now;
    ntscFx->setParams(shown(crtOverrides_->resolve(static_cast<Monitor>(now), global)));
}

// ─── CRT Settings ────────────────────────────────────────────────────────
//
// Sliders that drive the glass pass and the OpenEmulator-style demodulator:
// the four TV knobs (B/C/S/H), sharpness (chroma bandwidth), persistence,
// the post-effects (scanlines, barrel, vignette, shadow mask, glow).
//
// The look is PER MONITOR. The panel once had "look presets" and lost them
// because one click overwrote the whole block; what replaced them is layers
// (CrtPresets.h) — each display mode has a built-in preset, a control the
// user moves is stored for that mode alone, and a right-click puts that one
// control back.
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

    ImGui::Checkbox("Monitor II frame", &monitorArtworkEnabled);

    if (!crtOverrides_) loadCrtSettings();
    syncCrtMonitor();
    pom2::crt::Overrides& ov = *crtOverrides_;
    const Monitor mon = static_cast<Monitor>(crtMonitor_);

    // Which look is being edited. It follows the display mode; there is no
    // selector here on purpose — editing a monitor that is not on screen is
    // moving sliders blind.
    ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
    ImGui::TextUnformatted("Look of:");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(pom2::crt::monitorName(mon));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Each display mode keeps its own look: a built-in preset,\n"
            "plus the controls you changed while on it.\n"
            "A changed control is highlighted; right-click it to put\n"
            "that one control back to the preset.");

    ImGui::BeginDisabled(!crtEffectsEnabled);

    pom2::NtscParams p = ntscFx->getParams();
    const pom2::NtscParams before = p;
    const pom2::NtscParams base   = pom2::crt::preset(mon);
    bool changed = false;      // a control moved
    bool restored = false;     // a control went back to its preset
    // Vertex range of the mouse-dragged slider row. The draw list is rebuilt
    // each frame, so opacity restores immediately on release/close/collapse.
    int draggedRowFirst = -1, draggedRowEnd = -1;

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

    if (!ntscFx->available()) {
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
    // Open by default: the sliders are the panel, and hiding them behind a
    // collapsed header would leave it empty on open.
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
        const float labelW =
            std::max(ImGui::CalcTextSize("Analog bandwidth").x,
                     ImGui::CalcTextSize("Glow threshold").x) +
            ImGui::GetStyle().ItemSpacing.x * 2.0f;

        // The label of a control the user changed on this monitor is drawn in
        // the accent colour, and its tooltip says what the preset is.
        auto label = [&](const char* text, Field f, const char* tip,
                         const char* fmt) {
            const bool mine = f != Field::Count && ov.has(mon, f);
            if (mine) ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.accent));
            ImGui::TextUnformatted(text);
            if (mine) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && (tip || mine)) {
                ImGui::BeginTooltip();
                if (tip) ImGui::TextUnformatted(tip);
                if (mine) {
                    char preset[48];
                    std::snprintf(preset, sizeof preset, fmt,
                                  static_cast<double>(pom2::crt::get(base, f)));
                    ImGui::TextDisabled("Changed on this monitor — preset %s.\n"
                                        "Right-click the control to restore it.",
                                        preset);
                }
                ImGui::EndTooltip();
            }
            return mine;
        };
        auto slider = [&](const char* text, const char* id, float* v,
                          float lo, float hi, const char* tip,
                          const char* fmt = "%.2f") {
            const int rowFirst = ImGui::GetWindowDrawList()->VtxBuffer.Size;
            const Field f = pom2::crt::fieldAt(p, v);
            const bool mine = label(text, f, tip, fmt);
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat(id, v, lo, hi, fmt,
                                   ImGuiSliderFlags_AlwaysClamp))
                changed = true;
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
                !ImGui::TempInputIsActive(ImGui::GetItemID())) {
                draggedRowFirst = rowFirst;
                draggedRowEnd = ImGui::GetWindowDrawList()->VtxBuffer.Size;
            }
            if (mine && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                ov.clear(mon, f);
                restored = true;
            }
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

        // Light scattered in the glass and the phosphor layer: the bright
        // parts of the picture spill a soft halo over their surroundings.
        // Off on the colour presets, on (modestly) on the monochrome ones.
        ImGui::SeparatorText("Glow");
        slider("Glow", "##bloom", &p.bloom, 0.0f, 1.0f,
               "Light spilling out of the bright parts of the picture.\n"
               "0 = off (and costs nothing).");
        ImGui::BeginDisabled(p.bloom <= 0.0f);
        slider("Glow radius", "##bloomrad", &p.bloomRadius, 0.5f, 6.0f,
               "How far the halo reaches, in 560-dot columns per blur tap.",
               "%.1f");
        slider("Glow threshold", "##bloomthr", &p.bloomThreshold, 0.0f, 1.0f,
               "Only what is brighter than this glows.\n"
               "Lower = the whole picture hazes; higher = highlights only.");
        ImGui::EndDisabled();

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
        const bool maskMine = label("Shadow mask", Field::ShadowMask, nullptr, "%.0f");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##mask", &maskIdx, kMaskNames,
                         IM_ARRAYSIZE(kMaskNames))) {
            p.shadowMask = static_cast<pom2::NtscParams::ShadowMask>(maskIdx);
            changed = true;
        }
        if (maskMine && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ov.clear(mon, Field::ShadowMask);
            restored = true;
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
        // describes the machine being emulated, not a look — so it is one
        // setting for every monitor, like Sharp text below.
        changed |= ImGui::Checkbox("PAL composite (line-phase alternation)",
                                   &p.palMode);
        // Sharp-text bypass: keep glyphs crisp in TEXT mode by skipping the
        // shader for the whole text screen. HGR/DHGR/lo-res still run through
        // the demodulator.
        changed |= ImGui::Checkbox("Sharp text (bypass shader in TEXT mode)",
                                   &p.textSharp);

        ImGui::Spacing();
        const int mine = ov.count(mon);
        ImGui::BeginDisabled(mine == 0);
        if (ImGui::Button("Back to this monitor's preset")) {
            ov.clear(mon);
            restored = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (mine == 0) ImGui::TextDisabled("preset, nothing changed");
        else           ImGui::TextDisabled("%d control%s changed", mine, mine == 1 ? "" : "s");
    }

    ImGui::EndDisabled();

    if (changed) {
        // Whatever moved this frame becomes this monitor's own value. Read
        // off the difference rather than reported control by control, so a
        // control added to the panel cannot forget to register.
        for (std::size_t f = 0; f < pom2::crt::kFields; ++f) {
            const auto field = static_cast<Field>(f);
            const float v = pom2::crt::get(p, field);
            if (v != pom2::crt::get(before, field)) ov.set(mon, field, v);
        }
    }
    if (changed || restored)
        ntscFx->setParams(shown(ov.resolve(mon, p)));   // `p` carries PAL / sharp text

    if (draggedRowFirst >= 0 && !ImGui::IsWindowDocked()) {
        // Fade only this window, including its already-drawn background and
        // title bar. Global style alpha would also affect popups and would
        // need last-frame state before Begin(). Keep the active row readable.
        // Docked panels do not cover the picture and share their host's bg.
        auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
        for (int i = 0; i < vertices.Size; ++i) {
            if (i >= draggedRowFirst && i < draggedRowEnd) continue;
            ImU32& colour = vertices[i].col;
            const ImU32 alpha = (colour & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT;
            colour = (colour & ~IM_COL32_A_MASK) |
                (static_cast<ImU32>(alpha * 0.25f) << IM_COL32_A_SHIFT);
        }
    }

    ImGui::End();
}
