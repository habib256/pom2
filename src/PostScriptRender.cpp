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

// PostScriptRender — see header for why this delegates instead of parsing.

#include "PostScriptRender.h"

#include "ChildProcess.h"
#include "ThreadGuard.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace pom2 {

namespace {

/// Ghostscript is slow on a pathological job and POM2 must not wait forever
/// for one. Generous against a real page (a text page is well under a second
/// on any machine POM2 runs on) and short against a hang.
constexpr int kRenderTimeoutMs = 20000;
constexpr int kPollMs          = 10;

/// Refuse to render a page bigger than this. A guest can ask for any paper
/// size, and w*h bytes is allocated for the result: at 144 dpi a 100-inch
/// sheet is already 200 MB. Also the ceiling on a whole multi-page job: the
/// pages are unpacked into memory together.
constexpr std::size_t kMaxRasterBytes = 64u << 20;

/// Hard cap on the pages one job may hand back, so a runaway `showpage` loop
/// cannot turn into an unbounded vector of rasters. Well past any real job.
constexpr std::size_t kMaxPagesPerJob = 64;

/// Skip PGM header whitespace AND comments — `#` to end of line may appear
/// between any two tokens, which is the part of the format hand-rolled
/// parsers usually get wrong.
void skipPgmSpace(const uint8_t* d, std::size_t n, std::size_t& i)
{
    for (;;) {
        while (i < n && std::isspace(static_cast<unsigned char>(d[i]))) ++i;
        if (i < n && d[i] == '#') {
            while (i < n && d[i] != '\n') ++i;
            continue;
        }
        return;
    }
}

bool readPgmInt(const uint8_t* d, std::size_t n, std::size_t& i, long& out)
{
    skipPgmSpace(d, n, i);
    if (i >= n || !std::isdigit(static_cast<unsigned char>(d[i]))) return false;
    long v = 0;
    while (i < n && std::isdigit(static_cast<unsigned char>(d[i]))) {
        if (v > 100000000L) return false;            // absurd; bail rather than wrap
        v = v * 10 + (d[i] - '0');
        ++i;
    }
    out = v;
    return true;
}

/// One `P5` block: its geometry and where its raster starts and ends.
struct PgmBlock {
    long        w = 0, h = 0, maxval = 0;
    std::size_t data = 0;     // first raster byte
    std::size_t end  = 0;     // one past the last raster byte
};

/// Parse the `P5` block starting at `i`. Same rules as the single-page reader
/// always had; factored out so the pages after the first go through exactly
/// the same code rather than a second, looser copy of it.
bool readPgmBlock(const uint8_t* d, std::size_t n, std::size_t i,
                  PgmBlock& out, std::string& err)
{
    if (i >= n || n - i < 8) { err = "PGM too short"; return false; }
    if (d[i] != 'P' || d[i + 1] != '5') {
        err = "not a binary PGM (P5)";
        return false;
    }
    std::size_t p = i + 2;
    if (!readPgmInt(d, n, p, out.w) || !readPgmInt(d, n, p, out.h) ||
        !readPgmInt(d, n, p, out.maxval)) {
        err = "malformed PGM header";
        return false;
    }
    if (out.w <= 0 || out.h <= 0 || out.maxval <= 0 || out.maxval > 255) {
        err = "unsupported PGM geometry";
        return false;
    }
    // Exactly ONE whitespace byte separates the header from the data, and it
    // is part of the format — skipping "all whitespace" here would eat a
    // leading 0x20 pixel (a light-grey one) off the first row.
    if (p >= n || !std::isspace(static_cast<unsigned char>(d[p]))) {
        err = "missing PGM header terminator";
        return false;
    }
    ++p;
    const std::size_t need = static_cast<std::size_t>(out.w) *
                             static_cast<std::size_t>(out.h);
    if (need > kMaxRasterBytes) { err = "PGM raster too large"; return false; }
    if (n - p < need)           { err = "PGM truncated";        return false; }
    out.data = p;
    out.end  = p + need;
    return true;
}

/// Normalise a non-255 maxval so callers always see 0..255.
void normalisePgm(std::vector<uint8_t>& gray, long maxval)
{
    if (maxval == 255 || maxval <= 0) return;
    for (uint8_t& v : gray)
        v = static_cast<uint8_t>(v * 255 / maxval);
}

/// Remove `pom2ps_*` leftovers from an earlier RUN.
///
/// Every job stages a `.ps` and a `.pgm` under the scratch directory and the
/// Scrub guard below removes both on every exit path — but only on an exit.
/// A crash, a kill, or a power cut during a render leaves the pair behind,
/// and nothing ever collected them: the directory grew by up to a page raster
/// (megabytes) per lost job, for the life of the install. Swept once per
/// process, and only for files older than an hour so a CONCURRENT POM2's
/// in-flight job is never pulled out from under it.
void pruneStaleScratch(const std::string& dir)
{
    namespace fs = std::filesystem;
    static bool swept = false;
    if (swept) return;
    swept = true;

    std::error_code ec;
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(1);
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (const auto& e : it) {
        std::error_code entryEc;
        if (!e.is_regular_file(entryEc) || entryEc) continue;
        const std::string name = e.path().filename().string();
        if (name.rfind("pom2ps_", 0) != 0) continue;
        const auto when = fs::last_write_time(e.path(), entryEc);
        if (entryEc || when > cutoff) continue;
        fs::remove(e.path(), entryEc);
    }
}

std::string uniqueStem(const std::string& dir, const void* salt)
{
    // No Date/random needed: the address of the caller's request plus a
    // counter is unique within a process, and the files are removed before
    // this function's caller returns.
    static unsigned long counter = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "pom2ps_%lx_%lu",
                  static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(salt)),
                  counter++);
    return (std::filesystem::path(dir) / buf).string();
}

} // namespace

std::string escapeGsOutputFile(const std::string& path)
{
    std::string out;
    out.reserve(path.size() + 4);
    for (char c : path) {
        out.push_back(c);
        if (c == '%') out.push_back('%');
    }
    return out;
}

std::string findPostScriptInterpreter()
{
    // gs everywhere; gswin64c/gswin32c are the Windows console builds (the
    // plain `gswin64` opens a window, which is not what a spooler wants).
    for (const char* name : { "gs", "gswin64c", "gswin32c" }) {
        std::string p = ChildProcess::findOnPath(name);
        if (!p.empty()) return p;
    }
    return {};
}

bool looksLikePostScript(const uint8_t* data, std::size_t n)
{
    if (!data) return false;
    std::size_t i = 0;
    // A previous job's Ctrl-D and any stray whitespace may lead.
    while (i < n && (data[i] == kPsEndOfJob ||
                     std::isspace(static_cast<unsigned char>(data[i])))) ++i;
    return (i + 1 < n) && data[i] == '%' && data[i + 1] == '!';
}

bool parsePgm(const uint8_t* data, std::size_t n, PsRenderResult& out)
{
    out.ok = false;
    out.more.clear();
    out.extraPages = 0;
    if (!data) { out.error = "PGM too short"; return false; }

    PgmBlock first;
    if (!readPgmBlock(data, n, 0, first, out.error)) return false;
    out.w = static_cast<int>(first.w);
    out.h = static_cast<int>(first.h);
    out.gray.assign(data + first.data, data + first.end);
    normalisePgm(out.gray, first.maxval);

    // Every further `P5` block is another sheet of the SAME job: Ghostscript
    // writes each `showpage` into the same pgmraw file whenever the output
    // name carries no `%d`. Reading only the first is what truncated
    // multi-page jobs in silence.
    std::size_t i     = first.end;
    std::size_t bytes = out.gray.size();
    while (out.more.size() < kMaxPagesPerJob) {
        // Tolerate a stray newline between blocks; do NOT use skipPgmSpace,
        // whose `#` comment rule would run off into binary trailing data.
        while (i < n && std::isspace(static_cast<unsigned char>(data[i]))) ++i;
        if (i + 1 >= n || data[i] != 'P' || data[i + 1] != '5') break;
        PgmBlock next;
        std::string err;
        // Trailing junk that only LOOKS like a page: keep the pages already
        // read rather than failing the whole job.
        if (!readPgmBlock(data, n, i, next, err)) break;
        const std::size_t need = static_cast<std::size_t>(next.w) *
                                 static_cast<std::size_t>(next.h);
        if (bytes + need > kMaxRasterBytes) break;
        PsRenderPage page;
        page.w = static_cast<int>(next.w);
        page.h = static_cast<int>(next.h);
        page.gray.assign(data + next.data, data + next.end);
        normalisePgm(page.gray, next.maxval);
        out.more.push_back(std::move(page));
        bytes += need;
        i = next.end;
    }
    out.extraPages = static_cast<int>(out.more.size());

    out.ok = true;
    out.error.clear();
    return true;
}

bool renderPostScript(const std::string& interpreterPath,
                      const std::string& scratchDir,
                      const PsRenderRequest& req,
                      PsRenderResult& out)
{
    out = {};
    if (interpreterPath.empty()) {
        out.error = "no PostScript interpreter found (install Ghostscript)";
        return false;
    }
    if (req.postscript.empty()) { out.error = "empty job"; return false; }
    if (req.dpi <= 0 || req.widthIn <= 0.0 || req.heightIn <= 0.0) {
        out.error = "invalid page geometry";
        return false;
    }

    const int wpx = static_cast<int>(req.widthIn  * req.dpi + 0.5);
    const int hpx = static_cast<int>(req.heightIn * req.dpi + 0.5);
    if (wpx <= 0 || hpx <= 0 ||
        static_cast<std::size_t>(wpx) * hpx > kMaxRasterBytes) {
        out.error = "page too large to render";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(scratchDir, ec);
    pruneStaleScratch(scratchDir);
    const std::string stem = uniqueStem(scratchDir, &req);
    const std::string psPath  = stem + ".ps";
    const std::string pgmPath = stem + ".pgm";

    // Everything below must clean up both files on every exit path.
    //
    // ORDERING, and it matters: `scrub` is declared HERE, before the
    // ChildProcess below, so it is destroyed LAST — the interpreter is
    // reaped (or killed by ChildProcess's own destructor) before its input
    // and its raster are unlinked. Declaring the child first would invert
    // that and pull the `.ps` out from under a still-running gs, and the
    // `.pgm` out from under its final write. Keep this declaration above the
    // `ChildProcess gs`.
    struct Scrub {
        const std::string &a, &b;
        ~Scrub() {
            std::error_code e;
            std::filesystem::remove(a, e);
            std::filesystem::remove(b, e);
        }
    } scrub{psPath, pgmPath};

    {
        std::ofstream ps(psPath, std::ios::binary | std::ios::trunc);
        if (!ps || !ps.write(reinterpret_cast<const char*>(req.postscript.data()),
                             static_cast<std::streamsize>(req.postscript.size()))) {
            out.error = "cannot stage the job in " + scratchDir;
            return false;
        }
    }

    // -dSAFER is not optional: the job comes from EMULATED SOFTWARE, and
    // PostScript can open and delete host files. It confines the interpreter
    // to reading what it was given. -dPARANOIDSAFER is the older spelling and
    // is accepted as an alias by every version that matters.
    const std::vector<std::string> args = {
        "-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-dNOPROMPT",
        "-sDEVICE=pgmraw",
        "-r" + std::to_string(req.dpi),
        "-g" + std::to_string(wpx) + "x" + std::to_string(hpx),
        "-sOutputFile=" + escapeGsOutputFile(pgmPath),
        psPath,
    };

    ChildProcess gs;
    std::string err;
    if (!gs.start(interpreterPath, args, scratchDir, err)) {
        out.error = "cannot run " + interpreterPath + ": " + err;
        return false;
    }

    int waited = 0;
    while (gs.isRunning()) {
        if (waited >= kRenderTimeoutMs) {
            gs.stop(200);
            out.error = "the interpreter did not finish in " +
                        std::to_string(kRenderTimeoutMs / 1000) + " s";
            pom2::log().warn("LaserWriter", out.error);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        waited += kPollMs;
    }
    if (gs.lastExitCode() != 0) {
        out.error = "the interpreter rejected the job (exit " +
                    std::to_string(gs.lastExitCode()) + ")";
        pom2::log().warn("LaserWriter", out.error);
        return false;
    }

    std::ifstream in(pgmPath, std::ios::binary | std::ios::ate);
    if (!in) { out.error = "the interpreter produced no page"; return false; }
    const std::streampos end = in.tellg();
    // A multi-page job is several P5 blocks in this one file, so the ceiling
    // is the raster budget plus one header per page it may hold.
    if (end <= 0 ||
        static_cast<std::size_t>(end) > kMaxRasterBytes + 64 * kMaxPagesPerJob) {
        out.error = "the rendered page is empty or absurdly large";
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw(static_cast<std::size_t>(end));
    if (!in.read(reinterpret_cast<char*>(raw.data()),
                 static_cast<std::streamsize>(raw.size()))) {
        out.error = "short read on the rendered page";
        return false;
    }

    if (!parsePgm(raw.data(), raw.size(), out)) return false;

    pom2::log().info("LaserWriter",
        "Rendered a PostScript job: " + std::to_string(out.extraPages + 1) +
        " page(s) of " + std::to_string(out.w) + "x" + std::to_string(out.h) +
        " at " + std::to_string(req.dpi) + " dpi");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// PostScriptSpooler
// ─────────────────────────────────────────────────────────────────────────

PostScriptSpooler::~PostScriptSpooler()
{
    joinWorker();
}

void PostScriptSpooler::joinWorker()
{
    // Never under `mtx_`: the worker takes it to publish its result, so
    // joining with it held is a deadlock. Same shape, same reason, as
    // SpOverSlipLink::stop().
    if (worker_.joinable()) worker_.join();
}

void PostScriptSpooler::setScratchDir(std::string dir)
{
    std::lock_guard<std::mutex> lk(mtx_);
    scratchDir_ = std::move(dir);
}

void PostScriptSpooler::setPageGeometry(int dpi, double widthIn, double heightIn)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (dpi > 0)       dpi_      = dpi;
    if (widthIn  > 0.0) widthIn_  = widthIn;
    if (heightIn > 0.0) heightIn_ = heightIn;
}

void PostScriptSpooler::feed(const uint8_t* data, std::size_t n)
{
    if (!data || n == 0) return;

    std::thread stale;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (std::size_t i = 0; i < n; ++i) {
            if (data[i] == kPsEndOfJob) {
                // End of job. Anything after the Ctrl-D belongs to the NEXT
                // one, so it stays in `pending_` rather than being lost — and
                // the job that just closed is QUEUED even while a render is
                // in flight. Ignoring the separator when busy (which is what
                // this did) concatenated the two jobs into one buffer: the
                // second `%!PS-Adobe` landed mid-stream and the interpreter
                // saw one nonsensical job instead of two good ones.
                closeJobLocked();
                startNextLocked(stale);
                continue;
            }
            if (pending_.size() >= kMaxJobBytes) {
                // Drop rather than grow without bound. Logged once per job so
                // a runaway driver says so instead of looking like a hang.
                if (pending_.size() == kMaxJobBytes) {
                    pom2::log().warn("LaserWriter",
                        "PostScript job exceeded " +
                        std::to_string(kMaxJobBytes >> 20) +
                        " MB; the tail is being dropped");
                    pending_.push_back(data[i]);   // trip the == once only
                }
                continue;
            }
            pending_.push_back(data[i]);
        }
    }
    if (stale.joinable()) stale.join();
}

void PostScriptSpooler::flushNow()
{
    std::thread stale;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (pending_.empty()) return;
        closeJobLocked();
        startNextLocked(stale);
    }
    if (stale.joinable()) stale.join();
}

void PostScriptSpooler::closeJobLocked()
{
    // `mtx_` is held. The bytes since the last separator become a complete
    // job; anything that arrives next starts a fresh `pending_`.
    if (pending_.empty()) return;
    if (queued_.size() >= kMaxQueuedJobs) {
        pom2::log().warn("LaserWriter",
            "more than " + std::to_string(kMaxQueuedJobs) +
            " PostScript jobs are waiting; the oldest is being dropped");
        queued_.pop_front();
    }
    queued_.push_back(std::move(pending_));
    pending_.clear();
}

void PostScriptSpooler::startNextLocked(std::thread& retired)
{
    // `mtx_` is held. One interpreter at a time, and never while a finished
    // page is still uncollected — starting one would overwrite `done_`.
    if (busy_ || haveResult_ || queued_.empty()) return;
    // The previous job's worker has published and exited; hand its handle to
    // the caller, which joins it once the lock is dropped.
    retired = std::move(worker_);
    inFlight_ = std::move(queued_.front());
    queued_.pop_front();
    busy_ = true;

    PsRenderRequest req;
    req.postscript = inFlight_;
    req.dpi        = dpi_;
    req.widthIn    = widthIn_;
    req.heightIn   = heightIn_;
    const std::string scratch = scratchDir_;

    worker_ = pom2::guardedThread("LaserWriter", [this, req, scratch]() {
        PsRenderResult r;
        const std::string gsPath = findPostScriptInterpreter();
        if (gsPath.empty()) {
            r.ok = false;
            r.error = "no PostScript interpreter found — install Ghostscript, "
                      "or switch the head to its Diablo 630 mode";
        } else {
            (void)renderPostScript(gsPath, scratch, req, r);
        }
        std::lock_guard<std::mutex> lk(mtx_);
        done_       = std::move(r);
        haveResult_ = true;
        busy_       = false;
    });
}

bool PostScriptSpooler::takeResult(PsRenderResult& out)
{
    std::thread finished;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!haveResult_) return false;
        out         = std::move(done_);
        done_       = {};
        haveResult_ = false;
        // The worker has published and is about to exit; take the handle so
        // it can be joined outside the lock rather than leaking a thread per
        // page.
        if (!busy_) finished = std::move(worker_);
    }
    if (finished.joinable()) finished.join();
    // The result slot is free again, so a job that arrived while this one was
    // rendering can start now. This is the only place that can notice — the
    // worker itself must not spawn its own successor.
    std::thread stale;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        startNextLocked(stale);
    }
    if (stale.joinable()) stale.join();
    return true;
}

bool PostScriptSpooler::busy() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return busy_;
}

std::size_t PostScriptSpooler::pendingBytes() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_.size();
}

std::size_t PostScriptSpooler::queuedJobs() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return queued_.size();
}

void PostScriptSpooler::reset()
{
    // Wait for any render rather than abandoning it: the child process is
    // supervised by a ChildProcess living on the worker's stack, and dropping
    // the thread would leave it unreaped.
    joinWorker();
    std::lock_guard<std::mutex> lk(mtx_);
    pending_.clear();
    queued_.clear();
    inFlight_.clear();
    done_       = {};
    haveResult_ = false;
    busy_       = false;
}

} // namespace pom2
