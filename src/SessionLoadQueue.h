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
private:
    struct Request { SessionArchive session; std::string error; bool done = false, ok = false; };
    std::thread::id owner_;
    Loader loader_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Request>> pending_;
    bool closed_ = false;
};
}
