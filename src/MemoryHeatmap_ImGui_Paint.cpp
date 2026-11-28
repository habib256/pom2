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

// Memory Heatmap, the textures: what each column looks like at this frame's
// zoom. Three per column:
//
//   map   the plane's rows, one texel a byte while a byte is at least a
//         pixel; below that, MAX-POOLED to one texel a screen pixel
//         (heatview::maxPool: per kind the hottest byte of the pixel wins,
//         so an isolated access can never fall between pixels, and the
//         nearest-sampled 230-of-256 hatching is gone);
//   glow  the fresh accesses at a quarter of the resolution, sampled linear
//         and added over the map;
//   mini  the minimap's picture, pooled the same way to its own size.
//
// All on the UI thread with the lock released: the copy under the lock is
// MemoryHeatmap_ImGui::refresh.

#include "MemoryHeatmap_ImGui.h"

#include "IconsFontAwesome6.h"

#include "imgui.h"
#include "Pom2GL.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pom2 {

using heatview::Layout;

void MemoryHeatmap_ImGui::upload(GlTex& t, const uint32_t* px, int w, int h, bool linear)
{
    const bool fresh = !t.id;
    if (fresh) glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    if (fresh || t.linear != linear) {
        const GLint f = linear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t.linear = linear;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (fresh || t.w != w || t.h != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        t.w = w;
        t.h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
}

void MemoryHeatmap_ImGui::release(GlTex& t)
{
    if (t.id) glDeleteTextures(1, &t.id);
    t = {};
}

void MemoryHeatmap_ImGui::paint(float w, float h)
{
    const float s = xf_.scale, sY = xf_.scaleY;
    const heatview::Style& style = opt_.style;
    // The minimap while zoomed in: the whole layout, fitted to an inset in
    // the map's lower right corner — or its upper right one while the
    // selection (or the byte under a drag) would be under it. Collapsed, a
    // small pill in the same corner brings it back.
    const heatview::Transform whole = heatview::fit(layout_, w, h, opt_.fill);
    const bool zoomed = s > whole.scale * 1.02f || sY > whole.scaleY * 1.02f;
    mini_ = {};
    if (opt_.minimap && zoomed && w > 240.0f && h > 160.0f) {
        const float boxW = std::min(230.0f, w * 0.3f), boxH = std::min(170.0f, h * 0.38f);
        const heatview::Transform t = heatview::fit(layout_, boxW, boxH, opt_.fill);
        const float mw = layout_.width * t.scale, mh = Layout::kPlane * t.scaleY;
        auto place = [&](bool top) {
            mini_.x1 = lastOx_ + w - 12.0f;
            mini_.x0 = mini_.x1 - mw;
            mini_.y0 = top ? lastOy_ + 12.0f : lastOy_ + h - 12.0f - mh;
            mini_.y1 = mini_.y0 + mh;
        };
        // Would the inset hide what matters? The selection, the byte a Go to
        // landed on, or the pointer while it drags on the map.
        auto hides = [&](bool top) {
            place(top);
            const float m = 14.0f;
            auto inside = [&](float x, float y) {
                return x > mini_.x0 - m && x < mini_.x1 + m && y > mini_.y0 - m && y < mini_.y1 + m;
            };
            float x = 0, y = 0;
            if (selected_ && screenPointOf(selPlane_, selPhys_, x, y) && inside(x, y)) return true;
            if (ImGui::GetTime() < flashUntil_ && screenPointOf(flashLoc_.plane, flashLoc_.phys, x, y) &&
                inside(x, y)) return true;
            const ImVec2 mp = ImGui::GetIO().MousePos;
            return (panning_ || rangeDragging_) && inside(mp.x, mp.y);
        };
        if (hides(miniTop_) && !hides(!miniTop_)) miniTop_ = !miniTop_;
        place(miniTop_);
        mini_.s = t.scale;
        mini_.sY = t.scaleY;
        if (miniCollapsed_) {
            // The pill: where the inset's corner would be.
            const ImVec2 ts = ImGui::CalcTextSize(ICON_FA_MAP " minimap");
            mini_.pill = true;
            mini_.bx1 = mini_.x1;
            mini_.bx0 = mini_.x1 - ts.x - 14.0f;
            mini_.by0 = miniTop_ ? mini_.y0 : mini_.y1 - ts.y - 8.0f;
            mini_.by1 = mini_.by0 + ts.y + 8.0f;
        } else {
            mini_.shown = true;
            const float b = ImGui::GetFontSize() + 2.0f;   // the collapse button, top right
            mini_.bx1 = mini_.x1 + 4.0f;
            mini_.bx0 = mini_.bx1 - b;
            mini_.by0 = mini_.y0 - 4.0f;
            mini_.by1 = mini_.by0 + b;
        }
    }
    const bool glow = opt_.glow && !style.cumulative && std::min(s, sY) < 6.0f;

    std::vector<heatview::HeatCell> pooled;
    std::vector<uint32_t> px;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        Column& c = columns_[i];
        const int rows = 256 - heatview::firstRow(c.plane);
        const uint32_t lo = static_cast<uint32_t>(heatview::firstRow(c.plane)) << 8;
        c.logMax = style.cumulative ? heatview::countScale(c.copy, lo, 0xFFFF, style) : 0.0f;
        heatview::heatCells(c.plane, c.copy, c.values.data(), machine_.now, style, c.logMax, c.cells);

        // The map: native, or pooled to the pixels it covers.
        const float x0 = xf_.toScreenX(layout_.columns[i].x), x1 = x0 + Layout::kPlane * s;
        const float y0 = xf_.toScreenY(0.0f), y1 = y0 + static_cast<float>(rows) * sY;
        c.pooled = s < 1.0f || sY < 1.0f;
        // A column scrolled out of the view keeps last frame's texture: its
        // cells still feed the minimap and the loupe, nothing else.
        const bool onScreen = x1 > 0.0f && x0 < w;
        if (!onScreen) {
            // nothing: drawColumns clips it away
        } else if (c.pooled) {
            const int outW = s < 1.0f ? std::max(1, static_cast<int>(std::round(x1) - std::round(x0))) : 256;
            const int outH = sY < 1.0f ? std::max(1, static_cast<int>(std::round(y1) - std::round(y0))) : rows;
            heatview::maxPool(c.cells, rows, outW, outH, pooled);
            px.resize(pooled.size());
            for (std::size_t k = 0; k < pooled.size(); ++k) px[k] = heatview::compose(pooled[k], style);
            upload(c.map, px.data(), std::min(outW, 256), std::min(outH, rows), false);
        } else {
            // One texel a byte; the colours stay for the values drawn in the
            // cells (their contrast) — indexed by physical offset. Below 3 px
            // a byte the background is low-passed first (no moire).
            const std::vector<heatview::HeatCell>* from = &c.cells;
            if (s < 3.0f) {
                pooled = c.cells;
                heatview::smoothBackground(pooled, rows);
                from = &pooled;
            }
            c.rgba.resize(0x10000);
            for (uint32_t a = lo; a < 0x10000; ++a) c.rgba[a] = heatview::compose((*from)[a - lo], style);
            upload(c.map, c.rgba.data() + lo, 256, rows, false);
        }

        // The glow, with one transparent texel all round.
        if (glow) {
            const int gw = 64, gh = std::max(1, rows / 4);
            heatview::maxPool(c.cells, rows, gw, gh, pooled, false);
            px.assign(static_cast<std::size_t>(gw + 2) * static_cast<std::size_t>(gh + 2), 0u);
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x)
                    px[static_cast<std::size_t>(y + 1) * static_cast<std::size_t>(gw + 2) + static_cast<std::size_t>(x + 1)] =
                        heatview::glowColour(pooled[static_cast<std::size_t>(y) * gw + static_cast<std::size_t>(x)], style);
            upload(c.glow, px.data(), gw + 2, gh + 2, true);
        }

        // The minimap's picture.
        if (mini_.shown) {
            const int mw = std::max(1, static_cast<int>(std::round(Layout::kPlane * mini_.s)));
            const int mh = std::max(1, static_cast<int>(std::round(static_cast<float>(rows) * mini_.sY)));
            heatview::maxPool(c.cells, rows, std::min(mw, 256), std::min(mh, rows), pooled);
            // Read at a glance on a translucent inset: anything touched since
            // Clear lifted to a visible level, the background a little lighter.
            px.resize(pooled.size());
            for (std::size_t k = 0; k < pooled.size(); ++k) {
                heatview::HeatCell lifted = pooled[k];
                for (auto& l : lifted.k) if (l) l = static_cast<uint8_t>(std::max<int>(l, 120));
                lifted.bg = static_cast<uint8_t>(std::min(255, lifted.bg + 26));
                px[k] = heatview::compose(lifted, style);
            }
            upload(c.mini, px.data(), std::min(mw, 256), std::min(mh, rows), false);
        }
    }
}

}  // namespace pom2
