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

// PaintLayout.h — the layout rules of the Paint editor's window, as three
// small ImGui helpers: a row that WRAPS instead of running off the edge, the
// width an item will take before it is drawn, and a text that ELIDES its
// middle (full text in a tooltip) instead of being cut by the window.
//
// Why they exist (2026-11-28): every row of the editor was a run of
// SameLine() items with pixel sizes fixed for a 1.0 UI scale. At a narrower
// window or a 1.5 zoom the page row, the side panel's button rows, the
// status line and the dialogs' buttons ran past their window and were
// clipped. tests/paint_ui_probe.cpp measures it. Same idea as the Memory
// Heatmap's ButtonFlow (MemoryHeatmap_ImGui_Panel.cpp).
//
// Portable (shared with POM1): ImGui only, header-only.

#ifndef HGRPAINT_PAINT_LAYOUT_H
#define HGRPAINT_PAINT_LAYOUT_H

#include "imgui.h"

#include <string>

namespace hgrpaint {
namespace layout {

/// Width of a button showing `label` at its natural size.
inline float buttonWidth(const char* label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

/// Width of a checkbox and its label.
inline float checkboxWidth(const char* label)
{
    const float t = ImGui::CalcTextSize(label, nullptr, true).x;
    return ImGui::GetFrameHeight() + (t > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + t : 0.0f);
}

/// Lays items out left to right and starts a new line when the next one
/// would cross the right edge of the region it was created in. `keep(w)`
/// before a run of items `w` wide in all keeps them together (a new line
/// when the whole run does not fit where it would start) and leaves a wider
/// gap before it, so groups read as groups.
class Flow {
public:
    Flow()
        : right_(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x),
          sp_(ImGui::GetStyle().ItemSpacing.x) {}

    /// Call before drawing an item `w` wide; true when it went on the same line.
    bool next(float w)
    {
        const float gap = gap_ > 0.0f ? gap_ : sp_;
        gap_ = 0.0f;
        const bool same = !first_ && !breakNext_ && ImGui::GetItemRectMax().x + gap + w <= right_;
        if (same) ImGui::SameLine(0.0f, gap);
        first_ = false;
        breakNext_ = false;
        return same;
    }
    /// The next `w` pixels of items are a group: together, after a wider gap.
    void keep(float w)
    {
        gap_ = sp_ * 2.5f;
        if (!first_ && ImGui::GetItemRectMax().x + gap_ + w > right_) breakNext_ = true;
    }
    /// Room left on the current line after the usual gap.
    float room() const
    {
        if (first_ || breakNext_) return right_ - ImGui::GetCursorScreenPos().x;
        return right_ - (ImGui::GetItemRectMax().x + sp_);
    }
    /// The next item starts a new line.
    void breakLine() { breakNext_ = true; }
    /// Items drawn by another flow come first: the next item continues
    /// after the last of them.
    void follow() { first_ = false; breakNext_ = false; }

private:
    float right_;
    float sp_;
    float gap_ = 0.0f;
    bool first_ = true;
    bool breakNext_ = false;
};

/// `text` shortened to `maxW` pixels by eliding its middle: the head word
/// when there is a short one ("Saved …/pictures/PIC#062000"), else about
/// 40 % of the width from the start, then as much of the tail (a file name)
/// as fits. Cuts only between UTF-8 characters.
inline std::string elideMiddle(const std::string& text, float maxW)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;
    static const char* kDots = "\xE2\x80\xA6";   // U+2026
    const float dotsW = ImGui::CalcTextSize(kDots).x;
    auto cont = [](char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; };
    auto width = [](const std::string& t) { return ImGui::CalcTextSize(t.c_str()).x; };

    std::string head;
    const size_t sp = text.find(' ');
    if (sp != std::string::npos && sp < 24 && width(text.substr(0, sp + 1)) + dotsW < maxW * 0.5f) {
        head = text.substr(0, sp + 1);
    } else {
        size_t n = 0;
        while (n < text.size()) {
            size_t m = n + 1;
            while (m < text.size() && cont(text[m])) ++m;
            if (width(text.substr(0, m)) > maxW * 0.4f - dotsW) break;
            n = m;
        }
        head = text.substr(0, n);
    }
    size_t start = text.size();
    while (start > head.size()) {
        size_t m = start - 1;
        while (m > head.size() && cont(text[m])) --m;
        if (width(head + kDots + text.substr(m)) > maxW) break;
        start = m;
    }
    return head + kDots + text.substr(start);
}

/// A line of text no wider than `maxW`: elided in the middle when it would
/// not fit, the whole text in a tooltip then. Grey when `disabled`.
inline void textFitted(const std::string& text, float maxW, bool disabled = true)
{
    const std::string shown = elideMiddle(text, maxW);
    if (disabled) ImGui::TextDisabled("%s", shown.c_str());
    else          ImGui::TextUnformatted(shown.c_str());
    if (shown != text && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
}

/// A size for a dialog opening at the centre of the screen: `want` (in
/// units of the font size, so it follows the UI zoom), never more than
/// `frac` of the viewport.
inline ImVec2 dialogSize(float wantW, float wantH, float frac = 0.94f)
{
    const float f = ImGui::GetFontSize();
    const ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
    return ImVec2(wantW * f < vp.x * frac ? wantW * f : vp.x * frac,
                  wantH * f < vp.y * frac ? wantH * f : vp.y * frac);
}

} // namespace layout
} // namespace hgrpaint

#endif // HGRPAINT_PAINT_LAYOUT_H
