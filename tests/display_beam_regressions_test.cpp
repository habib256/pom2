// Three beam-raced display regressions from bug hunt #5 (2026-09-08), each
// found by a probe against a mode combination the static-frame tests never
// reach. All three are "the picture shows the PREVIOUS frame" failures:
//
//   1. Composite OE: a guest that clears MIXED during VBL (the tear-free
//      idiom) left rows 160-191 written by nobody — render() folded the
//      VBL-stamped event into the frame state, so `mixedGfx` was true but
//      the frame no longer ENDED in mixed graphics, and patchMixedTextBand
//      painted nothing while the demod stopped at row 160.
//   2. A beam-raced 80-col ⇄ 40-col switch painted half the frame into
//      `frame` and half into `frame80`; pixels() followed the last segment.
//   3. The 280-wide phosphor history was shared by every beam segment on a
//      line, so a MonoAmber per-line page split ghosted the left segment's
//      dots through the right one.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t IIE_80COL_ON  = 0xC00D;
constexpr uint16_t IIE_80COL_OFF = 0xC00C;
constexpr uint16_t CLR_TEXT      = 0xC050;
constexpr uint16_t SET_TEXT      = 0xC051;
constexpr uint16_t CLR_MIXED     = 0xC052;
constexpr uint16_t SET_MIXED     = 0xC053;
constexpr uint16_t SET_PAGE1     = 0xC054;
constexpr uint16_t SET_PAGE2     = 0xC055;
constexpr uint16_t CLR_HIRES     = 0xC056;
constexpr uint16_t SET_HIRES     = 0xC057;
constexpr int      kCyclesPerLine = 65;

uint16_t textRowAddr(int row)
{
    return static_cast<uint16_t>(0x0400 + 0x80 * (row & 7) + 0x28 * (row >> 3));
}

void fillText(Memory& mem, uint8_t seed)
{
    for (int row = 0; row < 24; ++row)
        for (int col = 0; col < 40; ++col)
            mem.memWrite(static_cast<uint16_t>(textRowAddr(row) + col),
                         static_cast<uint8_t>(0xC1 + ((row * 5 + col + seed) & 0x1F)));
}

void fillHgr(Memory& mem, uint16_t base, uint8_t v)
{
    for (uint32_t a = base; a < base + 0x2000u; ++a)
        mem.memWrite(static_cast<uint16_t>(a), v);
}

std::vector<uint32_t> rows(const Apple2Display& d, int y0, int y1)
{
    const int w = d.width();
    const uint32_t* p = d.pixels();
    return std::vector<uint32_t>(p + static_cast<size_t>(y0) * w,
                                 p + static_cast<size_t>(y1) * w);
}

int lum(uint32_t p)
{
    return static_cast<int>(p & 0xFF) + static_cast<int>((p >> 8) & 0xFF) +
           static_cast<int>((p >> 16) & 0xFF);
}

// ── 1. Leaving mixed mode during VBL must repaint the text band ──────────
void testMixedClearedInVblRepaintsTextBand()
{
    Memory mem;
    mem.setIIEMode(true);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);

    fillHgr(mem, 0x2000, 0x55);
    fillText(mem, 0);
    mem.memRead(CLR_TEXT);
    mem.memRead(SET_MIXED);
    mem.memRead(SET_HIRES);
    mem.memRead(SET_PAGE1);
    // Two quiet mixed frames so the text band and the demod are settled.
    for (int i = 0; i < 2; ++i) {
        mem.setCycleCounter(0);
        mem.beginVideoEventFrame();
        disp.render(mem);
    }
    const std::vector<uint32_t> bandBefore = rows(disp, 160, 192);

    // Frame 3: the guest rewrites the text band and clears MIXED in VBL.
    fillText(mem, 9);
    mem.setCycleCounter(0);
    mem.beginVideoEventFrame();
    mem.setCycleCounter(static_cast<uint64_t>(200) * kCyclesPerLine);   // VBL
    mem.memRead(CLR_MIXED);
    disp.render(mem);
    // The event happened after the beam finished this picture: THIS frame is
    // still mixed, the next one is full-screen graphics.
    assert(disp.lastRenderState().mixedMode &&
           "a VBL-stamped switch belongs to the next frame");
    const std::vector<uint32_t> bandAfter = rows(disp, 160, 192);
    size_t changed = 0;
    for (size_t i = 0; i < bandAfter.size(); ++i)
        if (bandAfter[i] != bandBefore[i]) ++changed;
    assert(changed > 0 && "rows 160-191 kept the previous frame's text band");
    std::puts("  mixed cleared in VBL repaints the text band: OK");
}

// ── 2. An 80-col ⇄ 40-col beam split lives in ONE buffer ─────────────────
void testEightyToFortyColumnSplitIsOneFrame()
{
    Memory mem;
    mem.setIIEMode(true);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    disp.setHiResMode(Apple2Display::HiResMode::MonoWhite);

    fillText(mem, 0);
    for (uint16_t a = 0x0400; a < 0x0800; ++a) mem.auxDataMutable()[a] = 0xA0 + (a & 0x1F);
    mem.memWrite(IIE_80COL_ON, 0);
    mem.memRead(SET_TEXT);
    mem.memRead(SET_PAGE1);
    mem.memRead(CLR_HIRES);

    // Reference: a full 80-column TEXT frame.
    Apple2Display ref;
    ref.setAuxMemory(mem.auxData());
    ref.setHiResMode(Apple2Display::HiResMode::MonoWhite);
    mem.setCycleCounter(0);
    mem.beginVideoEventFrame();
    ref.render(mem);
    assert(ref.width() == 560);
    const std::vector<uint32_t> textTop = rows(ref, 0, 8);

    // A first frame of something else entirely, so "stale" is detectable.
    mem.memRead(CLR_TEXT);                 // 40-col lo-res (80COL still on)
    mem.setCycleCounter(0);
    mem.beginVideoEventFrame();
    disp.render(mem);
    mem.memRead(SET_TEXT);

    // The beam-raced frame: 80-col TEXT at the top, $C050 at scanline 8.
    mem.setCycleCounter(0);
    mem.beginVideoEventFrame();
    mem.setCycleCounter(static_cast<uint64_t>(8) * kCyclesPerLine);
    mem.memRead(CLR_TEXT);
    disp.render(mem);
    assert(disp.width() == 560 &&
           "a frame with an 80-col segment must be published at 560");
    const std::vector<uint32_t> top = rows(disp, 0, 8);
    assert(top == textTop && "rows 0-7 must be this frame's 80-col text, not "
                             "the previous frame's picture");
    std::puts("  80-col/40-col beam split is one frame: OK");
}

// ── 3. Per-segment phosphor history on the 280-wide path ─────────────────
// The same frame three ways: page 1 only, page 2 only, and a per-line
// PAGE1 (cycle 5) / PAGE2 (cycle 44) split — the DIX raster shape. Each
// half of the split must equal its whole-page reference; the shared
// phosphor history let the left segment's page-1 dots ghost into the right.
void testPerLinePageSplitDoesNotGhostAcrossSegments()
{
    auto populate = [](Memory& mem) {
        uint32_t s = 0x2468;
        auto rnd = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
        for (uint32_t a = 0x2000; a < 0x6000; ++a) mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(rnd()));
        for (uint32_t a = 0x0400; a < 0x0C00; ++a) mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(rnd()));
    };
    auto setup = [](Memory& m) {
        m.memRead(CLR_TEXT); m.memRead(SET_MIXED); m.memRead(SET_HIRES);
        m.memWrite(IIE_80COL_ON, 0);
        m.memWrite(0xC000, 0);                 // 80STORE off
        m.memRead(0xC05F);                     // DHIRES off
    };
    const uint64_t base = 17030ull * 2;
    auto shot = [&](uint16_t page, bool split, int& w) {
        Memory mem;
        mem.setIIEMode(true);
        populate(mem);
        setup(mem);
        mem.memRead(page);
        Apple2Display d;
        d.setAuxMemory(mem.auxData());
        d.setHiResMode(Apple2Display::HiResMode::MonoGreen);
        mem.setCycleCounter(base);
        mem.beginVideoEventFrame();
        if (split)
            for (int y = 0; y < 192; ++y) {
                mem.setCycleCounter(base + static_cast<uint64_t>(y) * kCyclesPerLine + 5);
                mem.memRead(SET_PAGE1);
                mem.setCycleCounter(base + static_cast<uint64_t>(y) * kCyclesPerLine + 44);
                mem.memRead(SET_PAGE2);
            }
        d.render(mem);
        w = d.width();
        const uint32_t* p = d.pixels();
        return std::vector<uint32_t>(p, p + static_cast<size_t>(w) * 192);
    };
    int w1 = 0, w2 = 0, wb = 0;
    const auto f1 = shot(SET_PAGE1, false, w1);
    const auto f2 = shot(SET_PAGE2, false, w2);
    const auto fb = shot(SET_PAGE1, true,  wb);
    assert(w1 == 560 && w2 == 560 && wb == 560);
    int dl = 0, dr = 0;
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < wb; ++x) {
            const size_t k = static_cast<size_t>(y) * wb + x;
            const uint32_t want = (x < 280) ? f1[k] : f2[k];
            if (fb[k] != want) { if (x < 280) ++dl; else ++dr; }
        }
    assert(dl == 0 && "left half must be page 1");
    assert(dr == 0 && "right half must be page 2 — the left segment's dots "
                      "ghosted through the shared 280-wide phosphor history");
    std::puts("  per-line page split keeps its phosphor per segment: OK");
}

} // namespace

int main()
{
    std::puts("display_beam_regressions");
    testMixedClearedInVblRepaintsTextBand();
    testEightyToFortyColumnSplitIsOneFrame();
    testPerLinePageSplitDoesNotGhostAcrossSegments();
    std::puts("display_beam_regressions: OK");
    return 0;
}
