// POM2 — GPL-3.0-or-later
//
// MediaAutosave — background persistence for WHOLE-IMAGE removable media:
// the 5.25" `DiskImage` and the 3.5" `Disk35Image`, wherever they are mounted
// (Disk II drives, the //c+ Sony drives, SmartPort / Liron 3.5" units).
//
// Block devices already autosave (`Block512Backing_WriteBack.cpp`); a floppy
// only reached its host file on eject, swap, quit or profile switch, so
// anything that inspected the file while the disk was in the drive — a test,
// a script, a second program, a user's backup — saw the stale bytes, and a
// crash lost the whole session's writes. This is the same policy for them:
//
//   * a guest write bumps the medium's write serial;
//   * once the medium has been QUIET for `kQuiet` (a DOS SAVE is one burst
//     of sectors, and each capture costs the rewind history — see below), the
//     poll captures the complete payload under `stateMutex` (a memcpy) and
//     hands it to the injected `MediaCommitExecutor` — on the desktop ONE
//     worker thread, owned by the runtime layer — which writes it unlocked;
//   * dirty state stays set until the commit lands, and is retired only if
//     nothing was written since the capture — a failure keeps everything and
//     retries after `kRetry`, backing off to `kRetryMax` while it keeps
//     failing.
//
// Ordering. The explicit paths (eject, swap, flushAll, the //c+ firmware
// eject queue) commit on their own threads, so two captures of the same mount
// can race. Every capture takes a process-wide sequence number, and
// `commitMediaInOrder` refuses to let an OLDER capture replace a file a newer
// capture of the same mount already reached: the newest capture always
// carries every unconfirmed write (dirty flags are kept until confirmation),
// so "newest wins" is exactly right. The same mutex serialises every media
// commit, which is also what a sector format's read-modify-write needs.
//
// External change. A mounted image is a FILE, and the user's other tools
// (a file utility, a script, a second emulator, a sync client restoring an
// older copy) can rewrite it while it sits in the drive. Every commit used to
// replace the file with the mount's view of it, so whatever the other program
// wrote was lost without a word — and for a sector format, whose commit lays
// the guest's dirty tracks over the file's current bytes, the result was a
// disk made of two unrelated states (a catalog track from one, the data it
// names from the other). So a commit first checks that the file is still the
// one this process knows (`MediaFileStamp`): the bytes the mount read, or the
// bytes a commit of ours left there. When it is not, the file is LEFT ALONE
// and the guest's complete version goes to a sibling
// (`<name>.guest-<timestamp><ext>`), for this commit and every later one of
// that mount — the mount is *diverted*. Nothing is merged: two programs that
// both wrote a volume cannot be reconciled below the file system, and the
// user is the only one who knows which half they want.
// [D] Casso `ExternalChangePolicy.cpp` @ 2d899adf — the same policy (the
//     guest's version to a timestamped sibling, the external file untouched),
//     which is where the idea was taken from; the mechanism here is POM2's.
//
// Header-only on purpose: `DiskImage.cpp` and `Disk35Image.cpp` are linked by
// well over a hundred targets, and none of them should need a new source.
// Media layer: no threads here (cmake/Pom2Architecture.cmake) — the worker
// lives beside the block executor in BlockWriteBackExecutor.cpp.

#ifndef POM2_MEDIA_AUTOSAVE_H
#define POM2_MEDIA_AUTOSAVE_H

#include "Logger.h"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace pom2 {

struct MediaCommitResult {
    bool        ok = false;
    std::string error;
    /// Non-empty when the image file was changed by another program and the
    /// guest's version went to this sibling instead (see the file comment).
    /// `ok` is true then: the guest's writes ARE on disk, just not in the
    /// file the mount names.
    std::string divertedTo;
};

/// What a media file looked like when this process last knew its contents:
/// at mount, or after one of its own commits. Size and modification time are
/// the cheap test; `hash` (FNV-1a 64 of the bytes, 0 = not taken) settles a
/// file whose time moved while its contents did not — a `touch`, a backup
/// tool, a copy restored over itself — which must not be read as a change.
struct MediaFileStamp {
    bool           known = false;   ///< false: no check is made against it
    std::uintmax_t size  = 0;
    std::int64_t   mtime = 0;       ///< file_time_type ticks
    std::uint64_t  hash  = 0;

    bool sameTimeAndSize(const MediaFileStamp& o) const {
        return known && o.known && size == o.size && mtime == o.mtime;
    }
};

inline std::uint64_t hashMediaBytes(const std::uint8_t* data, std::size_t n,
                                    std::uint64_t h = 0xcbf29ce484222325ull)
{
    for (std::size_t i = 0; i < n; ++i) { h ^= data[i]; h *= 0x100000001b3ull; }
    return h ? h : 1;   // 0 is "not taken"
}

/// Stat `path`; with `withHash`, read it too. Not `known` when the file is
/// missing or unreadable. Never called with `stateMutex` held.
inline MediaFileStamp stampMediaFile(const std::string& path, bool withHash)
{
    namespace fs = std::filesystem;
    MediaFileStamp s;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return s;
    const auto size = fs::file_size(path, ec);
    if (ec) return s;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return s;
    s.size  = size;
    s.mtime = static_cast<std::int64_t>(time.time_since_epoch().count());
    if (withHash) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return s;
        std::uint64_t h = 0xcbf29ce484222325ull;
        char buf[65536];
        while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
            const auto got = static_cast<std::size_t>(f.gcount());
            for (std::size_t i = 0; i < got; ++i) {
                h ^= static_cast<std::uint8_t>(buf[i]);
                h *= 0x100000001b3ull;
            }
            if (!f) break;
        }
        s.hash = h ? h : 1;
    }
    s.known = true;
    return s;
}

/// The stamp of a file whose bytes the caller has just read: stat FIRST, then
/// read, then call this with the bytes — a writer that slips in between is
/// then seen as a change at the next commit instead of being missed.
inline MediaFileStamp stampMediaFile(const std::string& path,
                                     const std::uint8_t* data, std::size_t n)
{
    MediaFileStamp s = stampMediaFile(path, /*withHash=*/false);
    if (s.known) s.hash = hashMediaBytes(data, n);
    return s;
}

/// One commit handed to the executor. `wait()` may block; `ready()` never
/// does. Dropping the handle must not block either: an image is dropped under
/// `stateMutex` (eject, remount) while its commit may still be running.
class MediaCommitOperation {
public:
    virtual ~MediaCommitOperation() = default;
    virtual bool ready() const = 0;
    virtual MediaCommitResult wait() = 0;
};

/// The runtime transport that runs a commit off the machine lock — injected,
/// like `Block512Backing::WriteBackExecutor`, because the media layer owns no
/// threads. The desktop one is `pom2::mediaCommitExecutor()`
/// (BlockWriteBackExecutor.h): one worker, commits in submission order.
class MediaCommitExecutor {
public:
    virtual ~MediaCommitExecutor() = default;
    /// Must not block: it is called with `stateMutex` held.
    virtual std::shared_ptr<MediaCommitOperation>
    submit(std::function<MediaCommitResult()> work) = 0;
};

namespace detail {
struct MediaCommitOrder {
    struct Mount {
        uint64_t    landed = 0;   ///< newest capture sequence that reached disk
        std::string divertedTo;   ///< where this mount writes since a conflict
    };
    std::mutex                                      m;
    std::unordered_map<std::string, Mount>          mounts;  ///< path + lineage
    /// What a commit of THIS process last left at a path, whichever mount
    /// made it: two drives holding one image must not take each other's
    /// saves for a stranger's.
    std::unordered_map<std::string, MediaFileStamp> own;     ///< path
    static MediaCommitOrder& instance() {
        // Leaked: a commit may still be finishing while statics are torn down.
        static MediaCommitOrder* o = new MediaCommitOrder;
        return *o;
    }
};

/// `<dir>/<stem>.guest-YYYYMMDD-HHMMSS<ext>`, never an existing file. The
/// extension is kept last so the copy mounts as what it is.
inline std::string divertedSiblingFor(const std::string& path)
{
    namespace fs = std::filesystem;
    const fs::path p(path);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    const std::string base = p.stem().string() + ".guest-" + stamp;
    for (int n = 1; n < 1000; ++n) {
        fs::path candidate = p.parent_path() /
            (base + (n > 1 ? "-" + std::to_string(n) : std::string()) +
             p.extension().string());
        std::error_code ec;
        if (!fs::exists(candidate, ec)) return candidate.string();
    }
    return (p.parent_path() / (base + "-x" + p.extension().string())).string();
}
}  // namespace detail

/// A fresh capture sequence number (see the file comment), also used to name
/// a mount's lineage. Never 0.
inline uint64_t nextMediaCaptureSeq()
{
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

/// The file work of one commit. `target` is where to write: the image, or
/// the sibling a diverted mount writes to. `wholeSurface` is true for the
/// sibling — the callee must then write the medium's COMPLETE contents and
/// take nothing from the file at the mount's own path, which belongs to
/// somebody else now.
using MediaCommitWrite =
    std::function<bool(const std::string& target, bool wholeSurface, std::string& error)>;

/// Commit one whole-image capture of `path` taken at `seq` from the mount
/// `lineage`, in order: when a NEWER capture of the same mount has already
/// landed, this one is superseded and succeeds without writing. Ordering is
/// per mount, not per file — two drives holding the same image each save
/// their own tracks (a sector format merges them through its read-modify-
/// write), and one must never discard the other's. `seq == 0` (a payload
/// assembled by hand) is treated as the newest capture. The file work is
/// serialised against every other media commit.
///
/// `mounted` is the file as the mount read it (`known == false` skips the
/// external-change check: a payload assembled by hand, a test). When the
/// file is neither that nor what a commit of this process left there, the
/// mount is diverted — see the file comment — and `divertedTo`, when given,
/// receives the sibling's path (also on every later commit of that mount).
inline bool commitMediaInOrder(const std::string& path, uint64_t lineage,
                               uint64_t seq, const MediaFileStamp& mounted,
                               std::string& error, std::string* divertedTo,
                               const MediaCommitWrite& write)
{
    if (seq == 0) seq = nextMediaCaptureSeq();
    if (divertedTo) divertedTo->clear();
    std::error_code ec;
    std::string pathKey = std::filesystem::absolute(path, ec).lexically_normal().string();
    if (ec || pathKey.empty()) pathKey = path;
    const std::string key = pathKey + '\n' + std::to_string(lineage);
    auto& order = detail::MediaCommitOrder::instance();
    std::lock_guard<std::mutex> lk(order.m);
    auto& mount = order.mounts[key];
    if (mount.landed > seq) {
        error.clear();
        if (divertedTo) *divertedTo = mount.divertedTo;
        return true;
    }

    std::string target   = path;
    bool        diverted = !mount.divertedTo.empty();
    if (diverted) {
        target = mount.divertedTo;
    } else if (mounted.known) {
        // Cheap test first; the file is only read when its time or size
        // moved, and then its CONTENTS decide.
        MediaFileStamp now = stampMediaFile(path, /*withHash=*/false);
        const auto ownIt = order.own.find(pathKey);
        const MediaFileStamp* own = ownIt != order.own.end() ? &ownIt->second : nullptr;
        bool ours = !now.known                       // gone: recreating loses nothing
                 || now.sameTimeAndSize(mounted)
                 || (own && now.sameTimeAndSize(*own));
        if (!ours) {
            now  = stampMediaFile(path, /*withHash=*/true);
            ours = !now.known
                || (mounted.hash != 0 && now.hash == mounted.hash)
                || (own && own->hash != 0 && now.hash == own->hash);
        }
        if (!ours) {
            target   = detail::divertedSiblingFor(path);
            diverted = true;
        }
    }

    if (!write(target, diverted, error)) return false;
    mount.landed = seq;
    if (diverted) {
        if (mount.divertedTo.empty()) {
            mount.divertedTo = target;
            log().warn("Media", path + " was changed by another program while "
                       "it was mounted. It was left as that program wrote it; "
                       "the disk as the guest sees it is saved to " + target +
                       " from now on. Eject and re-insert to load the changed file.");
        }
        if (divertedTo) *divertedTo = target;
    } else {
        order.own[pathKey] = stampMediaFile(path, /*withHash=*/true);
    }
    return true;
}

/// The sentence the host shows for a diverted mount (status, tooltip, sync).
inline std::string describeDiversion(const std::string& divertedTo)
{
    return "the image file was changed by another program and was left "
           "untouched; this disk is being saved to " + divertedTo;
}

/// What the host reports about one mounted medium (Slot Config, /status).
struct MediumPersistence {
    bool        loaded = false;
    bool        pending = false;      ///< guest writes not yet in the file
    std::string path;
    std::string state = "unmounted";  ///< saved/pending/saving/error/disabled/diverted
    std::string error;
};

/// A medium the controller's poll can autosave. Implemented by `DiskImage`
/// and `Disk35Image`; cards hand theirs out through
/// `SlotPeripheral::autosavedMedia()`.
class AutosavedMedium {
public:
    /// With `stateMutex` held. Collects a finished commit, then captures and
    /// submits to `executor` when the medium is due (or at once with `force`,
    /// which the sync API uses). Returns the in-flight commit, if any.
    virtual std::shared_ptr<MediaCommitOperation>
    pollAutosave(MediaCommitExecutor& executor, bool force) = 0;
    virtual MediumPersistence persistence() const = 0;
protected:
    ~AutosavedMedium() = default;
};

/// The per-medium state machine. Embedded in the image, so it moves with it
/// (an eject moves the image out to be saved) and dies with its mount.
class MediaAutosave {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kQuiet{1000};
    /// First retry after a failure; doubles per consecutive failure up to
    /// kRetryMax. A track that no longer decodes fails the same way until
    /// the guest rewrites it, and should not log every five seconds.
    static constexpr std::chrono::milliseconds kRetry{5000};
    static constexpr std::chrono::milliseconds kRetryMax{60000};

    /// Every guest write (and anything else that changes what the file owes).
    void noteWrite() { ++writeSerial_; }
    /// A successful inline commit can divert too. Preserve that notice
    /// without retiring or replacing any outstanding background operation.
    void noteDiversion(const std::string& path) {
        if (!path.empty()) divertedTo_ = path;
    }
    /// A different medium, or none: forget the in-flight commit and errors.
    void reset() { *this = MediaAutosave{}; }

    /// Collect a finished commit. True when it landed AND nothing was written
    /// since its capture — the caller may then retire its dirty state.
    bool collect(const char* tag) {
        if (!inFlight_ || !inFlight_->ready()) return false;
        const MediaCommitResult r = inFlight_->wait();
        inFlight_.reset();
        if (r.ok) {
            error_.clear();
            retryAt_  = {};
            failures_ = 0;
            noteDiversion(r.divertedTo);
            return writeSerial_ == capturedSerial_;
        }
        // Log each distinct failure once; the retries would otherwise repeat
        // the same line for as long as the cause lasts.
        if (r.error != error_)
            log().warn(tag, "Background save failed: " + r.error);
        refuse(r.error);
        return false;
    }

    /// True when the caller should capture now. `pending` = the medium holds
    /// writes the file lacks AND write-back is allowed.
    bool due(bool pending, bool force) {
        if (!pending) {
            observedSerial_ = writeSerial_;
            // Nothing owed and nothing in flight: whatever saved the medium
            // (an explicit flush, a Liron flushAll, saveDirty) settled the
            // old failure. Only a successful AUTOSAVE collect used to clear
            // it, so the medium read "error" and /disk/sync failed for a
            // disk that was fully saved (bug hunt 2026-09-29).
            if (!inFlight_) { error_.clear(); retryAt_ = {}; failures_ = 0; }
            return false;
        }
        const auto now = Clock::now();
        if (writeSerial_ != observedSerial_) {
            observedSerial_ = writeSerial_;
            quietUntil_     = now + kQuiet;
        }
        if (force) return true;
        if (inFlight_) return false;
        return now >= quietUntil_ && now >= retryAt_;
    }

    /// Submit the captured payload.
    std::shared_ptr<MediaCommitOperation>
    start(MediaCommitExecutor& executor, std::function<MediaCommitResult()> work) {
        capturedSerial_ = writeSerial_;
        try {
            inFlight_ = executor.submit(std::move(work));
        } catch (const std::exception& e) {
            refuse(e.what());
        }
        return inFlight_;
    }

    /// The capture itself was refused (a track that no longer decodes, …).
    void refuse(const std::string& error) {
        error_ = error;
        auto delay = kRetry;
        for (int i = 0; i < failures_ && delay < kRetryMax; ++i) delay *= 2;
        if (delay > kRetryMax) delay = kRetryMax;
        ++failures_;
        retryAt_ = Clock::now() + delay;
    }

    std::shared_ptr<MediaCommitOperation> inFlight() const { return inFlight_; }
    const std::string& error() const { return error_; }
    /// Where this mount's saves go since its file was changed from outside
    /// (empty: into the file). Sticky for the life of the mount.
    const std::string& divertedTo() const { return divertedTo_; }

    /// `state`/`error` for the host, with a diversion folded in: it is not a
    /// failure (the writes are on disk) but it must not read as "saved".
    void describe(MediumPersistence& p, bool loaded, bool dirty, bool enabled) const {
        p.state = state(loaded, dirty, enabled);
        p.error = error_;
        if (loaded && error_.empty() && !divertedTo_.empty()) {
            p.state = "diverted";
            p.error = describeDiversion(divertedTo_);
        }
    }

    const char* state(bool loaded, bool dirty, bool enabled) const {
        if (!loaded) return "unmounted";
        if (!error_.empty()) return "error";
        if (inFlight_) return "saving";
        if (dirty) return enabled ? "pending" : "disabled";
        return "saved";
    }

private:
    std::shared_ptr<MediaCommitOperation> inFlight_;
    uint64_t          writeSerial_ = 0, observedSerial_ = 0, capturedSerial_ = 0;
    int               failures_ = 0;   // consecutive, for the retry backoff
    Clock::time_point quietUntil_{}, retryAt_{};
    std::string       error_;
    std::string       divertedTo_;
};

}  // namespace pom2

#endif // POM2_MEDIA_AUTOSAVE_H
