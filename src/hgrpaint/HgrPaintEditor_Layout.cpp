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

// HgrPaintEditor_Layout.cpp — the Paint editor's window: what goes where.
//
//   ┌─ page row ─ Monitor · HGR HGR2 GR GR2 DHGR … ─────────────────────────┐
//   │  toolbar ── Load Import Save PNG · undo redo clear · zoom · View · ?  │
//   ├───────┬───────────────────────────────────────────────────────────────┤
//   │ tools │                 drawing canvas                                │
//   │ tool's│                                                               │
//   │options│                                                               │
//   │ nav.  │                                                               │
//   ├───────┴───────────────────────────────────────────────────────────────┤
//   │ colour palette                                                        │
//   │ status line: x y byte · tool · colour · page · zoom · undo / redo     │
//   └───────────────────────────────────────────────────────────────────────┘
//
// Every row WRAPS to the window's width (layout::Flow) and every size is in
// units of the font, so the window works from 800x560 at a 1.5 UI zoom up:
// tests/paint_ui_probe.cpp draws it at four sizes and two zooms and fails
// when anything is clipped. The display options (grid, colour pipeline,
// aspect, flipbook, ghost, onion skin) live in the toolbar's View menu, as
// in the Memory Heatmap; the side panel keeps only the tools, the current
// tool's options and the navigator, so it rarely needs to scroll.
//
// Portable (shared with POM1): ImGui and the host seam only.

#include "HgrPaintEditor.h"
#include "HgrPaintEditorTables.h"
#include "PaintLayout.h"

#include "imgui.h"
#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

using namespace hgrpaint::tables;
using hgrpaint::HgrColor;
using hgrpaint::layout::Flow;
using hgrpaint::layout::buttonWidth;
using hgrpaint::layout::checkboxWidth;

namespace {

const ImU32 kSelTint   = IM_COL32(58, 96, 150, 255);
const ImU32 kSelHover  = IM_COL32(78, 116, 170, 255);
const ImU32 kSelBorder = IM_COL32(255, 220, 60, 255);

// A button at its natural size in a wrapping row, with its tooltip.
bool flowButton(Flow& row, const char* label, const char* tip)
{
    row.next(buttonWidth(label));
    const bool clicked = ImGui::Button(label);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return clicked;
}

bool flowCheckbox(Flow& row, const char* label, bool* v, const char* tip)
{
    row.next(checkboxWidth(label));
    const bool changed = ImGui::Checkbox(label, v);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return changed;
}

float groupWidth(std::initializer_list<float> widths)
{
    float w = 0.0f;
    for (float x : widths) w += x;
    return w + ImGui::GetStyle().ItemSpacing.x * static_cast<float>(widths.size() - 1);
}

const char* const kHelp =
    "Mouse\n"
    "  Left drag    draw with the current tool / colour\n"
    "  Right drag   quick-erase (paint black) - no tool switch\n"
    "  Middle drag  pan the canvas\n"
    "  Wheel        zoom, anchored on the cursor\n"
    "  Pinch        zoom with two fingers (touchpad / touch screen)\n"
    "  Alt + Left   eyedropper (pick colour)\n"
    "  Shift        constrain Line to 0/45/90 deg, Rect/Ellipse to a square\n"
    "\n"
    "Keys\n"
    "  P E L R O F I S M T   pencil eraser line rect ellipse fill eyedrop select palette text\n"
    "  1-6 colours   [ ] thickness   +/- zoom   G grid   X toggle filled (rect/ellipse)\n"
    "  Ctrl+Z / Ctrl+Y undo/redo   Ctrl+C/X/V copy/cut/paste";

} // namespace

// ── Top: the page row, then the toolbar, in ONE wrapping flow ─────────────

void hgrpaint::HgrPaintEditor::renderTopBar()
{
    // One flow: on a wide window the pages and the toolbar share a line;
    // narrower, each group wraps as a whole.
    Flow row;
    renderPageButtons(row);   // the pages + Monitor (HgrPaintEditor_Monitor.cpp)
    renderFileRow(row);

    // Edit: undo, redo, clear page (undoable too).
    const float ib = buttonWidth(ICON_FA_TRASH_CAN);
    row.keep(groupWidth({ib, ib, ib}));
    if (flowButton(row, ICON_FA_ROTATE_LEFT, "Undo (Ctrl+Z)")) doUndo();
    if (flowButton(row, ICON_FA_ROTATE_RIGHT, "Redo (Ctrl+Y, Ctrl+Shift+Z)")) doRedo();
    if (flowButton(row, ICON_FA_TRASH_CAN, "Clear page (Undo brings it back)")) clearPage();

    // Zoom: out, the rung, in, fit.
    char rung[8];
    std::snprintf(rung, sizeof rung, "%dx", kZoomLadder[zoomIdx]);
    const float rungW = ImGui::CalcTextSize("16x").x;
    row.keep(groupWidth({ib, rungW, ib, ib}));
    if (flowButton(row, ICON_FA_MAGNIFYING_GLASS_MINUS, "Zoom out (-, wheel, pinch)"))
        zoomIdx = std::max(zoomIdx - 1, 0);
    row.next(rungW);
    ImGui::AlignTextToFramePadding();
    {
        // Centred in the width of the widest rung, so the row does not jitter.
        const float pad = (rungW - ImGui::CalcTextSize(rung).x) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
        ImGui::TextUnformatted(rung);
    }
    if (flowButton(row, ICON_FA_MAGNIFYING_GLASS_PLUS, "Zoom in (+, wheel, pinch)"))
        zoomIdx = std::min(zoomIdx + 1, kZoomLadderCount - 1);
    if (flowButton(row, ICON_FA_EXPAND, "Fit the page to the window")) wantFit = true;

    // View menu + help.
    const char* viewLabel = ICON_FA_SLIDERS " View";
    const float helpW = ImGui::CalcTextSize("(?)").x;
    row.keep(groupWidth({buttonWidth(viewLabel), helpW}));
    if (flowButton(row, viewLabel,
                   "Grid, palette seams, colour pipeline, 4:3 aspect,\n"
                   "flipbook, ghost of the other page, onion skin."))
        ImGui::OpenPopup("##paintview");
    renderViewPopup();
    row.next(helpW);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kHelp);

    // The last file operation's message: the rest of the line, elided in the
    // middle (whole in its tooltip), or a line of its own when little is left.
    if (!status.empty()) {
        if (row.room() < ImGui::GetFontSize() * 14.0f) row.breakLine();
        const float w = std::min(row.room(), ImGui::CalcTextSize(status.c_str()).x);
        row.next(w);
        ImGui::AlignTextToFramePadding();
        layout::textFitted(status, w);
    }
}

void hgrpaint::HgrPaintEditor::renderFileRow(layout::Flow& row)
{
    const char* kLoad = ICON_FA_FOLDER_OPEN " Load";
    const char* kImport = ICON_FA_IMAGE " Import";
    const char* kSave = ICON_FA_FLOPPY_DISK " Save";
    const char* kPng = ICON_FA_FILE_EXPORT " Save PNG";
    row.keep(groupWidth({buttonWidth(kLoad), buttonWidth(kImport), buttonWidth(kSave), buttonWidth(kPng)}));
    if (flowButton(row, kLoad, dhgrMode
            ? "Open a file picker and load a raw 16 KB DHGR (A2FC) image"
            : "Open a file picker and load a raw 8 KB HGR image"))
        openFileBrowser(false);
    if (flowButton(row, kImport, dhgrMode
            ? "Import a PNG/JPG picture and convert it to DHGR\n"
              "(140x192, quantised to the 16 Apple II colours with CAM16-UCS dithering)"
            : grMode
            ? "Import a PNG/JPG picture and convert it to GR (lo-res)\n"
              "(40x48 blocks, quantised to the 16 lo-res colours with CAM16-UCS dithering)"
            : "Import a PNG/JPG picture and convert it to HGR\n"
              "(ii-pix-style: CAM16-UCS perceptual dithering vs the true NTSC colours)"))
        openFileBrowser(false, 0, /*importMode=*/true);
    if (flowButton(row, kSave, "Save the page as a raw dump (NAME#06AAAA, BLOAD-able)"))
        openFileBrowser(true, /*kind=*/0);
    if (flowButton(row, kPng, "Export the canvas as a PNG picture"))
        openFileBrowser(true, /*kind=*/1);
}

void hgrpaint::HgrPaintEditor::renderViewPopup()
{
    if (!ImGui::BeginPopup("##paintview")) return;
    const float sliderW = ImGui::GetFontSize() * 13.0f;

    ImGui::SeparatorText("Canvas");
    ImGui::Checkbox("Grid", &showGrid);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pixel grid over the canvas (G).");
    ImGui::SameLine();
    ImGui::Checkbox("Seams", &showConflicts);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(dhgrMode
            ? "Outline pixels whose rendered dots deviate from their block\n"
              "colour (NTSC fringing at colour transitions)."
            : "Mark byte pairs that disagree on the palette bit (NTSC bleed).");
    ImGui::SameLine();
    ImGui::Checkbox("4:3", &aspect43);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Aspect-correct display (the Apple II filled a 4:3 CRT).");
    ImGui::Checkbox("NTSC colour", &ntscColor);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Colour through the pipeline below; untick for the monochrome preview.");
    // Colour pipeline (when the host offers a choice). The mono preview
    // bypasses it, so it is greyed out then rather than hidden.
    if (host) {
        const auto pipes = host->canvasPipelines();
        if (pipes.size() > 1) {
            float w = 0.0f;
            for (const std::string& p : pipes) w = std::max(w, ImGui::CalcTextSize(p.c_str()).x);
            const int cur = host->canvasPipeline();
            ImGui::BeginDisabled(!ntscColor);
            ImGui::SetNextItemWidth(w + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f);
            if (ImGui::BeginCombo("##canvaspipe",
                                  pipes[static_cast<size_t>(cur) < pipes.size() ? cur : 0].c_str())) {
                for (int i = 0; i < static_cast<int>(pipes.size()); ++i)
                    if (ImGui::Selectable(pipes[i].c_str(), i == cur))
                        host->setCanvasPipeline(i);
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Colour decode pipeline the canvas (and the\n"
                                  "import preview) renders through.");
        }
    }

    // Flipbook (double-buffer animation preview: page 1 <-> page 2) and the
    // ghost of the other page.
    ImGui::SeparatorText("Animation");
    ImGui::Checkbox("Flip", &flipShow);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Alternate the canvas between page 1 and page 2 at the\n"
                          "chosen rate - previews double-buffered animation.\n"
                          "Drawing still targets the selected page.");
    ImGui::SameLine();
    ImGui::Checkbox("Ghost", &ghostOther);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Overlay the OTHER page at low opacity while drawing\n"
                          "(classic animation onion-skinning between frames).");
    if (flipShow) {
        ImGui::SetNextItemWidth(sliderW);
        ImGui::SliderFloat("##fliphz", &flipHz, 1.0f, 30.0f, "Flip %.0f Hz");
    }
    if (ghostOther && !flipShow) {
        ImGui::SetNextItemWidth(sliderW);
        ImGui::SliderFloat("##ghosta", &ghostAlpha, 0.05f, 0.9f, "Ghost %.2f");
    }

    // Onion skin (armed from the import preview's "Onion skin").
    ImGui::SeparatorText("Onion skin");
    if (onionTex) {
        ImGui::Checkbox("Show", &onionShow);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tracing overlay of the last imported source.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove##onion")) {
            if (host) host->destroyTexture(onionTex);
            onionTex = nullptr;
            onionShow = false;
        }
        if (onionShow) {
            ImGui::SetNextItemWidth(sliderW);
            ImGui::SliderFloat("##onionalpha", &onionAlpha, 0.05f, 0.95f, "Onion %.2f");
        }
    } else {
        ImGui::TextDisabled("Import a picture, then \"Onion skin\".");
    }
    ImGui::EndPopup();
}

// ── Left: tools, the tool's options, the navigator ────────────────────────

float hgrpaint::HgrPaintEditor::toolPanelWidth() const
{
    // Three tool buttons a row (or Copy / Cut / Paste, if wider), the
    // panel's padding, and room for its scroll bar so the grid does not
    // reflow when one appears. All in font units: it follows the UI zoom.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float btn = std::round(ImGui::GetFrameHeight() * 1.45f);
    const float inner = std::max(3.0f * btn + 2.0f * st.ItemSpacing.x,
                                 groupWidth({buttonWidth("Copy"), buttonWidth("Cut"), buttonWidth("Paste")}));
    return inner + 2.0f * st.WindowPadding.x + st.ScrollbarSize;
}

void hgrpaint::HgrPaintEditor::renderToolPanel()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Tool palette: icon buttons, as many a row as fit ───────────────────
    const char* toolTips[] = {
        "Pencil (P)", "Eraser (E)", "Line (L)", "Rectangle (R)", "Ellipse (O)",
        "Fill (F)", "Eyedropper (I)", "Select (S)", "Palette shift (M)", "Text (T)" };
    const float btn = std::round(ImGui::GetFrameHeight() * 1.45f);
    {
        Flow tools;
        for (int i = 0; i < kToolCount; ++i) {
            tools.next(btn);
            const bool sel = (static_cast<int>(tool) == i);
            ImGui::PushID(i);
            if (sel) {
                ImGui::PushStyleColor(ImGuiCol_Button, kSelTint);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSelHover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSelHover);
            }
            if (ImGui::Button(kToolIcons[i], ImVec2(btn, btn))) { prevTool = tool; tool = static_cast<Tool>(i); }
            if (sel) ImGui::PopStyleColor(3);
            if (sel) dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kSelBorder, 0, 0, 2.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", toolTips[i]);
            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // ── Tool options (contextual) ──────────────────────────────────────────
    // Stroke thickness / brush size — only the tools that stamp the brush use
    // it (Pencil, Eraser, Line, Rectangle, Ellipse).
    const bool usesThickness = (tool == Tool::Pencil || tool == Tool::Eraser ||
                                tool == Tool::Line || tool == Tool::Rectangle ||
                                tool == Tool::Ellipse);
    if (usesThickness) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##thickness", &brushSize, 1, 7, "Size %d px");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stroke thickness / brush size  ([ and ])");
    }
    if (tool == Tool::Rectangle || tool == Tool::Ellipse) {
        ImGui::Checkbox("Filled", &rectFilled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Filled shape (X)");
    }
    // ── Fill-pattern strip (MacPaint) for the pattern-aware tools ──────────
    const bool usesPattern = (tool == Tool::Pencil || tool == Tool::Line ||
                              tool == Tool::Rectangle || tool == Tool::Ellipse ||
                              tool == Tool::Fill);
    if (usesPattern) {
        const float sw = std::floor(ImGui::GetFrameHeight() * 0.87f);   // 8x8 pattern swatch
        const float cell = sw / 8.0f;
        Flow pats;
        for (int i = 0; i < kPatternCount; ++i) {
            pats.next(sw);
            ImGui::PushID(400 + i);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##pat", ImVec2(sw, sw)))
                patternIdx = i;
            const ImU32 fg = IM_COL32(230, 230, 230, 255);
            const ImU32 bg = IM_COL32(25, 25, 25, 255);
            dl->AddRectFilled(p0, ImVec2(p0.x + sw, p0.y + sw), bg);
            for (int yy = 0; yy < 8; ++yy)
                for (int xx = 0; xx < 8; ++xx)
                    if ((kPatterns[i].rows[yy] >> xx) & 1)
                        dl->AddRectFilled(ImVec2(p0.x + xx * cell, p0.y + yy * cell),
                                          ImVec2(p0.x + (xx + 1) * cell, p0.y + (yy + 1) * cell), fg);
            dl->AddRect(p0, ImVec2(p0.x + sw, p0.y + sw),
                        i == patternIdx ? kSelBorder : IM_COL32(90, 90, 90, 255),
                        0, 0, i == patternIdx ? 2.0f : 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s pattern%s", kPatterns[i].name,
                                  i == 0 ? "" : " (bits paint the colour, gaps paint black)");
            ImGui::PopID();
        }
    }

    // Mirror symmetry — brush/shape plots repeat about the enabled axes
    // (region ops — paste, text, fill — deliberately don't). Fill never routes
    // through applyPlot, so it cannot mirror: offering the toggles there was a
    // UI lie (bug hunt #11). Brush tools only.
    if (usesThickness) {
        Flow sym;
        flowCheckbox(sym, "SymX", &mirrorX, "Mirror drawing about the vertical axis");
        flowCheckbox(sym, "SymY", &mirrorY, "Mirror drawing about the horizontal axis");
    }
    if (tool == Tool::PaletteShift) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo("##msb", &paletteMsbMode, "MSB: clear\0MSB: set\0MSB: toggle\0");
    }
    if (tool == Tool::Select) {
        Flow sel;
        if (flowButton(sel, "Copy", "Copy the selection (Ctrl+C)")) {
            flushOpenStroke();
            copySelection(false);
        }
        if (flowButton(sel, "Cut", "Cut the selection (Ctrl+X)")) {
            flushOpenStroke();
            copySelection(true);
        }
        if (flowButton(sel, "Paste", "Paste the clip, then place it (Ctrl+V)") && clipUsableHere()) {
            flushOpenStroke();
            pasting = true; pasteX = std::min(selX0, selX1); pasteY = std::min(selY0, selY1);
        }
        // Clip transforms (apply to the clipboard content; paste to commit).
        if (clip.w > 0) {
            sel.keep(groupWidth({buttonWidth("FlipH"), buttonWidth("FlipV"), buttonWidth("Rot")}));
            auto at = [&](int x, int y) { return static_cast<size_t>(y) * clip.w + x; };
            if (flowButton(sel, "FlipH", "Flip the clip left-right")) {
                for (int y = 0; y < clip.h; ++y)
                    for (int x = 0; x < clip.w / 2; ++x) {
                        if (clip.sixteen) std::swap(clip.idx[at(x, y)], clip.idx[at(clip.w - 1 - x, y)]);
                        else              std::swap(clip.px [at(x, y)], clip.px [at(clip.w - 1 - x, y)]);
                    }
            }
            if (flowButton(sel, "FlipV", "Flip the clip upside down")) {
                for (int y = 0; y < clip.h / 2; ++y)
                    for (int x = 0; x < clip.w; ++x) {
                        if (clip.sixteen) std::swap(clip.idx[at(x, y)], clip.idx[at(x, clip.h - 1 - y)]);
                        else              std::swap(clip.px [at(x, y)], clip.px [at(x, clip.h - 1 - y)]);
                    }
            }
            if (flowButton(sel, "Rot", "Rotate the clip a quarter turn clockwise")) {
                // Block-aware for a GR / DLGR clip (a sample is a 7x4 block
                // there); the plain transpose for HGR / DHGR. The flag comes
                // from the CLIP, not the current mode: clipUsableHere() already
                // refuses a clip of the other geometry, and reading the mode
                // here rotated a GR clip as per-pixel (and a DHGR one as
                // blocks) whenever the two disagreed. See HgrPaintModel.h.
                hgrpaint::rotateClipCW(clip.w, clip.h, clip.sixteen,
                                       clip.blockMode, clip.px, clip.idx);
            }
        }
    }
    if (tool == Tool::Text) {
        ImGui::TextWrapped("%s", textPlaced
            ? "Type, Enter to stamp in the current colour. Click to move the caret."
            : "Click the canvas to place the text caret.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        // A freshly placed caret pulls focus here so keystrokes go to this box
        // (making WantTextInput true) rather than the emulated keyboard.
        if (focusTextInput) { ImGui::SetKeyboardFocusHere(); focusTextInput = false; }
        const bool enter = ImGui::InputText("##textbuf", textBuf, sizeof(textBuf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        Flow row;
        row.next(buttonWidth("Stamp"));
        const bool stamp = ImGui::Button("Stamp") || enter;
        if (stamp && textPlaced && textBuf[0]) {
            stampText(textBuf, color); textBuf[0] = '\0';
            // Enter deactivates the InputText (EnterReturnsTrue), which would leak
            // the next keystrokes to the emulated keyboard. Re-arm focus so the
            // caret (advanced by stampText) stays hot for continuous typing.
            // Only for the Enter path — a mouse Stamp click shouldn't steal focus.
            if (enter) focusTextInput = true;
        }
        char at[32];
        if (textPlaced) std::snprintf(at, sizeof at, "@ %d,%d", textX, textY);
        else            std::snprintf(at, sizeof at, "(no caret)");
        row.next(ImGui::CalcTextSize(at).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", at);
    }

    // Navigator thumbnail (only shown when the zoomed image overflows the
    // canvas viewport).
    ImGui::Separator();
    renderMinimap();
}

// ── Bottom: the colour palette and the status line ───────────────────────

void hgrpaint::HgrPaintEditor::renderColorBar(layout::Flow& row)
{
    // The swatches sit closer than other items: a palette, not buttons.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fh = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x * 0.5f, ImGui::GetStyle().ItemSpacing.y));
    Flow swatches;   // wraps on its own spacing; `row` resumes after it

    // GR / DHGR / DLGR: the 16 Apple II colours, selecting grColor.
    if (sixteenMode()) {
        const ImVec2 grSw(std::round(fh * 1.1f), fh);
        for (int i = 0; i < 16; ++i) {
            swatches.next(grSw.x);
            const bool sel = (i == grColor);
            ImGui::PushID(300 + i);
            ImGui::PushStyleColor(ImGuiCol_Button, kGrPalette[i]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGrPalette[i]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGrPalette[i]);
            if (ImGui::Button("##grsw", grSw)) grColor = i;
            ImGui::PopStyleColor(3);
            if (sel) dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kSelBorder, 0, 0, 2.5f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d: %s", i, kGrColorNames[i]);
            ImGui::PopID();
        }
    } else {
        const HgrColor palette[] = { HgrColor::Black, HgrColor::White, HgrColor::Violet,
                                     HgrColor::Green, HgrColor::Blue, HgrColor::Orange };
        const ImVec2 swSz(std::round(fh * 1.45f), fh);
        for (int i = 0; i < 6; ++i) {
            swatches.next(swSz.x);
            const HgrColor c = palette[i];
            const bool sel = (c == color);
            ImGui::PushID(200 + i);
            ImGui::PushStyleColor(ImGuiCol_Button, swatchColor(c));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, swatchColor(c));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, swatchColor(c));
            if (ImGui::Button("##sw", swSz)) color = c;
            ImGui::PopStyleColor(3);
            if (sel) dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kSelBorder, 0, 0, 2.5f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%d)", colorName(c), i + 1);
            ImGui::PopID();
        }
    }
    ImGui::PopStyleVar();
    // The chosen colour's name is in the status line, after its swatch.
    row.follow();   // the status continues after the last swatch
}

void hgrpaint::HgrPaintEditor::renderStatusBar(layout::Flow& row, int lx, int ly, bool hovered)
{
    // Persistent info line (HGR-04): coords, byte, parity-snapped column, tool,
    // colour swatch, page, zoom, undo/redo depth. Survives the mouse leaving the
    // canvas via the cached lastHoverX/Y. Its segments wrap as a whole.
    const char* toolNames[] = { "Pencil", "Eraser", "Line", "Rect", "Ellipse",
                                "Fill", "Eyedropper", "Select", "Palette", "Text" };
    const HgrColor activeColor = (tool == Tool::Eraser) ? HgrColor::Black : color;

    char where[96];
    if (hovered && lx >= 0 && ly >= 0 && dlgrMode) {
        std::snprintf(where, sizeof where, "x=%3d y=%3d  block=%d,%d  colour %d", lx, ly, lx / 7, ly / 4,
                      hgrpaint::dlgrBlockColorAt(shadow.data(), lx / 7, ly / 4));
    } else if (hovered && lx >= 0 && ly >= 0 && dhgrMode) {
        // DHGR: fat colour pixel + the MAIN-plane byte of its first dot (the aux
        // byte lives at the same address in the other bank).
        int offs[2];
        const int n = hgrpaint::dhgrPixelOffsets(lx, ly, offs);
        const int mainOff = (n > 0) ? (offs[0] >= kHiresSize ? offs[0] - kHiresSize : offs[0]) : 0;
        std::snprintf(where, sizeof where, "x=%3d y=%3d  byte=$%04X  colour %d", lx, ly,
                      baseAddr() + mainOff, hgrpaint::dhgrColorAt(shadow.data(), lx, ly));
    } else if (hovered && lx >= 0 && ly >= 0 && grMode) {   // GR: text-page byte (PAINT-3)
        std::snprintf(where, sizeof where, "x=%3d y=%3d  byte=$%04X  colour %d", lx, ly,
                      baseAddr() + hgrpaint::grBlockOffset(lx / 7, ly / 4),
                      hgrpaint::grBlockColorAt(shadow.data(), lx / 7, ly / 4));
    } else if (hovered && lx >= 0 && ly >= 0) {
        const int snapped = hgrpaint::snapColumn(lx, activeColor);
        std::snprintf(where, sizeof where, "x=%3d y=%3d  byte=$%04X  col->%d", lx, ly,
                      baseAddr() + hgrpaint::hgrByteOffset(snapped, ly), snapped);
    } else {
        std::snprintf(where, sizeof where, "x=--- y=---  byte=$----  col->-");
    }
    char page[32], zoom[16], undoRedo[40];
    std::snprintf(page, sizeof page, "Page %d ($%04X)", page2 ? 2 : 1, baseAddr());
    std::snprintf(zoom, sizeof zoom, "Zoom %dx", kZoomLadder[zoomIdx]);
    std::snprintf(undoRedo, sizeof undoRedo, "Undo:%zu Redo:%zu", undo.size(), redo.size());

    const float barW = ImGui::CalcTextSize("|").x + ImGui::GetStyle().ItemSpacing.x;
    // A segment: a bar before it when it continues the line. The first one
    // follows the colour palette after a group's gap, with no bar.
    bool first = true;
    auto lead = [&](float w) {
        if (first) { first = false; row.keep(w); row.next(w); }
        else if (row.next(w + barW)) { ImGui::TextDisabled("|"); ImGui::SameLine(); }
        ImGui::AlignTextToFramePadding();
    };
    auto seg = [&](const char* s) { lead(ImGui::CalcTextSize(s).x); ImGui::TextUnformatted(s); };

    seg(where);
    seg(toolNames[static_cast<int>(tool)]);
    {   // The active colour: a swatch and its name (the 16-colour index on
        // the GR / DHGR / DLGR pages, where the eraser paints colour 0).
        const float h = ImGui::GetTextLineHeight();
        const bool sixteen = sixteenMode();
        const int gi = (tool == Tool::Eraser) ? 0 : grColor;
        char cn[40];
        if (sixteen) std::snprintf(cn, sizeof cn, "%d %s", gi, kGrColorNames[gi]);
        else         std::snprintf(cn, sizeof cn, "%s", colorName(activeColor));
        lead(h + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(cn).x);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p(ImGui::GetCursorScreenPos().x,
                       ImGui::GetCursorScreenPos().y + ImGui::GetStyle().FramePadding.y);
        dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), sixteen ? kGrPalette[gi] : swatchColor(activeColor));
        dl->AddRect(p, ImVec2(p.x + h, p.y + h), IM_COL32(160, 160, 160, 255));
        ImGui::Dummy(ImVec2(h, ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(cn);
    }
    seg(page);
    seg(zoom);
    seg(undoRedo);
}

// ── The window ─────────────────────────────────────────────────────────────

void hgrpaint::HgrPaintEditor::render(const std::vector<uint8_t>& memory,
                                      const std::vector<uint8_t>* aux)
{
    // The monitor writes nothing, so it runs before the shortcuts (undo pokes).
    if (monitor) { renderMonitor(memory, aux); return; }
    handleShortcuts();

    renderTopBar();
    ImGui::Separator();

    // The bottom strip (colour palette + status line) wraps with the width,
    // so its height is MEASURED: last frame's, or a two-row guess the first
    // time. The body takes the rest.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float bottomH = bottomStripH > 0.0f
        ? bottomStripH
        : ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() + st.ItemSpacing.y * 2.0f;

    ImGui::BeginChild("hgrbody", ImVec2(0.0f, -bottomH), ImGuiChildFlags_None);
    {
        ImGui::BeginChild("hgrtools", ImVec2(toolPanelWidth(), 0.0f), ImGuiChildFlags_Borders);
        renderToolPanel();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("hgrcanvasside", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        renderCanvas(memory, aux);
        ImGui::EndChild();
    }
    ImGui::EndChild();
    const float bodyBottom = ImGui::GetItemRectMax().y;

    ImGui::Separator();
    {   // One flow: on a wide window the palette and the status share a line.
        Flow bottom;
        renderColorBar(bottom);
        renderStatusBar(bottom, lastHoverX, lastHoverY, lastHoverX >= 0);
    }
    bottomStripH = std::ceil(ImGui::GetCursorScreenPos().y - st.ItemSpacing.y - bodyBottom);

    renderFileBrowser();    // modal Load / Save / Save PNG / Import picker
    renderImportPreview();  // modal image-import preview with live sliders
}
