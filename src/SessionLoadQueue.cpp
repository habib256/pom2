// POM2 — GPL-3.0-or-later
#include "SessionLoadQueue.h"
#include <exception>
#include <utility>
namespace pom2 {
SessionLoadQueue::SessionLoadQueue(Loader loader) : owner_(std::this_thread::get_id()), loader_(std::move(loader)) {}
bool SessionLoadQueue::load(const SessionArchive& session, std::string& error)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) { error = "session loader is shutting down"; return false; }
    if (std::this_thread::get_id() == owner_) {
        lock.unlock();
        // Same barrier as process(): a bad_alloc on a big archive must be a
        // failed load, not an exception thrown into the UI's frame loop.
        try { return loader_(session, error); }
        catch (const std::exception& e) { error = e.what(); }
        catch (...) { error = "session reconstruction failed"; }
        return false;
    }
    if (remoteSuspended_) { error = "session load cancelled"; return false; }
    auto request = std::make_shared<Request>(); request->session = session;
    pending_.push_back(request);
    cv_.wait(lock, [&] { return request->done || closed_; });
    if (!request->done) { error = "session loader is shutting down"; return false; }
    error = request->error; return request->ok;
}
void SessionLoadQueue::process()
{
    std::deque<std::shared_ptr<Request>> work;
    { std::lock_guard<std::mutex> lock(mutex_); if (closed_) return; work.swap(pending_); }
    for (auto& request : work) {
        bool ok = false; std::string error;
        try { ok = loader_(request->session, error); }
        catch (const std::exception& e) { error = e.what(); }
        catch (...) { error = "session reconstruction failed"; }
        { std::lock_guard<std::mutex> lock(mutex_);
          request->ok = ok; request->error = std::move(error); request->done = true; }
        cv_.notify_all();
    }
}
void SessionLoadQueue::suspendRemote(bool on)
{
    std::lock_guard<std::mutex> lock(mutex_);
    remoteSuspended_ = on;
    if (!on) return;
    for (auto& request : pending_) { request->error = "session load cancelled"; request->done = true; }
    pending_.clear(); cv_.notify_all();
}
void SessionLoadQueue::close()
{
    std::lock_guard<std::mutex> lock(mutex_); closed_ = true; pending_.clear(); cv_.notify_all();
}
}
