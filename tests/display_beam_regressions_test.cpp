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
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t IIE_80COL_ON  = 0xC00D;
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
                // hpos 44 → column 20 (PAGE2 maps to hpos - 24 since the
                // 2026-11-08 re-anchoring), mid-line.
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

// ── 4. A page split INSIDE the mixed text band, under the composite demod ─
// The band used to be painted once from the end-of-frame state, so a
// per-line PAGE1 (cycle 5) / PAGE2 (cycle 45, hpos - 25 = column 20, the
// middle of the line) split in rows 160-191 drew
// both halves from one page — under the three composite pipelines only.
// References are frames with the same event SHAPE but one page throughout,
// so all three go through the per-segment painter.
void testPageSplitInsideMixedTextBand()
{
    auto shot = [](uint16_t leftPage, uint16_t rightPage) {
        Memory mem;
        mem.setIIEMode(true);
        for (uint16_t a = 0x0400; a < 0x0800; ++a) mem.memWrite(a, static_cast<uint8_t>(0xC1 + (a & 0x0F)));
        for (uint16_t a = 0x0800; a < 0x0C00; ++a) mem.memWrite(a, static_cast<uint8_t>(0xB0 + (a & 0x07)));
        fillHgr(mem, 0x2000, 0x2A);
        mem.memRead(CLR_TEXT); mem.memRead(SET_MIXED); mem.memRead(SET_HIRES);
        mem.memRead(SET_PAGE1);
        Apple2Display d;
        d.setAuxMemory(mem.auxData());
        d.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
        const uint64_t base = 17030ull * 2;
        mem.setCycleCounter(base);
        mem.beginVideoEventFrame();
        for (int y = 160; y < 192; ++y) {
            mem.setCycleCounter(base + static_cast<uint64_t>(y) * kCyclesPerLine + 5);
            mem.memRead(leftPage);
            // hpos 44 → column 20 (PAGE2 at `hpos - 24` since the 2026-11-08
            // re-anchoring; it was hpos 45 at `hpos - 25`): the band splits
            // at x 280 of 560.
            mem.setCycleCounter(base + static_cast<uint64_t>(y) * kCyclesPerLine + 44);
            mem.memRead(rightPage);
        }
        d.render(mem);
        assert(d.width() == 560);
        return rows(d, 160, 192);
    };
    const auto p1    = shot(SET_PAGE1, SET_PAGE1);
    const auto p2    = shot(SET_PAGE2, SET_PAGE2);
    const auto split = shot(SET_PAGE1, SET_PAGE2);
    assert(p1 != p2 && "the two text pages must differ visibly");
    int dl = 0, dr = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 560; ++x) {
            const size_t k = static_cast<size_t>(y) * 560 + x;
            const uint32_t want = (x < 280) ? p1[k] : p2[k];
            if (split[k] != want) { if (x < 280) ++dl; else ++dr; }
        }
    assert(dl == 0 && "left half of the band must be page 1");
    assert(dr == 0 && "right half of the band must be page 2 — it was painted "
                      "from the end-of-frame page");
    std::puts("  page split inside the mixed text band: OK");
}

// OFV/main.a switches between page-2 lo-res checkerboards and HGR, with
// four moving 20-line bars. Replay several positions into the SAME display:
// every dot must belong to this frame, including where a bar used to sit.
void testMovingCheckerboardBarsOnIIcAdapter()
{
    Memory mem;
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    auto card = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::IIcAdapter);
    auto* rgb = card.get();
    mem.slotBus().plug(7, std::move(card));
    Apple2Display d;
    d.setAuxMemory(mem.auxData());
    d.setChatMauveCard(rgb);
    d.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
    d.setMachineRunning(true);
    fillHgr(mem, 0x4000, 0);
    for (int row = 0; row < 24; ++row)
        for (int col = 0; col < 40; ++col)
            mem.memWrite(static_cast<uint16_t>(textRowAddr(row) + 0x400 + col),
                         ((col - row * 2) & 3) < 2 ? 0xFF : 0);
    mem.memRead(CLR_TEXT); mem.memRead(CLR_MIXED);
    mem.memRead(SET_PAGE2); mem.memRead(0xC05F); mem.memRead(SET_HIRES);
    constexpr uint64_t frameCycles = 65 * 312;
    for (int f = 0; f < 60; ++f) {
        const std::array<int, 4> bars{{(f * 3) % 172, (f * 3 + 43) % 172,
                                     (f * 3 + 86) % 172, (f * 3 + 129) % 172}};
        const uint64_t base = (f + 1) * frameCycles;
        mem.setCycleCounter(base);
        mem.beginVideoEventFrame();
        auto inBar = [&](int y) {
            for (int top : bars) if (y >= top && y < top + 20) return true;
            return false;
        };
        for (int y = 0; y < 192; ++y) {
            mem.setCycleCounter(base + y * 65 + 5);
            mem.memRead(inBar(y) ? CLR_HIRES : SET_HIRES);
            // Source's partial bars show HGR through the TV in the middle.
            // Columns 8 and 34: HIRES maps to hpos - 24 since the 2026-11-08
            // re-anchoring (it was 25 + col at hpos - 25).
            if (inBar(y) && (f & 1)) {
                mem.setCycleCounter(base + y * 65 + 24 + 8);
                mem.memRead(SET_HIRES);
                mem.setCycleCounter(base + y * 65 + 24 + 34);
                mem.memRead(CLR_HIRES);
            }
        }
        d.render(mem);
        assert(d.width() == 560);
        const uint32_t* pixels = d.pixels();
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 560; ++x) {
                const int col = x / 14;
                const bool visible = inBar(y) && (!(f & 1) || col < 8 || col >= 34);
                const uint32_t want = visible && ((col - (y / 8) * 2) & 3) < 2
                    ? 0xFFFFFFFF : 0xFF000000;
                assert(pixels[y * 560 + x] == want && "moving checkerboard left stale raster pixels");
            }
    }
    std::puts("  //c adapter: 60 moving checkerboard frames, every dot current: OK");
}

} // namespace

int main()
{
    std::puts("display_beam_regressions");
    testPageSplitInsideMixedTextBand();
    testMixedClearedInVblRepaintsTextBand();
    testEightyToFortyColumnSplitIsOneFrame();
    testPerLinePageSplitDoesNotGhostAcrossSegments();
    testMovingCheckerboardBarsOnIIcAdapter();
    std::puts("display_beam_regressions: OK");
    return 0;
}
