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

// printer_paper_tray_test — who is allowed to put a sheet in the tray.
//
// Two rules, both of them about the boundary between the DATA STREAM and the
// FRONT PANEL, and both broken in opposite directions before this test:
//
//  1. `clearAll()` ("Clear all" in the ImageWriter panel, which reports
//     "Paper tray emptied") must actually empty it. It cleared `pages_`
//     first and THEN power-cycled — and `resetPrinter()` deliberately ejects
//     whatever is still on the platen rather than discarding it, so the sheet
//     landed straight back in the tray that had just been emptied. Worse, the
//     eject advanced `sheetsEjected()`, which is the odometer
//     `MainWindow::archiveNewPrinterPages()` keys off: the sheet the user
//     asked POM2 to forget was copied into the DURABLE print history on the
//     next frame.
//
//  2. A form feed IN THE BYTE STREAM is paper motion the guest asked for and
//     always ejects; `formFeed()` is the PANEL BUTTON and carries the panel's
//     "not a blank sheet" rule. The C. Itoh head has always done the first
//     (imagewriter_smoke's `ESC R 999 <FF>` case pins all 999 sheets coming
//     out of a page that never had ink on it), but the ESC/P and Diablo
//     parsers routed their `$0C` through `formFeed()` — so the same byte meant
//     two different things depending on which head was fitted, and an FX-80
//     job's page count silently disagreed with its PDF export and its print
//     history.

#include "ImageWriter.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using pom2::ImageWriter;
using pom2::IwModel;

namespace {

void feed(ImageWriter& iw, const char* s)
{
    for (const char* p = s; *p; ++p)
        iw.printChar(static_cast<uint8_t>(*p));
}

bool blank(const ImageWriter::Page& p)
{
    for (uint8_t v : p.pix)
        if (v) return false;
    return true;
}

size_t blankSheets(const ImageWriter& iw)
{
    size_t n = 0;
    for (size_t i = 0; i < iw.completedPageCount(); ++i)
        if (blank(iw.completedPage(i))) ++n;
    return n;
}

// ── 1. "Clear all" empties the tray ──────────────────────────────────────
void testClearAllEmptiesTheTray()
{
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);

    feed(iw, "HELLO\r");
    iw.formFeed();                       // one finished sheet in the tray
    feed(iw, "WORLD");                   // …and ink still on the platen
    assert(iw.completedPageCount() == 1);
    assert(iw.sheetsEjected() == 1);
    assert(!iw.currentPageBlank());

    const size_t ejectedBefore = iw.sheetsEjected();
    iw.clearAll();

    // The tray is EMPTY. It used to hold one sheet: the platen's, ejected by
    // the power cycle into the stack that had just been cleared.
    assert(iw.completedPageCount() == 0);
    assert(iw.droppedPageCount()   == 0);
    assert(iw.bytesReceived()      == 0);
    assert(iw.currentPageBlank());

    // …and no phantom sheet was reported to the archiver. `sheetsEjected()`
    // is monotonic by contract (see its doc comment), so it must not go
    // BACKWARDS either — resetting it would make archiveNewPrinterPages()
    // skip real pages later.
    assert(iw.sheetsEjected() == ejectedBefore);

    // A clearAll on an already-blank platen is the same story with nothing
    // to eject, and must stay a no-op on the odometer.
    iw.clearAll();
    assert(iw.completedPageCount() == 0);
    assert(iw.sheetsEjected() == ejectedBefore);

    std::printf("  ok: clearAll() empties the tray and ejects nothing into it\n");
}

// ── 2. A data-stream FF ejects on every head ─────────────────────────────
void testDataStreamFormFeedEjectsOnEveryHead()
{
    // "A" FF FF "B" FF: the guest asked for three sheets — an inked one, a
    // blank one it fed past, and a second inked one.
    const struct { IwModel model; const char* name; } kHeads[] = {
        { IwModel::ImageWriterII,     "ImageWriter II (C. Itoh)" },
        { IwModel::EpsonFX80,         "Epson FX-80 (ESC/P)"      },
        { IwModel::LaserWriterDiablo, "LaserWriter (Diablo 630)" },
    };

    for (const auto& h : kHeads) {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        iw.setModel(h.model);
        iw.setAutoFeed(false);
        assert(iw.completedPageCount() == 0);

        feed(iw, "A\f\fB\f");

        // Three sheets, one of them blank — the same answer on all three
        // grammars. The ESC/P and Diablo heads used to give TWO, because
        // their `$0C` went through the front panel's blank-sheet rule.
        if (iw.completedPageCount() != 3 || blankSheets(iw) != 1) {
            std::printf("  FAIL %s: %zu sheet(s), %zu blank (expected 3 / 1)\n",
                        h.name, iw.completedPageCount(), blankSheets(iw));
            assert(false);
        }
        assert(iw.sheetsEjected() == 3);
    }

    std::printf("  ok: a form feed in the byte stream ejects on all three "
                "lineages\n");
}

// ── 3. …and the FRONT PANEL button still refuses a blank sheet ───────────
void testPanelFormFeedStillRefusesABlankSheet()
{
    for (IwModel m : { IwModel::ImageWriterII, IwModel::EpsonFX80,
                       IwModel::LaserWriterDiablo }) {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        iw.setModel(m);
        iw.setAutoFeed(false);

        feed(iw, "A");
        iw.formFeed();                  // the inked sheet comes out
        iw.formFeed();                  // …and the blank one does not
        iw.formFeed();
        assert(iw.completedPageCount() == 1);
        assert(blankSheets(iw) == 0);
        assert(iw.sheetsEjected() == 1);
    }

    std::printf("  ok: the panel FORM FEED button still keeps a blank sheet "
                "on the platen\n");
}

} // namespace

int main()
{
    std::printf("printer_paper_tray_test\n");
    testClearAllEmptiesTheTray();
    testDataStreamFormFeedEjectsOnEveryHead();
    testPanelFormFeedStillRefusesABlankSheet();
    std::printf("printer_paper_tray_test: all ok\n");
    return 0;
}
