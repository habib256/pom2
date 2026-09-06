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

// ChildProcess implementation. See the header for the zombie and
// terminate-is-not-a-request traps.

#include "ChildProcess.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if POM2_HAS_CHILD_PROCESS
#  ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#      define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#      define NOMINMAX
#    endif
#    include <windows.h>
#  else
#    include <cerrno>
#    include <csignal>
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <sys/syscall.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#  endif
#endif

namespace pom2 {
namespace {

/// Process-wide bookkeeping for the teardown threads `stopDetached()` spawns.
/// They are detached — nobody can join them — so the only way main() can know
/// they finished their SIGKILL sweep is to count them. `draining` is the
/// "the process is going away, stop waiting politely" flag they poll instead
/// of sleeping through the grace period.
struct DetachedTeardowns {
    std::mutex              mtx;
    std::condition_variable cv;
    int                     pending  = 0;
    bool                    draining = false;
};

DetachedTeardowns& detachedTeardowns()
{
    static DetachedTeardowns state;
    return state;
}

/// One in-flight teardown. CONSTRUCTED ON THE CALLER'S THREAD (as a lambda
/// capture initialiser, evaluated before std::thread starts running the body)
/// so a drainDetached() racing the spawn still sees the teardown it must wait
/// for; retired by the worker when the lambda is destroyed.
class DetachedTicket
{
public:
    DetachedTicket()
    {
        auto& d = detachedTeardowns();
        std::lock_guard<std::mutex> lk(d.mtx);
        ++d.pending;
    }
    DetachedTicket(DetachedTicket&& other) noexcept : live_(other.live_)
    {
        other.live_ = false;
    }
    DetachedTicket(const DetachedTicket&)            = delete;
    DetachedTicket& operator=(const DetachedTicket&) = delete;
    ~DetachedTicket()
    {
        if (!live_) return;
        auto& d = detachedTeardowns();
        {
            std::lock_guard<std::mutex> lk(d.mtx);
            --d.pending;
        }
        d.cv.notify_all();
    }

    /// The grace-period sleep, interruptible. Returns false as soon as
    /// drainDetached() says the process is exiting, so the caller skips
    /// straight to the kill.
    static bool graceStep(int ms)
    {
        auto& d = detachedTeardowns();
        std::unique_lock<std::mutex> lk(d.mtx);
        d.cv.wait_for(lk, std::chrono::milliseconds(ms),
                      [&d] { return d.draining; });
        return !d.draining;
    }

private:
    bool live_ = true;
};

} // namespace

// Platform-independent: the registry above is the only state it touches, and
// a build with no child processes simply never registers anything.
void ChildProcess::drainDetached(int maxWaitMs)
{
    auto& d = detachedTeardowns();
    std::unique_lock<std::mutex> lk(d.mtx);
    if (d.pending == 0) return;

    d.draining = true;
    d.cv.notify_all();
    if (maxWaitMs < 0) maxWaitMs = 0;
    d.cv.wait_for(lk, std::chrono::milliseconds(maxWaitMs),
                  [&d] { return d.pending == 0; });
    // Lowered again on the way out: a caller that drains early (a test, or a
    // future "close all helpers" button) must not turn every LATER
    // stopDetached() into an instant kill.
    d.draining = false;
}

ChildProcess::~ChildProcess() { stop(); }

#if !POM2_HAS_CHILD_PROCESS

bool ChildProcess::start(const std::string&, const std::vector<std::string>&,
                         const std::string&, std::string& errOut)
{ errOut = "launching helper programs is not available in this build"; return false; }
bool ChildProcess::isRunning() { return false; }
void ChildProcess::stop(int) {}
void ChildProcess::stopDetached() {}
void ChildProcess::reset() {}
std::string ChildProcess::findOnPath(const std::string&) { return {}; }
int ChildProcess::runConsoleSignalBrokerIfRequested(int, char*[]) { return -1; }

#else

// ═════════════════════════════════════════════════════════════════════════
// POSIX
// ═════════════════════════════════════════════════════════════════════════
#ifndef _WIN32

void ChildProcess::reset() { pid_ = -1; }

bool ChildProcess::start(const std::string& exePath,
                         const std::vector<std::string>& args,
                         const std::string& workingDir,
                         std::string& errOut)
{
    if (isRunning()) stop();

    if (exePath.empty()) { errOut = "no helper program configured"; return false; }
    if (::access(exePath.c_str(), X_OK) != 0) {
        errOut = exePath + ": " + std::strerror(errno);
        return false;
    }

    // Build argv BEFORE forking: allocating between fork() and exec() in a
    // multi-threaded process can deadlock on the allocator's lock, and POM2
    // is very much multi-threaded (CPU worker, audio, link workers).
    std::vector<std::string> owned;
    owned.reserve(args.size() + 1);
    owned.push_back(exePath);
    for (const auto& a : args) owned.push_back(a);
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& s : owned) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    // Descriptor ceiling for the child's close loop, queried BEFORE the fork
    // for the same reason argv is built here: sysconf() is not on the
    // async-signal-safe list.
    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd < 3 || maxFd > 65536) maxFd = 65536;

    // The write end is closed automatically by a successful exec. On a
    // pre-exec failure the child sends only fixed-size integers (async-signal
    // safe), allowing start() to report the actual chdir/exec error instead
    // of briefly claiming a dead helper was launched.
    int errorPipe[2] = {-1, -1};
    if (::pipe(errorPipe) != 0) {
        errOut = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    if (::fcntl(errorPipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        const int e = errno;
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        errOut = std::string("fcntl(FD_CLOEXEC): ") + std::strerror(e);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        errOut = std::string("fork: ") + std::strerror(e);
        return false;
    }

    if (pid == 0) {
        ::close(errorPipe[0]);
        struct LaunchError { int stage; int error; };
        auto failLaunch = [&](int stage) {
            const LaunchError detail{stage, errno};
            const char* p = reinterpret_cast<const char*>(&detail);
            size_t left = sizeof(detail);
            while (left != 0) {
                const ssize_t n = ::write(errorPipe[1], p, left);
                if (n > 0) { p += n; left -= static_cast<size_t>(n); }
                else if (n < 0 && errno == EINTR) continue;
                else break;
            }
            ::_exit(127);
        };
        // Child. Only async-signal-safe calls from here to execv().
        //
        // New process group: POM2 running in a terminal would otherwise pass
        // its Ctrl-C to the helper as well, killing it out from under us and
        // leaving the emulator convinced it is still there.
        if (::setpgid(0, 0) != 0) failLaunch(0);
        if (!workingDir.empty()) {
            if (::chdir(workingDir.c_str()) != 0) failLaunch(1);
        }

        // Close every inherited descriptor above stdio.
        //
        // fork() dups the ENTIRE descriptor table, and nothing in POM2 opens
        // its sockets with SOCK_CLOEXEC. Without this loop the helper holds a
        // live copy of the SP-over-SLIP listener on 1985, the AI-control
        // server on 6503 and any SSC telnet listener — and keeps them BOUND
        // after POM2 closes its own copy. "Drop peer" then fails to re-bind
        // with EADDRINUSE, and a Ctrl-C on POM2 (which runs no destructor, so
        // helper_.stop() never fires) leaves an orphan squatting the port for
        // the next session. That is trap 2 in child_process_test.cpp, and the
        // header's "nothing is inherited beyond stdio" promise — the Win32
        // branch keeps it with bInheritHandles=FALSE, this is the POSIX half.
        //
        // close_range() is one syscall for the whole range; the loop is the
        // fallback for kernels older than 5.9. Both are async-signal-safe.
        bool closedByRange = false;
#if defined(__linux__) && defined(SYS_close_range)
        {
            const unsigned keep = static_cast<unsigned>(errorPipe[1]);
            const bool below = keep <= 3 ||
                ::syscall(SYS_close_range, 3u, keep - 1u, 0u) == 0;
            const bool above =
                ::syscall(SYS_close_range, keep + 1u, ~0u, 0u) == 0;
            closedByRange = below && above;
        }
#endif
        if (!closedByRange) {
            for (int fd = 3; fd < static_cast<int>(maxFd); ++fd) {
                if (fd != errorPipe[1]) ::close(fd);
            }
        }

        ::execv(argv[0], argv.data());
        failLaunch(2);
    }

    ::close(errorPipe[1]);

    // Set the group from the PARENT too. The child does it as well, and
    // whichever runs first wins — without this, `stop()` can signal the group
    // in the window before the child's own setpgid() lands, and miss it.
    // EACCES simply means the child already exec'd, which is fine.
    ::setpgid(pid, pid);

    struct LaunchError { int stage; int error; } detail{};
    char* dst = reinterpret_cast<char*>(&detail);
    size_t received = 0;
    while (received < sizeof(detail)) {
        const ssize_t n = ::read(errorPipe[0], dst + received,
                                 sizeof(detail) - received);
        if (n > 0) received += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else break;  // EOF = exec succeeded
    }
    ::close(errorPipe[0]);
    if (received != 0) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        const char* stage = detail.stage == 0 ? "setpgid" :
                            detail.stage == 1 ? "chdir" : "exec";
        errOut = std::string(stage) + " " +
                 (detail.stage == 1 ? workingDir :
                  detail.stage == 2 ? exePath : std::string{}) + ": " +
                 std::strerror(detail.error);
        reset();
        return false;
    }

    pid_      = static_cast<int>(pid);
    path_     = exePath;
    exitCode_ = -1;
    return true;
}

bool ChildProcess::isRunning()
{
    if (pid_ < 0) return false;

    // waitpid, NOT kill(pid, 0): a child that has exited but not been reaped
    // still answers kill(), so the naive check reports it alive forever AND
    // leaves a zombie behind.
    int   status = 0;
    const pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0) return true;             // still running
    if (r < 0) {
        if (errno == EINTR) return true; // ask again next tick
        reset();                         // ECHILD: not ours (already reaped)
        return false;
    }
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    reset();
    return false;
}

void ChildProcess::stop(int graceMs)
{
    if (pid_ < 0) return;

    // Signal the whole PROCESS GROUP, not just the child.
    //
    // A helper that spawns its own children — a launcher script, or a shell
    // wrapper — leaves them running when only the direct child is killed, and
    // they inherit our stdout pipe. That is not theoretical: it made this
    // file's own test hang under ctest, which waits for every process holding
    // the pipe, while the same test passed when run by hand. In production it
    // would mean a stray FujiNet still holding the loopback port after POM2
    // "stopped" it. The child is its own group leader (setpgid above), so the
    // negated pid addresses exactly its group and nothing else.
    ::kill(-pid_, SIGTERM);

    // Give it the grace period to shut down cleanly — a FujiNet flushing an
    // SD-card image should be allowed to finish. Save the group id up
    // front: isRunning() reaps the direct child and reset() clears pid_.
    const pid_t group = pid_;
    const int stepMs = 25;
    bool exited = false;
    for (int waited = 0; waited < graceMs; waited += stepMs) {
        if (!isRunning()) { exited = true; break; }
        ::usleep(static_cast<useconds_t>(stepMs) * 1000);
    }

    // Sweep the whole GROUP with SIGKILL no matter how the direct child
    // went. Returning as soon as the direct child exited skipped this, so
    // a wrapper script (run-fujinet) that dies on SIGTERM instantly left
    // its SIGTERM-trapping fujinet grandchild alive after "stop" — still
    // holding the loopback port and contending with the next "start".
    // ESRCH (nothing left) is harmless, zombies awaiting init's reap are
    // unaffected, and killing survivors at stop's conclusion is exactly
    // what the Win32 branch's Job Object does when it closes. (A group
    // liveness probe with kill(-group, 0) is NOT usable as a wait
    // condition here: a grandchild zombie still counts as a member, which
    // would stall every stop for the full grace.)
    ::kill(-group, SIGKILL);
    if (exited || pid_ < 0) return;
    // Reap the corpse; without this the zombie outlives us until POM2 exits.
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    exitCode_ = -1;
    reset();
}

void ChildProcess::stopDetached()
{
    if (pid_ < 0) return;

    // The polite ask happens HERE, synchronously, so the helper starts
    // shutting down at the instant the caller asked for it — only the WAIT is
    // handed off. Same negated pid as stop(): the whole process group.
    const pid_t group = pid_;
    ::kill(-group, SIGTERM);

    // Ownership moves to the thread: this object must not waitpid() for a
    // child somebody else is reaping, and its own destructor must find
    // nothing to do.
    reset();
    exitCode_ = -1;

    // The exception barrier CLAUDE.md requires on every long-lived thread,
    // written out by hand: `pom2::guardedThread` lives in ThreadGuard.h, which
    // is a RUNTIME header, and this file is FOUNDATION — the layer check
    // (cmake/Pom2Architecture.cmake) refuses the include. Nothing in the body
    // below can throw, and if that ever changes the catch is what keeps an
    // escaping exception from calling std::terminate() on the whole emulator.
    // Registered HERE, before the thread starts, so drainDetached() cannot
    // miss a teardown it should have waited for (see DetachedTicket).
    std::thread([group, ticket = DetachedTicket{}] {
        try {
            // The same grace poll stop() does, with the same 25 ms step, and
            // then the same unconditional SIGKILL sweep of the group — a
            // wrapper script that dies on SIGTERM leaves a SIGTERM-trapping
            // grandchild otherwise, still holding the loopback port.
            constexpr int kGraceMs = 2000;
            constexpr int kStepMs  = 25;
            for (int waited = 0; waited < kGraceMs; waited += kStepMs) {
                int status = 0;
                const pid_t r = ::waitpid(group, &status, WNOHANG);
                if (r != 0 && !(r < 0 && errno == EINTR)) break;
                // The interruptible sleep: drainDetached() wakes it so the
                // SIGKILL below lands before main() returns, instead of this
                // thread dying with the process still owing it.
                if (!DetachedTicket::graceStep(kStepMs)) break;
            }
            ::kill(-group, SIGKILL);
            // Reap, or the zombie outlives us until POM2 exits. ECHILD simply
            // means the poll above already collected it.
            int status = 0;
            while (::waitpid(group, &status, 0) < 0 && errno == EINTR) {}
        } catch (...) {
        }
    }).detach();
}

std::string ChildProcess::findOnPath(const std::string& name)
{
    auto executable = [](const std::string& p) {
        struct stat st{};
        return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
               ::access(p.c_str(), X_OK) == 0;
    };

    if (name.find('/') != std::string::npos)
        return executable(name) ? name : std::string{};

    if (const char* path = std::getenv("PATH")) {
        std::string p(path);
        size_t start = 0;
        while (start <= p.size()) {
            const size_t end = p.find(':', start);
            const std::string dir =
                p.substr(start, end == std::string::npos ? std::string::npos
                                                         : end - start);
            if (!dir.empty()) {
                const std::string cand = dir + "/" + name;
                if (executable(cand)) return cand;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    // The places a FujiNet desktop build actually lands when it was not
    // installed to a PATH directory.
    const char* home = std::getenv("HOME");
    std::vector<std::string> extra = {
        "/usr/local/bin/" + name,
        "/opt/" + name + "/" + name,
        "/app/bin/" + name,                       // inside a flatpak
    };
    if (home) {
        extra.push_back(std::string(home) + "/.local/bin/" + name);
        extra.push_back(std::string(home) + "/bin/" + name);
    }
    for (const auto& c : extra)
        if (executable(c)) return c;
    return {};
}

int ChildProcess::runConsoleSignalBrokerIfRequested(int, char*[]) { return -1; }

// ═════════════════════════════════════════════════════════════════════════
// Win32
// ═════════════════════════════════════════════════════════════════════════
#else

namespace {
HANDLE H(void* h) { return static_cast<HANDLE>(h); }

std::wstring quoteWindowsArg(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
        return a;
    std::wstring q = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') {
            q.append(slashes * 2 + 1, L'\\');
            q += L'"';
        } else {
            q.append(slashes, L'\\');
            q += c;
        }
        slashes = 0;
    }
    q.append(slashes * 2, L'\\');
    q += L'"';
    return q;
}

// GenerateConsoleCtrlEvent can only target the caller's console. Launch this
// executable in a tiny broker mode so the long-lived POM2 process never calls
// FreeConsole(), never replaces its console, and never invalidates CRT stdio.
bool launchConsoleSignalBroker(DWORD childPid)
{
    std::vector<wchar_t> self(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, self.data(),
                                      static_cast<DWORD>(self.size()));
    if (n == 0 || n >= self.size()) return false;
    const std::wstring selfPath(self.data(), n);
    std::wstring cmd = quoteWindowsArg(selfPath) +
                       L" --pom2-console-signal-broker " +
                       std::to_wstring(childPid);
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(selfPath.c_str(), mutableCmd.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    const DWORD waited = WaitForSingleObject(pi.hProcess, 2000);
    DWORD code = 1;
    const bool ok = waited == WAIT_OBJECT_0 &&
                    GetExitCodeProcess(pi.hProcess, &code) && code == 0;
    if (waited == WAIT_TIMEOUT) {
        // Do not leave a delayed broker holding only a numeric PID. Once the
        // helper is killed that PID may be reused, and a late AttachConsole
        // could otherwise send Ctrl+C to an unrelated process.
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }
    CloseHandle(pi.hProcess);
    return ok;
}

int runConsoleSignalBroker(DWORD childPid)
{
    if (!AttachConsole(childPid)) return 2;
    if (!SetConsoleCtrlHandler(nullptr, TRUE)) {
        FreeConsole();
        return 3;
    }
    const bool sent = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != FALSE;
    if (sent) Sleep(25);
    FreeConsole();
    return sent ? 0 : 4;
}

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& s)
{
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      s.data(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}
} // namespace

void ChildProcess::reset()
{
    if (handle_) CloseHandle(H(handle_));
    if (job_) CloseHandle(H(job_));
    handle_ = nullptr;
    job_ = nullptr;
}

bool ChildProcess::start(const std::string& exePath,
                         const std::vector<std::string>& args,
                         const std::string& workingDir,
                         std::string& errOut)
{
    if (isRunning()) stop();
    if (exePath.empty()) { errOut = "no helper program configured"; return false; }

    // Win32 takes ONE command line, not an argv array, so every argument has
    // to be quoted the way the CRT will re-split it.
    const std::wstring wideExe = utf8ToWide(exePath);
    const std::wstring wideDir = utf8ToWide(workingDir);
    if (wideExe.empty() || (!workingDir.empty() && wideDir.empty())) {
        errOut = "helper path is not valid UTF-8";
        return false;
    }
    std::wstring cmd = quoteWindowsArg(wideExe);
    for (const auto& a : args) {
        const std::wstring wideArg = utf8ToWide(a);
        if (!a.empty() && wideArg.empty()) {
            errOut = "helper argument is not valid UTF-8";
            return false;
        }
        cmd += L' ';
        cmd += quoteWindowsArg(wideArg);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    // CREATE_NEW_CONSOLE is required for a later Ctrl+C.  Hide that console so
    // launching a helper from the GUI does not pop a terminal window.
    si.dwFlags    = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        errOut = "CreateJobObject failed (" +
                 std::to_string(GetLastError()) + ")";
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        errOut = "SetInformationJobObject failed (" +
                 std::to_string(GetLastError()) + ")";
        CloseHandle(job);
        return false;
    }

    // A dedicated console isolates Ctrl+C to the helper tree.  Do not combine
    // CREATE_NEW_PROCESS_GROUP with this: Windows disables Ctrl+C for the root
    // of a newly-created process group.  The Job Object below remains the hard
    // process-tree containment/fallback mechanism.
    if (!CreateProcessW(wideExe.c_str(), mutableCmd.data(), nullptr, nullptr,
                        FALSE,
                        CREATE_NEW_CONSOLE | CREATE_SUSPENDED,
                        nullptr,
                        workingDir.empty() ? nullptr : wideDir.c_str(),
                        &si, &pi)) {
        const DWORD e = GetLastError();
        errOut = exePath + ": CreateProcess failed (" + std::to_string(e) + ")";
        CloseHandle(job);
        return false;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        const DWORD e = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        errOut = exePath + ": AssignProcessToJobObject failed (" +
                 std::to_string(e) + ")";
        return false;
    }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        const DWORD e = GetLastError();
        TerminateJobObject(job, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        errOut = exePath + ": ResumeThread failed (" +
                 std::to_string(e) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    handle_   = pi.hProcess;
    job_      = job;
    path_     = exePath;
    exitCode_ = -1;
    return true;
}

bool ChildProcess::isRunning()
{
    if (!handle_) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(H(handle_), &code)) { reset(); return false; }
    if (code == STILL_ACTIVE) return true;
    exitCode_ = static_cast<int>(code);
    reset();
    return false;
}

void ChildProcess::stop(int graceMs)
{
    if (!handle_) return;
    // FujiNet-PC's documented shutdown is Ctrl+C.  Its dedicated hidden
    // console makes that event addressable without involving POM2's console.
    // If attach/signal fails, skip the grace delay and use the Job Object.
    const bool signalled = launchConsoleSignalBroker(GetProcessId(H(handle_)));

    bool exited = false;
    if (signalled) {
        // Poll in short steps like the POSIX branch, so a helper that does
        // honour the break does not cost the whole grace period either.
        constexpr DWORD stepMs = 25;
        const DWORD grace = graceMs > 0 ? static_cast<DWORD>(graceMs) : 0;
        for (DWORD waited = 0; waited < grace; waited += stepMs) {
            const DWORD wait = (std::min)(stepMs, grace - waited);
            if (WaitForSingleObject(H(handle_), wait) == WAIT_OBJECT_0) {
                exited = true;
                break;
            }
        }
    }
    if (!exited) {
        if (job_) TerminateJobObject(H(job_), 1);
        else TerminateProcess(H(handle_), 1);
    }
    WaitForSingleObject(H(handle_), 1000);
    exitCode_ = -1;
    reset();
}

void ChildProcess::stopDetached()
{
    if (!handle_) return;

    // Ownership of BOTH handles moves to the thread — the job especially:
    // it carries JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so closing it here would
    // kill the tree instantly and defeat the grace period this preserves.
    // reset() is bypassed for the same reason (it closes them).
    void* const h = handle_;
    void* const j = job_;
    handle_   = nullptr;
    job_      = nullptr;
    exitCode_ = -1;

    // Hand-written exception barrier — see the POSIX branch for why
    // ThreadGuard.h is not available to this file.
    // Registered HERE, before the thread starts — see the POSIX branch.
    std::thread([h, j, ticket = DetachedTicket{}] {
        try {
            const bool signalled =
                launchConsoleSignalBroker(GetProcessId(H(h)));
            bool exited = false;
            if (signalled) {
                constexpr DWORD stepMs = 25;
                constexpr DWORD grace  = 2000;
                for (DWORD waited = 0; waited < grace; waited += stepMs) {
                    if (WaitForSingleObject(H(h), 0) == WAIT_OBJECT_0) {
                        exited = true;
                        break;
                    }
                    // Interruptible: drainDetached() wakes it so the
                    // TerminateJobObject below lands before main() returns.
                    if (!DetachedTicket::graceStep(static_cast<int>(stepMs)))
                        break;
                }
            }
            if (!exited) {
                if (j) TerminateJobObject(H(j), 1);
                else TerminateProcess(H(h), 1);
            }
            WaitForSingleObject(H(h), 1000);
            CloseHandle(H(h));
            if (j) CloseHandle(H(j));
        } catch (...) {
        }
    }).detach();
}

std::string ChildProcess::findOnPath(const std::string& name)
{
    const std::string exe = (name.size() > 4 &&
                             name.compare(name.size() - 4, 4, ".exe") == 0)
                                ? name : name + ".exe";
    const std::wstring wideExe = utf8ToWide(exe);
    if (wideExe.empty()) return {};
    const DWORD needed = SearchPathW(nullptr, wideExe.c_str(), nullptr,
                                     0, nullptr, nullptr);
    if (needed == 0) return {};
    std::vector<wchar_t> buf(static_cast<size_t>(needed) + 1, L'\0');
    wchar_t* filePart = nullptr;
    const DWORD n = SearchPathW(nullptr, wideExe.c_str(), nullptr,
                                static_cast<DWORD>(buf.size()), buf.data(),
                                &filePart);
    if (n > 0 && n < buf.size()) return wideToUtf8(std::wstring(buf.data(), n));
    return {};
}

int ChildProcess::runConsoleSignalBrokerIfRequested(int argc, char* argv[])
{
    if (argc != 3 || !argv || !argv[1] || !argv[2] ||
        std::strcmp(argv[1], "--pom2-console-signal-broker") != 0)
        return -1;
    char* end = nullptr;
    const unsigned long raw = std::strtoul(argv[2], &end, 10);
    if (!end || *end != '\0' || raw == 0 || raw > 0xffffffffUL) return 1;
    return runConsoleSignalBroker(static_cast<DWORD>(raw));
}

#endif // _WIN32
#endif // POM2_HAS_CHILD_PROCESS

} // namespace pom2
