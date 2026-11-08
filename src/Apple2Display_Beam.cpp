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

// See the member comment in the header. Only the fields that moved since the
// last render with the CPU running are copied — an event-driven change made
// while the machine still ran is already in `liveStateAtRun_` and is left to
// the published frame (and to the beam-race replay) to describe.
bool Apple2Display::applyIdleSwitchOverride(Memory::DisplayState& s,
                                            Memory& mem) const
{
    if (!cpuIdle_) return false;
    bool changed = false;
    const Memory::DisplayState live = mem.getDisplayState();
    const Memory::DisplayState& was = liveStateAtRun_;
    if (live.textMode    != was.textMode) { s.textMode    = live.textMode; changed = true; }
    if (live.mixedMode   != was.mixedMode) { s.mixedMode   = live.mixedMode; changed = true; }
    if (live.page2       != was.page2) { s.page2       = live.page2; changed = true; }
    if (live.hiRes       != was.hiRes) { s.hiRes       = live.hiRes; changed = true; }
    if (live.eightyCol   != was.eightyCol) { s.eightyCol   = live.eightyCol; changed = true; }
    if (live.an3         != was.an3) { s.an3         = live.an3; changed = true; }
    if (live.altChar     != was.altChar) { s.altChar     = live.altChar; changed = true; }
    if (live.dhgr        != was.dhgr) { s.dhgr        = live.dhgr; changed = true; }
    if (live.eightyStore != was.eightyStore) { s.eightyStore = live.eightyStore; changed = true; }
    if (live.an0         != was.an0) { s.an0         = live.an0; changed = true; }
    if (live.an2         != was.an2) { s.an2         = live.an2; changed = true; }
    return changed;
}

void Apple2Display::updateIdleVideoRam(Memory& mem, bool cycleMoved)
{
    constexpr size_t bytesPerBank = 0x0800 + 0x4000;
    if (!cpuIdle_) {
        // Only a frame with beam-time RAM needs an input copy for pause.
        if (!mem.videoWriteLogArmed()) {
            liveVideoRamAtRun_.clear();
            idleVideoRamEdited_ = true;
            return;
        }
        liveVideoRamAtRun_.resize(bytesPerBank * 2);
        idleVideoRamEdited_ = false;
    } else if (cycleMoved || liveVideoRamAtRun_.empty()) {
        idleVideoRamEdited_ = true;
        return;
    } else if (idleVideoRamEdited_) {
        return;
    }
    size_t offset = 0;
    for (const uint8_t* bank : {mem.data(), mem.videoAuxData()}) {
        const auto checkOrCopy = [&](size_t address, size_t count) {
            auto* saved = liveVideoRamAtRun_.data() + offset;
            if (cpuIdle_) {
                if (std::memcmp(saved, bank + address, count) != 0)
                    idleVideoRamEdited_ = true;
            } else {
                std::memcpy(saved, bank + address, count);
            }
            offset += count;
        };
        checkOrCopy(0x0400, 0x0800);
        checkOrCopy(0x2000, 0x4000);
    }
}

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
    if (cm || force560_) return false;
    if (isMonochrome() && !state.textMode) return false;

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
    // A line that MIXES a 40-col (280) and an 80-col (560) segment is pinned
    // to frame80 by renderBeamRacing (force560_): usesLegacyPath() then
    // sends the 280 segments here too, renderInternalBand doubles them
    // across, and the restore keeps their neighbours (horizontal_split_560 § 3).
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
    // The text tints (a VidHD's $C022), over the columns this segment owns —
    // renderInternalBand runs them for a full-width band; this clipped path
    // used to skip them, so a split line lost its colours (hunt 2026-11-01).
    textTintPass(state, scanY0, scanY1, col0 * 7, col1 * 7);
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
        // -25 fixes — user-confirmed in both RGB and composite. HiRes and
        // PAGE2 moved to the same -25 on 2026-11-08 (below).
        case Memory::VideoEventKind::Dhgr:    // derived DHIRES state
        case Memory::VideoEventKind::An3:     // $C05E/$C05F  (AN3/DHIRES)
            col -= 1;                       // display-side: effect one col left
            if (col < 0) col = 0;
            break;
        // PAGE2 / 80STORE: hpos - 25 too (2026-11-08). Two parts of French
        // Touch's DIX — the release built for real PAL machines ("IRL
        // machines PAL", loader.a) — fix it, on the same loader sync chain:
        // (French Touch's timings were tuned on real PAL machines, so they
        // are graded A, CLAUDE.md § grades.)
        // [A] Crazy Cycles II part 1 flips pages 8 times a line, 5 cycles
        //     apart (`LDA (CELLn),Y`), to draw 8 cells x 5 bytes = 40
        //     columns; at -24 every boundary sat one column right (a black
        //     sliver at column 0, a 4-byte last cell). At -25 the grid is
        //     whole (grouik_sync_probe § 7, "cell-grid phase").
        // [A] Mad Effect's silhouette edge (the `LDA $C055` after an HBL
        //     `LDA $C054`, routine.a) sweeps beam cycles 40..64; every edge
        //     lands on screen only for an offset in [25, 40] — at -24 the
        //     cycle-64 extreme fell past column 39.
        // The 2026-07-31 -24 came from a standalone MADEF.dsk of unknown
        // build: its source ships a "REAL PAL APPLE II" and an
        // "APPLEWIN/A2PC" RET_VBL a few cycles apart, and that disk is no
        // longer in the tree to tell which. TEXT / MIXED keep their measured
        // one-cycle lag behind a PAGE2 flip ([A] photos, below): hpos - 24.
        // HiRes ($C056/$C057) with them: like PAGE2 it changes the ADDRESS
        // the scanner fetches, and two French Touch parts the user checked
        // by eye on 2026-11-08 put it there — OLDSKOOL FORT ET VERT, whose
        // only mid-line switches are HiRes at beam cycles 25/26 (its AN3
        // writes all fall in HBL), sat 2 columns off at -24 for both and one
        // with PAGE2 alone moved; Mad Effect 2's LoRes reads at cycles 22-32
        // made its first column wrong from top to bottom. Checked by eye in
        // POM2 by the user on 2026-11-08 (a visual check of the emulator,
        // not a hardware measurement), with PAGE2 + HiRes at -25: OLDSKOOL
        // "parfait"
        // on the unenhanced //e PAL (NMOS — it is 6502-only), Mad Effect
        // "parfait", Mad Effect 2 and Crazy Cycles II "parfaitement calé". (The 2026-09-02
        // report that pulled HiRes back to -24 was made against the old
        // PAGE2 -24 chain and the standalone MADEF.dsk.)
        case Memory::VideoEventKind::HiRes:
        case Memory::VideoEventKind::Page2:
        case Memory::VideoEventKind::EightyStore: {
            const VideoTiming& t = pom2VideoTiming(vstd);
            const int hpos = static_cast<int>(
                e.emuCycle % static_cast<uint64_t>(t.cyclesPerScanline));
            col = std::clamp(hpos - (kVideoAccessColumnOffset + 1), 0, 40);
            break;
        }
        // TEXT ($C050/$C051) and MIXED ($C052/$C053) reach the screen ONE
        // cycle after a PAGE2 flip made on the same cycle, so they land one
        // column RIGHT of the PAGE2 mapping (hpos - 25 since 2026-11-08,
        // i.e. hpos - 24 here; it was hpos - 23 against the old -24).
        // [A] tomcw, AppleWin GH#656 (2019), real //e PAL photos: after
        //     Sather's VBL sync, `LDA $C055/$C054` on line 0 and
        //     `LDA $C050/$C051` on line 7 at the same phase — the GR box is
        //     one column right of the PAGE2 box; AppleWin Video.cpp
        //     "Only 1-cycle delay for VF_TEXT & VF_MIXED mode changes
        //     (GH#656)" @ master 2026-11-01. sosaria7 (appleinpc#9): "There
        //     is no delay except TXTCLR, TXTSET, MIXEDCLR, MIXEDSET".
        // [A] arekkusu42, "VIDMODE LATENCY" photos, MAME PR #15247 (//e,
        //     ][+, IIgs, //c PAL): TEXT and MIXED 3 PH0 cycles vs PAGE2 2
        //     (marked "glitching, approximate") — MAME apple2video.cpp
        //     txt_w/mix_w delayed_update(3) vs scr_w delayed_update(2).
        // [B] Sather, Understanding the Apple IIe p. 8-12: GR+2 (GR delayed
        //     two scanner clocks) selects GRAPHICS vs TEXT patterns in the
        //     video ROM; a page flip only changes the next fetch's address.
        // Computed from hpos (not col+1) so a switch at hpos 25 lands at
        // column 1 and one in HBL (hpos <= 24) at column 0.
        // Audit: docs/audits/parite-apple2video-2026-11-01.md, Rapport 2.
        case Memory::VideoEventKind::TextMode:
        case Memory::VideoEventKind::MixedMode: {
            const VideoTiming& t = pom2VideoTiming(vstd);
            const int hpos = static_cast<int>(
                e.emuCycle % static_cast<uint64_t>(t.cyclesPerScanline));
            col = std::clamp(hpos - kVideoAccessColumnOffset, 0, 40);   // PAGE2 + 1
            break;
        }
        default:
            // Everything else stays on the page-calibrated -24. Page2 /
            // 80Store are fetch-side (addressing) and MAD EFFECT / DROL / DIX
            // pin them there.
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
                             uint8_t)>& paint,
    const std::function<void(int)>* beforeBand)
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
    // With beam-time RAM (`beforeBand`, the video write log) the band above
    // the flip reads the page as the beam saw it, so the exact replay is
    // right and the shortcut — which hides the tear a real machine shows —
    // is not needed.
    Memory::DisplayState start = frameStart;
    if (pageOn != pageOff && !beforeBand) {
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
    // Lines a Cut event starts: a band must begin there even though the
    // display state is unchanged (the RAM under it is not).
    std::array<bool, kHeight> cutAt{};

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
            if (events[ei].kind == Memory::VideoEventKind::Cut) { cutAt[y] = true; ++ei; continue; }
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
               x.dhgr == z.dhgr && x.eightyStore == z.eightyStore &&
               x.an2 == z.an2;
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
        if (y == kHeight || cutAt[y] || !sameSegs(perLine[y], perLine[bandY0])) {
            if (beforeBand) (*beforeBand)(bandY0);
            int col0 = 0;
            for (const auto& s : perLine[bandY0]) {
                paint(s.st, bandY0, y, col0, s.colEnd, s.latch);
                col0 = s.colEnd;
            }
            bandY0 = y;
        }
    }
}

// Beam-time RAM (Memory.h § Video write log). The display paints a frame
// after the machine ran it; when the log describes the published frame,
// video RAM is rewound to the frame start, the bands are cut wherever a
// logged write changes what the beam fetched, and every replay moves RAM to
// its band before painting it (bandHook_). The destructor puts RAM back
// exactly, on every way out of render().
Apple2Display::BeamTimeRam::BeamTimeRam(Apple2Display& d, Memory& m,
                                        std::vector<Memory::VideoEvent>& events)
    : display(d), mem(m), on(false)
{
    // Built AFTER renderCardPicture, never before: a VidHD paints SHR from
    // aux $2000-$9FFF in one pass, and a rewind armed first left lines 0-102
    // (below $6000) at the frame start and the rest live — a shear at ~102.
    // A pause alone keeps beam-time RAM. Host edits and single steps are
    // stamped after the published frame and must show immediately instead.
    if (d.cpuIdle_) {
        auto stoppedState = m.getDisplayStateAtFrameStart();
        if (d.idleVideoRamEdited_ || d.applyIdleSwitchOverride(stoppedState, m)) return;
    }
    // A frame with no soft-switch event cuts bands only for writes into the
    // pages it shows: a write elsewhere (the hidden page) cannot change the
    // picture, and an extra band would only move the paint off the
    // full-frame path for nothing.
    const Memory::DisplayState st = m.getDisplayStateAtFrameStart();
    const bool page2 = st.page2 && !st.eightyStore;
    const std::function<bool(uint16_t, bool)> shown = [st, page2](uint16_t a, bool) {
        const uint16_t text = page2 ? 0x0800 : 0x0400;
        const uint16_t gfx  = page2 ? 0x4000 : 0x2000;
        const bool inText = a >= text && a < text + 0x0400;
        const bool inGfx  = a >= gfx  && a < gfx  + 0x2000;
        if (st.textMode || !st.hiRes) return inText;      // text, lo-res
        return inGfx || (st.mixedMode && inText);
    };
    on = m.videoRewindBegin(events.empty() ? &shown : nullptr);
    if (!on) return;
    mem.videoRewindTo(0);
    Memory& rm = mem;
    display.bandHook_ = [&rm](int line) { rm.videoRewindTo(line); };
    if (events.empty()) {               // full-frame paths band themselves
        display.beamCuts_ = mem.videoRewindCuts();
        return;
    }
    const uint64_t frameStart = mem.videoPublishedFrameStart();
    for (uint16_t cut : mem.videoRewindCuts())
        events.push_back({frameStart + cut * 65ull, cut,
                          Memory::VideoEventKind::Cut, false});
}

Apple2Display::BeamTimeRam::~BeamTimeRam()
{
    if (!on) return;
    mem.videoRewindEnd();
    display.bandHook_ = nullptr;
    display.beamCuts_.clear();
}

// patchMixedTextBand's no-split path. Beam-time RAM cut inside the band (a
// frame with no event keeps its cuts in beamCuts_, not in `events`): paint
// it in those bands, in the 560 domain, as the split path does — painted
// once from line 160 the composite pipelines showed a status-line write a
// frame late below it, where the LUT ones showed the tear (bug hunt
// 2026-10-23). Otherwise moves RAM to line 160 and leaves the band to the
// caller. True when the band is painted.
bool Apple2Display::patchMixedTextByCuts(Memory& mem, const Memory::DisplayState& state)
{
    if (!std::any_of(beamCuts_.begin(), beamCuts_.end(), [](uint16_t c) {
            return c > kMixedTextFirstScanline && c < kHeight; })) {
        if (bandHook_) bandHook_(kMixedTextFirstScanline);
        return false;
    }
    force560_ = true;
    int y0 = kMixedTextFirstScanline;
    auto paintBand = [&](int y1) {
        if (y1 <= y0) return;
        if (bandHook_) bandHook_(y0);
        renderInternalSegment(mem, state, y0, y1, 0, 40);
        y0 = y1;
    };
    for (const uint16_t c : beamCuts_)
        if (c > kMixedTextFirstScanline && c < kHeight) paintBand(c);
    paintBand(kHeight);
    force560_ = false;
    setUseFrame80(true);
    return true;
}

void Apple2Display::renderInternalBeamTime(Memory& mem, const Memory::DisplayState& state)
{
    // renderInternal, in bands: RAM moves to each band's fetch time first.
    int y0 = 0;
    for (const uint16_t cut : beamCuts_) {
        if (bandHook_) bandHook_(y0);
        renderInternalBand(mem, state, y0, cut);
        y0 = cut;
    }
    if (bandHook_) bandHook_(y0);
    renderInternalBand(mem, state, y0, kHeight);
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
    // Which buffer does this frame live in? A replay whose segments are not
    // all 280-wide or all 560-wide painted half the picture into `frame` and
    // half into `frame80`, and pixels() followed whichever the LAST segment
    // set — the other half showed the PREVIOUS frame. Probe the segmentation
    // first and, when it straddles, pin the frame to the 560 domain: exactly
    // what the Chat Mauve already does, for the same reason.
    bool anyLegacy = false, any560 = false;
    forEachBeamSegment(beamStart, events, mem.videoStandard(), startLatch,
        [&](const Memory::DisplayState& st, int, int, int, int, uint8_t) {
            (usesLegacyPath(mem, st) ? anyLegacy : any560) = true;
        }, bandHook());
    force560_ = anyLegacy && any560;
    forEachBeamSegment(beamStart, std::move(events),
        mem.videoStandard(), startLatch,
        [&](const Memory::DisplayState& st, int y0, int y1, int col0, int col1,
            uint8_t latch) {
            bandLatch_ = latch;
            renderInternalSegment(mem, st, y0, y1, col0, col1);
            bandLatch_ = -1;
        }, bandHook());
    force560_ = false;
}

