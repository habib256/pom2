// POM2 Apple II Emulator — PostScript delegation test.
//
// The LaserWriter's PostScript mode does not parse; it runs somebody else's
// interpreter (PostScriptRender.h explains why). Two thirds of that path are
// testable with no interpreter installed, which is the state of most CI
// machines, and this pins those:
//
//   1. The PGM reader, which is the whole interchange format. Hand-rolled
//      PGM parsers get comments, a non-255 maxval and the single-whitespace
//      header terminator wrong; each has its own case here.
//   2. The job sniffer, which decides whether a stream goes to the
//      interpreter at all. A false positive would swallow a plain text job.
//   3. The spooler's threading contract: it must never block the caller, it
//      must join its worker, and with no interpreter present it must report
//      that as an ERROR rather than hanging or silently dropping the job.
//   4. adoptRenderedPage, the seam where a rendered raster becomes a POM2
//      page — including that greys survive, since the page model was always
//      an intensity ramp and a 1-bit blit would throw that away.
//
// The interpreter itself is exercised only when one is installed; that branch
// says so rather than being skipped silently.

#include "ImageWriter.h"
#include "PostScriptRender.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <string>
#include <vector>
#include "TestTempPath.h"

using pom2::ImageWriter;

namespace {

std::vector<uint8_t> bytesOf(const std::string& s)
{
    return { s.begin(), s.end() };
}

void testPgmHappyPath()
{
    // 3x2, maxval 255, values 0..250.
    std::string hdr = "P5\n3 2\n255\n";
    std::vector<uint8_t> pgm = bytesOf(hdr);
    const uint8_t px[6] = { 0, 50, 100, 150, 200, 250 };
    pgm.insert(pgm.end(), px, px + 6);

    pom2::PsRenderResult r;
    assert(pom2::parsePgm(pgm.data(), pgm.size(), r));
    assert(r.ok && r.w == 3 && r.h == 2);
    assert(r.gray.size() == 6);
    for (int i = 0; i < 6; ++i) assert(r.gray[i] == px[i]);
    std::printf("  ok: PGM 3x2 round-trips\n");
}

void testPgmComments()
{
    // A `#` comment may sit between ANY two header tokens. A parser that only
    // skips whitespace reads the '#' as a digit-less token and gives up — or
    // worse, misreads the geometry.
    std::string hdr = "P5\n# rendered by ghostscript\n3\n# width above\n2 255\n";
    std::vector<uint8_t> pgm = bytesOf(hdr);
    for (int i = 0; i < 6; ++i) pgm.push_back(static_cast<uint8_t>(i * 10));

    pom2::PsRenderResult r;
    assert(pom2::parsePgm(pgm.data(), pgm.size(), r));
    assert(r.w == 3 && r.h == 2);
    assert(r.gray[5] == 50);
    std::printf("  ok: PGM header comments are skipped\n");
}

void testPgmMaxvalNormalised()
{
    // maxval 15 must be scaled to 0..255, or an adopted page comes out
    // almost white and looks like a failed render rather than a scaled one.
    std::vector<uint8_t> pgm = bytesOf("P5\n2 1\n15\n");
    pgm.push_back(0);
    pgm.push_back(15);

    pom2::PsRenderResult r;
    assert(pom2::parsePgm(pgm.data(), pgm.size(), r));
    assert(r.gray[0] == 0);
    assert(r.gray[1] == 255);
    std::printf("  ok: a non-255 maxval is normalised\n");
}

void testPgmSingleWhitespaceTerminator()
{
    // EXACTLY one whitespace byte ends the header and the data starts. A
    // parser that skips "all whitespace" eats a leading 0x20 pixel — a light
    // grey one — off the first row and shifts the whole image by a pixel.
    std::vector<uint8_t> pgm = bytesOf("P5\n2 1\n255\n");
    pgm.push_back(0x20);       // a real pixel that happens to be whitespace
    pgm.push_back(0x40);

    pom2::PsRenderResult r;
    assert(pom2::parsePgm(pgm.data(), pgm.size(), r));
    assert(r.gray.size() == 2);
    assert(r.gray[0] == 0x20 && r.gray[1] == 0x40);
    std::printf("  ok: a 0x20 pixel is not eaten as header whitespace\n");
}

void testPgmRejects()
{
    pom2::PsRenderResult r;
    const std::vector<uint8_t> notPgm = bytesOf("P2\n2 2\n255\n1 2 3 4\n");
    assert(!pom2::parsePgm(notPgm.data(), notPgm.size(), r));
    assert(!r.error.empty());

    std::vector<uint8_t> truncated = bytesOf("P5\n4 4\n255\n");
    truncated.push_back(1);                    // 1 byte where 16 are promised
    assert(!pom2::parsePgm(truncated.data(), truncated.size(), r));

    const std::vector<uint8_t> empty;
    assert(!pom2::parsePgm(empty.data(), empty.size(), r));
    assert(!pom2::parsePgm(nullptr, 0, r));
    std::printf("  ok: malformed / truncated / empty PGMs are refused\n");
}

void testPgmMultiPage()
{
    // Ghostscript writes EVERY `showpage` into the same pgmraw file when the
    // output name carries no `%d`, so a multi-page job comes back as several
    // P5 blocks back to back. The reader stopped after the first and nothing
    // said so: `extraPages` was declared, documented — and never assigned, so
    // page 2 of a two-page document vanished in silence.
    std::vector<uint8_t> two = bytesOf("P5\n2 2\n255\n");
    const uint8_t p1[4] = { 10, 20, 30, 40 };
    two.insert(two.end(), p1, p1 + 4);
    std::vector<uint8_t> hdr2 = bytesOf("P5\n2 2\n255\n");
    two.insert(two.end(), hdr2.begin(), hdr2.end());
    const uint8_t p2[4] = { 50, 60, 70, 80 };
    two.insert(two.end(), p2, p2 + 4);

    pom2::PsRenderResult r;
    assert(pom2::parsePgm(two.data(), two.size(), r));
    assert(r.extraPages == 1);
    assert(r.more.size() == 1);
    // The FIRST block is still the inlined page…
    for (int i = 0; i < 4; ++i) assert(r.gray[i] == p1[i]);
    // …and the second is whole, not a re-read of the first.
    assert(r.more[0].w == 2 && r.more[0].h == 2);
    for (int i = 0; i < 4; ++i) assert(r.more[0].gray[i] == p2[i]);

    // A single page must still report zero extras, and a trailing newline
    // between blocks (or after the last one) must not be read as a page.
    std::vector<uint8_t> one = bytesOf("P5\n2 1\n255\n");
    one.push_back(1); one.push_back(2); one.push_back('\n');
    pom2::PsRenderResult s;
    assert(pom2::parsePgm(one.data(), one.size(), s));
    assert(s.extraPages == 0 && s.more.empty());
    std::printf("  ok: a concatenated two-page PGM yields both pages\n");
}

void testSpoolerQueuesTheSecondJob()
{
    // Two jobs arriving back to back. The second Ctrl-D used to be dropped
    // whenever a render was in flight, which did not merely delay the job —
    // it WELDED the two streams into one buffer, so the interpreter got one
    // nonsensical job instead of two good ones. Each Ctrl-D must close a job,
    // and each job must produce its own result. (No interpreter needed: with
    // none installed each render fails loudly, and TWO failures is still two
    // renders.)
    pom2::PostScriptSpooler sp;
    sp.setScratchDir(pom2test::tempPath("pom2_ps_test_scratch"));
    sp.setPageGeometry(72, 1.0, 1.0);

    std::vector<uint8_t> both = bytesOf("%!PS-Adobe-2.0\nshowpage\n");
    both.push_back(pom2::kPsEndOfJob);
    const auto second = bytesOf("%!PS-Adobe-2.0\n0 0 moveto\nshowpage\n");
    both.insert(both.end(), second.begin(), second.end());
    both.push_back(pom2::kPsEndOfJob);
    sp.feed(both.data(), both.size());
    // Nothing left half-fed: both jobs are closed, one running or queued.
    assert(sp.pendingBytes() == 0);

    int collected = 0;
    for (int i = 0; i < 8000 && collected < 2; ++i) {
        pom2::PsRenderResult page;
        if (sp.takeResult(page)) ++collected;
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(collected == 2 && "the second job never rendered");
    assert(sp.queuedJobs() == 0 && !sp.busy());
    std::printf("  ok: two jobs back to back produce two renders\n");
}

void testJobSniffer()
{
    const auto ps = bytesOf("%!PS-Adobe-2.0\n100 100 moveto\n");
    assert(pom2::looksLikePostScript(ps.data(), ps.size()));

    // The Ctrl-D a previous job left, then the magic.
    std::vector<uint8_t> withCtrlD = { pom2::kPsEndOfJob, '\r', '\n' };
    withCtrlD.insert(withCtrlD.end(), ps.begin(), ps.end());
    assert(pom2::looksLikePostScript(withCtrlD.data(), withCtrlD.size()));

    // A plain text job must NOT be mistaken for one — that would send a
    // listing to the interpreter and print nothing.
    const auto text = bytesOf("Dear Sir,\r\n  Please find enclosed\r\n");
    assert(!pom2::looksLikePostScript(text.data(), text.size()));

    // A percent sign alone is not the magic; DSC is "%!".
    const auto pct = bytesOf("% discount table\r\n");
    assert(!pom2::looksLikePostScript(pct.data(), pct.size()));

    assert(!pom2::looksLikePostScript(nullptr, 0));
    std::printf("  ok: the job sniffer accepts %%! and refuses plain text\n");
}

void testSpoolerWithoutInterpreter()
{
    // The contract that matters when Ghostscript is absent: feed() returns
    // promptly, the job is not silently dropped, and the failure surfaces as
    // a result with an error rather than as a hang or a lost page.
    pom2::PostScriptSpooler sp;
    sp.setScratchDir(pom2test::tempPath("pom2_ps_test_scratch"));
    sp.setPageGeometry(72, 2.0, 2.0);

    const auto job = bytesOf("%!PS-Adobe-2.0\nshowpage\n");
    sp.feed(job.data(), job.size());
    assert(sp.pendingBytes() == job.size());

    const uint8_t eoj = pom2::kPsEndOfJob;
    sp.feed(&eoj, 1);
    // The job left the buffer the moment the Ctrl-D arrived.
    assert(sp.pendingBytes() == 0);

    // Spin until the worker publishes. Bounded so a regression fails the test
    // instead of hanging the suite.
    pom2::PsRenderResult page;
    bool got = false;
    for (int i = 0; i < 4000 && !got; ++i) {
        got = sp.takeResult(page);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(got && "the spooler never published a result");

    const std::string gs = pom2::findPostScriptInterpreter();
    if (gs.empty()) {
        assert(!page.ok);
        assert(page.error.find("interpreter") != std::string::npos);
        std::printf("  ok: with no interpreter the job fails loudly "
                    "(\"%s\")\n", page.error.c_str());
    } else {
        // An interpreter IS installed, so the whole path ran for real.
        assert(page.ok && "the installed interpreter rejected a trivial job");
        assert(page.w == 144 && page.h == 144);
        std::printf("  ok: rendered a real page through %s (%dx%d)\n",
                    gs.c_str(), page.w, page.h);

        // Stronger: a job that DRAWS must put ink where it said. A blank
        // page satisfies "ok" and would hide a wrong device, a wrong
        // geometry or a silently failing job — so fill the bottom-left
        // quarter and check that corner is inked and the far one is not.
        //
        // PostScript's origin is bottom-left and the raster's is top-left,
        // so the filled quarter lands in the raster's BOTTOM-left.
        pom2::PostScriptSpooler drawer;
        drawer.setScratchDir(pom2test::tempPath("pom2_ps_test_scratch"));
        drawer.setPageGeometry(72, 2.0, 2.0);
        const auto box = bytesOf(
            "%!PS-Adobe-2.0\n0 setgray 0 0 72 72 rectfill\nshowpage\n");
        drawer.feed(box.data(), box.size());
        drawer.feed(&eoj, 1);

        pom2::PsRenderResult drawn;
        bool gotDrawn = false;
        for (int i = 0; i < 4000 && !gotDrawn; ++i) {
            gotDrawn = drawer.takeResult(drawn);
            if (!gotDrawn)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(gotDrawn && drawn.ok);
        const int w = drawn.w, h = drawn.h;
        const uint8_t bottomLeft = drawn.gray[static_cast<size_t>(h - 10) * w + 10];
        const uint8_t topRight   = drawn.gray[static_cast<size_t>(9) * w + (w - 10)];
        assert(bottomLeft < 64  && "the filled quarter did not render");
        assert(topRight   > 192 && "the whole page came out inked");
        std::printf("  ok: a drawn box lands where PostScript put it "
                    "(bottom-left %u, top-right %u)\n",
                    static_cast<unsigned>(bottomLeft),
                    static_cast<unsigned>(topRight));
    }

    // Bytes arriving after the Ctrl-D belong to the NEXT job, not this one.
    const auto next = bytesOf("%!PS-Adobe-2.0\n");
    sp.feed(next.data(), next.size());
    assert(sp.pendingBytes() == next.size());
    sp.reset();
    assert(sp.pendingBytes() == 0);
    std::printf("  ok: post-Ctrl-D bytes start the next job; reset clears\n");
}

void testAdoptRenderedPage()
{
    // The seam where an external raster becomes a POM2 page. The page model
    // is an intensity RAMP (see indexToRgb), so a mid-grey must survive as a
    // mid-grey — a 1-bit blit here would throw away everything anti-aliased
    // PostScript text is made of.
    ImageWriter iw;
    iw.setSpeed(ImageWriter::Speed::Instant);
    iw.setModel(pom2::IwModel::LaserWriterPostScript);

    const int w = iw.pageRasterWidth();
    const int h = iw.pageRasterHeight();
    assert(w > 0 && h > 0);

    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, 255);  // bare paper
    // A black block, a mid-grey block, and the rest paper.
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x) gray[y * w + x] = 0;
    for (int y = 20; y < 30; ++y)
        for (int x = 0; x < 10; ++x) gray[y * w + x] = 128;

    assert(iw.adoptRenderedPage(gray.data(), w, h));

    const auto& pix = iw.currentPage().pix;
    const uint8_t black = pix[0 * w + 0];
    const uint8_t mid   = pix[25 * w + 5];
    const uint8_t paper = pix[100 * w + 100];
    assert((black & 0x1F) == 31);          // full ink
    assert((paper & 0x1F) == 0);           // none
    assert((mid & 0x1F) > 5 && (mid & 0x1F) < 26);   // genuinely in between
    // Neutral black band, or the page would render tinted.
    assert((black >> 5) == 7);
    std::printf("  ok: adoptRenderedPage keeps greys (ink 31 / %u / 0)\n",
                static_cast<unsigned>(mid & 0x1F));

    // A raster larger than the sheet is clipped, not a buffer overrun.
    std::vector<uint8_t> huge(static_cast<size_t>(w + 50) * (h + 50), 0);
    assert(iw.adoptRenderedPage(huge.data(), w + 50, h + 50));
    // And the degenerate inputs are refused rather than trusted.
    assert(!iw.adoptRenderedPage(nullptr, w, h));
    assert(!iw.adoptRenderedPage(gray.data(), 0, h));
    assert(!iw.adoptRenderedPage(gray.data(), w, -1));
    std::printf("  ok: an oversized raster clips; degenerate input refused\n");
}

void testPostScriptHeadFallsBackToText()
{
    // If interception ever fails, a PostScript job reaching the parser must
    // PRINT — that is what a printer which cannot render one does, and it is
    // what makes the failure visible instead of silent.
    ImageWriter iw;
    iw.setSpeed(ImageWriter::Speed::Instant);
    iw.setModel(pom2::IwModel::LaserWriterPostScript);
    const auto job = bytesOf("%!PS-Adobe-2.0\n");
    iw.printBytes(job.data(), job.size());
    long ink = 0;
    for (uint8_t v : iw.currentPage().pix) if (v & 0x1F) ++ink;
    assert(ink > 0);
    std::printf("  ok: a missed PostScript job prints as text (%ld dots)\n", ink);
}

// `-sOutputFile=` is a FORMAT string to Ghostscript: an unescaped `%` in the
// scratch path made it write somewhere else (or refuse), and the job came back
// as "the interpreter produced no page". Every `%` must be doubled, and
// nothing else may change.
void testOutputFileEscaping()
{
    assert(pom2::escapeGsOutputFile("/tmp/pom2/out.pgm") == "/tmp/pom2/out.pgm");
    assert(pom2::escapeGsOutputFile("/tmp/100%25 done/out.pgm") ==
           "/tmp/100%%25 done/out.pgm");
    // The one that actually bites: a literal %d in a folder name would have
    // been taken as the page-number placeholder.
    assert(pom2::escapeGsOutputFile("C:\\Users\\a%db\\p.pgm") ==
           "C:\\Users\\a%%db\\p.pgm");
    assert(pom2::escapeGsOutputFile("%") == "%%");
    assert(pom2::escapeGsOutputFile("").empty());
    std::printf("  ok: %%%% escaping for -sOutputFile\n");
}

} // namespace

int main()
{
    std::puts("PostScript delegation test");
    testOutputFileEscaping();
    testPgmHappyPath();
    testPgmComments();
    testPgmMaxvalNormalised();
    testPgmSingleWhitespaceTerminator();
    testPgmRejects();
    testPgmMultiPage();
    testJobSniffer();
    testSpoolerQueuesTheSecondJob();
    testSpoolerWithoutInterpreter();
    testAdoptRenderedPage();
    testPostScriptHeadFallsBackToText();
    std::puts("postscript_render: OK");
    return 0;
}
