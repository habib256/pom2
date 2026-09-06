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

// ImageWriter smoke test — pins the host-side Apple ImageWriter II printer
// (src/ImageWriter.cpp, ported from greg-kennedy/ImageWriter) plus the
// spool→printer seam the UI streams through.
//
// What is pinned, and why each matters:
//
//   1. Paper geometry — page raster = paper size (points/72) x page DPI.
//      Everything downstream (dot positions, PNG export) is in those units.
//   2. Text path — a glyph lays down ink; bit 7 is stripped so Apple II
//      output ("HELLO" with the high bit set, which is what COUT emits)
//      prints the same as plain ASCII; CR parks the head at the left
//      margin; LF advances by the line spacing.
//   3. Colour ribbon — ESC K selects a band, and overprinting two bands
//      ORs them into the correct mixed colour (magenta|yellow = red).
//      This is the encoding the whole page raster is built on.
//   4. Bit-image graphics — ESC G nnnn consumes exactly nnnn bytes as
//      dot columns and puts each column's 8 pins on paper at the pitch's
//      density. Screen dumps and Print Shop output are nothing but this.
//   5. Command framing — ESC R (repeat) expands without inflating the
//      byte odometer, and an unknown ESC command swallows only itself.
//   6. Paper handling — FF ejects onto the completed stack, a blank sheet
//      is not ejected by the FORM FEED button, and the stack is capped
//      (a guest that form-feeds in a loop must not exhaust host RAM).
//   7. Spool seam — PrinterCard::drainSpoolFrom hands over exactly the
//      bytes written since the previous poll, and replays from 0 after a
//      clearSpool() so the printer never goes silently deaf.
//   8. Eject invalidation — an eject reallocates the completed-sheet
//      vector and, at the cap, renames every index. The panel's ordering
//      discipline rests on that; this is where it is stated.
//   9. Repeat pacing — ESC R / ESC V / ESC U ask for up to 9999 units of
//      work from one input byte, and the mechanism budgets only ever look
//      between input bytes. The run must be resumable, not atomic.

#include "ImageWriter.h"
#include "PrinterCard.h"
#include "PrinterFeedCursor.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using pom2::ImageWriter;

namespace {

void feed(ImageWriter& iw, const char* s)
{
    iw.printBytes(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

size_t inkPixels(const ImageWriter::Page& p)
{
    size_t n = 0;
    for (uint8_t v : p.pix) if (v != 0) ++n;
    return n;
}

/// Ribbon band (top 3 bits) of the first inked pixel, or 0 if the page is
/// blank.
uint8_t firstBand(const ImageWriter::Page& p)
{
    for (uint8_t v : p.pix) if (v != 0) return static_cast<uint8_t>(v >> 5);
    return 0;
}

void testPaperGeometry()
{
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    assert(iw.pageWidth()  == static_cast<int>(8.5 * 144));   // 1224
    assert(iw.pageHeight() == static_cast<int>(11.0 * 144));  // 1584
    assert(iw.currentPageBlank());
    assert(iw.completedPageCount() == 0);

    iw.setPaperSize(ImageWriter::PaperSize::A4);
    assert(iw.pageWidth()  == static_cast<int>(595 / 72.0 * 144));
    assert(iw.pageHeight() == static_cast<int>(842 / 72.0 * 144));

    iw.setDpi(72);
    assert(iw.dpi() == 72);
    assert(iw.pageWidth() == static_cast<int>(595 / 72.0 * 72));

    // Out-of-range DPI clamps rather than producing a degenerate raster.
    iw.setDpi(10000);
    assert(iw.dpi() == ImageWriter::kMaxDpi);

    std::printf("  ok: paper geometry (size x DPI → raster)\n");
}

void testTextAndHighBit()
{
    ImageWriter plain(144, ImageWriter::PaperSize::Letter);
    feed(plain, "HELLO");
    const size_t plainInk = inkPixels(plain.currentPage());
    assert(plainInk > 0);
    assert(firstBand(plain.currentPage()) == 7);        // black ribbon

    // Apple II COUT sets bit 7 on every character; soft switch B-6 is open
    // by default, so the printer must mask it back off.
    ImageWriter hiBit(144, ImageWriter::PaperSize::Letter);
    const uint8_t msbHello[] = { 'H'|0x80, 'E'|0x80, 'L'|0x80, 'L'|0x80, 'O'|0x80 };
    hiBit.printBytes(msbHello, sizeof(msbHello));
    assert(hiBit.currentPage().pix == plain.currentPage().pix);

    // The "LF after CR" DIP switch defaults on — the Apple II never sends
    // an LF, so with it off every printout would overprint one line.
    assert(plain.autoFeed());
    {
        ImageWriter cr(144, ImageWriter::PaperSize::Letter);
        const double before = cr.status().headY;
        feed(cr, "A\r");
        assert(cr.status().headY > before);
        cr.resetPrinterHard();
        cr.setAutoFeed(false);
        const double flat = cr.status().headY;
        feed(cr, "A\r");
        assert(cr.status().headY == flat);
    }

    // CR returns the head to the left margin, LF advances one line.
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    iw.setAutoFeed(false);
    feed(iw, "AB");
    const double afterTwo = iw.status().headX;
    assert(afterTwo > 0.25);                            // moved right of margin
    feed(iw, "\r");
    assert(iw.status().headX == 0.25);                  // ImageWriter left margin
    const double y0 = iw.status().headY;
    feed(iw, "\n");
    const double y1 = iw.status().headY;
    assert(y1 > y0 && (y1 - y0) > 0.16 && (y1 - y0) < 0.17);   // 1/6 in

    // ESC B → 1/8 in spacing.
    feed(iw, "\x1b" "B\n");
    const double y2 = iw.status().headY;
    assert((y2 - y1) > 0.12 && (y2 - y1) < 0.13);

    std::printf("  ok: glyph ink, bit-7 strip, CR/LF, ESC A/B spacing\n");
}

void testColorRibbon()
{
    // ESC K 1 = yellow (band 4), ESC K 2 = magenta (band 1).
    // Auto-LF off so the CR below re-strikes the SAME line.
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    iw.setAutoFeed(false);
    feed(iw, "\x1b" "K1M");
    assert(firstBand(iw.currentPage()) == 4);

    // Overprint magenta on the same spot: the bands OR into red (band 5).
    feed(iw, "\r\x1b" "K2M");
    bool sawRed = false;
    for (uint8_t v : iw.currentPage().pix)
        if ((v >> 5) == 5) { sawRed = true; break; }
    assert(sawRed);

    feed(iw, "\x1b" "K0");
    assert(std::string(iw.status().colorName) == "black");

    // Palette: blank paper is white, full-intensity black band is black,
    // and the yellow band subtracts only blue.
    uint8_t r = 0, g = 0, b = 0;
    ImageWriter::indexToRgb(0, r, g, b);
    assert(r == 255 && g == 255 && b == 255);
    ImageWriter::indexToRgb(static_cast<uint8_t>((7 << 5) | 0x1F), r, g, b);
    assert(r == 0 && g == 0 && b == 0);
    ImageWriter::indexToRgb(static_cast<uint8_t>((4 << 5) | 0x1F), r, g, b);
    assert(r == 255 && g == 255 && b == 0);

    std::printf("  ok: ESC K ribbon bands + subtractive overprint + palette\n");
}

void testBitImageGraphics()
{
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    feed(iw, "\x1b" "E");                   // 12 cpi → 96 dpi graphics
    assert(iw.status().graphicsDpi == 96);

    feed(iw, "\x1b" "G0004");               // four dot columns follow
    assert(iw.status().inGraphics);
    for (int i = 0; i < 4; ++i) iw.printChar(0xFF);   // all 8 pins
    assert(!iw.status().inGraphics);

    // 4 columns x 8 pins, each dot covering 144/96 = 1.5 px horizontally
    // and 144/72 = 2 px vertically → a solid block, no gaps.
    const size_t ink = inkPixels(iw.currentPage());
    assert(ink >= 4 * 8 * 2);

    // A graphics byte must NOT be masked to 7 bits — bit 7 is pin 8.
    ImageWriter one(144, ImageWriter::PaperSize::Letter);
    feed(one, "\x1b" "E\x1b" "G0001");
    one.printChar(0x80);                    // bottom pin only
    assert(inkPixels(one.currentPage()) > 0);

    // …and the head advanced by exactly one dot at the active density.
    assert(one.status().headX > 0.25);
    assert(one.status().headX - 0.25 - (1.0 / 96.0) < 1e-9);

    // ESC C selects the LQ 3-byte column format (24 pins at 216 dpi).
    ImageWriter lq(144, ImageWriter::PaperSize::Letter);
    feed(lq, "\x1b" "E\x1b" "C0002");
    // …and it selects an LQ DENSITY: pitch index 2 | 8 = 10, which is
    // 192 dpi, not the 96 dpi of index 2. The status line read the density
    // out of a truncated 8-entry copy of the table with `printRes_ & 7`, so
    // every LQ pass reported its non-LQ twin.
    assert(lq.status().graphicsDpi == 192);
    for (int i = 0; i < 6; ++i) lq.printChar(0xFF);   // 2 columns x 3 bytes
    assert(!lq.status().inGraphics);
    assert(inkPixels(lq.currentPage()) > 0);

    std::printf("  ok: ESC G / ESC C bit images (8- and 24-pin columns)\n");
}

void testCommandFraming()
{
    // ESC R nnn c repeats one character; the odometer counts the 6 bytes
    // that actually crossed the cable, not the 8 characters printed.
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    feed(iw, "\x1b" "R008*");
    assert(iw.bytesReceived() == 6);
    const double x = iw.status().headX;
    assert(x - 0.25 - 8.0 / 12.0 < 1e-9 && x > 0.25);   // 8 chars at 12 cpi

    // An unrecognised ESC command swallows only its own command byte —
    // the text after it must still print.
    ImageWriter unk(144, ImageWriter::PaperSize::Letter);
    feed(unk, "\x1b\x01" "A");
    assert(inkPixels(unk.currentPage()) > 0);

    // Pitch commands move both the character width and the graphics density.
    ImageWriter pitch(144, ImageWriter::PaperSize::Letter);
    feed(pitch, "\x1b" "n"); assert(pitch.status().graphicsDpi == 72);
    feed(pitch, "\x1b" "N"); assert(pitch.status().graphicsDpi == 80);
    feed(pitch, "\x1b" "Q"); assert(pitch.status().graphicsDpi == 136);
    assert(pitch.status().cpi == 17.0);

    // Style bits are reflected in the status readout the panel shows.
    feed(pitch, "\x1b" "!");
    assert(pitch.status().styleText.find("bold") != std::string::npos);
    feed(pitch, "\x1b" "\"");
    assert(pitch.status().styleText.find("bold") == std::string::npos);

    std::printf("  ok: ESC R expansion, unknown-command framing, pitch/style\n");
}

void testPaperHandling()
{
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);

    // FORM FEED on a blank sheet must not eject (matches the real button).
    iw.formFeed();
    assert(iw.completedPageCount() == 0);

    feed(iw, "X");
    iw.formFeed();
    assert(iw.completedPageCount() == 1);
    assert(iw.currentPageBlank());
    assert(inkPixels(iw.completedPage(0)) > 0);

    // A guest FF ($0C) ejects too.
    feed(iw, "Y\x0c");
    assert(iw.completedPageCount() == 2);

    // The stack is capped: older sheets roll off and are counted.
    for (size_t i = 0; i < ImageWriter::kMaxPages + 5; ++i) feed(iw, "Z\x0c");
    assert(iw.completedPageCount() == ImageWriter::kMaxPages);
    assert(iw.droppedPageCount() == 7);         // 2 + 37 ejected, 32 kept

    iw.clearAll();
    assert(iw.completedPageCount() == 0);
    assert(iw.droppedPageCount() == 0);
    assert(iw.bytesReceived() == 0);
    assert(iw.currentPageBlank());

    std::printf("  ok: form feed, guest FF, page cap, clear all\n");
}

void testRgbaExport()
{
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);
    feed(iw, "\x1b" "K0#");

    std::vector<uint8_t> rgba;
    ImageWriter::pageToRgba(iw.currentPage(), rgba);
    const auto& p = iw.currentPage();
    assert(rgba.size() == static_cast<size_t>(p.w) * p.h * 4);

    bool sawBlackInk = false, sawWhitePaper = false;
    for (size_t i = 0; i < p.pix.size(); ++i) {
        const uint8_t* c = &rgba[i * 4];
        assert(c[3] == 255);                    // opaque everywhere
        if (c[0] == 0 && c[1] == 0 && c[2] == 0)             sawBlackInk   = true;
        if (c[0] == 255 && c[1] == 255 && c[2] == 255)       sawWhitePaper = true;
    }
    assert(sawBlackInk && sawWhitePaper);

    // The output is sized from w*h but the loop used to run over pix.size(),
    // so a Page whose pixel buffer is LONGER than its declared geometry wrote
    // past the end of the caller's vector — a heap overflow driven by the
    // page, not by the caller. Both directions are pinned here: an oversized
    // buffer stops at w*h, a short one stops at its own end (and leaves the
    // rest of the sheet untouched) instead of reading past it.
    {
        ImageWriter::Page big;
        big.w = 4; big.h = 2;
        big.pix.assign(4 * 2 * 10, 0x1F);          // ten times too many pixels
        std::vector<uint8_t> outBig;
        ImageWriter::pageToRgba(big, outBig);
        assert(outBig.size() == 4u * 2u * 4u);

        ImageWriter::Page shortP;
        shortP.w = 4; shortP.h = 2;
        shortP.pix.assign(3, 0x1F);                // fewer than w*h
        std::vector<uint8_t> outShort;
        ImageWriter::pageToRgba(shortP, outShort);
        assert(outShort.size() == 4u * 2u * 4u);
        for (size_t i = 3 * 4; i < outShort.size(); ++i)
            assert(outShort[i] == 0);              // never converted, never read
    }

    std::printf("  ok: page → RGBA export (and the w*h bound both ways)\n");
}

void testSpoolSeam()
{
    // The UI streams bytes card → printer with drainSpoolFrom(); this is
    // the exact sequence MainWindow::pumpImageWriter() performs, using the
    // same cursor helper it calls.
    PrinterCard card(1);
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);
    size_t      consumed = 0;
    const void* source   = nullptr;

    auto pump = [&]() {
        std::vector<uint8_t> fresh;
        consumed = pom2::printerFeedCursor(source, consumed,
                                           &card, card.bytesWritten());
        consumed = card.drainSpoolFrom(consumed, fresh);
        if (!fresh.empty()) iw.printBytes(fresh.data(), fresh.size());
        return fresh.size();
    };

    assert(pump() == 0);                        // nothing spooled yet

    card.deviceSelectWrite(1, 'A');
    card.deviceSelectWrite(1, 'B');
    assert(pump() == 2);
    assert(iw.bytesReceived() == 2);
    assert(pump() == 0);                        // no double delivery

    card.deviceSelectWrite(1, 'C');
    assert(pump() == 1);
    assert(iw.bytesReceived() == 3);

    // "Clear spool" in the Printer panel rewinds the card behind our back;
    // the next poll must resynchronise instead of going deaf forever.
    card.clearSpool();
    assert(pump() == 0);
    card.deviceSelectWrite(1, 'D');
    assert(pump() == 1);
    assert(iw.bytesReceived() == 4);

    // ─── Source handover ────────────────────────────────────────────────
    // Regression: the cursor was re-seated at 0 on any source change, so a
    // source whose spool outlives its source status re-delivered the whole
    // thing. The SSC printer tap is exactly that — its spool is never
    // cleared by the app, and the "Feed ImageWriter printer" checkbox is a
    // one-click toggle. Two SSCs is the //c's default layout (slot 1 =
    // printer port, slot 2 = modem port), so unticking slot 1 handed the
    // source to slot 2 and printed the entire modem transcript.
    //
    // Modelled here with a second card standing in for the other source:
    // what matters is that it arrives already holding bytes.
    {
        PrinterCard a(1), b(2);
        ImageWriter pr(72, ImageWriter::PaperSize::Letter);
        size_t      cur = 0;
        const void* src = nullptr;

        auto drain = [&](PrinterCard& c) {
            std::vector<uint8_t> fresh;
            cur = pom2::printerFeedCursor(src, cur, &c, c.bytesWritten());
            cur = c.drainSpoolFrom(cur, fresh);
            if (!fresh.empty()) pr.printBytes(fresh.data(), fresh.size());
            return fresh.size();
        };

        // Frame 1: A is the source and has spooled nothing yet — which is
        // what the real pump always sees, since it runs from the first
        // frame and the ImageWriter is built in the MainWindow ctor.
        assert(drain(a) == 0);

        // Source B spools a session's worth while A is the live source.
        for (char ch : std::string("MODEM TRANSCRIPT"))
            b.deviceSelectWrite(1, static_cast<uint8_t>(ch));

        a.deviceSelectWrite(1, 'X');
        assert(drain(a) == 1);
        assert(pr.bytesReceived() == 1);

        // Hand over to B: it must adopt B's backlog, not print it.
        assert(drain(b) == 0);
        assert(pr.bytesReceived() == 1);
        // …and still deliver what B spools from here on.
        b.deviceSelectWrite(1, 'Y');
        assert(drain(b) == 1);
        assert(pr.bytesReceived() == 2);

        // Hand back to A: same rule, and A's earlier 'X' is not replayed.
        assert(drain(a) == 0);
        assert(pr.bytesReceived() == 2);

        // A source dropping out entirely (card unplugged / tap unticked)
        // and coming back must not replay either. That is the off/on
        // toggle that reprinted whole jobs.
        src = nullptr;                      // the no-source frame
        assert(drain(a) == 0);
        assert(pr.bytesReceived() == 2);
    }

    std::printf("  ok: drainSpoolFrom streaming + resync + source handover\n");
}

void testMechanismPacing()
{
    // The card delivers a line in one frame; the mechanism prints it at
    // 250 cps draft, so the page must build up over several ticks.
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);
    iw.setSpeed(ImageWriter::Speed::Draft);

    const char* line = "HELLO WORLD";              // 11 chars ≈ 44 ms
    iw.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    assert(iw.busy());
    assert(iw.pendingBytes() == 11);
    assert(iw.bytesReceived() == 0);               // nothing printed yet

    // One 60 Hz frame buys 16.7 ms ≈ 4 characters — not the whole line.
    iw.tick(1.0 / 60.0);
    const uint64_t afterOneFrame = iw.bytesReceived();
    assert(afterOneFrame > 0 && afterOneFrame < 11);
    assert(iw.busy());

    // Ticking past the line's print time drains it exactly once.
    for (int i = 0; i < 10; ++i) iw.tick(1.0 / 60.0);
    assert(!iw.busy());
    assert(iw.pendingBytes() == 0);
    assert(iw.bytesReceived() == 11);

    // A tick with nothing queued must not bank credit that would later
    // dump a whole line in one frame.
    iw.tick(5.0);
    iw.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    iw.tick(1.0 / 60.0);
    assert(iw.busy());
    iw.flushPending();
    assert(!iw.busy());
    assert(iw.bytesReceived() == 22);

    // NLQ is the slow head: same line, more frames.
    ImageWriter nlq(72, ImageWriter::PaperSize::Letter);
    nlq.setSpeed(ImageWriter::Speed::NLQ);
    nlq.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    nlq.tick(1.0 / 60.0);
    assert(nlq.bytesReceived() < afterOneFrame);

    // Instant is the old behaviour — everything lands the moment it is
    // queued and ticked, and switching to it never strands a job.
    ImageWriter fast(72, ImageWriter::PaperSize::Letter);
    fast.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    fast.setSpeed(ImageWriter::Speed::Instant);
    assert(!fast.busy());
    assert(fast.bytesReceived() == 11);

    // Power-cycling the printer throws the input buffer away with it.
    ImageWriter off(72, ImageWriter::PaperSize::Letter);
    off.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    off.resetPrinterHard();
    assert(!off.busy() && off.pendingBytes() == 0);
    off.tick(1.0);
    assert(off.bytesReceived() == 0);

    std::printf("  ok: mechanism pacing (draft / NLQ / instant / reset)\n");
}

void testCommandBoundsAndTabs()
{
    // Six defects found by auditing the port against the ImageWriter II
    // Technical Reference rather than only against greg-kennedy's
    // imagewriter.cpp. Four are inherited reference bugs that still put
    // wrong ink on paper, so POM2 deviates deliberately — the kind of
    // divergence CLAUDE.md asks to be commented at the site and pinned.
    const double kChar = 1.0 / 12.0;           // 12 cpi default cell

    // 1. HT goes to the NEAREST stop right of the head, not the farthest.
    //    Reference (imagewriter.cpp:1131-1141) keeps overwriting as it
    //    scans, so the first TAB jumped to the LAST stop and every later
    //    one was a no-op — every columnar report came out as one column.
    {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        feed(iw, "\x1B(010,020,030.");         // stops at 10, 20, 30 chars
        assert(std::abs(iw.status().headX - 0.25) < 1e-9);   // left margin
        feed(iw, "\t");
        assert(std::abs(iw.status().headX - 10 * kChar) < 1e-9);
        feed(iw, "\t");
        assert(std::abs(iw.status().headX - 20 * kChar) < 1e-9);
        feed(iw, "\t");
        assert(std::abs(iw.status().headX - 30 * kChar) < 1e-9);
    }

    // 2. ESC 1..6 ADDS n/120" of intercharacter space; it is not an
    //    absolute head position. The reference assigns curX_ = n/unit, so
    //    `ESC 3` mid-line threw the head from 1.25" back to 0.02" and
    //    destroyed every justified line a proportional driver produced.
    {
        // Baseline: what one 'X' advances by in proportional mode with no
        // extra intercharacter space. NOT hardcoded — since the character
        // ROMs landed, a proportional advance is the glyph's own escapement
        // (see docs/printer_plan.md phase A), so pinning a number here would
        // pin the font data rather than this command's behaviour.
        double plain = 0.0;
        {
            ImageWriter iw(144, ImageWriter::PaperSize::Letter);
            feed(iw, "\x1B" "p");
            feed(iw, "HELLO WORLD");
            const double b = iw.status().headX;
            feed(iw, "X");
            plain = iw.status().headX - b;
            assert(plain > 0.0);
        }

        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        feed(iw, "\x1B" "p");                  // proportional, 10 cpi
        feed(iw, "HELLO WORLD");
        const double before = iw.status().headX;
        feed(iw, "\x1B" "3");
        assert(iw.status().headX == before);   // the command itself moves nothing
        feed(iw, "X");
        const double advance = iw.status().headX - before;
        assert(advance > 0.0);                 // forward, never backward
        // THE property: ESC 3 ADDED exactly 3/120", it did not replace the
        // advance with an absolute position.
        assert(std::abs(advance - (plain + 3.0 / 120.0)) < 1e-9);
    }

    // 3. `ESC c` must not silently destroy the sheet on the platen. The
    //    reference could discard it (imagewriter.cpp:315) because it wrote
    //    every page to disk as it went; here the sheet exists nowhere
    //    else, so a short report with no trailing FF vanished the moment
    //    the next program sent its init.
    {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        feed(iw, "IMPORTANT REPORT");
        assert(!iw.currentPageBlank());
        assert(iw.completedPageCount() == 0);
        feed(iw, "\x1B" "c");
        assert(iw.completedPageCount() == 1);  // ejected, not binned
        assert(iw.currentPageBlank());         // fresh sheet on the platen

        // …but `ESC c` on a blank platen must not eject a blank sheet,
        // the same rule the FORM FEED button follows.
        feed(iw, "\x1B" "c");
        assert(iw.completedPageCount() == 1);
    }

    // 4. `ESC H 0000` — a zero-length page made every LF eject, so three
    //    line feeds produced three sheets and a real job rolled the whole
    //    32-page stack away in blanks.
    {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        feed(iw, "\x1B" "H0000");
        feed(iw, "X\nY\nZ\n");
        assert(iw.completedPageCount() == 0);
    }

    // 5. `ESC L` clamps to the sheet. 999 put the margin 83" out and every
    //    page came out blank; 000 gave a negative margin that clipped the
    //    first character. Both silent.
    {
        ImageWriter wide(144, ImageWriter::PaperSize::Letter);
        feed(wide, "\x1B" "L999");
        feed(wide, "HELLO");
        assert(inkPixels(wide.currentPage()) > 0);      // still on the paper

        ImageWriter zero(144, ImageWriter::PaperSize::Letter);
        feed(zero, "\x1B" "L000");
        assert(zero.status().headX >= 0.0);
        feed(zero, "HELLO");
        assert(inkPixels(zero.currentPage()) > 0);
    }

    // 6. The stall watchdog must not fire on a byte that is merely SLOW.
    //    `ESC H 9999` + FF is honest mechanism time, and a flat 10 s cap
    //    cut it short and logged a STALL for a printer working correctly.
    //    (ESC H now clamps too, so this also checks the two interact.)
    {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        feed(iw, "\x1B" "H9999");
        const uint8_t ff = 0x0C;
        iw.queueBytes(&ff, 1);
        int ticks = 0;
        while (iw.busy() && ticks < 60 * 60) { iw.tick(1.0 / 60.0); ++ticks; }
        assert(!iw.busy());
        // Letter is 11" of transport at 5 ips = 2.2 s, well inside the
        // watchdog — the byte was affordable, not forced.
        assert(ticks / 60.0 < 9.0);
    }

    std::printf("  ok: HT/VT nearest stop, ESC 1-6 relative, ESC c keeps the "
                "sheet, ESC H/L clamped\n");
}

void testBoundedCatchUp()
{
    // Regression: past a 1 MiB backlog, queueBytes() called flushPending()
    // and printed the WHOLE backlog synchronously — on the UI thread, from
    // pumpImageWriter(). Measured from a real 6502: 0.6 s for an Applesoft
    // print loop, 5.4 s for random binary, 119 s for a form-feed storm,
    // all inside one frame. The credit cap in tick() bounds credited
    // *seconds*; it never bounded the *work*.
    //
    // The contract now: queueBytes() never prints, and each tick does a
    // bounded slice — capped in bytes AND in sheet ejects, because an
    // eject copies a whole page raster and so is orders of magnitude
    // dearer than a glyph.
    const size_t kOver = (1u << 20) + 4096;        // just past kMaxBacklog

    // 1. A 1 MiB+ dump of plain text: queueBytes prints nothing at all,
    //    and no single tick drains it either.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        const std::vector<uint8_t> blob(kOver, 'X');
        iw.queueBytes(blob.data(), blob.size());
        assert(iw.bytesReceived() == 0);           // NOT printed on the wire
        assert(iw.pendingBytes() == kOver);
        assert(iw.catchingUp());

        iw.tick(1.0 / 60.0);
        const uint64_t afterOne = iw.bytesReceived();
        assert(afterOne > 0);                      // it does make progress…
        assert(afterOne < kOver);                  // …but not all of it
        assert(afterOne <= (16u << 10));           // within the byte budget
    }

    // 2. A form-feed storm — the 119 s case. Every one-byte FF asks for a
    //    whole sheet, so the sheet budget, not the byte budget, is what
    //    has to bite. Bound ejects per tick, or one frame copies hundreds
    //    of page rasters.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        std::vector<uint8_t> ffs(kOver);
        for (size_t i = 0; i < ffs.size(); ++i)
            ffs[i] = (i % 2) ? 0x0C : 'A';         // ink, eject, ink, eject…
        iw.queueBytes(ffs.data(), ffs.size());
        assert(iw.completedPageCount() == 0);      // queueBytes ejected none

        const size_t before = iw.completedPageCount() + iw.droppedPageCount();
        iw.tick(1.0 / 60.0);
        const size_t ejected =
            iw.completedPageCount() + iw.droppedPageCount() - before;
        assert(ejected > 0);
        assert(ejected <= 4);                      // the per-tick sheet cap
    }

    // 3. Memory stays bounded even against a guest that outruns the
    //    catch-up rate. Dropping input truncates a printout, which is bad
    //    — but it is counted and traced, and the alternative (unbounded
    //    heap, or an unbounded synchronous flush) is worse.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        const std::vector<uint8_t> blob(1u << 20, 'Y');
        for (int i = 0; i < 8; ++i) iw.queueBytes(blob.data(), blob.size());
        assert(iw.pendingBytes() <= (4u << 20));
        assert(iw.droppedInputBytes() > 0);
    }

    // 4. Catch-up disarms once the backlog is cleared, so the printer goes
    //    back to Draft/NLQ pacing instead of staying flat out forever.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        const std::vector<uint8_t> blob(kOver, 'Z');
        iw.queueBytes(blob.data(), blob.size());
        assert(iw.catchingUp());
        for (int i = 0; i < 400 && iw.busy(); ++i) iw.tick(1.0 / 60.0);
        assert(!iw.busy());
        assert(!iw.catchingUp());

        // Back to paced: one frame buys ~4 characters, not the line.
        const char* line = "HELLO WORLD";
        iw.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
        const uint64_t mark = iw.bytesReceived();
        iw.tick(1.0 / 60.0);
        assert(iw.bytesReceived() - mark < 11);
    }

    std::printf("  ok: backlog catch-up is bounded per tick (bytes, sheets, "
                "memory)\n");
}

void testAutoLineFeedDetection()
{
    // The SW A-8 question — feed on CR or not — has three right answers
    // depending on who is sending, and Auto settles it from the stream.
    // Regression: with the printer always feeding, Print Shop's colour
    // passes (separated by a bare CR so they overprint) marched down the
    // page as a coloured staircase instead of landing on one line.
    const double kLine = 1.0 / 6.0;

    // 1. Bare CR (a plain PR#n : PRINT) — the printer must feed, or the
    //    whole listing overprints one line.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        assert(iw.autoFeedMode() == ImageWriter::AutoFeed::Auto);
        assert(iw.autoFeedActive());
        feed(iw, "A\r");
        assert(std::abs(iw.status().headY - kLine) < 1e-9);
        feed(iw, "B\r");
        assert(std::abs(iw.status().headY - 2 * kLine) < 1e-9);
        assert(!iw.autoFeedLatchedOff());
    }

    // 2. CR+LF (every real driver, and the Grappler+ firmware) — one
    //    advance per line, not two, and the switch latches off.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        feed(iw, "A\r\n");
        assert(std::abs(iw.status().headY - kLine) < 1e-9);
        assert(iw.autoFeedLatchedOff());
        feed(iw, "B\r\n");
        assert(std::abs(iw.status().headY - 2 * kLine) < 1e-9);

        // 3. …and once latched, a bare CR overprints instead of feeding,
        //    which is what a colour pass needs.
        const double y = iw.status().headY;
        feed(iw, "\rC");
        assert(std::abs(iw.status().headY - y) < 1e-9);
        assert(std::abs(iw.status().headX - 0.25) > 1e-9);   // it did print
    }

    // 4. An LF that is NOT preceded by a CR still feeds.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        feed(iw, "\n");
        assert(std::abs(iw.status().headY - kLine) < 1e-9);
        assert(!iw.autoFeedLatchedOff());
    }

    // 5. Pinning the switch by hand still wins over the detector.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setAutoFeedMode(ImageWriter::AutoFeed::On);
        feed(iw, "A\r\n");                     // CR feeds, LF swallowed
        assert(std::abs(iw.status().headY - kLine) < 1e-9);
        assert(iw.autoFeedActive());           // stays on: not Auto

        ImageWriter off(72, ImageWriter::PaperSize::Letter);
        off.setAutoFeedMode(ImageWriter::AutoFeed::Off);
        feed(off, "A\r");
        assert(off.status().headY == 0.0);     // never feeds on CR
    }

    // 6. A power cycle re-arms the detector for the next job.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        feed(iw, "A\r\n");
        assert(iw.autoFeedLatchedOff());
        iw.resetPrinterHard();
        assert(!iw.autoFeedLatchedOff());
        assert(iw.autoFeedActive());
    }

    // 7. `ESC c` (initialize printer) re-arms it too — and it is the only
    //    thing a GUEST can send that does. Regression: the latch used to
    //    survive every reset the guest could reach, so it was scoped to
    //    the host session rather than the job. Job 1 in the CR+LF
    //    convention latched CR "don't feed"; job 2 sending bare CRs (a
    //    plain `PR#n : LIST`) then printed its whole listing overprinted
    //    onto one black line, unrecoverable from inside the guest.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        feed(iw, "REPORT\r\nTOTAL\r\n");           // job 1: CR+LF driver
        assert(iw.autoFeedLatchedOff());

        feed(iw, "\x1B" "c");                      // job 2 announces itself
        assert(!iw.autoFeedLatchedOff());
        assert(iw.autoFeedActive());

        const double y0 = iw.status().headY;
        feed(iw, "10 PRINT\r");                    // bare CR, as LIST emits
        assert(std::abs(iw.status().headY - (y0 + kLine)) < 1e-9);
    }

    // 8. …but a bare CR WITHOUT `ESC c` must still overprint. This is the
    //    guard on case 7: Print Shop separates its yellow/cyan/magenta
    //    passes with a bare CR and never sends `ESC c`, so re-arming on
    //    anything looser would march its colour passes down the page as a
    //    staircase — the exact bug the detector exists to prevent.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        feed(iw, "PASS1\r\n");
        assert(iw.autoFeedLatchedOff());
        const double y = iw.status().headY;
        feed(iw, "\rPASS2");                       // colour pass 2
        assert(std::abs(iw.status().headY - y) < 1e-9);
        assert(iw.autoFeedLatchedOff());
    }

    std::printf("  ok: line-feed-after-CR detection (bare CR / CR+LF / "
                "overprint / ESC c re-arm)\n");
}

void testNoUnaffordableByte()
{
    // Regression: the credit cap was a flat 1 s, but a form feed near the
    // top of a Letter sheet costs 2.2 s of paper transport — so that byte
    // could never be afforded, the queue stalled forever, and (with BUSY
    // wired back to the card) the guest hung in its firmware ACK loop.
    // Print Shop froze on every page eject.
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);
    iw.setSpeed(ImageWriter::Speed::Draft);
    const uint8_t job[] = { 'H', 'I', 0x0C };       // two chars, then FF
    iw.queueBytes(job, sizeof job);
    for (int f = 0; f < 600; ++f) iw.tick(1.0 / 60.0);   // 10 s of frames
    assert(!iw.busy());
    assert(iw.pendingBytes() == 0);
    assert(iw.completedPageCount() == 1);          // the sheet came out

    // Same for the NLQ carriage return, whose slew from the right margin
    // is also longer than the old cap.
    ImageWriter nlq(72, ImageWriter::PaperSize::Ledger);   // 11 in wide
    nlq.setSpeed(ImageWriter::Speed::NLQ);
    const uint8_t wide[] = { 0x0C, 0x0D };
    nlq.queueBytes(wide, sizeof wide);
    for (int f = 0; f < 900; ++f) nlq.tick(1.0 / 60.0);
    assert(!nlq.busy());

    // And the belt-and-braces watchdog: whatever the cost model says, a
    // byte may not sit unprinted forever. Paper the size of a barn door
    // makes the form feed cost far more than any cap.
    ImageWriter big(72, ImageWriter::PaperSize::A3);
    big.setSpeed(ImageWriter::Speed::NLQ);
    const uint8_t ff[] = { 'X', 0x0C };
    big.queueBytes(ff, sizeof ff);
    for (int f = 0; f < 60 * 60; ++f) big.tick(1.0 / 60.0);   // 60 s
    assert(!big.busy());

    std::printf("  ok: no byte is ever unaffordable (form feed / NLQ CR)\n");
}

void testTraceClosedOnDestruction()
{
    // Regression: the printer owned the trace `FILE*` but had no
    // destructor, and `stopTrace()` was only ever reached from the
    // panel's checkbox. Quitting while tracing therefore stranded the
    // partial hex row inside `traceRow_` — it had never reached stdio, so
    // the C runtime's exit flush could not save it — and the file ended
    // with no footer. On the `POM2_TRACE_PRINTER=1` path (the one used to
    // capture a trace for a bug report) that meant every trace ended
    // truncated, with no way to tell it apart from one cut short by a
    // crash.
    const std::string path = "imagewriter_trace_dtor_test.log";
    std::remove(path.c_str());

    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        std::string err;
        assert(iw.startTrace(path, err));
        assert(iw.tracing());
        // Fewer than the 16 bytes that force a row flush, so the whole
        // row is still buffered when the printer goes away.
        feed(iw, "HI");
    }   // ← destructor: this is what used to lose the row

    std::FILE* f = std::fopen(path.c_str(), "rb");
    assert(f);
    std::string body;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
    std::fclose(f);
    std::remove(path.c_str());

    // The buffered row reached the file...
    assert(body.find("48 49") != std::string::npos);   // 'H' 'I'
    assert(body.find("|HI|") != std::string::npos);
    // ...and the trace says it ended on purpose.
    assert(body.find("# trace closed after 2 bytes") != std::string::npos);

    std::printf("  ok: trace flushed + closed when the printer is destroyed\n");
}

// 12. Parser hardening (bug-hunt 2026-07-28). Three ways a hostile or
//     corrupted stream used to wedge or corrupt the parser:
//       a) a non-digit inside an ESC G count went negative and the
//          uint32_t cast turned it into ~4 G bytes of graphics data —
//          the printer went deaf for the rest of the session;
//       b) ESC ' / ESC I left the previous command's parameter count
//          armed, so the next 1-6 printable characters were swallowed;
//       c) ESC r (reverse feed) + LFs walked the head to negative Y.
void testParserHardening()
{
    // a) ESC G with a corrupted digit must not wedge in graphics mode.
    {
        ImageWriter iw;
        feed(iw, "\x1BG0-10");                 // '-' is not a digit
        // Worst case the clamped count (0x0y10-ish) eats a few bytes —
        // definitely not 4 billion. Feed a small payload then text.
        for (int i = 0; i < 512; ++i) { const uint8_t b = 0xFF; iw.printBytes(&b, 1); }
        assert(!iw.status().inGraphics);
        feed(iw, "TEXT\r");
        assert(inkPixels(iw.currentPage()) > 0);
    }

    // b) ESC I right after a parametered command must not eat text.
    //    Identical streams except for the (unsupported, zero-parameter)
    //    ESC I must produce identical ink — the stale parameter count
    //    used to swallow "HELL".
    {
        ImageWriter iw, ref;
        feed(iw,  "\x1BG0004\xFF\xFF\xFF\xFF\x1BIHELLO\r");
        feed(ref, "\x1BG0004\xFF\xFF\xFF\xFF"     "HELLO\r");
        assert(inkPixels(iw.currentPage()) == inkPixels(ref.currentPage()));
    }

    // c) Reverse feed clamps at the top edge and keeps paying (positive)
    //    paper-transport cost.
    {
        ImageWriter iw;
        feed(iw, "X\r\n\x1Br");                // one line down, reverse
        for (int i = 0; i < 50; ++i) feed(iw, "\n");
        assert(iw.status().headY >= 0.0);
    }

    std::printf("  ok: corrupted counts, ESC I framing and reverse feed "
                "are all bounded\n");
}

void testEjectInvalidatesSheetReferences()
{
    // Regression (heap use-after-free, confirmed under AddressSanitizer).
    // The ImageWriter panel bound
    //
    //     const Page& page = (shown < nDone) ? iw.completedPage(shown)
    //                                        : iw.currentPage();
    //
    // near the top of the frame and then, further down the SAME frame, its
    // "Print now" button called iw.flushPending() before handing `page` to
    // uploadPage(). Whatever the queue held, that call can eject, and an
    // eject invalidates the binding two different ways:
    //
    //   * newPage() push_back()s into pages_, which REALLOCATES — the
    //     reference dangles and uploadPage() read freed memory;
    //   * at the 32-sheet cap it first erase()s the front, which shifts
    //     without reallocating — nothing dangles, but every index then
    //     names a different sheet, so `shown`, `nDone` and the panel's
    //     texture cache key (droppedPageCount() + shown) stop agreeing.
    //
    // The panel is GL + Dear ImGui and cannot be driven headlessly, so what
    // is pinned here is the printer-side fact its ordering discipline
    // exists for. The discipline itself — mutate first, re-resolve after,
    // never hold a Page reference across a call into the printer — lives
    // next to the `Selection` helper in ImageWriter_ImGui.cpp.
    const uint8_t job[] = { 'Z', 0x0C };       // one inked line, then eject

    // 1. Ejecting reallocates: a reference taken before flushPending() does
    //    not survive it. The address is captured as an integer while it is
    //    still valid, so the comparison itself stays well defined.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        feed(iw, "A\x0C");
        assert(iw.completedPageCount() == 1);

        const auto first = reinterpret_cast<uintptr_t>(&iw.completedPage(0));
        size_t ejects = 0;
        while (reinterpret_cast<uintptr_t>(&iw.completedPage(0)) == first &&
               ejects < 8) {
            iw.queueBytes(job, sizeof job);
            iw.flushPending();                 // ← what the button calls
            ++ejects;
        }
        assert(ejects > 0);
        assert(reinterpret_cast<uintptr_t>(&iw.completedPage(0)) != first);
    }

    // 2. At the cap the stack shifts instead of growing: the sheet an index
    //    names changes underneath the caller, and so does the cache key,
    //    since droppedPageCount() moves with it. Sheets are told apart by
    //    how much ink they carry.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        for (size_t i = 0; i < ImageWriter::kMaxPages; ++i) {
            for (size_t c = 0; c <= i; ++c) feed(iw, "X");
            feed(iw, "\x0C");
        }
        assert(iw.completedPageCount() == ImageWriter::kMaxPages);
        assert(iw.droppedPageCount() == 0);

        const int    shown  = 0;               // the sheet the panel shows
        const size_t inkWas = inkPixels(iw.completedPage(shown));
        const size_t keyWas = iw.droppedPageCount() + shown;

        iw.queueBytes(job, sizeof job);
        iw.flushPending();                     // ← the button again

        assert(iw.completedPageCount() == ImageWriter::kMaxPages);
        assert(iw.droppedPageCount() == 1);
        assert(iw.droppedPageCount() + shown != keyWas);        // key moved…
        assert(inkPixels(iw.completedPage(shown)) != inkWas);    // …so did the
    }                                                            //   sheet

    std::printf("  ok: an eject invalidates both sheet references and sheet "
                "indices\n");
}

void testRepeatIsPacedNotAtomic()
{
    // Regression: `ESC R nnn c` expanded all nnn copies inside the single
    // input byte that completed the sequence — and `c` may itself be a form
    // feed. Neither catch-up budget can bound that (both are only checked
    // BETWEEN input bytes), and byteCost charged the terminating byte
    // nothing, because `numParam_ < neededParam_` still holds on it, so the
    // sequence ran free of credit on the paced path too.
    //
    // Measured for `PR#1 : PRINT CHR$(27);"R999";CHR$(12)`: 773 ms inside
    // ONE tick() at Letter/144 dpi and 13.8 s at Ledger/288 dpi, with 999
    // sheets ejected and 967 dropped — the user's whole tray gone in the
    // same frame, worse than the freeze the catch-up budgets were added to
    // fix.
    //
    // The contract: a repeat run is resumable state paced by byteCost like
    // any other printing, and every one of the 999 sheets still comes out.
    // Clamping the count is not a fix — a real printer prints 999
    // characters, and truncating a valid job is silent output corruption.
    const uint8_t job[] = { 0x1B, 'R', '9', '9', '9', 0x0C };

    // 1. Paced path. Six bytes is far under kMaxBacklog, so catch-up never
    //    arms and the credit cap is the only thing bounding the work.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        iw.queueBytes(job, sizeof job);
        assert(!iw.catchingUp());

        // One 60 Hz frame buys 16.7 ms and a Letter form feed is 2.2 s of
        // paper transport, so the frame that parses the command ejects
        // nothing at all — and still owes the run afterwards.
        iw.tick(1.0 / 60.0);
        assert(iw.completedPageCount() + iw.droppedPageCount() == 0);
        assert(iw.busy());
        assert(iw.pendingBytes() == 0);        // the six bytes are consumed…
        assert(iw.bytesReceived() == 6);       // …and the odometer is honest

        size_t ejected = 0, worstTick = 0;
        int    ticks   = 0;
        while (iw.busy() && ticks < 20000) {
            const size_t before =
                iw.completedPageCount() + iw.droppedPageCount();
            iw.tick(1.0);                      // a whole second at a time
            ejected = iw.completedPageCount() + iw.droppedPageCount();
            if (ejected - before > worstTick) worstTick = ejected - before;
            ++ticks;
        }
        assert(!iw.busy());
        assert(ejected == 999);                // every sheet that was asked
        assert(worstTick <= 2);                // …one or two per second of
        assert(ticks > 500);                   //   mechanism time, not 999
        assert(iw.bytesReceived() == 6);       // still six bytes on the wire
    }

    // 2. Catch-up path: the same command buried in a backlog past
    //    kMaxBacklog, where Draft/NLQ pacing is suspended. The per-tick
    //    SHEET budget has to bite on a repeat exactly as it does on a
    //    stream of form feeds.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        std::vector<uint8_t> blob((1u << 20) + 4096, 'X');
        std::memcpy(blob.data(), job, sizeof job);
        iw.queueBytes(blob.data(), blob.size());
        assert(iw.catchingUp());

        for (int i = 0; i < 8; ++i) {
            const size_t before =
                iw.completedPageCount() + iw.droppedPageCount();
            iw.tick(1.0 / 60.0);
            const size_t after =
                iw.completedPageCount() + iw.droppedPageCount();
            assert(after - before <= 4);       // kCatchUpSheets
        }
    }

    // 3. `ESC V nnnn c` / `ESC U nnnn abc` are the same shape at ten times
    //    the count. They eject nothing (a dot column never moves paper),
    //    but a backlog of them cost 1.4 s in ONE catch-up tick against
    //    9 ms for the same backlog of plain text, because every column ran
    //    free inside one input byte. Routed through the same resumable
    //    state they are ordinary bit-image bytes: paced, and priced at the
    //    dot-column rate.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        const uint8_t escU[] = { 0x1B, 'U', '9','9','9','9', 0xFF,0xFF,0xFF };
        iw.queueBytes(escU, sizeof escU);

        iw.tick(1.0 / 60.0);
        assert(iw.busy());
        assert(iw.status().inGraphics);        // most of the run still owed
        assert(iw.bytesReceived() == 9);

        int ticks = 0;
        while (iw.busy() && ticks < 20000) { iw.tick(1.0); ++ticks; }
        assert(!iw.busy());
        assert(!iw.status().inGraphics);       // all 9999 columns laid down
        assert(iw.completedPageCount() + iw.droppedPageCount() == 0);
        assert(inkPixels(iw.currentPage()) > 0);
        assert(iw.bytesReceived() == 9);
    }

    // 4. The immediate entry points stay atomic: no mechanism sits between
    //    the byte and the paper there, so `printBytes` (and "Print now",
    //    and Speed::Instant) must run a repeat out before returning.
    {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        feed(iw, "\x1B" "R008*");
        assert(iw.status().headX - 0.25 - 8.0 / 12.0 < 1e-9);
        assert(!iw.busy());

        ImageWriter now(72, ImageWriter::PaperSize::Letter);
        now.setSpeed(ImageWriter::Speed::Draft);
        now.queueBytes(job, sizeof job);
        now.tick(1.0 / 60.0);                  // parses, ejects nothing
        assert(now.busy());
        now.flushPending();                    // "Print now"
        assert(!now.busy());
        assert(now.completedPageCount() + now.droppedPageCount() == 999);
    }

    std::printf("  ok: ESC R / ESC V / ESC U repeats are paced, not atomic\n");
}

} // namespace

// The carriage stops at the right margin — including in GRAPHICS.
//
// Found by fuzzing the control stream (bug hunt 8 round 2). `printBitGraph`
// advanced `curX_` per dot column with no margin test, while every other
// head-motion path in the file has one, so an over-long bit-image run walked
// the head off the sheet: `ESC V 9060 <col>` at 80 dpi parked it 113 inches
// out on an 8.5-inch page and the mechanism charged the full dot-column rate
// for all 9 060 columns — 22 emulated seconds of BUSY, printing nothing,
// because fillDots had already clipped every one of those dots away.
//
// Two properties, and the first is what makes the second safe:
//   1. OUTPUT NEUTRALITY. An over-long run must produce EXACTLY the page an
//      exactly-fitting run produces. Discarded, never wrapped — a wrapped bit
//      image is a corrupted one.
//   2. The head stops on the paper, and the run drains in about the time one
//      line of travel costs rather than fifteen.
void testGraphicsStopsAtTheRightMargin()
{
    const int    dpi      = 144;
    const double paperW   = 8.5;
    // ESC V nnnn c: repeat column `c` nnnn times. At the default pitch the
    // graphics density is 80 dpi, so a full 8.5" line is ~680 columns; ask
    // for 9999 and 4000, both far past it.
    auto runV = [&](const char* count, uint8_t col) {
        ImageWriter iw(dpi, ImageWriter::PaperSize::Letter);
        iw.setSpeed(ImageWriter::Speed::Draft);
        std::vector<uint8_t> job = { 0x1B, 'V' };
        for (const char* p = count; *p; ++p) job.push_back(static_cast<uint8_t>(*p));
        job.push_back(col);
        iw.queueBytes(job.data(), job.size());
        int ticks = 0;
        while ((iw.pendingBytes() || iw.pendingRepeats()) && ticks < 100000) {
            iw.tick(0.05);
            ++ticks;
        }
        assert(!iw.pendingBytes() && !iw.pendingRepeats());
        return std::make_pair(iw.currentPage().pix, ticks);
    };

    const auto huge  = runV("9999", 0xFF);
    const auto large = runV("4000", 0xFF);

    // 1. Same ink, both times: everything past the margin was already being
    //    thrown away by the raster clip, so dropping it earlier cannot move a
    //    single dot. (This is the assertion that would fail if the excess were
    //    wrapped to the next line instead of discarded.)
    assert(huge.first == large.first);
    // …and the page is not blank, so the comparison means something.
    bool anyInk = false;
    for (uint8_t v : huge.first) if (v) { anyInk = true; break; }
    assert(anyInk);

    // 2. Bounded time. One 8.5" line at 80 dpi Draft is well under a second of
    //    mechanism time; before the fix this took 454 ticks (22.7 s) for the
    //    9 060-column case and scaled with the count. Both counts must now
    //    cost the same — the head stops in the same place either way.
    assert(huge.second == large.second);
    assert(huge.second < 100);            // < 5 emulated seconds

    // 3. The head is on the paper.
    {
        ImageWriter iw(dpi, ImageWriter::PaperSize::Letter);
        const uint8_t job[] = { 0x1B, 'V', '9', '9', '9', '9', 0xFF };
        iw.queueBytes(job, sizeof job);
        for (int i = 0; i < 200 && (iw.pendingBytes() || iw.pendingRepeats()); ++i)
            iw.tick(0.05);
        assert(iw.status().headX <= paperW);
    }

    std::printf("  ok: bit-image runs stop at the right margin (same ink, "
                "bounded time)\n");
}

// ── The PACED drain has a work budget too, not only a credit one ────────
//
// tick() banks elapsed seconds and spends them against the cost model, which
// bounds how much MECHANISM TIME one tick may run through. It does not bound
// the WORK, and not every unit costs time: escape parameters are free, and so
// is every bit-image column past the right margin (the carriage is against
// the stop, the column is discarded). A guest that floods `ESC V 9999` past
// the margin therefore queued millions of zero-cost units, and one tick — on
// the UI thread, from pumpImageWriter() — drained the lot with the window
// frozen for it. The catch-up path has had a per-tick budget since the
// 1 MiB-backlog fix; this is the same budget on the paced path.
void testPacedDrainIsBounded()
{
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    iw.setSpeed(ImageWriter::Speed::Draft);

    // 1. Park the head at the right margin, where columns become free. One
    //    over-long run does it, and it drains in bounded time (§ above).
    const uint8_t run[] = { 0x1B, 'V', '9', '9', '9', '9', 0xFF };
    iw.queueBytes(run, sizeof run);
    for (int i = 0; i < 400 && iw.busy(); ++i) iw.tick(0.05);
    assert(!iw.busy());

    // 2. 200 more such runs — about two million units, every one of them
    //    free, and only ~1.4 KiB of queue, so catch-up never arms and this
    //    is squarely the paced path.
    std::vector<uint8_t> flood;
    for (int i = 0; i < 200; ++i)
        flood.insert(flood.end(), run, run + sizeof run);
    iw.queueBytes(flood.data(), flood.size());
    assert(!iw.catchingUp());

    iw.tick(1.0 / 60.0);
    assert(iw.busy() && "one frame must not drain a two-million-unit flood");

    // 3. It still finishes — the budget delays the work, it never drops it.
    for (int i = 0; i < 2000 && iw.busy(); ++i) iw.tick(1.0 / 60.0);
    assert(!iw.busy());
    std::printf("  ok: the paced drain is bounded per tick as well\n");
}

// ── A Print Shop-shaped COLOUR job prints in colour, on one line ─────────
//
// This is the shape the 2026-07-26 trace captured from the real Print Shop's
// colour page, and the thing a user means by "does the ImageWriter II print
// in colour": three graphics passes, each preceded by its own `ESC K` band
// and separated by a BARE CR so they overprint the same line.
//
//   ESC T16 CR LF          advance one line
//   ESC K1 CR ESC G….      yellow  pass
//   ESC K3 CR ESC G….      cyan    pass   — bare CR: SAME line
//   ESC K2 CR ESC G….      magenta pass   — bare CR: SAME line
//
// Run in the DEFAULT AutoFeed::Auto, because that is what a user gets and
// because the whole point of Auto is to settle "does CR feed paper?" from
// the stream — get it wrong and the passes come out as a coloured staircase
// instead of one picture.
void testPrintShopColourPass()
{
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    iw.setPowered(true);
    iw.setOnline(true);
    iw.setRibbon(ImageWriter::Ribbon::FourColour);

    std::vector<uint8_t> job;
    auto put = [&](const char* t) { for (const char* p = t; *p; ++p)
                                        job.push_back(uint8_t(*p)); };
    auto pass = [&](const char* escK, int cols) {
        put(escK);
        job.push_back(0x0D);                       // bare CR — same line
        char hdr[16];
        std::snprintf(hdr, sizeof(hdr), "\x1b" "G%04d", cols);
        put(hdr);
        for (int i = 0; i < cols; ++i) job.push_back(0xFF);
    };

    put("\x1b" "T16");
    put("\r\n");                                   // the CR+LF Auto learns from
    pass("\x1b" "K1", 396);                        // yellow
    pass("\x1b" "K3", 442);                        // cyan
    pass("\x1b" "K2", 326);                        // magenta
    job.push_back(0x0C);

    iw.queueBytes(job.data(), job.size());
    int ticks = 0;
    while ((iw.pendingBytes() || iw.pendingRepeats()) && ticks < 200000) {
        iw.tick(0.05);
        ++ticks;
    }
    assert(!iw.pendingBytes() && !iw.pendingRepeats());
    assert(iw.completedPageCount() >= 1);
    const ImageWriter::Page& page = iw.completedPage(0);

    // Which bands landed, and on which rows?
    long dots[8] = {0};
    int firstRow = 1 << 30, lastRow = -1;
    for (int y = 0; y < page.h; ++y)
        for (int x = 0; x < page.w; ++x) {
            const uint8_t v = page.pix[static_cast<size_t>(y) * page.w + x];
            if (!v) continue;
            ++dots[v >> 5];
            if (y < firstRow) firstRow = y;
            if (y > lastRow)  lastRow  = y;
        }

    // 1. ONE line. A staircase is the documented failure: 8 dots at 72 dpi
    //    is 16 rows on a 144 dpi page, so all three passes must share them.
    assert(lastRow - firstRow + 1 <= 16 &&
           "the three colour passes must overprint one line, not staircase");

    // 2. Real colour, mixed subtractively. Cyan is the widest pass, so it
    //    covers the other two: the page must show cyan alone where only it
    //    reaches, cyan|yellow = GREEN where the yellow pass ends, and all
    //    three = BLACK in the middle. Any of these missing means a band was
    //    dropped or the passes did not land on top of each other.
    assert(dots[2] > 0 && "cyan-only region missing");
    assert(dots[6] > 0 && "cyan|yellow (green) overprint missing");
    assert(dots[7] > 0 && "three-band (black) overprint missing");

    // 3. …and the palette turns those bands into actual colours.
    uint8_t r = 0, g = 0, b = 0;
    ImageWriter::indexToRgb(static_cast<uint8_t>((2 << 5) | 0x1F), r, g, b);
    assert(r == 0 && g == 255 && b == 255);          // cyan
    ImageWriter::indexToRgb(static_cast<uint8_t>((6 << 5) | 0x1F), r, g, b);
    assert(r == 0 && g == 255 && b == 0);            // green
    ImageWriter::indexToRgb(static_cast<uint8_t>((7 << 5) | 0x1F), r, g, b);
    assert(r == 0 && g == 0 && b == 0);              // black

    std::printf("  ok: Print Shop colour passes overprint one line "
                "(cyan %ld, green %ld, black %ld dots)\n",
                dots[2], dots[6], dots[7]);
}

int main()
{
    std::printf("ImageWriter smoke test\n");
    testPaperGeometry();
    testTextAndHighBit();
    testColorRibbon();
    testBitImageGraphics();
    testCommandFraming();
    testPaperHandling();
    testRgbaExport();
    testSpoolSeam();
    testMechanismPacing();
    testCommandBoundsAndTabs();
    testBoundedCatchUp();
    testAutoLineFeedDetection();
    testNoUnaffordableByte();
    testTraceClosedOnDestruction();
    testParserHardening();
    testEjectInvalidatesSheetReferences();
    testRepeatIsPacedNotAtomic();
    testGraphicsStopsAtTheRightMargin();
    testPacedDrainIsBounded();
    testPrintShopColourPass();
    std::printf("PASS\n");
    return 0;
}
