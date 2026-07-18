// Pom1 Apple 1 Emulator
// Copyright (C) 2000-2026 Verhille Arnaud
//
// HGR paint editor — see HgrPaintEditor.h. Independent reimplementation
// inspired by fadden's HGRTool (concept only).

#include "HgrPaintEditor.h"
#include "HgrConvert.h"          // image → HGR import (ii-pix-style, all in hgrpaint/)
#include "HgrFont.h"             // bbfont CP437 glyph table for the Text tool

#include "imgui.h"
#include "IconsFontAwesome6.h"   // FA-solid glyphs (merged into the UI font, like bench/)
// No GL/GLFW include: all texture work goes through IHgrPaintHost::uploadTexture/
// destroyTexture so this portable module stays backend-agnostic (see the header).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

using hgrpaint::HgrColor;

namespace {

// microSD "SD CARD OS" tagged-filename helpers. That firmware stores no header:
// a file's type + load address live in its NAME, as "NAME#TTAAAA" (TT = type,
// 06 = binary; AAAA = hex load address). So an HGR page saved for $2000 must be
// named "NAME#062000" for `@L NAME` / `LOAD NAME` to place it at $2000. See
// src/MicroSD.cpp::parseTag. `loadAddr` is the page base ($2000/$4000, or the
// $0400/$0800 lo-res base — same convention, type still 06 = binary).
std::string sdCardTag(uint16_t loadAddr)
{
    char t[8];
    std::snprintf(t, sizeof(t), "#06%04X", loadAddr);
    return std::string(t);
}

// Turn a raw basename into an SD-CARD-OS-loadable default: strip a legacy ".hgr"
// extension, and append "#06AAAA" unless the name is already tagged (contains
// '#'). Empty → "IMAGE". Keeps the round-trip @S/@L compatible.
std::string sdCardDefaultName(std::string base, uint16_t loadAddr)
{
    // Drop a trailing ".hgr" (any case) — the tag replaces the extension.
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = base.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == "hgr") base.erase(dot);
    }
    if (base.empty()) base = "IMAGE";
    if (base.find('#') != std::string::npos) return base;   // already tagged
    return base + sdCardTag(loadAddr);
}

// Fixed zoom ladder (HGR-09), 1x .. 16x. Mouse-wheel + Fit step the index.
const int kZoomLadder[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
const int kZoomLadderCount = static_cast<int>(sizeof(kZoomLadder) / sizeof(kZoomLadder[0]));

// Approximate sRGB for each HGR colour, for the palette swatches + tool
// previews (the canvas itself is rendered by the host NTSC pipeline, the
// source of truth).
ImU32 swatchColor(HgrColor c)
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

const char* colorName(HgrColor c)
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
const ImU32 kGrPalette[16] = {
    IM_COL32(0x00,0x00,0x00,255), IM_COL32(0xa7,0x0b,0x40,255),
    IM_COL32(0x40,0x1c,0xf7,255), IM_COL32(0xe6,0x28,0xff,255),
    IM_COL32(0x00,0x74,0x40,255), IM_COL32(0x80,0x80,0x80,255),
    IM_COL32(0x19,0x90,0xff,255), IM_COL32(0xbf,0x9c,0xff,255),
    IM_COL32(0x40,0x63,0x00,255), IM_COL32(0xe6,0x6f,0x00,255),
    IM_COL32(0x80,0x80,0x80,255), IM_COL32(0xff,0x8b,0xbf,255),
    IM_COL32(0x19,0xd7,0x00,255), IM_COL32(0xbf,0xe3,0x08,255),
    IM_COL32(0x58,0xf4,0xbf,255), IM_COL32(0xff,0xff,0xff,255),
};
const char* const kGrColorNames[16] = {
    "Black", "Dark Red", "Dark Blue", "Purple", "Dark Green", "Dark Gray",
    "Medium Blue", "Light Blue", "Brown", "Orange", "Light Gray", "Pink",
    "Light Green", "Yellow", "Aquamarine", "White" };

// MacPaint-style 8×8 fill patterns (bit x of row y&7 = foreground). Order is
// append-only (patternIdx is plain state).
struct Pattern8 { const char* name; uint8_t rows[8]; };
const Pattern8 kPatterns[] = {
    { "Solid",      {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF} },
    { "50%",        {0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55} },
    { "25%",        {0x88,0x00,0x22,0x00,0x88,0x00,0x22,0x00} },
    { "75%",        {0x77,0xFF,0xDD,0xFF,0x77,0xFF,0xDD,0xFF} },
    { "H lines",    {0xFF,0x00,0x00,0x00,0xFF,0x00,0x00,0x00} },
    { "V lines",    {0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11} },
    { "Diagonal",   {0x11,0x22,0x44,0x88,0x11,0x22,0x44,0x88} },
    { "Brick",      {0xFF,0x08,0x08,0x08,0xFF,0x80,0x80,0x80} },
};
constexpr int kPatternCount = static_cast<int>(sizeof(kPatterns) / sizeof(kPatterns[0]));

// FontAwesome-solid glyph for each tool, in Tool-enum order.
const char* const kToolIcons[10] = {
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

} // namespace

hgrpaint::HgrPaintEditor::HgrPaintEditor(IHgrPaintHost* host_)
    : host(host_),
      // Sized for the widest mode (DHGR renders 560 dots); HGR/GR only fill
      // the first kHiresWidth×kHiresHeight pixels and upload with texW().
      canvasRgba(static_cast<size_t>(2 * kHiresWidth) * kHiresHeight, 0),
      shadow(kHiresSize, 0)
{
}

hgrpaint::HgrPaintEditor::~HgrPaintEditor()
{
    releaseGL();   // no-op if the host already released them before context teardown
}

bool hgrpaint::HgrPaintEditor::patternOn(int x, int y) const
{
    const int i = (patternIdx >= 0 && patternIdx < kPatternCount) ? patternIdx : 0;
    return (kPatterns[i].rows[y & 7] >> (x & 7)) & 1;
}

hgrpaint::HgrPaintEditor::Session hgrpaint::HgrPaintEditor::session() const
{
    Session s;
    s.mode = dlgrMode ? 3 : dhgrMode ? 2 : grMode ? 1 : 0;
    s.page2 = page2;
    s.zoomIdx = zoomIdx;
    s.ntscColor = ntscColor;
    s.aspect43 = aspect43;
    s.canvasPipeline = host ? host->canvasPipeline() : 0;
    s.browserDir = browserDir;
    return s;
}

void hgrpaint::HgrPaintEditor::restoreSession(const Session& s)
{
    int mode = s.mode;
    if ((mode == 2 || mode == 3) && !(host && host->supportsDhgr())) mode = 0;
    switchPage(mode, s.page2);
    zoomIdx = std::clamp(s.zoomIdx, 0, kZoomLadderCount - 1);
    firstFit = false;   // an explicit zoom was restored — don't refit over it
    ntscColor = s.ntscColor;
    aspect43 = s.aspect43;
    if (host) host->setCanvasPipeline(s.canvasPipeline);
    browserDir = s.browserDir;
}

void hgrpaint::HgrPaintEditor::releaseGL()
{
    // Destroy GPU textures via the host. Call this BEFORE the GL context is torn
    // down at shutdown — the destructor runs after that, so deleting here (and
    // zeroing the handles) avoids GL calls on a dead context.
    if (host) {
        if (texture)          host->destroyTexture(texture);
        if (importPreviewTex) host->destroyTexture(importPreviewTex);
        if (importSrcTex)     host->destroyTexture(importSrcTex);
        if (onionTex)         host->destroyTexture(onionTex);
        if (flipTex)          host->destroyTexture(flipTex);
    }
    texture = importPreviewTex = importSrcTex = onionTex = flipTex = nullptr;
}

void hgrpaint::HgrPaintEditor::renderShadow(uint32_t* out, bool mono)
{
    if (!host) return;
    if (dhgrMode)
        host->renderDhgrPage(shadow.data(), shadow.data() + kHiresSize, out, mono);
    else if (dlgrMode)
        host->renderDlgrPage(shadow.data(), shadow.data() + 0x400, out, mono);
    else
        host->renderHgrPage(shadow.data(), out, mono, grMode);
}

// ─────────────────────────────────────────────────────────────
// Painting primitives (operate on the shadow, emit writes, record undo)
// ─────────────────────────────────────────────────────────────

void hgrpaint::HgrPaintEditor::beginStroke(bool batch)
{
    stroke.clear();
    strokeBatching = batch && host;
    if (strokeBatching) host->beginBatch();
}

void hgrpaint::HgrPaintEditor::commitStroke()
{
    if (strokeBatching) { host->endBatch(); strokeBatching = false; }
    if (stroke.empty()) return;
    undo.push_back(std::move(stroke));
    stroke.clear();
    if (undo.size() > 64) undo.erase(undo.begin());
    // A fresh edit invalidates any redo history.
    redo.clear();
}

void hgrpaint::HgrPaintEditor::applyGrPlot(int x, int y, HgrColor c)
{
    // Map the shared 280×192 canvas pixel to a 40×48 lo-res block (7×4 px each).
    if (x < 0 || x >= kHiresWidth || y < 0 || y >= kHiresHeight) return;
    const int bx = x / 7, by = y / 4;
    const int idx = (c == HgrColor::Black) ? 0 : grColor;   // eraser/black → colour 0
    const int probe = hgrpaint::grBlockOffset(bx, by);
    if (probe < 0) return;
    const uint8_t old = shadow[probe];
    const int off = hgrpaint::plotGrBlock(shadow.data(), bx, by, idx);
    if (off < 0) return;   // nibble already that colour → no change
    emitShadowEdit(off, old);
}

void hgrpaint::HgrPaintEditor::applyDhgrPlot(int x, int y, HgrColor c)
{
    // x is a LOGICAL DHGR colour pixel (0..139); y a scanline.
    const int idx = (c == HgrColor::Black) ? 0 : grColor;   // eraser/black → colour 0
    int offs[2];
    const int n = hgrpaint::dhgrPixelOffsets(x, y, offs);
    if (n == 0) return;
    const uint8_t old0 = shadow[offs[0]];
    const uint8_t old1 = (n > 1) ? shadow[offs[1]] : 0;
    if (hgrpaint::plotDhgrPixel(shadow.data(), x, y, idx) <= 0) return;
    if (shadow[offs[0]] != old0) emitShadowEdit(offs[0], old0);
    if (n > 1 && shadow[offs[1]] != old1) emitShadowEdit(offs[1], old1);
}

void hgrpaint::HgrPaintEditor::applyDlgrPlot(int x, int y, HgrColor c)
{
    // Map the 560-dot logical space to an 80×48 block (7 dots × 4 px each).
    if (x < 0 || x >= 2 * kHiresWidth || y < 0 || y >= kHiresHeight) return;
    const int bx = x / 7, by = y / 4;
    const int idx = (c == HgrColor::Black) ? 0 : grColor;
    const int probe = hgrpaint::dlgrBlockOffset(bx, by);
    if (probe < 0) return;
    const uint8_t old = shadow[probe];
    const int off = hgrpaint::plotDlgrBlock(shadow.data(), bx, by, idx);
    if (off < 0) return;
    emitShadowEdit(off, old);
}

void hgrpaint::HgrPaintEditor::dlgrFloodFill(int x, int y, int colorIndex)
{
    if (x < 0 || x >= 2 * kHiresWidth || y < 0 || y >= kHiresHeight) return;
    const int sbx = x / 7, sby = y / 4;
    const int seed = hgrpaint::dlgrBlockColorAt(shadow.data(), sbx, sby);
    if (seed < 0 || (seed == (colorIndex & 0x0F) && patternIdx == 0)) return;
    std::vector<uint8_t> seen(static_cast<size_t>(kDlgrCols) * kGrRows, 0);
    std::vector<std::pair<int,int>> stack;
    stack.emplace_back(sbx, sby);
    seen[static_cast<size_t>(sby) * kDlgrCols + sbx] = 1;
    while (!stack.empty()) {
        const auto [bx, by] = stack.back(); stack.pop_back();
        const int probe = hgrpaint::dlgrBlockOffset(bx, by);
        const uint8_t old = shadow[probe];
        const int idx = patternOn(bx, by) ? colorIndex : 0;
        const int off = hgrpaint::plotDlgrBlock(shadow.data(), bx, by, idx);
        if (off >= 0) emitShadowEdit(off, old);
        const int nb[4][2] = {{bx-1,by},{bx+1,by},{bx,by-1},{bx,by+1}};
        for (auto& n : nb) {
            const int nx = n[0], ny = n[1];
            if (nx < 0 || nx >= kDlgrCols || ny < 0 || ny >= kGrRows) continue;
            const size_t i = static_cast<size_t>(ny) * kDlgrCols + nx;
            if (seen[i]) continue;
            if (hgrpaint::dlgrBlockColorAt(shadow.data(), nx, ny) != seed) continue;
            seen[i] = 1;
            stack.emplace_back(nx, ny);
        }
    }
}

void hgrpaint::HgrPaintEditor::applyPlotRaw(int x, int y, HgrColor c)
{
    if (grMode)  { applyGrPlot(x, y, c);   return; }
    if (dhgrMode){ applyDhgrPlot(x, y, c); return; }
    if (dlgrMode){ applyDlgrPlot(x, y, c); return; }
    const int off = hgrpaint::targetOffset(x, y, c);
    if (off < 0) return;
    const uint8_t old = shadow[off];
    const int changed = hgrpaint::plotPage(shadow.data(), x, y, c);
    if (changed < 0) return;
    emitShadowEdit(changed, old);
}

void hgrpaint::HgrPaintEditor::applyPlot(int x, int y, HgrColor c)
{
    applyPlotRaw(x, y, c);
    // Mirror symmetry: every brush/shape plot repeats about the enabled axes
    // (all inside the same stroke, so one undo unwinds the whole set). The
    // centre column/row plots once — the mirrored coordinate is checked for
    // identity, not parity.
    const int mx = logicalW() - 1 - x;
    const int my = kHiresHeight - 1 - y;
    if (mirrorX && mx != x)                          applyPlotRaw(mx, y, c);
    if (mirrorY && my != y)                          applyPlotRaw(x, my, c);
    if (mirrorX && mirrorY && mx != x && my != y)    applyPlotRaw(mx, my, c);
}

void hgrpaint::HgrPaintEditor::paintBrush(int cx, int cy, HgrColor c)
{
    const int r = brushSize - 1;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            applyPlotPat(cx + dx, cy + dy, c);
}

void hgrpaint::HgrPaintEditor::paintLine(int x0, int y0, int x1, int y1, HgrColor c)
{
    // Bresenham; stamp the brush at each step.
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        paintBrush(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void hgrpaint::HgrPaintEditor::paintRect(int x0, int y0, int x1, int y1, HgrColor c, bool filled)
{
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (filled) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                applyPlotPat(x, y, c);
    } else {
        paintLine(x0, y0, x1, y0, c);
        paintLine(x0, y1, x1, y1, c);
        paintLine(x0, y0, x0, y1, c);
        paintLine(x1, y0, x1, y1, c);
    }
}

void hgrpaint::HgrPaintEditor::paintEllipse(int x0, int y0, int x1, int y1, HgrColor c, bool filled)
{
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const int a = (x1 - x0) / 2;       // semi-axes
    const int b = (y1 - y0) / 2;
    const int cx = x0 + a;             // centre
    const int cy = y0 + b;

    // Degenerate boxes: fall back to a point / line so 1px drags still draw.
    if (a <= 0 && b <= 0) { paintBrush(cx, cy, c); return; }
    if (a <= 0) { paintLine(cx, y0, cx, y1, c); return; }
    if (b <= 0) { paintLine(x0, cy, x1, cy, c); return; }

    // Plot the four symmetric points (outline) or two horizontal spans (fill)
    // for ellipse coordinates (ex, ey) relative to the centre. The outline stamps
    // the brush so it honours the thickness slider like Line/Rectangle; the fill is
    // already solid so it plots single pixels.
    auto emit = [&](int ex, int ey) {
        if (filled) {
            for (int x = cx - ex; x <= cx + ex; ++x) {
                applyPlotPat(x, cy + ey, c);
                applyPlotPat(x, cy - ey, c);
            }
        } else {
            paintBrush(cx + ex, cy + ey, c);
            paintBrush(cx - ex, cy + ey, c);
            paintBrush(cx + ex, cy - ey, c);
            paintBrush(cx - ex, cy - ey, c);
        }
    };

    // Midpoint ellipse algorithm.
    const long a2 = static_cast<long>(a) * a;
    const long b2 = static_cast<long>(b) * b;
    long x = 0, y = b;
    long dx = 0, dy = 2 * a2 * y;
    long d1 = b2 - a2 * b + a2 / 4;
    emit(static_cast<int>(x), static_cast<int>(y));
    // Region 1.
    while (dx < dy) {
        x++;
        dx += 2 * b2;
        if (d1 < 0) {
            d1 += b2 + dx;
        } else {
            y--;
            dy -= 2 * a2;
            d1 += b2 + dx - dy;
        }
        emit(static_cast<int>(x), static_cast<int>(y));
    }
    // Region 2.
    long d2 = b2 * (x * 2 + 1) * (x * 2 + 1) / 4 + a2 * (y - 1) * (y - 1) - a2 * b2;
    while (y > 0) {
        y--;
        dy -= 2 * a2;
        if (d2 > 0) {
            d2 += a2 - dy;
        } else {
            x++;
            dx += 2 * b2;
            d2 += a2 - dy + dx;
        }
        emit(static_cast<int>(x), static_cast<int>(y));
    }
}

void hgrpaint::HgrPaintEditor::grFloodFill(int x, int y, int colorIndex)
{
    if (x < 0 || x > 279 || y < 0 || y > 191) return;
    // Lo-res flood in BLOCK space (40×48): replace the 4-connected region of equal
    // block colour at the seed with colorIndex, recording per-byte undo edits.
    const int sbx = x / 7, sby = y / 4;
    const int seed = hgrpaint::grBlockColorAt(shadow.data(), sbx, sby);
    // Same-colour fill is a no-op only when the fill is SOLID — a patterned
    // fill over its own colour is a legitimate texturing move.
    if (seed < 0 || (seed == (colorIndex & 0x0F) && patternIdx == 0)) return;
    std::vector<uint8_t> seen(static_cast<size_t>(kGrCols) * kGrRows, 0);
    std::vector<std::pair<int,int>> stack;
    stack.emplace_back(sbx, sby);
    seen[static_cast<size_t>(sby) * kGrCols + sbx] = 1;
    while (!stack.empty()) {
        const auto p = stack.back(); stack.pop_back();
        const int bx = p.first, by = p.second;
        const int probe = hgrpaint::grBlockOffset(bx, by);
        const uint8_t old = shadow[probe];
        // Pattern sampled at block coordinates so the fill tiles like a brush.
        const int idx = patternOn(bx, by) ? colorIndex : 0;
        const int off = hgrpaint::plotGrBlock(shadow.data(), bx, by, idx);
        if (off >= 0) emitShadowEdit(off, old);
        const int nb[4][2] = {{bx-1,by},{bx+1,by},{bx,by-1},{bx,by+1}};
        for (auto& n : nb) {
            const int nx = n[0], ny = n[1];
            if (nx < 0 || nx >= kGrCols || ny < 0 || ny >= kGrRows) continue;
            const size_t ni = static_cast<size_t>(ny) * kGrCols + nx;
            if (seen[ni]) continue;
            if (hgrpaint::grBlockColorAt(shadow.data(), nx, ny) != seed) continue;
            seen[ni] = 1;
            stack.emplace_back(nx, ny);
        }
    }
}

void hgrpaint::HgrPaintEditor::dhgrFloodFill(int x, int y, int colorIndex)
{
    // DHGR flood in colour-pixel space (140×192): replace the 4-connected
    // region of equal pixel colour at the seed. No NTSC-perception pass needed —
    // the aligned block model is a flat grid like GR, just finer.
    const int seed = hgrpaint::dhgrColorAt(shadow.data(), x, y);
    if (seed < 0 || (seed == (colorIndex & 0x0F) && patternIdx == 0)) return;
    std::vector<uint8_t> seen(static_cast<size_t>(kDhgrWidth) * kHiresHeight, 0);
    std::vector<std::pair<int,int>> stack;
    stack.emplace_back(x, y);
    seen[static_cast<size_t>(y) * kDhgrWidth + x] = 1;
    while (!stack.empty()) {
        const auto [px, py] = stack.back(); stack.pop_back();
        int offs[2];
        const int n = hgrpaint::dhgrPixelOffsets(px, py, offs);
        if (n > 0) {
            const uint8_t old0 = shadow[offs[0]];
            const uint8_t old1 = (n > 1) ? shadow[offs[1]] : 0;
            const int idx = patternOn(px, py) ? colorIndex : 0;   // patterned fill
            if (hgrpaint::plotDhgrPixel(shadow.data(), px, py, idx) > 0) {
                if (shadow[offs[0]] != old0) emitShadowEdit(offs[0], old0);
                if (n > 1 && shadow[offs[1]] != old1) emitShadowEdit(offs[1], old1);
            }
        }
        const int nb[4][2] = {{px-1,py},{px+1,py},{px,py-1},{px,py+1}};
        for (auto& nn : nb) {
            const int nx = nn[0], ny = nn[1];
            if (nx < 0 || nx >= kDhgrWidth || ny < 0 || ny >= kHiresHeight) continue;
            const size_t idx = static_cast<size_t>(ny) * kDhgrWidth + nx;
            if (seen[idx]) continue;
            if (hgrpaint::dhgrColorAt(shadow.data(), nx, ny) != seed) continue;
            seen[idx] = 1;
            stack.emplace_back(nx, ny);
        }
    }
}

void hgrpaint::HgrPaintEditor::floodFill(int x, int y, HgrColor c)
{
    if (x < 0 || x >= logicalW() || y < 0 || y > 191) return;
    if (grMode)  { grFloodFill(x, y, (c == HgrColor::Black) ? 0 : grColor); return; }
    if (dhgrMode){ dhgrFloodFill(x, y, (c == HgrColor::Black) ? 0 : grColor); return; }
    if (dlgrMode){ dlgrFloodFill(x, y, (c == HgrColor::Black) ? 0 : grColor); return; }
    // Flood by *perceived* artifact colour (hgrpaint::fillRegion renders the page
    // through the host NTSC pipeline), which is what the eye sees — a raw-bit
    // flood leaks through the off sub-pixels that dither every chromatic region.
    // fillRegion mutates a page buffer directly, so snapshot the shadow, run it,
    // then diff back into per-byte edits to keep undo + the live RAM writes working.
    std::vector<uint8_t> before = shadow;
    hgrpaint::fillRegion(shadow.data(), x, y, c,
                         [this](const uint8_t* page8k, uint32_t* out) {
                             if (host) host->renderHgrPage(page8k, out, /*mono=*/false);
                         });
    for (int off = 0; off < static_cast<int>(shadow.size()); ++off) {
        if (shadow[off] == before[off]) continue;
        emitShadowEdit(off, before[off]);
    }
}

void hgrpaint::HgrPaintEditor::applyOps(const std::vector<ByteEdit>& ops, bool forward)
{
    // forward = redo (write newVal, in recorded order); reverse = undo (write
    // oldVal, in reverse order so repeated touches of the same byte unwind to
    // the correct earliest value — see commitStroke ordering).
    auto write = [&](const ByteEdit& e, uint8_t val) {
        const int off = shadowOffOfAddr(e.addr);
        if (off >= 0 && off < static_cast<int>(shadow.size())) shadow[off] = val;
        hostPoke(e.addr, val);
    };
    if (host) host->beginBatch();   // one publish for the whole undo/redo replay
    if (forward) {
        for (const auto& e : ops) write(e, e.newVal);
    } else {
        for (auto it = ops.rbegin(); it != ops.rend(); ++it) write(*it, it->oldVal);
    }
    if (host) host->endBatch();
}

void hgrpaint::HgrPaintEditor::doUndo()
{
    if (undo.empty()) return;
    auto ops = std::move(undo.back());
    undo.pop_back();
    applyOps(ops, false);
    redo.push_back(std::move(ops));
    if (redo.size() > 64) redo.erase(redo.begin());
}

void hgrpaint::HgrPaintEditor::doRedo()
{
    if (redo.empty()) return;
    auto ops = std::move(redo.back());
    redo.pop_back();
    applyOps(ops, true);
    undo.push_back(std::move(ops));
    if (undo.size() > 64) undo.erase(undo.begin());
}

void hgrpaint::HgrPaintEditor::clearPage()
{
    beginStroke(true);
    // Bound by pageBytes(), not shadow.size(): in GR (lo-res) mode baseAddr() is
    // $0400 and the page is only 0x400 bytes, so iterating the full 0x2000 shadow
    // would poke $0400-$23FF — clobbering GR page 2, user RAM, and HIRES page 1.
    const int limit = std::min(pageBytes(), static_cast<int>(shadow.size()));
    for (int off = 0; off < limit; ++off) {
        // Text pages: never touch the screen holes — peripheral firmware
        // scratch (DLGR checks per plane; both planes carry holes).
        if ((grMode || dlgrMode) && hgrpaint::grIsScreenHole(off & 0x3FF)) continue;
        if (shadow[off] != 0) {
            const uint8_t old = shadow[off];
            shadow[off] = 0;
            emitShadowEdit(off, old);
        }
    }
    commitStroke();
}

// ─────────────────────────────────────────────────────────────
// Selection / clipboard (HGR-06) and palette-shift (HGR-11)
// ─────────────────────────────────────────────────────────────

void hgrpaint::HgrPaintEditor::applyIdxPlot(int x, int y, int colorIndex)
{
    if (grMode || dlgrMode) {
        if (x < 0 || x >= logicalW() || y < 0 || y >= kHiresHeight) return;
        const int bx = x / 7, by = y / 4;   // 7 canvas px per block in both spaces
        const int probe = grMode ? hgrpaint::grBlockOffset(bx, by)
                                 : hgrpaint::dlgrBlockOffset(bx, by);
        if (probe < 0) return;
        const uint8_t old = shadow[probe];
        const int off = grMode
            ? hgrpaint::plotGrBlock(shadow.data(), bx, by, colorIndex)
            : hgrpaint::plotDlgrBlock(shadow.data(), bx, by, colorIndex);
        if (off >= 0) emitShadowEdit(off, old);
    } else if (dhgrMode) {
        int offs[2];
        const int n = hgrpaint::dhgrPixelOffsets(x, y, offs);
        if (n == 0) return;
        const uint8_t old0 = shadow[offs[0]];
        const uint8_t old1 = (n > 1) ? shadow[offs[1]] : 0;
        if (hgrpaint::plotDhgrPixel(shadow.data(), x, y, colorIndex) <= 0) return;
        if (shadow[offs[0]] != old0) emitShadowEdit(offs[0], old0);
        if (n > 1 && shadow[offs[1]] != old1) emitShadowEdit(offs[1], old1);
    }
}

void hgrpaint::HgrPaintEditor::copySelection(bool cut)
{
    if (!hasSel) return;
    const int x0 = std::min(selX0, selX1), x1 = std::max(selX0, selX1);
    const int y0 = std::min(selY0, selY1), y1 = std::max(selY0, selY1);
    clip.w = x1 - x0 + 1;
    clip.h = y1 - y0 + 1;
    clip.sixteen = sixteenMode();
    clip.px.clear();
    clip.idx.clear();
    if (clip.sixteen) {
        // 16-colour modes copy indices at the logical grid (GR reads through
        // its 7×4 canvas-px blocks, DHGR per colour pixel).
        clip.idx.assign(static_cast<size_t>(clip.w) * clip.h, 0);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const int v = grMode
                    ? hgrpaint::grBlockColorAt(shadow.data(), x / 7, y / 4)
                    : dlgrMode
                    ? hgrpaint::dlgrBlockColorAt(shadow.data(), x / 7, y / 4)
                    : hgrpaint::dhgrColorAt(shadow.data(), x, y);
                clip.idx[static_cast<size_t>(y - y0) * clip.w + (x - x0)] =
                    static_cast<int8_t>(std::max(v, 0));
            }
    } else {
        // Store LOGICAL colours so paste re-snaps parity at any destination.
        clip.px.assign(static_cast<size_t>(clip.w) * clip.h, HgrColor::Black);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                clip.px[static_cast<size_t>(y - y0) * clip.w + (x - x0)] =
                    hgrpaint::colorAt(shadow.data(), x, y);
    }
    if (cut) {
        beginStroke(true);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                applyPlotRaw(x, y, HgrColor::Black);   // region op: no mirror
        commitStroke();
    }
}

void hgrpaint::HgrPaintEditor::pasteFloatingAt(int destX, int destY)
{
    if (!clipUsableHere()) return;
    beginStroke(true);
    for (int y = 0; y < clip.h; ++y)
        for (int x = 0; x < clip.w; ++x) {
            if (clip.sixteen) {
                const int v = clip.idx[static_cast<size_t>(y) * clip.w + x];
                if (v == 0) continue;             // black = transparent
                applyIdxPlot(destX + x, destY + y, v);
            } else {
                const HgrColor c = clip.px[static_cast<size_t>(y) * clip.w + x];
                if (c == HgrColor::Black) continue;   // black = transparent
                applyPlotRaw(destX + x, destY + y, c);   // region op: no mirror
            }
        }
    commitStroke();
}

void hgrpaint::HgrPaintEditor::paintPaletteByte(int lx, int ly)
{
    if (grMode || dhgrMode || dlgrMode) return;   // per-byte palette bit is a 280-HGR concept
    // (dlgrMode included: its 0x800 shadow must never see HGR interleave offsets)
    if (lx < 0 || lx > 279 || ly < 0 || ly > 191) return;
    const int byteCol = lx / 7;
    const int off = hgrpaint::hgrByteOffset(0, ly) + byteCol;
    const uint8_t old = shadow[off];
    const int ch = hgrpaint::setBytePalette(shadow.data(), byteCol, ly, paletteMsbMode);
    if (ch < 0) return;
    emitShadowEdit(ch, old);
}

void hgrpaint::HgrPaintEditor::stampText(const char* text, HgrColor c)
{
    if (grMode || dlgrMode) return;   // lo-res blocks are too coarse for glyphs
    if (!textPlaced || !text || !*text) return;
    // HGR: chromatic colours occupy a single column parity (Violet/Blue even,
    // Green/Orange odd); light only the glyph pixels on that parity so the text
    // renders as one clean artifact colour instead of double-stamping snapped
    // columns. White/Black light every pixel. DHGR has no parity: glyphs stamp
    // as fat 140-px colour pixels (~20 chars/line). One undo step per stamp.
    const int parity = (dhgrMode) ? -1
                     : (c == HgrColor::Violet || c == HgrColor::Blue)  ? 0
                     : (c == HgrColor::Green  || c == HgrColor::Orange) ? 1
                     : -1;
    beginStroke(true);
    int cx = textX, cy = textY;
    for (const char* p = text; *p; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (ch == '\n') { cx = textHomeX; cy += kBBFontGlyphH; continue; }
        // Word-wrap at the right edge so long strings don't run off the page.
        if (cx + kBBFontGlyphW > logicalW()) { cx = textHomeX; cy += kBBFontGlyphH; }
        if (cy >= kHiresHeight) break;
        for (int gy = 0; gy < kBBFontGlyphH; ++gy)
            for (int gx = 0; gx < kBBFontGlyphW; ++gx) {
                if (!hgrpaint::bbFontPixel(ch, gx, gy)) continue;
                const int px = cx + gx;
                if (parity >= 0 && (px & 1) != parity) continue;
                applyPlotRaw(px, cy + gy, c);   // region op: no mirror
            }
        cx += kBBFontAdvance;
    }
    commitStroke();
    // Drop the caret to the next line so repeated Enter presses stack text.
    textY = std::min(cy + kBBFontGlyphH, kHiresHeight - 1);
    textX = textHomeX;
}

// ─────────────────────────────────────────────────────────────
// UI
// ─────────────────────────────────────────────────────────────

void hgrpaint::HgrPaintEditor::renderMinimap()
{
    // Navigator thumbnail, drawn in the LEFT tool panel (below "Clear page").
    // Reads the canvas scroll metrics captured by renderCanvas last frame and
    // recentres the view via pendingScroll (applied at the next canvas draw).
    if (canvasScale <= 0.0f) return;
    // Only useful when the image overflows the viewport (something to navigate).
    const bool overflow = (canvasScrollMaxX > 1.0f) || (canvasScrollMaxY > 1.0f);
    if (!overflow) return;

    const float mmW = std::min(ImGui::GetContentRegionAvail().x, 130.0f);
    const float mmH = mmW * static_cast<float>(kHiresHeight) / static_cast<float>(kHiresWidth);
    ImGui::TextDisabled("Navigator");
    const ImVec2 mmMin = ImGui::GetCursorScreenPos();
    // InvisibleButton (not Dummy): it captures the mouse so a click/drag pans the
    // view instead of moving the editor window (same fix as the canvas).
    ImGui::InvisibleButton("##hgrminimap", ImVec2(mmW, mmH));
    const bool active = ImGui::IsItemActive();
    const ImVec2 mmMax(mmMin.x + mmW, mmMin.y + mmH);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(mmMin, mmMax, IM_COL32(0, 0, 0, 255));
    dl->AddImage(host->textureToImTexture(texture), mmMin, mmMax);
    dl->AddRect(mmMin, mmMax, IM_COL32(160, 160, 160, 255));

    // Visible viewport box (logical coords) → thumbnail.
    const float lx0 = canvasScrollX / canvasScale;
    const float ly0 = canvasScrollY / canvasScaleY;
    const float lw  = canvasViewW  / canvasScale;
    const float lh  = canvasViewH  / canvasScaleY;
    auto mmx = [&](float lx){ return mmMin.x + (lx / kHiresWidth)  * mmW; };
    auto mmy = [&](float ly){ return mmMin.y + (ly / kHiresHeight) * mmH; };
    dl->AddRect(ImVec2(mmx(lx0), mmy(ly0)), ImVec2(mmx(lx0 + lw), mmy(ly0 + lh)),
                IM_COL32(255, 255, 0, 230), 0, 0, 1.5f);

    // Click/drag inside the thumbnail recentres the view. Driven by the button's
    // active (held) state, and the mouse fraction is clamped so dragging past the
    // thumbnail edge still pans to the image border.
    if (active) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float fx = std::clamp((m.x - mmMin.x) / mmW, 0.0f, 1.0f);
        const float fy = std::clamp((m.y - mmMin.y) / mmH, 0.0f, 1.0f);
        pendingScrollX = std::clamp(fx * kHiresWidth  * canvasScale  - canvasViewW * 0.5f, 0.0f, canvasScrollMaxX);
        pendingScrollY = std::clamp(fy * kHiresHeight * canvasScaleY - canvasViewH * 0.5f, 0.0f, canvasScrollMaxY);
    }
}

void hgrpaint::HgrPaintEditor::switchPage(int mode, bool toPage2)
{
    const bool toGr = (mode == 1), toDhgr = (mode == 2), toDlgr = (mode == 3);
    if (grMode == toGr && dhgrMode == toDhgr && dlgrMode == toDlgr &&
        page2 == toPage2) return;
    // baseAddr() changes and undo/redo store ABSOLUTE addresses for the old page,
    // so flush any open op and drop history (same reasoning as the round-2 C2 fix).
    if (dragging) { commitStroke(); dragging = false; }
    pasting = false; hasSel = false;
    undo.clear(); redo.clear();
    grMode = toGr; dhgrMode = toDhgr; dlgrMode = toDlgr; page2 = toPage2;
    // Double-mode shadows carry BOTH planes (aux first); the single modes keep
    // the legacy 8 KB scratch. renderCanvas refills from live RAM next frame.
    shadow.assign(dhgrMode ? kDhgrPairSize : dlgrMode ? kDlgrPairSize
                                                      : kHiresSize, 0);
    // The 16-colour plots treat HgrColor::Black as "erase"; make sure a fresh
    // 16-colour session paints the chosen grColor rather than erasing if
    // Black happened to be picked.
    if (sixteenMode() && color == HgrColor::Black) color = HgrColor::White;
}

void hgrpaint::HgrPaintEditor::renderTopBar()
{
    // Slim top strip: page/mode select + help on line 1, file ops on line 2. Lives
    // above the tool palette + canvas, MacPaint-style. Eight pages: HIRES 1/2,
    // lo-res GR 1/2, and (on hosts with an aux bank) DHGR 1/2 + DLGR 1/2.
    struct PageBtn { const char* label; int mode; bool p2; const char* tip; };
    static const PageBtn kPages[8] = {
        { "HGR",   0, false, "HIRES page 1 ($2000)" },
        { "HGR2",  0, true,  "HIRES page 2 ($4000)" },
        { "GR",    1, false, "Lo-res GR page 1 ($0400)" },
        { "GR2",   1, true,  "Lo-res GR page 2 ($0800)" },
        { "DHGR",  2, false, "Double hi-res page 1 (aux+main $2000)" },
        { "DHGR2", 2, true,  "Double hi-res page 2 (aux+main $4000)" },
        { "DLGR",  3, false, "Double lo-res page 1 (aux+main $0400)" },
        { "DLGR2", 3, true,  "Double lo-res page 2 (aux+main $0800)" },
    };
    const int curMode = dlgrMode ? 3 : dhgrMode ? 2 : grMode ? 1 : 0;
    const int nPages = (host && host->supportsDhgr()) ? 8 : 4;
    for (int i = 0; i < nPages; ++i) {
        if (i != 0) ImGui::SameLine();
        const bool sel = (curMode == kPages[i].mode && page2 == kPages[i].p2);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(58, 96, 150, 255));
        if (ImGui::Button(kPages[i].label)) {
            switchPage(kPages[i].mode, kPages[i].p2);
            // Drive the live machine's soft switches to the picked page/mode so
            // the screen follows the editor. Done on every click (even re-picking
            // the current page) so it also re-asserts the display if a program had
            // left the machine in text/another page.
            if (host) {
                if (kPages[i].mode == 2)      host->setDisplayModeDhgr(kPages[i].p2);
                else if (kPages[i].mode == 3) host->setDisplayModeDlgr(kPages[i].p2);
                else host->setDisplayMode(kPages[i].mode == 1, kPages[i].p2);
            }
        }
        if (sel) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s — switch page/mode (clears undo history)", kPages[i].tip);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Mouse\n"
            "  Left drag    draw with the current tool / colour\n"
            "  Right drag   quick-erase (paint black) — no tool switch\n"
            "  Middle drag  pan the canvas\n"
            "  Wheel        zoom, anchored on the cursor\n"
            "  Alt + Left   eyedropper (pick colour)\n"
            "  Shift        constrain Line to 0/45/90 deg, Rect/Ellipse to a square\n"
            "\n"
            "Keys\n"
            "  P E L R O F I S M T   pencil eraser line rect ellipse fill eyedrop select palette text\n"
            "  1-6 colours   [ ] thickness   +/- zoom   G grid   X toggle filled (rect/ellipse)\n"
            "  Ctrl+Z / Ctrl+Y undo/redo   Ctrl+C/X/V copy/cut/paste");

    renderFileRow();   // line 2: path + Load / Save / Save PNG / stamp / status
}

void hgrpaint::HgrPaintEditor::renderToolPanel()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 kSelTint   = IM_COL32(58, 96, 150, 255);
    const ImU32 kSelHover  = IM_COL32(78, 116, 170, 255);
    const ImU32 kSelBorder = IM_COL32(255, 220, 60, 255);

    // ── Tool palette: 3-column grid of icon buttons ──────────────────────────
    const char* toolTips[] = {
        "Pencil (P)", "Eraser (E)", "Line (L)", "Rectangle (R)", "Ellipse (O)",
        "Fill (F)", "Eyedropper (I)", "Select (S)", "Palette shift (M)", "Text (T)" };
    const ImVec2 btnSz(34, 34);
    for (int i = 0; i < kToolCount; ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        const bool sel = (static_cast<int>(tool) == i);
        ImGui::PushID(i);
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Button, kSelTint);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSelHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSelHover);
        }
        if (ImGui::Button(kToolIcons[i], btnSz)) { prevTool = tool; tool = static_cast<Tool>(i); }
        if (sel) ImGui::PopStyleColor(3);
        if (sel) dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kSelBorder, 0, 0, 2.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", toolTips[i]);
        ImGui::PopID();
    }

    ImGui::Separator();

    // ── Tool options (contextual) ────────────────────────────────────────────
    // Stroke thickness / brush size — only the tools that stamp the brush use it
    // (Pencil, Eraser, Line, Rectangle, Ellipse); Fill/Eyedropper/Select/Palette/
    // Text ignore it, so hide the slider there to keep the meaning clear.
    const bool usesThickness = (tool == Tool::Pencil || tool == Tool::Eraser ||
                                tool == Tool::Line || tool == Tool::Rectangle ||
                                tool == Tool::Ellipse);
    if (usesThickness) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##thickness", &brushSize, 1, 7, "Thickness %d px");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stroke thickness / brush size  ([ and ])");
    }
    if (tool == Tool::Rectangle || tool == Tool::Ellipse)
        ImGui::Checkbox("Filled", &rectFilled);
    // ── Fill-pattern strip (MacPaint) for the pattern-aware tools ────────────
    const bool usesPattern = (tool == Tool::Pencil || tool == Tool::Line ||
                              tool == Tool::Rectangle || tool == Tool::Ellipse ||
                              tool == Tool::Fill);
    if (usesPattern) {
        const float cell = 2.5f;                    // 8×8 pattern → 20 px swatch
        for (int i = 0; i < kPatternCount; ++i) {
            if (i % 4 != 0) ImGui::SameLine();
            ImGui::PushID(400 + i);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##pat", ImVec2(8 * cell, 8 * cell)))
                patternIdx = i;
            const ImU32 fg = IM_COL32(230, 230, 230, 255);
            const ImU32 bg = IM_COL32(25, 25, 25, 255);
            dl->AddRectFilled(p0, ImVec2(p0.x + 8 * cell, p0.y + 8 * cell), bg);
            for (int yy = 0; yy < 8; ++yy)
                for (int xx = 0; xx < 8; ++xx)
                    if ((kPatterns[i].rows[yy] >> xx) & 1)
                        dl->AddRectFilled(ImVec2(p0.x + xx * cell, p0.y + yy * cell),
                                          ImVec2(p0.x + (xx + 1) * cell,
                                                 p0.y + (yy + 1) * cell), fg);
            dl->AddRect(p0, ImVec2(p0.x + 8 * cell, p0.y + 8 * cell),
                        i == patternIdx ? IM_COL32(255, 220, 60, 255)
                                        : IM_COL32(90, 90, 90, 255),
                        0, 0, i == patternIdx ? 2.0f : 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s pattern%s", kPatterns[i].name,
                                  i == 0 ? "" : " (bits paint the colour, gaps paint black)");
            ImGui::PopID();
        }
    }

    // Mirror symmetry — brush/shape plots repeat about the enabled axes
    // (region ops — paste, text, fill — deliberately don't).
    if (usesThickness || tool == Tool::Fill) {
        ImGui::Checkbox("SymX", &mirrorX);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mirror drawing about the vertical axis");
        ImGui::SameLine();
        ImGui::Checkbox("SymY", &mirrorY);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mirror drawing about the horizontal axis");
    }
    if (tool == Tool::PaletteShift) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo("##msb", &paletteMsbMode,
                     "MSB: clear\0MSB: set\0MSB: toggle\0");
    }
    if (tool == Tool::Select) {
        if (ImGui::Button("Copy")) copySelection(false);
        ImGui::SameLine();
        if (ImGui::Button("Cut"))  copySelection(true);
        if (ImGui::Button("Paste") && clipUsableHere()) {
            if (dragging) { commitStroke(); dragging = false; }   // flush an open stroke
            pasting = true; pasteX = std::min(selX0, selX1); pasteY = std::min(selY0, selY1);
        }
        // Clip transforms (apply to the clipboard content; paste to commit).
        if (clip.w > 0) {
            auto at = [&](int x, int y) { return static_cast<size_t>(y) * clip.w + x; };
            if (ImGui::Button("FlipH")) {
                for (int y = 0; y < clip.h; ++y)
                    for (int x = 0; x < clip.w / 2; ++x) {
                        if (clip.sixteen) std::swap(clip.idx[at(x, y)], clip.idx[at(clip.w - 1 - x, y)]);
                        else              std::swap(clip.px [at(x, y)], clip.px [at(clip.w - 1 - x, y)]);
                    }
            }
            ImGui::SameLine();
            if (ImGui::Button("FlipV")) {
                for (int y = 0; y < clip.h / 2; ++y)
                    for (int x = 0; x < clip.w; ++x) {
                        if (clip.sixteen) std::swap(clip.idx[at(x, y)], clip.idx[at(x, clip.h - 1 - y)]);
                        else              std::swap(clip.px [at(x, y)], clip.px [at(x, clip.h - 1 - y)]);
                    }
            }
            ImGui::SameLine();
            if (ImGui::Button("Rot")) {
                // Rotate the clip 90° clockwise: (x,y) → (h-1-y, x), dims swap.
                Clip r;
                r.w = clip.h; r.h = clip.w; r.sixteen = clip.sixteen;
                if (clip.sixteen) r.idx.assign(static_cast<size_t>(r.w) * r.h, 0);
                else              r.px.assign(static_cast<size_t>(r.w) * r.h, HgrColor::Black);
                for (int y = 0; y < clip.h; ++y)
                    for (int x = 0; x < clip.w; ++x) {
                        const size_t d = static_cast<size_t>(x) * r.w + (clip.h - 1 - y);
                        if (clip.sixteen) r.idx[d] = clip.idx[at(x, y)];
                        else              r.px [d] = clip.px [at(x, y)];
                    }
                clip = std::move(r);
            }
        }
    }
    if (tool == Tool::Text) {
        ImGui::TextWrapped("%s", textPlaced
            ? "Type, Enter to stamp in the current colour. Click to move the caret."
            : "Click the canvas to place the text caret.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        // A freshly placed caret pulls focus here so keystrokes go to this box
        // (making WantTextInput true) rather than the emulated Apple-1 keyboard.
        if (focusTextInput) { ImGui::SetKeyboardFocusHere(); focusTextInput = false; }
        const bool enter = ImGui::InputText("##textbuf", textBuf, sizeof(textBuf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool stamp = ImGui::Button("Stamp") || enter;
        if (stamp && textPlaced && textBuf[0]) {
            stampText(textBuf, color); textBuf[0] = '\0';
            // Enter deactivates the InputText (EnterReturnsTrue), which would leak
            // the next keystrokes to the emulated Apple-1 keyboard. Re-arm focus so
            // the caret (advanced by stampText) stays hot for continuous typing.
            // Only for the Enter path — a mouse Stamp click shouldn't steal focus.
            if (enter) focusTextInput = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(textPlaced ? "@ %d,%d" : "(no caret)", textX, textY);
    }

    ImGui::Separator();

    // ── Zoom ─────────────────────────────────────────────────────────────────
    if (ImGui::Button("-##zoom")) zoomIdx = std::max(zoomIdx - 1, 0);
    ImGui::SameLine();
    ImGui::Text("%dx", kZoomLadder[zoomIdx]);
    ImGui::SameLine();
    if (ImGui::Button("+##zoom")) zoomIdx = std::min(zoomIdx + 1, kZoomLadderCount - 1);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) wantFit = true;

    ImGui::Separator();

    // ── Display toggles ──────────────────────────────────────────────────────
    ImGui::Checkbox("Grid", &showGrid);
    ImGui::Checkbox("Seams", &showConflicts);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(dhgrMode
            ? "Outline pixels whose rendered dots deviate from their block\n"
              "colour (NTSC fringing at colour transitions)."
            : "Mark byte pairs that disagree on the palette bit (NTSC bleed).");
    ImGui::Checkbox("NTSC", &ntscColor);
    // Colour-pipeline selector (only when the host offers a choice and the
    // canvas is in colour — the mono preview bypasses the pipeline).
    if (ntscColor && host) {
        const auto pipes = host->canvasPipelines();
        if (pipes.size() > 1) {
            int cur = host->canvasPipeline();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##canvaspipe",
                                  pipes[static_cast<size_t>(cur) < pipes.size() ? cur : 0].c_str())) {
                for (int i = 0; i < static_cast<int>(pipes.size()); ++i)
                    if (ImGui::Selectable(pipes[i].c_str(), i == cur))
                        host->setCanvasPipeline(i);
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Colour decode pipeline the canvas (and the\n"
                                  "import preview) renders through.");
        }
    }
    ImGui::Checkbox("4:3", &aspect43);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Aspect-correct display (the Apple II filled a 4:3 CRT).");
    // ── Flipbook (double-buffer animation preview: page 1 ↔ page 2) ─────────
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
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##fliphz", &flipHz, 1.0f, 30.0f, "Flip %.0f Hz");
    }
    if (ghostOther && !flipShow) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##ghosta", &ghostAlpha, 0.05f, 0.9f, "Ghost %.2f");
    }

    // Onion-skin controls (armed from the import preview's "Onion skin").
    if (onionTex) {
        ImGui::Checkbox("Onion", &onionShow);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tracing overlay of the last imported source.");
        ImGui::SameLine();
        if (ImGui::SmallButton("x##onion")) {
            if (host) host->destroyTexture(onionTex);
            onionTex = nullptr;
            onionShow = false;
        }
        if (onionShow) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##onionalpha", &onionAlpha, 0.05f, 0.95f, "Onion %.2f");
        }
    }

    ImGui::Separator();

    // ── Edit ─────────────────────────────────────────────────────────────────
    if (ImGui::Button("Undo")) doUndo();
    ImGui::SameLine();
    if (ImGui::Button("Redo")) doRedo();
    if (ImGui::Button("Clear page")) clearPage();

    // Navigator thumbnail, below the edit buttons (only shown when the zoomed
    // image overflows the canvas viewport).
    ImGui::Separator();
    renderMinimap();
}

void hgrpaint::HgrPaintEditor::renderColorBar()
{
    // Horizontal colour palette along the bottom (MacPaint pattern strip).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 swSz(34, 26);

    // GR / DHGR / DLGR mode: the 16 Apple II colours, selecting grColor.
    if (sixteenMode()) {
        const ImVec2 grSw(26, 22);
        for (int i = 0; i < 16; ++i) {
            if (i % 8 != 0) ImGui::SameLine();
            const bool sel = (i == grColor);
            ImGui::PushID(300 + i);
            ImGui::PushStyleColor(ImGuiCol_Button, kGrPalette[i]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGrPalette[i]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGrPalette[i]);
            if (ImGui::Button("##grsw", grSw)) grColor = i;
            ImGui::PopStyleColor(3);
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (sel) dl->AddRect(a, b, IM_COL32(255, 220, 60, 255), 0, 0, 2.5f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d: %s", i, kGrColorNames[i]);
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("  GR %d: %s", grColor, kGrColorNames[grColor]);
        return;
    }

    const HgrColor palette[] = { HgrColor::Black, HgrColor::White, HgrColor::Violet,
                                 HgrColor::Green, HgrColor::Blue, HgrColor::Orange };
    for (int i = 0; i < 6; ++i) {
        if (i != 0) ImGui::SameLine();
        const HgrColor c = palette[i];
        const bool sel = (c == color);
        ImGui::PushID(200 + i);
        ImGui::PushStyleColor(ImGuiCol_Button, swatchColor(c));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, swatchColor(c));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, swatchColor(c));
        if (ImGui::Button("##sw", swSz)) color = c;
        ImGui::PopStyleColor(3);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (sel) dl->AddRect(a, b, IM_COL32(255, 220, 60, 255), 0, 0, 2.5f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%d)", colorName(c), i + 1);
        ImGui::PopID();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("  Colour: %s", colorName(color));
}

void hgrpaint::HgrPaintEditor::renderCanvas(const std::vector<uint8_t>& memory,
                                            const std::vector<uint8_t>* aux)
{
    // Refresh the shadow from live RAM so external program writes show, and so
    // read-modify-write paints start from the current bytes. The DHGR shadow is
    // the [aux 8 KB][main 8 KB] pair; a missing aux snapshot reads as zeros.
    if (dhgrMode || dlgrMode) {
        const size_t pb = static_cast<size_t>(planeBytes());
        if (aux && aux->size() >= static_cast<size_t>(baseAddr()) + pb)
            std::copy(aux->begin() + baseAddr(),
                      aux->begin() + baseAddr() + pb,
                      shadow.begin());
        else
            std::fill(shadow.begin(), shadow.begin() + pb, uint8_t{0});
        if (memory.size() >= static_cast<size_t>(baseAddr()) + pb)
            std::copy(memory.begin() + baseAddr(),
                      memory.begin() + baseAddr() + pb,
                      shadow.begin() + pb);
    } else if (memory.size() >= static_cast<size_t>(baseAddr()) + kHiresSize) {
        std::copy(memory.begin() + baseAddr(),
                  memory.begin() + baseAddr() + kHiresSize,
                  shadow.begin());
    }

    // Render the shadow page through the host NTSC pipeline (the same renderer the
    // emulator screen uses) so the canvas is pixel-identical. The shadow was just
    // refreshed from live RAM and reflects any pokes we made this frame. Upload it
    // through the host (crisp nearest-neighbour, like the GEN2 window).
    renderShadow(canvasRgba.data(), !ntscColor);
    if (host)
        texture = host->uploadTexture(texture, canvasRgba.data(),
                                      texW(), kHiresHeight, /*linear=*/false);

    // Flipbook / ghost: render the SIBLING page (page 1↔2) from live RAM the
    // same way the shadow is built, so the animation preview and the ghost
    // overlay track external program writes too.
    if ((flipShow || ghostOther) && host) {
        const uint16_t otherBase = (grMode || dlgrMode)
            ? (page2 ? 0x0400 : 0x0800) : (page2 ? 0x2000 : 0x4000);
        flipShadow.assign(shadow.size(), 0);
        if (dhgrMode || dlgrMode) {
            const size_t pb = static_cast<size_t>(planeBytes());
            if (aux && aux->size() >= static_cast<size_t>(otherBase) + pb)
                std::copy(aux->begin() + otherBase,
                          aux->begin() + otherBase + pb, flipShadow.begin());
            if (memory.size() >= static_cast<size_t>(otherBase) + pb)
                std::copy(memory.begin() + otherBase,
                          memory.begin() + otherBase + pb,
                          flipShadow.begin() + pb);
        } else if (memory.size() >= static_cast<size_t>(otherBase) + kHiresSize) {
            std::copy(memory.begin() + otherBase,
                      memory.begin() + otherBase + kHiresSize, flipShadow.begin());
        }
        flipRgba.assign(static_cast<size_t>(texW()) * kHiresHeight, 0);
        if (dhgrMode)
            host->renderDhgrPage(flipShadow.data(), flipShadow.data() + kHiresSize,
                                 flipRgba.data(), !ntscColor);
        else if (dlgrMode)
            host->renderDlgrPage(flipShadow.data(), flipShadow.data() + 0x400,
                                 flipRgba.data(), !ntscColor);
        else
            host->renderHgrPage(flipShadow.data(), flipRgba.data(), !ntscColor, grMode);
        flipTex = host->uploadTexture(flipTex, flipRgba.data(),
                                      texW(), kHiresHeight, /*linear=*/false);
    }

    float scale = static_cast<float>(kZoomLadder[zoomIdx]);
    // Optional aspect-correct display: the Apple II active area filled a 4:3
    // CRT, so its 280-equivalent columns are slightly narrower than square
    // (192·4/3 = 256 wide for 192 rows). All horizontal maths below goes
    // through this factor; 1.0 keeps the historical square-pixel canvas.
    const float af = aspect43 ? (192.0f * 4.0f / 3.0f) / kHiresWidth : 1.0f;
    ImVec2 imgSize(kHiresWidth * scale * af, kHiresHeight * scale);

    ImGui::BeginChild("hgrcanvas", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // Apply a scroll requested by the minimap last frame (HGR-10).
    if (pendingScrollX >= 0) { ImGui::SetScrollX(pendingScrollX); pendingScrollX = -1; }
    if (pendingScrollY >= 0) { ImGui::SetScrollY(pendingScrollY); pendingScrollY = -1; }

    // First open: fit the image to the viewport rather than leaving a tiny 3x
    // canvas adrift in a big window (a poor first impression).
    if (firstFit) { firstFit = false; wantFit = true; }

    // ── Zoom-to-fit (HGR-09): pick the largest ladder step that fits ─────────
    if (wantFit) {
        wantFit = false;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        int best = 0;
        for (int i = 0; i < kZoomLadderCount; ++i) {
            if (kHiresWidth  * kZoomLadder[i] * af <= avail.x &&
                kHiresHeight * kZoomLadder[i] <= avail.y)
                best = i;
        }
        zoomIdx = best;
        scale = static_cast<float>(kZoomLadder[zoomIdx]);
        imgSize = ImVec2(kHiresWidth * scale * af, kHiresHeight * scale);
    }

    // ── Mouse-wheel zoom (HGR-09): step the ladder, recentre on the cursor ──
    {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f) {
            const int oldZoom = kZoomLadder[zoomIdx];
            const int ni = std::clamp(zoomIdx + (io.MouseWheel > 0 ? 1 : -1),
                                      0, kZoomLadderCount - 1);
            if (ni != zoomIdx) {
                // Logical pixel currently under the cursor, in the OLD scale.
                const ImVec2 cur = ImGui::GetCursorScreenPos();
                const float anchorLX = (io.MousePos.x - cur.x) / (oldZoom * af);
                const float anchorLY = (io.MousePos.y - cur.y) / oldZoom;
                zoomIdx = ni;
                const int newZoom = kZoomLadder[zoomIdx];
                scale = static_cast<float>(newZoom);
                imgSize = ImVec2(kHiresWidth * scale * af, kHiresHeight * scale);
                // Keep that logical pixel under the mouse after rescaling.
                const float mouseInChildX = io.MousePos.x - cur.x + ImGui::GetScrollX();
                const float mouseInChildY = io.MousePos.y - cur.y + ImGui::GetScrollY();
                ImGui::SetScrollX(anchorLX * newZoom * af - (mouseInChildX - ImGui::GetScrollX()));
                ImGui::SetScrollY(anchorLY * newZoom - (mouseInChildY - ImGui::GetScrollY()));
            }
        }
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // The canvas is an InvisibleButton, NOT an Image: it captures the mouse so a
    // drag PAINTS instead of moving the editor window. ImGui moves a window when
    // you drag its background — an Image never consumes the drag, a button does.
    // It grabs left/right/middle so erase (RMB) and pan (MMB) also keep the window
    // put; the window now only moves from its title bar (HGR draw bug fix).
    ImGui::InvisibleButton("##hgrcanvasimg", imgSize,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Flipbook phase: alternate the displayed page at flipHz (editing always
    // hits the selected page regardless of which phase is on screen).
    const bool showSibling = flipShow && flipTex &&
        (static_cast<int>(ImGui::GetTime() * flipHz) & 1);
    dl->AddImage(host->textureToImTexture(showSibling ? flipTex : texture), origin,
                 ImVec2(origin.x + imgSize.x, origin.y + imgSize.y));
    // Ghost of the sibling page (onion-skin style) — only when not flipping,
    // a flicker + ghost together would be unreadable.
    if (ghostOther && !flipShow && flipTex)
        dl->AddImage(host->textureToImTexture(flipTex), origin,
                     ImVec2(origin.x + imgSize.x, origin.y + imgSize.y),
                     ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, static_cast<int>(ghostAlpha * 255.0f)));

    // Onion-skin tracing layer (280-eq visual space → screen via zoom·aspect).
    if (onionShow && onionTex && host) {
        const float cs = scale * af;
        const ImVec2 oa(origin.x + onionX0 * cs, origin.y + onionY0 * scale);
        const ImVec2 ob(origin.x + (onionX0 + onionW) * cs,
                        origin.y + (onionY0 + onionH) * scale);
        dl->AddImage(host->textureToImTexture(onionTex), oa, ob,
                     ImVec2(onionU0, onionV0), ImVec2(onionU1, onionV1),
                     IM_COL32(255, 255, 255,
                              static_cast<int>(onionAlpha * 255.0f)));
    }

    // ── Middle-button drag pans the canvas at any zoom. Start when pressed over
    // the canvas, continue via per-frame delta until release — robust while the
    // InvisibleButton owns the mouse. ───────────────────────────────────────────
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panning = true;
    if (panning && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        ImGui::SetScrollX(ImGui::GetScrollX() - d.x);
        ImGui::SetScrollY(ImGui::GetScrollY() - d.y);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) panning = false;

    // Each logical pixel is pxScreenW()× wider on screen than an HGR one (DHGR
    // paints 140 fat colour pixels across the same 280·zoom canvas footprint),
    // narrowed by the optional 4:3 aspect factor.
    const float xs = scale * pxScreenW() * af;

    // Optional pixel grid (only at high zoom so it stays readable). HGR/GR mark
    // byte columns (7 px); DHGR marks the 140 colour pixels directly.
    if (showGrid && kZoomLadder[zoomIdx] >= (dhgrMode ? 4 : 3)) {
        const ImU32 gcol = IM_COL32(80, 80, 80, 90);
        const int step = dhgrMode ? 1 : 7;   // byte cols / colour px / DLGR blocks
        for (int x = 0; x <= logicalW(); x += step) {
            const float fx = origin.x + x * xs;
            dl->AddLine(ImVec2(fx, origin.y), ImVec2(fx, origin.y + imgSize.y), gcol);
        }
        const int rowStep = (grMode || dlgrMode) ? 4 : 8;   // block rows vs byte rows
        for (int y = 0; y <= kHiresHeight; y += rowStep) {
            const float fy = origin.y + y * scale;
            dl->AddLine(ImVec2(origin.x, fy), ImVec2(origin.x + imgSize.x, fy), gcol);
        }
    }

    // ── Palette-seam overlay (HGR-07): mark adjacent lit bytes that disagree
    // on the shared high bit — where NTSC artifact-colour bleed happens. Scan
    // only the visible/scrolled region for perf.
    if (showConflicts && !grMode && !dhgrMode && !dlgrMode) {   // palette seams are a 280-HGR concept
        const float sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
        const ImVec2 vis = ImGui::GetContentRegionAvail();
        const int y0 = std::clamp(static_cast<int>(sy / scale), 0, kHiresHeight - 1);
        const int y1 = std::clamp(static_cast<int>((sy + vis.y) / scale) + 1, 0, kHiresHeight - 1);
        const int bc0 = std::clamp(static_cast<int>((sx / xs) / 7) - 1, 0, 38);
        const int bc1 = std::clamp(static_cast<int>(((sx + vis.x) / xs) / 7) + 1, 0, 38);
        const ImU32 seamCol = IM_COL32(255, 0, 0, 110);
        for (int y = y0; y <= y1; ++y)
            for (int bc = bc0; bc <= bc1; ++bc)
                if (hgrpaint::byteHasPaletteSeam(shadow.data(), bc, y)) {
                    const float fx = origin.x + (bc + 1) * 7 * xs;  // seam at byte boundary
                    const float fy = origin.y + y * scale;
                    dl->AddRect(ImVec2(fx - xs, fy), ImVec2(fx + xs, fy + scale), seamCol);
                }
    }

    // ── DHGR fringing overlay: outline colour pixels whose RENDERED dots (the
    // canvas came through the real NTSC pipeline) deviate from their aligned
    // block colour — exactly where the sliding window bleeds across a colour
    // transition. Only meaningful on the colour canvas.
    if (showConflicts && dhgrMode && ntscColor) {
        const float sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
        const ImVec2 vis = ImGui::GetContentRegionAvail();
        const int y0 = std::clamp(static_cast<int>(sy / scale), 0, kHiresHeight - 1);
        const int y1 = std::clamp(static_cast<int>((sy + vis.y) / scale) + 1, 0, kHiresHeight - 1);
        const int x0 = std::clamp(static_cast<int>(sx / xs) - 1, 0, kDhgrWidth - 1);
        const int x1 = std::clamp(static_cast<int>((sx + vis.x) / xs) + 1, 0, kDhgrWidth - 1);
        const ImU32 seamCol = IM_COL32(255, 0, 0, 110);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const int c = hgrpaint::dhgrColorAt(shadow.data(), x, y);
                if (c < 0) continue;
                const uint32_t want = kGrPalette[c] & 0x00FFFFFF;
                bool fringed = false;
                for (int d = 0; d < 4 && !fringed; ++d)
                    fringed = (canvasRgba[static_cast<size_t>(y) * 560 + 4 * x + d]
                               & 0x00FFFFFF) != want;
                if (fringed) {
                    const float fx = origin.x + x * xs;
                    const float fy = origin.y + y * scale;
                    dl->AddRect(ImVec2(fx, fy), ImVec2(fx + xs, fy + scale), seamCol);
                }
            }
    }

    // Map mouse → logical pixel (DHGR: 140 fat colour pixels per line).
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int lx = static_cast<int>((mouse.x - origin.x) / xs);
    int ly = static_cast<int>((mouse.y - origin.y) / scale);
    lx = std::clamp(lx, 0, logicalW() - 1);
    ly = std::clamp(ly, 0, kHiresHeight - 1);
    if (hovered) { lastHoverX = lx; lastHoverY = ly; }

    const bool altDown = ImGui::GetIO().KeyAlt;
    const bool eyedrop = (tool == Tool::Eyedropper) || altDown;
    const HgrColor activeColor = (tool == Tool::Eraser) ? HgrColor::Black : color;

    // Shift constrains shapes: Line to 0/45/90°, Rect/Ellipse to a square. Used
    // for the live preview AND the committed geometry so they always agree.
    auto constrainEnd = [&]() -> std::pair<int,int> {
        int ex = lx, ey = ly;
        if (ImGui::GetIO().KeyShift) {
            const int ddx = ex - dragStartX, ddy = ey - dragStartY;
            if (tool == Tool::Line) {
                const int adx = std::abs(ddx), ady = std::abs(ddy);
                if (adx > 2 * ady)      ey = dragStartY;            // horizontal
                else if (ady > 2 * adx) ex = dragStartX;           // vertical
                else { const int m = (adx + ady) / 2;              // 45°
                       ex = dragStartX + (ddx < 0 ? -m : m);
                       ey = dragStartY + (ddy < 0 ? -m : m); }
            } else if (tool == Tool::Rectangle || tool == Tool::Ellipse) {
                const int m = std::max(std::abs(ddx), std::abs(ddy));
                ex = dragStartX + (ddx < 0 ? -m : m);
                ey = dragStartY + (ddy < 0 ? -m : m);
            }
            ex = std::clamp(ex, 0, logicalW() - 1);
            ey = std::clamp(ey, 0, kHiresHeight - 1);
        }
        return {ex, ey};
    };

    // ── Brush-footprint + colour-snapped cursor preview (HGR-08) ────────────
    if (hovered && !dragging && !eyedrop && !grMode &&
        (tool == Tool::Pencil || tool == Tool::Eraser)) {
        // DHGR has no column parity: the ghost sits on the fat pixel as-is.
        const int snapped = dhgrMode ? lx : hgrpaint::snapColumn(lx, activeColor);
        const ImU32 ghost = (swatchColor(activeColor) & 0x00FFFFFF) | 0x80000000;
        const int r = brushSize - 1;
        const float bx = origin.x + (snapped - r) * xs;
        const float by = origin.y + (ly - r) * scale;
        dl->AddRect(ImVec2(bx, by),
                    ImVec2(bx + (2 * r + 1) * xs, by + (2 * r + 1) * scale), ghost);
        // Marker showing parity nudge: actual click column vs snapped column.
        if (snapped != lx) {
            const float ax = origin.x + lx * xs;
            dl->AddLine(ImVec2(ax, by), ImVec2(ax, by + (2 * r + 1) * scale),
                        IM_COL32(255, 255, 0, 200));
        }
    }

    // ── Text caret: one glyph-cell box at the stamp origin ───────────────────
    if (tool == Tool::Text && textPlaced) {
        const float cxs = origin.x + textX * xs;
        const float cys = origin.y + textY * scale;
        const bool phase = (static_cast<int>(ImGui::GetTime() * 2.0) & 1) != 0;
        dl->AddRect(ImVec2(cxs, cys),
                    ImVec2(cxs + kBBFontGlyphW * xs, cys + kBBFontGlyphH * scale),
                    phase ? IM_COL32(255, 220, 60, 235) : IM_COL32(255, 220, 60, 110));
    }

    // ── Selection marching-ants (HGR-06) ────────────────────────────────────
    if (hasSel && !pasting) {
        const int sx0 = std::min(selX0, selX1), sx1 = std::max(selX0, selX1);
        const int sy0 = std::min(selY0, selY1), sy1 = std::max(selY0, selY1);
        const ImVec2 a(origin.x + sx0 * xs, origin.y + sy0 * scale);
        const ImVec2 b(origin.x + (sx1 + 1) * xs, origin.y + (sy1 + 1) * scale);
        const bool phase = (static_cast<int>(ImGui::GetTime() * 4.0) & 1) != 0;
        dl->AddRect(a, b, phase ? IM_COL32(255,255,255,255) : IM_COL32(0,0,0,255));
        dl->AddRect(ImVec2(a.x-1,a.y-1), ImVec2(b.x+1,b.y+1),
                    phase ? IM_COL32(0,0,0,255) : IM_COL32(255,255,255,255));
    }

    if (pasting) {
        // ── Floating paste (HGR-06): clip follows the cursor; click commits ──
        if (hovered) { pasteX = lx; pasteY = ly; }
        for (int cy = 0; cy < clip.h; ++cy)
            for (int cx = 0; cx < clip.w; ++cx) {
                ImU32 col;
                if (clip.sixteen) {
                    const int v = clip.idx[static_cast<size_t>(cy) * clip.w + cx];
                    if (v == 0) continue;
                    col = kGrPalette[v & 0x0F];
                } else {
                    const HgrColor c = clip.px[static_cast<size_t>(cy) * clip.w + cx];
                    if (c == HgrColor::Black) continue;
                    col = swatchColor(c);
                }
                const float px = origin.x + (pasteX + cx) * xs;
                const float py = origin.y + (pasteY + cy) * scale;
                dl->AddRectFilled(ImVec2(px, py), ImVec2(px + xs, py + scale),
                                  (col & 0x00FFFFFF) | 0xC0000000);
            }
        dl->AddRect(ImVec2(origin.x + pasteX * xs, origin.y + pasteY * scale),
                    ImVec2(origin.x + (pasteX + clip.w) * xs, origin.y + (pasteY + clip.h) * scale),
                    IM_COL32(255, 255, 0, 230));
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            pasteFloatingAt(pasteX, pasteY);
            pasting = false;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) pasting = false;   // cancel
    } else {
        // Right-button = quick freehand erase (paint black) for any drawing tool —
        // no need to switch to the Eraser. Runs only when no left-button op is open.
        if (!dragging && tool != Tool::Select && !eyedrop) {
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                rmbErase = true; beginStroke();
                lastX = lx; lastY = ly;
                paintBrush(lx, ly, HgrColor::Black);
            } else if (rmbErase && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                if (lx != lastX || ly != lastY) {
                    paintLine(lastX, lastY, lx, ly, HgrColor::Black);   // interpolate
                    lastX = lx; lastY = ly;
                }
            }
        }
        // Terminate the RMB erase OUTSIDE the start guard, so holding Alt (eyedrop)
        // or switching tool/Select mid-drag still commits the stroke to undo and
        // clears the flag — otherwise rmbErase stranded true, losing undo and
        // blocking left-button drawing (the `!rmbErase` guard below).
        if (rmbErase && (ImGui::IsMouseReleased(ImGuiMouseButton_Right) ||
                         !ImGui::IsMouseDown(ImGuiMouseButton_Right))) {
            commitStroke(); rmbErase = false;
        }

        if (!rmbErase && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (eyedrop) {
                if (grMode) {
                    const int gi = hgrpaint::grBlockColorAt(shadow.data(), lx / 7, ly / 4);
                    if (gi >= 0) grColor = gi;
                } else if (dlgrMode) {
                    const int gi = hgrpaint::dlgrBlockColorAt(shadow.data(), lx / 7, ly / 4);
                    if (gi >= 0) grColor = gi;
                } else if (dhgrMode) {
                    const int gi = hgrpaint::dhgrColorAt(shadow.data(), lx, ly);
                    if (gi >= 0) grColor = gi;
                } else {
                    color = hgrpaint::colorAt(shadow.data(), lx, ly);
                }
                if (tool == Tool::Eyedropper) { tool = prevTool; }  // one-shot revert
            } else if (tool == Tool::Select) {
                dragging = true; hasSel = true;
                dragStartX = lx; dragStartY = ly;
                selX0 = selX1 = lx; selY0 = selY1 = ly;
            } else if (tool == Tool::Text && !grMode && !dlgrMode) {
                // Place / move the caret; glyphs are stamped from the tool panel.
                // Grab keyboard focus into the text box next frame so typing lands
                // there instead of leaking to the emulated Apple-1 keyboard (the
                // host key path only yields when an ImGui text field WantTextInput).
                textPlaced = true; textX = lx; textY = ly; textHomeX = lx;
                focusTextInput = true;
            } else if (tool == Tool::Text) {
                // Text is glyph-based — too coarse for GR; ignore the click.
            } else {
                dragging = true;
                dragStartX = lx; dragStartY = ly;
                lastX = lx; lastY = ly;
                // Batch the bulk tools (shapes paint at release, fill floods many
                // bytes at once); leave freehand pencil/eraser/palette unbatched so
                // they keep updating the live host screen as you drag.
                const bool bulk = (tool == Tool::Line || tool == Tool::Rectangle ||
                                   tool == Tool::Ellipse || tool == Tool::Fill);
                beginStroke(bulk);
                if (tool == Tool::Pencil || tool == Tool::Eraser) paintBrush(lx, ly, activeColor);
                else if (tool == Tool::PaletteShift) paintPaletteByte(lx, ly);
                else if (tool == Tool::Fill) { floodFill(lx, ly, activeColor); commitStroke(); dragging = false; }
            }
        }

        if (dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (tool == Tool::Pencil || tool == Tool::Eraser) {
                if (lx != lastX || ly != lastY) {
                    paintLine(lastX, lastY, lx, ly, activeColor);   // interpolate fast drags
                    lastX = lx; lastY = ly;
                }
            } else if (tool == Tool::PaletteShift) {
                if (lx / 7 != lastX / 7 || ly != lastY) {
                    paintPaletteByte(lx, ly);
                    lastX = lx; lastY = ly;
                }
            } else if (tool == Tool::Select) {
                selX1 = lx; selY1 = ly;
            } else if (tool == Tool::Line) {
                const auto [ex, ey] = constrainEnd();
                const ImVec2 a(origin.x + (dragStartX + 0.5f) * xs, origin.y + (dragStartY + 0.5f) * scale);
                const ImVec2 b(origin.x + (ex + 0.5f) * xs, origin.y + (ey + 0.5f) * scale);
                dl->AddLine(a, b, swatchColor(activeColor), 1.5f);
            } else if (tool == Tool::Rectangle) {
                const auto [ex, ey] = constrainEnd();
                const ImVec2 a(origin.x + dragStartX * xs, origin.y + dragStartY * scale);
                const ImVec2 b(origin.x + (ex + 1) * xs, origin.y + (ey + 1) * scale);
                if (rectFilled) dl->AddRectFilled(a, b, (swatchColor(activeColor) & 0x00FFFFFF) | 0x80000000);
                else            dl->AddRect(a, b, swatchColor(activeColor), 0, 0, 1.5f);
            } else if (tool == Tool::Ellipse) {
                const auto [ex, ey] = constrainEnd();
                const ImVec2 center(origin.x + (dragStartX + ex + 1) * 0.5f * xs,
                                    origin.y + (dragStartY + ey + 1) * 0.5f * scale);
                const ImVec2 radius(std::abs(ex - dragStartX) * 0.5f * xs + 0.5f,
                                    std::abs(ey - dragStartY) * 0.5f * scale + 0.5f);
                if (rectFilled) dl->AddEllipseFilled(center, radius, (swatchColor(activeColor) & 0x00FFFFFF) | 0x80000000);
                else            dl->AddEllipse(center, radius, swatchColor(activeColor), 0.0f, 0, 1.5f);
            }
        }

        if (dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const auto [ex, ey] = constrainEnd();
            if (tool == Tool::Line) paintLine(dragStartX, dragStartY, ex, ey, activeColor);
            else if (tool == Tool::Rectangle) paintRect(dragStartX, dragStartY, ex, ey, activeColor, rectFilled);
            else if (tool == Tool::Ellipse) paintEllipse(dragStartX, dragStartY, ex, ey, activeColor, rectFilled);
            else if (tool == Tool::Select) { selX1 = lx; selY1 = ly; }
            // Commit the open stroke (no-op for Select, whose edits aren't a stroke,
            // and for Fill, which already committed on click).
            commitStroke();
            dragging = false;
        }
    }

    // Capture canvas scroll/view metrics so the navigator thumbnail in the left
    // tool panel (drawn earlier this frame, from last frame's values) can map the
    // visible viewport and recentre on click.
    canvasScrollX    = ImGui::GetScrollX();
    canvasScrollY    = ImGui::GetScrollY();
    canvasScrollMaxX = ImGui::GetScrollMaxX();
    canvasScrollMaxY = ImGui::GetScrollMaxY();
    canvasViewW      = ImGui::GetWindowSize().x;
    canvasViewH      = ImGui::GetWindowSize().y;
    canvasScale      = scale * af;   // screen px per 280-equivalent column
    canvasScaleY     = scale;

    ImGui::EndChild();
}

void hgrpaint::HgrPaintEditor::openImportPreview(const std::string& path)
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::string err;
    if (!hgrpaint::decodeImageFile(path, w, h, rgba, err)) {
        status = "Import failed: " + err;
        return;
    }
    importSrcRgba = std::move(rgba);
    importSrcW = w; importSrcH = h;
    importSrcName = std::filesystem::path(path).filename().string();
    importCropActive = false;          // fresh image → import the whole frame
    importCropDragging = false;
    importDirty = true;
    importSrcTexDirty = true;
    importPreviewOpen = true;
}

void hgrpaint::HgrPaintEditor::renderImportPreview()
{
    // Free the (potentially tens-of-MB) decoded source + its full-res GPU texture
    // once the dialog has closed — Apply / Cancel / Esc all land here next frame
    // (popup not open, no pending re-open). swap() reclaims capacity, not just size.
    if (!importPreviewOpen && !importSrcRgba.empty() &&
        !ImGui::IsPopupOpen("Import preview##hgr")) {
        std::vector<uint8_t>().swap(importSrcRgba);
        std::vector<uint8_t>().swap(importPage);
        std::vector<uint32_t>().swap(importPreview);
        if (importSrcTex && host) { host->destroyTexture(importSrcTex); importSrcTex = nullptr; }
        // The preview GPU texture also follows the popup's lifetime — until
        // we destroy it here, the ~210 KB RGBA8 stays resident on the
        // renderer for the rest of the session even though no widget
        // references it. Pair it with importSrcTex so both go together.
        if (importPreviewTex && host) { host->destroyTexture(importPreviewTex); importPreviewTex = nullptr; }
        importSrcW = importSrcH = 0;
        importSrcTexDirty = true;
    }

    if (importPreviewOpen) { ImGui::OpenPopup("Import preview##hgr"); importPreviewOpen = false; importDirty = true; }

    const ImVec2 vpCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(960.0f, 600.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Import preview##hgr", nullptr, ImGuiWindowFlags_NoCollapse))
        return;

    ImGui::TextUnformatted(dhgrMode
        ? (importDhgrModel == 0
           ? "Image \xE2\x86\x92 DHGR  (560 dots, NTSC sliding window, CAM16-UCS dithering)"
           : importDhgrModel == 1
           ? "Image \xE2\x86\x92 DHGR  (140x192 blocks, 16 colours, CAM16-UCS dithering)"
           : importDhgrModel == 2
           ? "Image \xE2\x86\x92 DHGR mono  (560x192 1-bit, luma dithering)"
           : "Image \xE2\x86\x92 DHGR NTSC  (8-px chroma, 86 colours, composite targets)")
        : dlgrMode
        ? "Image \xE2\x86\x92 DLGR  (80x48 blocks, 16 colours, CAM16-UCS dithering)"
        : grMode
        ? "Image \xE2\x86\x92 GR lo-res  (40x48 blocks, CAM16-UCS perceptual dithering)"
        : "Image \xE2\x86\x92 HGR  (ii-pix: CAM16-UCS perceptual dithering)");
    ImGui::TextDisabled("%s  \xE2\x80\x94  %d x %d source", importSrcName.c_str(), importSrcW, importSrcH);
    ImGui::Separator();

    // Source-thumbnail dimensions (whole image): cap the long side at 360 px, but
    // also keep a minimum extent so a pathological aspect ratio (very tall/narrow
    // or very wide source) can't collapse to a sub-pixel, undraggable strip.
    const float ph = kHiresHeight * 2.0f;       // result preview height (384)
    float sw = ph * (importSrcH ? static_cast<float>(importSrcW) / importSrcH : 1.0f), sh = ph;
    if (sw > 360.0f) { sw = 360.0f; sh = importSrcW ? sw * importSrcH / importSrcW : ph; }
    sw = std::max(sw, 48.0f);
    sh = std::max(sh, 48.0f);

    // Click-vs-crop discriminator measured in SCREEN pixels, not source pixels, so
    // sensitivity doesn't scale with source resolution (a 1px jitter on a 4000px
    // image no longer becomes an accidental crop). Shared by the reconvert gate,
    // the release commit, and the dimming overlay so they always agree.
    auto cropUsable = [&]() {
        if (importSrcW <= 0 || importSrcH <= 0) return false;
        return (importCropX1 - importCropX0) * sw / importSrcW >= 5.0f &&
               (importCropY1 - importCropY0) * sh / importSrcH >= 5.0f;
    };

    // ── Reconvert + re-render the HGR preview when anything changed ───────────
    if (importDirty && !importSrcRgba.empty()) {
        importDirty = false;
        hgrpaint::ImportOptions opt;
        opt.stretch    = importStretch;
        opt.dither     = importDither;
        // opt.serpentine is deliberately NOT set: it is deprecated for HGR (the
        // NTSC decode is physically left-to-right; the converter's refinement
        // passes replace what serpentine papered over) — see HgrConvert.h.
        opt.diffusion  = importDiffusion;
        opt.brightness = importBrightness;
        opt.contrast   = importContrast;
        opt.gamma      = importGamma;
        opt.chromaWeight = 6.0f - importColourNoise * 5.2f;   // 0→6 clean, 1→0.8 vivid
        opt.kernel = importKernel ? hgrpaint::DitherKernel::JarvisMod
                                  : hgrpaint::DitherKernel::FloydSteinberg;
        // Apply the crop both when committed AND while dragging (importCropActive is
        // only set on release), so the FIRST crop drag shows a live cropped preview
        // instead of the uncropped page. A sub-minimum rect falls through to whole.
        if ((importCropActive || importCropDragging) && cropUsable()) {
            opt.cropX0 = importCropX0; opt.cropY0 = importCropY0;
            opt.cropX1 = importCropX1; opt.cropY1 = importCropY1;
        }
        // GR (lo-res) and DHGR have no NTSC coupling in the block model — flat
        // grids of 16-colour cells — so they use the simple CAM16-UCS block
        // quantisers. 280-HIRES keeps the ii-pix analysis-by-synthesis path.
        importPage.assign(static_cast<size_t>(pageBytes()), 0);
        if (dhgrMode && importDhgrModel == 0)
            hgrpaint::imageToDhgrPage560(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (dhgrMode && importDhgrModel == 2)
            hgrpaint::imageToDhgrMonoPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (dhgrMode && importDhgrModel == 3)
            hgrpaint::imageToDhgrPage560Ntsc(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (dhgrMode)
            hgrpaint::imageToDhgrPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (dlgrMode)
            hgrpaint::imageToDlgrPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (grMode)
            hgrpaint::imageToGrPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else
            hgrpaint::imageToHgrPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        importPreview.assign(static_cast<size_t>(texW()) * kHiresHeight, 0);
        if (host) {
            if (dhgrMode)
                // The mono model previews through the mono pipeline — its
                // dither patterns would read as artifact-colour confetti in
                // colour.
                host->renderDhgrPage(importPage.data(), importPage.data() + kHiresSize,
                                     importPreview.data(), importDhgrModel == 2);
            else if (dlgrMode)
                host->renderDlgrPage(importPage.data(), importPage.data() + 0x400,
                                     importPreview.data(), false);
            else
                host->renderHgrPage(importPage.data(), importPreview.data(), false, grMode);
            importPreviewTex = host->uploadTexture(importPreviewTex, importPreview.data(),
                                                   texW(), kHiresHeight, /*linear=*/false);
        }
    }

    // ── Upload the source thumbnail once (for the side-by-side view) ──────────
    if (importSrcTexDirty && !importSrcRgba.empty() && host) {
        importSrcTexDirty = false;
        importSrcTex = host->uploadTexture(importSrcTex, importSrcRgba.data(),
                                           importSrcW, importSrcH, /*linear=*/true);
    }

    // ── Side-by-side: source (left) | HGR result (right) — kept at the top ────
    ImGui::BeginGroup();
    ImGui::TextDisabled("Source  \xE2\x80\x94  drag to select a crop region");
    if (importSrcTex && importSrcW > 0 && importSrcH > 0) {
        const ImVec2 imgPos = ImGui::GetCursorScreenPos();
        ImGui::Image(host->textureToImTexture(importSrcTex), ImVec2(sw, sh));

        // Overlay an invisible button at the same spot so we capture drags
        // without ImGui swallowing them as a window move.
        ImGui::SetCursorScreenPos(imgPos);
        ImGui::InvisibleButton("##cropsrc", ImVec2(sw, sh));
        const bool hov = ImGui::IsItemHovered();

        // Screen <-> source-pixel mapping (the thumbnail shows the whole image).
        auto toSrc = [&](const ImVec2& p, int& sx, int& sy) {
            float fx = (p.x - imgPos.x) / sw, fy = (p.y - imgPos.y) / sh;
            fx = fx < 0.0f ? 0.0f : (fx > 1.0f ? 1.0f : fx);
            fy = fy < 0.0f ? 0.0f : (fy > 1.0f ? 1.0f : fy);
            sx = static_cast<int>(fx * importSrcW + 0.5f);
            sy = static_cast<int>(fy * importSrcH + 0.5f);
        };
        auto toScreen = [&](int sx, int sy) {
            return ImVec2(imgPos.x + static_cast<float>(sx) / importSrcW * sw,
                          imgPos.y + static_cast<float>(sy) / importSrcH * sh);
        };

        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            importCropDragging = true;
            toSrc(ImGui::GetIO().MousePos, importCropAnchorX, importCropAnchorY);
        }
        if (importCropDragging) {
            int mx, my;
            toSrc(ImGui::GetIO().MousePos, mx, my);
            const int nx0 = std::min(importCropAnchorX, mx);
            const int ny0 = std::min(importCropAnchorY, my);
            const int nx1 = std::max(importCropAnchorX, mx);
            const int ny1 = std::max(importCropAnchorY, my);
            // Reconvert (a full ii-pix pass, tens of ms) ONLY when the rect actually
            // changes — holding the mouse still no longer re-runs the converter.
            if (nx0 != importCropX0 || ny0 != importCropY0 ||
                nx1 != importCropX1 || ny1 != importCropY1) {
                importCropX0 = nx0; importCropY0 = ny0;
                importCropX1 = nx1; importCropY1 = ny1;
                importDirty = true;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                importCropDragging = false;
                // A real drag (>= ~5 screen px) selects a crop; a click clears it.
                importCropActive = cropUsable();
                importDirty = true;
            }
        }

        // Draw the crop rectangle + dim the area outside it. Only dim once the rect
        // is usable, so a not-yet-large-enough drag doesn't dim the whole image
        // while the HGR result still previews uncropped (the two halves agreeing).
        if (importCropActive || importCropDragging) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = toScreen(importCropX0, importCropY0);
            const ImVec2 b = toScreen(importCropX1, importCropY1);
            if (cropUsable()) {
                const ImVec2 tl(imgPos.x, imgPos.y), br(imgPos.x + sw, imgPos.y + sh);
                const ImU32 dim = IM_COL32(0, 0, 0, 110);
                dl->AddRectFilled(tl, ImVec2(br.x, a.y), dim);            // above
                dl->AddRectFilled(ImVec2(tl.x, b.y), br, dim);            // below
                dl->AddRectFilled(ImVec2(tl.x, a.y), ImVec2(a.x, b.y), dim);  // left
                dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(br.x, b.y), dim);  // right
            }
            dl->AddRect(a, b, IM_COL32(255, 215, 0, 255), 0.0f, 0, 2.0f);
        }
    }
    if (importCropActive) {
        ImGui::Text("Crop: %d,%d  %dx%d", importCropX0, importCropY0,
                    importCropX1 - importCropX0, importCropY1 - importCropY0);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear crop")) {
            importCropActive = false; importCropDragging = false; importDirty = true;
        }
    } else {
        ImGui::TextDisabled("Crop: whole image");
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled(dhgrMode ? "DHGR result" : dlgrMode ? "DLGR result"
                        : grMode ? "GR result" : "HGR result");
    if (importPreviewTex && host)
        ImGui::Image(host->textureToImTexture(importPreviewTex),
                     ImVec2(kHiresWidth * 2.0f, ph));
    ImGui::EndGroup();
    ImGui::Separator();

    // ── Live controls (any change re-converts the preview next frame) ─────────
    ImGui::SetNextItemWidth(-180);
    importDirty |= ImGui::SliderFloat("Colour noise", &importColourNoise, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Left: flat greys dither clean black/white.\n"
                          "Right: more vivid colour (and more colour speckle).");
    ImGui::SetNextItemWidth(-180);
    importDirty |= ImGui::SliderFloat("Brightness", &importBrightness, 0.3f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(-180);
    importDirty |= ImGui::SliderFloat("Contrast", &importContrast, 0.4f, 2.5f, "%.2f");
    ImGui::SetNextItemWidth(-180);
    importDirty |= ImGui::SliderFloat("Gamma", &importGamma, 0.4f, 2.5f, "%.2f");
    ImGui::SetNextItemWidth(-180);
    importDirty |= ImGui::SliderFloat("Diffusion (grain)", &importDiffusion, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Error-diffusion strength.\n"
                          "1 = full dithering/grain, lower = smoother but flatter.\n"
                          "0.7 is ii-pix's HGR default.");
    ImGui::SetNextItemWidth(-180);
    {
        const char* kKernelNames[] = { "Floyd-Steinberg", "Jarvis-mod" };
        importDirty |= ImGui::Combo("Diffusion kernel", &importKernel, kKernelNames, 2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Jarvis-mod pushes the error 4 pixels forward - the same\n"
                              "reach as an NTSC sliding-window transition (ii-pix's HGR\n"
                              "choice, smoother). Floyd-Steinberg is the classic grain.");
    }
    if (dhgrMode) {
        static const char* kDhgrModels[] = {
            "560 dots (lookahead)", "140 px blocks (Dazzle Draw)",
            "560 mono (1-bit)", "560 NTSC 8-px (composite)" };
        ImGui::SetNextItemWidth(-180);
        if (ImGui::Combo("DHGR model", &importDhgrModel, kDhgrModels, 4))
            importDirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "560 dots: every dot chosen through the real NTSC sliding-window\n"
                "decode + cross-column refinement (ii-pix model) - full resolution,\n"
                "fringing avoided/exploited. Slower.\n"
                "140 px: independent 4-dot colour blocks (Dazzle Draw style) -\n"
                "instant, clean blocks that are easy to retouch, but half the\n"
                "resolution and fringing at colour seams.\n"
                "560 mono: 1-bit luma dither for monochrome displays.\n"
                "560 NTSC 8-px: scores against the 86 colours the 8-dot chroma\n"
                "bleed produces - pick a composite canvas pipeline (left tool\n"
                "panel) to preview it faithfully.");
        if (importDhgrModel == 3 && host && host->canvasPipeline() < 4)
            ImGui::TextDisabled("NTSC 8-px targets composite displays - switch the\n"
                                "canvas pipeline (left panel) to AppleWin NTSC or\n"
                                "OE composite so this preview shows the real colours.");
    }
    importDirty |= ImGui::Checkbox("Dither", &importDither);
    ImGui::SameLine();
    // Serpentine is deprecated for HGR (the converter ignores it): the NTSC
    // half-dot carry + sliding window are physically left-to-right, so a
    // right-to-left dither row would score against a fabricated left word. The
    // refinement passes fix the diagonal smear it used to hide.
    ImGui::BeginDisabled(true);
    ImGui::Checkbox("Serpentine", &importSerpentine);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Not applicable to HGR: the NTSC decode is physically\n"
                          "left-to-right, so alternating the scan direction would\n"
                          "dither against a fabricated left context.");
    ImGui::SameLine();
    importDirty |= ImGui::Checkbox("Stretch", &importStretch);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        importColourNoise = 0.30f; importBrightness = 1.0f; importContrast = 1.0f;
        importGamma = 1.0f; importDiffusion = 0.7f; importDither = true;
        importSerpentine = true; importStretch = false; importKernel = 1;
        importDirty = true;
        importCropActive = false; importCropDragging = false;
    }
    ImGui::Separator();

    // Arm the imported source as an onion-skin tracing layer over the canvas.
    // Placement mirrors the resampler's fit/letterbox in visual 280-eq space
    // (and the crop window becomes the texture UVs) so the overlay lines up
    // with what Apply would put on the page.
    if (ImGui::Button("Onion skin", ImVec2(110, 0)) && !importSrcRgba.empty() && host) {
        int cx0 = 0, cy0 = 0, cx1 = importSrcW, cy1 = importSrcH;
        if ((importCropActive || importCropDragging) && cropUsable()) {
            cx0 = importCropX0; cy0 = importCropY0;
            cx1 = importCropX1; cy1 = importCropY1;
        }
        const float cw = static_cast<float>(cx1 - cx0), ch = static_cast<float>(cy1 - cy0);
        if (importStretch) {
            onionX0 = 0; onionY0 = 0; onionW = 280; onionH = 192;
        } else {
            const float s = std::min(280.0f / cw, 192.0f / ch);
            onionW = cw * s; onionH = ch * s;
            onionX0 = (280.0f - onionW) * 0.5f;
            onionY0 = (192.0f - onionH) * 0.5f;
        }
        onionU0 = static_cast<float>(cx0) / importSrcW;
        onionV0 = static_cast<float>(cy0) / importSrcH;
        onionU1 = static_cast<float>(cx1) / importSrcW;
        onionV1 = static_cast<float>(cy1) / importSrcH;
        onionTex = host->uploadTexture(onionTex, importSrcRgba.data(),
                                       importSrcW, importSrcH, /*linear=*/true);
        onionShow = true;
        status = "Onion skin armed: " + importSrcName;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep this source as a tracing overlay on the canvas\n"
                          "(toggle + opacity in the left tool panel).");
    ImGui::SameLine();
    if (ImGui::Button("Apply to page", ImVec2(130, 0)) && !importPage.empty()) {
        beginStroke(true);
        // Bound by pageBytes(), not shadow.size(): in GR mode baseAddr() is $0400
        // and the live page is only 0x400 bytes (the importer fills just those),
        // so iterating the full 0x2000 shadow would poke zeros past the page — see
        // the same guard on the save path.
        const int limit = std::min({pageBytes(), static_cast<int>(shadow.size()),
                                    static_cast<int>(importPage.size())});
        for (int off = 0; off < limit; ++off) {
            // Text pages: never touch the screen holes (per plane in DLGR).
            if ((grMode || dlgrMode) && hgrpaint::grIsScreenHole(off & 0x3FF)) continue;
            if (importPage[off] == shadow[off]) continue;
            const uint8_t old = shadow[off];
            shadow[off] = importPage[off];
            emitShadowEdit(off, old);
        }
        commitStroke();
        status = "Imported " + importSrcName;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void hgrpaint::HgrPaintEditor::openFileBrowser(bool forSave, int saveKind, bool importMode)
{
    browserForSave = forSave;
    browserImport = importMode;
    browserSaveKind = saveKind;
    if (browserDir.empty()) {
        // Prefer the host's context folder (POM1 → sdcard/HGR/), else the CWD.
        if (host) browserDir = host->browseDir();
        if (browserDir.empty()) {
            std::error_code ec;
            browserDir = std::filesystem::current_path(ec).string();
            if (ec || browserDir.empty()) browserDir = ".";
        }
    }
    // Seed a default filename for Save from the current path's basename.
    std::string defName;
    if (forSave) {
        std::string base = std::filesystem::path(filePath).filename().string();
        if (saveKind == 1) {                          // PNG export
            if (base.empty()) base = "image.png";
        } else {                                      // raw HGR page → SD-card tag
            base = sdCardDefaultName(base, baseAddr());
        }
        std::snprintf(browserSaveName, sizeof(browserSaveName), "%s", base.c_str());
        defName = base;
    }

    // Prefer the host's OS-native file picker; fall back to the in-process ImGui
    // browser below when the host has none (WASM, Linux without zenity/kdialog,
    // or a headless/test host). Matches the MainWindow dialogs' native+fallback.
    if (host) {
        std::string title, desc, ext;
        if (importMode)        { title = "Import picture";  desc = "Images (PNG, JPG, BMP)"; ext = "png,jpg,jpeg,bmp,gif,tga"; }
        else if (saveKind == 1){ title = "Export PNG";      desc = "PNG image";             ext = "png"; }
        else                   { title = forSave ? "Save HGR image" : "Load HGR image";
                                 // No extension filter: SD CARD OS images are named
                                 // NAME#062000 (no extension), so *.hgr would hide
                                 // them. Empty ext = all files, and on save it also
                                 // disables saveFile's single-extension auto-append
                                 // so the #06 tag isn't mangled into "...#062000.hgr".
                                 desc = forSave ? "HGR raw (saved as NAME#06xxxx)"
                                                : "HGR raw / SD-card image (NAME#06xxxx)";
                                 ext  = ""; }
        std::string picked;
        if (host->pickFilePath(forSave, title, desc, ext, browserDir, defName, picked)) {
            // Track where the user went so the next pick + the ImGui fallback
            // start there too.
            std::error_code ec;
            std::filesystem::path pp(picked);
            std::string dir = pp.parent_path().string();
            if (!dir.empty()) browserDir = dir;
            performFileAction(forSave, saveKind, importMode, picked);
            return;
        }
        // A native picker that returned false means the user CANCELLED (or it
        // errored) — stay put. Only fall back to the ImGui browser when the host
        // has no native picker at all (WASM, Linux without zenity/kdialog).
        if (host->nativeFilePickerAvailable()) return;
    }
    browserOpen = true;
}

bool hgrpaint::HgrPaintEditor::performFileAction(bool forSave, int saveKind,
                                                 bool importMode,
                                                 const std::string& fullPath)
{
    namespace fs = std::filesystem;
    const std::string name = fs::path(fullPath).filename().string();

    if (!forSave && importMode) {
        // Don't touch filePath: openImportPreview takes the path directly, and a
        // .jpg there would poison the next Save's default raw-HGR filename.
        openImportPreview(fullPath);
        return true;
    }
    if (!forSave) {                                       // Load raw page
        std::snprintf(filePath, sizeof(filePath), "%s", fullPath.c_str());
        std::string err;
        // Purge the page first: loadImage writes only the file's own length, so a
        // file shorter than the full page (e.g. an 8184-byte "no screen-hole"
        // dump) would otherwise leave a stale tail. renderCanvas re-reads VRAM
        // into `shadow` next frame, so the canvas reflects the load by itself.
        // (In DHGR the offsets walk both planes via addrOfShadowOff; in GR the
        // screen holes are skipped — peripheral firmware scratch.)
        if (host) {
            host->beginBatch();
            for (int off = 0; off < pageBytes(); ++off) {
                if ((grMode || dlgrMode) && hgrpaint::grIsScreenHole(off & 0x3FF))
                    continue;
                hostPoke(addrOfShadowOff(off), 0);
            }
            host->endBatch();
        }
        bool ok = false;
        if (dhgrMode) {
            ok = host && host->loadDhgrImage(filePath, baseAddr(), err);
        } else if (grMode || dlgrMode) {
            // Text-page loads go byte-by-byte through pokes instead of the
            // host's raw loader, so the file's screen-hole bytes (stale
            // scratch from whatever machine saved it) never land in the LIVE
            // holes. DLGR reads the 2 KB pair (aux plane first, like save).
            std::ifstream in(fullPath, std::ios::binary);
            if (!in) {
                err = "cannot open " + fullPath;
            } else if (host) {
                std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
                const int n = std::min<int>(static_cast<int>(bytes.size()), pageBytes());
                host->beginBatch();
                for (int off = 0; off < n; ++off) {
                    if (hgrpaint::grIsScreenHole(off & 0x3FF)) continue;
                    hostPoke(addrOfShadowOff(off), static_cast<uint8_t>(bytes[off]));
                }
                host->endBatch();
                ok = true;
            }
        } else {
            ok = host && host->loadImage(filePath, baseAddr(), err);
        }
        if (ok) {
            char addr[8];
            std::snprintf(addr, sizeof(addr), "%04X", baseAddr());  // mode-aware
            status = "Loaded " + name + " into $" + addr;
            return true;
        }
        status = "Load failed: " + (err.empty() ? std::string("(bad file)") : err);
        return false;                                     // surface the failure
    }

    // Save (raw page dump or PNG export).
    std::snprintf(filePath, sizeof(filePath), "%s", fullPath.c_str());
    std::string err;
    bool ok = false;
    if (saveKind == 1) {                                  // PNG export
        ok = host && host->savePng(fullPath, canvasRgba.data(),
                                   texW(), kHiresHeight, err);
        status = ok ? ("Exported PNG: " + fullPath)
                    : ("PNG export failed: " + (err.empty() ? std::string("(error)") : err));
    } else {                                              // raw page dump
        // Guarantee the SD-CARD-OS tag: if the final name has no '#', append
        // "#06AAAA" (type 06 = binary, AAAA = page load address) so the file is
        // directly `@L NAME` / `LOAD NAME`-able at its page address on the
        // Apple-1. Operates on the basename only; the directory is preserved.
        fs::path outP(fullPath);
        if (outP.filename().string().find('#') == std::string::npos) {
            outP = outP.parent_path() /
                   sdCardDefaultName(outP.filename().string(), baseAddr());
        }
        const std::string outPath = outP.string();
        std::snprintf(filePath, sizeof(filePath), "%s", outPath.c_str());

        // 280-HIRES only: bake the POM1HGR tag into the unused screen-hole bytes
        // ($1FF8-$1FFF) — past the last displayed byte, so invisible. The lo-res
        // page is just 1 KB and has no such screen hole; DHGR dumps stay pristine
        // A2FC (both planes' screen holes belong to the picture format).
        if (!grMode && !dhgrMode && !dlgrMode) {
            static const char kTag[8] = { 'P','O','M','1','H','G','R','\0' };
            for (int i = 0; i < 8; ++i) {
                const int off = 0x1FF8 + i;
                shadow[off] = static_cast<uint8_t>(kTag[i]);
                if (host) host->pokeByte(static_cast<uint16_t>(baseAddr() + off),
                                         static_cast<uint8_t>(kTag[i]));
            }
        }
        ok = host && (dhgrMode ? host->saveDhgrImage(outPath, baseAddr(), err)
                    : dlgrMode ? host->saveDlgrImage(outPath, baseAddr(), err)
                    : host->saveImage(outPath, baseAddr(), pageBytes(), err));
        const std::string outName = fs::path(outPath).filename().string();
        status = ok ? (dhgrMode ? ("Saved 16 KB DHGR (A2FC, aux+main): " + outName)
                     : dlgrMode ? ("Saved 2 KB DLGR pair (aux+main): " + outName)
                     : grMode   ? ("Saved 1 KB lo-res GR page: " + outName)
                                : ("Saved 8 KB HGR (+POM1HGR tag): " + outName))
                    : ("Save failed: " + (err.empty() ? std::string("(error)") : err));
    }
    return ok;
}

void hgrpaint::HgrPaintEditor::renderFileBrowser()
{
    namespace fs = std::filesystem;
    if (browserOpen) { ImGui::OpenPopup("HGR File##browser"); browserOpen = false; }

    const ImVec2 vpCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600, 460), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("HGR File##browser", nullptr, ImGuiWindowFlags_NoCollapse))
        return;

    ImGui::TextUnformatted(browserForSave
        ? (browserSaveKind == 1 ? "Save PNG export"
           : dhgrMode ? "Save DHGR image (16 KB, A2FC)" : "Save HGR image (8 KB)")
        : (browserImport ? (dhgrMode ? "Import picture (PNG / JPG / BMP) — converted to DHGR"
                            : grMode ? "Import picture (PNG / JPG / BMP) — converted to GR (lo-res)"
                                     : "Import picture (PNG / JPG / BMP) — converted to HGR")
                         : (dhgrMode
                            ? "Load DHGR image (pick a file — 16 KB ones are highlighted)"
                            : "Load HGR image (pick a file — 8 KB ones are highlighted)")));
    ImGui::TextDisabled("%s", browserDir.c_str());
    ImGui::Separator();

    // ── Directory + file listing ─────────────────────────────────────────────
    ImGui::BeginChild("##fblist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.0f), true);
    if (ImGui::Selectable("../", false)) {
        std::error_code ec;
        fs::path up = fs::path(browserDir).parent_path();
        if (!up.empty()) browserDir = up.string();
        (void)ec;
    }
    std::vector<fs::directory_entry> dirs, files;
    try {
        for (const auto& e : fs::directory_iterator(browserDir,
                 fs::directory_options::skip_permission_denied)) {
            std::error_code ec;
            if (e.is_directory(ec)) dirs.push_back(e);
            else if (e.is_regular_file(ec)) files.push_back(e);
        }
    } catch (...) {
        ImGui::TextDisabled("(cannot read this directory)");
    }
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename().string() < b.path().filename().string();
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    for (const auto& d : dirs) {
        const std::string name = d.path().filename().string();
        if (ImGui::Selectable((name + "/").c_str(), false))
            browserDir = d.path().string();
    }
    for (const auto& f : files) {
        std::error_code ec;
        const std::uintmax_t sz = f.file_size(ec);
        const std::string name = f.path().filename().string();
        // Highlight the files that suit the current mode: 8 KB raw pages for
        // Load/Save, image files for Import.
        std::string ext = f.path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(c));
        const bool isImg = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                            ext == ".bmp" || ext == ".gif" || ext == ".tga");
        const bool relevant = browserImport
            ? isImg
            : (!ec && (dhgrMode ? (sz >= 16000 && sz <= 16384)
                     : dlgrMode ? (sz >= 2000 && sz <= 2048)
                                : (sz >= 8000 && sz <= 8192)));
        char label[320];
        std::snprintf(label, sizeof(label), "%-28s %8llu B", name.c_str(),
                      static_cast<unsigned long long>(sz));
        if (!relevant) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 150, 255));
        if (ImGui::Selectable(label, false)) {
            if (browserForSave) {
                std::snprintf(filePath, sizeof(filePath), "%s", f.path().string().c_str());
                std::snprintf(browserSaveName, sizeof(browserSaveName), "%s", name.c_str());
            } else {                       // Load or Import: act immediately
                performFileAction(false, browserSaveKind, browserImport, f.path().string());
                ImGui::CloseCurrentPopup();
            }
        }
        if (!relevant) ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // (No import options here: picking a file opens the preview dialog, which
    // exposes Stretch/Dither plus the full live controls — duplicating them in
    // this browser only pre-seeded state the preview immediately supersedes.)
    if (!browserForSave && browserImport)
        ImGui::TextDisabled("Pick an image \xE2\x80\x94 crop + dither options open in the preview.");

    // ── Action row ───────────────────────────────────────────────────────────
    if (browserForSave) {
        ImGui::SetNextItemWidth(-160);
        ImGui::InputText("##fbname", browserSaveName, sizeof(browserSaveName));
        ImGui::SameLine();
        if (ImGui::Button("Save", ImVec2(70, 0)) && browserSaveName[0]) {
            const std::string full = (fs::path(browserDir) / browserSaveName).string();
            if (performFileAction(true, browserSaveKind, false, full))
                ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel", ImVec2(70, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void hgrpaint::HgrPaintEditor::renderFileRow()
{
    if (ImGui::Button("Load")) openFileBrowser(false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(dhgrMode
            ? "Open a file picker and load a raw 16 KB DHGR (A2FC) image"
            : "Open a file picker and load a raw 8 KB HGR image");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_IMAGE " Import")) openFileBrowser(false, 0, /*importMode=*/true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(dhgrMode
            ? "Import a PNG/JPG picture and convert it to DHGR\n"
              "(140x192, quantised to the 16 Apple II colours with CAM16-UCS dithering)"
            : grMode
            ? "Import a PNG/JPG picture and convert it to GR (lo-res)\n"
              "(40x48 blocks, quantised to the 16 lo-res colours with CAM16-UCS dithering)"
            : "Import a PNG/JPG picture and convert it to HGR\n"
              "(ii-pix-style: CAM16-UCS perceptual dithering vs the true NTSC colours)");
    ImGui::SameLine();
    if (ImGui::Button("Save")) openFileBrowser(true, /*kind=*/0);
    ImGui::SameLine();
    if (ImGui::Button("Save PNG")) openFileBrowser(true, /*kind=*/1);
    if (!status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", status.c_str()); }
}

void hgrpaint::HgrPaintEditor::renderStatusBar(int lx, int ly, bool hovered)
{
    // Persistent info line (HGR-04): coords, byte, parity-snapped column, tool,
    // colour swatch, page, zoom, undo/redo depth. Survives the mouse leaving the
    // canvas via the cached lastHoverX/Y.
    const char* toolNames[] = { "Pencil", "Eraser", "Line", "Rect", "Ellipse",
                                "Fill", "Eyedropper", "Select", "Palette", "Text" };
    const HgrColor activeColor = (tool == Tool::Eraser) ? HgrColor::Black : color;

    if (hovered && lx >= 0 && ly >= 0 && dlgrMode) {
        ImGui::Text("x=%3d y=%3d  block=%d,%d  colour %d", lx, ly, lx / 7, ly / 4,
                    hgrpaint::dlgrBlockColorAt(shadow.data(), lx / 7, ly / 4));
    } else if (hovered && lx >= 0 && ly >= 0 && dhgrMode) {
        // DHGR: fat colour pixel + the MAIN-plane byte of its first dot (the aux
        // byte lives at the same address in the other bank).
        int offs[2];
        const int n = hgrpaint::dhgrPixelOffsets(lx, ly, offs);
        const int mainOff = (n > 0)
            ? (offs[0] >= kHiresSize ? offs[0] - kHiresSize : offs[0]) : 0;
        ImGui::Text("x=%3d y=%3d  byte=$%04X  colour %d", lx, ly,
                    baseAddr() + mainOff,
                    hgrpaint::dhgrColorAt(shadow.data(), lx, ly));
    } else if (hovered && lx >= 0 && ly >= 0) {
        const int snapped = hgrpaint::snapColumn(lx, activeColor);
        ImGui::Text("x=%3d y=%3d  byte=$%04X  col->%d", lx, ly,
                    baseAddr() + hgrpaint::hgrByteOffset(snapped, ly), snapped);
    } else {
        ImGui::TextUnformatted("x=--- y=---  byte=$----  col->-");
    }
    ImGui::SameLine();
    ImGui::Text(" | %s ", toolNames[static_cast<int>(tool)]);
    ImGui::SameLine();
    // Active-colour swatch.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), swatchColor(activeColor));
        dl->AddRect(p, ImVec2(p.x + h, p.y + h), IM_COL32(160, 160, 160, 255));
        ImGui::Dummy(ImVec2(h, h));
    }
    ImGui::SameLine();
    ImGui::Text("%s | Page %d ($%04X) | Zoom %dx | Undo:%zu Redo:%zu",
                colorName(activeColor), page2 ? 2 : 1, baseAddr(),
                kZoomLadder[zoomIdx], undo.size(), redo.size());
}

void hgrpaint::HgrPaintEditor::handleShortcuts()
{
    // Keyboard shortcuts (HGR-02). Only act when the editor window (and its
    // children) is focused and no text widget wants input, so we never steal
    // keys from the file-path InputText or the main Apple 1 keyboard path.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;

    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    // Only remember the genuine previous tool — a redundant same-tool keypress must
    // not set prevTool=Eyedropper, which would strand the one-shot eyedropper revert.
    auto pick = [&](Tool t) { if (t != tool) prevTool = tool; tool = t; };

    if (io.KeyCtrl) {
        // Ctrl+Z = undo, Ctrl+Y or Ctrl+Shift+Z = redo, Ctrl+C/X/V = clipboard.
        if (pressed(ImGuiKey_Z)) { if (io.KeyShift) doRedo(); else doUndo(); }
        if (pressed(ImGuiKey_Y)) doRedo();
        if (pressed(ImGuiKey_C)) copySelection(false);
        if (pressed(ImGuiKey_X)) copySelection(true);
        if (pressed(ImGuiKey_V) && clipUsableHere()) {
            if (dragging) { commitStroke(); dragging = false; }   // flush an open stroke
            pasting = true;
            pasteX = hasSel ? std::min(selX0, selX1) : 0;
            pasteY = hasSel ? std::min(selY0, selY1) : 0;
        }
        return;   // don't let Ctrl combos fall through to plain-key tools
    }

    // Esc cancels a floating paste / clears the selection.
    if (pressed(ImGuiKey_Escape)) { pasting = false; hasSel = false; }
    // Arrow keys nudge a floating paste by 1px.
    if (pasting) {
        if (pressed(ImGuiKey_LeftArrow))  pasteX = std::max(pasteX - 1, 0);
        if (pressed(ImGuiKey_RightArrow)) pasteX = std::min(pasteX + 1, logicalW() - 1);
        if (pressed(ImGuiKey_UpArrow))    pasteY = std::max(pasteY - 1, 0);
        if (pressed(ImGuiKey_DownArrow))  pasteY = std::min(pasteY + 1, kHiresHeight - 1);
        if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
            pasteFloatingAt(pasteX, pasteY);
            pasting = false;
        }
    }

    if (pressed(ImGuiKey_P)) pick(Tool::Pencil);
    if (pressed(ImGuiKey_E)) pick(Tool::Eraser);
    if (pressed(ImGuiKey_L)) pick(Tool::Line);
    if (pressed(ImGuiKey_R)) pick(Tool::Rectangle);
    if (pressed(ImGuiKey_F)) pick(Tool::Fill);
    // (Ellipse on O, Eyedropper on I, Select on S, Palette-shift on M.)
    if (pressed(ImGuiKey_O)) pick(Tool::Ellipse);
    if (pressed(ImGuiKey_I)) pick(Tool::Eyedropper);
    if (pressed(ImGuiKey_S)) pick(Tool::Select);
    if (pressed(ImGuiKey_M)) pick(Tool::PaletteShift);
    if (pressed(ImGuiKey_T)) pick(Tool::Text);

    // Palette 1-6.
    const HgrColor palette[] = { HgrColor::Black, HgrColor::White, HgrColor::Violet,
                                 HgrColor::Green, HgrColor::Blue, HgrColor::Orange };
    const ImGuiKey numKeys[] = { ImGuiKey_1, ImGuiKey_2, ImGuiKey_3,
                                 ImGuiKey_4, ImGuiKey_5, ImGuiKey_6 };
    // In the 16-colour modes the colour bar drives grColor; skip the HGR
    // palette keys so '1' (Black) can't turn the pencil into an eraser (the
    // 16-colour plots read `color` only to detect the eraser).
    if (!sixteenMode())
        for (int i = 0; i < 6; ++i)
            if (pressed(numKeys[i])) color = palette[i];

    // X toggles Filled, but only for the tools that expose it — otherwise it
    // silently flips hidden state with no on-screen feedback.
    if (pressed(ImGuiKey_X) && (tool == Tool::Rectangle || tool == Tool::Ellipse))
        rectFilled = !rectFilled;
    if (pressed(ImGuiKey_G)) showGrid = !showGrid;

    // Zoom +/- (main row and keypad).
    if (pressed(ImGuiKey_Equal) || pressed(ImGuiKey_KeypadAdd))
        zoomIdx = std::min(zoomIdx + 1, kZoomLadderCount - 1);
    if (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract))
        zoomIdx = std::max(zoomIdx - 1, 0);

    // Brush size [ ].
    if (pressed(ImGuiKey_LeftBracket))  brushSize = std::max(brushSize - 1, 1);
    if (pressed(ImGuiKey_RightBracket)) brushSize = std::min(brushSize + 1, 7);
}

void hgrpaint::HgrPaintEditor::render(const std::vector<uint8_t>& memory,
                                      const std::vector<uint8_t>* aux)
{
    handleShortcuts();

    // MacPaint / MousePaint layout:
    //   ┌──────────────── top bar: page · file · help ─────────────────┐
    //   │ tools │            drawing canvas                            │
    //   │ (left)│                                                      │
    //   ├───────┴──────────────────────────────────────────────────────
    //   │ colour palette (bottom) · status line                        │
    renderTopBar();
    ImGui::Separator();

    // Reserve the bottom strip for the colour palette + status line, then split
    // the rest into the left tool palette and the drawing canvas.
    const float bottomH = 30.0f
                        + ImGui::GetTextLineHeightWithSpacing()
                        + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    const float panelW = 146.0f;

    ImGui::BeginChild("hgrbody", ImVec2(0.0f, -bottomH), false);
    {
        ImGui::BeginChild("hgrtools", ImVec2(panelW, 0.0f), true);
        renderToolPanel();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("hgrcanvasside", ImVec2(0.0f, 0.0f), false);
        renderCanvas(memory, aux);
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::Separator();
    renderColorBar();
    renderStatusBar(lastHoverX, lastHoverY, lastHoverX >= 0);

    renderFileBrowser();    // modal Load / Save / Save PNG / Import picker
    renderImportPreview();  // modal image-import preview with live sliders
}
