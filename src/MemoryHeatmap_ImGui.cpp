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

// The window, the copy under the lock, and the map. The textures are made
// in MemoryHeatmap_ImGui_Paint.cpp, the toolbar in _Toolbar.cpp, the status
// line / minimap / monitor bars in _Status.cpp, the rest in _Panel.cpp.

#include "MemoryHeatmap_ImGui.h"

#include "CpuClock.h"
#include "EmulationController.h"
#include "IconsFontAwesome6.h"
#include "Memory.h"
#include "PinchZoom_ImGui.h"

#include "imgui.h"
#include "Pom2GL.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

namespace pom2 {

using heatview::Layout;

namespace {

constexpr float kGutterLeft = 54.0f;   // page labels
constexpr float kGutterTop  = 40.0f;   // titles, then byte offsets
constexpr float kSideWidth  = 350.0f;  // the inspector

// The calm palette of the chrome around the map.
constexpr ImU32 kCanvas   = IM_COL32(13, 14, 17, 255);
constexpr ImU32 kFrame    = IM_COL32(44, 48, 58, 255);
constexpr ImU32 kMuted    = IM_COL32(128, 134, 150, 255);
constexpr ImU32 kGold     = IM_COL32(255, 210, 64, 255);

ImU32 colour(heatview::Rgb c, int alpha = 255)
{
    return IM_COL32(c.r, c.g, c.b, alpha);
}

const char* planeSubtitle(int plane)
{
    if (plane == HeatData::kRom) return "$C000-$FFFF, as the CPU reads it";
    if (plane == HeatData::kMain) return "64K + language card";
    return "aux 64K + language card";
}

/// The rows of a column, top to bottom on screen, as a span of canvas y.
void rowSpan(const Layout& l, int plane, float& top, float& bottom)
{
    const int r0 = heatview::firstRow(plane);
    top = std::min(heatview::rowY(l, r0), heatview::rowY(l, 255));
    bottom = top + static_cast<float>(256 - r0);
}

}  // namespace

MemoryHeatmap_ImGui::~MemoryHeatmap_ImGui()
{
    for (Column& c : columns_) {
        release(c.map);
        release(c.glow);
        release(c.mini);
    }
}

void MemoryHeatmap_ImGui::detach(EmulationController& ctrl)
{
    if (!heat_) return;
    ctrl.setHeatSink(nullptr);
    heat_.reset();
    frozen_.reset();
}

bool MemoryHeatmap_ImGui::takeRequest(Request& out)
{
    if (request_.kind == Request::Kind::None) return false;
    out = request_;
    request_ = {};
    return true;
}

std::vector<int> MemoryHeatmap_ImGui::wantedPlanes() const
{
    std::vector<int> planes;
    const bool aux = opt_.showAux && machine_.iie;
    if (opt_.showMain || (!opt_.showRom && !aux)) planes.push_back(HeatData::kMain);
    if (aux) {
        const int slot = opt_.auxChoice < 0 ? machine_.cpuAuxSlot
                                        : std::min(opt_.auxChoice, machine_.auxBanks - 1);
        planes.push_back(HeatData::auxPlane(slot));
    }
    // ROM & I/O last, at the far right (owner, 2026-11-28): the RAM banks
    // are what one reads first, side by side as in AppleWin FT.
    if (opt_.showRom) planes.push_back(HeatData::kRom);
    return planes;
}

// ── One frame's picture, copied under the lock ─────────────────────────────

void MemoryHeatmap_ImGui::refresh(EmulationController& ctrl)
{
    const bool frozen = frozen_ != nullptr;
    const std::vector<int> before = [&] {
        std::vector<int> p;
        for (const Column& c : columns_) p.push_back(c.plane);
        return p;
    }();
    {
        auto st = ctrl.lockState();
        const Memory& mem = st.memory();
        if (wantClear_) heat_->clear();
        if (wantMonitorReset_) heat_->resetCycleMonitor();
        wantClear_ = wantMonitorReset_ = false;
        machine_.iie = mem.isIIE();
        machine_.iicClass = mem.isIIcClass();
        machine_.auxBanks = std::max<int>(1, static_cast<int>(mem.ramWorksBanks()));
        machine_.cpuAuxSlot = mem.ramWorksBank();
        const VideoTiming& timing = pom2VideoTiming(mem.videoStandard());
        machine_.frameCycles = timing.cyclesPerFrame;
        machine_.clockHz = timing.cpuClockHz;
        opt_.style.frameCycles = machine_.frameCycles;
        if (!frozen) {
            machine_.now = heat_->now();
            machine_.irq = heat_->interruptLoad().frames();
            machine_.irqSummary = heat_->interruptLoad().summary();
            machine_.monitor = heat_->cycleMonitor();
        }
        const std::vector<int> planes = wantedPlanes();
        if (planes.size() < columns_.size())
            for (std::size_t i = planes.size(); i < columns_.size(); ++i) {
                release(columns_[i].map);
                release(columns_[i].glow);
                release(columns_[i].mini);
            }
        columns_.resize(planes.size());
        const HeatData& source = frozen ? static_cast<const HeatData&>(*frozen_)
                                        : static_cast<const HeatData&>(*heat_);
        for (std::size_t i = 0; i < planes.size(); ++i) {
            Column& c = columns_[i];
            const bool moved = c.plane != planes[i] || c.values.empty();
            c.plane = planes[i];
            // Frozen, the picture stays — unless the column now shows another
            // plane or the mode asks for the other half of the record.
            if (frozen && !moved && c.copy.counts == opt_.style.cumulative) continue;
            const uint32_t lo = static_cast<uint32_t>(heatview::firstRow(c.plane)) << 8;
            source.copyPlane(c.plane, opt_.style.cumulative, lo, 0xFFFF, c.copy);
            if (frozen && !moved) continue;    // the values and rails stay frozen
            c.values.resize(0x10000);
            if (c.plane == HeatData::kMain) {
                for (uint32_t p = 0; p < 0x10000; ++p)
                    c.values[p] = mem.lcPhysicalPeek(static_cast<uint16_t>(p));
            } else if (c.plane == HeatData::kRom) {
                // No read of the I/O page: a soft switch would answer it.
                std::fill(c.values.begin(), c.values.begin() + 0xC100, uint8_t{0});
                for (uint32_t a = 0xC100; a < 0xD000; ++a)
                    c.values[a] = mem.peekCpuView(static_cast<uint16_t>(a));
                for (uint32_t a = 0xD000; a < 0x10000; ++a)
                    c.values[a] = mem.romPeek(static_cast<uint16_t>(a));
            } else {
                const auto slot = static_cast<uint8_t>(c.plane - HeatData::kAux0);
                for (uint32_t p = 0; p < 0x10000; ++p)
                    c.values[p] = mem.auxPhysicalPeek(slot, static_cast<uint16_t>(p));
            }
            c.rails = {};
        }
        // The rails (AppleWin FT's bars): where each CPU page reads and
        // writes NOW, filed under the physical row it reaches; and the
        // pages the video shows. [D] FT @ 52ddd021 samples one address per
        // region ($0000, $0200, $0400, $2000, $4000, $C000, $D000, $E000);
        // POM2 asks Memory::debugBank for every page, which agrees with FT
        // region by region and is exact where a region is split.
        if (!frozen) {
            for (Column& c : columns_) c.rails = {};
            auto mark = [&](uint16_t addr, bool write) {
                const HeatData::Cell cell = HeatData::locate(addr, mem.debugBank(addr, write));
                for (Column& c : columns_)
                    if (c.plane == cell.plane)
                        c.rails.bank[cell.phys >> 8] |= write ? heatview::RailWrite
                                                             : heatview::RailRead;
            };
            for (int page = 0; page < 256; ++page) {
                mark(static_cast<uint16_t>(page << 8), false);
                mark(static_cast<uint16_t>(page << 8), true);
            }
            const Memory::DisplayState d = mem.getDisplayState();
            heatview::VideoSwitches v;
            v.text = d.textMode; v.mixed = d.mixedMode; v.page2 = d.page2; v.hires = d.hiRes;
            v.col80 = d.eightyCol; v.dhgr = d.dhgr; v.store80 = d.eightyStore;
            heatview::Rails mainShown, auxShown;
            heatview::markShown(v, machine_.iie, mainShown, auxShown);
            for (Column& c : columns_) {
                if (c.plane == HeatData::kMain) c.rails.shown = mainShown.shown;
                if (c.plane == HeatData::auxPlane(0)) c.rails.shown = auxShown.shown;
            }
        }
    }
    std::vector<int> after;
    for (const Column& c : columns_) after.push_back(c.plane);
    if (after != before || layout_.bottomUp != opt_.bottomUp) fitPending_ = true;
    layout_ = heatview::makeLayout(after, opt_.bottomUp);
    // A selection in a plane no longer drawn goes.
    if (selected_ && !columnOf(selPlane_)) selected_ = ranged_ = false;
}

// ── The window ─────────────────────────────────────────────────────────────

void MemoryHeatmap_ImGui::render(EmulationController& ctrl, bool* open, const LabelFn& labelOf,
                                 const ResolveFn& resolve)
{
    if (!*open) { detach(ctrl); return; }
    if (!heat_) {
        const Memory* mem = nullptr;
        const M6502* cpu = nullptr;
        {
            auto st = ctrl.lockState();
            mem = &st.memory();
            cpu = &st.cpu();
        }
        // Built (~8 MB zeroed) with the lock released; the objects it points
        // at live as long as the controller.
        heat_ = std::make_unique<MemoryHeatmap>(*mem, *cpu);
        ctrl.setHeatSink(heat_.get());   // takes the lock itself
        fitPending_ = true;
    }

    ImGui::SetNextWindowSize(ImVec2(1180, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory Heatmap", open)) { ImGui::End(); return; }

    const auto t0 = std::chrono::steady_clock::now();
    drawToolbar(ctrl);
    refresh(ctrl);
    gotoText(ctrl, resolve);
    paintMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = opt_.inspector ? std::min(kSideWidth, avail.x * 0.45f) : 0.0f;
    const ImGuiWindowFlags still = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::BeginChild("##heatmap_map", ImVec2(avail.x - side - (side > 0 ? 6.0f : 0.0f), 0),
                      ImGuiChildFlags_None, still);
    drawCanvas(ctrl, labelOf);
    drawContextMenu(ctrl);
    ImGui::EndChild();
    if (side > 0.0f) {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::BeginChild("##heatmap_side", ImVec2(0, 0), ImGuiChildFlags_Borders);
        drawInspector(ctrl, labelOf);
        ImGui::EndChild();
    }
    ImGui::End();
}

// ── The map ────────────────────────────────────────────────────────────────

bool MemoryHeatmap_ImGui::cellRect(int plane, uint16_t phys, float ox, float oy, float& ax,
                                   float& ay, float& bx, float& by) const
{
    float cx = 0, cy = 0;
    if (!heatview::cellOrigin(layout_, plane, phys, cx, cy)) return false;
    ax = ox + xf_.toScreenX(cx);
    ay = oy + xf_.toScreenY(cy);
    bx = ax + xf_.scale;
    by = ay + xf_.scaleY;
    return true;
}

void MemoryHeatmap_ImGui::drawCanvas(EmulationController& ctrl, const LabelFn& labelOf)
{
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float statusH = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 6.0f;
    const float stripH = monitorStripHeight();
    // Zoomed in until the values show, the region names leave the map (they
    // would cover values) for a wider left gutter beside the page axis.
    const float fs0 = ImGui::GetFontSize();
    regionGutter_ = opt_.regions && !fitPending_ && xf_.scale >= fs0 * 1.35f && xf_.scaleY >= fs0;
    const float gutterLeft = kGutterLeft + (regionGutter_ ? fs0 * 3.6f : 0.0f);
    const float ox = p0.x + gutterLeft, oy = p0.y + kGutterTop;
    const float w = std::max(64.0f, avail.x - gutterLeft - 6.0f);
    const float h = std::max(64.0f, avail.y - kGutterTop - statusH - stripH - 4.0f);
    if (fitPending_) { xf_ = heatview::fit(layout_, w, h, opt_.fill); fitPending_ = false; }
    else if (w != lastW_ || h != lastH_) {
        xf_.scale = std::max(xf_.scale, heatview::fit(layout_, w, h, opt_.fill).scale);
        heatview::clampPan(xf_, layout_, w, h);
    }
    lastW_ = w; lastH_ = h; lastOx_ = ox; lastOy_ = oy;
    if (pendingJump_ && columnOf(pendingLoc_.plane)) {   // a jump the layout can now place
        pendingJump_ = false;
        select(pendingLoc_.plane, pendingLoc_.phys, false);
        float cx = 0, cy = 0;
        if (heatview::cellOrigin(layout_, selPlane_, selPhys_, cx, cy)) {
            // Readable: the value shows in its cell (drawValues' threshold).
            const float readable = std::ceil(ImGui::GetFontSize() * 1.7f);
            heatview::centreOn(xf_, layout_, w, h, cx + 0.5f, cy + 0.5f, std::max(xf_.scale, readable));
        }
        flashLoc_ = pendingLoc_;
        flashUntil_ = ImGui::GetTime() + 1.2;
    }

    ImGui::InvisibleButton("##heatcanvas", ImVec2(std::max(1.0f, avail.x),
                                                  std::max(1.0f, kGutterTop + h)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool itemHovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool inMap = m.x >= ox && m.x < ox + w && m.y >= oy && m.y < oy + h;
    // The minimap (drawn last frame where it is drawn this one) takes its
    // clicks before the map does.
    const bool inButton = (mini_.shown || mini_.pill) && m.x >= mini_.bx0 && m.x < mini_.bx1 &&
                          m.y >= mini_.by0 && m.y < mini_.by1;
    const bool inMini = inButton || (mini_.shown && m.x >= mini_.x0 - 5.0f && m.x < mini_.x1 + 5.0f &&
                                     m.y >= mini_.y0 - 5.0f && m.y < mini_.y1 + 5.0f);
    if (itemHovered && inButton && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        miniCollapsed_ = !miniCollapsed_;   // the "-" on the inset, or the pill
    else if (itemHovered && inMini && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) miniDragging_ = true;
    if (miniDragging_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) miniDragging_ = false;
        else if (mini_.shown) {
            const float cx = (m.x - mini_.x0) / mini_.s, cy = (m.y - mini_.y0) / mini_.sY;
            heatview::centreOn(xf_, layout_, w, h, cx, cy, xf_.scale);
        }
    }

    // ── What is under the pointer ──────────────────────────────────────────
    hovered_ = false;
    if (itemHovered && inMap && !inMini && !miniDragging_) {
        int column = 0;
        uint16_t phys = 0;
        if (heatview::hit(layout_, xf_.toCanvasX(m.x - ox), xf_.toCanvasY(m.y - oy), column, phys)) {
            hovered_ = true;
            hoverPlane_ = layout_.columns[static_cast<std::size_t>(column)].plane;
            hoverPhys_ = phys;
        }
    }

    // ── Mouse: zoom, pan, select, range, menus ─────────────────────────────
    if (itemHovered && inMap && !inMini && io.MouseWheel != 0.0f)
        heatview::zoomAt(xf_, layout_, w, h, m.x - ox, m.y - oy, std::pow(1.25f, io.MouseWheel));
    // Two-finger pinch and Ctrl+wheel (PinchZoom.h): the same zoom, about the
    // fingers; a smart magnify fits. The minimap keeps its own gestures.
    {
        const pinch::Point f = pinch::router().focal();
        const bool onMini = (mini_.shown || mini_.pill) &&
                            ((f.x >= mini_.bx0 && f.x < mini_.bx1 && f.y >= mini_.by0 && f.y < mini_.by1) ||
                             (mini_.shown && f.x >= mini_.x0 - 5.0f && f.x < mini_.x1 + 5.0f &&
                              f.y >= mini_.y0 - 5.0f && f.y < mini_.y1 + 5.0f));
        pinch::Gesture pg;
        if (!onMini && pinchZoomTake(ImVec2(ox, oy), ImVec2(ox + w, oy + h), pg)) {
            if (pg.fit) fitPending_ = true;
            else heatview::zoomAt(xf_, layout_, w, h, pg.focal.x - ox, pg.focal.y - oy, pg.factor());
        }
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !miniDragging_ && !inMini) {
        pressed_ = true;
        panning_ = rangeDragging_ = false;
        if (io.KeyShift && hovered_) {
            rangeDragging_ = true;
            if (!selected_ || selPlane_ != hoverPlane_) rangeAnchor_ = hoverPhys_;
            else rangeAnchor_ = ranged_ ? rangeAnchor_ : selPhys_;
            selected_ = ranged_ = true;
            selPlane_ = hoverPlane_;
        }
    }
    if (pressed_ && !rangeDragging_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
        panning_ = true;
    if (panning_ || (itemHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f))) {
        xf_.panX -= io.MouseDelta.x / xf_.scale;
        xf_.panY -= io.MouseDelta.y / xf_.scaleY;
        heatview::clampPan(xf_, layout_, w, h);
    }
    if (rangeDragging_ && hovered_ && hoverPlane_ == selPlane_) {
        rangeLo_ = std::min(rangeAnchor_, hoverPhys_);
        rangeHi_ = std::max(rangeAnchor_, hoverPhys_);
        selPhys_ = hoverPhys_;
        rangeSummaryAge_ = 0;
    }
    if (pressed_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (!panning_ && !rangeDragging_) {
            if (hovered_) select(hoverPlane_, hoverPhys_, false);
            else if (inMap && !inMini) selected_ = ranged_ = false;
        }
        if (rangeDragging_ && rangeLo_ == rangeHi_) ranged_ = false;
        pressed_ = panning_ = rangeDragging_ = false;
    }
    if (itemHovered && !inMini && !io.KeyShift && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (hovered_) request_ = {Request::Kind::Listing, HeatData::cpuAddress(hoverPlane_, hoverPhys_)};
        else fitPending_ = true;
    }
    if (itemHovered && hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        menuPlane_ = hoverPlane_;
        menuPhys_ = hoverPhys_;
        menuOnRange_ = ranged_ && selPlane_ == hoverPlane_ && hoverPhys_ >= rangeLo_ &&
                       hoverPhys_ <= rangeHi_;
        ImGui::OpenPopup("##heatmenu");
    }
    // Keys only with the focus: hovered alone, they belong to the Apple II.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) handleKeys(ctrl, w, h);

    // ── Textures for this frame's transform, then the drawing ──────────────
    {
        const auto t0 = std::chrono::steady_clock::now();
        paint(w, h);
        paintMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(ox - 1, oy - 1), ImVec2(ox + w + 1, oy + h + 1), kCanvas, 4.0f);
    dl->PushClipRect(ImVec2(ox, oy), ImVec2(ox + w, oy + h), true);
    drawColumns(dl, ox, oy, w, h);
    drawGrid(dl, ox, oy, w, h);
    drawRegions(dl, ox, oy, w, h);
    drawValues(dl, ox, oy, w, h);
    drawLabels(dl, ox, oy, w, h, labelOf);
    drawMarks(dl, ox, oy);
    mini_.shown = drawMinimap(dl, ox, oy, w, h);
    dl->PopClipRect();
    dl->AddRect(ImVec2(ox - 1, oy - 1), ImVec2(ox + w + 1, oy + h + 1), kFrame, 4.0f);
    drawAxes(dl, p0.x, p0.y, ox, oy, w, h);
    if (regionGutter_) drawGutterRegions(dl, p0.x, ox, oy, w, h);

    // ── Tooltip with the loupe, while a byte is smaller than its value ─────
    if (hovered_ && opt_.loupe && std::min(xf_.scale, xf_.scaleY) < 16.0f && !panning_ &&
        !rangeDragging_ && !ImGui::IsPopupOpen("##heatmenu")) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(describeByte(hoverPlane_, hoverPhys_, labelOf).c_str());
        drawLoupe(hoverPlane_, hoverPhys_);
        ImGui::EndTooltip();
    }

    // ── Under the map: FT's monitoring bars, then the status line ──────────
    float y = oy + h + 6.0f;
    if (stripH > 0.0f) {
        drawMonitorStrip(dl, ox, y, w);
        y += stripH;
    }
    drawStatusLine(ctrl, p0.x + 4.0f, y, avail.x - 8.0f, labelOf);
}

void MemoryHeatmap_ImGui::drawColumns(ImDrawList* dl, float ox, float oy, float w, float h)
{
    (void)w; (void)h;
    const float s = xf_.scale, sY = xf_.scaleY;
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    // The map is sampled NEAREST: a byte is a cell, and a pooled texture
    // has one texel a pixel. (The backend's default sampler is linear.)
    if (pio.DrawCallback_SetSamplerNearest) dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const Column& c = columns_[i];
        const float x0 = ox + xf_.toScreenX(layout_.columns[i].x);
        const float x1 = x0 + Layout::kPlane * s;
        float top = 0, bottom = 0;
        rowSpan(layout_, c.plane, top, bottom);
        const float y0 = oy + xf_.toScreenY(top), y1 = oy + xf_.toScreenY(bottom);
        // A pooled texture has one texel a pixel: drawn on whole pixels.
        const float ax = c.pooled ? std::round(x0) : x0, bx = c.pooled ? std::round(x1) : x1;
        const float ay = c.pooled ? std::round(y0) : y0, by = c.pooled ? std::round(y1) : y1;
        const ImVec2 uvTop(0.0f, layout_.bottomUp ? 1.0f : 0.0f);
        const ImVec2 uvBottom(1.0f, layout_.bottomUp ? 0.0f : 1.0f);
        if (c.map.id)
            dl->AddImage(static_cast<ImTextureID>(c.map.id), ImVec2(ax, ay), ImVec2(bx, by), uvTop, uvBottom);
        // The empty rows of the ROM column: said once, quietly.
        if (c.plane == HeatData::kRom) {
            const float ey0 = oy + xf_.toScreenY(std::min(heatview::rowY(layout_, 0), heatview::rowY(layout_, 0xBF)));
            const float ey1 = ey0 + 0xC0 * sY;
            dl->AddRect(ImVec2(x0, ey0 + 3), ImVec2(x1, ey1 - 3), IM_COL32(255, 255, 255, 14), 6.0f);
            const char* note = "no ROM or I/O below $C000";
            const ImVec2 ts = ImGui::CalcTextSize(note);
            if (x1 - x0 > ts.x + 12 && ey1 - ey0 > ts.y * 3)
                dl->AddText(ImVec2((x0 + x1 - ts.x) * 0.5f, (ey0 + ey1 - ts.y) * 0.5f), IM_COL32(255, 255, 255, 46), note);
        }
    }
    if (pio.DrawCallback_SetSamplerLinear) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);

    // The glow: fresh accesses, a quarter of the resolution, sampled linear
    // and ADDED over the map — a short bloom that is gone in a few frames.
    const bool glow = opt_.glow && !opt_.style.cumulative && std::min(s, sY) < 6.0f &&
                      pio.DrawCallback_ResetRenderState;
    if (glow) {
        dl->AddCallback([](const ImDrawList*, const ImDrawCmd*) { glBlendFunc(GL_SRC_ALPHA, GL_ONE); }, nullptr);
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            const Column& c = columns_[i];
            if (!c.glow.id) continue;
            const float x0 = ox + xf_.toScreenX(layout_.columns[i].x);
            const float x1 = x0 + Layout::kPlane * s;
            float top = 0, bottom = 0;
            rowSpan(layout_, c.plane, top, bottom);
            const float y0 = oy + xf_.toScreenY(top), y1 = oy + xf_.toScreenY(bottom);
            // One transparent texel all round: the bloom spills past the edge.
            const float mx = 4.0f * s, my = 4.0f * sY;
            const ImVec2 uvTop(0.0f, layout_.bottomUp ? 1.0f : 0.0f);
            const ImVec2 uvBottom(1.0f, layout_.bottomUp ? 0.0f : 1.0f);
            dl->AddImage(static_cast<ImTextureID>(c.glow.id), ImVec2(x0 - mx, y0 - my), ImVec2(x1 + mx, y1 + my),
                         uvTop, uvBottom, IM_COL32(255, 255, 255, 150));
        }
        dl->AddCallback(pio.DrawCallback_ResetRenderState, nullptr);
    }

    if (!opt_.rails) return;
    // Rails: bank (read / write / both, in the palette), then the video.
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const Column& c = columns_[i];
        const float x1 = ox + xf_.toScreenX(layout_.columns[i].x) + Layout::kPlane * s;
        const float bx = x1 + 3.0f, vx = x1 + 9.0f;
        for (int r = heatview::firstRow(c.plane); r < 256;) {
            const uint8_t v = c.rails.bank[static_cast<std::size_t>(r)];
            int e = r;
            while (e + 1 < 256 && c.rails.bank[static_cast<std::size_t>(e + 1)] == v) ++e;
            if (v) {
                const float ya = oy + xf_.toScreenY(std::min(heatview::rowY(layout_, r), heatview::rowY(layout_, e)));
                dl->AddRectFilled(ImVec2(bx, ya), ImVec2(bx + 4.0f, ya + (e - r + 1) * sY),
                                  colour(heatview::railColour(v, opt_.style.palette)), 1.5f);
            }
            r = e + 1;
        }
        for (int r = 0; r < 256; ++r) {
            if (!c.rails.shown[static_cast<std::size_t>(r)]) continue;
            int e = r;
            while (e + 1 < 256 && c.rails.shown[static_cast<std::size_t>(e + 1)]) ++e;
            const float ya = oy + xf_.toScreenY(std::min(heatview::rowY(layout_, r), heatview::rowY(layout_, e)));
            dl->AddRectFilled(ImVec2(vx, ya), ImVec2(vx + 4.0f, ya + (e - r + 1) * sY),
                              IM_COL32(236, 236, 240, 255), 1.5f);
            r = e;
        }
    }
}

void MemoryHeatmap_ImGui::drawGrid(ImDrawList* dl, float ox, float oy, float w, float h)
{
    const float s = xf_.scale, sY = xf_.scaleY;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const int plane = columns_[i].plane;
        const float x0 = ox + xf_.toScreenX(layout_.columns[i].x);
        const float x1 = x0 + Layout::kPlane * s;
        float top = 0, bottom = 0;
        rowSpan(layout_, plane, top, bottom);
        const float y0 = oy + xf_.toScreenY(top), y1 = oy + xf_.toScreenY(bottom);
        const int r0 = heatview::firstRow(plane);
        // Rows: every $1000 when there is room, every page from 6 px a page.
        for (int r = r0 + 1; r < 256; ++r) {
            const bool big = (r & 0x0F) == 0;
            if (!big && sY < 6.0f) continue;
            if (big && sY * 16.0f < 6.0f) continue;
            const float edge = layout_.bottomUp ? heatview::rowY(layout_, r) + 1.0f : heatview::rowY(layout_, r);
            const float y = oy + xf_.toScreenY(edge);
            if (y < oy || y > oy + h) continue;
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), IM_COL32(255, 255, 255, big ? 16 : 9));
        }
        // Columns: every 16 bytes from 24 px a group, every byte from 10 px.
        for (int c = 1; c < 256; ++c) {
            const bool group = (c & 0x0F) == 0;
            if (!group && s < 10.0f) continue;
            if (group && s * 16.0f < 24.0f) continue;
            const float x = x0 + static_cast<float>(c) * s;
            if (x < ox || x > ox + w) continue;
            dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(255, 255, 255, group ? 16 : 9));
        }
    }
}

void MemoryHeatmap_ImGui::drawRegions(ImDrawList* dl, float ox, float oy, float w, float h)
{
    (void)w; (void)h;
    if (!opt_.regions) return;
    const float s = xf_.scale, sY = xf_.scaleY;
    const float fs = ImGui::GetFontSize();
    // Quiet until hovered: hairline boundaries, faint tags; the region under
    // the pointer is outlined and named in full.
    const heatview::Region* hot = hovered_ ? heatview::regionAt(hoverPlane_, hoverPhys_) : nullptr;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const int plane = columns_[i].plane;
        const float x0 = ox + xf_.toScreenX(layout_.columns[i].x);
        const float x1 = x0 + Layout::kPlane * s;
        for (const heatview::Region& r : heatview::regions(plane)) {
            const int lo = r.lo >> 8, hi = r.hi >> 8;
            const float yLo = oy + xf_.toScreenY(layout_.bottomUp ? heatview::rowY(layout_, lo) + 1.0f
                                                                  : heatview::rowY(layout_, lo));
            const float yHi = oy + xf_.toScreenY(layout_.bottomUp ? heatview::rowY(layout_, hi)
                                                                  : heatview::rowY(layout_, hi) + 1.0f);
            const bool isHot = &r == hot && plane == hoverPlane_;
            if (lo > heatview::firstRow(plane))
                dl->AddLine(ImVec2(x0, yLo), ImVec2(x1, yLo), IM_COL32(255, 255, 255, isHot ? 60 : 22));
            if (isHot)
                dl->AddRect(ImVec2(x0, std::min(yLo, yHi)), ImVec2(x1, std::max(yLo, yHi)),
                            IM_COL32(255, 255, 255, 70), 0.0f, 0, 1.0f);
            if (regionGutter_ || (!*r.tag && !isHot)) continue;   // zoomed in: the gutter names them
            const float rowsPx = static_cast<float>(hi - lo + 1) * sY;
            const float tf = fs * 0.8f;
            if (rowsPx < tf + 2.0f) continue;
            const char* text = isHot ? r.name : r.tag;
            const float yt = std::min(yLo, yHi) + 2.0f;
            dl->AddText(nullptr, tf, ImVec2(x0 + 4, yt), isHot ? IM_COL32(240, 242, 250, 235)
                                                                : IM_COL32(200, 206, 222, 92), text);
        }
    }
}

void MemoryHeatmap_ImGui::drawGutterRegions(ImDrawList* dl, float gx, float ox, float oy, float w, float h)
{
    // The regions of the column at the view's centre, named in the left
    // gutter: a bar along the axis spans each region's rows, its name sticks
    // to the top of the view while the region is on screen.
    if (columns_.empty()) return;
    const float cx = xf_.toCanvasX(w * 0.5f);
    std::size_t col = 0;
    for (std::size_t i = 0; i < columns_.size(); ++i)
        if (cx >= layout_.columns[i].x - Layout::kGap * 0.5f) col = i;
    const int plane = columns_[col].plane;
    const float sY = xf_.scaleY;
    const float fs = ImGui::GetFontSize(), tf = fs * 0.8f;
    const heatview::Region* hot = hovered_ && hoverPlane_ == plane ? heatview::regionAt(plane, hoverPhys_) : nullptr;
    dl->PushClipRect(ImVec2(gx, oy), ImVec2(ox, oy + h), true);
    int n = 0;
    for (const heatview::Region& r : heatview::regions(plane)) {
        const int lo = r.lo >> 8, hi = r.hi >> 8;
        const float ya = oy + xf_.toScreenY(std::min(heatview::rowY(layout_, lo), heatview::rowY(layout_, hi)));
        const float yb = ya + static_cast<float>(hi - lo + 1) * sY;
        ++n;
        if (yb < oy || ya > oy + h) continue;
        const bool isHot = &r == hot;
        const ImU32 bar = isHot ? IM_COL32(255, 210, 64, 230) : (n & 1) ? IM_COL32(150, 158, 180, 110)
                                                                         : IM_COL32(110, 118, 140, 110);
        dl->AddRectFilled(ImVec2(ox - 5.0f, std::max(ya, oy) + 1.0f), ImVec2(ox - 3.0f, std::min(yb, oy + h) - 1.0f), bar);
        const char* text = *r.tag ? r.tag : "free";
        const float ty = std::clamp(ya + 2.0f, oy + 2.0f, yb - tf - 2.0f);
        if (yb - std::max(ya, oy) < tf + 2.0f) continue;
        dl->AddText(nullptr, tf, ImVec2(gx + 4.0f, ty),
                    isHot ? IM_COL32(255, 228, 140, 255) : *r.tag ? IM_COL32(196, 202, 218, 220)
                                                              : IM_COL32(140, 146, 162, 150), text);
    }
    dl->PopClipRect();
}

void MemoryHeatmap_ImGui::drawValues(ImDrawList* dl, float ox, float oy, float w, float h)
{
    const float s = xf_.scale, sY = xf_.scaleY;
    const float fs = ImGui::GetFontSize();
    if (s < fs * 1.35f || sY < fs) return;
    const float cx0 = xf_.toCanvasX(0.0f), cx1 = xf_.toCanvasX(w);
    const float cy0 = xf_.toCanvasY(0.0f), cy1 = xf_.toCanvasY(h);
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const Column& c = columns_[i];
        const float colX = layout_.columns[i].x;
        const int b0 = std::max(0, static_cast<int>(std::floor(cx0 - colX)));
        const int b1 = std::min(255, static_cast<int>(std::floor(cx1 - colX)));
        if (b0 > b1) continue;
        const int y0 = std::max(0, static_cast<int>(std::floor(cy0)));
        const int y1 = std::min(255, static_cast<int>(std::floor(cy1)));
        for (int y = y0; y <= y1; ++y) {
            const int row = layout_.bottomUp ? 255 - y : y;
            if (row < heatview::firstRow(c.plane)) continue;
            for (int bcol = b0; bcol <= b1; ++bcol) {
                const auto phys = static_cast<uint16_t>((row << 8) | bcol);
                if (c.plane == HeatData::kRom && phys < 0xC100) continue;   // no value read there
                const uint32_t px = c.rgba[phys];
                const int lum = static_cast<int>((px & 0xFF) * 3 + ((px >> 8) & 0xFF) * 6 + ((px >> 16) & 0xFF)) / 10;
                char hex[4];
                std::snprintf(hex, sizeof hex, "%02X", c.values[phys]);
                const ImVec2 ts = ImGui::CalcTextSize(hex);
                const float sx = ox + xf_.toScreenX(colX + static_cast<float>(bcol));
                const float sy = oy + xf_.toScreenY(static_cast<float>(y));
                dl->AddText(ImVec2(sx + (s - ts.x) * 0.5f, sy + (sY - ts.y) * 0.5f),
                            lum > 140 ? IM_COL32(10, 10, 14, 230) : IM_COL32(226, 230, 240, 170), hex);
            }
        }
    }
}

void MemoryHeatmap_ImGui::drawLabels(ImDrawList* dl, float ox, float oy, float w, float h,
                                     const LabelFn& labelOf)
{
    const float s = xf_.scale, sY = xf_.scaleY;
    if (!opt_.labels || !labelOf || s < 12.0f || sY < 12.0f) return;
    const int cx0 = static_cast<int>(std::floor(xf_.toCanvasX(0.0f)));
    const int cx1 = static_cast<int>(std::floor(xf_.toCanvasX(w)));
    const int cy0 = static_cast<int>(std::floor(xf_.toCanvasY(0.0f)));
    const int cy1 = static_cast<int>(std::floor(xf_.toCanvasY(h)));
    const int key[5] = {cx0, cx1, cy0, cy1, static_cast<int>(columns_.size())};
    // The visible bytes' labels, looked up again when the view moves or
    // every second (symbols may have been loaded meanwhile).
    if (!std::equal(std::begin(key), std::end(key), std::begin(labelKey_)) || --labelAge_ <= 0) {
        std::copy(std::begin(key), std::end(key), std::begin(labelKey_));
        labelAge_ = 60;
        labelCache_.clear();
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            const int plane = columns_[i].plane;
            const int colX = static_cast<int>(layout_.columns[i].x);
            const int b0 = std::max(0, cx0 - colX), b1 = std::min(255, cx1 - colX);
            if (b0 > b1) continue;
            for (int y = std::max(0, cy0); y <= std::min(255, cy1); ++y) {
                const int row = layout_.bottomUp ? 255 - y : y;
                if (row < heatview::firstRow(plane)) continue;
                for (int b = b0; b <= b1; ++b) {
                    const auto phys = static_cast<uint16_t>((row << 8) | b);
                    const DebugAddress at = HeatData::cpuAddress(plane, phys);
                    std::string l = labelOf(at.addr, at.bank);
                    if (!l.empty()) labelCache_.push_back({plane, phys, std::move(l)});
                }
            }
        }
    }
    // No value hidden: a small marker in the cell's corner; the name in the
    // cell's top margin when there is room above the value; the whole name
    // on a flag only for the byte under the pointer or the selection (the
    // status line and the inspector name it too).
    const float fs = ImGui::GetFontSize(), tf = fs * 0.7f;
    const float margin = (sY - fs) * 0.5f;
    for (const LabelHit& hit : labelCache_) {
        float ax = 0, ay = 0, bx = 0, by = 0;
        if (!cellRect(hit.plane, hit.phys, ox, oy, ax, ay, bx, by)) continue;
        const float t = std::min(7.0f, std::min(s, sY) * 0.35f);
        dl->AddTriangleFilled(ImVec2(ax + 1, ay + 1), ImVec2(ax + 1 + t, ay + 1), ImVec2(ax + 1, ay + 1 + t),
                              IM_COL32(120, 190, 255, 235));
        const bool focus = (hovered_ && hit.plane == hoverPlane_ && hit.phys == hoverPhys_) ||
                           (selected_ && hit.plane == selPlane_ && hit.phys == selPhys_);
        if (focus) {
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(tf, FLT_MAX, 0.0f, hit.text.c_str());
            const float fy = ay - ts.y - 4.0f >= oy ? ay - ts.y - 4.0f : by + 1.0f;   // above, else below
            dl->AddRectFilled(ImVec2(ax, fy), ImVec2(ax + ts.x + 8.0f, fy + ts.y + 3.0f), IM_COL32(16, 22, 36, 240), 3.0f);
            dl->AddRect(ImVec2(ax, fy), ImVec2(ax + ts.x + 8.0f, fy + ts.y + 3.0f), IM_COL32(120, 190, 255, 160), 3.0f);
            dl->AddText(nullptr, tf, ImVec2(ax + 4.0f, fy + 1.5f), IM_COL32(170, 215, 255, 255), hit.text.c_str());
        } else if (margin >= tf + 2.0f) {
            dl->PushClipRect(ImVec2(ax + t + 2.0f, ay), ImVec2(bx - 1.0f, ay + margin), true);
            dl->AddText(nullptr, tf, ImVec2(ax + t + 3.0f, ay + (margin - tf) * 0.5f), IM_COL32(150, 205, 255, 230),
                        hit.text.c_str());
            dl->PopClipRect();
        }
    }
}

void MemoryHeatmap_ImGui::drawMarks(ImDrawList* dl, float ox, float oy)
{
    const float s = xf_.scale, sY = xf_.scaleY;
    float ax = 0, ay = 0, bx = 0, by = 0;
    // Crosshair on the hovered byte: a band on its page (one row is one page
    // in every column) and its byte column, quiet.
    if (hovered_ && cellRect(hoverPlane_, hoverPhys_, ox, oy, ax, ay, bx, by)) {
        const float left = ox + xf_.toScreenX(0.0f), right = ox + xf_.toScreenX(layout_.width);
        float top = 0, bottom = 0;
        rowSpan(layout_, hoverPlane_, top, bottom);
        const float colTop = oy + xf_.toScreenY(top), colBottom = oy + xf_.toScreenY(bottom);
        dl->AddRectFilled(ImVec2(left, ay), ImVec2(right, std::max(by, ay + 1.0f)), IM_COL32(255, 255, 255, 16));
        dl->AddRectFilled(ImVec2(ax, colTop), ImVec2(std::max(bx, ax + 1.0f), colBottom), IM_COL32(255, 255, 255, 16));
        const float grow = std::min(s, sY) < 4.0f ? 2.0f : 0.5f;
        dl->AddRect(ImVec2(ax - grow, ay - grow), ImVec2(bx + grow, by + grow), IM_COL32(255, 255, 255, 235), 0.0f, 0, 1.5f);
    }
    if (ranged_ && columnOf(selPlane_)) {
        for (int row = rangeLo_ >> 8; row <= (rangeHi_ >> 8); ++row) {
            const auto first = static_cast<uint16_t>(std::max<int>(rangeLo_, row << 8));
            const auto last = static_cast<uint16_t>(std::min<int>(rangeHi_, (row << 8) | 0xFF));
            float fa = 0, fb = 0, fc = 0, fd = 0, la = 0, lb = 0, lc = 0, ld = 0;
            if (!cellRect(selPlane_, first, ox, oy, fa, fb, fc, fd) ||
                !cellRect(selPlane_, last, ox, oy, la, lb, lc, ld)) continue;
            dl->AddRectFilled(ImVec2(fa, fb), ImVec2(lc, ld), IM_COL32(255, 210, 64, 64));
        }
        if (cellRect(selPlane_, rangeLo_, ox, oy, ax, ay, bx, by))
            dl->AddRect(ImVec2(ax - 1, ay - 1), ImVec2(bx + 1, by + 1), kGold, 0.0f, 0, 2.0f);
        if (cellRect(selPlane_, rangeHi_, ox, oy, ax, ay, bx, by))
            dl->AddRect(ImVec2(ax - 1, ay - 1), ImVec2(bx + 1, by + 1), kGold, 0.0f, 0, 2.0f);
    }
    if (selected_ && cellRect(selPlane_, selPhys_, ox, oy, ax, ay, bx, by)) {
        // A pulsing gold box; small cells get a target of four ticks.
        const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
        const ImU32 col = IM_COL32(255, 210, 64, static_cast<int>(255 * pulse));
        dl->AddRect(ImVec2(ax - 1.5f, ay - 1.5f), ImVec2(bx + 1.5f, by + 1.5f), col, 0.0f, 0, 2.0f);
        if (std::min(s, sY) < 6.0f) {
            const float cx = (ax + bx) * 0.5f, cy = (ay + by) * 0.5f, r0 = 4.0f, r1 = 12.0f;
            dl->AddLine(ImVec2(cx - r1, cy), ImVec2(cx - r0, cy), col, 2.0f);
            dl->AddLine(ImVec2(cx + r0, cy), ImVec2(cx + r1, cy), col, 2.0f);
            dl->AddLine(ImVec2(cx, cy - r1), ImVec2(cx, cy - r0), col, 2.0f);
            dl->AddLine(ImVec2(cx, cy + r0), ImVec2(cx, cy + r1), col, 2.0f);
        }
    }
    // Where a jump landed: a ring that closes on the byte, then fades.
    const double left = flashUntil_ - ImGui::GetTime();
    if (left > 0.0 && cellRect(flashLoc_.plane, flashLoc_.phys, ox, oy, ax, ay, bx, by)) {
        const float t = static_cast<float>(left / 1.2);     // 1 → 0
        const float cx = (ax + bx) * 0.5f, cy = (ay + by) * 0.5f;
        const float r = std::max(bx - ax, by - ay) * 0.75f + 40.0f * t * t;
        dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(255, 230, 140, static_cast<int>(230 * t)), 40, 2.5f);
    }
}

void MemoryHeatmap_ImGui::drawAxes(ImDrawList* dl, float gx, float gy, float ox, float oy,
                                   float w, float h)
{
    const float s = xf_.scale, sY = xf_.scaleY;
    const float fs = ImGui::GetFontSize();
    // Titles and byte offsets above each column. A title sits over the part
    // of ITS column that is on screen — never over another column's.
    dl->PushClipRect(ImVec2(ox - 2, gy), ImVec2(ox + w + 2, oy), true);
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const int plane = columns_[i].plane;
        const float x0 = ox + xf_.toScreenX(layout_.columns[i].x);
        const float x1 = x0 + Layout::kPlane * s;
        const float v0 = std::max(x0, ox), v1 = std::min(x1, ox + w);
        if (v1 - v0 > 8.0f) {
            const std::string title = heatview::planeTitle(plane, machine_.auxBanks);
            const bool cpuBank = HeatData::isAux(plane) && plane - HeatData::kAux0 == machine_.cpuAuxSlot &&
                                 machine_.auxBanks > 1;
            const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
            const char* sub = planeSubtitle(plane);
            const float subFs = fs * 0.82f;
            const ImVec2 ss = ImGui::GetFont()->CalcTextSizeA(subFs, FLT_MAX, 0.0f, sub);
            const bool withSub = ts.x + 10.0f + ss.x <= v1 - v0;
            const float total = withSub ? ts.x + 10.0f + ss.x : ts.x;
            if (total <= v1 - v0 + 0.5f) {
                // Centred on what is visible of the column: it follows a pan.
                const float tx = std::clamp((v0 + v1 - total) * 0.5f, v0, v1 - total);
                dl->AddText(ImVec2(tx, gy + 1), IM_COL32(232, 234, 242, 255), title.c_str());
                if (withSub)
                    dl->AddText(nullptr, subFs, ImVec2(tx + ts.x + 10.0f, gy + 1 + fs * 0.12f), kMuted, sub);
                if (cpuBank)
                    dl->AddText(ImVec2(tx - fs, gy + 1), kGold, ICON_FA_CARET_RIGHT);
            }
        }
        // Offsets: every byte from 22 px, every 16 from 22 px a group.
        const bool each = s >= 22.0f;
        if (!each && s * 16.0f < 22.0f) continue;
        for (int c = 0; c < 256; c += each ? 1 : 16) {
            const float x = x0 + c * s;
            if (x + s < ox || x > ox + w) continue;
            char t[6];
            std::snprintf(t, sizeof t, "+%02X", c);
            const ImVec2 os = ImGui::CalcTextSize(t);
            const float span = each ? s : 16.0f * s;
            const bool lit = hovered_ && hoverPlane_ == plane &&
                             (hoverPhys_ & 0xFF) / (each ? 1 : 16) == c / (each ? 1 : 16);
            dl->AddText(nullptr, fs * 0.85f, ImVec2(x + (span - os.x * 0.85f) * 0.5f, oy - fs - 3),
                        lit ? IM_COL32(255, 255, 255, 255) : kMuted, t);
        }
    }
    dl->PopClipRect();
    // Page labels on the left: as dense as the zoom allows.
    dl->PushClipRect(ImVec2(gx, oy - fs * 0.5f), ImVec2(ox, oy + h + fs * 0.5f), true);
    int step = 1;
    while (step < 64 && step * sY < fs + 3.0f) step *= 2;
    for (int r = 0; r < 256; r += step) {
        const float y = oy + xf_.toScreenY(heatview::rowY(layout_, r)) + (sY - fs) * 0.5f;
        if (y < oy - fs || y > oy + h) continue;
        char t[8];
        std::snprintf(t, sizeof t, "$%02X00", r);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(ox - ts.x - 6, y), kMuted, t);
    }
    if (hovered_) {   // the hovered page, always, highlighted
        const int r = hoverPhys_ >> 8;
        const float y = oy + xf_.toScreenY(heatview::rowY(layout_, r)) + (sY - fs) * 0.5f;
        char t[8];
        std::snprintf(t, sizeof t, "$%02X00", r);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddRectFilled(ImVec2(ox - ts.x - 9, y - 1), ImVec2(ox - 2, y + ts.y + 1),
                          IM_COL32(56, 64, 86, 255), 3.0f);
        dl->AddText(ImVec2(ox - ts.x - 6, y), IM_COL32(255, 255, 255, 255), t);
    }
    dl->PopClipRect();
}

bool MemoryHeatmap_ImGui::screenPointOf(int plane, uint16_t phys, float& x, float& y) const
{
    float cx = 0, cy = 0;
    if (!heatview::cellOrigin(layout_, plane, phys, cx, cy)) return false;
    x = lastOx_ + xf_.toScreenX(cx + 0.5f);
    y = lastOy_ + xf_.toScreenY(cy + 0.5f);
    return true;
}

}  // namespace pom2
