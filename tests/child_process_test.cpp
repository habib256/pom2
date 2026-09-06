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

// Helper-process supervision test — pins src/ChildProcess.cpp.
//
// POM2 launches a FujiNet desktop build for the user instead of making them
// start it by hand. Getting that wrong is not cosmetic:
//
//   1. A ZOMBIE READS AS ALIVE. On POSIX a child that exited stays in the
//      process table until somebody wait()s for it, and `kill(pid, 0)` still
//      succeeds on it. A liveness check written that way reports the helper
//      running forever, so the UI offers "Stop" for a process that is gone
//      and never offers "Start" again.
//   2. AN ORPHANED HELPER HOLDS THE PORT. If POM2 exits without reaping its
//      child, the FujiNet keeps the loopback connection and the *next* POM2
//      session cannot bind 1985 — which looks like POM2 being broken.
//   3. A FAILED LAUNCH MUST SAY SO. A missing or non-executable path has to
//      come back as a clean error, not a half-started state.
//
// POSIX uses /bin/sh as the stand-in helper.  Win32 re-launches this test
// binary in a child mode whose console handler records receipt of Ctrl+C.

#include "ChildProcess.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#if !POM2_HAS_CHILD_PROCESS

int main()
{
    std::puts("SKIP: child processes are unavailable");
    return 0;
}

#elif defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <thread>

namespace {

char gStoppedMarker[MAX_PATH]{};

BOOL WINAPI ctrlHandler(DWORD type)
{
    if (type != CTRL_C_EVENT) return FALSE;
    HANDLE f = CreateFileA(gStoppedMarker, GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    ExitProcess(0);
}

std::string modulePathUtf8()
{
    std::vector<wchar_t> path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(),
                                      static_cast<DWORD>(path.size()));
    assert(n > 0 && n < path.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(), n, nullptr,
                                          0, nullptr, nullptr);
    assert(bytes > 0);
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.data(), n, out.data(), bytes,
                        nullptr, nullptr);
    return out;
}

bool waitForFile(const char* path, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 10) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int childMode(const char* stopped, const char* ready)
{
    std::snprintf(gStoppedMarker, sizeof(gStoppedMarker), "%s", stopped);
    if (!SetConsoleCtrlHandler(ctrlHandler, TRUE)) return 2;
    HANDLE f = CreateFileA(ready, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 3;
    CloseHandle(f);
    Sleep(60000);
    return 4;
}

void testGracefulCtrlC(int argc, char** argv)
{
    assert(argc > 0 && argv[0]);
    char temp[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, temp);
    assert(n > 0 && n < MAX_PATH);
    const DWORD pid = GetCurrentProcessId();
    const std::string stopped = std::string(temp) + "pom2-child-stopped-" +
                                std::to_string(pid) + ".tmp";
    const std::string ready = std::string(temp) + "pom2-child-ready-" +
                              std::to_string(pid) + ".tmp";
    DeleteFileA(stopped.c_str());
    DeleteFileA(ready.c_str());

    pom2::ChildProcess p;
    std::string err;
    assert(p.start(modulePathUtf8(),
                   { "--ctrl-child", stopped, ready }, "", err));
    assert(waitForFile(ready.c_str(), 5000));
    assert(p.isRunning());

    const auto before = std::chrono::steady_clock::now();
    p.stop(2000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before).count();
    assert(waitForFile(stopped.c_str(), 1000));
    assert(elapsed < 1500); // hard fallback would consume the full grace period
    assert(!p.isRunning());

    DeleteFileA(stopped.c_str());
    DeleteFileA(ready.c_str());
}

} // namespace

int main(int argc, char** argv)
{
    if (const int broker =
            pom2::ChildProcess::runConsoleSignalBrokerIfRequested(argc, argv);
        broker >= 0)
        return broker;
    if (argc == 4 && std::string(argv[1]) == "--ctrl-child")
        return childMode(argv[2], argv[3]);
    testGracefulCtrlC(argc, argv);
    std::puts("child_process: OK");
    return 0;
}

#else

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

using pom2::ChildProcess;

void sleepMs(int ms)
{ std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ── 1. A short-lived child is noticed, and reaped ────────────────────────
void testExitIsDetectedAndReaped()
{
    ChildProcess p;
    std::string err;
    assert(p.start("/bin/sh", { "-c", "exit 3" }, "", err));

    // It must eventually report NOT running. If isRunning() used kill(pid,0)
    // this loop would never end — the zombie answers.
    bool ended = false;
    for (int i = 0; i < 200 && !ended; ++i) {
        if (!p.isRunning()) ended = true;
        else sleepMs(10);
    }
    assert(ended);
    // And the exit status survived the reap.
    assert(p.lastExitCode() == 3);
}

// ── 2. A long-lived child is alive until we stop it ──────────────────────
void testStopTerminates()
{
    ChildProcess p;
    std::string err;
    assert(p.start("/bin/sh", { "-c", "sleep 60" }, "", err));

    sleepMs(50);
    assert(p.isRunning());

    const auto t0 = std::chrono::steady_clock::now();
    p.stop(1000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    assert(!p.isRunning());
    // SIGTERM should have done it well inside the grace period — if we had to
    // fall through to SIGKILL this would sit at the full 1000 ms.
    assert(ms < 900);
}

// ── 3. A child that ignores SIGTERM is killed anyway ─────────────────────
void testStubbornChildIsKilled()
{
    ChildProcess p;
    std::string err;
    // trap '' TERM makes the shell ignore SIGTERM outright.
    assert(p.start("/bin/sh", { "-c", "trap '' TERM; sleep 60" }, "", err));
    sleepMs(80);
    assert(p.isRunning());

    p.stop(200);              // short grace, so the SIGKILL path runs
    assert(!p.isRunning());   // it is gone regardless

    // AND SO IS ITS `sleep`. The shell here spawns a grandchild; killing only
    // the direct child leaves that grandchild running and holding our stdout
    // pipe, which is invisible when this test is run by hand but hangs it
    // under ctest (ctest waits for every process on the pipe). `stop()`
    // signals the process group for exactly this reason. If that regresses,
    // this test times out rather than failing an assert — the comment is the
    // only thing that will explain why.
}

// ── 4. The destructor must not leak the child ────────────────────────────
void testDestructorStops()
{
    int pidProbe = 0;
    {
        ChildProcess p;
        std::string err;
        assert(p.start("/bin/sh", { "-c", "sleep 60" }, "", err));
        sleepMs(50);
        assert(p.isRunning());
        pidProbe = 1;
    }   // ~ChildProcess() must terminate it
    assert(pidProbe == 1);
    // Nothing to assert directly without racing the OS; the value here is
    // that the destructor path runs under the same asserts as stop() and
    // does not hang. A leaked helper is what holds the loopback port.
}

// ── 5. Failures are clean ────────────────────────────────────────────────
void testStartFailures()
{
    ChildProcess p;
    std::string err;

    assert(!p.start("", {}, "", err));
    assert(!err.empty());
    assert(!p.isRunning());

    err.clear();
    assert(!p.start("/definitely/not/here/fujinet", {}, "", err));
    assert(!err.empty());
    assert(!p.isRunning());

    // A path that exists but is not executable.
    err.clear();
    assert(!p.start("/etc/hostname", {}, "", err));
    assert(!p.isRunning());

    // Failures that happen only after fork must be synchronously surfaced
    // with their stage and OS reason, not reported as a successful launch.
    err.clear();
    assert(!p.start("/bin/sh", {}, "/definitely/not/a/directory", err));
    assert(err.find("chdir") != std::string::npos);
    assert(!p.isRunning());

    char badExe[] = "/tmp/pom2_bad_exec_XXXXXX";
    const int fd = ::mkstemp(badExe);
    assert(fd >= 0);
    const char bytes[] = "not an executable format\n";
    assert(::write(fd, bytes, sizeof(bytes) - 1) ==
           static_cast<ssize_t>(sizeof(bytes) - 1));
    assert(::fchmod(fd, 0700) == 0);
    ::close(fd);
    err.clear();
    assert(!p.start(badExe, {}, "", err));
    assert(err.find("exec") != std::string::npos);
    assert(!err.empty());
    assert(!p.isRunning());
    ::unlink(badExe);
}

// ── 6. Restart replaces the previous child ───────────────────────────────
void testRestartReplaces()
{
    ChildProcess p;
    std::string err;
    assert(p.start("/bin/sh", { "-c", "sleep 60" }, "", err));
    sleepMs(50);
    assert(p.isRunning());

    // Starting again must stop the first one rather than leaking it.
    assert(p.start("/bin/sh", { "-c", "sleep 60" }, "", err));
    sleepMs(50);
    assert(p.isRunning());
    p.stop(1000);
    assert(!p.isRunning());
}

// ── 7. findOnPath ────────────────────────────────────────────────────────
void testFindOnPath()
{
    // Something that is certainly on PATH.
    const std::string sh = ChildProcess::findOnPath("sh");
    assert(!sh.empty());
    assert(sh.find("/sh") != std::string::npos);

    // An explicit path is validated rather than searched.
    assert(ChildProcess::findOnPath("/bin/sh") == "/bin/sh");
    assert(ChildProcess::findOnPath("/definitely/not/here").empty());

    // A name nobody has must come back empty, not throw.
    assert(ChildProcess::findOnPath("pom2-no-such-helper-xyz").empty());
}

// ── 8. The child inherits NO descriptor beyond stdio ─────────────────────
//
// This is trap 2 above, at its root. fork() dups the whole descriptor table
// and POM2 opens no socket with SOCK_CLOEXEC, so a child that does not close
// what it inherited keeps POM2's listeners BOUND: "Drop peer" then fails to
// re-bind with EADDRINUSE, and a Ctrl-C (which runs no destructor, so the
// helper is never stopped) leaves an orphan squatting the port for the next
// session. Before the close loop in ChildProcess::start this failed on the
// first rebind.
void testChildDoesNotInheritListeners()
{
    // A listener on an ephemeral port, so the test never collides with a real
    // POM2 session — the DEFECT is about descriptor inheritance, not 1985.
    auto bindListener = [](int& portOut) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons(static_cast<uint16_t>(portOut));
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
            ::listen(fd, 2) != 0) {
            ::close(fd);
            return -1;
        }
        socklen_t len = sizeof(a);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        portOut = ntohs(a.sin_port);
        return fd;
    };

    int port = 0;                       // 0 = let the kernel pick
    const int listenFd = bindListener(port);
    assert(listenFd >= 0 && port != 0);

    ChildProcess p;
    std::string  err;
    assert(p.start("/bin/sh", { "-c", "sleep 30" }, "", err));
    sleepMs(50);
    assert(p.isRunning());

    // Give up OUR copy. If the child inherited one, the port stays in LISTEN
    // and nothing can take it — which is exactly the wedge users saw.
    ::close(listenFd);

    const int again = bindListener(port);
    const bool rebound = again >= 0;
    if (rebound) ::close(again);
    p.stop(1000);

    assert(rebound && "the helper inherited POM2's listening socket");
}

// ── 9. stopDetached(): the same shutdown, without the wait ───────────────
//
// `stop()` polls for its whole grace period on purpose — a FujiNet flushing an
// SD-card image deserves to finish. `~FujiNetCard` cannot afford that: it runs
// inside `SlotBus::plug()`, which POM2 calls with the emulator's state mutex
// held on every slot rebuild and every profile switch, so swapping a card out
// froze the machine and the window together for two seconds.
//
// Two things are pinned. The RETURN is immediate even for a child that ignores
// SIGTERM (the case that costs stop() the full grace). And the child still
// dies: the grandchild `sleep` holds this process's stdout pipe, so if the
// detached thread's SIGKILL sweep of the group ever regresses, ctest — which
// waits for every process on that pipe — HANGS rather than failing an assert.
// That is the same trap testStubbornChildIsKilled documents.
void testStopDetachedDoesNotWait()
{
    ChildProcess p;
    std::string err;
    assert(p.start("/bin/sh", { "-c", "trap '' TERM; sleep 60" }, "", err));
    sleepMs(80);
    assert(p.isRunning());

    const auto t0 = std::chrono::steady_clock::now();
    p.stopDetached();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    // stop() would have sat here for the whole 2 s grace and then some.
    assert(ms < 100 && "stopDetached() waited for the child");
    // The object owns nothing any more, so its own destructor has nothing to
    // do either — that is what makes it safe in a destructor under the lock.
    assert(!p.isRunning());

    // Outlive the detached reaper: the grace, the SIGKILL and the reap all run
    // on a thread this process must not exit from under.
    sleepMs(2500);
}

} // namespace

int main()
{
    testExitIsDetectedAndReaped();
    testStopTerminates();
    testStubbornChildIsKilled();
    testDestructorStops();
    testStartFailures();
    testRestartReplaces();
    testFindOnPath();
    testChildDoesNotInheritListeners();
    testStopDetachedDoesNotWait();

    std::puts("child_process: OK");
    return 0;
}

#endif
