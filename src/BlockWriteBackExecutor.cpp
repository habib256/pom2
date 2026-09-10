// POM2 — GPL-3.0-or-later
#include "BlockWriteBackExecutor.h"
#include <future>
#include <atomic>

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
}
