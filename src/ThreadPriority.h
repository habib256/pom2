// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ThreadPriority — ask the OS, best effort, to schedule the emulation thread
// ahead of ordinary work. The speaker and the cards' audio are made on that
// thread: when a busy desktop (a Raspberry Pi behind a compositor) runs it
// late, the audio device runs dry and crackles, and a Pi user had to renice
// POM2 by hand to stop it (Apple II Enthusiasts, 2026-10-20).
//
// Best effort and never real time: a SCHED_FIFO/RR thread that runs flat
// out (the speed menu's MAX) would take a core from the desktop. Linux gets
// a negative nice only when the user's RLIMIT_NICE allows one (no root);
// macOS the user-interactive QoS class; Windows ABOVE_NORMAL. What happened
// is returned as one line for the log.

#include <string>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pom2 {

/// Raise the calling thread's priority as far as the user may; returns
/// what was done, for the log.
inline std::string raiseCurrentThreadPriority()
{
#if defined(__EMSCRIPTEN__)
    return "the browser schedules the emulation";
#elif defined(__linux__)
    // RLIMIT_NICE: the lowest nice reachable is 20 - rlim_cur. Default 0:
    // only a nice of 0 or more, i.e. nothing to gain without a grant.
    struct rlimit limit {};
    int target = 0;
    if (getrlimit(RLIMIT_NICE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
        target = 20 - static_cast<int>(limit.rlim_cur);
    else if (limit.rlim_cur == RLIM_INFINITY)
        target = -20;
    if (target < -10) target = -10;   // ahead of the desktop, not of the kernel's own work
    if (target >= 0)
        return "normal priority (to let POM2 raise it, allow a nice of -10 for this user: "
               "'<user> - nice -10' in /etc/security/limits.conf, then log in again)";
    const auto tid = static_cast<id_t>(syscall(SYS_gettid));
    // The user's own nice wins either way: never lower one they raised
    // (`nice -n -15 POM2`, which -10 would undo), never raise one they
    // lowered on purpose (`nice -n 5 POM2`) (bug hunt 2026-11-25).
    errno = 0;
    const int current = getpriority(PRIO_PROCESS, tid);
    if (errno == 0 && (current <= target || current > 0))
        return "kept at nice " + std::to_string(current);
    if (setpriority(PRIO_PROCESS, tid, target) != 0)
        return "normal priority (setpriority refused nice " + std::to_string(target) + ")";
    return "raised to nice " + std::to_string(target);
#elif defined(__APPLE__)
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0
        ? "raised to the user-interactive QoS class" : "normal priority (QoS refused)";
#elif defined(_WIN32)
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)
        ? "raised to above normal" : "normal priority (SetThreadPriority refused)";
#else
    return "normal priority";
#endif
}

} // namespace pom2
