// POM2 — GPL-3.0-or-later
#include "BlockWriteBackExecutor.h"
#include "ThreadGuard.h"
#include <future>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace pom2 {
namespace {
using Backing = Block512Backing;
class Barrier final : public Backing::CommitBarrier {
public:
    explicit Barrier(std::shared_ptr<Barrier> previous) : previous_(std::move(previous)) {}
    void wait() override {
        if (previous_) previous_->await();
        waited_ = true;
    }
    void complete() override {
        if (!completed_.exchange(true)) {
            done_.set_value();
            settled_ = waited_.load();
        }
    }
    bool settled() const { return settled_.load(); }
private:
    void await() {
        // A discarded ticket can complete without having waited. Keep its
        // predecessor in the chain so cancellation cannot reorder commits.
        if (previous_) previous_->await();
        finished_.wait();
        settled_ = true;
    }
    const std::shared_ptr<Barrier> previous_;
    std::promise<void> done_;
    std::shared_future<void> finished_ = done_.get_future().share();
    std::atomic<bool> completed_{false}, waited_{false}, settled_{false};
};
class Operation final : public Backing::WriteBackOperation {
public:
    std::shared_future<Backing::WriteBackResult> result;
    bool ready() const override {
        return result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
    Backing::WriteBackResult wait() override { return result.get(); }
};
class Executor final : public Backing::WriteBackExecutor {
public:
    std::shared_ptr<Backing::CommitBarrier> reserve(
        std::shared_ptr<Backing::CommitBarrier> previous) override {
        auto barrier = std::static_pointer_cast<Barrier>(previous);
        if (barrier && barrier->settled()) barrier.reset();
        return std::make_shared<Barrier>(std::move(barrier));
    }
    std::shared_ptr<Backing::WriteBackOperation> submit(
        Backing::PendingWriteBack pending,
        std::shared_ptr<Backing::WriteBackOperation> previous) override {
        auto work = [pending = std::move(pending), previous]() mutable {
            Backing::WriteBackResult result;
            try { result.ok = Backing::commitWriteBack(std::move(pending), result.error); }
            catch (const std::exception& e) { result.ok = false; result.error = e.what(); }
            catch (...) { result.ok = false; result.error = "unexpected write-back failure"; }
            return result;
        };
        auto operation = std::make_shared<Operation>();
#ifdef __EMSCRIPTEN__
        // IDBFS is in-memory; the existing session heartbeat syncs IndexedDB.
        std::promise<Backing::WriteBackResult> promise;
        promise.set_value(work());
        operation->result = promise.get_future().share();
#else
        operation->result = std::async(std::launch::async, std::move(work)).share();
#endif
        return operation;
    }
};
}
std::shared_ptr<Backing::WriteBackExecutor> blockWriteBackExecutor() {
    static auto executor = std::make_shared<Executor>();
    return executor;
}

namespace {
// From a std::promise, never std::async: an async future BLOCKS in its
// destructor, and an image dropped under `stateMutex` (eject, remount) with a
// commit still running would then wait out the file I/O there.
class MediaOperation final : public MediaCommitOperation {
public:
    explicit MediaOperation(std::shared_future<MediaCommitResult> f) : result_(std::move(f)) {}
    bool ready() const override {
        return result_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
    MediaCommitResult wait() override { return result_.get(); }
private:
    std::shared_future<MediaCommitResult> result_;
};

MediaCommitResult runMediaCommit(std::function<MediaCommitResult()>& work) {
    MediaCommitResult r;
    try { r = work(); }
    catch (const std::exception& e) { r.ok = false; r.error = e.what(); }
    catch (...) { r.ok = false; r.error = "unexpected media write-back failure"; }
    return r;
}

#ifdef __EMSCRIPTEN__
// No threads; the IDBFS write lands in memory and the session heartbeat makes
// it durable, as for the block executor above.
class MediaWorker final : public MediaCommitExecutor {
public:
    std::shared_ptr<MediaCommitOperation>
    submit(std::function<MediaCommitResult()> work) override {
        std::promise<MediaCommitResult> promise;
        promise.set_value(runMediaCommit(work));
        return std::make_shared<MediaOperation>(promise.get_future().share());
    }
    void drain() {}
};
#else
// One worker, so the commits of every floppy land in the order they were
// captured. Leaked on purpose: a joinable std::thread destroyed at exit calls
// std::terminate, and a commit may still be finishing then —
// `~EmulationController` drains it first.
class MediaWorker final : public MediaCommitExecutor {
public:
    std::shared_ptr<MediaCommitOperation>
    submit(std::function<MediaCommitResult()> work) override {
        auto promise = std::make_shared<std::promise<MediaCommitResult>>();
        auto op = std::make_shared<MediaOperation>(promise->get_future().share());
        std::lock_guard<std::mutex> lk(m_);
        queue_.push_back({std::move(work), std::move(promise)});
        if (!thread_.joinable())
            thread_ = guardedThread("MediaCommit", [this] { loop(); });
        cv_.notify_one();
        return op;
    }
    void drain() {
        std::unique_lock<std::mutex> lk(m_);
        idle_.wait(lk, [this] { return queue_.empty() && !busy_; });
    }
private:
    struct Job {
        std::function<MediaCommitResult()>               work;
        std::shared_ptr<std::promise<MediaCommitResult>> promise;
    };
    void loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return !queue_.empty(); });
                job = std::move(queue_.front());
                queue_.pop_front();
                busy_ = true;
            }
            job.promise->set_value(runMediaCommit(job.work));
            std::lock_guard<std::mutex> lk(m_);
            busy_ = false;
            if (queue_.empty()) idle_.notify_all();
        }
    }
    std::mutex              m_;
    std::condition_variable cv_, idle_;
    std::deque<Job>         queue_;
    std::thread             thread_;
    bool                    busy_ = false;
};
#endif
MediaWorker& mediaWorker() {
    static MediaWorker* worker = new MediaWorker;
    return *worker;
}
}  // namespace

MediaCommitExecutor& mediaCommitExecutor() { return mediaWorker(); }
void drainMediaCommits() { mediaWorker().drain(); }
}
