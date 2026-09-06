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

// Print-history test — pins src/PrinterHistory.cpp.
//
// POM2 could already export a PDF but forgot everything on quit. This is the
// durable half, so what matters is that it SURVIVES — and that its failure
// modes are boring:
//
//   1. IT RELOADS. A store written by one session must come back intact in
//      the next, with metadata attached. That is the entire point.
//   2. A PARTIAL WRITE DOES NOT POISON IT. The index is rewritten whole on
//      every change; if that is interrupted, the previous index must still be
//      the one on disk (hence the write-to-temp-then-rename), and a truncated
//      or foreign index must yield an EMPTY history rather than rows pointing
//      at files that mean something else.
//   3. IT IS BOUNDED. An emulator left running must not quietly fill a disk
//      with printouts.
//   4. PAGES OF ONE DOCUMENT GROUP. Sheets ejected back to back are one job;
//      otherwise "re-preview this job" restores a single page and is useless.

// This TU provides the stb implementations. The GUI's single non-static copy
// lives in Pom2HgrPaintHost.cpp, which is not linked here — same arrangement
// imagewriter_pdf_test.cpp uses, and the reason this test does not drag the
// whole paint-host include cone in for a PNG writer.
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

#include "PrinterHistory.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using pom2::HistoryPage;
using pom2::ImageWriter;
using pom2::PrinterHistory;

/// A page with a recognisable raster, so a reload can be checked to have
/// brought back THIS page and not merely A page.
ImageWriter::Page makePage(int w, int h, uint8_t seed)
{
    ImageWriter::Page p;
    p.w   = w;
    p.h   = h;
    p.dpi = 144;
    p.pix.assign(static_cast<size_t>(w) * h, 0);
    // Low 5 bits are ink, top 3 the ribbon band — write ink only.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            p.pix[static_cast<size_t>(y) * w + x] =
                static_cast<uint8_t>(((x + y + seed) % 31));
    return p;
}

fs::path scratch(const char* leaf)
{
    fs::path d = fs::temp_directory_path() / "pom2_printer_history" / leaf;
    std::error_code ec;
    fs::remove_all(d, ec);
    return d;
}

// ── 1. Store, reload, and the metadata survives ──────────────────────────
void testPersistsAcrossSessions()
{
    const fs::path dir = scratch("persist");
    std::string err;

    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.size() == 0);

        const auto page = makePage(64, 48, 3);
        assert(h.addPage(page, /*model*/ 2, /*ribbon*/ 1, 8.5, 11.0, err));
        // The row is there IMMEDIATELY — the panel draws it on the next
        // frame. The PNG is not: encoding it costs ~100 ms on a real page and
        // happens on the writer thread, so the file only has to exist once
        // the queue has drained.
        assert(h.size() == 1);
        h.flushPending();
        assert(fs::exists(dir / h.pages()[0].file));
    }

    {   // A whole new object, as a new session would be.
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.size() == 1);
        const HistoryPage& p = h.pages()[0];
        assert(p.model == 2);
        assert(p.ribbon == 1);
        assert(p.paperW == 8.5);
        assert(p.paperL == 11.0);
        assert(p.w == 64 && p.h == 48 && p.dpi == 144);
        assert(!p.savedAt.empty());

        // And the raster really comes back.
        std::vector<uint8_t> rgba;
        int w = 0, h2 = 0;
        assert(h.loadRgba(p, rgba, w, h2, err));
        assert(w == 64 && h2 == 48);
        assert(rgba.size() == static_cast<size_t>(w) * h2 * 4);
    }
    std::printf("  ok: a stored page reloads with its metadata and raster\n");
}

// ── 2. Pages of one document share a job ─────────────────────────────────
void testPagesGroupIntoJobs()
{
    const fs::path dir = scratch("jobs");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));

    // Three sheets ejected back to back, as a three-page document does.
    for (int i = 0; i < 3; ++i)
        assert(h.addPage(makePage(32, 24, static_cast<uint8_t>(i)), 0, 0,
                         8.0, 11.0, err));

    assert(h.size() == 3);
    const uint64_t job = h.pages()[0].job;
    for (const auto& p : h.pages()) assert(p.job == job);

    // jobPages returns them oldest first — the order they were printed.
    const auto pages = h.jobPages(job);
    assert(pages.size() == 3);
    assert(pages.front()->file < pages.back()->file);
    std::printf("  ok: sheets ejected together form one job, oldest first\n");
}

// ── 3. Bounded ───────────────────────────────────────────────────────────
void testTrimsToCap()
{
    const fs::path dir = scratch("cap");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));

    const size_t over = PrinterHistory::kMaxPages + 5;
    for (size_t i = 0; i < over; ++i)
        assert(h.addPage(makePage(8, 8, static_cast<uint8_t>(i)), 0, 0,
                         8.0, 11.0, err));

    assert(h.size() == PrinterHistory::kMaxPages);
    h.flushPending();

    // The evicted PNGs are gone from disk too — a cap that only trimmed the
    // index would leak files forever, which is the bug worth pinning.
    size_t pngs = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".png") ++pngs;
    assert(pngs == PrinterHistory::kMaxPages);

    std::printf("  ok: capped at %zu pages, and the evicted PNGs are deleted\n",
                PrinterHistory::kMaxPages);
}

// ── 4. A foreign or truncated index yields an EMPTY history ──────────────
void testBadIndexIsIgnored()
{
    {   // Foreign format.
        const fs::path dir = scratch("foreign");
        std::string err;
        {
            PrinterHistory h;
            assert(h.open(dir.string(), err));
            assert(h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
        }
        std::ofstream(dir / "index.txt", std::ios::trunc)
            << "{\"some\":\"json\"}\n";

        PrinterHistory h;
        assert(h.open(dir.string(), err));
        // Better an empty history than rows pointing at files whose meaning
        // POM2 cannot vouch for.
        assert(h.size() == 0);
        // ...but an index POM2 cannot READ is not permission to DELETE. The
        // sweep that removes unreferenced PNGs used to run behind the empty
        // page list a failed parse leaves, so one unrecognised index erased
        // up to 200 printouts on open — silently, with the size()==0 above
        // as its only visible trace. The bytes must still be there, and the
        // index that could not be read must be kept aside, not overwritten.
        // (Bug hunt 2026-09-06 #H1.)
        assert(fs::exists(dir / "p000001.png"));
        int aside = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().filename().string().rfind("index.txt.bad-", 0) == 0)
                ++aside;
        assert(aside == 1);
    }

    {   // Truncated final record — the line is skipped, earlier ones survive.
        const fs::path dir = scratch("truncated");
        std::string err;
        {
            PrinterHistory h;
            assert(h.open(dir.string(), err));
            assert(h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
            assert(h.addPage(makePage(16, 16, 2), 0, 0, 8.0, 11.0, err));
        }
        // Chop the last line mid-record.
        std::string all;
        {
            std::ifstream in(dir / "index.txt");
            std::string line;
            int n = 0;
            while (std::getline(in, line)) { all += line + "\n"; if (++n == 2) break; }
        }
        all += "p999999.png\t2026-01-01 00:00:00\t9\t0";   // no newline, short
        std::ofstream(dir / "index.txt", std::ios::trunc) << all;

        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.size() == 1);          // the intact record survived
    }
    std::printf("  ok: a foreign index empties, a truncated one keeps what parsed\n");
}

void testIndexCannotEscapeStore()
{
    const fs::path dir = scratch("path_escape");
    const fs::path outside = dir.parent_path() / "do-not-delete.txt";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream(outside) << "sentinel";
    std::ofstream(dir / "index.txt")
        << "pom2-printer-history\t1\n"
        << "../../do-not-delete.txt\t2026-01-01 00:00:00\t1\t0\t0\t8\t11\t1\t1\t144\n";

    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));
    assert(h.size() == 0);
    assert(fs::exists(outside));
    assert(h.clear(err));
    assert(fs::exists(outside));

    // The public API is confined too; callers cannot manufacture a
    // HistoryPage that bypasses the index parser's validation.
    HistoryPage forged;
    forged.file = "../../do-not-delete.txt";
    std::vector<uint8_t> rgba;
    int w = 0, hh = 0;
    assert(!h.loadRgba(forged, rgba, w, hh, err));
    assert(!h.erase(forged, err));
    assert(fs::exists(outside));
    fs::remove(outside, ec);
    std::printf("  ok: index paths cannot escape the history directory\n");
}

// ── 5. A page whose PNG was deleted by hand drops out ────────────────────
void testMissingFileDropsRow()
{
    const fs::path dir = scratch("missing");
    std::string err;
    std::string victim;
    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
        assert(h.addPage(makePage(16, 16, 2), 0, 0, 8.0, 11.0, err));
        victim = h.pages()[0].file;
    }
    fs::remove(dir / victim);

    PrinterHistory h;
    assert(h.open(dir.string(), err));
    assert(h.size() == 1);
    assert(h.pages()[0].file != victim);
    std::printf("  ok: a hand-deleted PNG leaves no dead row\n");
}

// ── 6. Erase and clear ───────────────────────────────────────────────────
void testEraseAndClear()
{
    const fs::path dir = scratch("erase");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));
    for (int i = 0; i < 3; ++i)
        assert(h.addPage(makePage(16, 16, static_cast<uint8_t>(i)), 0, 0,
                         8.0, 11.0, err));

    const std::string gone = h.pages()[1].file;
    assert(h.erase(h.pages()[1], err));
    assert(h.size() == 2);
    assert(!fs::exists(dir / gone));

    assert(h.clear(err));
    assert(h.size() == 0);
    size_t pngs = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".png") ++pngs;
    assert(pngs == 0);

    // A cleared store still reloads as a valid empty one.
    PrinterHistory h2;
    assert(h2.open(dir.string(), err));
    assert(h2.size() == 0);
    std::printf("  ok: erase and clear remove both the row and the file\n");
}

// ── 7. New pages never overwrite old files after a reload ────────────────
void testFileCounterSurvivesReload()
{
    const fs::path dir = scratch("counter");
    std::string err;
    std::string first;
    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
        first = h.pages()[0].file;
    }
    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.addPage(makePage(16, 16, 2), 0, 0, 8.0, 11.0, err));
        // If the counter restarted at 1 the new page would clobber the old
        // one's PNG and the history would show two rows of the same image.
        assert(h.pages()[0].file != first);
        assert(h.size() == 2);
        assert(fs::exists(dir / first));
    }
    std::printf("  ok: the file counter resumes, so a reload cannot clobber\n");
}

// ── 8. The encode is off the render thread, and nothing is lost ─────────
//
// addPage is reached from MainWindow::pumpImageWriter on the ImGui RENDER
// thread, once per ejected sheet. Encoding a real Letter page at 144 dpi
// measured 99-143 ms — six to eight dropped frames, and ImageWriter::tick
// allows four ejects in one tick, so a form feed froze the UI for half a
// second. So the conversion and the PNG deflate live on a writer thread. What
// that must NOT cost:
//
//   * the panel seeing the row (pages_ is updated synchronously);
//   * browsing a sheet before its file exists (served from the queue);
//   * a page ejected just before quit (the destructor drains, not discards).
void testWritesAreDeferredButNeverLost()
{
    const fs::path dir = scratch("async");
    std::string err;
    std::string firstFile;

    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        for (int i = 0; i < 4; ++i)
            assert(h.addPage(makePage(64, 48, static_cast<uint8_t>(i)), 0, 0,
                             8.5, 11.0, err));

        // Metadata is immediate — this is what the panel reads.
        assert(h.size() == 4);
        firstFile = h.pages()[0].file;

        // A page the writer has not reached yet still renders: loadRgba
        // serves it out of the queue rather than failing on a missing file.
        // (If the writer already drained, this simply reads it from disk —
        // either way the caller gets the raster.)
        std::vector<uint8_t> rgba;
        int w = 0, hh = 0;
        assert(h.loadRgba(h.pages()[0], rgba, w, hh, err));
        assert(w == 64 && hh == 48);
        assert(rgba.size() == static_cast<size_t>(w) * hh * 4);

        h.flushPending();
        assert(h.pendingWrites() == 0);
        for (const auto& p : h.pages()) assert(fs::exists(dir / p.file));
    }

    // Destruction drains: a sheet accepted a frame before quit is on disk.
    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.addPage(makePage(64, 48, 9), 0, 0, 8.5, 11.0, err));
        // deliberately NO flushPending() — the destructor must do it
    }
    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.size() == 5);
        for (const auto& p : h.pages()) assert(fs::exists(dir / p.file));
        assert(fs::exists(dir / firstFile));
    }

    std::printf("  ok: encodes are deferred, and the destructor drains them\n");
}

void testEncodeFailureRemovesDanglingRow()
{
    const fs::path dir = scratch("encode_failure");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));

    // The encoder writes a temp file and RENAMES it over the page. A
    // directory sitting at the page's own path makes that rename fail
    // deterministically and without privileges — and, unlike planting
    // something at the temp name, it does not depend on what that temp name
    // is (it is now unique per process + per call; see tempSiblingPath).
    fs::create_directory(dir / "p000001.png");
    assert(h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
    h.flushPending();
    assert(h.size() == 0);
    assert(fs::is_directory(dir / "p000001.png"));   // never became a page

    // The repair is durable, not merely an in-memory cosmetic cleanup.
    PrinterHistory reopened;
    assert(reopened.open(dir.string(), err));
    assert(reopened.size() == 0);
    std::printf("  ok: a failed async encode cannot leave a dangling row\n");
}

void testRejectsMalformedRaster()
{
    const fs::path dir = scratch("bad_raster");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));
    auto page = makePage(16, 16, 1);
    page.pix.pop_back();
    assert(!h.addPage(page, 0, 0, 8.0, 11.0, err));
    assert(h.size() == 0);
    std::printf("  ok: malformed raster dimensions are rejected safely\n");
}

void testIndexFailureRollsBackAndRetries()
{
    const fs::path dir = scratch("index_failure");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));

    // Make the index COMMIT fail, without relying on permissions and without
    // assuming the temp file's name (it is unique per process + per call now;
    // see pom2::tempSiblingPath). A directory at the index's own path makes
    // the rename fail deterministically.
    fs::create_directory(dir / "index.txt");
    assert(!h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
    assert(h.size() == 0);
    assert(h.pendingWrites() == 0);
    assert(!fs::exists(dir / "p000001.png"));

    fs::remove_all(dir / "index.txt");
    assert(h.addPage(makePage(16, 16, 2), 0, 0, 8.0, 11.0, err));
    assert(h.pages()[0].file == "p000001.png"); // counter/job rolled back
    h.flushPending();
    assert(fs::exists(dir / "p000001.png"));
    std::printf("  ok: an index failure is rolled back and remains retryable\n");
}

void testCounterBeyondSixDigitsAndOrphanCleanup()
{
    const fs::path dir = scratch("wide_counter");
    std::string err;
    fs::create_directories(dir);
    const auto page = makePage(8, 8, 7);
    std::vector<uint8_t> rgba;
    ImageWriter::pageToRgba(page, rgba);
    assert(stbi_write_png((dir / "p999999.png").string().c_str(), 8, 8, 4,
                          rgba.data(), 8 * 4));
    assert(stbi_write_png((dir / "p888888.png").string().c_str(), 8, 8, 4,
                          rgba.data(), 8 * 4));
    std::ofstream(dir / "index.txt")
        << "pom2-printer-history\t1\n"
        << "p999999.png\t2026-01-01 00:00:00\t1\t0\t0\t8\t11\t8\t8\t144\n";

    PrinterHistory h;
    assert(h.open(dir.string(), err));
    assert(!fs::exists(dir / "p888888.png")); // safe unreferenced orphan
    assert(h.addPage(page, 0, 0, 8.0, 11.0, err));
    assert(h.pages()[0].file == "p1000000.png");
    h.flushPending();

    PrinterHistory reopened;
    assert(reopened.open(dir.string(), err));
    assert(reopened.size() == 2);
    assert(reopened.pages()[0].file == "p1000000.png");
    std::printf("  ok: counters beyond six digits reload; safe orphans clean up\n");
}

void testRepeatedIndexRowsAreBounded()
{
    const fs::path dir = scratch("repeated_rows");
    std::string err;
    fs::create_directories(dir);
    const auto page = makePage(1, 1, 1);
    std::vector<uint8_t> rgba;
    ImageWriter::pageToRgba(page, rgba);
    assert(stbi_write_png((dir / "p000001.png").string().c_str(), 1, 1, 4,
                          rgba.data(), 4));
    std::ofstream index(dir / "index.txt");
    index << "pom2-printer-history\t1\n";
    for (size_t i = 0; i < PrinterHistory::kMaxPages + 50; ++i)
        index << "p000001.png\t2026-01-01 00:00:00\t" << (i + 1)
              << "\t0\t0\t8\t11\t1\t1\t144\n";
    index.close();

    PrinterHistory h;
    assert(h.open(dir.string(), err));
    assert(h.size() == PrinterHistory::kMaxPages);
    std::printf("  ok: repeated index rows are capped during parsing\n");
}

void testOversizedEncodedPageIsRejectedBeforeDecode()
{
    const fs::path dir = scratch("oversized_png");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));

    const fs::path png = dir / "p000001.png";
    std::ofstream out(png, std::ios::binary | std::ios::trunc);
    assert(out);
    out.seekp(64 * 1024 * 1024);
    out.put('\0');
    out.close();
    assert(out);

    HistoryPage page;
    page.file = "p000001.png";
    std::vector<uint8_t> rgba;
    int w = 0, height = 0;
    assert(!h.loadRgba(page, rgba, w, height, err));
    assert(rgba.empty());
    std::printf("  ok: oversized encoded page is refused before decode\n");
}

} // namespace

int main()
{
    testPersistsAcrossSessions();
    testPagesGroupIntoJobs();
    testTrimsToCap();
    testBadIndexIsIgnored();
    testIndexCannotEscapeStore();
    testMissingFileDropsRow();
    testEraseAndClear();
    testFileCounterSurvivesReload();
    testWritesAreDeferredButNeverLost();
    testEncodeFailureRemovesDanglingRow();
    testRejectsMalformedRaster();
    testIndexFailureRollsBackAndRetries();
    testCounterBeyondSixDigitsAndOrphanCleanup();
    testRepeatedIndexRowsAreBounded();
    testOversizedEncodedPageIsRejectedBeforeDecode();

    std::puts("printer_history: OK");
    return 0;
}
