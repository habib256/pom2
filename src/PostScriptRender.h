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

#ifndef POM2_POSTSCRIPT_RENDER_H
#define POM2_POSTSCRIPT_RENDER_H

// PostScript rendering for the LaserWriter head — by DELEGATION, not by
// interpretation.
//
// PostScript is not a command set the way ESC/P and the C. Itoh grammar are.
// It is a Turing-complete stack language with a full graphics model: paths and
// Bezier flattening, even-odd and non-zero winding fills, clipping, halftones,
// and Type 1 fonts that arrive encrypted and hinted. Writing an interpreter
// for it would be larger than the whole rest of POM2's printer subsystem and
// would never be faithful — the failure mode of an almost-right PostScript
// interpreter is a page that is subtly wrong, which is worse than no page.
//
// So POM2 does what it already does for the FujiNet firmware: it runs somebody
// else's program. `ChildProcess` supervises a Ghostscript invocation exactly
// as it supervises the FujiNet helper, and the result comes back as a raster.
//
// LICENSING. Ghostscript is AGPL and POM2 is GPLv3. Running a SEPARATE PROCESS
// is not linking, so this is an optional runtime dependency rather than a
// derived work: POM2 ships nothing of Ghostscript, detects it at runtime, and
// degrades to "the job is spooled here, no interpreter found" when it is
// absent. Do not turn this into a library binding without revisiting that.
//
// PGM, not PNG, is the interchange. `-sDEVICE=pgmraw` gives a five-token ASCII
// header and then raw bytes, which is thirty lines to parse; asking for PNG
// would mean carrying a PNG *decoder* to read back what POM2 only ever writes.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pom2 {

/// One rendered sheet. `gray` is one byte per pixel, 0 = full ink and 255 =
/// bare paper (Ghostscript's `pgmraw` convention), row-major, `w` bytes per
/// row.
struct PsRenderPage {
    int                  w = 0;
    int                  h = 0;
    std::vector<uint8_t> gray;
};

/// Where the rendered job came from and what shape it is. The FIRST page is
/// inlined (`w`/`h`/`gray`) because most jobs have exactly one; `more` holds
/// pages 2..n in order.
struct PsRenderResult {
    bool                 ok = false;
    std::string          error;
    int                  w = 0;
    int                  h = 0;
    std::vector<uint8_t> gray;
    /// Pages past the first, in order. A PostScript job may emit any number
    /// of `showpage`s, and Ghostscript writes every one of them into the same
    /// `pgmraw` file when the output name carries no `%d` — so they arrive as
    /// consecutive `P5` blocks and `parsePgm` unpacks all of them. Reading
    /// only the first truncated multi-page jobs in silence.
    std::vector<PsRenderPage> more;
    /// `more.size()`, kept as a plain count for status text.
    int                  extraPages = 0;
};

/// A job to render: the bytes the guest sent, and the sheet to put them on.
struct PsRenderRequest {
    std::vector<uint8_t> postscript;
    int    dpi      = 144;      ///< match the ImageWriter page raster
    double widthIn  = 8.5;
    double heightIn = 11.0;
};

/// The interpreter POM2 will use, or "" when there is none. Looks for
/// `gs` (and `gswin64c` on Windows) the same way the FujiNet helper is found.
/// Cheap enough to call per panel frame; it does no process work.
std::string findPostScriptInterpreter();

/// True when `data` looks like a PostScript job rather than a text or Diablo
/// stream. Deliberately narrow: a leading `%!` (the DSC magic every driver
/// emits), optionally after the Ctrl-D and whitespace a previous job left.
/// A false positive here would swallow a plain text job into the interpreter.
bool looksLikePostScript(const uint8_t* data, std::size_t n);

/// Standard PostScript job separator. Drivers send it to mark end-of-job, and
/// it is what tells the spooler a job is complete rather than still arriving.
constexpr uint8_t kPsEndOfJob = 0x04;   // Ctrl-D

/// Run `req` through the interpreter at `interpreterPath` and fill `out`.
///
/// SYNCHRONOUS and BLOCKING: it spawns a process and waits for it. Never call
/// it from the UI thread, and never with `stateMutex` held — a page of
/// PostScript is tens to hundreds of milliseconds of somebody else's CPU.
/// `PostScriptSpooler` below is the asynchronous wrapper that exists so
/// callers do not have to remember that.
///
/// `scratchDir` receives the temporary .ps and .pgm; both are removed before
/// returning, on success and on failure alike.
bool renderPostScript(const std::string& interpreterPath,
                      const std::string& scratchDir,
                      const PsRenderRequest& req,
                      PsRenderResult& out);

/// Parse a binary PGM (`P5`) — or a concatenation of them, which is what a
/// multi-page job comes back as: the first block fills `out`'s own fields and
/// every further block is appended to `out.more`. Exposed for its own sake
/// because it is the one piece of `renderPostScript` that can be tested
/// without an interpreter installed — which is most CI machines.
bool parsePgm(const uint8_t* data, std::size_t n, PsRenderResult& out);

/// Asynchronous front end for `renderPostScript`.
///
/// The guest's bytes arrive on the UI thread (the printer pump runs there),
/// and rendering a page is somebody else's process for tens to hundreds of
/// milliseconds. Doing that inline would freeze the window exactly the way
/// mounting an image under `stateMutex` used to freeze the machine — so the
/// render happens on a guarded worker and the UI collects the page later.
///
/// Nothing here ever takes `stateMutex`; it is host-side only.
class PostScriptSpooler
{
public:
    PostScriptSpooler() = default;
    ~PostScriptSpooler();

    PostScriptSpooler(const PostScriptSpooler&)            = delete;
    PostScriptSpooler& operator=(const PostScriptSpooler&) = delete;

    /// Where the temporary .ps / .pgm go. Must be set before the first job.
    void setScratchDir(std::string dir);
    /// The sheet a rendered page must fit, in pixels and dots per inch.
    void setPageGeometry(int dpi, double widthIn, double heightIn);

    /// Feed bytes from the guest. A Ctrl-D (`kPsEndOfJob`) ends the job and
    /// starts a render; bytes arriving while one runs are held for the next.
    /// A Ctrl-D arriving DURING a render closes that next job and queues it —
    /// dropping the separator instead (which is what this used to do) welded
    /// two jobs into one stream that no interpreter could make sense of.
    void feed(const uint8_t* data, std::size_t n);

    /// Render what has been fed so far without waiting for a Ctrl-D. For the
    /// driver that just stops sending — over a serial line there is no
    /// end-of-file to notice, so somebody has to decide, and that somebody is
    /// the UI (an idle timer, or the panel's own button).
    void flushNow();

    /// UI thread: collect a finished page, if there is one. Returns false and
    /// leaves `out` alone when nothing is ready.
    bool takeResult(PsRenderResult& out);

    /// A render is in flight. The UI shows this; it is also why `feed` holds
    /// rather than starting a second interpreter.
    bool busy() const;
    /// Bytes waiting for a Ctrl-D or a `flushNow`.
    std::size_t pendingBytes() const;
    /// Complete jobs waiting for the interpreter to free up.
    std::size_t queuedJobs() const;

    /// Drop everything: the buffer, any finished page not yet collected. Does
    /// NOT abandon a running render — that would leak the child process — so
    /// it waits for one to finish and discards the result.
    void reset();

private:
    /// Move `pending_` into `queued_` as one complete job. Caller holds `mtx_`.
    void closeJobLocked();
    /// Start the job at the head of `queued_` if the interpreter is free AND
    /// the result slot is empty (starting one would overwrite an uncollected
    /// page). Caller must hold `mtx_`; the retired worker handle is moved into
    /// `retired` for the caller to join OUTSIDE the lock (joining under `mtx_`
    /// deadlocks — the worker takes it to publish).
    void startNextLocked(std::thread& retired);
    void joinWorker();

    mutable std::mutex   mtx_;
    std::string          scratchDir_;
    int                  dpi_      = 144;
    double               widthIn_  = 8.5;
    double               heightIn_ = 11.0;
    std::vector<uint8_t> pending_;      // bytes since the last Ctrl-D
    /// Jobs closed by a Ctrl-D and waiting their turn. One interpreter runs
    /// at a time; a second job must WAIT, not merge into the first.
    std::deque<std::vector<uint8_t>> queued_;
    std::vector<uint8_t> inFlight_;
    PsRenderResult       done_;
    bool                 haveResult_ = false;
    bool                 busy_       = false;
    std::thread          worker_;

    /// A runaway job must not eat the heap. A page of PostScript is a few
    /// tens of KB; a megabyte is already a driver gone wrong.
    static constexpr std::size_t kMaxJobBytes = 4u << 20;
    /// …and neither must a queue of them. Past this the OLDEST waiting job is
    /// dropped: a guest that outruns the interpreter this far is not going to
    /// be caught up with, and the newest job is the one the user is waiting
    /// for. Dropping is logged.
    static constexpr std::size_t kMaxQueuedJobs = 16;
};

} // namespace pom2

#endif // POM2_POSTSCRIPT_RENDER_H
