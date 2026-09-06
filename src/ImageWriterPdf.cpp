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

// ImageWriterPdf — see the header for the format rationale.
//
// Layout produced (PDF 1.4, classic xref table):
//   obj 1        /Catalog
//   obj 2        /Pages (kids list)
//   obj 3        the shared /Indexed /DeviceRGB palette (all sheets use
//                the same 256-entry ribbon/intensity palette, so it is
//                emitted once and referenced from every image)
//   obj 4+3i     /Page      (sheet i)
//   obj 5+3i     contents stream ("paint the image across the media box")
//   obj 6+3i     image XObject (FlateDecode'd indexed raster)
// followed by the xref table and trailer. Offsets are byte-exact because
// the whole file is assembled in one std::string before writing.

#include "ImageWriterPdf.h"

// Declarations only — the single non-static stb_image_write implementation
// lives in Pom2HgrPaintHost.cpp (see the note at the top of that file).
// This vintage of the header does not forward-declare stbi_zlib_compress
// (it only appears in the implementation section), so declare it here with
// the same linkage STBIWDEF resolves to in C++ (`extern "C"`).
#include "stb_image_write.h"
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data,
                                             int data_len, int* out_len,
                                             int quality);

#include "AtomicFileReplace.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace pom2 {

namespace {

/// The 256-entry palette as a PDF hex string, from the same
/// `indexToRgb` law the PNG export and the panel texture use.
std::string paletteHex()
{
    std::string hex;
    hex.reserve(256 * 6);
    char buf[8];
    for (int i = 0; i < 256; ++i) {
        uint8_t r, g, b;
        ImageWriter::indexToRgb(static_cast<uint8_t>(i), r, g, b);
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X", r, g, b);
        hex += buf;
    }
    return hex;
}

std::string fmt(const char* f, double a, double b = 0.0)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), f, a, b);
    return buf;
}

} // namespace

bool writeImageWriterPdf(const std::vector<const ImageWriter::Page*>& pages,
                         const std::string& path, std::string& err)
{
    if (pages.empty()) {
        err = "no pages to export";
        return false;
    }
    for (const auto* p : pages) {
        if (!p || p->w <= 0 || p->h <= 0 ||
            p->pix.size() != static_cast<size_t>(p->w) * p->h || p->dpi <= 0) {
            err = "malformed page raster";
            return false;
        }
    }

    const size_t n = pages.size();
    const int nObjs = 3 + static_cast<int>(n) * 3;

    std::string pdf;
    pdf.reserve(64 * 1024);
    std::vector<size_t> offsets(static_cast<size_t>(nObjs) + 1, 0);

    auto beginObj = [&](int id) {
        offsets[static_cast<size_t>(id)] = pdf.size();
        pdf += std::to_string(id) + " 0 obj\n";
    };

    // Header. The second line's high-bit bytes mark the file as binary so
    // transfer tools never re-encode line endings (PDF 1.4 spec §3.4.1).
    pdf += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";

    beginObj(1);
    pdf += "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    beginObj(2);
    pdf += "<< /Type /Pages /Kids [";
    for (size_t i = 0; i < n; ++i) {
        if (i) pdf += ' ';
        pdf += std::to_string(4 + 3 * static_cast<int>(i)) + " 0 R";
    }
    pdf += "] /Count " + std::to_string(n) + " >>\nendobj\n";

    beginObj(3);
    pdf += "[/Indexed /DeviceRGB 255 <" + paletteHex() + ">]\nendobj\n";

    for (size_t i = 0; i < n; ++i) {
        const ImageWriter::Page& p = *pages[i];
        const int pageId  = 4 + 3 * static_cast<int>(i);
        const int contId  = pageId + 1;
        const int imgId   = pageId + 2;
        // Physical size in PDF points (1/72 in) from the sheet's own DPI.
        const double wPt = static_cast<double>(p.w) * 72.0 / p.dpi;
        const double hPt = static_cast<double>(p.h) * 72.0 / p.dpi;

        beginObj(pageId);
        pdf += "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
               fmt("%.2f %.2f", wPt, hPt) +
               "] /Resources << /XObject << /Im0 " + std::to_string(imgId) +
               " 0 R >> >> /Contents " + std::to_string(contId) + " 0 R >>\n"
               "endobj\n";

        // Contents: scale the unit image square across the media box. PDF
        // image space puts the FIRST sample row at the TOP of the unit
        // square (spec §4.8.3), matching the raster's top-down rows.
        const std::string content =
            "q\n" + fmt("%.2f 0 0 %.2f", wPt, hPt) + " 0 0 cm\n/Im0 Do\nQ";
        beginObj(contId);
        pdf += "<< /Length " + std::to_string(content.size()) +
               " >>\nstream\n" + content + "\nendstream\nendobj\n";

        int zLen = 0;
        unsigned char* z = stbi_zlib_compress(
            const_cast<unsigned char*>(p.pix.data()),
            static_cast<int>(p.pix.size()), &zLen, 8);
        if (!z) {
            err = "zlib compression failed (page " + std::to_string(i + 1) +
                  ")";
            return false;
        }
        beginObj(imgId);
        pdf += "<< /Type /XObject /Subtype /Image /Width " +
               std::to_string(p.w) + " /Height " + std::to_string(p.h) +
               " /ColorSpace 3 0 R /BitsPerComponent 8 /Filter /FlateDecode"
               " /Length " + std::to_string(zLen) + " >>\nstream\n";
        pdf.append(reinterpret_cast<const char*>(z),
                   static_cast<size_t>(zLen));
        std::free(z);
        pdf += "\nendstream\nendobj\n";
    }

    // xref table + trailer. Offsets are 10-digit zero-padded per spec.
    const size_t xrefPos = pdf.size();
    pdf += "xref\n0 " + std::to_string(nObjs + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    char line[32];
    for (int id = 1; id <= nObjs; ++id) {
        std::snprintf(line, sizeof(line), "%010zu 00000 n \n",
                      offsets[static_cast<size_t>(id)]);
        pdf += line;
    }
    pdf += "trailer\n<< /Size " + std::to_string(nObjs + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefPos) +
           "\n%%EOF\n";

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path out(path);
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    // The durable temp + rename commit every other write-back in POM2 uses,
    // rather than a bare ofstream(trunc). A plain truncating open destroys the
    // PREVIOUS export the moment it succeeds, so an ENOSPC part-way through
    // left neither file — and it follows a symlink at the destination, and
    // publishes bytes still in page cache. The whole PDF is already assembled
    // in `pdf`, so there is nothing to stream and no reason not to.
    if (!pom2::writeFileAtomic(out, pdf.data(), pdf.size(), ec)) {
        err = "cannot write " + out.string() +
              (ec ? (": " + ec.message()) : std::string());
        return false;
    }
    return true;
}

} // namespace pom2
