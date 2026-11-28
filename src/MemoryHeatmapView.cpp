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

#include "MemoryHeatmapView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace pom2::heatview {

// ── Colour ─────────────────────────────────────────────────────────────────

Rgb kindColour(int kind)
{
    // FT draws pure primaries (write R, execute G, read B with half of it in
    // G: an azure). POM2 keeps the hues and softens them a little so that two
    // kinds on one byte still read (write + read = magenta, as FT's rails).
    switch (kind) {
    case HeatData::Write: return {255, 56, 48};
    case HeatData::Read:  return {40, 136, 255};
    case HeatData::Exec:  return {48, 230, 92};
    case HeatData::Dma:   return {255, 186, 36};
    }
    return {255, 255, 255};
}

const char* kindName(int kind)
{
    switch (kind) {
    case HeatData::Write: return "written";
    case HeatData::Read:  return "read";
    case HeatData::Exec:  return "executed";
    case HeatData::Dma:   return "card DMA";
    }
    return "?";
}

uint8_t bitsLuma(uint8_t value)
{
    // [D] AppleWin FT Debugger_Heatmap.cpp @ Fr3nchT0uch/AppleWin
    // extended_debugger 52ddd021: popcount[0] = 0, else (7 + bits) << 2.
    if (value == 0) return 0;
    int bits = 0;
    for (unsigned v = value; v; v >>= 1) bits += static_cast<int>(v & 1u);
    return static_cast<uint8_t>((7 + bits) << 2);
}

float intensity(uint32_t v, uint32_t now, const Style& style, float logMax)
{
    if (v == 0) return 0.0f;
    if (style.cumulative) {
        if (logMax <= 0.0f) return 0.0f;
        const float x = std::log2(static_cast<float>(v) + 1.0f) / logMax;
        return 0.22f + 0.78f * std::min(1.0f, x);
    }
    if (v > now) return 0.0f;
    const float horizon = static_cast<float>(style.fadeFrames) * static_cast<float>(style.frameCycles) /
                          static_cast<float>(1u << HeatData::kStampShift);
    const float age = static_cast<float>(now - v);
    if (horizon > 0.0f && age < horizon) {
        // [D] FT @ 52ddd021 Debugger_Heatmap.cpp: a heat value starts at
        // 0x1FFFF, loses one per instruction, and is drawn >> 9 — a LINEAR
        // fade from 255 to 0. POM2's palette keeps a floor so the last frames
        // of the fade stay visible on the dim background.
        const float x = 1.0f - age / horizon;
        return style.palette == Palette::Ft ? x : 0.18f + 0.82f * x;
    }
    return style.trail ? 0.13f : 0.0f;
}

uint32_t cellColour(const uint32_t v[HeatData::kKinds], uint32_t now, uint8_t value,
                    const Style& style, float logMax)
{
    HeatCell c;
    for (int k = 0; k < HeatData::kKinds; ++k) {
        const auto ki = static_cast<std::size_t>(k);
        c.k[ki] = (v[k] && style.show[ki])
            ? static_cast<uint8_t>(std::min(255.0f, intensity(v[k], now, style, logMax) * 255.0f + 0.5f))
            : uint8_t{0};
    }
    c.bg = style.background == Background::Bits ? bitsLuma(value)
         : style.background == Background::Value ? static_cast<uint8_t>(value * 56 / 255)
         : uint8_t{0};
    return compose(c, style);
}

float countScale(const HeatData::PlaneCopy& copy, uint32_t lo, uint32_t hi, const Style& style)
{
    uint32_t most = 0;
    for (int k = 0; k < HeatData::kKinds; ++k) {
        if (!style.show[static_cast<std::size_t>(k)]) continue;
        const auto& v = copy.v[static_cast<std::size_t>(k)];
        if (v.size() <= hi) continue;
        for (uint32_t a = lo; a <= hi; ++a) most = std::max(most, v[a]);
    }
    return most ? std::log2(static_cast<float>(most) + 1.0f) : 0.0f;
}

void paintPlane(int plane, const HeatData::PlaneCopy& copy, const uint8_t* values,
                uint32_t now, const Style& style, float logMax, std::vector<uint32_t>& rgba)
{
    std::vector<HeatCell> cells;
    heatCells(plane, copy, values, now, style, logMax, cells);
    rgba.resize(0x10000);
    const uint32_t lo = static_cast<uint32_t>(firstRow(plane)) << 8;
    std::fill(rgba.begin(), rgba.begin() + lo, 0u);
    for (uint32_t a = lo; a < 0x10000; ++a) rgba[a] = compose(cells[a - lo], style);
}

// ── Cells, max-pooling, composition ───────────────────────────────────────

void heatCells(int plane, const HeatData::PlaneCopy& copy, const uint8_t* values, uint32_t now,
               const Style& style, float logMax, std::vector<HeatCell>& out)
{
    const uint32_t lo = static_cast<uint32_t>(firstRow(plane)) << 8;
    const std::size_t n = 0x10000 - lo;
    out.resize(n);
    // Recent mode: the level of a stamp depends on its age only, so the fade
    // is a table over the horizon, kept while the style does not change —
    // this runs over 64 K bytes a column a frame. Every age below the float
    // horizon intensity() uses is in it, and no more.
    struct Fade {
        int frames = -1, frameCycles = -1;
        Palette palette = Palette::Pom2;
        bool trail = false;
        uint32_t horizon = 0;
        uint8_t trailLevel = 0;
        std::vector<uint8_t> level;
    };
    static thread_local Fade fade;
    if (!style.cumulative &&
        (fade.frames != style.fadeFrames || fade.frameCycles != style.frameCycles ||
         fade.palette != style.palette || fade.trail != style.trail)) {
        fade.frames = style.fadeFrames;
        fade.frameCycles = style.frameCycles;
        fade.palette = style.palette;
        fade.trail = style.trail;
        fade.horizon = static_cast<uint32_t>(std::max(1.0f, std::ceil(static_cast<float>(style.fadeFrames) *
                                                                     static_cast<float>(style.frameCycles) /
                                                                     static_cast<float>(1u << HeatData::kStampShift))));
        const uint32_t t0 = fade.horizon + 1;   // any "now": the level depends on the age only
        fade.level.resize(fade.horizon);
        for (uint32_t age = 0; age < fade.horizon; ++age)
            fade.level[age] = static_cast<uint8_t>(std::min(255.0f, intensity(t0 - age, t0, style, 0.0f) * 255.0f + 0.5f));
        fade.trailLevel = static_cast<uint8_t>(std::min(255.0f, intensity(1, t0, style, 0.0f) * 255.0f + 0.5f));
    }
    // One kind at a time: a plain loop over one array.
    for (int k = 0; k < HeatData::kKinds; ++k) {
        const auto ki = static_cast<std::size_t>(k);
        const uint32_t* v = copy.v[ki].data() + lo;
        if (!style.show[ki] || copy.v[ki].size() < 0x10000) {
            for (std::size_t a = 0; a < n; ++a) out[a].k[ki] = 0;
            continue;
        }
        if (!style.cumulative) {
            const uint32_t horizon = fade.horizon;
            const uint8_t* table = fade.level.data();
            const uint8_t trail = fade.trailLevel;
            for (std::size_t a = 0; a < n; ++a) {
                const uint32_t st = v[a];
                const uint32_t age = now - st;
                out[a].k[ki] = (st == 0 || st > now) ? 0 : age < horizon ? table[age] : trail;
            }
        } else {
            for (std::size_t a = 0; a < n; ++a)
                out[a].k[ki] = v[a] ? static_cast<uint8_t>(std::min(255.0f, intensity(v[a], now, style, logMax) * 255.0f + 0.5f))
                                    : uint8_t{0};
        }
    }
    // The background, from a table of the 256 values.
    std::array<uint8_t, 256> bg{};
    for (int v = 0; v < 256; ++v)
        bg[static_cast<std::size_t>(v)] =
            style.background == Background::Bits ? bitsLuma(static_cast<uint8_t>(v))
          : style.background == Background::Value ? static_cast<uint8_t>(v * 56 / 255) : uint8_t{0};
    for (std::size_t a = 0; a < n; ++a) out[a].bg = values ? bg[values[lo + a]] : 0;
}

uint32_t compose(const HeatCell& c, const Style& style)
{
    if (style.palette == Palette::Ft) {
        // [D] FT @ 52ddd021 Debugger_Heatmap.cpp, DrawMemHeatmap:
        //   r = W | bg,  g = X | (B >> 1) | bg,  b = B | bg
        // bitwise ORs of the >> 9 heat values over the popcount grey. FT has
        // no DMA (its Z80 path records reads and writes); POM2 ORs DMA into
        // red and green, amber, so it still shows.
        const unsigned bg = c.bg, w = c.k[HeatData::Write], r = c.k[HeatData::Read],
                       x = c.k[HeatData::Exec], d = c.k[HeatData::Dma];
        const unsigned red = w | bg | d, green = x | (r >> 1) | bg | (d * 3 / 4), blue = r | bg;
        return 0xFF000000u | (blue << 16) | (green << 8) | red;
    }
    // POM2: a dim, slightly cool background under hues that ADD, so two
    // kinds on one byte mix (write + read = magenta, as on FT's rails), and
    // the freshest accesses flash toward white. Integer tables: this runs
    // for every byte of every column every frame.
    struct Tables {
        std::array<std::array<uint16_t, 3>, 256> bg{};
        std::array<std::array<std::array<uint16_t, 3>, 256>, HeatData::kKinds> kind{};
        std::array<uint16_t, 256> flash{};   // x 1/256, toward white
        Tables()
        {
            for (int v = 0; v < 256; ++v) {
                bg[v] = {static_cast<uint16_t>(9.0f + v * 0.55f + 0.5f), static_cast<uint16_t>(10.0f + v * 0.58f + 0.5f),
                         static_cast<uint16_t>(13.0f + v * 0.70f + 0.5f)};
                flash[v] = v > 232 ? static_cast<uint16_t>(256.0f * 0.32f * static_cast<float>(v - 232) / 23.0f + 0.5f) : 0;
            }
            for (int k = 0; k < HeatData::kKinds; ++k) {
                const Rgb kc = kindColour(k);
                for (int l = 0; l < 256; ++l)
                    kind[k][l] = {static_cast<uint16_t>(kc.r * l / 255), static_cast<uint16_t>(kc.g * l / 255),
                                  static_cast<uint16_t>(kc.b * l / 255)};
            }
        }
    };
    static const Tables t;
    unsigned r = t.bg[c.bg][0], g = t.bg[c.bg][1], b = t.bg[c.bg][2];
    unsigned top = 0;
    for (std::size_t k = 0; k < HeatData::kKinds; ++k) {   // level 0 adds 0: no branch
        const unsigned lvl = c.k[k];
        top = std::max(top, lvl);
        const auto& kc = t.kind[k][lvl];
        r += kc[0]; g += kc[1]; b += kc[2];
    }
    r = std::min(r, 255u); g = std::min(g, 255u); b = std::min(b, 255u);
    if (!style.cumulative && top > 232) {
        const unsigned f = t.flash[top];
        r += ((255u - r) * f + 128u) >> 8;
        g += ((255u - g) * f + 128u) >> 8;
        b += ((255u - b) * f + 128u) >> 8;
    }
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

uint32_t glowColour(const HeatCell& c, const Style& style)
{
    if (style.cumulative) return 0;
    float r = 0, g = 0, b = 0, a = 0;
    for (int k = 0; k < HeatData::kKinds; ++k) {
        const int lvl = c.k[static_cast<std::size_t>(k)];
        if (lvl <= 200) continue;
        const float f = static_cast<float>(lvl - 200) / 55.0f;
        const Rgb kc = style.palette == Palette::Ft
            ? (k == HeatData::Write ? Rgb{255, 0, 0} : k == HeatData::Read ? Rgb{0, 128, 255}
               : k == HeatData::Exec ? Rgb{0, 255, 0} : Rgb{255, 192, 0})
            : kindColour(k);
        r += kc.r * f; g += kc.g * f; b += kc.b * f;
        a = std::max(a, f);
    }
    if (a <= 0.0f) return 0;
    auto q = [](float x) { return static_cast<uint32_t>(std::clamp(x, 0.0f, 255.0f) + 0.5f); };
    return (q(a * 255.0f) << 24) | (q(b) << 16) | (q(g) << 8) | q(r);
}

void maxPool(const std::vector<HeatCell>& src, int rows, int outW, int outH,
             std::vector<HeatCell>& dst, bool withBackground)
{
    outW = std::clamp(outW, 1, 256);
    outH = std::clamp(outH, 1, std::max(1, rows));
    dst.assign(static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH), HeatCell{});
    if (rows <= 0 || src.size() < static_cast<std::size_t>(rows) * 256u) return;
    // HEAT: each source byte belongs to exactly one output pixel (an integer
    // partition), and the pixel keeps the hottest byte per kind.
    // BACKGROUND: a box filter over what the pixel covers — and never less
    // than two bytes, the Nyquist width. An integer mean, or a box of one
    // pixel at 1.1 bytes, aliases: a $00 / $FF fill (POM2's power-on RAM)
    // came out as a moire of 0-to-60 stripes nine pixels apart.
    // The filter window of output i over a source of n: [lo, lo + width).
    auto window = [](int i, float f, bool pooled, int n, float& lo, float& width) {
        width = pooled ? std::min(static_cast<float>(n), std::max(f, 2.0f)) : 1.0f;
        lo = std::clamp((static_cast<float>(i) + 0.5f) * f - width * 0.5f, 0.0f, static_cast<float>(n) - width);
    };
    auto overlap = [](float a0, float a1, int b) {
        return std::min(a1, static_cast<float>(b + 1)) - std::max(a0, static_cast<float>(b));
    };
    // The taps of each output column, once: its heat span and its background
    // weights (normalised).
    struct Tap { int x; float w; };
    std::array<int, 257> c0{};
    for (int i = 0; i <= outW; ++i) c0[static_cast<std::size_t>(i)] = i * 256 / outW;
    const float fx = 256.0f / static_cast<float>(outW);
    const float fy = static_cast<float>(rows) / static_cast<float>(outH);
    std::vector<Tap> taps;
    std::array<int, 257> tap0{};
    for (int i = 0; i < outW; ++i) {
        tap0[static_cast<std::size_t>(i)] = static_cast<int>(taps.size());
        if (!withBackground) continue;
        float x0 = 0.0f, width = 1.0f;
        window(i, fx, outW < 256, 256, x0, width);
        for (int x = static_cast<int>(x0); x < 256 && static_cast<float>(x) < x0 + width; ++x) {
            const float w = overlap(x0, x0 + width, x);
            if (w > 0.0f) taps.push_back({x, w / width});
        }
    }
    tap0[static_cast<std::size_t>(outW)] = static_cast<int>(taps.size());
    std::array<float, 256> rowBg{};
    for (int j = 0; j < outH; ++j) {
        const int r0 = j * rows / outH;
        const int r1 = std::max(r0 + 1, (j + 1) * rows / outH);
        if (withBackground) {
            float y0 = 0.0f, height = 1.0f;
            window(j, fy, outH < rows, rows, y0, height);
            rowBg.fill(0.0f);
            for (int r = static_cast<int>(y0); r < rows && static_cast<float>(r) < y0 + height; ++r) {
                const float wy = overlap(y0, y0 + height, r) / height;
                if (wy <= 0.0f) continue;
                const HeatCell* row = &src[static_cast<std::size_t>(r) * 256u];
                for (int x = 0; x < 256; ++x) rowBg[static_cast<std::size_t>(x)] += wy * row[x].bg;
            }
        }
        HeatCell* out = &dst[static_cast<std::size_t>(j) * static_cast<std::size_t>(outW)];
        for (int i = 0; i < outW; ++i) {
            const int a0 = c0[static_cast<std::size_t>(i)];
            const int a1 = std::max(a0 + 1, c0[static_cast<std::size_t>(i) + 1]);
            HeatCell& m = out[i];
            for (int r = r0; r < r1; ++r) {
                const HeatCell* row = &src[static_cast<std::size_t>(r) * 256u];
                for (int x = a0; x < a1; ++x)
                    for (std::size_t k = 0; k < m.k.size(); ++k) m.k[k] = std::max(m.k[k], row[x].k[k]);
            }
            if (!withBackground) continue;
            float bg = 0.0f;
            for (int t = tap0[static_cast<std::size_t>(i)]; t < tap0[static_cast<std::size_t>(i) + 1]; ++t)
                bg += taps[static_cast<std::size_t>(t)].w * rowBg[static_cast<std::size_t>(taps[static_cast<std::size_t>(t)].x)];
            m.bg = static_cast<uint8_t>(std::clamp(bg + 0.5f, 0.0f, 255.0f));
        }
    }
}

void smoothBackground(std::vector<HeatCell>& cells, int rows)
{
    if (rows <= 0 || cells.size() < static_cast<std::size_t>(rows) * 256u) return;
    std::array<uint8_t, 256> line{};
    for (int r = 0; r < rows; ++r) {
        HeatCell* row = &cells[static_cast<std::size_t>(r) * 256u];
        for (int x = 0; x < 256; ++x) line[static_cast<std::size_t>(x)] = row[x].bg;
        for (int x = 0; x < 256; ++x) {
            const unsigned l = line[static_cast<std::size_t>(x > 0 ? x - 1 : 1)];
            const unsigned rr = line[static_cast<std::size_t>(x < 255 ? x + 1 : 254)];
            row[x].bg = static_cast<uint8_t>((l + 2u * line[static_cast<std::size_t>(x)] + rr + 2u) / 4u);
        }
    }
}

Rgb railColour(uint8_t bits, Palette palette)
{
    // [D] FT @ 52ddd021 Debugger_Heatmap.cpp: COLOR_READ 0x00FF7000 (light
    // blue), COLOR_WRITE 0x000000FF (red), ORed where both — Win32 COLORREF
    // is 0x00BBGGRR. POM2's palette uses its own read / write hues.
    const bool r = bits & RailRead, w = bits & RailWrite;
    if (palette == Palette::Ft)
        return {static_cast<uint8_t>(w ? 255 : 0), static_cast<uint8_t>(r ? 112 : 0),
                static_cast<uint8_t>(r ? 255 : 0)};
    if (r && w) return {214, 74, 232};
    return kindColour(r ? HeatData::Read : HeatData::Write);
}

// ── Where a byte is on the screen ──────────────────────────────────────────

ScreenSpot screenSpot(int plane, uint16_t phys)
{
    ScreenSpot s;
    if (plane == HeatData::kRom) return s;
    s.aux = HeatData::isAux(plane);
    // The inverse of Memory::textRowOffset / hiResLineOffset (pinned against
    // them): offset = 0x80*(row%8) + 40*(row/8) for text, 0x400*(y%8) +
    // 0x80*((y/8)%8) + 40*(y/64) for hi-res. The last 8 of each 128 bytes
    // are never fetched (the screen holes).
    auto split = [&](unsigned inner, int& third, int& col) {
        if (inner >= 120) { s.hole = true; return; }
        third = static_cast<int>(inner / 40);
        col = static_cast<int>(inner % 40);
    };
    if (phys >= 0x0400 && phys < 0x0C00) {
        s.kind = ScreenSpot::Text;
        s.page = phys < 0x0800 ? 1 : 2;
        const unsigned off = phys - (s.page == 1 ? 0x0400u : 0x0800u);
        int third = 0;
        split(off & 0x7Fu, third, s.col);
        s.row = third * 8 + static_cast<int>(off >> 7);
        return s;
    }
    if (phys >= 0x2000 && phys < 0x6000) {
        s.kind = ScreenSpot::Hires;
        s.page = phys < 0x4000 ? 1 : 2;
        const unsigned off = phys - (s.page == 1 ? 0x2000u : 0x4000u);
        int third = 0;
        split(off & 0x7Fu, third, s.col);
        s.row = third * 64 + static_cast<int>(((off >> 7) & 7u) << 3) + static_cast<int>(off >> 10);
        return s;
    }
    return s;
}

std::string describeSpot(const ScreenSpot& s)
{
    char t[160];
    if (s.kind == ScreenSpot::Text) {
        if (s.hole) {
            std::snprintf(t, sizeof t, "text page %d: a screen hole (not displayed)", s.page);
            return t;
        }
        std::snprintf(t, sizeof t,
                      "text%d row %d, col %d \xc2\xb7 80-col %d \xc2\xb7 lo-res rows %d (low nibble) / %d (high)",
                      s.page, s.row, s.col, 2 * s.col + (s.aux ? 0 : 1), 2 * s.row, 2 * s.row + 1);
        return t;
    }
    if (s.kind == ScreenSpot::Hires) {
        if (s.hole) {
            std::snprintf(t, sizeof t, "HGR page %d: a screen hole (not displayed)", s.page);
            return t;
        }
        const int dhgr0 = 14 * s.col + (s.aux ? 0 : 7);
        std::snprintf(t, sizeof t, "HGR%d line %d, byte %d, dots %d-%d \xc2\xb7 DHGR dots %d-%d (%s)",
                      s.page, s.row, s.col, 7 * s.col, 7 * s.col + 6, dhgr0, dhgr0 + 6,
                      s.aux ? "aux half" : "main half");
        return t;
    }
    return {};
}

// ── Options ────────────────────────────────────────────────────────────────

std::string optionsText(const Options& o)
{
    const Style& s = o.style;
    const char* bg = s.background == Background::Bits ? "bits"
                   : s.background == Background::Value ? "value" : "none";
    std::string t;
    auto put = [&](const char* k, const std::string& v) {
        if (!t.empty()) t += ' ';
        t += k;
        t += '=';
        t += v;
    };
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };
    put("mode", s.cumulative ? "cumulative" : "recent");
    put("fade", std::to_string(s.fadeFrames));
    put("bg", bg);
    put("trail", flag(s.trail));
    put("palette", s.palette == Palette::Ft ? "ft" : "pom2");
    put("show", flag(s.show[HeatData::Read]) + flag(s.show[HeatData::Write]) +
                    flag(s.show[HeatData::Exec]) + flag(s.show[HeatData::Dma]));
    put("rom", flag(o.showRom));
    put("main", flag(o.showMain));
    put("aux", flag(o.showAux));
    put("auxbank", std::to_string(o.auxChoice));
    put("bottomup", flag(o.bottomUp));
    put("fill", flag(o.fill));
    put("regions", flag(o.regions));
    put("rails", flag(o.rails));
    put("loupe", flag(o.loupe));
    put("inspector", flag(o.inspector));
    put("minimap", flag(o.minimap));
    put("labels", flag(o.labels));
    put("glow", flag(o.glow));
    return t;
}

Options parseOptions(std::string_view text)
{
    Options o;
    Style& s = o.style;
    while (!text.empty()) {
        const std::size_t sp = text.find(' ');
        const std::string_view word = text.substr(0, sp);
        text = sp == std::string_view::npos ? std::string_view{} : text.substr(sp + 1);
        const std::size_t eq = word.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view k = word.substr(0, eq), v = word.substr(eq + 1);
        const bool on = v == "1";
        auto number = [&](int lo, int hi, int& out) {
            try {
                out = std::clamp(std::stoi(std::string(v)), lo, hi);
            } catch (...) {}
        };
        if (k == "mode") s.cumulative = v == "cumulative";
        else if (k == "fade") number(1, 1500, s.fadeFrames);
        else if (k == "bg") s.background = v == "value" ? Background::Value
                                        : v == "none" ? Background::None : Background::Bits;
        else if (k == "trail") s.trail = on;
        else if (k == "show" && v.size() == 4) {
            s.show[HeatData::Read] = v[0] == '1';
            s.show[HeatData::Write] = v[1] == '1';
            s.show[HeatData::Exec] = v[2] == '1';
            s.show[HeatData::Dma] = v[3] == '1';
        }
        else if (k == "rom") o.showRom = on;
        else if (k == "main") o.showMain = on;
        else if (k == "aux") o.showAux = on;
        else if (k == "auxbank") number(-1, HeatData::kMaxAuxBanks - 1, o.auxChoice);
        else if (k == "bottomup") o.bottomUp = on;
        else if (k == "fill") o.fill = on;
        else if (k == "regions") o.regions = on;
        else if (k == "rails") o.rails = on;
        else if (k == "loupe") o.loupe = on;
        else if (k == "inspector") o.inspector = on;
        else if (k == "palette") s.palette = v == "ft" ? Palette::Ft : Palette::Pom2;
        else if (k == "minimap") o.minimap = on;
        else if (k == "labels") o.labels = on;
        else if (k == "glow") o.glow = on;
    }
    return o;
}

// ── Regions ────────────────────────────────────────────────────────────────

const std::vector<Region>& regions(int plane)
{
    // A RAM bank in its physical layout (Memory::lcPhysical / auxPhysical).
    // [E] The usual Apple II page map — zero page, stack, the GETLN buffer,
    // page 3, the text and hi-res pages — as every reference repeats it; not
    // re-read for this table. Settled by: the memory-map chapter of the Apple
    // IIe Technical Reference Manual. The video pages are the ones POM2's
    // display reads (Apple2Display). "Free" means the ROM leaves it to
    // programs, not that DOS or ProDOS does.
    static const std::vector<Region> ram = {
        {0x0000, 0x00FF, "ZP",   "Zero page"},
        {0x0100, 0x01FF, "STK",  "6502 stack"},
        {0x0200, 0x02FF, "BUF",  "Input buffer (GETLN)"},
        {0x0300, 0x03FF, "P3",   "Page 3 (vectors, DOS hooks)"},
        {0x0400, 0x07FF, "TXT1", "Text / lo-res page 1"},
        {0x0800, 0x0BFF, "TXT2", "Text / lo-res page 2"},
        {0x0C00, 0x1FFF, "",     "Free RAM"},
        {0x2000, 0x3FFF, "HGR1", "Hi-res page 1"},
        {0x4000, 0x5FFF, "HGR2", "Hi-res page 2"},
        {0x6000, 0xBFFF, "",     "Free RAM"},
        {0xC000, 0xCFFF, "LC1",  "Language card $D000, bank 1"},
        {0xD000, 0xDFFF, "LC2",  "Language card $D000, bank 2"},
        {0xE000, 0xFFFF, "LC",   "Language card $E000-$FFFF"},
    };
    // The CPU's $C000-$FFFF when it reaches the motherboard and the slots.
    static const std::vector<Region> rom = {
        {0xC000, 0xC07F, "I/O",  "Motherboard soft switches"},
        {0xC080, 0xC08F, "LCSW", "Language-card switches"},
        {0xC090, 0xC0FF, "DEV",  "Slot I/O ($C0n0, DEVSEL)"},
        {0xC100, 0xC7FF, "CnXX", "Slot ROMs ($Cn00, IOSEL)"},
        {0xC800, 0xCFFF, "C8",   "Expansion ROM ($C800, I/O STROBE)"},
        {0xD000, 0xF7FF, "BAS",  "BASIC ROM"},
        {0xF800, 0xFFFF, "MON",  "Monitor ROM"},
    };
    return plane == HeatData::kRom ? rom : ram;
}

const Region* regionAt(int plane, uint16_t phys)
{
    for (const Region& r : regions(plane))
        if (phys >= r.lo && phys <= r.hi) return &r;
    return nullptr;
}

std::string planeTitle(int plane, int auxBanks)
{
    if (plane == HeatData::kMain) return "MAIN";
    if (plane == HeatData::kRom) return "ROM & I/O";
    const int slot = plane - HeatData::kAux0;
    if (auxBanks <= 1) return "AUX";
    return "AUX bank " + std::to_string(slot);
}

// ── Rails ──────────────────────────────────────────────────────────────────

void markShown(const VideoSwitches& v, bool iie, Rails& main, Rails& aux0)
{
    // [D] AppleWin FT Debugger_Heatmap.cpp ("Drawing Video display bars")
    // @ Fr3nchT0uch/AppleWin extended_debugger 52ddd021: text or lo-res
    // whenever TEXT, !HIRES or MIXED; hi-res when !TEXT && HIRES; page 1 when
    // 80STORE or !PAGE2 (with 80STORE, PAGE2 banks the CPU into aux and the
    // picture stays on page 1 — POM2's own display reads it so). Where POM2
    // differs, deliberately, to follow what its display reads: FT's aux text
    // bar is (TEXT && 80COL) || ((GR || MIXED) && DHIRES) — it misses the
    // 80-column text of a MIXED hi-res screen and lights aux for DHIRES
    // without 80COL; FT's aux hi-res bar wants DHIRES alone, POM2 80COL too.
    const bool page1 = (iie && v.store80) || !v.page2;
    const bool textish = v.text || !v.hires || v.mixed;
    const bool hgr = !v.text && v.hires;
    const bool auxText = iie && v.col80 && (v.text || v.mixed || (!v.hires && v.dhgr));
    const bool auxHgr = iie && v.col80 && v.dhgr && hgr;
    auto mark = [](Rails& r, int lo, int hi) { for (int p = lo; p <= hi; ++p) r.shown[static_cast<std::size_t>(p)] = true; };
    if (textish) {
        mark(main, page1 ? 0x04 : 0x08, page1 ? 0x07 : 0x0B);
        if (auxText) mark(aux0, page1 ? 0x04 : 0x08, page1 ? 0x07 : 0x0B);
    }
    if (hgr) {
        mark(main, page1 ? 0x20 : 0x40, page1 ? 0x3F : 0x5F);
        if (auxHgr) mark(aux0, page1 ? 0x20 : 0x40, page1 ? 0x3F : 0x5F);
    }
}

// ── Layout and transform ───────────────────────────────────────────────────

Layout makeLayout(const std::vector<int>& planes, bool bottomUp)
{
    Layout l;
    l.bottomUp = bottomUp;
    float x = 0.0f;
    for (int p : planes) {
        l.columns.push_back({p, x});
        x += Layout::kPlane + Layout::kRail + Layout::kGap;
    }
    l.width = l.columns.empty() ? Layout::kPlane : x - Layout::kGap;
    return l;
}

bool hit(const Layout& l, float cx, float cy, int& column, uint16_t& phys)
{
    if (cy < 0.0f || cy >= Layout::kPlane) return false;
    for (std::size_t i = 0; i < l.columns.size(); ++i) {
        const float x = cx - l.columns[i].x;
        if (x < 0.0f || x >= Layout::kPlane) continue;
        const int y = static_cast<int>(std::floor(cy));
        const int row = l.bottomUp ? 255 - y : y;
        if (row < firstRow(l.columns[i].plane)) return false;
        column = static_cast<int>(i);
        phys = static_cast<uint16_t>((row << 8) | static_cast<int>(std::floor(x)));
        return true;
    }
    return false;
}

bool cellOrigin(const Layout& l, int plane, uint16_t phys, float& cx, float& cy)
{
    for (const auto& c : l.columns) {
        if (c.plane != plane) continue;
        cx = c.x + static_cast<float>(phys & 0xFF);
        cy = rowY(l, phys >> 8);
        return true;
    }
    return false;
}

void applyAspect(Transform& t, const Layout& l, float h)
{
    t.scaleY = t.fill ? std::max(t.scale, h / l.height) : t.scale;
}

Transform fit(const Layout& l, float w, float h, bool fill)
{
    Transform t;
    t.fill = fill;
    t.scale = std::max(0.05f, fill ? w / l.width : std::min(w / l.width, h / l.height));
    if (fill && t.scale > h / l.height) t.scale = std::max(0.05f, std::min(w / l.width, h / l.height));
    applyAspect(t, l, h);
    clampPan(t, l, w, h);
    return t;
}

void clampPan(Transform& t, const Layout& l, float w, float h)
{
    applyAspect(t, l, h);
    auto axis = [](float& pan, float extent, float view) {
        if (view >= extent) pan = (extent - view) * 0.5f;
        else pan = std::clamp(pan, 0.0f, extent - view);
    };
    axis(t.panX, l.width, w / t.scale);
    axis(t.panY, l.height, h / t.scaleY);
}

void zoomAt(Transform& t, const Layout& l, float w, float h, float sx, float sy,
            float factor, float maxScale)
{
    const float cx = t.toCanvasX(sx), cy = t.toCanvasY(sy);
    const float least = fit(l, w, h, t.fill).scale;
    t.scale = std::clamp(t.scale * factor, least, std::max(least, maxScale));
    applyAspect(t, l, h);
    t.panX = cx - sx / t.scale;
    t.panY = cy - sy / t.scaleY;
    clampPan(t, l, w, h);
}

void centreOn(Transform& t, const Layout& l, float w, float h, float cx, float cy, float scale)
{
    t.scale = std::max(fit(l, w, h, t.fill).scale, scale);
    applyAspect(t, l, h);
    t.panX = cx - w * 0.5f / t.scale;
    t.panY = cy - h * 0.5f / t.scaleY;
    clampPan(t, l, w, h);
}

// ── Hot spots ──────────────────────────────────────────────────────────────

std::vector<Hot> hottest(const HeatData& data, int plane, int kind, std::size_t n)
{
    std::vector<Hot> out;
    const HeatData::PlaneData* d = data.plane(plane);
    if (!d || kind < 0 || kind >= HeatData::kKinds) return out;
    const auto& c = d->count[static_cast<std::size_t>(kind)];
    const uint32_t lo = static_cast<uint32_t>(firstRow(plane)) << 8;
    for (uint32_t a = lo; a < 0x10000; ++a)
        if (c[a]) out.push_back({plane, static_cast<uint16_t>(a), c[a]});
    const std::size_t keep = std::min(n, out.size());
    std::partial_sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(keep), out.end(),
                      [](const Hot& a, const Hot& b) {
                          return a.count != b.count ? a.count > b.count : a.phys < b.phys;
                      });
    out.resize(keep);
    return out;
}

}  // namespace pom2::heatview
