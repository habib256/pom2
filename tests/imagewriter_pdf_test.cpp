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

// ImageWriter multi-page PDF export smoke test — pins the PDF serialiser
// (`writeImageWriterPdf`) end to end:
//
//   * structural validity: header, /Count, per-object xref offsets that
//     actually land on "N 0 obj", startxref → "xref", trailing %%EOF —
//     the parts a strict viewer trips on if the byte accounting drifts;
//   * raster fidelity: the FlateDecode stream inflates (via stb_image's
//     zlib decoder) back to the exact indexed bytes that went in;
//   * physical size: the /MediaBox comes from each sheet's OWN dpi
//     (Page::dpi), so a 144-dpi sheet completed before a host DPI change
//     still exports at its true inches;
//   * ImageWriter integration: sheets printed through the real control
//     path carry their dpi and export without error;
//   * error contract: empty page list and malformed rasters refuse.

// This test binary provides the single stb_image_write implementation
// (the GUI's lives in Pom2HgrPaintHost.cpp, not linked here).
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

#include "ImageWriter.h"
#include "ImageWriterPdf.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

using pom2::ImageWriter;
using pom2::writeImageWriterPdf;

namespace {

const char* kOutPath = "imagewriter_pdf_test_out/test.pdf";

std::string readAll(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    assert(f && "exported PDF must exist");
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// First position of `needle` at/after `from`; asserts it exists.
size_t findAt(const std::string& hay, const std::string& needle, size_t from = 0)
{
    const size_t p = hay.find(needle, from);
    assert(p != std::string::npos);
    return p;
}

void testStructureAndRoundTrip()
{
    // Two tiny sheets at different DPIs. Sheet 1: 4x3 @ 72 dpi (4.00 x
    // 3.00 pt... no — 72 dpi → 1 px = 1 pt, so 4.00 3.00). Sheet 2:
    // 6x2 @ 144 dpi → 3.00 1.00 pt.
    ImageWriter::Page a;
    a.w = 4; a.h = 3; a.dpi = 72;
    a.pix = { 0, 31, 0x3F, 0,
              0x5F, 0, 0, 0x7F,
              0, 0xFF, 0xDF, 0 };
    ImageWriter::Page b;
    b.w = 6; b.h = 2; b.dpi = 144;
    b.pix.assign(12, 0xE1);   // faint black

    std::string err;
    assert(writeImageWriterPdf({ &a, &b }, kOutPath, err) && err.empty());

    const std::string pdf = readAll(kOutPath);
    assert(pdf.rfind("%PDF-1.4\n", 0) == 0);
    assert(pdf.size() > 8 && pdf.substr(pdf.size() - 6) == "%%EOF\n");
    findAt(pdf, "/Count 2");
    findAt(pdf, "/Indexed /DeviceRGB 255");

    // MediaBox from each sheet's own dpi.
    findAt(pdf, "/MediaBox [0 0 4.00 3.00]");
    findAt(pdf, "/MediaBox [0 0 3.00 1.00]");

    // startxref points at the xref table.
    const size_t sx = pdf.rfind("startxref\n");
    assert(sx != std::string::npos);
    const size_t xrefPos =
        static_cast<size_t>(std::atol(pdf.c_str() + sx + 10));
    assert(pdf.compare(xrefPos, 4, "xref") == 0);

    // Every xref entry must land exactly on its "N 0 obj" header.
    // Table: "xref\n0 K\n" then one 20-byte line per object incl. obj 0.
    const size_t tbl = findAt(pdf, "\n", xrefPos + 5) + 1;   // skip "0 K"
    const int nObjs = 3 + 2 * 3;
    for (int id = 1; id <= nObjs; ++id) {
        const char* line = pdf.c_str() + tbl + 20 * static_cast<size_t>(id);
        const size_t off = static_cast<size_t>(std::atol(line));
        const std::string want = std::to_string(id) + " 0 obj";
        assert(pdf.compare(off, want.size(), want) == 0);
    }

    // Round-trip each image stream through stb's zlib decoder.
    const ImageWriter::Page* sheets[] = { &a, &b };
    size_t search = 0;
    for (const ImageWriter::Page* p : sheets) {
        const size_t dict = findAt(pdf, "/Subtype /Image", search);
        const size_t lenPos = findAt(pdf, "/Length ", dict);
        const int zLen = std::atoi(pdf.c_str() + lenPos + 8);
        const size_t data = findAt(pdf, "stream\n", dict) + 7;
        int outLen = 0;
        char* raw = stbi_zlib_decode_malloc(pdf.data() + data, zLen, &outLen);
        assert(raw != nullptr);
        assert(outLen == static_cast<int>(p->pix.size()));
        assert(std::memcmp(raw, p->pix.data(), p->pix.size()) == 0);
        std::free(raw);
        search = data + static_cast<size_t>(zLen);
    }

    std::printf("  ok: structure + xref offsets + Flate round-trip\n");
}

void testImageWriterIntegration()
{
    // Print through the real control path: text on sheet 1, form feed,
    // text on sheet 2 — then export completed + current.
    ImageWriter iw(72);
    iw.setSpeed(ImageWriter::Speed::Instant);
    const char* line1 = "PAGE ONE\r";
    iw.printBytes(reinterpret_cast<const uint8_t*>(line1), strlen(line1));
    iw.formFeed();
    const char* line2 = "PAGE TWO\r";
    iw.printBytes(reinterpret_cast<const uint8_t*>(line2), strlen(line2));

    assert(iw.completedPageCount() == 1);
    assert(!iw.currentPageBlank());
    assert(iw.completedPage(0).dpi == 72);
    assert(iw.currentPage().dpi == 72);

    std::vector<const ImageWriter::Page*> sheets;
    sheets.push_back(&iw.completedPage(0));
    sheets.push_back(&iw.currentPage());
    std::string err;
    assert(writeImageWriterPdf(sheets,
                               "imagewriter_pdf_test_out/job.pdf", err));
    const std::string pdf = readAll("imagewriter_pdf_test_out/job.pdf");
    findAt(pdf, "/Count 2");
    // Letter @ 72 dpi = 612 x 792 pt, the classic US-Letter media box.
    findAt(pdf, "/MediaBox [0 0 612.00 792.00]");

    std::printf("  ok: ImageWriter sheets → 2-page PDF (Letter 612x792)\n");
}

void testErrorContract()
{
    std::string err;
    assert(!writeImageWriterPdf({}, kOutPath, err) && !err.empty());

    ImageWriter::Page bad;
    bad.w = 4; bad.h = 3; bad.dpi = 72;
    bad.pix.assign(5, 0);    // wrong size
    err.clear();
    assert(!writeImageWriterPdf({ &bad }, kOutPath, err) && !err.empty());

    std::printf("  ok: error contract (empty list / malformed raster)\n");
}

// The export used to be a bare `ofstream(trunc)`: it destroyed the PREVIOUS
// export the instant it opened, followed a symlink at the destination, and
// published bytes still in page cache. It goes through the same durable
// temp + rename commit as every other write-back now.
// (Bug hunt 2026-09-06 #H6.)
void testDurableCommit()
{
    namespace fs = std::filesystem;
    ImageWriter::Page p;
    p.w = 8; p.h = 4; p.dpi = 72;
    p.pix.assign(static_cast<std::size_t>(p.w) * p.h, 0);

    std::string err;
    assert(writeImageWriterPdf({ &p }, kOutPath, err) && err.empty());
    const std::string good = readAll(kOutPath);
    assert(good.rfind("%PDF-1.4", 0) == 0);

    // A failed export must leave the previous one intact and no debris.
    const fs::path out(kOutPath);
    const fs::path blocked = out.parent_path() / "blocked.pdf";
    std::error_code ec;
    fs::remove_all(blocked, ec);
    fs::create_directory(blocked, ec);
    err.clear();
    assert(!writeImageWriterPdf({ &p }, blocked.string(), err) && !err.empty());
    assert(fs::is_directory(blocked));
    assert(readAll(kOutPath) == good && "the earlier export was disturbed");

    int debris = 0;
    for (const auto& e : fs::directory_iterator(out.parent_path(), ec)) {
        const std::string nm = e.path().filename().string();
        if (nm.size() > 8 && nm.compare(nm.size() - 8, 8, ".pom2tmp") == 0)
            ++debris;
    }
    assert(debris == 0);
    fs::remove_all(blocked, ec);

    std::printf("  ok: durable commit (previous export survives a failure)\n");
}

}  // namespace

int main()
{
    testStructureAndRoundTrip();
    testImageWriterIntegration();
    testErrorContract();
    testDurableCommit();
    std::printf("OK imagewriter_pdf\n");
    return 0;
}
