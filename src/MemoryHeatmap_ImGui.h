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

// MemoryHeatmap_ImGui — Tools ▸ Memory Heatmap (UI-022).
//
// AppleWin FT's memory heatmap (fenarinarsa, MemoryHeatmapView.h) in a
// window of its own, live while the machine runs: the ROM / I/O column, MAIN
// and AUX (any RamWorks bank) side by side, one pixel a byte, $0000 at the
// bottom, FT's colours, rails, background and Monitoring-card bars. And what
// a window allows:
//
//   * one toolbar row: the mode, the four kinds as coloured chips (they are
//     the legend), the fade, Clear / Freeze / PNG, a View popup with every
//     other choice, Go to with Back / Forward;
//   * zoom (wheel, +/-) down to the byte, its hex value in its cell and the
//     debugger's labels where they fall; drag to pan; Home to fit; a minimap
//     while zoomed in; below a pixel a byte the map is MAX-POOLED (the
//     hottest byte of each pixel wins), fresh accesses glow;
//   * a status line, always: the byte under the pointer (or the selection)
//     as a breadcrumb, its bank, label, value, where it shows on the screen
//     (text row / column, hi-res line / byte / dots) and its last accesses;
//   * click: select — the inspector shows everything about the byte; arrows
//     move it; shift-drag: a range to break on in its bank; double-click or
//     Enter: the listing; right click: BP / BPMR / BPMW / Watch;
//   * the hottest bytes since Clear (a click jumps there); French Touch's
//     $C066-$C069 section / IRQ timings (FT's two bars) and POM2's own
//     interrupt load per frame.
//
// The recording (MemoryHeatmap) is attached only while this window is open:
// closed, it costs the emulator nothing. Drawn every frame (closed too) so
// the frame it is shut detaches it.
//
// Files: MemoryHeatmap_ImGui.cpp (window, refresh, the map), _Paint.cpp (the
// textures: pooled, glow, minimap), _Toolbar.cpp (toolbar, View popup, Go to
// and history, keys), _Status.cpp (status line, minimap, monitor bars),
// _Panel.cpp (inspector, loupe, menus, breakpoints, legend, PNG).

#ifndef POM2_MEMORY_HEATMAP_IMGUI_H
#define POM2_MEMORY_HEATMAP_IMGUI_H

#include "CycleMonitor.h"
#include "DebugAddress.h"
#include "InterruptLoad.h"
#include "MemoryHeatmap.h"
#include "MemoryHeatmapView.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class EmulationController;
struct ImDrawList;

namespace pom2 {

class MemoryHeatmap_ImGui
{
public:
    MemoryHeatmap_ImGui() = default;
    ~MemoryHeatmap_ImGui();
    MemoryHeatmap_ImGui(const MemoryHeatmap_ImGui&) = delete;
    MemoryHeatmap_ImGui& operator=(const MemoryHeatmap_ImGui&) = delete;

    /// The label the loaded symbols give an address in a bank ("" if none).
    using LabelFn = std::function<std::string(uint16_t, DebugBank)>;
    /// An address, "bank:$addr" or a label, as the Debugger reads them.
    using ResolveFn = std::function<bool(std::string_view, DebugAddress&)>;

    /// Call every frame. Attaches the recording while `*open`, detaches it
    /// (and frees it) on the frame the window is closed. Takes stateMutex
    /// itself: the caller must not hold it.
    void render(EmulationController& ctrl, bool* open, const LabelFn& labelOf = {},
                const ResolveFn& resolve = {});
    /// Detach before the controller goes (shutdown).
    void detach(EmulationController& ctrl);

    /// Something for another window, applied by the caller with the lock
    /// released: the listing, the Memory viewer, the Watch window.
    struct Request {
        enum class Kind : uint8_t { None, Listing, MemoryViewer, Watch, WatchWord };
        Kind kind = Kind::None;
        DebugAddress at;
    };
    bool takeRequest(Request& out);

    /// Where a byte's cell centre was drawn on the last frame (screen
    /// pixels), false when its plane is not drawn. For a probe that drives
    /// the window with a mouse of its own (tests/heatmap_ui_probe.cpp).
    bool screenPointOf(int plane, uint16_t phys, float& x, float& y) const;
    /// The selected byte / range (false: none).
    bool selectedByte(int& plane, uint16_t& phys) const
    { plane = selPlane_; phys = selPhys_; return selected_; }
    bool selectedRange(uint16_t& lo, uint16_t& hi) const
    { lo = rangeLo_; hi = rangeHi_; return ranged_; }
    float zoom() const { return xf_.scale; }

    /// The screen rectangle of the minimap drawn on the last frame (false:
    /// none — not zoomed in, or switched off). For the probe.
    bool minimapRect(float& x0, float& y0, float& x1, float& y1) const
    { x0 = mini_.x0; y0 = mini_.y0; x1 = mini_.x1; y1 = mini_.y1; return mini_.shown; }
    /// The View button's centre, and the View popup's rectangle as last
    /// drawn (false: never drawn). For the probe.
    void viewButtonCentre(float& x, float& y) const
    { x = (viewBtn_[0] + viewBtn_[2]) * 0.5f; y = (viewBtn_[1] + viewBtn_[3]) * 0.5f; }
    bool viewPopupRect(float& x0, float& y0, float& x1, float& y1) const
    { x0 = viewPopup_[0]; y0 = viewPopup_[1]; x1 = viewPopup_[2]; y1 = viewPopup_[3]; return x1 > x0; }
    /// The minimap's collapse button, or the pill of a collapsed one (false:
    /// neither is drawn). For the probe.
    bool minimapButtonCentre(float& x, float& y) const
    { x = (mini_.bx0 + mini_.bx1) * 0.5f; y = (mini_.by0 + mini_.by1) * 0.5f; return mini_.shown || mini_.pill; }
    /// The canvas point at the centre of the view (for the probe).
    void viewCentre(float& cx, float& cy) const
    { cx = xf_.toCanvasX(lastW_ * 0.5f); cy = xf_.toCanvasY(lastH_ * 0.5f); }
    /// How long the last refresh + paint took the UI thread (ms).
    double lastPaintMs() const { return paintMs_; }

    /// The options, as one settings value (`memory_heatmap_options`).
    std::string optionsText() const { return heatview::optionsText(opt_); }
    // The look is not a user choice any more (owner, 2026-11-28): a saved
    // palette / background / trail / glow from an older version is ignored.
    void setOptionsText(std::string_view text)
    {
        opt_ = heatview::parseOptions(text);
        const heatview::Options defaults;
        opt_.style.palette = defaults.style.palette;
        opt_.style.background = defaults.style.background;
        opt_.style.trail = defaults.style.trail;
        opt_.glow = defaults.glow;
        fitPending_ = true;
    }

private:
    // ── What one frame draws (MemoryHeatmap_ImGui.cpp, _Paint.cpp) ─────────
    struct GlTex { unsigned id = 0; int w = 0, h = 0; bool linear = false; };
    struct Column {
        int plane = HeatData::kMain;
        HeatData::PlaneCopy copy;
        std::vector<uint8_t> values;     // 64 K, physical layout
        heatview::Rails rails;
        std::vector<heatview::HeatCell> cells;   // the plane's rows, 256 a row
        std::vector<uint32_t> rgba;      // 256 x 256 native colours (loupe, PNG)
        float logMax = 0.0f;
        GlTex map;                       // native, or max-pooled to the view
        GlTex glow;                      // the fresh accesses, 1/4 scale, linear
        GlTex mini;                      // the minimap's picture
        bool pooled = false;
    };
    struct Machine {
        bool iie = false, iicClass = false;
        int  auxBanks = 1;               // RamWorks backing slots
        int  cpuAuxSlot = 0;             // the bank the CPU selects
        int  frameCycles = 17030;
        int  clockHz = 1022727;
        uint32_t now = 0;
        std::vector<InterruptLoad::Frame> irq;
        InterruptLoad::Summary irqSummary;
        CycleMonitor monitor;
    };
    struct Loc { int plane = HeatData::kMain; uint16_t phys = 0; };
    /// The minimap's inset (the picture's corners, its scale), the collapse
    /// button on it, and the pill that brings a collapsed one back.
    struct MiniMap {
        bool shown = false, pill = false;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0, s = 1, sY = 1;
        float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;   // collapse button / pill
    };

    void refresh(EmulationController& ctrl);
    std::vector<int> wantedPlanes() const;
    void drawCanvas(EmulationController& ctrl, const LabelFn& labelOf);
    void drawColumns(ImDrawList* dl, float ox, float oy, float w, float h);
    void drawGrid(ImDrawList* dl, float ox, float oy, float w, float h);
    void drawValues(ImDrawList* dl, float ox, float oy, float w, float h);
    void drawRegions(ImDrawList* dl, float ox, float oy, float w, float h);
    void drawGutterRegions(ImDrawList* dl, float gx, float ox, float oy, float w, float h);
    void drawLabels(ImDrawList* dl, float ox, float oy, float w, float h, const LabelFn& labelOf);
    void drawMarks(ImDrawList* dl, float ox, float oy);
    void drawAxes(ImDrawList* dl, float gx, float gy, float ox, float oy, float w, float h);
    bool cellRect(int plane, uint16_t phys, float ox, float oy, float& ax, float& ay,
                  float& bx, float& by) const;

    // ── Textures (MemoryHeatmap_ImGui_Paint.cpp) ───────────────────────────
    void paint(float w, float h);
    static void upload(GlTex& t, const uint32_t* px, int w, int h, bool linear);
    static void release(GlTex& t);

    // ── Toolbar, Go to, history, keys (MemoryHeatmap_ImGui_Toolbar.cpp) ────
    void drawToolbar(EmulationController& ctrl);
    void drawViewPopup();
    void gotoText(EmulationController& ctrl, const ResolveFn& resolve);
    void handleKeys(EmulationController& ctrl, float w, float h);
    void toggleFreeze(EmulationController& ctrl);
    /// Select `phys` of `plane`, make its bank visible, zoom to where its
    /// value is readable, centre and flash it; recorded in the history.
    void jumpTo(int plane, uint16_t phys, bool record = true);
    void historyStep(int delta);

    // ── Status line, minimap, monitor bars (MemoryHeatmap_ImGui_Status.cpp)
    void drawStatusLine(EmulationController& ctrl, float x, float y, float w, const LabelFn& labelOf);
    bool drawMinimap(ImDrawList* dl, float ox, float oy, float w, float h);
    float monitorStripHeight() const;
    void drawMonitorStrip(ImDrawList* dl, float x, float y, float w);
    void drawMonitorSection();

    // ── Beside the map (MemoryHeatmap_ImGui_Panel.cpp) ─────────────────────
    void drawInspector(EmulationController& ctrl, const LabelFn& labelOf);
    void drawLoupe(int plane, uint16_t phys);
    void drawContextMenu(EmulationController& ctrl);
    void drawLegend();
    void exportPng();
    /// Arm (or with `access` 0, remove) a memory breakpoint on each byte of
    /// [lo, hi] of `plane`, in the bank the byte answers in.
    void armRange(EmulationController& ctrl, int plane, uint16_t lo, uint16_t hi, int access);
    void armPc(EmulationController& ctrl, const DebugAddress& at);
    HeatData::ByteInfo infoOf(EmulationController& ctrl, int plane, uint16_t phys);
    void select(int plane, uint16_t phys, bool centre);
    void say(std::string text);
    const Column* columnOf(int plane) const;
    uint8_t valueOf(int plane, uint16_t phys) const;
    std::string describeAge(uint32_t stamp) const;
    std::string shortAge(uint32_t stamp) const;
    std::string describeByte(int plane, uint16_t phys, const LabelFn& labelOf) const;

    std::unique_ptr<MemoryHeatmap> heat_;
    std::unique_ptr<HeatData> frozen_;    // the picture Freeze keeps
    std::vector<Column> columns_;
    Machine machine_;
    heatview::Options opt_;
    heatview::Layout layout_;
    heatview::Transform xf_;
    bool  fitPending_ = true;
    bool  wantClear_ = false, wantMonitorReset_ = false;
    float lastW_ = 0.0f, lastH_ = 0.0f, lastOx_ = 0.0f, lastOy_ = 0.0f;
    double paintMs_ = 0.0;
    MiniMap mini_;
    bool miniDragging_ = false;
    bool miniCollapsed_ = false;          // this session only
    bool miniTop_ = false;                // moved up: something it would hide is below
    float viewBtn_[4] = {0, 0, 0, 0};     // the View button, for the probe
    float viewPopup_[4] = {0, 0, 0, 0};   // the View popup, last drawn
    bool regionGutter_ = false;           // region names in the left gutter (zoomed in)

    // Pointer and selection.
    bool hovered_ = false;
    int  hoverPlane_ = HeatData::kMain;
    uint16_t hoverPhys_ = 0;
    bool selected_ = false;
    int  selPlane_ = HeatData::kMain;
    uint16_t selPhys_ = 0;
    bool ranged_ = false;                 // a range [rangeLo_, rangeHi_] of selPlane_
    uint16_t rangeLo_ = 0, rangeHi_ = 0, rangeAnchor_ = 0;
    bool rangeDragging_ = false, panning_ = false, pressed_ = false;
    int  menuPlane_ = HeatData::kMain;
    uint16_t menuPhys_ = 0;
    bool menuOnRange_ = false;
    bool focusGoto_ = false, gotoPending_ = false;
    bool pendingJump_ = false;            // a jump, placed once the layout has its plane
    Loc  pendingLoc_;
    std::array<char, 48> gotoText_{};
    std::vector<Loc> history_;            // Back / Forward (Alt+Left / Alt+Right)
    int  historyPos_ = -1;
    double flashUntil_ = 0.0;             // the byte a jump landed on, ringed
    Loc  flashLoc_;

    // The debugger's labels in view, cached (a lookup per byte per frame
    // would be thousands of map searches).
    struct LabelHit { int plane; uint16_t phys; std::string text; };
    std::vector<LabelHit> labelCache_;
    int labelKey_[5] = {-1, -1, -1, -1, -1};
    int labelAge_ = 0;

    // Inspector caches (refreshed a few times a second).
    struct RangeSummary { uint32_t touched[HeatData::kKinds]{}; uint64_t total[HeatData::kKinds]{}; };
    RangeSummary rangeSummary_;
    int rangeSummaryAge_ = 0;
    int hotKind_ = HeatData::Write;
    std::vector<heatview::Hot> hot_;
    int hotAge_ = 0;
    HeatData::ByteInfo statusInfo_;       // the status line's byte, this frame

    std::string status_;
    double statusUntil_ = 0.0;
    Request request_;
};

}  // namespace pom2

#endif  // POM2_MEMORY_HEATMAP_IMGUI_H
