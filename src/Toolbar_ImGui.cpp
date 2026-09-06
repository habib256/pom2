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

#include "Toolbar_ImGui.h"

#include "IconsFontAwesome6.h"
#include "Pom2Theme.h"
#include "imgui.h"
// BeginViewportSideBar is declared in imgui_internal.h (it is what
// BeginMainMenuBar itself is built on). We use it so the toolbar reserves its
// height in the viewport work area — see the comment in render(). MainWindow
// already depends on imgui_internal.h for the same family of helpers.
#include "imgui_internal.h"

namespace pom2 {

namespace {

// Convenience: icon + text fallback in one place. When `fa-solid-900.ttf`
// failed to load the icon glyph renders as `?`; the tooltip carries the
// long label either way.
//
// `tint` colours the GLYPH, not the button face. A row of fully coloured
// buttons reads as decoration; a neutral face with a coloured icon reads as
// meaning — and it's what lets power-off (destructive) stop looking exactly
// like single-step (harmless), which was the toolbar's core legibility
// problem. 0 = inherit the normal text colour.
struct Btn {
    const char* icon;
    const char* id;
    const char* tip;
    ImU32       tint = 0;
};

bool iconButton(const Btn& b, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    if (b.tint) ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::ColorConvertU32ToFloat4(b.tint));
    char label[64];
    std::snprintf(label, sizeof(label), "%s##%s", b.icon, b.id);
    const bool clicked = ImGui::Button(label);
    if (b.tint) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    // AllowWhenDisabled: a greyed-out button is exactly when the user needs
    // the tooltip that says WHY, and plain IsItemHovered() never reports a
    // disabled item.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && b.tip)
        ImGui::SetTooltip("%s", b.tip);
    return clicked;
}

// Width for a combo that must never clip its widest entry. The old toolbar
// hardcoded pixel widths (86 / 90 / 110), which broke the moment the theme
// grew FramePadding — "//e PAL" clipped to "//e PA" — and would break again
// at any UI zoom. Measuring the widest label plus the frame chrome makes the
// control self-sizing at every scale.
float comboWidth(const char* widest) {
    return ImGui::CalcTextSize(widest).x
         + ImGui::GetStyle().FramePadding.x * 2.0f
         + ImGui::GetFrameHeight();          // the dropdown arrow button
}

// A toolbar toggle that is currently ON: accent-tinted face + accent glyph,
// so "this mode is active" is legible without hovering for the tooltip.
struct ToggleStyle {
    bool pushed = false;
    void begin(bool on) {
        if (!on) return;
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        pushed = true;
    }
    void end() { if (pushed) { ImGui::PopStyleColor(); pushed = false; } }
};

const char* profileShortLabel(SystemProfile p) {
    switch (p) {
        case SystemProfile::AppleII:             return "][";
        case SystemProfile::AppleIIPlus:         return "][+";
        case SystemProfile::AppleIIeUnenhanced:  return "//e-U";
        case SystemProfile::AppleIIe:            return "//e";
        case SystemProfile::AppleIIc:            return "//c";
        case SystemProfile::AppleIIcPlus:        return "//c+";
        case SystemProfile::AppleIIePAL:         return "//e PAL";
        case SystemProfile::AppleIIcPAL:         return "//c PAL";
        case SystemProfile::AppleIIeUnenhancedPAL: return "//e-U PAL";
    }
    return "??";
}

// Compact label shown inside the toolbar combo when collapsed. Full
// names live in the dropdown rows (e.g. "//e/c — Français (342-0274-A)")
// but those wouldn't fit in the toolbar's ~110 px width.
const char* shortLocaleLabel(CharRomLocale l) {
    switch (l) {
        case CharRomLocale::ProfileDefault:                   return "Default";
        case CharRomLocale::AppleIIClassic:                   return "Classic";
        case CharRomLocale::VidexLowerCase:                   return "Videx LC";
        case CharRomLocale::AppleIIeUS_Enhanced:              return "US";
        case CharRomLocale::AppleIIeUS_Unenhanced:            return "US-U";
        case CharRomLocale::AppleIIeFrench:                   return "FR";
        case CharRomLocale::AppleIIeFrenchCanadian:           return "FR-CA";
        case CharRomLocale::AppleIIeFrenchCanadianUnenhanced: return "FR-CA-U";
        case CharRomLocale::AppleIIeUK_Enhanced:              return "UK";
        case CharRomLocale::AppleIIeUK_Unenhanced:            return "UK-U";
        case CharRomLocale::AppleIIeGerman:                   return "DE";
        case CharRomLocale::AppleIIeGermanImproved:           return "DE+";
        case CharRomLocale::AppleIIeFrench8k_FR:              return "FR-8K";
        case CharRomLocale::AppleIIeFrench8k_US:              return "US-8K";
        case CharRomLocale::AppleIIeFrenchTouchBlock:        return "FT-Blk";
    }
    return "?";
}

} // anon namespace

Toolbar_ImGui::Result Toolbar_ImGui::render(
    float /*unused*/, const Snapshot& snap)
{
    Result r;

    // Pin to the top-left, flush against the menu bar. `WorkPos`
    // already excludes the main menu bar (it's the top-left of the
    // viewport's work area), so we use it as-is — adding a
    // `menuBarHeight` offset on top pushed the toolbar one row too
    // low (fixed 2026-05-15). The `menuBarHeight` parameter is kept
    // for ABI stability in case the caller can't always feed
    // `WorkPos`; ignored here.
    // A viewport side bar rather than a hand-positioned window (changed when
    // docking landed). Two reasons: `BeginViewportSideBar` *reserves* its
    // height in the viewport work area, so the DockSpace below it — and any
    // window that positions from `WorkPos` — automatically starts underneath;
    // and the reservation follows the bar's real height, so a UI zoom that
    // makes the toolbar taller no longer lets the dockspace slide under it.
    //
    // The height has to be computed rather than measured: side bars take
    // their axis size up front. One frame of content = frame height + the
    // window padding we push below.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float barPadY  = 4.0f;
    const float barHeight = ImGui::GetFrameHeight() + barPadY * 2.0f;
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoDocking      |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, barPadY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4.0f, 4.0f));
    if (!ImGui::BeginViewportSideBar("##POM2_Toolbar", vp, ImGuiDir_Up,
                                     barHeight, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return r;
    }

    const Palette& pal = palette();

    // ── Power group ───────────────────────────────────────────────────
    // Power-cycle (cold boot, wipes RAM) + soft reset (Ctrl-Reset).
    // Hard reset is keyboard-only (F12) — was redundant in the toolbar
    // (effectively the same UX as soft reset for most users, the
    // distinction matters for the few users who reach for it).
    //
    // Power-cycle is the only destructive control in this row, so it is the
    // only one wearing the danger colour; reset stays neutral.
    if (iconButton({ ICON_FA_POWER_OFF,    "ColdBoot",
                     "Power-cycle (wipe RAM + cold boot)", pal.danger })) {
        r.requestColdBoot = true;
    }
    ImGui::SameLine();
    if (iconButton({ ICON_FA_ROTATE_LEFT,  "SoftReset",
                     "Reset (Ctrl-Reset / F11)" })) {
        r.requestSoftReset = true;
    }

    verticalRule();

    // ── Run group ─────────────────────────────────────────────────────
    // Rewind sits on the opposite side of Pause from Step: hold it to replay
    // backwards (same gesture as F6 / the Devices ▸ Rewind bar). Enabled only
    // when there's recorded history; iconButton can't report "held", so it's
    // drawn inline to read IsItemActive().
    {
        const bool canRewind = snap.rewindEnabled && snap.rewindHasFrames;
        ImGui::BeginDisabled(!canRewind);
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "%s##RewindHold", ICON_FA_BACKWARD_FAST);
        ImGui::Button(lbl);
        if (ImGui::IsItemActive()) r.requestRewindHeld = true;
        ImGui::EndDisabled();
        // The interesting half of this tooltip is the disabled one ("turn on
        // recording"), which is the half IsItemHovered() could never show.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", canRewind
                ? "Hold to rewind (live) — also F6, or Devices \xe2\x96\xb8 Rewind"
                : "Rewind: turn on recording in Devices \xe2\x96\xb8 Rewind");
        ImGui::SameLine();
    }

    // Running → amber pause glyph ("click to stop"); paused → green play
    // glyph ("click to go"). The colour tracks the ACTION, matching every
    // media transport the user already knows.
    const char* runIcon = snap.isRunning
        ? ICON_FA_CIRCLE_PAUSE : ICON_FA_CIRCLE_PLAY;
    const char* runTip  = snap.isRunning
        ? "Pause (CPU stops at next instruction boundary)"
        : "Run (resume CPU from current PC)";
    if (iconButton({ runIcon, "PauseToggle", runTip,
                     snap.isRunning ? pal.warn : pal.ok })) {
        r.requestPauseToggle = true;
    }
    ImGui::SameLine();
    if (iconButton({ ICON_FA_FORWARD_STEP, "Step",
                     "Single-instruction step (only useful when paused)" },
                   !snap.isRunning)) {
        r.requestStep = true;
    }

    verticalRule();

    // ── Speed selector ───────────────────────────────────────────────
    // Combo: 1× / 2× / 4× / MAX. The current speed sticks to whichever
    // bucket the cyclesPerFrame value rounds into; off-bucket values
    // (the user typed a custom speed) read as
    // "custom" with no checkmark. The buckets follow the active video
    // standard (1× = 17045 @60 Hz NTSC, 20313 @50 Hz PAL) so "1×" is the
    // machine's real clock on both.
    const VideoTiming& vt = pom2VideoTiming(snap.videoStandard);
    const int kSpeed1x  = vt.cyclesPerFrame;
    const int kSpeed2x  = kSpeed1x * 2;
    const int kSpeed4x  = kSpeed1x * 4;
    static constexpr int kSpeedMax = 1'000'000;
    const double mhz1x =
        static_cast<double>(kSpeed1x) * vt.refreshHz / 1e6;
    const char* speedLabel = "Speed";
    if      (snap.cyclesPerFrame == kSpeed1x)  speedLabel = ICON_FA_GAUGE_SIMPLE " 1×";
    else if (snap.cyclesPerFrame == kSpeed2x)  speedLabel = ICON_FA_GAUGE      " 2×";
    else if (snap.cyclesPerFrame == kSpeed4x)  speedLabel = ICON_FA_GAUGE_HIGH " 4×";
    else if (snap.cyclesPerFrame == kSpeedMax) speedLabel = ICON_FA_BOLT       " MAX";
    else                                        speedLabel = ICON_FA_GAUGE      " …";
    // Widest collapsed label is the icon + "MAX".
    ImGui::SetNextItemWidth(comboWidth(ICON_FA_BOLT " MAX"));
    if (ImGui::BeginCombo("##POM2ToolbarSpeed", speedLabel)) {
        char lbl[64];
        std::snprintf(lbl, sizeof lbl,
                      ICON_FA_GAUGE_SIMPLE " 1× (%.2f MHz)", mhz1x);
        if (ImGui::Selectable(lbl, snap.cyclesPerFrame == kSpeed1x))
            r.setCyclesPerFrame = kSpeed1x;
        std::snprintf(lbl, sizeof lbl,
                      ICON_FA_GAUGE       " 2× (%.2f MHz)", mhz1x * 2);
        if (ImGui::Selectable(lbl, snap.cyclesPerFrame == kSpeed2x))
            r.setCyclesPerFrame = kSpeed2x;
        std::snprintf(lbl, sizeof lbl,
                      ICON_FA_GAUGE_HIGH  " 4× (%.2f MHz)", mhz1x * 4);
        if (ImGui::Selectable(lbl, snap.cyclesPerFrame == kSpeed4x))
            r.setCyclesPerFrame = kSpeed4x;
        if (ImGui::Selectable(ICON_FA_BOLT       " MAX (uncapped)",
                              snap.cyclesPerFrame == kSpeedMax))
            r.setCyclesPerFrame = kSpeedMax;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Emulation speed");

    verticalRule();

    // ── Profile selector ─────────────────────────────────────────────
    // Short-label combo. Switching = full cold reset under the hood
    // (the host calls `applyProfile()`), so the user picks rarely.
    char profileBuf[16];
    std::snprintf(profileBuf, sizeof(profileBuf),
                  ICON_FA_COMPUTER " %s", profileShortLabel(snap.activeProfile));
    ImGui::SetNextItemWidth(comboWidth(ICON_FA_COMPUTER " //e-U PAL"));
    if (ImGui::BeginCombo("##POM2ToolbarProfile", profileBuf)) {
        for (SystemProfile p : pom2::allProfiles()) {
            char rowBuf[32];
            std::snprintf(rowBuf, sizeof(rowBuf), "%s  (%s)",
                          profileShortLabel(p),
                          std::string(pom2::profileConfig(p).displayName).c_str());
            if (ImGui::Selectable(rowBuf, snap.activeProfile == p)) {
                r.setProfileRequested = true;
                r.setProfile          = p;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("System profile (cold-reset switch)");

    verticalRule();

    // ── Char ROM (locale) selector ───────────────────────────────────
    // Hot swap: changing the locale calls Memory::loadCharRom(path)
    // immediately — no profile-switch / cold-reset. Apple2Display
    // re-reads `mem.charRom()` on every frame so the new glyph table
    // shows up at the next refresh. The dropdown filters entries by
    // active profile so a II+ user doesn't see a German //e font
    // (which would render as garbage on a 2 KB-expecting profile).
    {
        const CharRomEntry& active = charRomEntry(snap.charRomLocale);
        char buf[40];
        std::snprintf(buf, sizeof(buf), ICON_FA_FONT " %s",
                      snap.charRomLocale == CharRomLocale::ProfileDefault
                          ? "Default"
                          : shortLocaleLabel(snap.charRomLocale));
        ImGui::SetNextItemWidth(comboWidth(ICON_FA_FONT " FR-CA-U"));
        if (ImGui::BeginCombo("##POM2ToolbarCharRom", buf)) {
            for (const auto& e : charRomCatalog()) {
                if (!charRomFitsProfile(e, snap.activeProfile)) continue;
                if (ImGui::Selectable(e.displayName,
                                      snap.charRomLocale == e.locale)) {
                    r.setCharRomRequested = true;
                    r.setCharRomLocale    = e.locale;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            const char* path = active.path[0] ? active.path
                                              : "(profile probe order)";
            ImGui::SetTooltip("Character-generator ROM\n%s\n→ %s",
                              active.displayName, path);
        }
    }

    verticalRule();

    // ── Disk shortcuts ───────────────────────────────────────────────
    // Both insert AND eject-all now live in the Disk Library panel
    // (multi-source picker with boot-on-click + header-row "Eject All")
    // — kept out of the toolbar to avoid two ways to do the same thing.

    // ── Display color / monochrome toggle ────────────────────────────
    // One-click flip between color and B&W phosphor. The host remembers
    // the specific submode on each side (e.g. Mono Green ↔ Color 4-bit).
    // Tinted active when a mono mode is showing.
    {
        ToggleStyle t;
        t.begin(snap.displayIsMono);
        if (iconButton({ ICON_FA_CIRCLE_HALF_STROKE, "MonoColorToggle",
                         snap.displayIsMono
                             ? "Monochrome — click for color"
                             : "Color — click for black & white",
                         snap.displayIsMono ? pal.accent : 0 })) {
            r.requestMonoColorToggle = true;
        }
        t.end();
    }

    verticalRule();

    // ── Tooling ──────────────────────────────────────────────────────
    if (iconButton({ ICON_FA_CAMERA,       "Screenshot",
                     "Save screenshot to ./screenshot_NNN.ppm  (F9)" })) {
        r.requestScreenshot = true;
    }
    ImGui::SameLine();
    {
        // Memory Map Grid is a toggle — accent face + accent glyph while the
        // grid is visible so the state is obvious without hovering.
        ToggleStyle t;
        t.begin(snap.memoryGridVisible);
        if (iconButton({ ICON_FA_BORDER_ALL, "MemGrid",
                         snap.memoryGridVisible
                             ? "MemoryGrid viewer (visible — click to hide)"
                             : "MemoryGrid viewer (click to show)",
                         snap.memoryGridVisible ? pal.accent : 0 })) {
            r.requestMemoryGridToggle = true;
        }
        t.end();
    }
    ImGui::SameLine();
    // Full screen = kiosk. Deliberately NOT a ToggleStyle button: the
    // toolbar is never drawn in kiosk (renderFrame early-outs before the
    // menu bar), so an "on" state is unreachable here and painting one
    // would be a lie. ICON_FA_EXPAND matches the View menu item, which is
    // the same action under a different name.
    if (iconButton({ ICON_FA_EXPAND, "Kiosk",
                     "Full screen (kiosk) — no UI chrome  (Ctrl+Alt+F)" })) {
        r.requestKioskToggle = true;
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
    if (iconButton({ ICON_FA_CIRCLE_INFO, "About",
                     "About POM2" })) {
        r.requestAbout = true;
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    return r;
}

} // namespace pom2
