// Text wears the phosphor — bug hunt #10.
//
// The crisp text painters hard-coded white, so on the green (P31) and amber
// pipelines every text screen — every boot — was white on a green monitor,
// and a MIXED frame showed a green graphics band above a white text band.
// display_golden_hash pinned the defect: iie/text40/mono{white,green,amber}
// recorded ONE hash. The lit colour of TEXT40, TEXT80 and the MIXED text band
// must be the mode's phosphor, and the band must match the graphics above it.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

uint16_t textRowAddr(int row) { return static_cast<uint16_t>(0x0400 + 0x80 * (row & 7) + 0x28 * (row >> 3)); }

uint32_t firstLit(const Apple2Display& d, int y0, int y1)
{
    const int w = d.width();
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t p = d.pixels()[y * w + x];
            if ((p & 0xFFFFFF) != 0) return p;
        }
    return 0;
}

uint32_t expected(Apple2Display::HiResMode m)
{
    switch (m) {
        case Apple2Display::HiResMode::MonoGreen: return 0xFF33FF33u;
        case Apple2Display::HiResMode::MonoAmber: return 0xFF00B0FFu;
        default:                                  return 0xFFFFFFFFu;
    }
}

}  // namespace

int main()
{
    for (const Apple2Display::HiResMode m : { Apple2Display::HiResMode::MonoWhite,
                                             Apple2Display::HiResMode::MonoGreen,
                                             Apple2Display::HiResMode::MonoAmber }) {
        // TEXT40.
        {
            Memory mem; mem.setIIEMode(true);
            Apple2Display d; d.setAuxMemory(mem.auxData()); d.setHiResMode(m);
            for (int c = 0; c < 40; ++c) mem.writeRamUnchecked(textRowAddr(3) + c, 0xC1 + (c & 0x0F));
            mem.memRead(0xC051); mem.memRead(0xC054); mem.memRead(0xC056);
            for (int i = 0; i < 2; ++i) { mem.setCycleCounter(0); mem.beginVideoEventFrame(); d.render(mem); }
            const uint32_t lit = firstLit(d, 24, 32);
            if (lit != expected(m)) {
                std::printf("FAIL: TEXT40 mode %d lit %08X, want %08X\n", (int)m, lit, expected(m));
                assert(false && "text is not painted in the phosphor's colour");
            }
        }
        // TEXT80.
        {
            Memory mem; mem.setIIEMode(true);
            Apple2Display d; d.setAuxMemory(mem.auxData()); d.setHiResMode(m);
            for (int c = 0; c < 40; ++c) { mem.writeRamUnchecked(textRowAddr(3) + c, 0xC1); mem.auxDataMutable()[textRowAddr(3) + c] = 0xC2; }
            mem.memWrite(0xC00D, 0);
            mem.memRead(0xC051); mem.memRead(0xC054); mem.memRead(0xC056);
            for (int i = 0; i < 2; ++i) { mem.setCycleCounter(0); mem.beginVideoEventFrame(); d.render(mem); }
            const uint32_t lit = firstLit(d, 24, 32);
            assert(lit == expected(m) && "80-column text is not painted in the phosphor's colour");
        }
        // MIXED: the text band must match the graphics band.
        {
            Memory mem; mem.setIIEMode(true);
            Apple2Display d; d.setAuxMemory(mem.auxData()); d.setHiResMode(m);
            for (uint16_t a = 0x2000; a < 0x4000; ++a) mem.writeRamUnchecked(a, 0x7F);
            for (int c = 0; c < 40; ++c) mem.writeRamUnchecked(textRowAddr(21) + c, 0xC1 + (c & 0x0F));
            mem.memRead(0xC050); mem.memRead(0xC053); mem.memRead(0xC057); mem.memRead(0xC054);
            for (int i = 0; i < 2; ++i) { mem.setCycleCounter(0); mem.beginVideoEventFrame(); d.render(mem); }
            const uint32_t gfx = firstLit(d, 0, 160), band = firstLit(d, 168, 176);
            if ((gfx & 0xFFFFFF) != (band & 0xFFFFFF)) {
                std::printf("FAIL: mode %d MIXED graphics %08X but text band %08X\n", (int)m, gfx, band);
                assert(false && "the mixed text band wears a different colour than the graphics");
            }
        }
    }
    std::printf("mono_text_phosphor OK\n");
    return 0;
}
