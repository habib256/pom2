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

// Display golden-hash oracle — Phase 0 regression net for the display
// reorganization (CLAUDE.md "layers d'effets" refactor).
//
// Purpose: freeze the EXACT current output of every CPU decode path so a
// mechanical refactor (factoring shared decoders, splitting HiResMode into
// pipeline+effect layers) can be proven byte-for-byte behaviour-preserving.
// This is the oracle the refactor leans on — if a hash changes unexpectedly
// during Phases 1-2, a decoder was altered, not just moved.
//
// What it covers: the cross product of
//   scenes  = {text40, text80, lores, lores+mixed, hgr, hgr+mixed,
//              dhgr, dhgr+mixed, dlgr, dlgr+mixed, text40 flash-phase,
//              text40 page2, hgr page2, hgr 80STORE+page2 (must equal the
//              page-1 hash — Sather 5.10), hgr+AN3 (rev-0 bit-7 mask),
//              hgr 80COL+mixed (upscale path)} on a //e, plus {text40,
//              lores, hgr} on a ][+ (the isIIE()==false branches).
//   + the Chat Mauve card: every scene above is rendered with a FÉLINE
//     (the default variant), and a "cm/" block freezes the per-variant
//     decodes — the latch sub-modes on a Féline and a Video-7, the Féline's
//     AN3-off HGR mono vs the Video-7's F/B, the Eve's table IX-1 modes,
//     TXT16 / TXTGREEN — plus the three mixed-mode boundary rows.
//   modes   = the integer-deterministic colour pipelines:
//              ColorNTSC, ColorCompMedium, ColorComp4Bit, ChatMauveRGB,
//              MonoWhite, MonoGreen, MonoAmber.
//   signal  = the 14.318 MHz composite generator (fillCompositeSignal),
//              hashed once per scene under ColorCompositeOE.
//
// Deliberately EXCLUDED from the golden table: ColorAppleWin's framebuffer.
// Its chromaLut[4][4096] is built from floating-point IIR math at init, so
// the final RGBA8 quantization is host/compiler-FP-dependent and would make
// a baked hash flaky in CI. The AppleWin path is covered two other ways:
//   - its *input* (the composite signal) is golden-hashed here, and
//   - its decode is pinned by applewin_ntsc_smoke_test.
//
// Determinism: every (scene, mode) pair renders into a FRESH Apple2Display
// and a fresh Memory, so frameCounter — derived from Memory's cycleCounter —
// is 0 (flash phase 0) unless the scene parks the clock deliberately
// (sText40Flash), persistence/AppleWin history are clear, and iteration
// order is irrelevant. ColorCompositeOECpu's framebuffer is excluded for
// the same FP reason as ColorAppleWin; its input signal is hashed. All hashed outputs are pure integer
// math (LUTs, palettes, bit ops) → stable across platforms.
//
// Regenerating goldens (after an INTENTIONAL decode change):
//   POM2_GOLDEN_RECORD=1 build/tests/test_display_golden
// then paste the printed table over kGolden[] below.
//
// Looking at a line instead of a hash (the per-dot RGB dump the Chat Mauve
// plan asks for, so a rule is argued from pixels, not from a sentence):
//   POM2_GOLDEN_DUMP=cm/dhgr-mixed-boundary:1 build/tests/test_display_golden
// prints that entry's scanline 1 as one hex RGB word per dot (560 or 280).

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

// ── FNV-1a 64-bit over a raw byte span ───────────────────────────────────
uint64_t fnv1a(const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628257ull;
    }
    return h;
}

// ── Soft-switch addresses (mirror dhgr_render_smoke_test) ─────────────────
constexpr uint16_t IIE_80COL_OFF = 0xC00C;
constexpr uint16_t IIE_80COL_ON  = 0xC00D;
constexpr uint16_t SET_TEXT      = 0xC051;
constexpr uint16_t CLR_TEXT      = 0xC050;
constexpr uint16_t SET_MIXED     = 0xC053;
constexpr uint16_t CLR_MIXED     = 0xC052;
constexpr uint16_t SET_PAGE1     = 0xC054;
constexpr uint16_t SET_HIRES     = 0xC057;
constexpr uint16_t CLR_HIRES     = 0xC056;
constexpr uint16_t DHIRES_ON     = 0xC05E;
constexpr uint16_t DHIRES_OFF    = 0xC05F;

// Deterministic, content-rich pattern for a memory address. Spreads bits
// across the byte so HGR artifact decode, lo-res nibbles and text glyph
// ranges (normal/inverse/flash) all get exercised.
uint8_t patByte(uint32_t addr, uint32_t salt)
{
    uint32_t v = addr * 2654435761u + salt * 40503u;
    v ^= v >> 15;
    return static_cast<uint8_t>(v & 0xFFu);
}

void fillMain(Memory& mem, uint16_t lo, uint16_t hi, uint32_t salt)
{
    for (uint32_t a = lo; a < hi; ++a)
        mem.memWrite(static_cast<uint16_t>(a), patByte(a, salt));
}

void fillAux(Memory& mem, uint16_t lo, uint16_t hi, uint32_t salt)
{
    uint8_t* aux = mem.auxDataMutable();
    for (uint32_t a = lo; a < hi; ++a)
        aux[a] = patByte(a, salt);
}

// Bring every relevant soft switch to a known baseline, then let each scene
// flip only what it needs. NOTE: the $C00x IIe paging switches (80COL) only
// respond to WRITES on a //e (Memory.cpp:892 gates iieHandleSoftSwitch on
// isWrite); the $C05x display switches respond to reads. Drive each via its
// real access type or the soft switch silently no-ops.
void baseline(Memory& mem)
{
    mem.memRead(CLR_TEXT);
    mem.memRead(CLR_MIXED);
    mem.memRead(SET_PAGE1);
    mem.memRead(CLR_HIRES);
    mem.memWrite(IIE_80COL_OFF, 0);
    mem.memRead(DHIRES_OFF);
}

struct Scene {
    const char* name;
    bool        iie;
    void      (*setup)(Memory&);
};

void sText40(Memory& m) {
    baseline(m); m.memRead(SET_TEXT);
    fillMain(m, 0x0400, 0x0800, 1);
}
void sText80(Memory& m) {
    baseline(m); m.memRead(SET_TEXT); m.memWrite(IIE_80COL_ON, 0);
    fillMain(m, 0x0400, 0x0800, 1); fillAux(m, 0x0400, 0x0800, 2);
}
// 40-col TEXT with DHIRES(AN3) on + 80COL off: triggers the Le Chat Mauve
// foreground/background colour-text path (renderTextChatMauveFgBg) under the
// ChatMauveRGB mode (char from main, fg/bg attr from aux). Other modes just
// render plain 40-col mono text — only the chatmauve hash exercises the path.
void sTextColorCM(Memory& m) {
    baseline(m); m.memRead(SET_TEXT); m.memRead(DHIRES_ON);   // 80COL stays off
    fillMain(m, 0x0400, 0x0800, 1); fillAux(m, 0x0400, 0x0800, 2);
}
void sLoRes(Memory& m) {
    baseline(m);                       // graphics + lo-res
    fillMain(m, 0x0400, 0x0800, 3);
}
void sLoResMixed(Memory& m) {
    baseline(m); m.memRead(SET_MIXED);
    fillMain(m, 0x0400, 0x0800, 3);
}
void sHgr(Memory& m) {
    baseline(m); m.memRead(SET_HIRES);
    fillMain(m, 0x2000, 0x4000, 4);
}
void sHgrMixed(Memory& m) {
    baseline(m); m.memRead(SET_HIRES); m.memRead(SET_MIXED);
    fillMain(m, 0x2000, 0x4000, 4);
    fillMain(m, 0x0400, 0x0800, 1);    // bottom 4 text rows
}
void sDhgr(Memory& m) {
    baseline(m); m.memWrite(IIE_80COL_ON, 0); m.memRead(SET_HIRES); m.memRead(DHIRES_ON);
    fillMain(m, 0x2000, 0x4000, 4); fillAux(m, 0x2000, 0x4000, 5);
}
void sDhgrMixed(Memory& m) {
    baseline(m); m.memWrite(IIE_80COL_ON, 0); m.memRead(SET_HIRES);
    m.memRead(DHIRES_ON); m.memRead(SET_MIXED);
    fillMain(m, 0x2000, 0x4000, 4); fillAux(m, 0x2000, 0x4000, 5);
    fillMain(m, 0x0400, 0x0800, 1); fillAux(m, 0x0400, 0x0800, 2);
}
void sDlgr(Memory& m) {
    baseline(m); m.memWrite(IIE_80COL_ON, 0); m.memRead(DHIRES_ON);
    fillMain(m, 0x0400, 0x0800, 6); fillAux(m, 0x0400, 0x0800, 7);
}
void sDlgrMixed(Memory& m) {
    baseline(m); m.memWrite(IIE_80COL_ON, 0); m.memRead(DHIRES_ON);
    m.memRead(SET_MIXED);
    fillMain(m, 0x0400, 0x0800, 6); fillAux(m, 0x0400, 0x0800, 7);
    fillMain(m, 0x0400, 0x0800, 1); fillAux(m, 0x0400, 0x0800, 2);
}
// FLASH-on phase: park the machine 16 emulated frames in (frameCounter is
// derived from cycleCounter since the host-refresh-pacing fix), flipping
// (frameCounter / 16) & 1 to 1 — $40-$7F text bytes render inverted vs the
// phase-0 text40 hash.
void sText40Flash(Memory& m) {
    baseline(m); m.memRead(SET_TEXT);
    fillMain(m, 0x0400, 0x0800, 1);
    m.setCycleCounter(16ull * 65 * 262);
}
// PAGE2 without 80STORE: the scanner must fetch text page 2 ($0800). Both
// pages get different salts so a wrong page choice changes the hash.
void sText40Page2(Memory& m) {
    baseline(m); m.memRead(SET_TEXT); m.memRead(0xC055 /*SET_PAGE2*/);
    fillMain(m, 0x0400, 0x0800, 1); fillMain(m, 0x0800, 0x0C00, 8);
}
void sHgrPage2(Memory& m) {
    baseline(m); m.memRead(SET_HIRES); m.memRead(0xC055);
    fillMain(m, 0x2000, 0x4000, 4); fillMain(m, 0x4000, 0x6000, 9);
}
// 80STORE + PAGE2: Sather table 5.10 — 80STORE hijacks PAGE2 into an aux
// bank-select, so the scanner must STAY on page 1 (same fetch as sHgr, but
// through the videoHgrPage2 gate; page 2 holds decoy data).
void sHgr80StorePage2(Memory& m) {
    baseline(m); m.memRead(SET_HIRES);
    // Fill BEFORE flipping 80STORE: with 80STORE+HIRES on, $2000-$3FFF
    // writes land in AUX page 1 (that's the switch's whole job) and the
    // scanner would then see zeros.
    fillMain(m, 0x2000, 0x4000, 4); fillMain(m, 0x4000, 0x6000, 9);
    m.memWrite(0xC001, 0 /*80STORE on*/); m.memRead(0xC055);
}
// IIe rev-0 DHIRES quirk: AN3 on + 80COL OFF in plain HGR suppresses the
// bit-7 half-dot delay (MAME bit7_mask). Pins the signal path's mask too —
// the 2026-07-12 paintHgr fix (composite disagreed with the LUT modes)
// would have been caught by this scene.
void sHgrAn3(Memory& m) {
    baseline(m); m.memRead(SET_HIRES); m.memRead(DHIRES_ON);  // 80COL off
    fillMain(m, 0x2000, 0x4000, 4);
}
// HGR with per-byte fg/bg colour pairs in aux — the Eve HGR Duochrome
// source data (only decoded when the card's duochrome toggle is on).
void sHgrDuo(Memory& m) {
    baseline(m); m.memRead(SET_HIRES);
    fillMain(m, 0x2000, 0x4000, 4); fillAux(m, 0x2000, 0x4000, 10);
}
// The same data with AN3 on (DHIRES) and 80COL off — the Video-7's F/B HGR
// state (MAME hgr_update rgb fg/bg gate), the Féline's monochrome state.
void sHgrDuoAn3(Memory& m) {
    sHgrDuo(m); m.memRead(DHIRES_ON);
}
// 40-col text with colour attributes in aux, 80COL off, AN3 untouched — the
// Eve's TXT16 state (no AN3 condition; the BASIC recipe never touches it).
void sText40Aux(Memory& m) {
    baseline(m); m.memRead(SET_TEXT);
    fillMain(m, 0x0400, 0x0800, 1); fillAux(m, 0x0400, 0x0800, 2);
}
// Mixed-DHGR boundary rows (chatmauve_dot_rules spells the dots out): row 0
// aligned, row 1 colour→BW cut, row 2 BW→colour repeat of a 1, row 3 repeat
// of a 0; the rest of the page is the usual pattern noise.
void sDhgrMixBoundary(Memory& m) {
    sDhgr(m);
    auto put = [&](int y, int col, uint8_t a, uint8_t mn) {
        const uint16_t addr = static_cast<uint16_t>(0x2000 + 0x400 * (y & 7)
            + 0x80 * ((y >> 3) & 7) + 0x28 * (y >> 6) + col);
        m.auxDataMutable()[addr] = a; m.memWrite(addr, mn);
    };
    for (int y = 0; y < 4; ++y) for (int c = 0; c < 40; ++c) put(y, c, 0, 0);
    put(0, 0, 0x8A, 0xFE); put(0, 1, 0x80, 0x80);
    put(1, 0, 0xFF, 0x06);
    put(2, 0, 0x40, 0xFF); put(2, 1, 0x80, 0x80);
    put(3, 0, 0x3F, 0xFF); put(3, 1, 0x80, 0x80);
}
// 80COL + HIRES + MIXED without DHGR: single-HGR graphics with an 80-col
// text band → the upscaleFrameToFrame80 path (280-wide graphics doubled
// into the 560-wide frame80).
void sHgr80ColMixed(Memory& m) {
    baseline(m); m.memWrite(IIE_80COL_ON, 0); m.memRead(SET_HIRES);
    m.memRead(SET_MIXED);                     // DHIRES stays off
    fillMain(m, 0x2000, 0x4000, 4);
    fillMain(m, 0x0400, 0x0800, 1); fillAux(m, 0x0400, 0x0800, 2);
}

const Scene kScenes[] = {
    { "iie/text40",     true,  sText40    },
    { "iie/text80",     true,  sText80    },
    { "iie/textcolorcm",true,  sTextColorCM},
    { "iie/lores",      true,  sLoRes     },
    { "iie/loresmixed", true,  sLoResMixed},
    { "iie/hgr",        true,  sHgr       },
    { "iie/hgrmixed",   true,  sHgrMixed  },
    { "iie/dhgr",       true,  sDhgr      },
    { "iie/dhgrmixed",  true,  sDhgrMixed },
    { "iie/dlgr",       true,  sDlgr      },
    { "iie/dlgrmixed",  true,  sDlgrMixed },
    { "iie/text40flash",true,  sText40Flash },
    { "iie/text40page2",true,  sText40Page2 },
    { "iie/hgrpage2",   true,  sHgrPage2  },
    { "iie/hgr80store2",true,  sHgr80StorePage2 },
    { "iie/hgran3",     true,  sHgrAn3    },
    { "iie/hgr80colmix",true,  sHgr80ColMixed },
    { "ii+/text40",     false, sText40    },
    { "ii+/lores",      false, sLoRes     },
    { "ii+/hgr",        false, sHgr       },
};

struct ModeEntry {
    Apple2Display::HiResMode m;
    const char* tag;
};
const ModeEntry kModes[] = {
    { Apple2Display::HiResMode::ColorNTSC,       "ntsc"     },
    { Apple2Display::HiResMode::ColorCompMedium, "medium"   },
    { Apple2Display::HiResMode::ColorComp4Bit,   "4bit"     },
    { Apple2Display::HiResMode::ChatMauveRGB,    "chatmauve"},
    { Apple2Display::HiResMode::MonoWhite,       "monowhite"},
    { Apple2Display::HiResMode::MonoGreen,       "monogreen"},
    { Apple2Display::HiResMode::MonoAmber,       "monoamber"},
};

// ── Golden table (host-independent integer paths) ─────────────────────────
// Populated by POM2_GOLDEN_RECORD=1 — see file header.
const std::map<std::string, uint64_t> kGolden = {
    { "iie/text40/ntsc", 0x64b9ef4cc731c75cULL },
    { "iie/text40/medium", 0x64b9ef4cc731c75cULL },
    { "iie/text40/4bit", 0x64b9ef4cc731c75cULL },
    { "iie/text40/chatmauve", 0x64b9ef4cc731c75cULL },
    { "iie/text40/monowhite", 0x64b9ef4cc731c75cULL },
    { "iie/text40/monogreen", 0xbfd914f99c4882dcULL },
    { "iie/text40/monoamber", 0x829455b46a50558cULL },
    { "iie/text40/signal", 0x3643a247208629e3ULL },
    { "iie/text80/ntsc", 0x355e30039c76795cULL },
    { "iie/text80/medium", 0x355e30039c76795cULL },
    { "iie/text80/4bit", 0x355e30039c76795cULL },
    { "iie/text80/chatmauve", 0x355e30039c76795cULL },
    { "iie/text80/monowhite", 0x355e30039c76795cULL },
    { "iie/text80/monogreen", 0xa5f98fd28f95bb5cULL },
    { "iie/text80/monoamber", 0x4f6ab127088a224cULL },
    { "iie/text80/signal", 0x7e66d99b2bb925fcULL },
    { "iie/textcolorcm/ntsc", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/medium", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/4bit", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/chatmauve", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/monowhite", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/monogreen", 0xbfd914f99c4882dcULL },
    { "iie/textcolorcm/monoamber", 0x829455b46a50558cULL },
    { "iie/textcolorcm/signal", 0x3643a247208629e3ULL },
    { "iie/lores/ntsc", 0x85abcadd1bb83d83ULL },
    { "iie/lores/medium", 0x85abcadd1bb83d83ULL },
    { "iie/lores/4bit", 0x85abcadd1bb83d83ULL },
    { "iie/lores/chatmauve", 0xa78a672e2f668d03ULL },
    { "iie/lores/monowhite", 0xe783070789523383ULL },
    { "iie/lores/monogreen", 0x0df1feaae6158183ULL },
    { "iie/lores/monoamber", 0x7e8404c1fe7bd783ULL },
    { "iie/lores/signal", 0x7aab3d2b6d9e0d83ULL },
    { "iie/loresmixed/ntsc", 0xf0c84732083b795cULL },
    { "iie/loresmixed/medium", 0xf0c84732083b795cULL },
    { "iie/loresmixed/4bit", 0xf0c84732083b795cULL },
    { "iie/loresmixed/chatmauve", 0x32445ffef6b2e95cULL },
    { "iie/loresmixed/monowhite", 0x3987f4819b150d5cULL },
    { "iie/loresmixed/monogreen", 0x8f5d83f5d2b557dcULL },
    { "iie/loresmixed/monoamber", 0xa522fdf6befe950cULL },
    { "iie/loresmixed/signal", 0x9e7ae7f3c797af03ULL },
    { "iie/hgr/ntsc", 0xac4e9dd561ff842eULL },
    { "iie/hgr/medium", 0xde163ccdc0549453ULL },
    { "iie/hgr/4bit", 0xdaa49b394af842f7ULL },
    { "iie/hgr/chatmauve", 0x3505e551d6200d83ULL },
    { "iie/hgr/monowhite", 0x75565f7ee4f7025cULL },
    { "iie/hgr/monogreen", 0xb953cafc4196e2dcULL },
    { "iie/hgr/monoamber", 0x16f1c2117085678cULL },
    { "iie/hgr/signal", 0x22b8536ef74be443ULL },
    { "iie/hgrmixed/ntsc", 0x5c24ad5625d0f235ULL },
    { "iie/hgrmixed/medium", 0x29d382b821c3f647ULL },
    { "iie/hgrmixed/4bit", 0xf34682cf003c4db5ULL },
    { "iie/hgrmixed/chatmauve", 0xfa6b53d401f98e43ULL },
    { "iie/hgrmixed/monowhite", 0x6fff8607d7464a83ULL },
    { "iie/hgrmixed/monogreen", 0x197cfff461b1a203ULL },
    { "iie/hgrmixed/monoamber", 0x8a990e9ad0d8b943ULL },
    { "iie/hgrmixed/signal", 0x30803956f9e395a3ULL },
    { "iie/dhgr/ntsc", 0xe15f3fd7367fd274ULL },
    { "iie/dhgr/medium", 0x809d733fa27113acULL },
    { "iie/dhgr/4bit", 0xcb30bb5f5eb7741cULL },
    { "iie/dhgr/chatmauve", 0x9adb0fbcb42d8883ULL },
    { "iie/dhgr/monowhite", 0xaae75d8da1df975cULL },
    { "iie/dhgr/monogreen", 0x3b3375014660335cULL },
    { "iie/dhgr/monoamber", 0xda582fcff14d474cULL },
    { "iie/dhgr/signal", 0x7b1f97d3d0debbbcULL },
    { "iie/dhgrmixed/ntsc", 0xfb11396ccb23e2f8ULL },
    { "iie/dhgrmixed/medium", 0x8e215ae6b9e4ffa7ULL },
    { "iie/dhgrmixed/4bit", 0x2540faad5ce36476ULL },
    { "iie/dhgrmixed/chatmauve", 0x9317aa71822cf903ULL },
    { "iie/dhgrmixed/monowhite", 0x6ca429b254b1395cULL },
    { "iie/dhgrmixed/monogreen", 0xc9b32d871348d45cULL },
    { "iie/dhgrmixed/monoamber", 0xd3062a81f22c8eccULL },
    { "iie/dhgrmixed/signal", 0x3fadd93b236ac7fcULL },
    { "iie/dlgr/ntsc", 0xaeca3b338c835183ULL },
    { "iie/dlgr/medium", 0xaeca3b338c835183ULL },
    { "iie/dlgr/4bit", 0xaeca3b338c835183ULL },
    { "iie/dlgr/chatmauve", 0x7164f3f70211f603ULL },
    { "iie/dlgr/monowhite", 0x98ec347af6668f83ULL },
    { "iie/dlgr/monogreen", 0xc5bdbdee79f9bc83ULL },
    { "iie/dlgr/monoamber", 0xd736a35b32917503ULL },
    { "iie/dlgr/signal", 0xd506811847d2da83ULL },
    { "iie/dlgrmixed/ntsc", 0xdc9faf1f29833983ULL },
    { "iie/dlgrmixed/medium", 0xdc9faf1f29833983ULL },
    { "iie/dlgrmixed/4bit", 0xdc9faf1f29833983ULL },
    { "iie/dlgrmixed/chatmauve", 0x839d81e227e6fd83ULL },
    { "iie/dlgrmixed/monowhite", 0xea841b799de28b83ULL },
    { "iie/dlgrmixed/monogreen", 0x20ffad13365d9b83ULL },
    { "iie/dlgrmixed/monoamber", 0x5ad27eea89507a83ULL },
    { "iie/dlgrmixed/signal", 0x7a4068ba02a9af03ULL },
    { "iie/text40flash/ntsc", 0x8f1b51e9b548b75cULL },
    { "iie/text40flash/medium", 0x8f1b51e9b548b75cULL },
    { "iie/text40flash/4bit", 0x8f1b51e9b548b75cULL },
    { "iie/text40flash/chatmauve", 0x8f1b51e9b548b75cULL },
    { "iie/text40flash/monowhite", 0x8f1b51e9b548b75cULL },
    { "iie/text40flash/monogreen", 0x6106773d2deb145cULL },
    { "iie/text40flash/monoamber", 0x01a63b6be86167ccULL },
    { "iie/text40flash/signal", 0x13b0a5b10da7ed23ULL },
    { "iie/text40page2/ntsc", 0x983e471172289783ULL },
    { "iie/text40page2/medium", 0x983e471172289783ULL },
    { "iie/text40page2/4bit", 0x983e471172289783ULL },
    { "iie/text40page2/chatmauve", 0x983e471172289783ULL },
    { "iie/text40page2/monowhite", 0x983e471172289783ULL },
    { "iie/text40page2/monogreen", 0x414158dfe2f39603ULL },
    { "iie/text40page2/monoamber", 0xc709a03b2875bb43ULL },
    { "iie/text40page2/signal", 0x87892d481d989f83ULL },
    { "iie/hgrpage2/ntsc", 0x7b9fd686ca6ccd10ULL },
    { "iie/hgrpage2/medium", 0x9fdd83e5a011ed17ULL },
    { "iie/hgrpage2/4bit", 0x9a3395c8fcbf8904ULL },
    { "iie/hgrpage2/chatmauve", 0xc24e542fee161683ULL },
    { "iie/hgrpage2/monowhite", 0x1af25acd7e9805dcULL },
    { "iie/hgrpage2/monogreen", 0x94b5cbcf38b3271cULL },
    { "iie/hgrpage2/monoamber", 0x7af22149db6ebd64ULL },
    { "iie/hgrpage2/signal", 0x7e9cd953cd9b639cULL },
    { "iie/hgr80store2/ntsc", 0xac4e9dd561ff842eULL },
    { "iie/hgr80store2/medium", 0xde163ccdc0549453ULL },
    { "iie/hgr80store2/4bit", 0xdaa49b394af842f7ULL },
    { "iie/hgr80store2/chatmauve", 0x3505e551d6200d83ULL },
    { "iie/hgr80store2/monowhite", 0x75565f7ee4f7025cULL },
    { "iie/hgr80store2/monogreen", 0xb953cafc4196e2dcULL },
    { "iie/hgr80store2/monoamber", 0x16f1c2117085678cULL },
    { "iie/hgr80store2/signal", 0x22b8536ef74be443ULL },
    { "iie/hgran3/ntsc", 0x26ac5c2231a77e1cULL },
    { "iie/hgran3/medium", 0x1fd6da1a6675a203ULL },
    { "iie/hgran3/4bit", 0xfe77b96a6343a7aaULL },
    { "iie/hgran3/chatmauve", 0x9cae0da6ceb78b83ULL },
    { "iie/hgran3/monowhite", 0x472c95c035619183ULL },
    { "iie/hgran3/monogreen", 0xf253ee760efaaa03ULL },
    { "iie/hgran3/monoamber", 0x68dcf510380fcc43ULL },
    { "iie/hgran3/signal", 0x61b68cc1d13d53c3ULL },
    { "iie/hgr80colmix/ntsc", 0xa0c25597e94c2f83ULL },
    { "iie/hgr80colmix/medium", 0x07ad6288f2c87d43ULL },
    { "iie/hgr80colmix/4bit", 0xeac5e7898dd75443ULL },
    { "iie/hgr80colmix/chatmauve", 0x1db8957c343a9e43ULL },
    { "iie/hgr80colmix/monowhite", 0x6af296162fa61783ULL },
    { "iie/hgr80colmix/monogreen", 0xeee9d4bcb9a3c583ULL },
    { "iie/hgr80colmix/monoamber", 0x63603998e21e5a83ULL },
    { "iie/hgr80colmix/signal", 0x30803956f9e395a3ULL },
    { "ii+/text40/ntsc", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/medium", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/4bit", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/chatmauve", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/monowhite", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/monogreen", 0xbfd914f99c4882dcULL },
    { "ii+/text40/monoamber", 0x829455b46a50558cULL },
    { "ii+/text40/signal", 0x3643a247208629e3ULL },
    { "ii+/lores/ntsc", 0x85abcadd1bb83d83ULL },
    { "ii+/lores/medium", 0x85abcadd1bb83d83ULL },
    { "ii+/lores/4bit", 0x85abcadd1bb83d83ULL },
    { "ii+/lores/chatmauve", 0xa78a672e2f668d03ULL },
    { "ii+/lores/monowhite", 0xe783070789523383ULL },
    { "ii+/lores/monogreen", 0x0df1feaae6158183ULL },
    { "ii+/lores/monoamber", 0x7e8404c1fe7bd783ULL },
    { "ii+/lores/signal", 0x7aab3d2b6d9e0d83ULL },
    { "ii+/hgr/ntsc", 0xac4e9dd561ff842eULL },
    { "ii+/hgr/medium", 0xde163ccdc0549453ULL },
    { "ii+/hgr/4bit", 0xdaa49b394af842f7ULL },
    { "ii+/hgr/chatmauve", 0x3505e551d6200d83ULL },
    { "ii+/hgr/monowhite", 0x75565f7ee4f7025cULL },
    { "ii+/hgr/monogreen", 0xb953cafc4196e2dcULL },
    { "ii+/hgr/monoamber", 0x16f1c2117085678cULL },
    { "ii+/hgr/signal", 0x22b8536ef74be443ULL },
    { "cm/feline/dhgr-bw560", 0xaae75d8da1df975cULL },
    { "cm/feline/dhgr-mixed", 0xc5a3a9cab441c1ccULL },
    { "cm/feline/dhgr-160to140", 0x9adb0fbcb42d8883ULL },
    { "cm/feline/dhgr-mixed-boundary", 0xb946e0ebb1847dc6ULL },
    { "cm/feline/hgr-an3off-mono", 0x9cae0da6ceb78b83ULL },
    { "cm/feline/textcolor-plain", 0x64b9ef4cc731c75cULL },
    { "cm/video7/dhgr-mixed", 0xc5a3a9cab441c1ccULL },
    { "cm/video7/dhgr-chunky", 0xee2c52b12456d419ULL },
    { "cm/video7/hgr-an3off-fgbg", 0x2ed79ae7c027d4c3ULL },
    { "cm/video7/textcolor", 0xcb80c0ac9bb5a603ULL },
    { "cm/eve/dhgr-col140", 0x9adb0fbcb42d8883ULL },
    { "cm/eve/dhgr-mixed-to-140", 0x9adb0fbcb42d8883ULL },
    { "cm/eve/dhgr-hr3-bw560", 0xaae75d8da1df975cULL },
    { "cm/eve/dhgr-hr1-col280a", 0xc936f5cd0d0c05c3ULL },
    { "cm/eve/dhgr-hr2-col280b", 0x012b65b7014c3643ULL },
    { "cm/eve/dhgr-hr12-blank", 0x9d77ba5943260383ULL },
    { "cm/eve/dhgr-hr123-cp280", 0xe969bf343d0230c3ULL },
    { "cm/eve/hgr-hrbw", 0x9cae0da6ceb78b83ULL },
    { "cm/eve/hgr-spec1", 0x54c471d499232f03ULL },
    { "cm/eve/hgr-spec2", 0x133d67239b8d9f83ULL },
    { "cm/eve/text-txt16", 0x2af32c122a32eb03ULL },
    { "cm/eve/text-txtgreen", 0xbfd914f99c4882dcULL },
    { "cm/eve/text80-txtgreen", 0xa5f98fd28f95bb5cULL },
    { "cm/eve/hgrmixed-txtgreen", 0x290db7b881e8ae43ULL },
};

} // namespace

int main()
{
    const bool record = std::getenv("POM2_GOLDEN_RECORD") != nullptr;
    std::string dumpName;
    int dumpLine = 0;
    if (const char* d = std::getenv("POM2_GOLDEN_DUMP")) {
        dumpName = d;
        const auto colon = dumpName.rfind(':');
        if (colon != std::string::npos) {
            dumpLine = std::atoi(dumpName.c_str() + colon + 1);
            dumpName.erase(colon);
        }
    }
    std::vector<std::pair<std::string, uint64_t>> results;

    for (const auto& sc : kScenes) {
        // pixels() per integer colour mode + the composite signal once.
        for (const auto& mo : kModes) {
            Memory mem;
            mem.setIIEMode(sc.iie);
            sc.setup(mem);

            Apple2Display disp;
            if (sc.iie) disp.setAuxMemory(mem.auxData());
            LeChatMauveCard chat;          // fresh; default COL140, colortext on
            disp.setChatMauveCard(&chat);
            disp.setHiResMode(mo.m);
            disp.render(mem);

            const size_t bytes =
                static_cast<size_t>(disp.width()) * disp.height() * sizeof(uint32_t);
            const uint64_t h = fnv1a(disp.pixels(), bytes);
            results.emplace_back(std::string(sc.name) + "/" + mo.tag, h);
        }

        // Composite signal generator (fillCompositeSignal) — integer 1-bit
        // waveform, hashed under the OE pipeline which always generates it.
        {
            Memory mem;
            mem.setIIEMode(sc.iie);
            sc.setup(mem);
            Apple2Display disp;
            if (sc.iie) disp.setAuxMemory(mem.auxData());
            LeChatMauveCard chat;
            disp.setChatMauveCard(&chat);
            disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
            disp.render(mem);
            assert(disp.signalProduced() && "OE signal must be produced for every scene");
            const size_t sbytes =
                static_cast<size_t>(disp.signalWidth()) * disp.signalHeight();
            const uint64_t h = fnv1a(disp.signal(), sbytes);
            results.emplace_back(std::string(sc.name) + "/signal", h);
        }
    }

    // ── Chat Mauve per-variant decodes, hash-frozen under the ChatMauveRGB
    // pipeline only (the other pipelines ignore the card). Each entry names
    // the variant, the latch value, the Eve switch byte and the scene.
    {
        using Variant = LeChatMauveCard::Variant;
        struct CmVariant {
            const char* name;
            void      (*setup)(Memory&);
            Variant     variant;
            LeChatMauveCard::RenderMode mode;
            uint8_t     eveSwitches;   // Eve only: bit i = EveSwitch i
        };
        constexpr uint8_t HR1 = 1u << LeChatMauveCard::HR1, HR2 = 1u << LeChatMauveCard::HR2,
                          HR3 = 1u << LeChatMauveCard::HR3, TXT16 = 1u << LeChatMauveCard::TXT16,
                          TXTGREEN = 1u << LeChatMauveCard::TXTGREEN;
        const CmVariant kCmVariants[] = {
            // Féline: the three latch modes that exist on it, 160 → COL140.
            { "cm/feline/dhgr-bw560",     sDhgr,     Variant::Feline, LeChatMauveCard::RenderMode::BW560,     0 },
            { "cm/feline/dhgr-mixed",     sDhgr,     Variant::Feline, LeChatMauveCard::RenderMode::Mixed,     0 },
            { "cm/feline/dhgr-160to140",  sDhgr,     Variant::Feline, LeChatMauveCard::RenderMode::Chunky160, 0 },
            { "cm/feline/dhgr-mixed-boundary", sDhgrMixBoundary, Variant::Feline, LeChatMauveCard::RenderMode::Mixed, 0 },
            { "cm/feline/hgr-an3off-mono", sHgrAn3,  Variant::Feline, LeChatMauveCard::RenderMode::COL140,    0 },
            { "cm/feline/textcolor-plain", sTextColorCM, Variant::Feline, LeChatMauveCard::RenderMode::COL140, 0 },
            // Video-7: all four latch modes, F/B HGR when AN3 is off, F/B text.
            { "cm/video7/dhgr-mixed",     sDhgr,     Variant::Video7, LeChatMauveCard::RenderMode::Mixed,     0 },
            { "cm/video7/dhgr-chunky",    sDhgr,     Variant::Video7, LeChatMauveCard::RenderMode::Chunky160, 0 },
            { "cm/video7/hgr-an3off-fgbg", sHgrDuoAn3, Variant::Video7, LeChatMauveCard::RenderMode::COL140,  0 },
            { "cm/video7/textcolor",      sTextColorCM, Variant::Video7, LeChatMauveCard::RenderMode::COL140, 0 },
            // Eve: table IX-1 through the HR switches, TXT16 (hi = bg), TXTGREEN.
            { "cm/eve/dhgr-col140",       sDhgr,     Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    0 },
            { "cm/eve/dhgr-mixed-to-140", sDhgr,     Variant::Eve,    LeChatMauveCard::RenderMode::Mixed,     0 },
            { "cm/eve/dhgr-hr3-bw560",    sDhgr,     Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    HR3 },
            { "cm/eve/dhgr-hr1-col280a",  sDhgr,     Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    HR1 },
            { "cm/eve/dhgr-hr2-col280b",  sDhgr,     Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    HR2 },
            { "cm/eve/dhgr-hr12-blank",   sDhgr,     Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    static_cast<uint8_t>(HR1 | HR2) },
            { "cm/eve/dhgr-hr123-cp280",  sDhgr,     Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    static_cast<uint8_t>(HR1 | HR2 | HR3) },
            { "cm/eve/hgr-hrbw",          sHgr,      Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    static_cast<uint8_t>(HR2 | HR3) },
            { "cm/eve/hgr-spec1",         sHgr,      Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    HR1 },
            { "cm/eve/hgr-spec2",         sHgr,      Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    HR2 },
            { "cm/eve/text-txt16",        sText40Aux, Variant::Eve,   LeChatMauveCard::RenderMode::COL140,    TXT16 },
            { "cm/eve/text-txtgreen",     sText40,   Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    TXTGREEN },
            { "cm/eve/text80-txtgreen",   sText80,   Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    TXTGREEN },
            { "cm/eve/hgrmixed-txtgreen", sHgrMixed, Variant::Eve,    LeChatMauveCard::RenderMode::COL140,    TXTGREEN },
        };
        for (const auto& v : kCmVariants) {
            Memory mem;
            mem.setIIEMode(true);
            v.setup(mem);
            Apple2Display disp;
            disp.setAuxMemory(mem.auxData());
            LeChatMauveCard chat(7, v.variant);
            chat.overrideMode(v.mode);
            for (int i = 0; i < 8; ++i)
                if ((v.eveSwitches >> i) & 1u)
                    chat.setEveSwitch(static_cast<LeChatMauveCard::EveSwitch>(i), true);
            disp.setChatMauveCard(&chat);
            disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
            disp.render(mem);
            const size_t bytes =
                static_cast<size_t>(disp.width()) * disp.height() * sizeof(uint32_t);
            results.emplace_back(v.name, fnv1a(disp.pixels(), bytes));
            if (dumpName == v.name) {
                const int w = disp.width();
                const uint32_t* row = disp.pixels() + static_cast<size_t>(dumpLine) * w;
                std::printf("%s line %d (%d dots, ABGR):\n", v.name, dumpLine, w);
                for (int x = 0; x < w; ++x)
                    std::printf("%08X%s", row[x], (x % 14 == 13) ? "\n" : " ");
            }
        }
    }

    if (record) {
        for (const auto& r : results)
            std::printf("    { \"%s\", 0x%016llxULL },\n",
                        r.first.c_str(),
                        static_cast<unsigned long long>(r.second));
        std::printf("// %zu golden entries\n", results.size());
        return 0;
    }

    int failures = 0;
    for (const auto& r : results) {
        auto it = kGolden.find(r.first);
        if (it == kGolden.end()) {
            std::fprintf(stderr, "MISSING golden for %s (got 0x%016llx)\n",
                         r.first.c_str(),
                         static_cast<unsigned long long>(r.second));
            ++failures;
        } else if (it->second != r.second) {
            std::fprintf(stderr,
                         "MISMATCH %s: expected 0x%016llx got 0x%016llx\n",
                         r.first.c_str(),
                         static_cast<unsigned long long>(it->second),
                         static_cast<unsigned long long>(r.second));
            ++failures;
        }
    }
    if (failures) {
        std::fprintf(stderr, "display golden: %d mismatch(es)\n", failures);
        return 1;
    }
    std::printf("display golden hash: OK (%zu paths pinned)\n", results.size());
    return 0;
}
