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

// CharRomDump — what an Apple II character-generator dump looks like, and
// how to turn one into the form the renderer wants. Split out of Memory
// because it is a fact about FILES, not about the memory map, and because
// it is worth testing on its own: two dumps of the same size can need
// opposite treatment.
//
// POM2's internal form is AppleWin's "csbits" convention:
//   * one byte per glyph row, 7 visible pixels
//   * bit 0 = leftmost, 1 = lit
//   * codes $00-$3F (inverse range) are stored PRE-FLIPPED, so they already
//     read as inverse video; codes $40-$7F hold the non-flashing glyph and
//     the renderer XORs 0x7F on the flash phase.
//
// Three dump conventions map onto it:
//
//   4 KB (IIe-class)  Pixels are stored with inverted polarity (1 = OFF)
//                     and bit 0 already leftmost. XOR 0xFF, nothing else.
//
//   2 KB, marked      AppleWin's video ROM and the tracked Videx dump. Bit 7
//                     marks the range: clear = inverse (XOR the low 7),
//                     set = flashing (leave). Bits are stored MSB-left
//                     (the video shift register reads that way), so the
//                     low 7 are reversed.
//
//   2 KB, unmarked    Legacy input without range markers (the tracked Videx
//                     dump is marked). With no bit 7 marker, the range has
//                     to be split by OFFSET instead (first 512 bytes are
//                     the inverse range). Choosing wrong here inverts the
//                     entire normal range, and because the glyph SHAPES
//                     come out identical either way it is invisible in a
//                     screenshot of ordinary text.

#ifndef POM2_CHAR_ROM_DUMP_H
#define POM2_CHAR_ROM_DUMP_H

#include <cstddef>
#include <cstdint>

namespace pom2 {

struct CharRomFacts {
    /// Whether the generator actually carries lowercase glyphs. A 4 KB
    /// IIe-class part always does; a 2 KB one usually does NOT — except
    /// the Videx chip, which is why this is a fact about the dump rather
    /// than about its size. The renderer folds a-z to A-Z when false.
    ///
    /// The test comes from the Videx manual's own description of the part
    /// it replaces (§ Discussion of character display): on a stock
    /// generator "Characters 80 - BF are identical to characters C0 - FF",
    /// so the lowercase slots hold repeats. A chip that adds lowercase
    /// breaks that equality.
    bool hasLowercase = false;
    /// Which 2 KB convention was detected — false means the Videx-style
    /// unmarked dump. Always true for a 4 KB part (the question does not
    /// arise there). Reported so the loader can say which rule it used.
    bool bit7Marker = true;
};

/// Normalise `size` bytes in place. `size` must be 2048 or 4096 — an 8 KB
/// international //e part is two 4 KB banks and the caller picks one first.
CharRomFacts normaliseCharRom(uint8_t* bytes, std::size_t size);

} // namespace pom2

#endif // POM2_CHAR_ROM_DUMP_H
