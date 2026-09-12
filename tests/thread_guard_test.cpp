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

// Thread exception barrier — pins src/ThreadGuard.h.
//
// An exception escaping the callable of a std::thread does not propagate
// anywhere: it calls std::terminate() and the whole emulator disappears with
// no log line, no message and no snapshot — the user reports a crash that is
// indistinguishable from a segfault. POM2 runs seven long-lived threads (CPU
// worker, SSC telnet, FujiNet SP link, print-history writer, AI server, plus
// two detached DNS lookups) and several of them allocate: the rewind capture
// grows multi-MB vectors against a 256 MiB budget, so bad_alloc there is a
// live possibility rather than a theoretical one.
//
// What this pins is the property the guard exists for, and it cannot be
// asserted the usual way: if the barrier regresses, the process dies instead
// of failing an assertion. So the throwing cases run in a FORKED CHILD whose
// exit status the parent checks — a child that terminates on the exception
// exits by SIGABRT, a child whose guard held exits 0. On platforms without
// fork() the throwing cases still run in-process: the guard is what keeps
// them from aborting, so a regression is still a hard failure there, just
// reported as a dead test binary rather than a clean assertion.

#include "ThreadGuard.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

struct NotAnException { int code; };

// ── The cases. Each returns normally iff the barrier held. ────────────────

void caseStdException()
{
    bool ran = false;
    pom2::runGuarded("test", [&] {
        ran = true;
        throw std::runtime_error("deliberate");
    });
    assert(ran && "the body must have been entered");
}

void caseNonStdException()
{
    pom2::runGuarded("test", [] { throw NotAnException{7}; });
}

void caseThreadBodyThrows()
{
    // The real shape: a thread whose body throws must still be joinable and
    // must not take the process with it.
    std::atomic<bool> entered{false};
    std::thread t = pom2::guardedThread("test", [&] {
        entered.store(true);
        throw std::logic_error("deliberate, on a thread");
    });
    t.join();
    assert(entered.load() && "the thread body must have run");
}

void caseCleanBodyStillRuns()
{
    // The guard must not change the ordinary path: side effects land, and a
    // body that returns normally is indistinguishable from an unguarded one.
    std::atomic<int> counter{0};
    std::thread t = pom2::guardedThread("test", [&] { counter.store(42); });
    t.join();
    assert(counter.load() == 42);

    int local = 0;
    pom2::runGuarded("test", [&] { local = 1; });
    assert(local == 1);
}

void caseMoveOnlyBody()
{
    // Every real call site captures by reference or copies a shared_ptr, but
    // FujiNetNetDevice moves a std::promise into its thread body. Pin that
    // guardedThread can carry a move-only callable at all.
    auto owned = std::make_unique<int>(5);
    std::atomic<int> seen{0};
    std::thread t = pom2::guardedThread(
        "test", [p = std::move(owned), &seen]() mutable { seen.store(*p); });
    t.join();
    assert(seen.load() == 5);
}

void runAllThrowingCases()
{
    caseStdException();
    caseNonStdException();
    caseThreadBodyThrows();
}

// ── Fork harness ─────────────────────────────────────────────────────────
// Returns true iff the child completed the throwing cases and exited 0.

#ifndef _WIN32
bool throwingCasesSurviveInAChild()
{
    const pid_t pid = ::fork();
    assert(pid >= 0 && "fork");
    if (pid == 0) {
        runAllThrowingCases();
        ::_exit(0);            // _exit: no atexit/stream flush from the child
    }
    int status = 0;
    const pid_t r = ::waitpid(pid, &status, 0);
    assert(r == pid);
    if (WIFSIGNALED(status)) {
        std::printf("thread_guard: child died on signal %d — the barrier is gone\n",
                    WTERMSIG(status));
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

} // namespace

int main()
{
    // The non-throwing cases are ordinary in-process assertions.
    caseCleanBodyStillRuns();
    caseMoveOnlyBody();

#ifndef _WIN32
    if (!throwingCasesSurviveInAChild()) {
        std::printf("thread_guard: FAILED — an exception escaped the guard\n");
        return 1;
    }
    // Re-run them in-process too: the child proved the process survives, this
    // proves the guard returns control to the caller rather than, say,
    // looping or exiting quietly.
#endif
    runAllThrowingCases();

    std::printf("thread_guard: all assertions passed\n");
    return 0;
}
