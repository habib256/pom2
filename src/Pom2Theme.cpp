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

#include "Pom2Theme.h"

#include <algorithm>
#include <cstring>

namespace pom2 {

namespace {

// ── Colour helpers ────────────────────────────────────────────────────────
// Everything is authored as 0xRRGGBB hex so the palette can be read (and
// eyedropped) as a normal design token list rather than as float triples.

constexpr ImVec4 rgb(unsigned hex, float a = 1.0f)
{
    return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((hex >>  8) & 0xFF) / 255.0f,
                  static_cast<float>((hex      ) & 0xFF) / 255.0f,
                  a);
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

ImVec4 withAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// ── Neutral base ramp ─────────────────────────────────────────────────────
// A slightly warm near-black rather than pure grey: pure grey next to the
// Apple II's saturated HGR magenta/orange reads as dirty blue. Every one of
// these is used at alpha 1.0 for window/popup backgrounds.
// Surface hierarchy, darkest to lightest. The ordering carries meaning and
// two constraints must hold: popups sit BELOW frames on this ramp (a popup
// darker than the widgets inside it is what makes a slider track visible in
// a menu — with both at kBg1 the zoom slider in View ▸ Interface had no
// visible track at all), and every one of them is used at alpha 1.0.
constexpr unsigned kBg0     = 0x0B0B0E;   // window background
constexpr unsigned kBg1     = 0x131318;   // popup / dropdown background
constexpr unsigned kBgBar    = 0x16161C;  // menu bar
constexpr unsigned kBg2     = 0x1B1B22;   // raised: frames, buttons, tabs
constexpr unsigned kBg3     = 0x26262F;   // raised + hovered
constexpr unsigned kBg4     = 0x32323D;   // raised + active
constexpr unsigned kBorder  = 0x2E2E38;
constexpr unsigned kText    = 0xDCDCE4;
// ImGui's default TextDisabled is 0.50 grey, which fails against these
// backgrounds — the old status bar used it and was effectively unreadable.
// 0x8A8A99 keeps a clear "secondary" step while staying legible.
constexpr unsigned kTextDim = 0x8A8A99;

// ── Semantic colours ──────────────────────────────────────────────────────
// Shared by toolbar + status bar so "destructive" looks the same everywhere.
constexpr unsigned kOk     = 0x3DD68C;
constexpr unsigned kWarn   = 0xFFB84D;
constexpr unsigned kDanger = 0xFF5C5C;
constexpr unsigned kInfo   = 0x5CA8FF;

struct AccentSpec {
    UiAccent    id;
    const char* key;
    const char* label;
    unsigned    base;    // full-strength accent
    unsigned    hover;   // lighter, for hovered widgets
    unsigned    active;  // darker, for held widgets
};

// Phosphor-derived accents. Amber (#FFB000) is the classic amber CRT; P31 is
// the green phosphor the Apple II monitors actually used.
constexpr AccentSpec kAccents[] = {
    { UiAccent::Amber, "amber", "Amber phosphor",  0xFFB000, 0xFFC63D, 0xD99400 },
    { UiAccent::Green, "green", "Green (P31)",     0x35D96F, 0x5CE88C, 0x27B159 },
    { UiAccent::Blue,  "blue",  "Cold blue",       0x4DA3FF, 0x7DBCFF, 0x2F82D6 },
    { UiAccent::Slate, "slate", "Slate (neutral)", 0x8892A6, 0xA6AFC0, 0x6B7488 },
};
constexpr std::size_t kAccentCount = sizeof(kAccents) / sizeof(kAccents[0]);

const AccentSpec& spec(UiAccent a)
{
    for (const AccentSpec& s : kAccents)
        if (s.id == a) return s;
    return kAccents[0];
}

Palette g_palette = {};
bool    g_paletteValid = false;
// Geometry scale applied by the last applyTheme() (uiScale x dpiScale).
// Kept here rather than read back from ImGuiStyle::_MainScale, which upstream
// explicitly marks as not-for-consumption.
float   g_scale = 1.0f;

} // anon namespace

const char* accentKey(UiAccent a)   { return spec(a).key; }
const char* accentLabel(UiAccent a) { return spec(a).label; }

UiAccent accentFromKey(const char* key)
{
    if (key) {
        for (const AccentSpec& s : kAccents)
            if (std::strcmp(s.key, key) == 0) return s.id;
    }
    return UiAccent::Amber;
}

const UiAccent* allAccents(std::size_t& count)
{
    // Static mirror of kAccents' ids so callers can iterate without seeing
    // the AccentSpec layout.
    static const UiAccent ids[kAccentCount] = {
        UiAccent::Amber, UiAccent::Green, UiAccent::Blue, UiAccent::Slate,
    };
    count = kAccentCount;
    return ids;
}

const Palette& palette()
{
    if (!g_paletteValid) {
        // First-call safety: build the default (Amber) palette without
        // touching the ImGui style, so a panel that queries colours before
        // main() has themed the context still gets sane values.
        const AccentSpec& s = spec(UiAccent::Amber);
        g_palette = Palette{
            ImGui::ColorConvertFloat4ToU32(rgb(s.base)),
            ImGui::ColorConvertFloat4ToU32(withAlpha(rgb(s.base), 0.55f)),
            ImGui::ColorConvertFloat4ToU32(rgb(kOk)),
            ImGui::ColorConvertFloat4ToU32(rgb(kWarn)),
            ImGui::ColorConvertFloat4ToU32(rgb(kDanger)),
            ImGui::ColorConvertFloat4ToU32(rgb(kInfo)),
            ImGui::ColorConvertFloat4ToU32(rgb(kText)),
            ImGui::ColorConvertFloat4ToU32(rgb(kTextDim)),
            ImGui::ColorConvertFloat4ToU32(rgb(0x3A3A45)),
            ImGui::ColorConvertFloat4ToU32(rgb(kBorder)),
        };
        g_paletteValid = true;
    }
    return g_palette;
}

void verticalRule(float padX)
{
    const float scaled = padX * g_scale;
    ImGui::SameLine(0.0f, scaled);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  h = ImGui::GetTextLineHeight();
    // Inset top and bottom so the rule reads as a light divider rather than
    // a full-height border competing with the window frame.
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.12f),
                                        ImVec2(p.x, p.y + h * 0.88f),
                                        palette().separator, 1.0f);
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine(0.0f, scaled);
}

void indicatorDot(bool on, ImU32 onColor, float radius, float lineHeight)
{
    const float  r = radius * g_scale;
    const float  h = lineHeight > 0.0f ? lineHeight
                                       : ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 centre(p.x + r, p.y + h * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (on) {
        // Faint halo so an active LED reads at a glance in peripheral
        // vision — the whole point of a drive light.
        dl->AddCircleFilled(centre, r * 2.0f,
                            (onColor & 0x00FFFFFF) | 0x30000000);
    }
    dl->AddCircleFilled(centre, r, on ? onColor : palette().ledOff);
    ImGui::Dummy(ImVec2(r * 2.0f, h));
    ImGui::SameLine();
}

void applyTheme(UiAccent accent, float uiScale, float dpiScale)
{
    const AccentSpec& acc = spec(accent);

    const ImVec4 accentBase   = rgb(acc.base);
    const ImVec4 accentHover  = rgb(acc.hover);
    const ImVec4 accentActive = rgb(acc.active);
    const ImVec4 bg0 = rgb(kBg0), bg2 = rgb(kBg2), bg3 = rgb(kBg3);

    // Rebuild from a pristine style — see the header's scaling contract:
    // ScaleAllSizes() is cumulative, so re-theming a live style would
    // compound the padding on every call.
    ImGuiStyle s = ImGuiStyle();

    // ── Geometry ──────────────────────────────────────────────────────────
    // Authored at 1.0; the DPI/zoom product is applied once at the bottom.
    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.PopupRounding     = 5.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 5.0f;

    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBarBorderSize  = 1.0f;

    s.WindowPadding     = ImVec2(10.0f, 8.0f);
    s.FramePadding      = ImVec2(8.0f, 4.0f);
    s.CellPadding       = ImVec2(6.0f, 3.0f);
    s.ItemSpacing       = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    s.IndentSpacing     = 20.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;

    s.WindowTitleAlign  = ImVec2(0.02f, 0.5f);
    s.ButtonTextAlign   = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextAlign      = ImVec2(0.0f, 0.5f);
    s.SeparatorTextPadding    = ImVec2(16.0f, 4.0f);

    // Anti-aliased everything: the rounded corners above look ragged without
    // it and the cost is negligible next to the emulated CRT shader.
    s.AntiAliasedLines       = true;
    s.AntiAliasedLinesUseTex = true;
    s.AntiAliasedFill        = true;

    // ── Colours ───────────────────────────────────────────────────────────
    ImVec4* c = s.Colors;

    c[ImGuiCol_Text]          = rgb(kText);
    c[ImGuiCol_TextDisabled]  = rgb(kTextDim);

    // OPAQUE. This is the whole point of the module — see the header.
    c[ImGuiCol_WindowBg]      = bg0;
    c[ImGuiCol_ChildBg]       = ImVec4(0, 0, 0, 0);   // inherit, no double-fill
    c[ImGuiCol_PopupBg]       = rgb(kBg1);            // menus over a live game

    c[ImGuiCol_Border]        = rgb(kBorder);
    c[ImGuiCol_BorderShadow]  = ImVec4(0, 0, 0, 0);

    // Frames read as RAISED (lighter than both the window and any popup they
    // appear in), matching buttons — one "interactive surface" step.
    c[ImGuiCol_FrameBg]        = bg2;
    c[ImGuiCol_FrameBgHovered] = bg3;
    c[ImGuiCol_FrameBgActive]  = rgb(kBg4);

    // The focused window's title carries an accent tint so "which panel am I
    // typing into" is answerable at a glance with 30-odd panels open.
    c[ImGuiCol_TitleBg]          = rgb(kBg1);
    c[ImGuiCol_TitleBgActive]    = mix(bg2, accentBase, 0.22f);
    c[ImGuiCol_TitleBgCollapsed] = rgb(kBg1);
    c[ImGuiCol_MenuBarBg]        = rgb(kBgBar);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = rgb(0x33333D);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x44444F);
    c[ImGuiCol_ScrollbarGrabActive]  = withAlpha(accentBase, 0.75f);

    c[ImGuiCol_CheckMark]        = accentBase;
    c[ImGuiCol_SliderGrab]       = withAlpha(accentBase, 0.85f);
    c[ImGuiCol_SliderGrabActive] = accentHover;

    // Buttons stay neutral; accent is reserved for state (checked, selected,
    // active) so an accented control always means something is *on*.
    c[ImGuiCol_Button]        = bg2;
    c[ImGuiCol_ButtonHovered] = bg3;
    c[ImGuiCol_ButtonActive]  = mix(bg3, accentActive, 0.45f);

    c[ImGuiCol_Header]        = withAlpha(accentBase, 0.22f);
    c[ImGuiCol_HeaderHovered] = withAlpha(accentBase, 0.34f);
    c[ImGuiCol_HeaderActive]  = withAlpha(accentBase, 0.48f);

    c[ImGuiCol_Separator]        = rgb(kBorder);
    c[ImGuiCol_SeparatorHovered] = withAlpha(accentBase, 0.60f);
    c[ImGuiCol_SeparatorActive]  = accentBase;

    c[ImGuiCol_ResizeGrip]        = withAlpha(accentBase, 0.16f);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(accentBase, 0.45f);
    c[ImGuiCol_ResizeGripActive]  = withAlpha(accentBase, 0.75f);

    c[ImGuiCol_InputTextCursor] = accentBase;

    c[ImGuiCol_Tab]                        = rgb(kBg1);
    c[ImGuiCol_TabHovered]                 = mix(bg2, accentBase, 0.30f);
    c[ImGuiCol_TabSelected]                = mix(bg3, accentBase, 0.20f);
    c[ImGuiCol_TabSelectedOverline]        = accentBase;
    c[ImGuiCol_TabDimmed]                  = rgb(kBg1);
    c[ImGuiCol_TabDimmedSelected]          = rgb(kBg2);
    c[ImGuiCol_TabDimmedSelectedOverline]  = withAlpha(accentBase, 0.35f);

    c[ImGuiCol_PlotLines]            = withAlpha(accentBase, 0.85f);
    c[ImGuiCol_PlotLinesHovered]     = accentHover;
    c[ImGuiCol_PlotHistogram]        = withAlpha(accentBase, 0.85f);
    c[ImGuiCol_PlotHistogramHovered] = accentHover;

    c[ImGuiCol_TableHeaderBg]     = rgb(kBg2);
    c[ImGuiCol_TableBorderStrong] = rgb(0x353540);
    c[ImGuiCol_TableBorderLight]  = rgb(0x24242C);
    c[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]     = ImVec4(1, 1, 1, 0.025f);

    c[ImGuiCol_TextLink]       = accentHover;
    c[ImGuiCol_TextSelectedBg] = withAlpha(accentBase, 0.30f);
    c[ImGuiCol_TreeLines]      = rgb(0x3A3A45);

    c[ImGuiCol_DragDropTarget]   = accentHover;
    c[ImGuiCol_DragDropTargetBg] = withAlpha(accentBase, 0.15f);
    c[ImGuiCol_UnsavedMarker]    = rgb(kWarn);

    c[ImGuiCol_NavCursor]            = accentBase;
    c[ImGuiCol_NavWindowingHighlight]= withAlpha(accentBase, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.02f, 0.02f, 0.03f, 0.60f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.02f, 0.02f, 0.03f, 0.70f);

    // ── Scale ─────────────────────────────────────────────────────────────
    // Geometry scales by the full product; fonts get the two factors
    // separately (ImGui multiplies them at draw time — no atlas rebuild).
    const float ui  = std::clamp(uiScale,  kUiScaleMin, kUiScaleMax);
    const float dpi = std::clamp(dpiScale, 0.5f, 4.0f);
    s.ScaleAllSizes(ui * dpi);
    g_scale = ui * dpi;
    s.FontScaleMain = ui;
    s.FontScaleDpi  = dpi;

    // Whole-style replacement — see the header's "call it between frames"
    // note for what a mid-frame call costs (one frame of a possibly stale
    // Push/Pop pair) and why POM2 accepts it.
    ImGui::GetStyle() = s;

    // Publish the semantic palette for the toolbar / status bar.
    g_palette = Palette{
        ImGui::ColorConvertFloat4ToU32(accentBase),
        ImGui::ColorConvertFloat4ToU32(withAlpha(accentBase, 0.55f)),
        ImGui::ColorConvertFloat4ToU32(rgb(kOk)),
        ImGui::ColorConvertFloat4ToU32(rgb(kWarn)),
        ImGui::ColorConvertFloat4ToU32(rgb(kDanger)),
        ImGui::ColorConvertFloat4ToU32(rgb(kInfo)),
        ImGui::ColorConvertFloat4ToU32(rgb(kText)),
        ImGui::ColorConvertFloat4ToU32(rgb(kTextDim)),
        ImGui::ColorConvertFloat4ToU32(rgb(0x3A3A45)),
        ImGui::ColorConvertFloat4ToU32(rgb(kBorder)),
    };
    g_paletteValid = true;
}

} // namespace pom2
