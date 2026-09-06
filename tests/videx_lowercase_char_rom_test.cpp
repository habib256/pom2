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

// The Videx LOWER CASE CHIP (1980) — a character generator that replaced the
// one on a II/II+ motherboard so the machine could display lowercase at all.
//
// POM2 used to decide "has lowercase" from the ROM's SIZE: 2 KB meant no
// lowercase, and a-z was folded to A-Z at render time. That is right for the
// stock generator and exactly wrong for this chip, which is also 2 KB. This
// pins the two things that had to change:
//
//   1. The dump's convention. AppleWin's 2 KB `Apple2_Video.rom` uses bit 7
//      of each byte as the inverse/flash range marker; the Videx dump never
//      sets bit 7 and needs the range split by offset instead. Getting this
//      wrong inverts the whole $00-$7F range and is invisible in normal text.
//   2. The lowercase test itself, which comes from the Videx manual (§
//      Discussion of character display): on a stock generator "Characters
//      80 - BF are identical to characters C0 - FF". A chip that adds
//      lowercase breaks that equality.
//
// ROM-gated on both files; SKIPs cleanly without either.

#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

namespace {

bool exists(const char* p) { std::ifstream f(p, std::ios::binary); return f.good(); }

std::string find(const char* leaf)
{
    const std::string bases[] = { "", "../", "../../" };
    for (const auto& b : bases) {
        const std::string p = b + "roms/" + leaf;
        if (exists(p.c_str())) return p;
    }
    return {};
}

/// The 8 rows of one glyph, as the renderer sees them after normalisation:
/// bit 0 = leftmost, 1 = lit.
void glyph(const Memory& mem, int idx, uint8_t out[8])
{
    const auto& rom = mem.charRom();
    for (int r = 0; r < 8; ++r)
        out[r] = static_cast<uint8_t>(rom[static_cast<std::size_t>(idx) * 8 + r] & 0x7F);
}

bool sameGlyph(const uint8_t a[8], const uint8_t b[8])
{
    for (int i = 0; i < 8; ++i) if (a[i] != b[i]) return false;
    return true;
}

} // namespace

int main()
{
    const std::string stock = find("apple2_char.rom");
    const std::string videx = find("Videx Lower Case Chip ROM.bin");
    if (stock.empty() || videx.empty()) {
        std::printf("videx_lowercase_char_rom: SKIP (needs roms/apple2_char.rom "
                    "and \"roms/Videx Lower Case Chip ROM.bin\")\n");
        return 77;   // ctest SKIP_RETURN_CODE
    }

    // ─── The stock generator: no lowercase ───────────────────────────────
    {
        Memory mem;
        assert(mem.loadCharRom(stock.c_str()) == 1);
        assert(mem.charRom().size() == 2048);
        assert(!mem.charRomHasLowercase() &&
               "the stock II/II+ generator has no lowercase glyphs");

        // The manual's invariant, checked directly: $80-$BF == $C0-$FF, so
        // the lowercase slot $E1 repeats whatever sits 0x40 below it — the
        // special-character slot $A1, not the 'A' at $C1. (Getting that pair
        // wrong is an easy mistake: $C1 and $E1 live in the SAME 64-glyph
        // block, so they were never each other's repeat.)
        uint8_t special[8], lower[8];
        glyph(mem, 0xA1, special);
        glyph(mem, 0xE1, lower);
        assert(sameGlyph(special, lower) &&
               "on a stock ROM the 'a' slot repeats the special char at $A1");
        std::printf("  ok: the stock 2 KB generator reports no lowercase\n");
    }

    // ─── The Videx chip: 2 KB, and lowercase ─────────────────────────────
    {
        Memory mem;
        assert(mem.loadCharRom(videx.c_str()) == 1);
        assert(mem.charRom().size() == 2048);
        assert(mem.charRomHasLowercase() &&
               "the Videx chip is 2 KB AND has lowercase — the whole point");

        uint8_t special[8], a[8], A[8];
        glyph(mem, 0xA1, special);
        glyph(mem, 0xE1, a);
        glyph(mem, 0xC1, A);
        assert(!sameGlyph(special, a) &&
               "the 'a' slot must no longer repeat $A1 — that IS the chip");

        // And it really is an 'a': no ascender, where 'A' has an apex on the
        // top row. Checked structurally rather than against a golden bitmap,
        // which would only restate the ROM back to itself.
        assert(a[0] == 0 && a[1] == 0 && "lowercase 'a' has no ascender");
        assert(A[0] != 0 || A[1] != 0);
        std::printf("  ok: the Videx chip is 2 KB and reports lowercase\n");
    }

    // ─── The convention: bit ordering survived normalisation ─────────────
    // 'L' is the cheapest unambiguous asymmetry: a vertical stroke on the
    // LEFT and a foot along the bottom. If the low-7-bit reversal were wrong
    // for this dump every glyph would render mirrored — and 'A', 'B' and 'a'
    // are all too symmetric to notice.
    {
        Memory mem;
        assert(mem.loadCharRom(videx.c_str()) == 1);
        uint8_t L[8];
        glyph(mem, 0xCC, L);       // 'L' in the normal range
        int stem = 0, footWidth = 0;
        for (int r = 0; r < 6; ++r) if (L[r] & 0x02) stem++;
        for (int b = 0; b < 7; ++b) if (L[6] & (1 << b)) footWidth++;
        assert(stem == 6 && "'L' has a stem down the left, not the right");
        assert((L[0] & 0x40) == 0 && "...and it must not touch the right edge");
        assert(footWidth >= 4 && "'L' has a foot along the bottom row");
        std::printf("  ok: bit order survives normalisation ('L' is not mirrored)\n");
    }

    std::printf("OK videx_lowercase_char_rom\n");
    return 0;
}
