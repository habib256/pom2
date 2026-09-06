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

// Screen-dump test — pins src/PrinterScreenDump.cpp against the real
// ImageWriter parser.
//
// The assertion that matters is a ROUND TRIP: build the byte stream from a
// known framebuffer, feed it to `ImageWriter`, and check the ink that lands on
// paper reproduces the picture. That pins the scanner and the parser AGAINST
// EACH OTHER, which is the whole point of synthesising a wire format instead
// of painting the page — a dump that agreed only with itself would be a
// screenshot with extra steps.
//
// Specifically kept honest here:
//
//   1. BIT 0 IS THE TOP DOT. The C. Itoh family packs a band that way and
//      Epson's ESC * packs it the other way. Get it wrong and every dump
//      comes out mirrored vertically in 8-pixel stripes — which looks like a
//      plausible picture, so only a round trip catches it.
//   2. THE BAND FEED ABUTS. 16/144 in is exactly the 8 dots of a band at
//      72 dpi. Too small and bands overprint, too large and the page is
//      combed with white lines.
//   3. AUTO-INVERT PICKS BY DENSITY. The screen is light-on-dark, paper is
//      the reverse; dumping a text screen without inverting floods the sheet.

#include "ImageWriter.h"
#include "PrinterScreenDump.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using pom2::ImageWriter;
using pom2::ScreenDumpOptions;

constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kWhite = 0xFFFFFFFFu;

/// Count the ESC G blocks and the total column bytes a stream carries.
struct StreamShape {
    int  gfxBlocks   = 0;
    int  totalCols   = 0;
    int  feeds       = 0;
    bool sawFormFeed = false;
};

StreamShape shapeOf(const std::vector<uint8_t>& s)
{
    StreamShape sh;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == 0x0C) { sh.sawFormFeed = true; continue; }
        if (s[i] != 0x1B || i + 1 >= s.size()) continue;
        if (s[i + 1] == 'G' && i + 5 < s.size()) {
            const int n = (s[i + 2] - '0') * 1000 + (s[i + 3] - '0') * 100 +
                          (s[i + 4] - '0') * 10 + (s[i + 5] - '0');
            ++sh.gfxBlocks;
            sh.totalCols += n;
            i += 5 + n;                       // skip the data too
        } else if (s[i + 1] == 'T') {
            ++sh.feeds;
            i += 3;
        }
    }
    return sh;
}

/// Render a stream and report which page pixels took ink.
struct Sheet {
    int w = 0, h = 0;
    std::vector<uint8_t> ink;
    bool at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return false;
        return ink[static_cast<size_t>(y) * w + x] != 0;
    }
};

Sheet renderStream(const std::vector<uint8_t>& stream)
{
    ImageWriter iw;
    iw.setSpeed(ImageWriter::Speed::Instant);
    iw.printBytes(stream.data(), stream.size());

    // A form feed banks the sheet, so read the completed one when present.
    const ImageWriter::Page& p =
        iw.completedPageCount() > 0 ? iw.completedPage(0) : iw.currentPage();

    Sheet s;
    s.w = p.w;
    s.h = p.h;
    s.ink.assign(static_cast<size_t>(p.w) * p.h, 0);
    for (size_t i = 0; i < s.ink.size(); ++i)
        s.ink[i] = (p.pix[i] & 0x1F) ? 1 : 0;
    return s;
}

/// Total inked pixels.
long inkCount(const Sheet& s)
{
    long n = 0;
    for (uint8_t v : s.ink) n += v;
    return n;
}

// ── 1. Stream shape ──────────────────────────────────────────────────────
void testStreamShape()
{
    const int w = 64, h = 24;
    std::vector<uint32_t> fb(static_cast<size_t>(w) * h, kBlack);

    std::vector<uint8_t> out;
    ScreenDumpOptions opt;
    opt.autoInvert = false;
    pom2::buildScreenDumpImageWriter(fb.data(), w, h, w, opt, out);

    const StreamShape sh = shapeOf(out);
    // 24 rows / 8 per band = 3 bands.
    assert(sh.gfxBlocks == 3);
    assert(sh.totalCols == w * 3);
    assert(sh.feeds == 3);
    assert(sh.sawFormFeed);
    std::printf("  ok: %d bands, %d column bytes, %d feeds\n",
                sh.gfxBlocks, sh.totalCols, sh.feeds);
}

// ── 2. THE round trip ────────────────────────────────────────────────────
void testRoundTripReproducesPattern()
{
    // A pattern whose top and bottom halves differ, so a flipped band packing
    // (bit 7 = top instead of bit 0) cannot pass by accident.
    const int w = 32, h = 16;
    std::vector<uint32_t> fb(static_cast<size_t>(w) * h, kBlack);
    for (int x = 0; x < w; ++x) fb[static_cast<size_t>(0) * w + x] = kWhite;
    for (int x = 0; x < 4; ++x)  fb[static_cast<size_t>(7) * w + x] = kWhite;

    std::vector<uint8_t> out;
    ScreenDumpOptions opt;
    opt.autoInvert = false;          // print the lit pixels as ink
    opt.formFeed   = false;          // keep it on the platen
    pom2::buildScreenDumpImageWriter(fb.data(), w, h, w, opt, out);

    const Sheet s = renderStream(out);
    assert(inkCount(s) > 0);

    // Find the inked bounding box.
    int minX = 1 << 30, maxX = -1, minY = 1 << 30, maxY = -1;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x)
            if (s.at(x, y)) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
    assert(maxX >= 0);

    // The full-width row is row 0 and the short row is row 7 of the same
    // band, so the widest ink must sit ABOVE the narrow ink. If bit 0 were
    // treated as the bottom dot, this ordering inverts.
    int widestRowY = -1, widestRun = -1;
    int narrowRowY = -1;
    for (int y = minY; y <= maxY; ++y) {
        int run = 0;
        for (int x = 0; x < s.w; ++x) if (s.at(x, y)) ++run;
        if (run > widestRun) { widestRun = run; widestRowY = y; }
        if (run > 0 && run < widestRun / 2 && narrowRowY < 0) narrowRowY = y;
    }
    assert(widestRowY >= 0);
    assert(narrowRowY > widestRowY);     // ← bit 0 is the TOP dot

    std::printf("  ok: round trip preserves the pattern "
                "(wide row y=%d, narrow row y=%d)\n", widestRowY, narrowRowY);
}

// ── 3. Bands abut ────────────────────────────────────────────────────────
void testBandsAbut()
{
    // A solid block spanning three bands must come out as ONE solid block:
    // no white comb between bands, no doubled rows.
    const int w = 16, h = 24;
    std::vector<uint32_t> fb(static_cast<size_t>(w) * h, kWhite);

    std::vector<uint8_t> out;
    ScreenDumpOptions opt;
    opt.autoInvert = false;
    opt.formFeed   = false;
    pom2::buildScreenDumpImageWriter(fb.data(), w, h, w, opt, out);

    const Sheet s = renderStream(out);
    int minY = 1 << 30, maxY = -1;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x)
            if (s.at(x, y)) { if (y < minY) minY = y; if (y > maxY) maxY = y; }
    assert(maxY >= 0);

    // Every row inside the block must carry ink — a gap means the band feed
    // overshot.
    int blank = 0;
    for (int y = minY; y <= maxY; ++y) {
        bool any = false;
        for (int x = 0; x < s.w; ++x) if (s.at(x, y)) { any = true; break; }
        if (!any) ++blank;
    }
    assert(blank == 0);
    std::printf("  ok: %d contiguous inked rows across 3 bands, no seam\n",
                maxY - minY + 1);
}

// ── 4. Auto-invert ───────────────────────────────────────────────────────
void testAutoInvert()
{
    const int w = 32, h = 8;

    // Mostly dark (a graphics screen): print the lit pixels, do not invert.
    std::vector<uint32_t> dark(static_cast<size_t>(w) * h, kBlack);
    dark[0] = kWhite;
    ScreenDumpOptions opt;      // autoInvert on by default
    assert(!pom2::screenDumpWouldInvert(dark.data(), w, h, w, opt));

    // Mostly lit (a text screen): invert, or the sheet floods.
    std::vector<uint32_t> lit(static_cast<size_t>(w) * h, kWhite);
    lit[0] = kBlack;
    assert(pom2::screenDumpWouldInvert(lit.data(), w, h, w, opt));

    // And the flood is real: without inversion a lit screen inks nearly
    // every dot.
    std::vector<uint8_t> flooded, sane;
    ScreenDumpOptions forced = opt;
    forced.autoInvert = false;
    forced.invert     = false;
    forced.formFeed   = false;
    pom2::buildScreenDumpImageWriter(lit.data(), w, h, w, forced, flooded);
    ScreenDumpOptions autoOpt = opt;
    autoOpt.formFeed = false;
    pom2::buildScreenDumpImageWriter(lit.data(), w, h, w, autoOpt, sane);

    assert(inkCount(renderStream(flooded)) > inkCount(renderStream(sane)) * 4);
    std::printf("  ok: auto-invert picks polarity by lit density\n");
}

// ── 5. Degenerate input must not crash ───────────────────────────────────
void testDegenerate()
{
    std::vector<uint8_t> out;
    ScreenDumpOptions opt;
    pom2::buildScreenDumpImageWriter(nullptr, 32, 32, 32, opt, out);
    assert(out.empty());

    std::vector<uint32_t> fb(4, kBlack);
    pom2::buildScreenDumpImageWriter(fb.data(), 0, 0, 0, opt, out);
    assert(out.empty());

    // A height that is not a multiple of the band is the normal case for a
    // 192-row screen dumped in 8s — but check a ragged one explicitly.
    pom2::buildScreenDumpImageWriter(fb.data(), 2, 2, 2, opt, out);
    assert(!out.empty());
    std::printf("  ok: null / empty / ragged input handled\n");
}


// ── 6. Front panel: power and online (printer plan phase D) ─────────────
void testPowerAndOnline()
{
    const int w = 16, h = 8;
    std::vector<uint32_t> fb(static_cast<size_t>(w) * h, kWhite);
    std::vector<uint8_t> stream;
    ScreenDumpOptions opt;
    opt.autoInvert = false;
    opt.invert     = false;
    opt.formFeed   = false;
    pom2::buildScreenDumpImageWriter(fb.data(), w, h, w, opt, stream);

    // Powered ON and online: ink lands.
    long onCount = 0;
    {
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.printBytes(stream.data(), stream.size());
        const ImageWriter::Page& p = iw.currentPage();
        for (size_t i = 0; i < p.pix.size(); ++i) if (p.pix[i] & 0x1F) ++onCount;
        assert(onCount > 0);
    }

    // Powered OFF: the bytes are gone, not queued.
    {
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setPowered(false);
        iw.printBytes(stream.data(), stream.size());
        const ImageWriter::Page& p = iw.currentPage();
        long n = 0;
        for (size_t i = 0; i < p.pix.size(); ++i) if (p.pix[i] & 0x1F) ++n;
        assert(n == 0);

        // AND THE PAPER SURVIVES the switch: powering back on must not have
        // ejected or wiped anything, and must not replay the lost bytes.
        iw.setPowered(true);
        const ImageWriter::Page& p2 = iw.currentPage();
        long n2 = 0;
        for (size_t i = 0; i < p2.pix.size(); ++i) if (p2.pix[i] & 0x1F) ++n2;
        assert(n2 == 0);

        // ...and it accepts new work straight away.
        iw.printBytes(stream.data(), stream.size());
        const ImageWriter::Page& p3 = iw.currentPage();
        long n3 = 0;
        for (size_t i = 0; i < p3.pix.size(); ++i) if (p3.pix[i] & 0x1F) ++n3;
        assert(n3 == onCount);
    }

    // OFFLINE: powered, but deselected — same "the byte never arrived".
    {
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setOnline(false);
        iw.printBytes(stream.data(), stream.size());
        const ImageWriter::Page& p = iw.currentPage();
        long n = 0;
        for (size_t i = 0; i < p.pix.size(); ++i) if (p.pix[i] & 0x1F) ++n;
        assert(n == 0);
        assert(iw.powered());
    }
    std::printf("  ok: power/online gate input and preserve the sheet\n");
}

// ── 7. Custom paper geometry ─────────────────────────────────────────────
void testPaperDimensions()
{
    ImageWriter iw;
    double w = 0, l = 0;

    // A legal size commits exactly.
    iw.setPaperDimensions(8.5, 11.0, &w, &l);
    assert(w == 8.5 && l == 11.0);
    assert(iw.paperWidthIn() == 8.5);

    // Quarter-inch snapping.
    iw.setPaperDimensions(8.6, 11.1, &w, &l);
    assert(w == 8.5);
    assert(l == 11.0);

    // Out of range clamps, and REPORTS what it clamped to — a caller that
    // asked for something impossible must not be left believing it got it.
    iw.setPaperDimensions(99.0, 999.0, &w, &l);
    assert(w == ImageWriter::kMaxPaperWidthIn);
    assert(l == ImageWriter::kMaxPaperLengthIn);
    iw.setPaperDimensions(0.1, 0.1, &w, &l);
    assert(w == ImageWriter::kMinPaperWidthIn);
    assert(l == ImageWriter::kMinPaperLengthIn);

    // The page raster followed the paper.
    iw.setPaperDimensions(8.0, 12.0, &w, &l);
    assert(iw.currentPage().w > 0 && iw.currentPage().h > 0);
    const double ratio = static_cast<double>(iw.currentPage().h) /
                         static_cast<double>(iw.currentPage().w);
    assert(ratio > 1.4 && ratio < 1.6);        // 12/8 = 1.5
    std::printf("  ok: paper dimensions snap, clamp and report\n");
}


// ── 8. Epson FX-80: ESC/P round trip (printer plan phase C3) ─────────────
void testEpsonRoundTrip()
{
    using pom2::IwModel;

    // Same asymmetric pattern as the C. Itoh round trip. The Epson packs bit
    // 7 as the TOP dot — the OPPOSITE of ESC G — so a parser that reused the
    // C. Itoh bit order would print this upside-down in 8-pixel stripes and
    // still look like a picture. Only the round trip catches it.
    const int w = 32, h = 16;
    std::vector<uint32_t> fb(static_cast<size_t>(w) * h, kBlack);
    for (int x = 0; x < w; ++x) fb[static_cast<size_t>(0) * w + x] = kWhite;
    for (int x = 0; x < 4; ++x)  fb[static_cast<size_t>(7) * w + x] = kWhite;

    std::vector<uint8_t> out;
    ScreenDumpOptions opt;
    opt.autoInvert = false;
    opt.formFeed   = false;
    pom2::buildScreenDumpEpson(fb.data(), w, h, w, opt, out);
    assert(!out.empty());

    // The stream must be ESC/P, not ESC G: ESC * with a mode byte and a
    // BINARY count, never four ASCII digits.
    bool sawStar = false;
    for (size_t i = 0; i + 4 < out.size(); ++i)
        if (out[i] == 0x1B && out[i + 1] == '*') {
            sawStar = true;
            // count low/high, little-endian
            const int n = out[i + 3] | (out[i + 4] << 8);
            assert(n == w);
            break;
        }
    assert(sawStar);

    ImageWriter iw;
    iw.setSpeed(ImageWriter::Speed::Instant);
    iw.setModel(IwModel::EpsonFX80);
    iw.printBytes(out.data(), out.size());

    const ImageWriter::Page& p = iw.currentPage();
    int minY = 1 << 30, maxY = -1;
    long ink = 0;
    std::vector<uint8_t> mask(p.pix.size(), 0);
    for (size_t i = 0; i < p.pix.size(); ++i) {
        if (!(p.pix[i] & 0x1F)) continue;
        mask[i] = 1; ++ink;
        const int y = static_cast<int>(i / p.w);
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    assert(ink > 0);

    int widestY = -1, widest = -1, narrowY = -1;
    for (int y = minY; y <= maxY; ++y) {
        int run = 0;
        for (int x = 0; x < p.w; ++x)
            if (mask[static_cast<size_t>(y) * p.w + x]) ++run;
        if (run > widest) { widest = run; widestY = y; }
        if (run > 0 && run < widest / 2 && narrowY < 0) narrowY = y;
    }
    assert(widestY >= 0);
    assert(narrowY > widestY);        // ← bit 7 is the TOP dot

    std::printf("  ok: FX-80 ESC/P round trip (wide y=%d, narrow y=%d)\n",
                widestY, narrowY);
}

// ── 9. The two grammars really are different ─────────────────────────────
void testEscPSlashCItohCollision()
{
    using pom2::IwModel;
    // ESC G is GRAPHICS on the C. Itoh family and DOUBLE-STRIKE on the
    // Epson. Feeding the same bytes to the two models must therefore do
    // completely different things — which is the reason the parsers are
    // separate rather than one switch with flags.
    const uint8_t seq[] = { 0x1B, 'G', 'A', 'B' };

    ImageWriter a;
    a.setSpeed(ImageWriter::Speed::Instant);
    a.printBytes(seq, sizeof(seq));

    ImageWriter b;
    b.setSpeed(ImageWriter::Speed::Instant);
    b.setModel(IwModel::EpsonFX80);
    b.printBytes(seq, sizeof(seq));

    long ia = 0, ib = 0;
    for (uint8_t v : a.currentPage().pix) if (v & 0x1F) ++ia;
    for (uint8_t v : b.currentPage().pix) if (v & 0x1F) ++ib;

    // On the Epson, ESC G is a style toggle and "AB" prints. On the C. Itoh
    // it starts a graphics block that swallows them.
    assert(ib > 0);
    assert(ia != ib);
    std::printf("  ok: ESC G means graphics on C.Itoh, double-strike on ESC/P "
                "(%ld vs %ld dots)\n", ia, ib);
}

// ── 10. The FX-80 head must not double-feed a CR+LF ──────────────────────
//
// processCommandChar keeps a "the previous byte was a CR we line-fed for"
// latch so a guest supplying its own LF does not get a second feed — but it
// sets that latch AFTER routing to the ESC/P parser, so for a long time the
// Epson head never maintained it. Every CR+LF line fed the platen TWICE.
//
// Visible in the screen dump above all: buildScreenDumpEpson ends each band
// with `ESC 3 24 \r \n`, so bands computed to abut at 1/9" landed 2/9" apart —
// a white seam the height of a band between every one, and a dump twice as
// long. testBandsAbut covers the C. Itoh head only, which is why this slipped
// through.
void testEpsonCrLfDoesNotDoubleFeed()
{
    using pom2::IwModel;

    auto headAfterTenLines = [](IwModel m) {
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(m);
        const uint8_t line[] = { 'X', 0x0D, 0x0A };
        for (int i = 0; i < 10; ++i) iw.printBytes(line, sizeof(line));
        return iw.status().headY;
    };

    const double citoh = headAfterTenLines(IwModel::ImageWriterII);
    const double epson = headAfterTenLines(IwModel::EpsonFX80);

    // Both heads default to 1/6" spacing, so ten CR+LF lines advance 10/6".
    // The bug put the Epson at exactly twice that.
    assert(epson < citoh * 1.5 &&
           "the ESC/P head feeds twice per CR+LF line");
    assert(std::fabs(epson - citoh) < 1e-6);

    std::printf("  ok: CR+LF feeds once on both heads (C.Itoh %.4f\", "
                "FX-80 %.4f\")\n", citoh, epson);
}

// ── 11. ESC N / ESC C cannot make every line feed eject ──────────────────
//
// The C. Itoh `ESC H 0000` case is already guarded (a zero-length page ejected
// on every single line feed and blew the sheet stack away in blanks). The
// ESC/P siblings had the same hole: `ESC N n` past the form length collapsed
// the bottom margin to zero, and `ESC C` clamped to the 69" paper maximum
// rather than the mounted sheet, so a longer form silently clipped its ink.
void testEpsonFormLengthGuards()
{
    using pom2::IwModel;

    auto ejectsAfterTenFeeds = [](const std::vector<uint8_t>& setup) {
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(IwModel::EpsonFX80);
        if (!setup.empty()) iw.printBytes(setup.data(), setup.size());
        const uint8_t lf = 0x0A;
        for (int i = 0; i < 10; ++i) iw.printBytes(&lf, 1);
        return iw.sheetsEjected();
    };

    // A legal skip-over-perforation leaves a printable page.
    assert(ejectsAfterTenFeeds({ 0x1B, 'N', 6 }) == 0);
    // An n at or past the form length is out of spec; real firmware ignores
    // it. Accepting it ejected one blank sheet per feed.
    assert(ejectsAfterTenFeeds({ 0x1B, 'N', 80 }) == 0);

    // ESC C with a form longer than the mounted sheet: clamped to the sheet,
    // so the page still ejects instead of running the head off the raster and
    // dropping the ink.
    {
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(IwModel::EpsonFX80);
        const uint8_t escC14in[] = { 0x1B, 'C', 0, 14 };   // n=0 → inches follow
        iw.printBytes(escC14in, sizeof(escC14in));
        const uint8_t lf = 0x0A;
        for (int i = 0; i < 80; ++i) iw.printBytes(&lf, 1);
        assert(iw.sheetsEjected() >= 1 &&
               "a 14\" form on a Letter sheet never ejected");
    }

    std::printf("  ok: ESC N / ESC C are clamped to the mounted sheet\n");
}

} // namespace

// ── A dump wider than the paper is CROPPED, never wrapped or re-timed ────
//
// POM2's own screen dump is the in-tree producer of long `ESC G` runs: an
// 80-column screen is 560 columns at 72 dpi = 7.78 in, which is wider than
// ISO B5's 6.93 in page. Bug hunt 8 round 2 made `printBitGraph` stop the
// carriage at the right margin (it used to walk arbitrarily far off the sheet
// and charge mechanism time for columns `fillDots` was already discarding), so
// this is the case where that change is observable on real output.
//
// The property it must preserve — and the one that fails the moment the excess
// is wrapped to the next line, shifted, or dropped a column early — is that
// the narrow page is the wide page CROPPED: same dots, same places, for every
// column B5 has room for.
void testDumpWiderThanPaperIsCropped()
{
    const int W = 560, H = 192;
    std::vector<uint32_t> fb(static_cast<size_t>(W) * H, kBlack);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (((x / 3) ^ (y / 5)) & 1)
                fb[static_cast<size_t>(y) * W + x] = kWhite;

    ScreenDumpOptions opt;
    std::vector<uint8_t> stream;
    pom2::buildScreenDumpImageWriter(fb.data(), W, H, W, opt, stream);

    auto printOn = [&](ImageWriter::PaperSize sz) {
        ImageWriter iw(144, sz);
        iw.setPowered(true);
        iw.setOnline(true);
        iw.queueBytes(stream.data(), stream.size());
        int ticks = 0;
        while ((iw.pendingBytes() || iw.pendingRepeats()) && ticks < 400000) {
            iw.tick(0.05);
            ++ticks;
        }
        assert(!iw.pendingBytes() && !iw.pendingRepeats());
        assert(iw.completedPageCount() >= 1);
        return iw.completedPage(0);
    };

    const ImageWriter::Page letter = printOn(ImageWriter::PaperSize::Letter);
    const ImageWriter::Page b5     = printOn(ImageWriter::PaperSize::B5);
    assert(letter.w > b5.w);            // the case is only interesting if so

    long lit = 0;
    const int rows = (letter.h < b5.h) ? letter.h : b5.h;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < b5.w; ++x) {
            const uint8_t a = letter.pix[static_cast<size_t>(y) * letter.w + x];
            const uint8_t b = b5.pix[static_cast<size_t>(y) * b5.w + x];
            assert(a == b && "narrow page must equal the wide page, cropped");
            if (b) ++lit;
        }
    assert(lit > 100000 && "the comparison has to be over real ink");

    std::printf("  ok: an over-wide dump crops (%ld lit dots identical across "
                "%d x %d)\n", lit, b5.w, rows);
}


// ── 13. ESC/P capability masking across the MX → RX → FX generations ─────
//
// The ESC/P lineage grew feature by feature, so a "second parser" is not the
// whole story: what a head ANSWERS differs per model. These pin the two
// consequences that actually reach paper.
void testEscPCapabilityMasking()
{
    using pom2::IwModel;

    // `ESC *` is FX-generation. On an MX-80 it is an unknown command, and the
    // point of the swallow path is that its DATA bytes must not print as a
    // screenful of text either.
    auto starDots = [](IwModel m) {
        // ESC * mode 5, 4 columns, all 8 wires lit.
        const uint8_t seq[] = { 0x1B, '*', 5, 0x04, 0x00,
                                0xFF, 0xFF, 0xFF, 0xFF };
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(m);
        iw.printBytes(seq, sizeof(seq));
        long ink = 0;
        for (uint8_t v : iw.currentPage().pix) if (v & 0x1F) ++ink;
        return ink;
    };
    const long fx = starDots(IwModel::EpsonFX80);
    const long mx = starDots(IwModel::EpsonMX80);
    assert(fx > 0);                 // the FX-80 draws the four columns
    assert(mx == 0);                // the MX-80 has no ESC * and eats the body
    std::printf("  ok: ESC * draws on FX-80 (%ld dots), swallowed whole on "
                "MX-80 (%ld)\n", fx, mx);

    // Graftrax-Plus is exactly the ESC L / italics / scripts upgrade, so the
    // same ESC L stream must print on one MX and vanish on the other. This is
    // the difference the ROM upgrade sold, expressed as one table bit.
    auto escLDots = [](IwModel m) {
        const uint8_t seq[] = { 0x1B, 'L', 0x04, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(m);
        iw.printBytes(seq, sizeof(seq));
        long ink = 0;
        for (uint8_t v : iw.currentPage().pix) if (v & 0x1F) ++ink;
        return ink;
    };
    const long plain    = escLDots(IwModel::EpsonMX80);
    const long graftrax = escLDots(IwModel::EpsonMX80Graftrax);
    assert(plain == 0);
    assert(graftrax > 0);
    std::printf("  ok: ESC L is the Graftrax upgrade (%ld → %ld dots)\n",
                plain, graftrax);

    // ESC K is the one graphics command EVERY ESC/P head has had since 1980,
    // so it must draw on the bare MX-80 too — otherwise "swallowed" would be
    // indistinguishable from "this head prints nothing".
    const uint8_t kSeq[] = { 0x1B, 'K', 0x04, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
    ImageWriter mxK;
    mxK.setSpeed(ImageWriter::Speed::Instant);
    mxK.setModel(IwModel::EpsonMX80);
    mxK.printBytes(kSeq, sizeof(kSeq));
    long kInk = 0;
    for (uint8_t v : mxK.currentPage().pix) if (v & 0x1F) ++kInk;
    assert(kInk > 0);
    std::printf("  ok: ESC K still draws on a bare MX-80 (%ld dots)\n", kInk);
}

// ── 14. A screen dump must be built for the head that is actually fitted ──
void testEpsonDumpCmdMatchesHead()
{
    using pom2::IwModel;
    // The 72-dpi ESC * dump on an MX-80 is the failure this guards: the head
    // drops the command and its data bytes land as text. Building the same
    // picture with ESC K instead has to put ink on the page.
    std::vector<uint32_t> px(64 * 16, 0xFF000000u);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 64; ++x)
            if (((x / 4) + (y / 4)) & 1) px[y * 64 + x] = 0xFFFFFFFFu;

    pom2::ScreenDumpOptions opt;
    opt.autoInvert = false;
    opt.invert     = false;
    opt.formFeed   = false;

    auto inkFor = [&](pom2::EpsonDumpCmd cmd) {
        std::vector<uint8_t> stream;
        pom2::buildScreenDumpEpson(px.data(), 64, 16, 64, opt, stream, cmd);
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(IwModel::EpsonMX80);
        iw.printBytes(stream.data(), stream.size());
        long ink = 0;
        for (uint8_t v : iw.currentPage().pix) if (v & 0x1F) ++ink;
        return ink;
    };
    const long viaStar = inkFor(pom2::EpsonDumpCmd::Star72);
    const long viaK    = inkFor(pom2::EpsonDumpCmd::K60);
    assert(viaStar == 0);
    assert(viaK > 0);
    std::printf("  ok: MX-80 dump needs ESC K (star=%ld dots, K=%ld)\n",
                viaStar, viaK);
}

// ── 15. The C. Itoh cousins share the core and differ where documented ───
void testCItohCousins()
{
    using pom2::IwModel;
    // The Prowriter 8510A is the mechanism Apple rebadged, so `ESC G`
    // graphics must behave identically to the DMP's — same parser, same
    // faces. What differs is the DMP's firmware gaps, expressed as its
    // ignored-ESC list.
    const uint8_t g[] = { 0x1B, 'G', '0', '0', '0', '4',
                          0xFF, 0xFF, 0xFF, 0xFF };
    auto dots = [&](IwModel m) {
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(m);
        iw.printBytes(g, sizeof(g));
        long ink = 0;
        for (uint8_t v : iw.currentPage().pix) if (v & 0x1F) ++ink;
        return ink;
    };
    const long dmp  = dots(IwModel::AppleDMP);
    const long pro  = dots(IwModel::Prowriter8510A);
    const long nec  = dots(IwModel::NecPc8023A);
    assert(dmp > 0 && pro == dmp && nec == dmp);
    std::printf("  ok: DMP / Prowriter 8510A / NEC PC-8023A share the C. Itoh "
                "graphics core (%ld dots each)\n", dmp);

    // Every model in the table must have a name and a usable std bank, or the
    // model combo in the panel shows a blank row and currentBank() derefs a
    // null. Cheap sweep, catches a half-added row.
    for (int i = 0; i < static_cast<int>(IwModel::Count); ++i) {
        const auto& p = pom2::iwModelProfile(static_cast<IwModel>(i));
        assert(p.name && p.name[0]);
        assert(p.stdFixed && p.stdProp);
        assert(p.draftCps > 0.0 && p.nlqCps > 0.0);
        assert(p.defaultCpi > 0.0);
        // Every ESC/P head POM2 carries has at least the skip-perforation
        // pair; a zero mask would mean a row was added without one.
        if (p.lineage == pom2::IwLineage::EscP) assert(p.escPFeatures != 0);
        // A non-ESC/P head must not carry ESC/P feature bits, which would
        // mean two columns got transposed in the table.
        if (p.lineage != pom2::IwLineage::EscP) assert(p.escPFeatures == 0);
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(static_cast<IwModel>(i));
        const uint8_t hello[] = { 'H', 'i', 0x0D, 0x0A };
        iw.printBytes(hello, sizeof(hello));   // must not crash on any head
    }
    std::printf("  ok: all %d heads have a complete profile and print\n",
                static_cast<int>(IwModel::Count));
}


// ── 16. The LaserWriter's Diablo 630 emulation ───────────────────────────
//
// A third grammar over the same mechanism. These pin the two things that
// make it a lineage rather than a capability mask: motion is by INDEX, and
// the same bytes mean different things than they do on the other two heads.
void testDiablo630()
{
    using pom2::IwModel;

    // ImageWriter is non-copyable (it owns a page raster), so the helper
    // measures in place and returns the numbers rather than the printer.
    struct Shot { double x = 0.0, y = 0.0; long ink = 0; };
    auto headAfter = [](IwModel m, const uint8_t* seq, size_t n) {
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(m);
        iw.printBytes(seq, n);
        Shot s;
        s.x = iw.status().headX;
        s.y = iw.status().headY;
        for (uint8_t v : iw.currentPage().pix) if (v & 0x1F) ++s.ink;
        return s;
    };

    // ESC RS n sets the HMI to (n-1)/120 in. n=13 → 12/120 = 1/10 in, so
    // five characters land exactly half an inch apart from the left margin.
    {
        const uint8_t seq[] = { 0x1B, 0x1E, 13, 'A', 'A', 'A', 'A', 'A' };
        const Shot s = headAfter(IwModel::LaserWriterDiablo, seq, sizeof(seq));
        const double moved = s.x - 0.25;      // 0.25" default left edge
        assert(std::fabs(moved - 0.5) < 1e-6);
        std::printf("  ok: ESC RS 13 gives a 1/10\" HMI (5 chars = %.4f\")\n",
                    moved);
    }

    // ESC US n sets the VMI to (n-1)/48 in. n=9 → 8/48 = 1/6 in per line.
    {
        const uint8_t seq[] = { 0x1B, 0x1F, 9, 'X', 0x0D, 0x0A,
                                             'X', 0x0D, 0x0A, 'X' };
        const Shot s = headAfter(IwModel::LaserWriterDiablo, seq, sizeof(seq));
        assert(std::fabs(s.y - 2.0 / 6.0) < 1e-6);
        std::printf("  ok: ESC US 9 gives a 1/6\" VMI (2 lines = %.4f\")\n",
                    s.y);
    }

    // A zero index must not be honoured: a VMI of 0 would print every line on
    // top of the last and never reach the bottom margin, so the page would
    // never eject and the job would spin.
    {
        const uint8_t seq[] = { 0x1B, 0x1F, 1, 'X', 0x0D, 0x0A, 'X', 0x0D, 0x0A };
        const Shot s = headAfter(IwModel::LaserWriterDiablo, seq, sizeof(seq));
        assert(s.y > 0.0);
        std::printf("  ok: a zero VMI is refused (y=%.4f\")\n", s.y);
    }

    // Backspace overstrike is how a daisywheel did bold: print, back up one
    // HMI, print again. The page model ORs ink, so the second pass must land
    // on the first — same x, and no more ink than one character's worth.
    {
        const uint8_t plain[] = { 'M' };
        const uint8_t over[]  = { 'M', 0x08, 'M' };
        const Shot a = headAfter(IwModel::LaserWriterDiablo, plain, sizeof(plain));
        const Shot b = headAfter(IwModel::LaserWriterDiablo, over,  sizeof(over));
        assert(a.ink > 0 && a.ink == b.ink);
        assert(std::fabs(a.x - b.x) < 1e-9);
        std::printf("  ok: BS overstrike lands on the same cell (%ld dots "
                    "either way)\n", a.ink);
    }

    // Tabs are set at the CURRENT position, not by number — the 630's own
    // idiom, and the reason ESC 2 (clear all) exists.
    {
        const uint8_t seq[] = { 'A', 'A', 'A', 'A', 0x1B, '1',   // tab here
                                0x0D,                            // back to left
                                0x09, 'Z' };                     // HT then print
        const Shot s = headAfter(IwModel::LaserWriterDiablo, seq, sizeof(seq));
        // Four cells at the power-on 10 cpi = 0.4", plus the left margin,
        // then one printed 'Z' cell past the stop.
        assert(std::fabs(s.x - (0.25 + 0.4 + 0.1)) < 1e-6);
        std::printf("  ok: ESC 1 sets a tab at the head (HT reached %.4f\")\n",
                    s.x);
    }

    // ESC P turns PROPORTIONAL SPACING on, and a proportional glyph advances
    // by its own escapement — which is quoted in the dot unit the pitch
    // command established, not in some absolute unit. The 630 emulation comes
    // up at the profile's fixed 1/80" unit and ESC P left it there, so every
    // escapement was divided by 80 instead of 144 and came out ~1.8× too
    // wide: about 5 cpi, a daisywheel typing in slow motion. ESC Q must hand
    // the unit back with the style.
    {
        const double kUnit = 144.0;      // the ESC p (10 cpi) proportional unit
        const double wM    = 17.0;       // kIw2StdProp escapements, in 1/144"
        const double wI    =  8.0;
        const uint8_t prop[] = { 0x1B, 'P', 'M', 'i' };
        const Shot s = headAfter(IwModel::LaserWriterDiablo, prop, sizeof(prop));
        const double moved = s.x - 0.25;                 // left margin
        assert(std::fabs(moved - (wM + wI) / kUnit) < 1e-6);
        // Not a fixed cell: 'M' and 'i' really did advance by different
        // amounts (the same two glyphs at 10 cpi would be exactly 0.2").
        assert(std::fabs(moved - 0.2) > 1e-3);
        std::printf("  ok: ESC P advances by the glyph's own escapement "
                    "(M+i = %.4f\")\n", moved);

        // ESC Q back to fixed: two cells at the power-on 10 cpi, exactly.
        const uint8_t back[] = { 0x1B, 'P', 0x1B, 'Q', 'M', 'i' };
        const Shot t = headAfter(IwModel::LaserWriterDiablo, back, sizeof(back));
        assert(std::fabs((t.x - 0.25) - 0.2) < 1e-6);
        std::printf("  ok: ESC Q restores the fixed pitch (%.4f\")\n",
                    t.x - 0.25);
    }

    // The grammars really are three. ESC E is UNDERLINE ON to a Diablo and
    // EMPHASIZED (bold) to an Epson, while on the C. Itoh family ESC E is
    // something else again — so the same four bytes must not produce the
    // same page. This is the assertion that justifies a third parser rather
    // than more flags on either existing one.
    {
        const uint8_t seq[] = { 0x1B, 'E', 'I', 'I' };
        long ink[3] = {0,0,0};
        const IwModel heads[3] = { IwModel::ImageWriterII, IwModel::EpsonFX80,
                                   IwModel::LaserWriterDiablo };
        for (int i = 0; i < 3; ++i)
            ink[i] = headAfter(heads[i], seq, sizeof(seq)).ink;
        // The Diablo underlines, so it must lay down strictly more ink than
        // the Epson, which only sets a style bit on the same two glyphs.
        assert(ink[2] > ink[1]);
        std::printf("  ok: ESC E differs across all three lineages "
                    "(C.Itoh %ld, ESC/P %ld, Diablo %ld dots)\n",
                    ink[0], ink[1], ink[2]);
    }

    // A reset arriving mid-command must not leave the collector armed — the
    // trap the ESC/P parser already documents. Here the reset is a model
    // switch (which power-cycles), and the next job must print in full.
    {
        ImageWriter iw;
        iw.setSpeed(ImageWriter::Speed::Instant);
        iw.setModel(IwModel::LaserWriterDiablo);
        const uint8_t halfCmd[] = { 0x1B, 0x1E };     // ESC RS, no parameter
        iw.printBytes(halfCmd, sizeof(halfCmd));
        iw.setModel(IwModel::ImageWriterII);
        iw.setModel(IwModel::LaserWriterDiablo);
        const uint8_t job[] = { 'H', 'i' };
        iw.printBytes(job, sizeof(job));
        long ink = 0;
        for (uint8_t v : iw.currentPage().pix) if (v & 0x1F) ++ink;
        assert(ink > 0);
        std::printf("  ok: a reset mid-command does not eat the next job "
                    "(%ld dots)\n", ink);
    }
}

int main()
{
    testStreamShape();
    testRoundTripReproducesPattern();
    testBandsAbut();
    testAutoInvert();
    testDegenerate();
    testPowerAndOnline();
    testPaperDimensions();
    testEpsonRoundTrip();
    testEscPSlashCItohCollision();
    testEpsonCrLfDoesNotDoubleFeed();
    testEpsonFormLengthGuards();
    testDumpWiderThanPaperIsCropped();
    testEscPCapabilityMasking();
    testEpsonDumpCmdMatchesHead();
    testCItohCousins();
    testDiablo630();

    std::puts("printer_screen_dump: OK");
    return 0;
}
