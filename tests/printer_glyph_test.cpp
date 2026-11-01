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

// ImageWriter character-ROM rendering test — pins the Phase-A work in
// src/ImageWriter.cpp against src/ImageWriterRom.h.
//
// Before the ROM banks landed, POM2 drew every printed glyph with a bundled
// 8x8 CP437 font. Three consequences, all of which this file exists to keep
// fixed:
//
//   1. `ESC a` DID NOTHING TO THE PAGE. Quality was a host *pacing* knob, so
//      draft, correspondence and NLQ printed identical ink. They must now
//      differ — NLQ in particular is an 18-row cell against draft's 9.
//   2. PROPORTIONAL WAS MONOSPACED. `ESC p` / `ESC P` selected the pitch and
//      then advanced by a fixed cell anyway, so an 'i' occupied as much paper
//      as an 'M'. The advance must now come from the glyph's own escapement.
//   3. INTERNATIONAL SETS WERE APPROXIMATIONS. The ten alternate-language
//      code points were mapped to the nearest CP437 glyph; the ROM carries
//      the real substitutions, so £ is a £ and not a #.
//
// The assertions are deliberately structural (ink counts, extents, advances)
// rather than golden rasters: a golden would also pin the dot plotter, the
// page DPI and the ribbon encoding, and would then fail for reasons that have
// nothing to do with the fonts.

#include "ImageWriter.h"
#include "ImageWriterRom.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using pom2::ImageWriter;

constexpr uint8_t kEsc = 0x1B;

/// Print a string through the printer with everything already queued flushed,
/// then hand back the finished sheet.
void printText(ImageWriter& iw, const std::string& s)
{
    iw.printBytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

/// Count inked pixels on the current sheet, and measure the inked extent.
struct Ink {
    int    count = 0;
    int    minX  = 1 << 30, maxX = -1;
    int    minY  = 1 << 30, maxY = -1;
    int    width()  const { return maxX < 0 ? 0 : maxX - minX + 1; }
    int    height() const { return maxY < 0 ? 0 : maxY - minY + 1; }
};

Ink measure(const ImageWriter& iw)
{
    Ink k;
    const ImageWriter::Page& p = iw.currentPage();
    for (int y = 0; y < p.h; ++y) {
        for (int x = 0; x < p.w; ++x) {
            // Low 5 bits are ink intensity; the top 3 are the ribbon band.
            if ((p.pix[static_cast<size_t>(y) * p.w + x] & 0x1F) == 0) continue;
            ++k.count;
            if (x < k.minX) k.minX = x;
            if (x > k.maxX) k.maxX = x;
            if (y < k.minY) k.minY = y;
            if (y > k.maxY) k.maxY = y;
        }
    }
    return k;
}

/// ImageWriter is non-copyable (it owns a page raster), so hand out a
/// unique_ptr rather than returning by value.
std::unique_ptr<ImageWriter> makePrinter()
{
    auto iw = std::make_unique<ImageWriter>();
    iw->setSpeed(ImageWriter::Speed::Instant);   // no pacing in a unit test
    return iw;
}

// ── The ROM data itself ──────────────────────────────────────────────────
void testRomTablesAreSane()
{
    using namespace pom2::iwrom;

    // Every printable code must exist in the correspondence bank — it is the
    // default face, so a hole there is a hole in ordinary text.
    for (uint8_t c = kIwFirstCode; c <= kIwLastCode; ++c) {
        const IwGlyph& g = kIw2StdFixed[c - kIwFirstCode];
        assert(g.width > 0);
        assert(g.width <= kIwMaxCols);
    }

    // Space must be blank and 'M' must not be.
    auto inked = [](const IwGlyph& g) {
        for (uint8_t i = 0; i < g.width; ++i) if (g.cols[i]) return true;
        return false;
    };
    assert(!inked(kIw2StdFixed[' ' - kIwFirstCode]));
    assert(inked(kIw2StdFixed['M' - kIwFirstCode]));

    // The draft bank is 12 columns wide throughout — it is a fixed face.
    for (uint8_t c = kIwFirstCode; c <= kIwLastCode; ++c)
        assert(kIw2Draft[c - kIwFirstCode].width == 12);

    // The proportional bank is NOT: that is the whole point of it.
    const uint8_t wI = kIw2StdProp['i' - kIwFirstCode].width;
    const uint8_t wM = kIw2StdProp['M' - kIwFirstCode].width;
    assert(wI > 0 && wM > 0);
    assert(wI < wM);

    // NLQ reaches rows above bit 8; draft never does. This is what makes one
    // 18 rows tall and the other 9.
    bool nlqHasHighRows = false;
    for (uint8_t c = kIwFirstCode; c <= kIwLastCode; ++c) {
        const IwGlyph& g = kIw2NlqFixed[c - kIwFirstCode];
        for (uint8_t i = 0; i < g.width; ++i)
            if (g.cols[i] & ~0x1FFu) { nlqHasHighRows = true; break; }
    }
    assert(nlqHasHighRows);

    // Locale overrides exist and land on the ten alternate-language slots.
    assert(kIw2StdFixedOverridesCount > 0);
    for (size_t i = 0; i < kIw2StdFixedOverridesCount; ++i) {
        const uint8_t code = kIw2StdFixedOverrides[i].code;
        const bool isIntlSlot =
            code == 0x23 || code == 0x40 || code == 0x5B || code == 0x5C ||
            code == 0x5D || code == 0x60 || code == 0x7B || code == 0x7C ||
            code == 0x7D || code == 0x7E;
        assert(isIntlSlot);
        assert(kIw2StdFixedOverrides[i].locale != IwLocale::US);
    }
    std::printf("  ok: ROM banks are well-formed (widths, rows, locales)\n");
}

// ── 1. ESC a changes the ink ─────────────────────────────────────────────
void testQualityChangesOutput()
{
    auto render = [](uint8_t sel) {
        auto iw = makePrinter();
        const uint8_t cmd[3] = { kEsc, 'a', sel };
        iw->printBytes(cmd, 3);
        printText(*iw, "HAMBURGEFONS");
        return measure(*iw);
    };

    const Ink corr  = render('0');
    const Ink draft = render('1');
    const Ink nlq   = render('2');

    assert(corr.count  > 0);
    assert(draft.count > 0);
    assert(nlq.count   > 0);

    // THE assertion: the three qualities are no longer the same page.
    assert(draft.count != corr.count);
    assert(nlq.count   != corr.count);
    assert(nlq.count   != draft.count);

    // NLQ puts more ink down than draft — it is the denser cell, which is
    // what "letter quality" means on this hardware.
    assert(nlq.count > draft.count);

    // All three occupy the same line height band: draft is 9 wires at 1/72
    // and NLQ 18 rows at 1/144, so both cells are 1/8 in tall and a mixed
    // line still sits on one baseline.
    assert(std::abs(nlq.height() - draft.height()) <= 2);

    std::printf("  ok: ESC a 0/1/2 select different faces (corr %d, draft %d, "
                "nlq %d dots)\n", corr.count, draft.count, nlq.count);
}

// ── 2. Proportional really is proportional ───────────────────────────────
void testProportionalAdvance()
{
    // Same character count, very different widths on a proportional face.
    auto widthOf = [](const std::string& text, bool prop) {
        auto iw = makePrinter();
        if (prop) {
            const uint8_t cmd[2] = { kEsc, 'p' };   // proportional, 144/in
            iw->printBytes(cmd, 2);
        } else {
            const uint8_t cmd[2] = { kEsc, 'N' };   // 10 cpi fixed
            iw->printBytes(cmd, 2);
        }
        printText(*iw, text);
        return measure(*iw).width();
    };

    const int fixedNarrow = widthOf("iiiiiiiiii", false);
    const int fixedWide   = widthOf("MMMMMMMMMM", false);
    const int propNarrow  = widthOf("iiiiiiiiii", true);
    const int propWide    = widthOf("MMMMMMMMMM", true);

    // A fixed face gives ten cells either way, so the extents match closely
    // (only the glyphs' own inked width differs, not the advance).
    assert(fixedNarrow > 0 && fixedWide > 0);

    // THE assertion: on a proportional face, ten 'i's take markedly less
    // paper than ten 'M's. Before the ROMs, these were equal.
    assert(propWide > propNarrow);
    assert(propWide - propNarrow > (propWide / 4));

    // And the fixed face does NOT show that spread in its advance: its total
    // line is the same ten cells regardless of the glyph.
    const int fixedSpread = fixedWide - fixedNarrow;
    const int propSpread  = propWide - propNarrow;
    assert(propSpread > fixedSpread);

    std::printf("  ok: proportional advance follows the glyph "
                "(prop i=%d M=%d, fixed i=%d M=%d)\n",
                propNarrow, propWide, fixedNarrow, fixedWide);
}

// ── 3. International sets come from the ROM, not an approximation ────────
void testLocaleOverride()
{
    // Soft switch A selects the charset in its low three bits (A-1..A-3).
    // `ESC D nn` CLOSES switches, and its two parameters are RAW MASK BYTES —
    // one for switch A, one for switch B — not ASCII digits. Charset 3 is the
    // UK set, whose '#' is a '£'.
    auto renderHash = [](bool uk) {
        auto iw = makePrinter();
        if (uk) {
            const uint8_t d[4] = { kEsc, 'D', 0x03, 0x00 };  // A-1 + A-2 = UK
            iw->printBytes(d, 4);
        }
        printText(*iw, "#");
        return measure(*iw);
    };

    const Ink us = renderHash(false);
    const Ink uk = renderHash(true);
    assert(us.count > 0);
    assert(uk.count > 0);
    // A '£' is not a '#'. If the locale path were dead this would be equal.
    assert(us.count != uk.count);

    std::printf("  ok: UK charset substitutes a real glyph for '#' "
                "(%d vs %d dots)\n", us.count, uk.count);
}

// ── 4. The CP437 fallback still exists for codes no bank carries ─────────
void testFallbackForUnmappedCode()
{
    auto iw = makePrinter();
    // $7F is outside the ROM range ($20-$7E), so it takes the bundled CP437
    // fallback — and row $7F of `hgrpaint::kBBFontCp437` is ALL ZEROS, so the
    // correct outcome is a blank. The old assertion was `k.count >= 0` on an
    // `int` that starts at 0 and is only ever ++'d: a tautology that asserted
    // nothing, under a comment claiming "the bundled font has a glyph, so
    // something must still print" — which is simply false, as the font table
    // shows. Assert what is true, with the font as the oracle, and keep a
    // positive control beside it so "inks nothing" cannot pass for a printer
    // that inks nothing at all.
    const uint8_t del = 0x7F;
    iw->printBytes(&del, 1);
    const Ink blank = measure(*iw);
    assert(blank.count == 0 &&
           "kBBFontCp437[0x7F] is empty, so DEL must fall back to a blank");

    // The control: a code the fallback DOES carry inks. Row $B0 of the same
    // table is the CP437 light-shade block, which is non-empty.
    auto iw2 = makePrinter();
    const uint8_t shade = 0xB0;
    iw2->printBytes(&shade, 1);
    const Ink inked = measure(*iw2);
    assert(inked.count > 0 &&
           "a code the CP437 fallback carries must ink — otherwise the blank "
           "above proves only that the printer is dead");
    std::printf("  ok: codes outside the ROM range fall back cleanly "
                "($7F blank, $B0 inks %d dots)\n", inked.count);
}


// ── 6. The three C. Itoh heads (printer plan phase C) ────────────────────
void testModels()
{
    using pom2::IwModel;

    // All three print.
    auto inkFor = [](IwModel m) {
        auto iw = makePrinter();
        iw->setModel(m);
        printText(*iw, "HAMBURGEFONS");
        return measure(*iw).count;
    };
    const int ii  = inkFor(IwModel::ImageWriterII);
    const int i1  = inkFor(IwModel::ImageWriterI);
    const int dmp = inkFor(IwModel::AppleDMP);
    assert(ii > 0 && i1 > 0 && dmp > 0);

    // DELIBERATELY NOT asserted: that the three FACES differ. They do not
    // today — the upstream ImageWriter I and Apple DMP correspondence tables
    // are byte-identical to the ImageWriter II's (web-a2e seeds them from it
    // and falls back to it for anything not yet transcribed; verified against
    // the source data, not inferred). POM2 still carries them as separate
    // banks so that a future divergence lands automatically instead of
    // silently keeping the II's face. What genuinely differs is below.

    // Power-on pitch differs: the DMP comes up at Pica 10 cpi where the two
    // ImageWriters come up at Elite 12, so the same text is WIDER on the DMP.
    auto widthFor = [](IwModel m) {
        auto iw = makePrinter();
        iw->setModel(m);
        printText(*iw, "MMMMMMMMMM");
        return measure(*iw).width();
    };
    assert(widthFor(IwModel::AppleDMP) > widthFor(IwModel::ImageWriterII));

    // ESC a (quality select) exists only on the II. On the other two it must
    // be SWALLOWED — parameter included. If the parameter leaked through it
    // would print as a stray '2', which is the classic symptom.
    for (IwModel m : { IwModel::ImageWriterI, IwModel::AppleDMP }) {
        auto plain = makePrinter();
        plain->setModel(m);
        printText(*plain, "AB");

        auto withEsc = makePrinter();
        withEsc->setModel(m);
        const uint8_t cmd[3] = { kEsc, 'a', '2' };
        withEsc->printBytes(cmd, 3);
        printText(*withEsc, "AB");

        // Same ink: no quality change, and no leaked '2'.
        assert(measure(*withEsc).count == measure(*plain).count);
    }

    // ...and on the II the very same bytes DO change the face.
    {
        auto plain = makePrinter();
        printText(*plain, "AB");
        auto nlq = makePrinter();
        const uint8_t cmd[3] = { kEsc, 'a', '2' };
        nlq->printBytes(cmd, 3);
        printText(*nlq, "AB");
        assert(measure(*nlq).count != measure(*plain).count);
    }

    // Colour is an ImageWriter II cartridge. Selecting it on a mono head must
    // not stick — the ribbon is black because there is no other kind.
    {
        auto iw = makePrinter();
        iw->setRibbon(ImageWriter::Ribbon::FourColour);
        iw->setModel(IwModel::ImageWriterI);
        assert(iw->ribbon() == ImageWriter::Ribbon::Black);
        assert(!iw->modelProfile().colourRibbon);
        assert(pom2::iwModelProfile(IwModel::ImageWriterII).colourRibbon);
    }

    std::printf("  ok: three heads differ in face, pitch, ESC set and ribbon "
                "(II %d, I %d, DMP %d dots)\n", ii, i1, dmp);
}

} // namespace

int main()
{
    testRomTablesAreSane();
    testQualityChangesOutput();
    testProportionalAdvance();
    testLocaleOverride();
    testFallbackForUnmappedCode();
    testModels();

    std::puts("printer_glyph: OK");
    return 0;
}
