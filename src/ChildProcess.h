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

// ChildProcess — launch and supervise one helper program, POSIX and Win32
// behind one API.
//
// POM2 owns the child for its whole life: it starts it, can tell whether it is
// still alive, and reaps it on shutdown. Nothing is inherited by the child
// beyond stdio, and nothing survives POM2 exiting — a helper left running
// after the emulator quits would hold the loopback port that the next session
// wants to listen on.
//
// "Nothing beyond stdio" is enforced, not assumed: Win32 passes
// bInheritHandles=FALSE, and the POSIX child closes every descriptor above 2
// between fork() and execv(). POM2 opens no socket with SOCK_CLOEXEC, so
// without that loop the helper would inherit — and keep BOUND — the very
// listeners the sentence above is about.
//
// Written for the FujiNet helper (a FujiNet desktop build POM2 can start for
// the user instead of making them run it by hand), but there is nothing
// FujiNet-specific here.
//
// ── The two traps ─────────────────────────────────────────────────────────
//
//   1. ZOMBIES. On POSIX a child that exits stays in the process table until
//      somebody wait()s for it. `isRunning()` therefore does a
//      `waitpid(WNOHANG)` rather than a bare `kill(pid, 0)` — the latter
//      reports a dead-but-unreaped child as alive, forever.
//   2. TERMINATE IS NOT A REQUEST. `stop()` asks politely first, waits a grace
//      period, and only then kills. A FujiNet flushing its SD card image
//      deserves the chance to finish. On POSIX the polite ask is a SIGTERM to
//      the process group. FujiNet-PC documents Ctrl+C as its Win32 shutdown
//      path, so that platform gives the child tree a dedicated hidden console;
//      `stop()` starts a tiny broker that attaches to it, sends
//      CTRL_C_EVENT without disturbing POM2's own console, then falls back
//      to terminating the Job
//      Object if the helper does not leave within the grace period.
//
// Not available under Emscripten (no processes in a browser):
// POM2_HAS_CHILD_PROCESS is 0 there and `start()` fails cleanly.

#ifndef POM2_CHILD_PROCESS_H
#define POM2_CHILD_PROCESS_H

#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
#  define POM2_HAS_CHILD_PROCESS 0
#else
#  define POM2_HAS_CHILD_PROCESS 1
#endif

namespace pom2 {

class ChildProcess
{
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess&)            = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Launch `exePath` with `args` (argv[0] is supplied automatically).
    /// `workingDir` empty = inherit POM2's. Returns false with a
    /// human-readable reason in `errOut`; a process already running is
    /// stopped first.
    bool start(const std::string& exePath,
               const std::vector<std::string>& args,
               const std::string& workingDir,
               std::string& errOut);

    /// Alive? Reaps the child if it has exited, so this is also what keeps
    /// zombies out of the process table — call it periodically.
    bool isRunning();

    /// Ask for a clean exit (SIGTERM on POSIX, Ctrl+C on Win32), wait
    /// `graceMs`, then kill the process tree. Safe when nothing is running.
    void stop(int graceMs = 2000);

    /// The same shutdown, without waiting for it.
    ///
    /// `stop()` polls for the WHOLE grace period, deliberately — a FujiNet
    /// flushing an SD-card image should be allowed to finish. That is fine on
    /// a button press and wrong in a destructor: `~FujiNetCard` runs inside
    /// `SlotBus::plug()`, which POM2 calls with the emulator's state mutex
    /// held on every slot rebuild and every profile switch, so a card being
    /// swapped out froze the machine and the window for two seconds
    /// (CLAUDE.md, "never hold stateMutex across file I/O" — a process
    /// teardown is worse, since the child decides how long it takes).
    ///
    /// This asks politely RIGHT NOW, from the caller's thread, then hands the
    /// grace period, the kill and the reap to a detached guarded thread and
    /// returns. Ownership of the child moves with it: this object is left
    /// holding nothing, so its own destructor has nothing to wait for, and
    /// `lastExitCode()` is -1 because nobody stayed to collect it.
    void stopDetached();

    /// Exit status of the last child that finished, or -1 if it was killed /
    /// never ran. Only meaningful once `isRunning()` has returned false.
    int  lastExitCode() const { return exitCode_; }

    const std::string& path() const { return path_; }

    /// Look for `name` on PATH and in the usual install locations, returning
    /// the first hit or "" — so the UI can offer a sensible default without
    /// the user hunting for the binary.
    static std::string findOnPath(const std::string& name);

    /// Win32 implementation detail: if argv describes the short-lived
    /// console-signal broker, deliver Ctrl+C and return its exit status.
    /// Returns -1 for an ordinary invocation (and on non-Windows builds).
    /// POM2 and the supervision test call this before normal argument setup.
    static int runConsoleSignalBrokerIfRequested(int argc, char* argv[]);

private:
    void reset();

#if POM2_HAS_CHILD_PROCESS
#  ifdef _WIN32
    void* handle_ = nullptr;      ///< HANDLE, kept void* to keep windows.h out
    void* job_    = nullptr;      ///< Job Object kills the whole helper tree
#  else
    int   pid_ = -1;
#  endif
#endif
    std::string path_;
    int         exitCode_ = -1;
};

} // namespace pom2

#endif // POM2_CHILD_PROCESS_H
