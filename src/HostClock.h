// POM2 — GPL-3.0-or-later
//
// The host's local time as the emulated machine reads it: the No-Slot
// Clock, the ThunderClock card, the Vs drive protocol's ProDOS date. One
// function, so that a recording or a playback of host input
// (EmulationController_Input.cpp) can put a clock of its own in its place —
// the recorded start instant and its UTC offset, plus the EMULATED time
// elapsed since — and a program that reads the date replays the same
// bytes, on another day and in another time zone.
#pragma once

#include <atomic>
#include <ctime>

namespace pom2 {

using HostTimeFn = std::tm (*)(void* context);

struct HostTimeOverride {
    std::atomic<HostTimeFn> fn{nullptr};
    std::atomic<void*>      context{nullptr};
};

inline HostTimeOverride& hostTimeOverride()
{
    static HostTimeOverride o;
    return o;
}

/// Set (context first, then the function) or clear (function first).
inline void setHostTimeOverride(HostTimeFn fn, void* context)
{
    HostTimeOverride& o = hostTimeOverride();
    if (fn) { o.context.store(context, std::memory_order_release); o.fn.store(fn, std::memory_order_release); }
    else    { o.fn.store(nullptr, std::memory_order_release); o.context.store(nullptr, std::memory_order_release); }
}

inline std::tm utcTime(std::time_t t)
{
    std::tm out{};
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

inline std::tm localTimeOf(std::time_t t)
{
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

/// The local time now — or the override's, while one is set.
inline std::tm hostLocalTimeNow()
{
    HostTimeOverride& o = hostTimeOverride();
    if (const HostTimeFn fn = o.fn.load(std::memory_order_acquire))
        return fn(o.context.load(std::memory_order_acquire));
    return localTimeOf(std::time(nullptr));
}

/// Seconds the local time is ahead of UTC at `t` (UTC+4 → 14400).
inline long utcOffsetSeconds(std::time_t t)
{
    std::tm local = localTimeOf(t), utc = utcTime(t);
    local.tm_isdst = 0; utc.tm_isdst = 0;
    // Both read as if local: the difference is the offset, DST included.
    return static_cast<long>(std::difftime(std::mktime(&local), std::mktime(&utc)));
}

} // namespace pom2
