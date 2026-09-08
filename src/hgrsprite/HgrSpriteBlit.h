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

// Pom1 Apple 1 Emulator
// Copyright (C) 2000-2026 Verhille Arnaud
//
// HGR sprite blit — the pure byte-level placement logic behind the GEN2 HGR
// Sprite editor. The Apple II has NO hardware sprites: a "sprite" is just a
// rectangle of HIRES bytes (wBytes × hRows, byte-aligned horizontally) a program
// copies/XORs onto the screen. This module extracts such a rectangle from — and
// stamps it back into — an 8 KB HGR page, honouring the Apple II non-linear row
// interleave (via hgrpaint::hgrByteOffset). No GL/ImGui/emulator dependency, so
// it unit-tests standalone (hgr_sprite_blit_smoke) and ports verbatim to POM2.
//
// The per-pixel drawing model (7 px/byte + shared palette high bit + artifact
// colour) is the already-pinned hgrpaint::HgrPaintModel — this module only moves
// whole bytes around, so it is the one piece the sprite editor adds on top.

#ifndef HGRSPRITE_BLIT_H
#define HGRSPRITE_BLIT_H

#include <cstdint>
#include <functional>

#include "HgrPaintModel.h"   // hgrpaint::HgrColor

namespace hgrsprite {

// Byte columns / rows of an Apple II HIRES page (280 px = 40 bytes × 7, 192 rows).
constexpr int kByteCols = 40;
constexpr int kRows     = 192;

// Extract a wBytes×hRows sprite whose top-left byte column is srcByteCol, top row
// srcRow, from an 8 KB page (page-relative bytes) into row-major `out`
// (hRows*wBytes bytes). Cells whose page byte is out of range read as 0.
void extract(const uint8_t* page, int srcByteCol, int srcRow,
             int wBytes, int hRows, uint8_t* out);

// Stamp a row-major wBytes×hRows sprite into a page at (dstByteCol,dstRow). Each
// in-range cell calls poke(pageRelativeOffset, value) — so a caller can route it
// to pokeByte(base+off) for the live card, or to a local buffer. Cells that fall
// off the page (column ≥ 40 or row ≥ 192) are silently skipped.
void stamp(const uint8_t* sprite, int wBytes, int hRows,
           int dstByteCol, int dstRow,
           const std::function<void(int, uint8_t)>& poke);

// Colour-aware ×2 magnify: build a DOUBLED HGR sprite (2*wBytes bytes × 2*hRows
// rows) from a per-pixel colour grid `cells` (row-major, wBytes*7 × hRows). Each
// source cell becomes a 2×2 destination block; because the block's left column is
// always even and the right always odd, it spans a full NTSC colour clock, so the
// authored colour reproduces RELIABLY regardless of parity (unlike a lone ×1
// pixel): Violet/Blue light the even (left) column, Green/Orange the odd (right),
// White lights both, Black neither; Blue/Orange set the byte's shared palette high
// bit. Palette is per-byte on real HGR, so if two cells of opposite palette groups
// fall in the same destination byte, that byte reads palette-1 (blue/orange wins) —
// the caller keeps like-hued cells byte-aligned to avoid it. `out` must hold
// (2*wBytes)*(2*hRows) bytes. This is the reliable-colour core the ×2 editor uses.
void magnifyColor2x(const hgrpaint::HgrColor* cells, int wBytes, int hRows,
                    uint8_t* out);

// ── DHGR-target export helpers ──────────────────────────────────────────────
// A DHGR-target sprite rasterises one lit shape pixel into one DHGR COLOUR
// pixel (4 dots) of a 16 KB aux+main pair, so a shape `shapePxWide` pixels wide
// occupies shapePxWide*4 dots — but a DHGR line only holds hgrpaint::kDhgrWidth
// (140) colour pixels / 560 dots, and the rasteriser clips there. These two
// keep the ca65 export clipping with it.

// Bytes per PLANE row a ca65 DHGR export emits for that shape. Clips the width
// at kDhgrWidth first, so the result is never above kByteCols — the real length
// of a plane row. (Deriving it from the un-clipped width put it as high as 80,
// which walked the export off the end of the row and, on the last row, off the
// end of the pair.)
int dhgrExportRowBytes(int shapePxWide);

/// Bounded 4-connected flood over RAW BITS of a monochrome sprite page: from
/// (x, y), every pixel whose lit state equals the seed's becomes `set`.
/// Returns the number of pixels stamped. Raw bits, not `hgrpaint::colorAt`:
/// a sprite is a monochrome shape, and colorAt() calls a lit pixel whose
/// horizontal neighbour is dark "Violet"/"Green" — column 0's left neighbour
/// is off the page and column wPx-1's right neighbour is the blank byte past
/// the sprite, so a colour-connected flood never reached either edge column
/// (bug hunt #11).
int floodFillMono(uint8_t* page, int wPx, int hRows, int x, int y, bool set);

// Slice a page-relative 16 KB DHGR pair ([aux 8 KB][main 8 KB], both on the
// HIRES row interleave) into two row-major byte tables of `nPer` bytes per row
// for rows [0,hRows). `auxOut`/`mainOut` must each hold nPer*hRows bytes —
// `nPer` is the caller's stride and is written as given. What the function
// clamps is what it READS: at most one kByteCols plane row per row and at most
// kRows rows, so no argument can take it outside `pair` (columns/rows past
// those bounds are simply left as the caller initialised them).
void extractDhgrPlanes(const uint8_t* pair, int nPer, int hRows,
                       uint8_t* auxOut, uint8_t* mainOut);

} // namespace hgrsprite

#endif // HGRSPRITE_BLIT_H
