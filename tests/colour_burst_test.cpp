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

// Pins the per-scanline colour burst (ColourBurst.h) and the two monitor
// models that answer it (BurstMonitor.h) on synthetic TEXT edge lists:
//   1. no edge: a full burst (TEXT off) or none (TEXT on) on every line;
//   2. the window is hpos 13-16 and sees TEXT one state after the access
//      that threw it — a TEXT period ending on hpos 14 leaves two states,
//      one ending on 15 one (hpos 16), on 16 none; the R-Y swing through
//      the TCA650 reversal is ~0 for two states and inverted for one;
//   3. a TEXT pulse inside the window cuts the burst in two (two CLRGATE'
//      falls = two PAL V toggles);
//   4. the ][ before Revision 7 gates hpos 17-20, a Revision 0 ][ never;
//   5. the NTSC killer (GSSquared's AppleColor fit): colour holds 131 lines
//      after 100 full-burst lines, and an isolated burst-less line keeps it;
//   6. the PAL decoder (H/2 identification, bistable, killer, saturation
//      recovery, delay line): 2-state bursts: the killer acts; 1-state
//      bursts: R-Y inverted; no burst: the killer acts; TEXT ending on hpos
//      11-13: normal; after a run, colour comes back late, per line, within
//      the bands measured on the photographs.

#include "BurstMonitor.h"
#include "ColourBurst.h"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace pom2;

namespace {

constexpr int kPal = 312, kNtsc = 262;

uint64_t at(int line, int hpos) { return static_cast<uint64_t>(line) * 65 + hpos; }

// TEXT on at `on`, off at `off` (hpos) on every line in [y0, y1).
std::vector<TextEdge> textInHbl(int y0, int y1, int on, int off)
{
    std::vector<TextEdge> e;
    for (int y = y0; y < y1; ++y) {
        e.push_back({at(y, on), true});
        e.push_back({at(y, off), false});
    }
    return e;
}

ColourBurstFrame frameOf(const std::vector<TextEdge>& e, int lines, bool textStart = false,
                         const BurstRules& r = kBurstIIe)
{
    return computeColourBurst(textStart, e.data(), e.size(), lines, r);
}

void testNoEdges()
{
    auto f = frameOf({}, kPal);
    assert(f.allFull && !f.allNone && f.lineCount == kPal);
    assert(f.lines[100].mask == 0xF && f.lines[100].falls == 1);
    f = frameOf({}, kNtsc, true);
    assert(f.allNone && !f.allFull && f.lines[5].states() == 0 && f.lines[5].falls == 0);
    std::printf("  ok: no edge, full burst / none\n");
}

void testWindowAndLag()
{
    // Off on hpos 14 -> the gate sees it from hpos 15: two states.
    auto f = frameOf(textInHbl(10, 11, 2, 14), kPal);
    assert(f.lines[10].mask == 0xC && f.lines[10].states() == 2 && f.lines[10].falls == 1);
    assert(f.lines[9].states() == 4 && f.lines[11].states() == 4);
    assert(!f.allFull && !f.allNone);
    // One cycle later: one state (hpos 16).
    f = frameOf(textInHbl(10, 11, 3, 15), kPal);
    assert(f.lines[10].mask == 0x8 && f.lines[10].falls == 1);
    // Off on hpos 16: none, and no CLRGATE' fall.
    f = frameOf(textInHbl(10, 11, 4, 16), kPal);
    assert(f.lines[10].mask == 0 && f.lines[10].falls == 0);
    // Off on hpos 13: three states; on 12 or before: the burst is whole.
    f = frameOf(textInHbl(10, 11, 1, 13), kPal);
    assert(f.lines[10].mask == 0xE);
    f = frameOf(textInHbl(10, 11, 1, 12), kPal);
    assert(f.lines[10].states() == 4);
    // TEXT on at hpos 13 (seen from 14) truncates the end: state 13 alone.
    f = frameOf({{at(10, 13), true}, {at(10, 40), false}}, kPal);
    assert(f.lines[10].mask == 0x1 && f.lines[10].falls == 1);
    std::printf("  ok: window hpos 13-16, TEXT seen 1 state late\n");
}

void testIdent()
{
    // The R-Y swing through the TCA650 reversal (kVSwitchTauStates).
    auto f = frameOf(textInHbl(10, 13, 2, 14), kPal);   // 2 states
    const float full = f.lines[0].ident, two = f.lines[10].ident;
    f = frameOf(textInHbl(10, 11, 3, 15), kPal);
    const float one = f.lines[10].ident;
    f = frameOf(textInHbl(10, 11, 1, 13), kPal);
    const float three = f.lines[10].ident;
    std::printf("  ident: full %.3f, 3 states %.3f, 2 states %.3f, 1 state %.3f\n",
                full, three, two, one);
    assert(full > 0.4f && full < 0.45f);
    assert(three > 0.15f);
    assert(two > 0.0f && two < 0.03f && "2 states: almost no swing");
    assert(one < -0.05f && "1 state: the swing reads the previous phase");
}

void testPulseInsideWindow()
{
    // Off before, on at 13 (seen 14), off at 14 (seen 15): states 13, 15, 16.
    auto f = frameOf({{at(20, 13), true}, {at(20, 14), false}}, kPal);
    assert(f.lines[20].mask == 0xD && f.lines[20].falls == 2);
    std::printf("  ok: a TEXT pulse inside the window = two gate falls\n");
}

void testOldBoards()
{
    // Pre-Revision 7: hpos 17-20. An off at 15 (seen 16) leaves them all.
    auto f = frameOf(textInHbl(30, 31, 3, 15), kNtsc, false, kBurstIIPreRev7);
    assert(f.lines[30].states() == 4);
    f = frameOf(textInHbl(30, 31, 3, 19), kNtsc, false, kBurstIIPreRev7);   // seen 20
    assert(f.lines[30].mask == 0x8);
    // Revision 0: the burst is there in TEXT mode too.
    f = frameOf({}, kNtsc, true, kBurstIIRev0);
    assert(f.allFull);
    std::printf("  ok: ][ before Revision 7, and Revision 0\n");
}

void testNtscKiller()
{
    NtscColourKiller k;
    LineBurst full{0xF, 1, 0.0f}, none{0, 0, 0.0f};
    for (int i = 0; i < 100; ++i) k.observe(full);
    assert(k.colour);
    int held = 0;
    while (k.observe(none)) ++held;
    // [D] GSSquared @ e569985e AppleColorKiller.hpp: "100 full-burst lines
    // then text: color holds 131 lines".
    assert(held == 131);
    // A burst wholly before the gate (hpos 13-14) is no burst to it.
    NtscColourKiller k2;
    for (int i = 0; i < 300; ++i) k2.observe(LineBurst{0x3, 1, 0.0f});
    assert(!k2.colour);

    // An isolated burst-less line in a frame keeps its colour.
    std::array<LineChroma, 192> out{};
    monitorLineChroma(frameOf(textInHbl(100, 101, 4, 16), kNtsc), VideoStandard::NTSC, out);
    for (int y = 0; y < 192; ++y) assert(out[static_cast<std::size_t>(y)] == LineChroma::Normal);
    // Every visible line burst-less: the killer acts part-way down.
    monitorLineChroma(frameOf(textInHbl(0, 192, 4, 16), kNtsc), VideoStandard::NTSC, out);
    assert(out[0] == LineChroma::Normal && out[191] == LineChroma::Killed);
    int firstKilled = -1;
    for (int y = 0; y < 192 && firstKilled < 0; ++y)
        if (out[static_cast<std::size_t>(y)] == LineChroma::Killed) firstKilled = y;
    std::printf("  ok: NTSC killer (colour off from line %d of a burst-less picture)\n",
                firstKilled);
    // Full-time TEXT: no colour anywhere.
    monitorLineChroma(frameOf({}, kNtsc, true), VideoStandard::NTSC, out);
    assert(out[0] == LineChroma::Killed && out[191] == LineChroma::Killed);
}

// A TEXT period on every line from 16 lines before the picture to its end
// (the blanking lines before the picture carry it too).
std::array<LineChroma, 192> palPicture(int on, int off, int lastLine = 191,
                                       std::array<LineColour, 192>* detail = nullptr)
{
    std::vector<TextEdge> e = textInHbl(0, lastLine + 1, on, off);
    std::vector<TextEdge> lead = textInHbl(kPal - 16, kPal, on, off);
    e.insert(e.begin(), lead.begin(), lead.end());
    std::array<LineChroma, 192> out{};
    monitorLineChroma(frameOf(e, kPal), VideoStandard::PAL, out, detail);
    return out;
}

int firstLine(const std::array<LineColour, 192>& d, int from, bool (*cond)(const LineColour&))
{
    for (int y = from; y < 192; ++y)
        if (cond(d[static_cast<std::size_t>(y)])) return y;
    return 999;
}

// From line 24: the decoder settles within the first ~40 lines of a run that
// starts 16 lines above the picture (the photographs show it settled where
// their picture content starts, line ~20).
void assertAll(const std::array<LineChroma, 192>& out, LineChroma c, const char* what)
{
    for (int y = 24; y < 192; ++y)
        if (out[static_cast<std::size_t>(y)] != c) {
            std::printf("  line %d: %d, wanted %d (%s)\n", y,
                        int(out[static_cast<std::size_t>(y)]), int(c), what);
            assert(false);
        }
}

void testPalDecoder()
{
    // The two photographed settings, and the ones either side.
    assertAll(palPicture(2, 14), LineChroma::Killed,    "2 states: killer");
    assertAll(palPicture(3, 15), LineChroma::VInverted, "1 state: R-Y inverted");
    assertAll(palPicture(4, 16), LineChroma::Killed,    "no burst: killer");
    // The lag, bounded on both sides by photographs ([A] 2026-11-28): the
    // TEXT period ending on hpos 13, 12 and 11 all look like an untouched
    // picture; 14 kills the colour, 15 negates R-Y.
    assertAll(palPicture(1, 13), LineChroma::Normal,    "hpos 13 (3 states): colour");
    assertAll(palPicture(0, 12), LineChroma::Normal,    "hpos 12 (4 states): colour");
    assertAll(palPicture(7, 11), LineChroma::Normal,    "hpos 11 (4 states): colour");

    // The killer is per line, and colour comes back late ([A] photographs):
    // after a run of 2-state lines ending on line 151, full bursts bring
    // colour back ~7 lines later (measured 158.3 +- 1 at half rise); after a
    // run of 1-state lines ending on line 40, the negated colour holds ~3
    // lines (44.3), grey follows, normal colour from ~56.
    std::array<LineColour, 192> d{};
    palPicture(2, 14, 151, &d);
    const int back = firstLine(d, 152, [](const LineColour& c) { return c.gain >= 0.5f; });
    for (int y = 24; y <= 151; ++y) assert(d[static_cast<std::size_t>(y)].gain == 0.0f);
    std::printf("  2-state run to 151: colour back at %d (photo: 158.3)\n", back);
    assert(back >= 155 && back <= 164);
    palPicture(3, 15, 40, &d);
    const int off = firstLine(d, 41, [](const LineColour& c) { return c.gain < 0.5f; });
    const int normal = firstLine(d, 41, [](const LineColour& c) { return c.gain >= 0.5f && c.v > 0.5f; });
    std::printf("  1-state run to 40: negated until %d (photo 44.3), normal from %d (photo 56.2)\n",
                off, normal);
    assert(off >= 41 && off <= 47 && normal >= 50 && normal <= 60);

    // An isolated burst-less line: the Apple misses one V step, the decoder
    // finds it out within a few lines; colour stays on around it.
    std::array<LineChroma, 192> out{};
    monitorLineChroma(frameOf(textInHbl(120, 121, 4, 16), kPal), VideoStandard::PAL, out);
    std::printf("  isolated burst-less line 120 -> lines 119-128:");
    for (int y = 119; y < 129; ++y) std::printf(" %d", int(out[static_cast<std::size_t>(y)]));
    std::printf("\n");
    assert(out[119] == LineChroma::Normal && out[140] == LineChroma::Normal);
    // Text on top, graphics below: the decoder takes a few lines to find
    // colour again (and the bistable), then it is normal.
    monitorLineChroma(frameOf({{at(80, 30), false}}, kPal, true), VideoStandard::PAL, out);
    assert(out[100] == LineChroma::Normal);
    std::printf("  ok: PAL decoder (2 states killed, 1 inverted, 0 killed, 3+ normal)\n");
}

} // namespace

int main()
{
    std::printf("colour_burst:\n");
    testNoEdges();
    testWindowAndLag();
    testIdent();
    testPulseInsideWindow();
    testOldBoards();
    testNtscKiller();
    testPalDecoder();
    std::printf("colour_burst OK\n");
    return 0;
}
