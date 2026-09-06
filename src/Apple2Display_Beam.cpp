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

// Apple2Display — beam-raced replay. One concern: turning a frame's
// soft-switch event log into per-band, per-column segment paints.
//
//   usesLegacyPath        which states render 280-wide (frame) vs 560
//                         (frame80) — and why the Chat Mauve pins 560
//   renderInternalSegment one (band × column window) paint, save/restore
//                         column bounding for the 560-wide painters
//   forEachBeamSegment    events → ordered per-line segments (+ the Chat
//                         Mauve latch replayed from the same log)
//   renderBeamRacing      the RGBA consumer (the composite signal path
//                         has its own replay in fillCompositeSignal)
//
// Split out of Apple2Display.cpp 2026-09-02 under the file-size ratchet —
// same move as Apple2Display_ChatMauve.cpp; shared page/band helpers live
// in Apple2Display_Internal.h.

#include "Apple2Display.h"

#include "Apple2Display_Internal.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <vector>

using namespace pom2::a2disp;

bool Apple2Display::usesLegacyPath(Memory& mem, const Memory::DisplayState& state) const
{
    // Mirror the early-return branches at the top of renderInternalBand: a
    // mid-scanline column split is only meaningful on the legacy 280-wide
    // text / hi-res / lo-res path. The 560-wide Chat Mauve and IIe 80-col /
    // DHGR modes paint full width (v1 horizontal-split scope-out).
    const bool cm = chatMauveActive();
    // Under the card the WHOLE frame is 560-domain: the legacy tail of
    // renderInternalBandImpl pixel-doubles its 280 band into frame80 during
    // a replay, so every segment — text, lo-res, single HGR — composes in
    // the SAME buffer. Before this, a DIX-style TEXT⇄HGR beam-raced frame
    // painted half its segments into `frame` and half into `frame80`, and
    // the presented buffer was whichever the LAST segment happened to set —
    // rasters that run clean on the composite pipelines fell apart on the
    // Chat Mauve one (the only mode where graphics are 560 while the
    // machine sits in 40 columns).
    //
    // This SUBSUMES the narrower rule that preceded it (de2860f), which sent
    // only the card's colour-TEXT mode down the 560 path. That test survived
    // BELOW this return as `cm && …`, i.e. as dead code reading like a rule
    // that still applied — removed rather than left to mislead.
    if (cm) return false;

    // The IIe 80-col block returns (non-legacy) for every sub-state except
    // its fall-through: !textMode && !dhgr && !mixedMode (plain 40-col HGR /
    // lo-res, which drops to the legacy path even with 80COL latched on).
    if (mem.isIIE() && state.eightyCol &&
        (state.textMode || state.dhgr || state.mixedMode))
        return false;

    return true;
}

void Apple2Display::renderInternalSegment(Memory& mem, const Memory::DisplayState& state,
                                          int scanY0, int scanY1, int col0, int col1)
{
    if (scanY0 >= scanY1 || col0 >= col1) return;

    // Full width → straight to the column-agnostic band painter.
    if (col0 == 0 && col1 == 40) {
        renderInternalBand(mem, state, scanY0, scanY1);
        return;
    }

    // ── 560-wide IIe / Le Chat Mauve mode, column-bounded by save/restore ──
    // The 560-dot painters (renderText80 / renderDhgr / renderLoResDouble /
    // the Chat Mauve renders / the mixed-HGR upscale) have cross-column
    // context and several sub-paths. Rather than thread [col0,col1) through
    // each, paint the band full width into frame80 but preserve the columns
    // OUTSIDE the [px0,px1) window — snapshot them first, restore after. This
    // composes correctly across several 560-wide segments on one line (each
    // restores what it does not own; final value of a column is whatever its
    // owning segment painted). One byte column = 14 frame80 dots.
    // Assumption: the whole scanline is 560-domain — a split that MIXES a
    // 40-col (280, `frame`) and an 80-col (560, `frame80`) segment is
    // undefined (different target buffers) and stays a v1 scope-out.
    if (!usesLegacyPath(mem, state)) {
        const int px0 = col0 * 14;
        const int px1 = col1 * 14;
        const size_t rowLen = static_cast<size_t>(kWidth80);
        const size_t nRows  = static_cast<size_t>(scanY1 - scanY0);
        std::vector<uint32_t> savedFb(nRows * rowLen);
        std::vector<uint8_t>  savedPe(nRows * rowLen);
        std::memcpy(savedFb.data(), frame80.data() + scanY0 * rowLen,
                    savedFb.size() * sizeof(uint32_t));
        std::memcpy(savedPe.data(), persistenceL80.data() + scanY0 * rowLen,
                    savedPe.size());
        renderInternalBand(mem, state, scanY0, scanY1);   // sets useFrame80_
        for (size_t r = 0; r < nRows; ++r) {
            uint32_t* fbRow = frame80.data()        + (scanY0 + r) * rowLen;
            uint8_t*  peRow = persistenceL80.data() + (scanY0 + r) * rowLen;
            const uint32_t* sFb = savedFb.data() + r * rowLen;
            const uint8_t*  sPe = savedPe.data() + r * rowLen;
            if (px0 > 0) {
                std::memcpy(fbRow, sFb, static_cast<size_t>(px0) * sizeof(uint32_t));
                std::memcpy(peRow, sPe, static_cast<size_t>(px0));
            }
            if (px1 < kWidth80) {
                std::memcpy(fbRow + px1, sFb + px1,
                            static_cast<size_t>(kWidth80 - px1) * sizeof(uint32_t));
                std::memcpy(peRow + px1, sPe + px1,
                            static_cast<size_t>(kWidth80 - px1));
            }
        }
        return;
    }

    // ── Legacy 280-wide path, clipped to byte columns [col0, col1) ──────
    // Same decision tree (and bandRows / bandScanlines clipping) as the
    // legacy tail of renderInternalBand, with the column window threaded in.
    setUseFrame80(false);
    int gLo = 0, gHi = 0, tLo = 0, tHi = 0;
    if (state.textMode) {
        if (bandRows(scanY0, scanY1, 0, 24, &tLo, &tHi))
            renderText(mem, state, tLo, tHi, col0, col1, scanY0, scanY1);
    } else if (state.hiRes) {
        if (bandScanlines(scanY0, scanY1, 0, 192, &gLo, &gHi))
            renderHiRes(mem, state, gLo, gHi, col0, col1);
        if (state.mixedMode && bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
            renderText(mem, state, tLo, tHi, col0, col1, scanY0, scanY1);
    } else {
        if (bandScanlines(scanY0, scanY1, 0, 48 * 4, &gLo, &gHi))
            renderLoRes(mem, state, gLo / 4, (gHi + 3) / 4, col0, col1, gLo, gHi);
        if (state.mixedMode && bandRows(scanY0, scanY1, 20, 24, &tLo, &tHi))
            renderText(mem, state, tLo, tHi, col0, col1, scanY0, scanY1);
    }
}

// A mid-line soft switch's VISIBLE column depends on WHICH switch it is.
// frameCycleToPos maps a switch's emuCycle to `byteCol = hpos - 24`, a
// constant calibrated (madef_phase_probe / vbl_edge_phase) on MAD EFFECT's
// mid-line $C055 — a PAGE2 flip, which is *fetch-side*: it selects the
// address the scanner reads on its NEXT fetch, so its effect shows at the
// following byte. The DHIRES/hi-res switches ($C056/$C057, $C05E/$C05F) are
// *display-side*: they re-interpret the byte being fetched NOW, one
// character cell LEFT of a page flip on the same cycle. Applying the
// page-calibrated -24 to those drew OLDSKOOL FORT ET VERT's HGR-mode raster
// bands one cell RIGHT of the TV-set art they frame — user-confirmed
// 2026-09-02 (persists on genuine NMOS, so it is this mapping and not the
// 65C02 cycle drift; the fix aligns in BOTH the Chat Mauve RGB and the
// OpenEmulator composite paths). So those kinds get one column pulled back.
// Subtracting after the [0,40] clamp equals clamping hpos-25 for every
// in-range value, so a switch thrown in HBL (byteCol already 0 — the DIX
// menu's $C056/$C057) stays at column 0 and does not move.
static int beamColForEvent(const Memory::VideoEvent& e, VideoStandard vstd)
{
    int col = Apple2Display::frameCycleToPos(e.emuCycle, vstd).byteCol;
    switch (e.kind) {
        // ONLY the DHIRES/AN3 colour clock ($C05E/$C05F) lands one column
        // left. OLDSKOOL FORT ET VERT's raster bands are AN3-driven (its
        // colour comes from the DHIRES artifact clock), and that is what the
        // -25 fixes — user-confirmed in both RGB and composite. HiRes
        // ($C056/$C057) does NOT get shifted: it is a graphics-MODE/address
        // switch, and MAD EFFECT flips lo/hi-res mid-line to place its
        // beam-raced picture — shifting HiRes pulled those regions one column
        // too far left (user report 2026-09-02). So HiRes stays with PAGE2 /
        // TextMode on the fetch-side -24.
        case Memory::VideoEventKind::Dhgr:    // derived DHIRES state
        case Memory::VideoEventKind::An3:     // $C05E/$C05F  (AN3/DHIRES)
            col -= 1;                       // display-side: effect one col left
            if (col < 0) col = 0;
            break;
        default:
            // Everything else stays on the page-calibrated -24. Page2 /
            // 80Store are fetch-side (addressing) and MAD EFFECT / DROL / DIX
            // pin them there. TextMode / MixedMode are left at -24 too: no
            // measured title exercises a mid-line text/graphics flip, and
            // $C050/$C051 changes the fetch REGION (text $0400 vs HGR $2000)
            // as well as the interpretation, so its side is genuinely
            // ambiguous — revisit only with a measured case, like this one.
            break;
    }
    return col;
}

void Apple2Display::forEachBeamSegment(
    const Memory::DisplayState& frameStart,
    std::vector<Memory::VideoEvent> events,
    VideoStandard std,
    uint8_t startLatch,
    const std::function<void(const Memory::DisplayState&, int, int, int, int,
                             uint8_t)>& paint)
{
    // ── Double-buffer page flips (DROL-class) vs beam-raced page splits ──
    // A frame whose PAGE2 events all go ONE direction is a buffer flip, not
    // beam racing: the game flips, then spends the next frames redrawing the
    // page it just hid. Replaying that flip at its raster position would
    // paint the band ABOVE it from the now-hidden page — but we read RAM at
    // render time, not at beam time, so that band shows the page MID-REDRAW
    // (half-erased sprites → strong flicker; the real beam saw it pristine).
    // Apply the final page frame-wide instead: the page displayed at frame
    // end is the freshly completed buffer, which is exactly what RAM holds.
    // Real beam-raced effects (DIX MODPAGE: page 1 left, page 2 right of the
    // SAME line) flip BOTH directions within a frame and keep exact replay.
    bool pageOn = false, pageOff = false;
    for (const auto& e : events)
        if (e.kind == Memory::VideoEventKind::Page2)
            (e.value ? pageOn : pageOff) = true;
    Memory::DisplayState start = frameStart;
    if (pageOn != pageOff) {
        start.page2 = pageOn;
        events.erase(std::remove_if(events.begin(), events.end(),
                         [](const Memory::VideoEvent& e) {
                             return e.kind == Memory::VideoEventKind::Page2;
                         }),
                     events.end());
    }

    // Raster order: scanline ascending, then byte column within the line.
    // (Stable so two switches at the same beam position keep push order.)
    std::stable_sort(events.begin(), events.end(),
        [std](const Memory::VideoEvent& a, const Memory::VideoEvent& b) {
            if (a.scanline != b.scanline) return a.scanline < b.scanline;
            return beamColForEvent(a, std) < beamColForEvent(b, std);
        });

    // Per visible scanline, the ordered list of column segments [prevEnd,
    // colEnd) and the display state active across each. Events on a scanline
    // subdivide it at their byteCol; the end-of-line state carries into the
    // next line. A scanline with no events is a single full-width [0, 40)
    // segment — the common case.
    struct Seg { int colEnd; Memory::DisplayState st; uint8_t latch; };
    std::array<std::vector<Seg>, kHeight> perLine;

    // The Chat Mauve mode latch, replayed from the same events: a Dhgr
    // event going ON→OFF is the $C05E→$C05F rising edge of AN3, which
    // clocks the current 80COL level — the state-change view of the same
    // edges the card counted live (LeChatMauveCard::onVideoSoftSwitch).
    Memory::DisplayState cur = start;
    uint8_t latch = startLatch & 0b11;
    size_t ei = 0;
    for (int y = 0; y < kHeight; ++y) {
        std::vector<Seg> segs;
        int prevCol = 0;
        while (ei < events.size() && events[ei].scanline == y) {
            const int col = beamColForEvent(events[ei], std);
            if (col > prevCol) { segs.push_back({col, cur, latch}); prevCol = col; }
            if (events[ei].kind == Memory::VideoEventKind::Dhgr &&
                cur.dhgr && !events[ei].value)
                latch = static_cast<uint8_t>(((latch << 1) | (cur.eightyCol ? 1u : 0u)) & 0b11);
            applyVideoEvent(cur, events[ei].kind, events[ei].value);
            ++ei;
        }
        segs.push_back({40, cur, latch});
        perLine[y] = std::move(segs);
    }

    // Merge vertically-adjacent scanlines with identical segmentation into one
    // band, then paint each band's column segments. A run of event-free lines
    // collapses to a single full-width paint — byte-identical to the
    // pre-horizontal-split behaviour (so existing demos do not regress), and
    // text / lo-res whose character row spans the band still paint whole rows.
    auto sameState = [](const Memory::DisplayState& x, const Memory::DisplayState& z) {
        return x.textMode == z.textMode && x.mixedMode == z.mixedMode &&
               x.page2 == z.page2 && x.hiRes == z.hiRes &&
               x.eightyCol == z.eightyCol && x.altChar == z.altChar &&
               x.dhgr == z.dhgr && x.eightyStore == z.eightyStore;
    };
    auto sameSegs = [&](const std::vector<Seg>& a, const std::vector<Seg>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].colEnd != b[i].colEnd || !sameState(a[i].st, b[i].st) ||
                a[i].latch != b[i].latch)
                return false;
        return true;
    };

    int bandY0 = 0;
    for (int y = 1; y <= kHeight; ++y) {
        if (y == kHeight || !sameSegs(perLine[y], perLine[bandY0])) {
            int col0 = 0;
            for (const auto& s : perLine[bandY0]) {
                paint(s.st, bandY0, y, col0, s.colEnd, s.latch);
                col0 = s.colEnd;
            }
            bandY0 = y;
        }
    }
}

void Apple2Display::renderBeamRacing(Memory& mem,
                                     std::vector<Memory::VideoEvent> events)
{
    setUseFrame80(false);
    // Frame-start latch: the value in force just before this frame's first
    // event, from the card's timestamped ring.
    const uint8_t startLatch = (chatMauve && !events.empty())
        ? static_cast<uint8_t>(chatMauve->latchBefore(events.front().emuCycle))
        : 0b11;
    // Frame-start state + any soft switch poked while the machine is stopped
    // (see Apple2Display::applyIdleSwitchOverride).
    Memory::DisplayState beamStart = mem.getDisplayStateAtFrameStart();
    applyIdleSwitchOverride(beamStart, mem);
    forEachBeamSegment(beamStart, std::move(events),
        mem.videoStandard(), startLatch,
        [&](const Memory::DisplayState& st, int y0, int y1, int col0, int col1,
            uint8_t latch) {
            bandLatch_ = latch;
            renderInternalSegment(mem, st, y0, y1, col0, col1);
            bandLatch_ = -1;
        });
}

