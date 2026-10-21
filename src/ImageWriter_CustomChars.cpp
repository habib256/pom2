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

// ImageWriter_CustomChars.cpp — the ImageWriter II's downloadable character
// set (Technical Reference ch. 7), split out of ImageWriter.cpp (file-size
// ratchet). The parser hooks — ESC - / + / I / ' / * / $ and the load
// intercept in printCharInternal — stay in ImageWriter.cpp; this file owns
// the loading state machine and the drawing.

#include "ImageWriter.h"
#include "PrinterSoundSink.h"

#include <algorithm>

namespace pom2 {

namespace {
constexpr uint8_t kSwitchBEighthDataBit = 0x20;   // switch B-6, as in ImageWriter.cpp
}

// Everything the printer was in the middle of receiving, which `ESC c` and a
// power-off both abandon: a half-received command must not eat the start of
// the next job. Every parser has its collector here (ImageWriter, ESC/P,
// Diablo), and the standard font comes back with any ESC I dropped — the
// SET itself survives a reset and is lost at power-off (ch. 7).
void ImageWriter::abandonPendingInput()
{
    escSeen_ = fsSeen_ = false;
    escCmd_  = 0;
    numParam_ = neededParam_ = 0;
    epsonNeed_  = 0;
    epsonCount_ = 0;
    diabloNeed_ = 0;
    diabloCmd_  = 0;
    customFont_ = 0;
    customLoad_ = CustomLoad::Off;
    bitGraph_.remBytes = 0;
    bitGraph_.readBytesColumn = 0;
    // A repeat run still owed would keep `busy()` asserted with nothing in
    // the input buffer to explain it. ESC V/U's deferred `msb_` restore is
    // owed with it: `ESC c` re-derives msb_ in updateSwitch(), but a power-off
    // does not, and left bit 7 masked for every byte of the next job.
    if (repeatRestoresMsb_)
        msb_ = (switchb_ & kSwitchBEighthDataBit) ? 0 : 255;
    repeatRemaining_   = 0;
    repeatPatPos_      = 0;
    repeatRestoresMsb_ = false;
}

void ImageWriter::clearCustomGlyphs()
{
    for (CustomGlyph& g : customGlyphs_) g = CustomGlyph{};
    customWide_ = false;
}

// The ESC commands of ch. 7. The ImageWriter I, the DMP and the 8510A share
// the parser but not that chapter, so on them these stay no-ops.
void ImageWriter::execCustomCharEscape(uint8_t cmd)
{
    if (model_ != IwModel::ImageWriterII) return;
    switch (cmd) {
    case 0x2d: case 0x2b:                 // ESC - / ESC +  max 8 / 16 dots
        // "When you send the ESC - or ESC + control codes … any previously-
        // loaded custom characters are erased from memory", and both "switch
        // the ImageWriter II into 8-bit mode": the data bytes are 8 bits —
        // what ESC Z ^@ SPACE does (open switch B-6).
        clearCustomGlyphs();
        customWide_ = (cmd == 0x2b);
        switchb_ &= static_cast<uint8_t>(~0x20);
        updateSwitch();
        break;
    case 0x49: customLoad_ = CustomLoad::Key; break;   // ESC I  start loading
    case 0x27: customFont_ = 1; break;                 // ESC '  custom font
    case 0x2a: customFont_ = 2; break;                 // ESC *  high ASCII
    default: break;
    }
}

// ── Custom characters (ImageWriter II Technical Reference, ch. 7) ──────
//
//   ESC - ESC I KEY1 WIDTH1 DATA1 … KEYn WIDTHn DATAn … CTRL-D
//
// KEY is the character replaced (32-126, or 160-239 with the 8-dot
// maximum); WIDTH a letter from Table 7-2 — A-P for 1-16 dots on wires 1-8,
// a-p on wires 2-9; then that many column bytes, bit 0 = top wire.
void ImageWriter::feedCustomLoad(uint8_t raw)
{
    switch (customLoad_) {
    case CustomLoad::Key:
        if (raw == 0x04) { customLoad_ = CustomLoad::Off; return; }   // CTRL-D
        customKey_  = raw;
        customLoad_ = CustomLoad::Width;
        return;
    case CustomLoad::Width: {
        const bool upper = raw >= 'A' && raw <= 'P';
        const bool lower = raw >= 'a' && raw <= 'p';
        if (!upper && !lower) {
            // Not a width code: the stream is garbled ("if a single
            // character is omitted or wrong, the whole result can become
            // garbled"). Stop loading rather than eat the rest of the job.
            customLoad_ = CustomLoad::Off;
            return;
        }
        customWidth_ = static_cast<uint8_t>(raw - (upper ? 'A' : 'a') + 1);
        customPos_   = 0;
        CustomGlyph& g = customGlyphs_[customKey_];
        g = CustomGlyph{};
        g.lowerWires = lower;
        customLoad_  = CustomLoad::Data;
        return;
    }
    case CustomLoad::Data: {
        // All WIDTH bytes are consumed whatever the maximum, so the framing
        // holds; columns past it are not kept.
        CustomGlyph& g = customGlyphs_[customKey_];
        const uint8_t maxW = customWide_ ? 16 : 8;
        if (customPos_ < maxW) g.cols[customPos_] = raw;
        if (++customPos_ >= customWidth_) {
            g.width = std::min(customWidth_, maxW);
            customLoad_ = CustomLoad::Key;
        }
        return;
    }
    case CustomLoad::Off:
        return;
    }
}

const ImageWriter::CustomGlyph* ImageWriter::customGlyphFor(uint8_t ch) const
{
    if (customFont_ == 0 || model_ != IwModel::ImageWriterII) return nullptr;
    // ESC * fetches the high-ASCII set with plain characters: "subtract 128
    // … and send the resulting standard ASCII character" (32-111 → 160-239).
    uint8_t code = ch;
    if (customFont_ == 2) {
        if (ch < 32 || ch > 111) return nullptr;
        code = static_cast<uint8_t>(ch + 0x80);
    }
    const CustomGlyph& g = customGlyphs_[code];
    // A key never given a shape prints its standard character.
    return g.width ? &g : nullptr;
}

double ImageWriter::customDotPitch() const
{
    // Custom characters "expand and contract with changes in character
    // pitch": a fixed pitch is eight dot columns per character (pica 80
    // dots/in, ultracondensed 136), a proportional one its defined unit.
    // Double width and condensed reach both through actcpi_.
    if ((style_ & kStyleProp) && definedUnit_ > 0)
        return (cpi_ / actcpi_) / static_cast<double>(definedUnit_);
    return 1.0 / (8.0 * actcpi_);
}

void ImageWriter::renderCustomGlyph(const CustomGlyph& g)
{
    // Wires are 1/72 in apart; wires 2-9 sit one wire lower than 1-8.
    const double dotW = customDotPitch();
    double dotH = 1.0 / 72.0;
    double top  = curY_ + (g.lowerWires ? 1.0 / 72.0 : 0.0);
    if (style_ & (kStyleSuperscript | kStyleSubscript | kStyleHalfHeight)) {
        dotH *= 2.0 / 3.0;
        if (!(style_ & kStyleSuperscript)) top += (8.0 / 72.0) - 8.0 * dotH;
    }
    const double inkW = (style_ & kStyleBold) ? dotW * 1.5 : dotW;
    int pins = 0;
    for (int x = 0; x < g.width; ++x) {
        for (int y = 0; y < 8; ++y) {
            if (!(g.cols[x] & (1u << y))) continue;
            ++pins;
            const double shear = (style_ & kStyleItalics) ? dotW * (7 - y) / 7.0 : 0.0;
            fillDots(curX_ + x * dotW + shear, top + y * dotH, inkW, dotH);
        }
    }
    if (sound_ && pins) sound_->strike(std::min(pins, 9));
}

} // namespace pom2
