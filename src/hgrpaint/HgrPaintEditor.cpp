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

// Pom1 Apple 1 Emulator
// Copyright (C) 2000-2026 Verhille Arnaud
//
// HGR paint editor — see HgrPaintEditor.h. Independent reimplementation
// inspired by fadden's HGRTool (concept only).

#include "HgrPaintEditor.h"
#include "HgrConvert.h"          // image → HGR import (ii-pix-style, all in hgrpaint/)
#include "HgrFont.h"             // bbfont CP437 glyph table for the Text tool
#include "HgrPaintEditorTables.h"

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

} // namespace

using namespace hgrpaint::tables;

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
        if (pasteTex)         host->destroyTexture(pasteTex);
        if (monTex)           host->destroyTexture(monTex);
    }
    texture = importPreviewTex = importSrcTex = onionTex = flipTex = nullptr;
    pasteTex = monTex = nullptr;
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

// beginStroke/commitStroke NEST. A plain bool here was a real defect: an
// operation started inside an open stroke (Ctrl+C/X during a shape drag, an
// undo replay fired mid-stroke) cleared the outer stroke's undo ops, and its
// commit flipped the single `strokeBatching` flag off — so the OUTER commit
// skipped its endBatch() and left PaintCardBatcher stuck at depth 1: every
// later poke queued into a batch nobody ever committed, the canvas diverging
// from the Apple screen for the rest of the session.
//
// The counter makes the pairing exact: only the outermost bracket starts a new
// undo step and pushes it, and each level remembers (in `strokeBatchMask_`)
// whether IT opened a host batch, so each begin/end pair matches its own.
void hgrpaint::HgrPaintEditor::beginStroke(bool batch)
{
    // The mask is the record of who opened a batch, so a level it cannot
    // record must not open one either: `strokeNest_ >= 31` used to still call
    // beginBatch() while commitStroke() — which reads the same mask — skipped
    // the matching endBatch(), re-creating the very leak the counter exists to
    // prevent, only 31 levels deeper. Beyond 31 an outer batch is open anyway.
    const bool doBatch = batch && host && strokeNest_ < 31;
    if (strokeNest_ == 0) stroke.clear();
    if (strokeNest_ < 31) {
        if (doBatch) strokeBatchMask_ |=  (1u << strokeNest_);
        else         strokeBatchMask_ &= ~(1u << strokeNest_);
    }
    ++strokeNest_;
    if (doBatch) host->beginBatch();
}

void hgrpaint::HgrPaintEditor::commitStroke()
{
    // Unbalanced commit (the Select-rectangle drag ends with one and never
    // opened a stroke): nothing to close, and nothing to push.
    if (strokeNest_ == 0) return;
    --strokeNest_;
    if (strokeNest_ < 31 && (strokeBatchMask_ & (1u << strokeNest_))) {
        strokeBatchMask_ &= ~(1u << strokeNest_);
        if (host) host->endBatch();
    }
    if (strokeNest_ > 0) return;   // inner bracket: keep accumulating into the outer step
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
    // Flood by *perceived* colour (raw bits leak through the dither), through
    // the module's ColorNTSC decode, not the host pipeline (PAINT-2).
    // fillRegion mutates a page buffer directly, so snapshot the shadow, run it,
    // then diff back into per-byte edits to keep undo + the live RAM writes working.
    std::vector<uint8_t> before = shadow;
    hgrpaint::fillRegion(shadow.data(), x, y, c,
                         hgrpaint::hgrRenderPageRgb,
                         // The panel's MacPaint pattern, in page coordinates.
                         [this](int px, int py) { return patternOn(px, py); });
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
    flushOpenStroke();  // an open drag commits first (as Copy/Cut/Paste do)
    if (undo.empty()) return;
    auto ops = std::move(undo.back());
    undo.pop_back();
    applyOps(ops, false);
    redo.push_back(std::move(ops));
    if (redo.size() > 64) redo.erase(redo.begin());
}

void hgrpaint::HgrPaintEditor::doRedo()
{
    flushOpenStroke(); if (redo.empty()) return;
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
    clip.blockMode = grMode || dlgrMode;   // 7x4 block replicas, not pixels
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
    // Per-byte palette bit is a 280-HGR concept — excluded on every
    // 16-colour page, and on DLGR it is a memory-safety matter too: the
    // code below indexes the shadow through the HGR interleave (to 0x1FF7)
    // and the DLGR shadow is the 0x800 aux+main pair (GR and DHGR keep a
    // buffer large enough to absorb it). The gate used to be spelled out as
    // `grMode || dhgrMode`, which the later DLGR page slipped through:
    // out-of-bounds read AND write, after which emitShadowEdit poked the
    // live machine across text page 2, user RAM and HGR page 1. Gate on
    // `sixteenMode()` — the one member that enumerates the 16-colour pages.
    if (sixteenMode()) return;
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

    // As wide as the side panel (which follows the UI zoom).
    const float mmW = std::max(ImGui::GetContentRegionAvail().x, 32.0f);
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
    // A hostless editor (headless/test embedding) has no texture to show; the
    // black rect above is then the whole thumbnail rather than a null deref.
    if (host) dl->AddImage(host->textureToImTexture(texture), mmMin, mmMax);
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

    // Two-finger pinch (host-supplied): Fit, or ladder rungs about the fingers.
    int pinchSteps = 0;
    ImVec2 pinchAt(0.0f, 0.0f);
    bool pinchFit = false;
    if (pinchLadder(host, pinchAcc, pinchSteps, pinchAt, pinchFit) && pinchFit) wantFit = true;

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
    // A pinch (above) takes the same path, anchored on the fingers.
    {
        ImGuiIO& io = ImGui::GetIO();
        int step = pinchSteps;
        ImVec2 anchor = pinchAt;
        if (step == 0 && ImGui::IsWindowHovered() && io.MouseWheel != 0.0f) {
            step = io.MouseWheel > 0 ? 1 : -1;
            anchor = io.MousePos;
        }
        if (step != 0) {
            const int oldZoom = kZoomLadder[zoomIdx];
            const int ni = std::clamp(zoomIdx + step, 0, kZoomLadderCount - 1);
            if (ni != zoomIdx) {
                // Logical pixel currently under the anchor, in the OLD scale.
                const ImVec2 cur = ImGui::GetCursorScreenPos();
                const float anchorLX = (anchor.x - cur.x) / (oldZoom * af);
                const float anchorLY = (anchor.y - cur.y) / oldZoom;
                zoomIdx = ni;
                const int newZoom = kZoomLadder[zoomIdx];
                scale = static_cast<float>(newZoom);
                imgSize = ImVec2(kHiresWidth * scale * af, kHiresHeight * scale);
                // Keep that logical pixel under the anchor after rescaling.
                const float mouseInChildX = anchor.x - cur.x + ImGui::GetScrollX();
                const float mouseInChildY = anchor.y - cur.y + ImGui::GetScrollY();
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
    if (host)
        dl->AddImage(host->textureToImTexture(showSibling ? flipTex : texture), origin,
                     ImVec2(origin.x + imgSize.x, origin.y + imgSize.y));
    // Ghost of the sibling page (onion-skin style) — only when not flipping,
    // a flicker + ghost together would be unreadable.
    if (ghostOther && !flipShow && flipTex && host)
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
    // 280-HGR concept, and the scan reads the shadow at HGR interleave
    // offsets — out of bounds on DLGR's 0x800 shadow, once per frame for
    // as long as the overlay is on. Gated on sixteenMode() for the same
    // reason as paintPaletteByte (DLGR slipped through the old spelling).
    if (showConflicts && !sixteenMode()) {
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
        // One rect per horizontal RUN of one colour, not per pixel: a draw
        // list is addressed with 16-bit indices and the browser build's
        // renderer (WebGL2) has no vertex offset, so 65536 vertices — 16384
        // quads — is all one window gets, and a full-page clip is 53 760
        // pixels. Runs alone do not bound it (a dithered picture changes
        // colour every pixel or two), so past kPasteQuadBudget the preview
        // is ONE textured quad instead; a host with no textures (headless)
        // gets the runs that fit, under the outline that is always drawn.
        // 0 = transparent (black).
        auto clipColor = [this](int cx, int cy) -> ImU32 {
            ImU32 col;
            if (clip.sixteen) {
                const int v = clip.idx[static_cast<size_t>(cy) * clip.w + cx];
                if (v == 0) return 0;
                col = kGrPalette[v & 0x0F];
            } else {
                const HgrColor c = clip.px[static_cast<size_t>(cy) * clip.w + cx];
                if (c == HgrColor::Black) return 0;
                col = swatchColor(c);
            }
            return (col & 0x00FFFFFF) | 0xC0000000;
        };
        constexpr int kPasteQuadBudget = 12000;   // 48 000 of the 65 536 vertices
        int runs = 0;
        for (int cy = 0; cy < clip.h; ++cy) {
            ImU32 prev = 0;
            for (int cx = 0; cx < clip.w; ++cx) {
                const ImU32 col = clipColor(cx, cy);
                if (col != 0 && col != prev) ++runs;
                prev = col;
            }
        }
        bool drawn = false;
        if (runs > kPasteQuadBudget && host && clip.w > 0 && clip.h > 0) {
            pasteRgba.assign(static_cast<size_t>(clip.w) * clip.h, 0);
            for (int cy = 0; cy < clip.h; ++cy)
                for (int cx = 0; cx < clip.w; ++cx)
                    pasteRgba[static_cast<size_t>(cy) * clip.w + cx] = clipColor(cx, cy);
            pasteTex = host->uploadTexture(pasteTex, pasteRgba.data(),
                                           clip.w, clip.h, /*linear=*/false);
            if (pasteTex) {
                dl->AddImage(host->textureToImTexture(pasteTex),
                             ImVec2(origin.x + pasteX * xs, origin.y + pasteY * scale),
                             ImVec2(origin.x + (pasteX + clip.w) * xs,
                                    origin.y + (pasteY + clip.h) * scale));
                drawn = true;
            }
        }
        if (!drawn) {
            int budget = kPasteQuadBudget;
            for (int cy = 0; cy < clip.h && budget > 0; ++cy)
                for (int cx = 0; cx < clip.w && budget > 0; ) {
                    const ImU32 col = clipColor(cx, cy);
                    int end = cx + 1;
                    while (end < clip.w && clipColor(end, cy) == col) ++end;
                    if (col != 0) {
                        const float py = origin.y + (pasteY + cy) * scale;
                        dl->AddRectFilled(ImVec2(origin.x + (pasteX + cx) * xs, py),
                                          ImVec2(origin.x + (pasteX + end) * xs, py + scale),
                                          col);
                        --budget;
                    }
                    cx = end;
                }
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

        // A drag whose release never arrives — the application lost focus
        // mid-drag, and ImGui then clears the button without a release event —
        // is closed here WITHOUT painting the shape. Left open, the stroke and
        // its host batch nested every later edit: nothing reached guest
        // memory or the undo stack again until a restart (bug hunt
        // 2026-11-18 round 2). Same shape as the right-button erase above.
        if (dragging && !ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            commitStroke();
            dragging = false;
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
            // The OS dialog asked "replace?" about the name the user TYPED. A
            // raw page save retargets an untagged name to NAME#06xxxx, so the
            // file about to be replaced is one nobody confirmed: typing `PIC`
            // silently destroyed an existing `PIC#062000`. Ask about the real
            // target, with the same question the ImGui browser asks.
            if (forSave) {
                const std::string target = resolvedSaveTarget(picked);
                if (target != picked && std::filesystem::exists(target, ec)) {
                    browserOverwritePath = picked;
                    nativeOverwriteOpen = true;
                    return;
                }
            }
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
        std::string err;
        std::vector<uint8_t> bytes;
        std::ifstream in(fullPath, std::ios::binary);
        if (!in) {
            err = "cannot open " + fullPath;
        } else {
            // Stage and validate before touching VRAM. The old order cleared
            // an unsaved drawing first, so an empty/truncated replacement
            // destroyed it even though the UI reported "Load failed".
            bytes.resize(static_cast<size_t>(pageBytes()) + 1);
            in.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<size_t>(in.gcount()));
            if (bytes.empty()) err = "empty file";
            else if (dhgrMode && bytes.size() < static_cast<size_t>(pageBytes()))
                err = "not a 16 KB DHGR (A2FC) dump: " + fullPath;
            // A page of the wrong length is another kind of file: a short one
            // zero-filled the rest of the picture, a long one (an A2FC in HGR
            // mode, a PNG) was cut to its first bytes, and the undo history
            // went either way. 8184 is the classic HGR save that stops short
            // of the last screen hole.
            else if (bytes.size() != static_cast<size_t>(pageBytes()) &&
                     !(!dhgrMode && !grMode && !dlgrMode && bytes.size() == 8184))
                err = "not a " + std::to_string(pageBytes()) +
                      "-byte page for this mode: " + fullPath;
            else if (!host) err = "no emulator";
        }

        const bool ok = err.empty();
        if (ok) {
            bytes.resize(std::min(bytes.size(), static_cast<size_t>(pageBytes())));
            // The load is ONE undoable stroke, like "Apply to page" of an
            // import: a mistaken Load used to be final, because it bypassed
            // the history and then had to clear it (the older ByteEdits'
            // `old` values described the replaced picture). Recorded against
            // the shadow, the earlier steps stay valid underneath it — the
            // history is a chain again. Every byte is still POKED, changed or
            // not: the shadow is one frame old, and a running program may
            // have written the page since.
            flushOpenStroke();
            beginStroke(true);
            const int limit = std::min(pageBytes(), static_cast<int>(shadow.size()));
            for (int off = 0; off < limit; ++off) {
                if ((grMode || dlgrMode) && hgrpaint::grIsScreenHole(off & 0x3FF))
                    continue;
                const uint8_t value = static_cast<size_t>(off) < bytes.size()
                    ? bytes[static_cast<size_t>(off)] : 0;
                const uint8_t old = shadow[off];
                shadow[off] = value;
                if (old != value) emitShadowEdit(off, old);
                else              hostPoke(addrOfShadowOff(off), value);
            }
            commitStroke();
            std::snprintf(filePath, sizeof(filePath), "%s", fullPath.c_str());
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
        // A2FC (both planes' screen holes belong to the picture format). Same
        // sixteenMode() gate as the two other interleave sites: DLGR is a
        // lo-res pair too, and its 0x800 shadow ends well before $1FF8, so
        // the old `!grMode && !dhgrMode` spelling overran it by 8 bytes AND
        // stamped "POM1HGR" into live RAM at $3F8/$7F8 on every DLGR save.
        //
        // The tag has to be in RAM before the host reads the page back, so it
        // is poked first — but it is a real edit of the user's page, and it
        // used to be made behind the undo history's back AND kept even when
        // the save then failed (a full disk left "POM1HGR" welded into the
        // drawing with no way to take it out). Remember the bytes it
        // displaces: a failed save puts them back, a successful one records
        // the tag as an ordinary one-step edit the user can undo.
        std::vector<ByteEdit> tagEdits;
        if (!sixteenMode()) {
            static const char kTag[8] = { 'P','O','M','1','H','G','R','\0' };
            for (int i = 0; i < 8; ++i) {
                const int off = 0x1FF8 + i;
                const uint8_t old = shadow[off];
                shadow[off] = static_cast<uint8_t>(kTag[i]);
                const uint32_t addr = addrOfShadowOff(off);
                if (old != shadow[off]) tagEdits.push_back({addr, old, shadow[off]});
                hostPoke(addr, shadow[off]);
            }
        }
        ok = host && (dhgrMode ? host->saveDhgrImage(outPath, baseAddr(), err)
                    : dlgrMode ? host->saveDlgrImage(outPath, baseAddr(), err)
                    : host->saveImage(outPath, baseAddr(), pageBytes(), err));
        if (!tagEdits.empty()) {
            if (ok) {
                undo.push_back(tagEdits);
                if (undo.size() > 64) undo.erase(undo.begin());
                redo.clear();
            } else {
                for (const auto& e : tagEdits) {          // roll the tag back
                    shadow[shadowOffOfAddr(e.addr)] = e.oldVal;
                    hostPoke(e.addr, e.oldVal);
                }
            }
        }
        const std::string outName = fs::path(outPath).filename().string();
        status = ok ? (dhgrMode ? ("Saved 16 KB DHGR (A2FC, aux+main): " + outName)
                     : dlgrMode ? ("Saved 2 KB DLGR pair (aux+main): " + outName)
                     : grMode   ? ("Saved 1 KB lo-res GR page: " + outName)
                                : ("Saved 8 KB HGR (+POM1HGR tag): " + outName))
                    : ("Save failed: " + (err.empty() ? std::string("(error)") : err));
    }
    return ok;
}

// The path a Save will really write: the raw-page kind appends the SD-CARD-OS
// "#06AAAA" tag to an untagged basename (performFileAction does the same), so
// an existence check on the typed name alone would miss the real target.
std::string hgrpaint::HgrPaintEditor::resolvedSaveTarget(const std::string& full) const
{
    namespace fs = std::filesystem;
    if (browserSaveKind != 0 || full.empty()) return full;
    fs::path outP(full);
    if (outP.filename().string().find('#') != std::string::npos) return full;
    return (outP.parent_path() /
            sdCardDefaultName(outP.filename().string(), baseAddr())).string();
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
        // Same flush as Ctrl+V below: a copy/cut fired mid-drag would nest a
        // second stroke inside the open one (Cut opens its own batch). A
        // Select rubber-band is NOT such a stroke — see flushOpenStroke().
        if (pressed(ImGuiKey_C)) {
            flushOpenStroke();
            copySelection(false);
        }
        if (pressed(ImGuiKey_X)) {
            flushOpenStroke();
            copySelection(true);
        }
        if (pressed(ImGuiKey_V) && clipUsableHere()) {
            flushOpenStroke();
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

