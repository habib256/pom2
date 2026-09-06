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

// Pom2Theme — the single owner of POM2's Dear ImGui look: colour palette,
// widget geometry, and UI scaling (user zoom × monitor DPI).
//
// Before this module the app ran on bare `ImGui::StyleColorsDark()`, which
// leaves `WindowBg` at alpha 0.94. On a black boot screen that's invisible;
// over a running HGR game every panel turns semi-transparent and the text
// underneath bleeds through (CRT Settings sliders read on top of the Disk
// Library rows). Every background here is fully opaque for that reason —
// legibility over a moving 6-colour image is the hard constraint, not a
// preference.
//
// Scaling contract
// ----------------
// `applyTheme()` rebuilds the whole `ImGuiStyle` from a default-constructed
// one on every call. That is deliberate: `ImGuiStyle::ScaleAllSizes()` is
// *cumulative* (it multiplies the live values and folds the factor into
// `_MainScale`), so applying it twice on a live style compounds — padding
// doubles, then quadruples. Rebuilding from scratch makes the call
// idempotent for a given (accent, uiScale, dpiScale) triple, which is what
// lets the View ▸ Interface zoom control re-apply the theme every time the
// user nudges the slider.
//
// Fonts are NOT re-rasterised: Dear ImGui 1.92's dynamic font system scales
// through `style.FontScaleMain` (user zoom) and `style.FontScaleDpi`
// (monitor scale) at draw time, so no atlas rebuild / backend texture
// refresh is needed when the scale changes mid-session.

#ifndef POM2_POM2THEME_H
#define POM2_POM2THEME_H

#include "imgui.h"

#include <cstddef>

namespace pom2 {

/// Accent hue. The three options are phosphor colours rather than arbitrary
/// UI hues — POM2 is an Apple II emulator and the chrome should read as one.
enum class UiAccent {
    Amber = 0,   ///< Amber phosphor (default) — warm, matches the ][ era.
    Green,       ///< P31 green phosphor.
    Blue,        ///< Cold blue, for users who want neutral chrome.
    Slate,       ///< Desaturated grey-blue: no hue at all, maximum neutrality.
};

/// Stable persistence keys (written to `state.cfg` as `ui_accent`).
const char* accentKey  (UiAccent a);
/// Human label for menus.
const char* accentLabel(UiAccent a);
/// Parse a persisted key back to an accent; unknown → Amber.
UiAccent    accentFromKey(const char* key);

/// All accents, in menu order.
const UiAccent* allAccents(std::size_t& count);

/// Semantic colours shared by the toolbar, the status bar and any panel that
/// needs to say "good / careful / destructive" without inventing its own
/// literals. Kept as ImU32 because that is what `ImDrawList` wants; use
/// `ImGui::ColorConvertU32ToFloat4` for `PushStyleColor`.
struct Palette {
    ImU32 accent;      ///< Active accent at full strength.
    ImU32 accentDim;   ///< Accent at ~55 % — idle/unfocused variants.
    ImU32 ok;          ///< Green: running, healthy, at speed.
    ImU32 warn;        ///< Amber: attention, throttled, unsaved.
    ImU32 danger;      ///< Red: destructive (power-cycle), error.
    ImU32 info;        ///< Blue: neutral informational.
    ImU32 text;        ///< Primary text.
    ImU32 textDim;     ///< Secondary text — still readable (unlike ImGui's default).
    ImU32 ledOff;      ///< Inactive indicator dot.
    ImU32 separator;   ///< Toolbar / status-bar rule colour.
};

/// The palette matching the last `applyTheme()` call. Valid after the first
/// call; before that it returns the Amber palette so early callers are safe.
const Palette& palette();

/// Apply POM2's style to the current ImGui context.
///
/// @param accent    accent hue (persisted as `ui_accent`).
/// @param uiScale   user zoom, 1.0 = 100 % (persisted as `ui_scale`).
/// @param dpiScale  monitor content scale from the windowing system
///                  (`glfwGetWindowContentScale`), 1.0 on a non-HiDPI display.
///
/// Geometry is scaled by `uiScale * dpiScale`; fonts by the same product,
/// applied as two separate ImGui factors so a future per-monitor DPI change
/// only has to touch `FontScaleDpi`.
///
/// **Call it between frames.** The implementation replaces the WHOLE
/// `ImGuiStyle` (`ImGui::GetStyle() = s`), which is only safe outside a
/// NewFrame()/Render() bracket: inside one, every `PushStyleVar`/
/// `PushStyleColor` still on the stack holds a copy of the OLD style's value
/// and pops it back into the NEW style, so a theme or zoom change made from a
/// menu handler mid-frame can leave a stray padding or colour behind for the
/// rest of that frame. Nothing worse — the next frame is drawn entirely from
/// the new style — which is why POM2 accepts the mid-frame calls it has today
/// (Interface menu, Ctrl+scroll zoom, a DPI change reported by GLFW) rather
/// than queueing them; if that ever stops being acceptable, the fix is a
/// pending-theme slot applied right after `ImGui::Render()`, not a change
/// here.
void applyTheme(UiAccent accent, float uiScale, float dpiScale);

// ── Shared chrome primitives ──────────────────────────────────────────────
// Small drawing helpers used by both the toolbar and the status bar, so the
// two rows speak the same visual language. They live here rather than in
// either caller because they encode theme decisions (rule colour, LED size),
// not layout.

/// Draw a real vertical rule at the cursor and advance past it. Replaces the
/// literal "|" text characters the toolbar used to separate its groups —
/// those inherited the text colour and baseline, so they read as content
/// rather than as structure.
///
/// Must be called between two same-line items; it handles its own
/// `SameLine` spacing on both sides.
void verticalRule(float padX = 6.0f);

/// Filled indicator dot, vertically centred on the current text line and
/// followed by `SameLine`. `on` picks between the given colour and the
/// palette's inactive grey, so a caller can pass one colour and let the
/// helper handle the off state.
///
/// NOT called `statusLed`: `StatusLed.h` already owns a `pom2::statusLed` for
/// *media* status (empty / ok / write-protected / error, with its own colour
/// table and tooltips). Two same-named functions in one namespace with
/// different meanings is a trap for whoever writes the next call, even though
/// overload resolution happens to pick correctly today.
/// `lineHeight` is the vertical span the dot centres itself on; 0 means the
/// text line. Pass `ImGui::GetFrameHeight()` inside a menu bar: plain items
/// there sit at the TOP of the bar (ImGui applies no frame padding to a bare
/// `Text()`), so a dot centred on the text line rides visibly high against
/// the bar it shares. Text gets away with that; a circle does not.
void indicatorDot(bool on, ImU32 onColor, float radius = 4.0f,
                  float lineHeight = 0.0f);

/// Clamp bounds for the user zoom control — shared by the menu widget and the
/// settings loader so a hand-edited `state.cfg` can't produce an unusable UI.
inline constexpr float kUiScaleMin  = 0.75f;
inline constexpr float kUiScaleMax  = 2.50f;
inline constexpr float kUiScaleStep = 0.05f;

} // namespace pom2

#endif // POM2_POM2THEME_H
