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

// HgrPaintEditorTables.h — the Paint editor's fixed tables (zoom ladder,
// swatch colours, colour names, fill patterns, tool icons), shared by the
// translation units of HgrPaintEditor (the editing core, the window layout,
// the dialogs). Private to the editor: nothing outside src/hgrpaint includes
// it. Portable (shared with POM1): ImGui and the icon font only.

#ifndef HGRPAINT_EDITOR_TABLES_H
#define HGRPAINT_EDITOR_TABLES_H

#include "HgrPaintModel.h"
#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cstdint>

namespace hgrpaint {
namespace tables {


// Fixed zoom ladder (HGR-09), 1x .. 16x. Mouse-wheel + Fit step the index.
inline constexpr int kZoomLadder[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
inline constexpr int kZoomLadderCount = static_cast<int>(sizeof(kZoomLadder) / sizeof(kZoomLadder[0]));

// Approximate sRGB for each HGR colour, for the palette swatches + tool
// previews (the canvas itself is rendered by the host NTSC pipeline, the
// source of truth).
inline ImU32 swatchColor(HgrColor c)
{
    switch (c) {
    case HgrColor::Black:  return IM_COL32(0, 0, 0, 255);
    case HgrColor::White:  return IM_COL32(255, 255, 255, 255);
    case HgrColor::Violet: return IM_COL32(221, 34, 221, 255);
    case HgrColor::Green:  return IM_COL32(20, 245, 60, 255);
    case HgrColor::Blue:   return IM_COL32(34, 102, 255, 255);
    case HgrColor::Orange: return IM_COL32(255, 120, 20, 255);
    }
    return IM_COL32(255, 255, 255, 255);
}

inline const char* colorName(HgrColor c)
{
    switch (c) {
    case HgrColor::Black:  return "Black";
    case HgrColor::White:  return "White";
    case HgrColor::Violet: return "Violet";
    case HgrColor::Green:  return "Green";
    case HgrColor::Blue:   return "Blue";
    case HgrColor::Orange: return "Orange";
    }
    return "?";
}

// The 16 Apple II lo-res (GR) colours, matching GraphicsCard::kApple2Palette
// (a hardware fact). ImU32 = IM_COL32(r,g,b,255).
inline const ImU32 kGrPalette[16] = {
    IM_COL32(0x00,0x00,0x00,255), IM_COL32(0xa7,0x0b,0x40,255),
    IM_COL32(0x40,0x1c,0xf7,255), IM_COL32(0xe6,0x28,0xff,255),
    IM_COL32(0x00,0x74,0x40,255), IM_COL32(0x80,0x80,0x80,255),
    IM_COL32(0x19,0x90,0xff,255), IM_COL32(0xbf,0x9c,0xff,255),
    IM_COL32(0x40,0x63,0x00,255), IM_COL32(0xe6,0x6f,0x00,255),
    IM_COL32(0x80,0x80,0x80,255), IM_COL32(0xff,0x8b,0xbf,255),
    IM_COL32(0x19,0xd7,0x00,255), IM_COL32(0xbf,0xe3,0x08,255),
    IM_COL32(0x58,0xf4,0xbf,255), IM_COL32(0xff,0xff,0xff,255),
};
inline const char* const kGrColorNames[16] = {
    "Black", "Dark Red", "Dark Blue", "Purple", "Dark Green", "Dark Gray",
    "Medium Blue", "Light Blue", "Brown", "Orange", "Light Gray", "Pink",
    "Light Green", "Yellow", "Aquamarine", "White" };

// MacPaint-style 8×8 fill patterns (bit x of row y&7 = foreground). Order is
// append-only (patternIdx is plain state).
struct Pattern8 { const char* name; uint8_t rows[8]; };
inline const Pattern8 kPatterns[] = {
    { "Solid",      {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF} },
    { "50%",        {0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55} },
    { "25%",        {0x88,0x00,0x22,0x00,0x88,0x00,0x22,0x00} },
    { "75%",        {0x77,0xFF,0xDD,0xFF,0x77,0xFF,0xDD,0xFF} },
    { "H lines",    {0xFF,0x00,0x00,0x00,0xFF,0x00,0x00,0x00} },
    { "V lines",    {0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11} },
    { "Diagonal",   {0x11,0x22,0x44,0x88,0x11,0x22,0x44,0x88} },
    { "Brick",      {0xFF,0x08,0x08,0x08,0xFF,0x80,0x80,0x80} },
};
inline constexpr int kPatternCount = static_cast<int>(sizeof(kPatterns) / sizeof(kPatterns[0]));

// FontAwesome-solid glyph for each tool, in Tool-enum order.
inline const char* const kToolIcons[10] = {
    ICON_FA_PENCIL,        // Pencil
    ICON_FA_ERASER,        // Eraser
    ICON_FA_SLASH,         // Line
    ICON_FA_SQUARE,        // Rectangle
    ICON_FA_CIRCLE,        // Ellipse
    ICON_FA_FILL_DRIP,     // Fill
    ICON_FA_EYE_DROPPER,   // Eyedropper
    ICON_FA_VECTOR_SQUARE, // Select (marquee)
    ICON_FA_PALETTE,       // Palette shift
    ICON_FA_FONT,          // Text (bbfont)
};

} // namespace tables
} // namespace hgrpaint

#endif // HGRPAINT_EDITOR_TABLES_H
