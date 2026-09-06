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

// PrinterHistory — completed printouts, kept across sessions.
//
// POM2 could already export a PDF, but it forgot everything the moment you
// quit: the sheet stack lives in `ImageWriter` and is capped at 32 pages. This
// is the durable half — every ejected sheet is written to disk as a PNG with
// the metadata needed to make sense of it later (which printer, which ribbon,
// what paper), and the panel can bring a past job back onto the platen.
//
// ── Layout on disk ────────────────────────────────────────────────────────
//
//   <user-data>/printouts/history/index.txt    one line per page, tab-separated
//   <user-data>/printouts/history/p000123.png  the page raster, 8-bit greyscale or
//                                      RGB when the ribbon was colour
//
// ── Why the index is not JSON ─────────────────────────────────────────────
//
// The plan said JSON. POM2 has no JSON *parser* — the AI control server only
// ever writes it — and pulling one in to read an index of a few dozen lines
// would be the tail wagging the dog. A tab-separated line format is trivial to
// write, trivial to parse, survives a truncated final line, and a user can
// read it in a terminal when something looks wrong. The one real cost is that
// a field containing a tab or a newline would corrupt a record, so the only
// free-text field (the label) is sanitised on the way in.
//
// ── What this deliberately does not do ────────────────────────────────────
//
// No thumbnails, no search, no compression beyond PNG's. A printout history is
// a few dozen pages; anything cleverer would be machinery in search of a
// problem.
//
// ── Why there IS a thread ─────────────────────────────────────────────────
//
// The one piece of machinery here is a single background writer, and it earns
// its place: `addPage` is reached from `MainWindow::pumpImageWriter` on the
// ImGui RENDER thread, once per ejected sheet. Encoding a Letter page at the
// default 144 dpi (1224×1584 → a 7.76 MB RGBA buffer) with stb's default
// compression level measured 99-143 ms — six to eight dropped frames, and
// `ImageWriter::tick` allows four ejects in one tick, so a form-feed catch-up
// froze the UI for half a second. That is the same budget ImageWriter.h
// defends when it explains why the spool drain was moved out of `queueBytes`.
//
// So the RGBA conversion and the PNG encode happen on the writer thread, while
// the index and the in-memory page list are updated synchronously — the panel
// sees the new row on the very next frame. A page still in the queue is served
// straight from it by `loadRgba`, so browsing a sheet that was just ejected
// works before its file exists. Nothing is lost on quit: the destructor drains
// the queue before joining.

#ifndef POM2_PRINTER_HISTORY_H
#define POM2_PRINTER_HISTORY_H

#include "ImageWriter.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pom2 {

/// One stored page. `file` is a bare filename inside the history directory,
/// never a path, so the store stays relocatable.
struct HistoryPage {
    std::string file;
    std::string savedAt;        ///< "YYYY-MM-DD HH:MM:SS", local time
    uint64_t    job     = 0;    ///< pages ejected without a gap share a job
    int         model   = 0;    ///< IwModel at the time
    int         ribbon  = 0;    ///< ImageWriter::Ribbon at the time
    double      paperW  = 0.0;  ///< inches
    double      paperL  = 0.0;
    int         w       = 0;    ///< raster size
    int         h       = 0;
    int         dpi     = 0;
};

class PrinterHistory
{
public:
    /// Largest number of pages kept. Older ones are deleted as new arrive —
    /// a printout is a few hundred KB, and an unbounded store on a machine
    /// left running is how an emulator ends up owning someone's disk.
    static constexpr size_t kMaxPages = 200;

    /// Pages accepted but not yet encoded. Past this `addPage` waits for the
    /// writer instead of growing the queue — a Page is ~1.9 MB, and an
    /// unbounded queue on a slow disk is the same "emulator owns your machine"
    /// failure `kMaxPages` guards against, in RAM. Eight is comfortably above
    /// the four sheets one `ImageWriter::tick` can eject.
    static constexpr size_t kMaxPending = 8;

    PrinterHistory() = default;
    /// Drains the write queue, then joins the writer. Nothing queued is lost.
    ~PrinterHistory();

    PrinterHistory(const PrinterHistory&)            = delete;
    PrinterHistory& operator=(const PrinterHistory&) = delete;

    /// Point the store at `dir`, creating it, and read whatever index is
    /// there. A missing or unreadable index is not an error — it means an
    /// empty history. Returns false only when the directory cannot be made.
    bool open(const std::string& dir, std::string& err);
    bool isOpen() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }

    /// Store one ejected sheet. `jobGapSeconds` decides whether this page
    /// continues the previous job or starts a new one — sheets ejected back
    /// to back are one print job, a sheet an hour later is not.
    bool addPage(const ImageWriter::Page& page, int model, int ribbon,
                 double paperW, double paperL, std::string& err);

    /// Newest first — which is the order a history is read in.
    const std::vector<HistoryPage>& pages() const { return pages_; }
    size_t size() const { return pages_.size(); }

    /// Apply failures reported by the background encoder on the render
    /// thread. Failed rows are removed from memory and the durable index, so
    /// the panel never keeps pointing at a PNG that cannot exist. Returns
    /// false once per failed batch and describes the dropped page(s).
    bool pollWriteFailures(std::string& err);

    /// Decode a stored page back to RGBA for display or re-export.
    bool loadRgba(const HistoryPage& p, std::vector<uint8_t>& rgba,
                  int& w, int& h, std::string& err) const;

    /// Every page of `job`, oldest first — what "re-preview this job" needs.
    std::vector<const HistoryPage*> jobPages(uint64_t job) const;

    /// Forget one page (and delete its PNG), or everything.
    bool erase(const HistoryPage& p, std::string& err);
    bool clear(std::string& err);

    /// Block until every accepted page is on disk. Called before any deletion
    /// (so a page cannot be removed and then recreated by the writer) and by
    /// the destructor; also what makes a test deterministic.
    void flushPending();
    /// Pages accepted but not yet written. Test/diagnostic hook.
    size_t pendingWrites() const;

private:
    /// What `readIndex` found. The distinction is the whole of the orphan
    /// sweep's safety: only a PARSED index says which PNGs are still
    /// referenced, so only then may unreferenced ones be deleted.
    enum class IndexState {
        Parsed,    ///< magic matched; `pages_` is the authoritative live set
        Missing,   ///< no index file at all (fresh store, or one deleted)
        Bad        ///< present but oversized / non-regular / wrong magic
    };

    bool writeIndex(std::string& err) const;
    IndexState readIndex();
    void writerLoop();
    void startWriter();
    void stopWriter();
    bool reconcileWriteFailures(std::string& err);
    void removeOrQueue(const std::string& file);
    void retryPendingDeletes();

    /// One sheet on its way to disk. The Page is carried rather than its RGBA
    /// expansion: ~1.9 MB instead of 7.76 MB, and it moves the conversion off
    /// the render thread along with the encode.
    struct PendingWrite {
        std::string       file;    ///< bare filename, matches HistoryPage::file
        ImageWriter::Page page;
    };

    /// Guards `queue_` + `writerQuit_` ONLY. `pages_`, `dir_` and the counters
    /// stay single-threaded (render thread), which is why the panel can read
    /// `pages()` without a lock the way it always has.
    mutable std::mutex              qMtx_;
    /// Signals both directions: work for the writer, room for the producer.
    mutable std::condition_variable qCv_;
    /// Signals "the queue is empty" for flushPending().
    mutable std::condition_variable qDoneCv_;
    /// The entry being encoded stays at the FRONT until its file exists, so
    /// `loadRgba` can always find a page that is not on disk yet.
    std::deque<PendingWrite>        queue_;
    /// Files whose PNG encode/commit failed. The writer only appends here;
    /// the render thread consumes it and is the sole owner of `pages_`.
    std::vector<std::string>        failedFiles_;
    std::vector<std::string>        pendingDeletes_;
    std::thread                     writer_;
    bool                            writerQuit_ = false;
    /// True between the writer thread's spawn and its exit — including an
    /// exit through `ThreadGuard`'s exception barrier. Both waits below key
    /// off it: a dead writer must fail the wait, not hang the render thread
    /// forever on a queue nothing will ever drain.
    bool                            writerAlive_ = false;

    std::string              dir_;
    std::vector<HistoryPage> pages_;      ///< newest first
    uint64_t                 nextJob_  = 1;
    uint64_t                 nextFile_ = 1;
    /// Wall-clock seconds since the last stored page, used to decide whether
    /// the next one continues the same job.
    int64_t                  lastPageEpoch_ = 0;
};

} // namespace pom2

#endif // POM2_PRINTER_HISTORY_H
