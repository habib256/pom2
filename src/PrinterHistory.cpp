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

// PrinterHistory implementation. See the header for the on-disk layout and
// why the index is a tab-separated text file rather than JSON.

#include "PrinterHistory.h"

#include "AtomicFileReplace.h"
#include "Logger.h"
#include "ThreadGuard.h"

// Declarations only — the single non-static stb implementations live in
// Pom2HgrPaintHost.cpp, the same arrangement ImageWriterPdf.cpp documents.
#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace pom2 {

namespace {

namespace fs = std::filesystem;

constexpr const char* kIndexName = "index.txt";
/// Version tag on the index's first line. An index POM2 does not recognise is
/// ignored rather than half-parsed — a stale format must not produce garbage
/// entries pointing at files that mean something else.
constexpr const char* kIndexMagic = "pom2-printer-history\t1";

// A normal printer page is only a few megapixels. Keep enough headroom for
// long paper while refusing compressed image bombs before stb allocates the
// decoded raster (which is then copied into the caller's RGBA vector).
constexpr uintmax_t kMaxPagePngBytes = 64u * 1024u * 1024u;
constexpr int kMaxPageDimension = 8192;
constexpr uint64_t kMaxPagePixels = 16u * 1024u * 1024u;

/// Sheets ejected within this many seconds of each other belong to the same
/// print job. A multi-page document ejects its sheets seconds apart; a new
/// document minutes later is a new job.
constexpr int64_t kJobGapSeconds = 90;

int64_t nowEpoch()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string nowStamp()
{
    const std::time_t t = static_cast<std::time_t>(nowEpoch());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    // 72, not 20: `%04d` is a MINIMUM width, and nothing in the type system
    // stops `tm_year + 1900` (an int) from needing 11 characters, so the
    // fields' worst case is 72 bytes. GCC 13 says so out loud under -O3 +
    // _FORTIFY_SOURCE (-Wformat-truncation), and it is right that a 32-byte
    // buffer can only answer by truncating — which would put a malformed
    // timestamp in the durable print-history index rather than fail loudly.
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

/// Same instant as `nowStamp`, but shaped for a filename (no spaces, no
/// colons — a colon is illegal on Windows and awkward everywhere).
std::string fileStamp()
{
    const std::time_t t = static_cast<std::time_t>(nowEpoch());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

/// Strip anything that would break a tab-separated record. Cheap insurance:
/// the only free-text field is a timestamp POM2 formats itself today, but a
/// label field is the obvious next addition and this is where it would bite.
std::string sanitise(std::string s)
{
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

bool validPageFile(const std::string& name)
{
    // `%06llu` is a MINIMUM width, not a cap: after p999999.png the next
    // valid name has seven digits. Accept the full uint64_t decimal width.
    if (name.size() < 11 || name.size() > 25 || name.front() != 'p' ||
        name.compare(name.size() - 4, 4, ".png") != 0)
        return false;
    return std::all_of(name.begin() + 1, name.end() - 4,
                       [](unsigned char c) { return c >= '0' && c <= '9'; });
}

} // namespace

// ── Open / index ─────────────────────────────────────────────────────────

bool PrinterHistory::open(const std::string& dir, std::string& err)
{
    // Retarget the store only once the writer is idle: it caches `dir_` per
    // job, and swapping the directory under a queued page would file it in
    // the wrong place. Also drains, so nothing pending is dropped.
    stopWriter();
    std::string priorErr;
    if (!reconcileWriteFailures(priorErr) && !priorErr.empty())
        pom2::log().warn("PrinterHistory", priorErr);
    retryPendingDeletes();
    pendingDeletes_.clear(); // pending names belong to the previous `dir_`

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        err = "cannot create " + dir + ": " + ec.message();
        return false;
    }
    dir_ = dir;
    pages_.clear();
    nextJob_ = nextFile_ = 1;
    lastPageEpoch_ = 0;
    const IndexState state = readIndex();

    // The sweep below deletes every PNG the index does not mention, and the
    // index is the ONLY record of which those are. An index that did not
    // parse therefore mentions nothing, and the sweep turned "POM2 cannot
    // read this file" into "POM2 deleted up to 200 of your printouts" —
    // silently, on open, for a file that is one truncated write or one
    // foreign version tag away. So: sweep only behind an index we actually
    // read. A bad one is moved aside under a name that says what it is (the
    // user may want to look at it, and a fresh index is written on the next
    // page) and every PNG is left exactly where it is.
    if (state == IndexState::Bad) {
        const fs::path bad = fs::path(dir_) / kIndexName;
        std::error_code mvEc;
        fs::rename(bad, fs::path(dir_) /
                            (std::string(kIndexName) + ".bad-" + fileStamp()),
                   mvEc);
        pom2::log().warn("PrinterHistory",
            mvEc ? "unreadable index kept in place (" + mvEc.message() +
                       ") — printouts left untouched"
                 : "unreadable index moved aside — printouts left untouched");
        return true;
    }
    if (state == IndexState::Missing) {
        // No index and no way to tell a crash orphan from a printout whose
        // index a user deleted by accident. Keeping the files costs disk;
        // deleting them costs the printouts. The next successful index write
        // makes the sweep possible again.
        return true;
    }

    // Clean safe, unreferenced page files left by a prior locked-file delete
    // or a process killed between PNG creation and index repair.
    std::vector<std::string> live;
    live.reserve(pages_.size());
    for (const auto& p : pages_) live.push_back(p.file);
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        const std::string file = entry.path().filename().string();
        std::error_code typeEc;
        if (entry.is_regular_file(typeEc) && validPageFile(file) &&
            std::find(live.begin(), live.end(), file) == live.end())
            removeOrQueue(file);
    }
    return true;
}

PrinterHistory::IndexState PrinterHistory::readIndex()
{
    const fs::path path = fs::path(dir_) / kIndexName;
    std::error_code ec;
    constexpr std::uintmax_t kMaxIndexBytes = 1024u * 1024u;
    const auto bytes = fs::file_size(path, ec);
    std::error_code existsEc;
    const bool present = fs::exists(path, existsEc) && !existsEc;
    if ((!ec && bytes > kMaxIndexBytes) || (ec && present)) {
        pom2::log().warn("PrinterHistory",
                         "refusing oversized or non-regular index");
        return IndexState::Bad;
    }
    std::ifstream in(path);
    if (!in) return present ? IndexState::Bad : IndexState::Missing;

    std::string line;
    if (!std::getline(in, line) || line.rfind(kIndexMagic, 0) != 0) {
        pom2::log().warn("PrinterHistory",
                         "unrecognised index format — starting a fresh history");
        return IndexState::Bad;
    }

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        HistoryPage p;
        std::string job, model, ribbon, pw, pl, w, h, dpi;
        // A truncated final line (killed mid-write) simply fails here and is
        // skipped, which is why the index is append-friendly plain text.
        if (!std::getline(ls, p.file, '\t'))    continue;
        if (!std::getline(ls, p.savedAt, '\t')) continue;
        if (!std::getline(ls, job, '\t'))       continue;
        if (!std::getline(ls, model, '\t'))     continue;
        if (!std::getline(ls, ribbon, '\t'))    continue;
        if (!std::getline(ls, pw, '\t'))        continue;
        if (!std::getline(ls, pl, '\t'))        continue;
        if (!std::getline(ls, w, '\t'))         continue;
        if (!std::getline(ls, h, '\t'))         continue;
        if (!std::getline(ls, dpi))             continue;
        // Never let an edited/corrupt index turn erase/trim into deletion
        // outside the history directory.
        if (!validPageFile(p.file)) continue;

        try {
            p.job    = std::stoull(job);
            p.model  = std::stoi(model);
            p.ribbon = std::stoi(ribbon);
            p.paperW = std::stod(pw);
            p.paperL = std::stod(pl);
            p.w      = std::stoi(w);
            p.h      = std::stoi(h);
            p.dpi    = std::stoi(dpi);
        } catch (...) {
            continue;                    // malformed record, skip it
        }

        // Drop entries whose PNG has gone — a user who deleted files by hand
        // should not get a history full of dead rows.
        std::error_code pageEc;
        if (!fs::exists(fs::path(dir_) / p.file, pageEc)) continue;

        nextJob_ = std::max(nextJob_, p.job + 1);
        // File names are pNNNNNN.png; recover the counter so a new page never
        // overwrites an old one.
        try {
            const uint64_t fileNo = std::stoull(
                p.file.substr(1, p.file.size() - 5));
            if (fileNo != std::numeric_limits<uint64_t>::max())
                nextFile_ = std::max(nextFile_, fileNo + 1);
        } catch (...) {
            continue;
        }
        // The index is user-editable and may repeat the same existing PNG an
        // arbitrary number of times. Enforce the documented store cap while
        // parsing as well as while adding, otherwise open() itself can OOM.
        if (pages_.size() == kMaxPages) pages_.erase(pages_.begin());
        pages_.push_back(std::move(p));
    }

    // Stored oldest-first; presented newest-first.
    std::reverse(pages_.begin(), pages_.end());
    return IndexState::Parsed;
}

bool PrinterHistory::writeIndex(std::string& err) const
{
    const fs::path final = fs::path(dir_) / kIndexName;
    // Unique per process + per call: two POM2 instances printing into one
    // per-user history both wrote `index.txt.tmp`. See tempSiblingPath().
    const fs::path tmp   = tempSiblingPath(final);

    // Anything already at the temp path is ours to clear or a plant to
    // refuse — trunc would otherwise write through a symlink. See
    // prepareTempPath().
    std::error_code tmpEc;
    if (!prepareTempPath(tmp, tmpEc)) {
        err = "cannot prepare " + tmp.string() + ": " + tmpEc.message();
        return false;
    }
    // The temp name is unique per call, so every failure path must remove it
    // or the history directory grows one orphan per failed write.
    auto discardTemp = [&tmp] {
        std::error_code rm;
        fs::remove(tmp, rm);
    };
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { err = "cannot write " + tmp.string(); return false; }
        out << kIndexMagic << "\n";
        // Oldest first on disk: appending is then the natural order, and a
        // reader that stops early still has the oldest records intact.
        for (auto it = pages_.rbegin(); it != pages_.rend(); ++it) {
            out << sanitise(it->file) << '\t' << sanitise(it->savedAt) << '\t'
                << it->job << '\t' << it->model << '\t' << it->ribbon << '\t'
                << it->paperW << '\t' << it->paperL << '\t'
                << it->w << '\t' << it->h << '\t' << it->dpi << '\n';
        }
        out.flush();
        out.close();
        if (!out) {
            err = "write failed: " + tmp.string();
            discardTemp();
            return false;
        }
    }

    // Rename over the old index: a crash mid-write then leaves the previous
    // index intact rather than a half-truncated one.
    std::error_code ec;
    if (!replaceFileAtomic(tmp, final, ec)) {
        err = "cannot replace the index: " + ec.message();
        discardTemp();
        return false;
    }
    return true;
}

// ── Background writer ────────────────────────────────────────────────────

PrinterHistory::~PrinterHistory()
{
    stopWriter();
    std::string err;
    if (!reconcileWriteFailures(err) && !err.empty())
        pom2::log().warn("PrinterHistory", err);
}

void PrinterHistory::startWriter()
{
    if (writer_.joinable()) {
        bool alive = false;
        {
            std::lock_guard<std::mutex> lk(qMtx_);
            alive = writerAlive_;
        }
        if (alive) return;
        // The previous writer is gone (it threw, and ThreadGuard turned that
        // into a return instead of a std::terminate). Reap it and spawn a
        // fresh one rather than queueing onto a thread that will never run
        // again — that is what used to hang `flushPending` for good.
        writer_.join();
        std::lock_guard<std::mutex> lk(qMtx_);
        writerQuit_ = false;
    }
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        writerAlive_ = true;
    }
    writer_ = pom2::guardedThread("PrinterHistory", [this] {
        // Clears the liveness flag on EVERY exit, including the one through
        // the exception barrier, and wakes whoever is waiting on the queue.
        struct AliveGuard {
            PrinterHistory* h;
            ~AliveGuard()
            {
                {
                    std::lock_guard<std::mutex> lk(h->qMtx_);
                    h->writerAlive_ = false;
                }
                h->qDoneCv_.notify_all();
                h->qCv_.notify_all();
            }
        } guard{this};
        writerLoop();
    });
}

void PrinterHistory::stopWriter()
{
    if (!writer_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        writerQuit_ = true;
    }
    // The loop only returns once the queue is EMPTY, so this drains rather
    // than discards: a sheet ejected a frame before the user quit still lands
    // on disk.
    qCv_.notify_all();
    writer_.join();
    std::lock_guard<std::mutex> lk(qMtx_);
    writerQuit_ = false;
}

void PrinterHistory::writerLoop()
{
    for (;;) {
        PendingWrite job;
        std::string  dir;
        {
            std::unique_lock<std::mutex> lk(qMtx_);
            qCv_.wait(lk, [this] { return writerQuit_ || !queue_.empty(); });
            if (queue_.empty()) return;         // asked to quit, nothing left
            // COPIED, not moved: the entry has to stay visible at the front
            // until its file exists, so loadRgba() can serve a sheet the user
            // clicked on before the encode finished.
            job = queue_.front();
            dir = dir_;
        }

        std::vector<uint8_t> rgba;
        ImageWriter::pageToRgba(job.page, rgba);
        const fs::path out = fs::path(dir) / job.file;
        const fs::path tmp = tempSiblingPath(out);
        std::error_code tmpEc;
        const bool tmpOk = prepareTempPath(tmp, tmpEc);
        bool written = tmpOk &&
                       rgba.size() >= static_cast<size_t>(job.page.w) *
                                      static_cast<size_t>(job.page.h) * 4 &&
                       stbi_write_png(tmp.string().c_str(), job.page.w,
                                      job.page.h, 4, rgba.data(),
                                      job.page.w * 4);
        std::error_code commitEc;
        if (written) written = replaceFileAtomic(tmp, out, commitEc);
        if (!written) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
            pom2::log().warn("PrinterHistory",
                             "cannot write " + out.string() +
                             (commitEc ? ": " + commitEc.message() : ""));
        }

        {
            std::lock_guard<std::mutex> lk(qMtx_);
            if (!written) failedFiles_.push_back(job.file);
            if (!queue_.empty()) queue_.pop_front();
        }
        qDoneCv_.notify_all();
        qCv_.notify_all();                      // a producer may want the room
    }
}

void PrinterHistory::flushPending()
{
    {
        std::unique_lock<std::mutex> lk(qMtx_);
        // `|| !writerAlive_`: this runs on the ImGui render thread (erase,
        // clear, quit), and waiting on "the queue drains" alone means waiting
        // forever when the thread that drains it has died — the window stops
        // painting, the cancel button included. A dead writer instead fails
        // the accepted pages the way a failed encode does: they leave the
        // queue, land in `failedFiles_`, and reconcileWriteFailures() drops
        // their rows from the durable index so nothing points at a PNG that
        // will never exist.
        qDoneCv_.wait(lk, [this] { return queue_.empty() || !writerAlive_; });
        if (!queue_.empty()) {
            pom2::log().warn("PrinterHistory",
                "the page writer is gone — discarding " +
                std::to_string(queue_.size()) + " unwritten page(s)");
            for (const auto& q : queue_) failedFiles_.push_back(q.file);
            queue_.clear();
        }
    }
    retryPendingDeletes();
    std::string err;
    if (!reconcileWriteFailures(err) && !err.empty())
        pom2::log().warn("PrinterHistory", err);
}

size_t PrinterHistory::pendingWrites() const
{
    std::lock_guard<std::mutex> lk(qMtx_);
    return queue_.size();
}

// ── Storing ──────────────────────────────────────────────────────────────

bool PrinterHistory::addPage(const ImageWriter::Page& page, int model,
                             int ribbon, double paperW, double paperL,
                             std::string& err)
{
    if (dir_.empty()) { err = "history is not open"; return false; }
    const bool dimensionsOverflow =
        page.w > 0 && page.h > 0 &&
        static_cast<size_t>(page.w) >
            std::numeric_limits<size_t>::max() / static_cast<size_t>(page.h);
    const size_t expectedPixels =
        (page.w > 0 && page.h > 0 && !dimensionsOverflow)
            ? static_cast<size_t>(page.w) * static_cast<size_t>(page.h)
            : 0;
    if (expectedPixels == 0 || page.pix.size() != expectedPixels) {
        err = "invalid page raster dimensions";
        return false;
    }

    char name[32];
    std::snprintf(name, sizeof(name), "p%06llu.png",
                  static_cast<unsigned long long>(nextFile_));

    const int64_t now = nowEpoch();
    const auto previousPages = pages_;
    const uint64_t previousNextJob = nextJob_;
    const uint64_t previousNextFile = nextFile_;
    const int64_t previousLastPage = lastPageEpoch_;
    HistoryPage p;
    p.file    = name;
    p.savedAt = nowStamp();
    // Sheets ejected close together are one document; a sheet much later is
    // a new one. Without this every page of a report is its own "job" and
    // re-preview becomes useless.
    p.job = (lastPageEpoch_ != 0 && now - lastPageEpoch_ <= kJobGapSeconds)
                ? nextJob_ - 1
                : nextJob_++;
    p.model  = model;
    p.ribbon = ribbon;
    p.paperW = paperW;
    p.paperL = paperL;
    p.w      = page.w;
    p.h      = page.h;
    p.dpi    = page.dpi;

    // Treat metadata as a transaction. In particular, do not queue the PNG
    // until the index commit succeeds: a locked index on Windows used to make
    // addPage return false while the writer still created an unindexed orphan.
    lastPageEpoch_ = now;
    ++nextFile_;
    pages_.insert(pages_.begin(), std::move(p));   // newest first

    std::vector<std::string> evicted;
    while (pages_.size() > kMaxPages) {
        evicted.push_back(pages_.back().file);
        pages_.pop_back();
    }

    if (!writeIndex(err)) {
        pages_ = previousPages;
        nextJob_ = previousNextJob;
        nextFile_ = previousNextFile;
        lastPageEpoch_ = previousLastPage;
        return false;
    }

    // Hand the sheet to the writer thread rather than encoding it here: this
    // runs on the ImGui render thread, once per ejected sheet, and a Letter
    // page at 144 dpi costs ~100-140 ms to convert and deflate.
    startWriter();
    {
        std::unique_lock<std::mutex> lk(qMtx_);
        // Same liveness escape as flushPending: a full queue behind a dead
        // writer must not block the render thread. The push still happens —
        // startWriter() above respawned the thread, so the page has a writer
        // again; the predicate only stops the wait from being unbounded.
        qCv_.wait(lk, [this] {
            return queue_.size() < kMaxPending || !writerAlive_;
        });
        queue_.push_back(PendingWrite{name, page});
    }
    qCv_.notify_all();

    // The committed index no longer references evicted pages. Deletion is
    // best effort: a transient Windows file lock may leave an orphan, but can
    // no longer leave a live row pointing at a missing file.
    for (const auto& file : evicted) removeOrQueue(file);
    return true;
}

bool PrinterHistory::reconcileWriteFailures(std::string& err)
{
    std::vector<std::string> failed;
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        failed.swap(failedFiles_);
    }
    if (failed.empty()) return true;

    pages_.erase(std::remove_if(pages_.begin(), pages_.end(),
                                [&](const HistoryPage& p) {
                                    return std::find(failed.begin(), failed.end(),
                                                     p.file) != failed.end();
                                }),
                 pages_.end());

    std::string indexErr;
    const bool indexOk = dir_.empty() || writeIndex(indexErr);
    err = "discarded " + std::to_string(failed.size()) +
          " print-history page(s) whose PNG could not be written";
    if (!indexOk) err += "; " + indexErr;
    return false;
}

bool PrinterHistory::pollWriteFailures(std::string& err)
{
    retryPendingDeletes();
    return reconcileWriteFailures(err);
}

void PrinterHistory::removeOrQueue(const std::string& file)
{
    if (!validPageFile(file)) return;
    bool stillQueued = false;
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        stillQueued = std::any_of(queue_.begin(), queue_.end(),
                                  [&](const PendingWrite& q) {
                                      return q.file == file;
                                  });
    }
    std::error_code ec;
    const bool removed = fs::remove(fs::path(dir_) / file, ec);
    if ((ec || stillQueued) &&
        std::find(pendingDeletes_.begin(), pendingDeletes_.end(), file) ==
            pendingDeletes_.end()) {
        pendingDeletes_.push_back(file);
        if (ec)
            pom2::log().warn("PrinterHistory",
                             "could not delete " + file + ": " + ec.message());
    } else if (removed) {
        pendingDeletes_.erase(std::remove(pendingDeletes_.begin(),
                                          pendingDeletes_.end(), file),
                              pendingDeletes_.end());
    }
}

void PrinterHistory::retryPendingDeletes()
{
    const auto pending = pendingDeletes_;
    for (const auto& file : pending) removeOrQueue(file);
}

// ── Reading back ─────────────────────────────────────────────────────────

bool PrinterHistory::loadRgba(const HistoryPage& p, std::vector<uint8_t>& rgba,
                              int& w, int& h, std::string& err) const
{
    if (!validPageFile(p.file)) {
        err = "invalid history page filename";
        return false;
    }
    // A sheet the writer has not reached yet is served straight from the
    // queue. The panel puts a freshly archived page on screen within a frame,
    // which is well inside the ~100 ms its encode takes.
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        for (const auto& q : queue_) {
            if (q.file != p.file) continue;
            ImageWriter::pageToRgba(q.page, rgba);
            w = q.page.w;
            h = q.page.h;
            return true;
        }
    }

    const fs::path full = fs::path(dir_) / p.file;
    std::error_code ec;
    const uintmax_t fileBytes = fs::file_size(full, ec);
    if (ec || fileBytes > kMaxPagePngBytes) {
        err = ec ? "cannot stat " + full.string()
                 : "history page exceeds the encoded-size limit";
        return false;
    }
    int infoW = 0, infoH = 0, infoComp = 0;
    if (!stbi_info(full.string().c_str(), &infoW, &infoH, &infoComp) ||
        infoW <= 0 || infoH <= 0 || infoW > kMaxPageDimension ||
        infoH > kMaxPageDimension ||
        static_cast<uint64_t>(infoW) * static_cast<uint64_t>(infoH) >
            kMaxPagePixels) {
        err = "history page dimensions are invalid or too large";
        return false;
    }
    int comp = 0;
    unsigned char* data = stbi_load(full.string().c_str(), &w, &h, &comp, 4);
    if (!data) {
        err = "cannot read " + full.string();
        return false;
    }
    if (w <= 0 || h <= 0 || w > kMaxPageDimension ||
        h > kMaxPageDimension ||
        static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kMaxPagePixels) {
        stbi_image_free(data);
        err = "decoded history page dimensions are too large";
        return false;
    }
    rgba.assign(data, data + static_cast<size_t>(w) * h * 4);
    stbi_image_free(data);
    return true;
}

std::vector<const HistoryPage*> PrinterHistory::jobPages(uint64_t job) const
{
    std::vector<const HistoryPage*> out;
    for (const auto& p : pages_)
        if (p.job == job) out.push_back(&p);
    // `pages_` is newest first; a job reads oldest first.
    std::reverse(out.begin(), out.end());
    return out;
}

// ── Deleting ─────────────────────────────────────────────────────────────

bool PrinterHistory::erase(const HistoryPage& p, std::string& err)
{
    if (!validPageFile(p.file)) {
        err = "invalid history page filename";
        return false;
    }
    // Never delete a file the writer is still about to create — it would be
    // resurrected moments later as an orphan the index does not mention.
    // A user-paced action can afford the wait; a per-sheet encode could not.
    flushPending();
    const std::string file = p.file;         // `p` may point into pages_
    const auto previousPages = pages_;
    pages_.erase(std::remove_if(pages_.begin(), pages_.end(),
                                [&](const HistoryPage& q) { return q.file == file; }),
                 pages_.end());
    if (!writeIndex(err)) {
        pages_ = previousPages;
        return false;
    }
    removeOrQueue(file);
    return true;
}

bool PrinterHistory::clear(std::string& err)
{
    flushPending();                          // see erase()
    const auto previousPages = pages_;
    pages_.clear();
    if (!writeIndex(err)) {
        pages_ = previousPages;
        return false;
    }
    for (const auto& p : previousPages) removeOrQueue(p.file);
    lastPageEpoch_ = 0;
    return true;
}

} // namespace pom2
