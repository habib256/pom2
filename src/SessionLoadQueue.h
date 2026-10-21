// POM2 — GPL-3.0-or-later
#pragma once
#include "SessionArchive.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
namespace pom2 {
// HTTP callers wait for the UI owner to replace its topology. Headless hosts
// reconstruct directly. Closing wakes all waiters before the server is joined.
class SessionLoadQueue {
public:
    using Loader = std::function<bool(const SessionArchive&, std::string&)>;
    explicit SessionLoadQueue(Loader loader);
    bool load(const SessionArchive& session, std::string& error);
    void process();
    void close();
    /// Fail every request still waiting and, while `on`, every new one from
    /// another thread — WITHOUT closing the queue. The thread that posted
    /// it is about to be joined from the owner's thread, the only one that
    /// would ever process it (AI server Stop); `false` again at its Start.
    void suspendRemote(bool on);
private:
    struct Request { SessionArchive session; std::string error; bool done = false, ok = false; };
    std::thread::id owner_;
    Loader loader_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Request>> pending_;
    bool closed_ = false;
    bool remoteSuspended_ = false;
};
}
