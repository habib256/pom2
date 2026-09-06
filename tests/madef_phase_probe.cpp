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

// MAD EFFECT beam-phase probe — diagnostic, not a pinned test.
//
// Boots the real French Touch "MAD EFFECT" disk (//e PAL 128K + Mockingboard,
// GPLv3 sources shipped alongside it in disks_5.4/demo/madef/) and dumps the
// horizontal position of every page-flip the demo throws in one frame.
//
// What it answers
// ---------------
// The demo draws each scanline as a PAGE1 window inside a PAGE2 background:
// `LDA $C054` opens, `LDA $C055` closes, and the two delays come from
// `Sources/routine.a` Table1[line] / Table2[line] (which satisfy
// Table1[i] + Table2[i] == 45 for every i — the pair is complementary, so the
// silhouette is symmetric about a vertical axis).
//
// POM2 maps an event's cycle to a column with
// `byteCol = clamp(hpos - 25, 0, 40)` (Apple2Display::frameCycleToPos). If
// POM2's idea of "hpos 25 = first visible byte" is off, the switches do not
// merely shift — everything outside the window CLAMPS to column 0 or 40 and
// the silhouette flattens. This probe prints the measured hpos per line next
// to the table value, so the phase falls out of the data as a constant offset
// instead of being inferred from how one reads the demo's comments.

#include "Apple2Display.h"
#include "CpuClock.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool fileExists(const std::string& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

std::string findFirst(std::initializer_list<const char*> cands)
{
    for (const char* c : cands) if (fileExists(c)) return c;
    return {};
}

// Sources/routine.a Table1 — the "open" delay per scanline (first 24 shown is
// enough to identify a constant offset; the full table is 256 bytes).
const int kTable1[] = {
    32, 31, 30, 29, 28, 28, 27, 26, 25, 24, 24, 23,
    23, 22, 22, 21, 21, 21, 20, 20, 20, 20, 20, 20,
};
const int kTable2[] = {
    13, 14, 15, 16, 17, 17, 18, 19, 20, 21, 21, 22,
    22, 23, 23, 24, 24, 24, 25, 25, 25, 25, 25, 25,
};

}  // namespace

int main(int argc, char** argv)
{
    const std::string rom  = findFirst({"../roms/apple2e.rom", "roms/apple2e.rom"});
    const std::string boot = findFirst({"../roms/disk2.rom", "roms/disk2.rom"});
    const std::string p6   = findFirst({"../roms/diskii_p6.rom", "roms/diskii_p6.rom"});
    // argv[1] overrides the disk so the same harness can be pointed at any
    // beam-raced demo (MAD EFFECT, CRAZY CYCLES II, ...); argv[2] = seconds
    // of emulated boot before sampling.
    const std::string dsk  = (argc > 1) ? std::string(argv[1]) : findFirst({
        "../disks_5.4/demo/madef/MADEF.dsk",
        "disks_5.4/demo/madef/MADEF.dsk"});
    const int bootSecs = (argc > 2) ? std::atoi(argv[2]) : 25;
    if (rom.empty() || boot.empty() || dsk.empty()) {
        std::printf("madef_phase_probe SKIP: missing apple2e.rom / disk2.rom / MADEF.dsk\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    Memory mem;
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    if (!mem.loadAppleIIRom(rom.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n");
        return 1;
    }

    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(boot) || !disk->insertDisk(dsk)) {
        std::fprintf(stderr, "Disk II setup failed\n");
        return 1;
    }
    if (!p6.empty()) disk->loadLssRom(p6);
    mem.slotBus().plug(6, std::move(disk));

    // The demo's whole frame sync is a Mockingboard 6522 T1 interrupt — with
    // no card in slot 4 it never syncs and the effect is meaningless.
    mem.slotBus().plug(4, std::make_unique<MockingboardCard>(4));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    cpu.hardReset();
    mem.slotBus().reset();

    // Boot + title + let the slideshow reach its drawing loop.
    for (int s = 0; s < bootSecs; ++s) cpu.run(1'022'727);
    std::printf("after boot: PC=$%04X text=%d hires=%d page2=%d\n",
                cpu.getProgramCounter(),
                mem.getDisplayState().textMode ? 1 : 0,
                mem.getDisplayState().hiRes ? 1 : 0,
                mem.getDisplayState().page2 ? 1 : 0);

    const VideoTiming& t = pom2VideoTiming(VideoStandard::PAL);
    const uint64_t lineCycles  = static_cast<uint64_t>(t.cyclesPerScanline);
    const uint64_t frameCycles = lineCycles *
                                 static_cast<uint64_t>(t.scanlinesPerFrame);

    // ── HUNT: scan the whole demo for column-0-only instability ─────────
    // The reported symptom is rare and appears only in some parts (VBL text
    // scrollers), so sampling a fixed instant misses it. Walk many frames
    // and flag every frame where the FIRST byte column changed while the
    // rest of those same rows did not — that is the symptom, isolated.
    {
        Apple2Display disp;
        disp.setAuxMemory(mem.auxData());
        std::vector<uint32_t> prev;
        const int kFrames = (argc > 3) ? std::atoi(argv[3]) : 2500;
        int hits = 0;
        std::printf("\nscanning %d frames for column-0-only changes...\n", kFrames);
        for (int f = 0; f < kFrames; ++f) {
            const uint64_t now    = mem.getCycleCounter();
            const uint64_t target = now + frameCycles;
            while (mem.getCycleCounter() < target) cpu.run(50);
            disp.invalidateTextFrameCache();
            disp.render(mem);
            const int W = disp.width(), H = disp.height();
            std::vector<uint32_t> cur(disp.pixels(),
                                      disp.pixels() + static_cast<size_t>(W) * H);
            if (!prev.empty() && prev.size() == cur.size()) {
                const int blk = (W == 560) ? 14 : 7;
                int only0 = 0, alsoRest = 0;
                for (int y = 0; y < H; ++y) {
                    const uint32_t* a = prev.data() + static_cast<size_t>(y) * W;
                    const uint32_t* b = cur.data()  + static_cast<size_t>(y) * W;
                    const bool d0   = std::memcmp(a, b, blk * sizeof(uint32_t)) != 0;
                    const bool dRes = std::memcmp(a + blk, b + blk,
                                          (W - blk) * sizeof(uint32_t)) != 0;
                    if (d0 && !dRes) ++only0; else if (d0) ++alsoRest;
                }
                if (only0 > 0 && ++hits <= 25)
                    std::printf("  frame %5d (t=%5.1fs, %dx%d): %3d rows col-0 ONLY"
                                " (%d rows changed elsewhere too)\n",
                                f, static_cast<double>(f) / 50.0, W, H,
                                only0, alsoRest);
            }
            prev = std::move(cur);
        }
        std::printf("column-0-only frames: %d of %d %s\n", hits, kFrames,
                    hits == 0 ? "(symptom NOT reproduced here)" : "");
    }

    // ── PUBLISHED-PATH measurement (must run FIRST) ─────────────────────
    // Everything below brackets frames by hand with beginVideoEventFrame(),
    // which latches `legacyEventBracket_` for good and BYPASSES the real
    // per-video-frame publication in advanceCycles() — including its
    // carry-over of events that straddle the frame boundary. That is the
    // path the renderer actually consumes, so measure it before switching
    // the object into legacy mode.
    {
        std::printf("\npublished path: boundary-switch census per frame\n");
        for (int f = 0; f < 10; ++f) {
            const uint64_t now    = mem.getCycleCounter();
            const uint64_t target = now + frameCycles;
            while (mem.getCycleCounter() < target) cpu.run(50);
            const auto evs = mem.takeVideoEvents();     // copy of published
            bool seen[200] = {false};
            int  at24 = 0, total = 0;
            for (const auto& e : evs) {
                if (e.kind != Memory::VideoEventKind::Page2) continue;
                ++total;
                if (e.emuCycle % lineCycles != 24) continue;
                ++at24;
                const int line = static_cast<int>((e.emuCycle % frameCycles)
                                                  / lineCycles);
                if (line >= 0 && line < 200) seen[line] = true;
            }
            std::printf("  frame %d: %4d flips, %3d at hpos 24; missing:",
                        f, total, at24);
            int shown = 0;
            for (int l = 0; l < 192; ++l)
                if (!seen[l] && shown++ < 6) std::printf(" %d", l);
            if (!shown) std::printf(" none");
            std::printf("\n");
        }
    }

    // ── Per-frame drift of the drawing loop ─────────────────────────────
    // The demo arms Mockingboard 6522 T1 in continuous mode with a latch of
    // one whole PAL frame, so its 192-line loop must start at the SAME cycle
    // phase every frame. Any per-frame delta here is a T1 period error, and
    // it accumulates until whole scanlines fall outside the visible band.
    {
        std::printf("\nframe | first-flip phase | delta\n");
        uint64_t prevPhase = 0;
        bool     havePrev  = false;
        int      drifting  = 0, samples = 0;
        for (int f = 0; f < 24; ++f) {
            mem.beginVideoEventFrame();
            const uint64_t now    = mem.getCycleCounter();
            const uint64_t target = now + (frameCycles - (now % frameCycles));
            while (mem.getCycleCounter() < target) cpu.run(50);
            auto evs = mem.takeVideoEvents();
            uint64_t first = 0; bool got = false;
            for (const auto& e : evs) {
                if (e.kind != Memory::VideoEventKind::Page2 || e.value) continue;
                if (!got || e.emuCycle < first) { first = e.emuCycle; got = true; }
            }
            if (!got) continue;
            const uint64_t phase = first % frameCycles;
            if (havePrev) {
                const int64_t d = static_cast<int64_t>(phase) -
                                  static_cast<int64_t>(prevPhase);
                std::printf("  %3d | %16llu | %5lld\n", f,
                            static_cast<unsigned long long>(phase),
                            static_cast<long long>(d));
                if (d != 0) ++drifting;
                ++samples;
            }
            prevPhase = phase; havePrev = true;
        }
        std::printf("drift: %d of %d frame steps moved %s\n",
                    drifting, samples,
                    drifting == 0 ? "(STABLE — T1 period exact)"
                                  : "(DRIFTING — T1 period wrong)");
    }

    // Hunt for a frame that actually carries the per-scanline flips.
    std::vector<Memory::VideoEvent> best;
    int bestCount = 0;
    for (int f = 0; f < 400; ++f) {
        mem.beginVideoEventFrame();
        const uint64_t now    = mem.getCycleCounter();
        const uint64_t target = now + (frameCycles - (now % frameCycles));
        while (mem.getCycleCounter() < target) cpu.run(50);
        auto evs = mem.takeVideoEvents();
        int page2 = 0;
        for (const auto& e : evs)
            if (e.kind == Memory::VideoEventKind::Page2) ++page2;
        if (page2 > bestCount) { bestCount = page2; best = evs; }
        if (page2 >= 300) break;      // full 192-line drawing loop
    }
    std::printf("richest frame: %d PAGE2 events\n", bestCount);
    if (bestCount == 0) {
        std::printf("NO page flips seen — the demo never reached its loop.\n");
        return 0;
    }

    // ── Delta analysis: immune to any frame-origin misalignment ─────────
    // The demo's per-line block is exactly 65 cycles, so `LDA $C054` sits at
    // a FIXED phase and only `LDA $C055` moves (Table1[i] + Table2[i] == 45).
    // Between the two accesses the code runs a fixed preamble, then branches
    // `Table1>>1` NOPs into a sled with the LSR carry adding one more cycle:
    //     delta(open→close) = const - Table1[i]
    // So plotting delta + Table1 must give a CONSTANT. If it does, POM2's
    // cycle accounting through the demo's inner loop is exact and only the
    // absolute phase is in question; if it does not, the error is inside the
    // instruction timing itself.
    {
        std::vector<Memory::VideoEvent> ordered;
        for (const auto& e : best)
            if (e.kind == Memory::VideoEventKind::Page2) ordered.push_back(e);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Memory::VideoEvent& a, const Memory::VideoEvent& b) {
                      return a.emuCycle < b.emuCycle;
                  });
        // STRONGEST invariant, independent of the animation: `x000 LDX #$63`
        // is self-modified, so the table INDEX changes per line and per frame
        // — line i does NOT use Table1[i], and no static correlation is
        // possible. But the per-line block is exactly 65 cycles whatever the
        // index, so consecutive OPEN events must be exactly 65 cycles apart.
        // Any deviation is a real CPU/bus timing error inside the loop.
        {
            std::vector<uint64_t> opens;
            for (const auto& e : ordered) if (!e.value) opens.push_back(e.emuCycle);
            int bad = 0, minD = 999999, maxD = -999999;
            for (size_t i = 1; i < opens.size(); ++i) {
                const int d = static_cast<int>(opens[i] - opens[i - 1]);
                if (d > 200) continue;              // frame gap, not a line step
                minD = std::min(minD, d); maxD = std::max(maxD, d);
                if (d != 65) ++bad;
            }
            std::printf("\nline period (consecutive OPEN events): %zu opens, "
                        "min %d max %d, non-65 steps: %d %s\n",
                        opens.size(), minD, maxD, bad,
                        bad == 0 ? "(EXACT — inner-loop timing is right)"
                                 : "(DRIFT — real timing error in the loop)");
        }

        std::printf("\nordered PAGE1→PAGE2 deltas (first 24 pairs, table index "
                    "is animated so these do NOT correlate to Table1):\n");
        std::printf(" pair | delta | Table1 | delta+T1\n");
        int idx = 0, smin = 9999, smax = -9999, pairs = 0;
        for (size_t i = 0; i + 1 < ordered.size() && idx < 24; ++i) {
            if (ordered[i].value || !ordered[i + 1].value) continue;  // want 0→1
            const int delta = static_cast<int>(ordered[i + 1].emuCycle -
                                               ordered[i].emuCycle);
            const int t1 = kTable1[idx];
            std::printf("  %3d | %5d | %6d | %8d\n", idx, delta, t1, delta + t1);
            smin = std::min(smin, delta + t1);
            smax = std::max(smax, delta + t1);
            ++pairs; ++idx; ++i;
        }
        if (pairs)
            std::printf("delta+Table1 over %d pairs: min %d max %d %s\n",
                        pairs, smin, smax,
                        smin == smax ? "(CONSTANT → inner-loop timing exact)"
                                     : "(NOT constant → timing error inside the loop)");
    }

    // ── Solve for the horizontal phase from the data ────────────────────
    // POM2 maps a switch with `byteCol = clamp(hpos - 25, 0, 40)`. If the
    // "25" is wrong, switches fall outside the 40-column window and CLAMP,
    // which is what makes some scanlines start at column 0 instead of a
    // third of the way in. The demo never intends a switch outside the
    // visible window, so the correct phase is the one where NOTHING clamps.
    // Sweep every candidate and count.
    {
        std::vector<int> hs;
        for (const auto& e : best) {
            if (e.kind != Memory::VideoEventKind::Page2) continue;
            hs.push_back(static_cast<int>(e.emuCycle % lineCycles));
        }
        std::printf("\nphase sweep (offset → switches outside the 40-col window):\n");
        int bestOff = -1, bestBad = 1 << 30;
        for (int off = 0; off < 65; ++off) {
            int bad = 0;
            for (int h : hs) {
                const int col = ((h - off) % 65 + 65) % 65;
                if (col >= 40) ++bad;
            }
            if (bad < bestBad) { bestBad = bad; bestOff = off; }
            if (bad == 0 || off % 8 == 0)
                std::printf("  off %2d -> %4d outside%s\n", off, bad,
                            bad == 0 ? "   <== clean" : "");
        }
        std::printf("best offset %d (%d outside, %zu switches total); "
                    "POM2 currently uses 25\n", bestOff, bestBad, hs.size());

        // ── The sweep above is MIS-POSED and kept only as a record ───────
        // It demands that EVERY switch land inside the visible window. But
        // the `LDA $C054` that closes the lit window is legitimately thrown
        // in HBL — that is the standard idiom (a switch in blanking governs
        // the whole upcoming line). Only the `LDA $C055` that OPENS the lit
        // run has to be inside the window: its position IS the silhouette.
        // Sweep those alone.
        std::vector<int> opens;   // $C055 = PAGE2 set = start of the lit run
        for (const auto& e : best) {
            if (e.kind != Memory::VideoEventKind::Page2 || !e.value) continue;
            opens.push_back(static_cast<int>(e.emuCycle % lineCycles));
        }
        std::printf("\nsweep over the %zu lit-run STARTS only:\n", opens.size());
        int bo = -1, bb = 1 << 30;
        for (int off = 0; off < 65; ++off) {
            int bad = 0, lo = 99, hi = -99;
            for (int h : opens) {
                const int col = ((h - off) % 65 + 65) % 65;
                if (col >= 40) ++bad;
                lo = std::min(lo, col); hi = std::max(hi, col);
            }
            if (bad < bb) { bb = bad; bo = off; }
            if (bad == 0)
                std::printf("  off %2d -> clean, columns span [%d..%d]\n",
                            off, lo, hi);
        }
        std::printf("best offset for lit-run starts: %d (%d outside)%s\n",
                    bo, bb, bb == 0 ? "  <== a clean phase EXISTS" : "");
    }

    // ── Clamp-boundary margin ───────────────────────────────────────────
    // `byteCol = clamp(hpos - OFF, 0, 40)` makes hpos OFF the boundary
    // between "governs the whole line (col 0)" and "col 1 onwards". A
    // switch landing within ±1 of it flips column 0 — the first 7-pixel
    // block — between two states as the demo's own ±1 cycle frame jitter
    // moves it across. That is a visible instability confined to exactly
    // one byte column, and it is what picks OFF out of the clean band:
    // choose the value whose boundary is FURTHEST from where switches
    // actually land.
    {
        int hist[65] = {0};
        for (const auto& e : best) {
            if (e.kind != Memory::VideoEventKind::Page2) continue;
            ++hist[e.emuCycle % lineCycles];
        }
        std::printf("\nswitch hpos histogram around the HBL/visible boundary:\n");
        for (int h = 16; h <= 32; ++h)
            if (hist[h]) std::printf("  hpos %2d : %4d\n", h, hist[h]);
        std::printf("candidate boundaries (clean band 21..24) and how many "
                    "switches sit within +/-1 of them:\n");
        for (int off = 21; off <= 25; ++off) {
            const int near = hist[(off + 64) % 65] + hist[off] + hist[(off + 1) % 65];
            std::printf("  OFF %2d -> %4d switch(es) within +/-1 %s\n",
                        off, near,
                        near == 0 ? "  <== no column-0 instability" : "");
        }
    }

    // ── Clamp-boundary margin ───────────────────────────────────────────
    // `byteCol = clamp(hpos - OFF, 0, 40)` makes hpos OFF the boundary
    // between "governs the whole line (column 0)" and "column 1 onwards".
    // A switch landing within +/-1 of it makes column 0 — the first 7-pixel
    // block — flip between two states as the demo's own +/-1 cycle frame
    // jitter walks it across the boundary. That is a visible instability
    // confined to exactly one byte column, and it is the criterion that
    // separates the otherwise-equivalent candidates in the clean band:
    // pick the OFF whose boundary is furthest from where switches land.
    {
        int hist[65] = {0};
        for (const auto& e : best) {
            if (e.kind != Memory::VideoEventKind::Page2) continue;
            ++hist[e.emuCycle % lineCycles];
        }
        std::printf("\nswitch-hpos histogram (non-empty bins):\n");
        for (int h = 0; h < 65; ++h)
            if (hist[h]) std::printf("  hpos %2d : %4d\n", h, hist[h]);
        std::printf("boundary margin — switches within +/-1 of each candidate:\n");
        for (int off = 20; off <= 26; ++off) {
            const int near = hist[(off + 64) % 65] + hist[off % 65]
                           + hist[(off + 1) % 65];
            std::printf("  OFF %2d -> %4d switch(es) at the boundary%s\n",
                        off, near, near == 0 ? "   <== stable column 0" : "");
        }
    }

    // ── Which scanlines carry a switch at each grid position? ───────────
    // CRAZY CYCLES II throws 8 switches per scanline on a regular 5-cycle
    // grid (hpos 24,29,...,59 → columns 0,5,...,35). If one grid position
    // is short by a scanline, that column is missing its switch on that
    // line — and if the missing line MOVES from frame to frame, the column
    // shimmers. Report, per frame, which scanlines lack the hpos-24 switch.
    {
        std::printf("\nper-frame: scanlines missing the boundary switch\n");
        for (int f = 0; f < 8; ++f) {
            mem.beginVideoEventFrame();
            const uint64_t now    = mem.getCycleCounter();
            const uint64_t target = now + (frameCycles - (now % frameCycles));
            while (mem.getCycleCounter() < target) cpu.run(50);
            auto evs = mem.takeVideoEvents();
            bool seen[200] = {false};
            int  atBoundary = 0;
            for (const auto& e : evs) {
                if (e.kind != Memory::VideoEventKind::Page2) continue;
                if (e.emuCycle % lineCycles != 24) continue;
                ++atBoundary;
                const int line = static_cast<int>((e.emuCycle % frameCycles)
                                                  / lineCycles);
                if (line >= 0 && line < 200) seen[line] = true;
            }
            std::printf("  frame %d: %3d at hpos 24; missing lines:", f, atBoundary);
            int shown = 0;
            for (int l = 0; l < 192; ++l)
                if (!seen[l] && shown++ < 8) std::printf(" %d", l);
            if (!shown) std::printf(" none");
            std::printf("\n");
        }
    }

    // Per visible line: first PAGE1 (open) and first PAGE2 (close).
    struct Seg { int open = -1; int close = -1; };
    std::vector<Seg> seg(192);
    for (const auto& e : best) {
        if (e.kind != Memory::VideoEventKind::Page2) continue;
        const int line = static_cast<int>((e.emuCycle % frameCycles) / lineCycles);
        const int hpos = static_cast<int>(e.emuCycle % lineCycles);
        if (line < 0 || line >= 192) continue;
        if (!e.value) { if (seg[line].open  < 0) seg[line].open  = hpos; }
        else          { if (seg[line].close < 0) seg[line].close = hpos; }
    }

    std::printf("\n line |  open hpos  col | close hpos  col |"
                " Table1  Table2 | open-T1  close-T2\n");
    for (int i = 0; i < 24; ++i) {
        const int oc = std::clamp(seg[i].open  - 25, 0, 40);
        const int cc = std::clamp(seg[i].close - 25, 0, 40);
        std::printf("  %3d |   %4d   %4d |   %4d   %4d |  %4d    %4d |  %5d   %5d\n",
                    i, seg[i].open, oc, seg[i].close, cc,
                    kTable1[i], kTable2[i],
                    seg[i].open  < 0 ? -999 : seg[i].open  - kTable1[i],
                    seg[i].close < 0 ? -999 : seg[i].close - kTable2[i]);
    }

    // If the mapping is right the two delta columns are CONSTANT; that
    // constant is POM2's phase error (0 = correct).
    int dmin = 9999, dmax = -9999, n = 0;
    for (int i = 0; i < 24; ++i) {
        if (seg[i].open < 0) continue;
        const int d = seg[i].open - kTable1[i];
        dmin = std::min(dmin, d); dmax = std::max(dmax, d); ++n;
    }
    if (n)
        std::printf("\nopen-vs-Table1 delta over %d lines: min %d max %d %s\n",
                    n, dmin, dmax,
                    dmin == dmax ? "(CONSTANT → that is the phase)"
                                 : "(NOT constant → not a pure phase error)");

    // How many switches land where POM2 clamps them away?
    int clampedLow = 0, clampedHigh = 0;
    for (int i = 0; i < 192; ++i) {
        for (int h : {seg[i].open, seg[i].close}) {
            if (h < 0) continue;
            if (h < 25) ++clampedLow;
            if (h - 25 > 40) ++clampedHigh;
        }
    }
    std::printf("switches clamped to col 0 (hpos < 25): %d ; to col 40: %d\n",
                clampedLow, clampedHigh);
    return 0;
}
