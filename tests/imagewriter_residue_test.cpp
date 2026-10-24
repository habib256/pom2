// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// CARDS-011: what bug hunt #15 left on the ImageWriter — unassigned control
// codes, Epson double strike, the 8-inch carriage, the paper size. Its own
// file: imagewriter_smoke_test.cpp sits at the 2000-line ceiling.

#include "ImageWriter.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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

void testImageWriterResidue()
{
    // An unassigned control code is ignored, not drawn as CP437 art.
    {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        const double x = iw.status().headX;
        const uint8_t ctl[] = { 0x01, 0x02, 0x10, 0x15, 0x1C, 0x1E };
        iw.printBytes(ctl, sizeof ctl);
        assert(inkPixels(iw.currentPage()) == 0 && iw.status().headX == x &&
               "unassigned control codes printed as glyphs");
    }
    // Epson double strike (ESC G) leaves more ink than a single pass.
    {
        auto ink = [](bool dbl) {
            ImageWriter iw(144, ImageWriter::PaperSize::Letter);
            iw.setModel(pom2::IwModel::EpsonFX80);
            if (dbl) { const uint8_t g[] = { 0x1B, 'G' }; iw.printBytes(g, 2); }
            feed(iw, "HHHH");
            return inkPixels(iw.currentPage());
        };
        assert(ink(true) > ink(false) && "ESC G double strike was a no-op");
    }
    // The carriage: an 8-inch line from the 1/4" margin, on Letter too —
    // 80 columns at Pica, 96 at the II's power-on Elite 12 cpi.
    {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        const double y0 = iw.status().headY;
        const int cols = static_cast<int>(std::lround(8.0 * iw.status().cpi));
        feed(iw, std::string(static_cast<size_t>(cols - 1), 'H').c_str());
        assert(iw.status().headY == y0 && "an 8-inch line holds its columns");
        feed(iw, "H");
        assert(iw.status().headY > y0 && "the head wraps at the 8-inch line, not the paper edge");
    }
    // Paper: a width drag leaves an A4's 11.69" alone, and says Custom until
    // a named format is picked again.
    {
        ImageWriter iw(144, ImageWriter::PaperSize::A4);
        const double len = iw.paperLengthIn();
        double cw = 0, cl = 0;
        iw.setPaperDimensions(9.0, len, &cw, &cl);
        assert(cw == 9.0 && cl == len && iw.paperLengthIn() == len &&
               "the length was re-snapped by a width change");
        assert(iw.paperIsCustom());
        iw.setPaperSize(ImageWriter::PaperSize::A4);
        assert(!iw.paperIsCustom() && iw.paperWidthIn() < 8.3 &&
               "re-picking the same named format must restore it");
    }
    std::printf("  ImageWriter residue: controls, double strike, carriage, paper: OK\n");
}

}  // namespace

int main()
{
    testImageWriterResidue();
    return 0;
}
