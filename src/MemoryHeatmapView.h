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

// MemoryHeatmapView — how the Memory Heatmap is drawn, without ImGui or GL:
// the colour of a byte, the columns and their rails, the named regions, the
// pages the video shows, and the zoom / pan transform. The window
// (MemoryHeatmap_ImGui) and the PNG export both paint with it, and the test
// `memory_heatmap` pins it.
//
// What comes from AppleWin FT (fenarinarsa's Debugger_Heatmap.cpp, branch
// `extended_debugger` of github.com/Fr3nchT0uch/AppleWin):
//   * one 256 x 256 map per 64 KB bank, MAIN and AUX side by side, $0000 at
//     the BOTTOM (Apple's own memory-map figures put $FFFF on top); one row a
//     page, the language card's $D000 bank 1 in the $C0-$CF rows;
//   * writes red, reads blue, executions green, fading with age, OVER a
//     background lit by the number of 1 bits of each byte (its `popcount`
//     table: 0 for $00, else (7 + bits) << 2);
//   * beside each map, a rail per region: where the CPU READS now (blue),
//     WRITES now (red), both (magenta) — and a white rail on the pages the
//     video shows.
// What POM2 adds: the ROM / I/O column, DMA in its own colour, a "trail" of
// what was touched since Clear, a cumulative mode, the names of the regions,
// max-pooling when a byte is smaller than a pixel, a glow on fresh accesses,
// where a byte sits on the screen. FT's own palette stays one click away
// (Palette::Ft).

#ifndef POM2_MEMORY_HEATMAP_VIEW_H
#define POM2_MEMORY_HEATMAP_VIEW_H

#include "MemoryHeatmap.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pom2::heatview {

// ── Colour ─────────────────────────────────────────────────────────────────

struct Rgb { uint8_t r, g, b; };
/// The colour of a kind of access (HeatData::Kind) at full intensity.
Rgb kindColour(int kind);
const char* kindName(int kind);

enum class Background : uint8_t { None, Bits, Value };
/// POM2's palette (softened hues ADDED over a dim, cool background, a flash
/// on the freshest accesses) or AppleWin FT's exactly (pure primaries ORed
/// over the popcount grey, a linear fade).
enum class Palette : uint8_t { Pom2, Ft };

struct Style {
    bool cumulative = false;   // counts since Clear instead of how recent
    int  fadeFrames = 25;      // recent: full at the access, gone after this
    Background background = Background::Bits;
    Palette palette = Palette::Pom2;
    bool trail = true;         // a byte touched since Clear keeps a dim tint
    int  frameCycles = 17030;  // one video frame, in CPU cycles
    std::array<bool, HeatData::kKinds> show{{true, true, true, true}};
};

/// FT's background: the luma of a byte from its bits set (0 for $00).
uint8_t bitsLuma(uint8_t value);

/// The intensity (0..1) of one kind for one byte. Recent: `v` is its stamp,
/// `now` the current stamp. Cumulative: `v` is its count, `logMax` the log2
/// of the largest count drawn (+1).
float intensity(uint32_t v, uint32_t now, const Style& style, float logMax);

/// The colour of one byte as 0xAABBGGRR (R in the low byte, as GL and PNG
/// want it). `v` holds the four kinds' stamps, or counts when cumulative.
uint32_t cellColour(const uint32_t v[HeatData::kKinds], uint32_t now, uint8_t value,
                    const Style& style, float logMax);

/// log2(max + 1) over a plane's counts in [lo, hi] for the shown kinds.
float countScale(const HeatData::PlaneCopy& copy, uint32_t lo, uint32_t hi, const Style& style);

/// Paint the rows of one plane into `rgba` (256 x 256, row = page, row 0
/// first; entries of rows the plane does not draw are left transparent).
void paintPlane(int plane, const HeatData::PlaneCopy& copy, const uint8_t* values,
                uint32_t now, const Style& style, float logMax, std::vector<uint32_t>& rgba);

// ── Cells, max-pooling, composition ───────────────────────────────────────
//
// What a byte shows, before it becomes a colour: the level (0-255) of each
// kind of access and the background. The map is drawn from these, so a view
// smaller than the map can be POOLED per kind — the hottest byte of each
// screen pixel wins, an isolated access never averages away (and the grey
// background, which is texture and not heat, is averaged).
struct HeatCell {
    std::array<uint8_t, HeatData::kKinds> k{};
    uint8_t bg = 0;   // FT's popcount luma, the value, or 0 (Style::background)
};
/// A plane's rows from firstRow(plane), row-major, 256 cells a row.
void heatCells(int plane, const HeatData::PlaneCopy& copy, const uint8_t* values, uint32_t now,
               const Style& style, float logMax, std::vector<HeatCell>& out);
/// The colour (0xAABBGGRR) of a cell in the style's palette.
uint32_t compose(const HeatCell& c, const Style& style);
/// The fresh part of a cell, for the glow drawn over the map (alpha = how
/// fresh; 0 = nothing to add). Recent mode only.
uint32_t glowColour(const HeatCell& c, const Style& style);
/// Max-pool `rows` x 256 cells into outW x outH (each at most the source's):
/// output column i takes source bytes [i*256/outW, (i+1)*256/outW), at least
/// one; the same for rows — per kind the maximum. The background is box-
/// filtered by the area each byte covers (texture, and aliasing otherwise).
/// `withBackground` false skips the filter (the glow needs only the heat).
void maxPool(const std::vector<HeatCell>& src, int rows, int outW, int outH,
             std::vector<HeatCell>& dst, bool withBackground = true);
/// Low-pass the background along each row, [1 2 1] / 4 (mirrored at the
/// ends), for a map drawn at 1-3 pixels a byte: nearest sampling at a
/// fractional scale gives bytes 1 or 2 pixels wide, and a $00 / $FF fill
/// turns into a moire. The heat is untouched.
void smoothBackground(std::vector<HeatCell>& cells, int rows);
/// The rail colour for RailRead | RailWrite bits in a palette.
Rgb railColour(uint8_t bits, Palette palette);

/// Everything the window lets the user choose, kept between sessions as one
/// settings value (`memory_heatmap_options`, "key=value" words). Unknown
/// words are skipped and a missing one keeps its default, so the format can
/// grow.
struct Options {
    Style style;
    bool showRom = true, showMain = true, showAux = true;
    int  auxChoice = -1;       // -1: the CPU's RamWorks bank
    bool bottomUp = true;      // FT / Apple: $0000 at the bottom
    bool fill = true;          // rows fill the height at the fit
    bool regions = true, rails = true;
    // Off by default (owner, 2026-11-28): the status line under the map
    // already says what is under the pointer; View turns them on.
    bool loupe = false, inspector = false;
    bool minimap = true;       // an overview inset while zoomed in
    bool labels = true;        // the debugger's symbols on the map, zoomed in
    bool glow = true;          // fresh accesses glow (recent mode)
};
std::string optionsText(const Options& o);
Options parseOptions(std::string_view text);

// ── What is where ──────────────────────────────────────────────────────────

/// The rows (pages) a plane draws: the ROM plane only $C0-$FF.
inline int firstRow(int plane) { return plane == HeatData::kRom ? 0xC0 : 0x00; }

struct Region {
    uint16_t lo, hi;
    const char* tag;    // short, drawn on the map
    const char* name;   // the hover / inspector
};
/// The regions of a RAM plane (main or aux, physical layout) or of the ROM
/// plane — what the hardware and the firmware put there.
const std::vector<Region>& regions(int plane);
const Region* regionAt(int plane, uint16_t phys);

/// A plane's title: "MAIN", "AUX", "AUX bank 3", "ROM & I/O".
std::string planeTitle(int plane, int auxBanks);

// ── Where a byte is on the screen ──────────────────────────────────────────

/// Where a byte of a RAM plane shows on the screen when its page is on it:
/// a text / lo-res cell (row 0-23, column 0-39) or a hi-res byte (line 0-191,
/// byte 0-39 = dots 7*byte .. 7*byte+6), in page 1 or 2 — or a "screen hole"
/// (the 8 bytes after each 120 the video never fetches). `aux` is the AUX
/// half: in 80 columns / double lo-res the EVEN column, in double hi-res the
/// first 7 of the 14 dots (Apple2Display.cpp's DHGR / DLGR comments).
struct ScreenSpot {
    enum Kind : uint8_t { None, Text, Hires } kind = None;
    int  page = 1;
    bool hole = false, aux = false;
    int  row = 0, col = 0;     // text: row, column; hi-res: line, byte
};
ScreenSpot screenSpot(int plane, uint16_t phys);
/// "text row 5, col 12 · 80-col 25 · lo-res rows 10-11" / "HGR1 line 37,
/// byte 12, dots 84-90 · DHGR dots 168-174" / "" for no spot.
std::string describeSpot(const ScreenSpot& s);

// ── Rails (FT's bars) ──────────────────────────────────────────────────────

enum : uint8_t { RailRead = 1, RailWrite = 2 };
struct Rails {
    std::array<uint8_t, 256> bank{};   // RailRead | RailWrite per row
    std::array<bool, 256> shown{};     // the video shows this page
};

/// The video switches that decide which pages are on screen.
struct VideoSwitches {
    bool text = true, mixed = false, page2 = false, hires = false;
    bool col80 = false, dhgr = false, store80 = false;
};
/// Mark the pages the video shows (FT's white bar): text / lo-res page 1 or
/// 2, hi-res page 1 or 2, in MAIN — and in AUX bank 0 for 80 columns,
/// double lo-res and double hi-res. 80STORE shows page 1 whatever PAGE2 says.
void markShown(const VideoSwitches& v, bool iie, Rails& main, Rails& aux0);

// ── Layout and transform ───────────────────────────────────────────────────

/// The canvas, in units of one byte: columns of 256 + a rail, side by side.
struct Layout {
    static constexpr float kPlane = 256.0f;   // one row = one page, one unit a byte
    static constexpr float kRail = 9.0f;      // bank rail 3, gap 1, video rail 3, pad 2
    static constexpr float kGap = 22.0f;      // between columns
    struct Column { int plane; float x; };
    std::vector<Column> columns;
    float width = 0.0f, height = kPlane;
    bool bottomUp = true;                     // FT / Apple: $0000 at the bottom
};
Layout makeLayout(const std::vector<int>& planes, bool bottomUp);
/// Canvas y of the top of a row (page).
inline float rowY(const Layout& l, int row) { return l.bottomUp ? 255.0f - row : float(row); }
/// The byte under a canvas point, or false (between columns, an empty row).
bool hit(const Layout& l, float cx, float cy, int& column, uint16_t& phys);
/// The canvas point at the top-left corner of a byte.
bool cellOrigin(const Layout& l, int plane, uint16_t phys, float& cx, float& cy);

/// screen = origin + (canvas - pan) * scale. With `fill`, a row is never
/// shorter than the height that shows all 256 rows: the map uses the whole
/// view at the fit (bytes taller than wide — the columns side by side are
/// much wider than tall) and the cells turn square once the zoom passes it.
struct Transform {
    float scale = 1.0f, scaleY = 1.0f, panX = 0.0f, panY = 0.0f;
    bool fill = true;
    float toScreenX(float cx) const { return (cx - panX) * scale; }
    float toScreenY(float cy) const { return (cy - panY) * scaleY; }
    float toCanvasX(float sx) const { return sx / scale + panX; }
    float toCanvasY(float sy) const { return sy / scaleY + panY; }
};
/// scaleY from scale, `fill` and the view's height.
void applyAspect(Transform& t, const Layout& l, float h);
/// The whole canvas, centred, in a view of `w` x `h` pixels.
Transform fit(const Layout& l, float w, float h, bool fill = true);
/// Zoom by `factor` keeping the canvas point under (sx, sy) where it is,
/// between the fit and `maxScale` pixels a byte; then clamp the pan.
void zoomAt(Transform& t, const Layout& l, float w, float h, float sx, float sy,
            float factor, float maxScale = 64.0f);
/// Keep the canvas on screen (centred along an axis it does not fill).
void clampPan(Transform& t, const Layout& l, float w, float h);
/// Centre the view on a canvas point at `scale` (at least the fit).
void centreOn(Transform& t, const Layout& l, float w, float h, float cx, float cy, float scale);

// ── Hot spots ──────────────────────────────────────────────────────────────

struct Hot { int plane; uint16_t phys; uint32_t count; };
/// The `n` bytes of `plane` with the most accesses of `kind` since Clear.
std::vector<Hot> hottest(const HeatData& data, int plane, int kind, std::size_t n);

}  // namespace pom2::heatview

#endif  // POM2_MEMORY_HEATMAP_VIEW_H
