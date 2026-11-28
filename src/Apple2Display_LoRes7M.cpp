// Apple2Display_LoRes7M.cpp — the //e's "abnormal undelayed 7M LORES mode".
//
// [B] Sather, *Understanding the Apple IIe*, p. 8-22, Table 8.8 note 3:
// "An abnormal undelayed 7M LORES mode can be selected by resetting 80COL
// and bringing FRCTXT' low. VID7M is identical to 7M in this mode." And
// below it: "7M LORES processing is basically the processing of LORES
// patterns at HIRES40 speed. It results in fragmented blocks for all
// patterns except 0000, 0101, 1010, and 1111 which result in blocks the same
// colors as the undelayed HIRES40 colors." To select it: LORES40 GRAPHICS
// with AN3 low (80COL off). It is the lo-res twin of the undelayed HIRES40
// POM2 already draws (AN3 low masks HGR's half-dot delay — renderHiRes'
// bit7Mask, and its column placement measured by S. Champailler,
// Apple2Display_Beam.cpp beamColForEvent).
//
// [D] AppleWin @ 98b72658, NTSC.cpp `updateScreenDoubleLores40`
// ("wsUpdateVideo7MLores", chosen on DHIRES without 80COL): the nibble
// repeated, shifted by 2 on odd columns, 7 bits pixel-doubled as HGR — the
// same bytes as sevenMLoresByte. [D] Accurapple @ 8d1783f (S. Champailler)
// derives the mode from the timing HAL's VID7M equations, which he measured
// on a real PAL //e (accurashader/notes.lyx, "LORES7M"); its multi_mode.s
// TEST 8 switches LORES40 <-> LORES7M mid-frame, but no capture of that
// test was found — that capture is what would raise this to [A].
//
// The picture is drawn as what the shift register sees: one undelayed HGR
// byte per column (sevenMLoresByte, Apple2VideoDecode.h), through the HGR
// painters, so every colour pipeline gives it the HIRES40 artifact colours
// Sather describes. A Le Chat Mauve keeps its own lo-res decode [F]: the
// card samples the stream, and what it makes of a 7 MHz lo-res is unknown.
//
// Not modelled: Sather's other way in — holding CONTROL-RESET over a LORES40
// display, RESET' low forcing AN3 low — POM2 does not hold RESET down.

#include "Apple2Display.h"
#include "Apple2Display_Internal.h"
#include "Apple2VideoDecode.h"
#include "Memory.h"

#include <algorithm>

using namespace pom2::a2v;
using namespace pom2::a2disp;   // videoTextPage2

bool Apple2Display::sevenMLores(const Memory& mem, const Memory::DisplayState& state)
{
    // state.dhgr = AN3 low (DHIRES on). On a ][ / ][+ the same switch is a
    // bare annunciator: no 80COL, no FRCTXT', no 7M LORES.
    // [F] isIIE() includes the //c and //c+: Sather describes the //e's
    // timing HAL; the //c is assumed to keep its VID7M equations. Settled
    // by: the same 7M LORES screen photographed on a real //c.
    return mem.isIIE() && !state.textMode && !state.hiRes && state.dhgr &&
           !state.eightyCol;
}

void Apple2Display::sevenMLoresStream(const Memory& mem,
                                      const Memory::DisplayState& state, int y,
                                      uint8_t (&stream)[560]) const
{
    uint8_t row[40];
    buildSevenMLoresRow(mem.data(), textRowAddress(y / 8, videoTextPage2(state)), y, row);
    buildBitStream(row, 0, stream, 0x7F);   // undelayed: no half-dot shift
}

void Apple2Display::renderSevenMLores(Memory& mem, const Memory::DisplayState& state,
                                      int firstRow, int lastRow, int col0, int col1,
                                      int clipY0, int clipY1)
{
    const int y0 = std::max(firstRow * 4, clipY0);
    const int y1 = std::min(lastRow * 4, std::min(clipY1, kHeight));
    if (y0 >= y1) return;
    for (int y = y0; y < y1; ++y) {
        uint8_t row[40];
        buildSevenMLoresRow(mem.data(), textRowAddress(y / 8, videoTextPage2(state)),
                            y, row);
        std::copy(row, row + 40, sevenMRows_.begin() + static_cast<size_t>(y) * 40);
    }
    // renderHiRes with its rows overridden: its bit7Mask is 0x7F already
    // (state.dhgr), the undelayed decode; the row cache keys on the words.
    hgrRowsOverride_ = sevenMRows_.data();
    renderHiRes(mem, state, y0, y1, col0, col1);
    hgrRowsOverride_ = nullptr;
}
