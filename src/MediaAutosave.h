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
#include <filesystem>
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
};

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
    std::mutex                                m;
    std::unordered_map<std::string, uint64_t> landed;
    static MediaCommitOrder& instance() {
        // Leaked: a commit may still be finishing while statics are torn down.
        static MediaCommitOrder* o = new MediaCommitOrder;
        return *o;
    }
};
}  // namespace detail

/// A fresh capture sequence number (see the file comment), also used to name
/// a mount's lineage. Never 0.
inline uint64_t nextMediaCaptureSeq()
{
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

/// Commit one whole-image capture of `path` taken at `seq` from the mount
/// `lineage`, in order: when a NEWER capture of the same mount has already
/// landed, this one is superseded and succeeds without writing. Ordering is
/// per mount, not per file — two drives holding the same image each save
/// their own tracks (a sector format merges them through its read-modify-
/// write), and one must never discard the other's. `seq == 0` (a payload
/// assembled by hand) is treated as the newest capture. `write(error)` does
/// the file work, serialised against every other media commit.
inline bool commitMediaInOrder(const std::string& path, uint64_t lineage,
                               uint64_t seq, std::string& error,
                               const std::function<bool(std::string&)>& write)
{
    if (seq == 0) seq = nextMediaCaptureSeq();
    std::error_code ec;
    std::string key = std::filesystem::absolute(path, ec).lexically_normal().string();
    if (ec || key.empty()) key = path;
    key += '\n' + std::to_string(lineage);
    auto& order = detail::MediaCommitOrder::instance();
    std::lock_guard<std::mutex> lk(order.m);
    uint64_t& last = order.landed[key];
    if (last > seq) { error.clear(); return true; }
    if (!write(error)) return false;
    last = seq;
    return true;
}

/// What the host reports about one mounted medium (Slot Config, /status).
struct MediumPersistence {
    bool        loaded = false;
    bool        pending = false;      ///< guest writes not yet in the file
    std::string path;
    std::string state = "unmounted";  ///< saved/pending/saving/error/disabled
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
        if (!pending) { observedSerial_ = writeSerial_; return false; }
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
};

}  // namespace pom2

#endif // POM2_MEDIA_AUTOSAVE_H
