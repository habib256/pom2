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

// POM2 — W5100 virtual-DNS resolver.
//
// The lookup is injectable, so these cases run with no network and no
// waiting. All three were previously unreachable: the bounded wait could only
// be exercised against a genuinely slow resolver, and the in-flight cap only
// by a guest looping OPEN over hostnames faster than DNS could answer.

#include "W5100NameResolver.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

using namespace pom2;
using Status = W5100NameResolver::Status;

// ── A cached answer costs no lookup at all ───────────────────────────────
void testCacheAvoidsSecondLookup()
{
    std::atomic<int> calls{ 0 };
    W5100NameResolver resolver([&calls](const std::string&) {
        ++calls;
        return 0x0100007Fu;   // 127.0.0.1, network byte order
    });

    auto first = resolver.resolve("example.test", 1000);
    assert(first.status == Status::Resolved);
    assert(first.address == 0x0100007Fu);
    assert(calls == 1);

    auto second = resolver.resolve("example.test", 1000);
    assert(second.status == Status::Resolved);
    assert(second.address == 0x0100007Fu);
    assert(calls == 1);   // answered from cache

    std::printf("  a cached answer costs no second lookup: OK\n");
}

// ── A name that does not resolve is remembered as "no" ───────────────────
//
// Negative caching matters here: a guest that keeps retrying a bad name would
// otherwise start a resolver thread every time.
void testFailureIsCachedToo()
{
    std::atomic<int> calls{ 0 };
    W5100NameResolver resolver([&calls](const std::string&) {
        ++calls;
        return 0u;
    });

    assert(resolver.resolve("nope.test", 1000).status == Status::Failed);
    assert(resolver.resolve("nope.test", 1000).status == Status::Failed);
    assert(calls == 1);

    std::printf("  a failed lookup is cached as a failure: OK\n");
}

// ── A slow lookup times out, then lands via poll() ───────────────────────
//
// This is the rule the whole class exists for: resolve() is called on the CPU
// thread under stateMutex, so it must NOT wait for a slow resolver. The
// answer is not thrown away — it arrives in the mailbox and poll() folds it
// into the cache on the CPU thread, which is what keeps the cache lock-free.
void testSlowLookupTimesOutThenArrives()
{
    W5100NameResolver resolver([](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return 0x0200007Fu;
    });

    const auto pending = resolver.resolve("slow.test", 10);
    assert(pending.status == Status::Pending);
    assert(pending.address == 0);

    // Nothing is in the cache yet — the answer has not been folded in.
    resolver.poll();

    // Give the detached resolver time to park its answer, then drain.
    for (int i = 0; i < 100; ++i) {
        resolver.poll();
        if (resolver.cacheSize() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(resolver.cacheSize() == 1);

    // Now it answers immediately, from the cache.
    const auto now = resolver.resolve("slow.test", 0);
    assert(now.status == Status::Resolved);
    assert(now.address == 0x0200007Fu);

    std::printf("  a slow lookup times out, then arrives via poll(): OK\n");
}

// ── In-flight lookups are capped ─────────────────────────────────────────
//
// A guest looping OPEN over random hostnames must not pile up resolver
// threads without bound. The cap is checked BEFORE std::async is called,
// because a future's destructor would block.
void testInFlightLookupsAreCapped()
{
    std::atomic<bool> release{ false };
    W5100NameResolver resolver([&release](const std::string&) {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 0x0300007Fu;
    });

    int refused = 0;
    for (int i = 0; i < W5100NameResolver::kMaxInFlight + 4; ++i) {
        const auto r = resolver.resolve("host" + std::to_string(i) + ".test", 1);
        if (r.status == Status::Refused) ++refused;
    }
    assert(refused > 0);   // the cap engaged rather than spawning unbounded

    release = true;
    // Let the parked lookups finish so the threads exit before teardown.
    for (int i = 0; i < 200; ++i) {
        resolver.poll();
        if (resolver.cacheSize() >= W5100NameResolver::kMaxInFlight) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::printf("  in-flight lookups are capped: OK\n");
}

// ── New lookups are rate limited ─────────────────────────────────────────
//
// The in-flight cap bounds resolver THREADS, not queries: an instant lookup
// frees its slot instantly, so a guest looping OPEN over
// `<payload>.attacker.example` had an unmetered channel out through the host's
// own resolver — a DNS exfiltration path with POM2 doing the transmitting.
// This is the meter on that, and it deliberately sits AFTER the cache lookup:
// a guest reconnecting to one host in a loop is ordinary and costs nothing.
void testNewLookupsAreRateLimited()
{
    std::atomic<int> calls{ 0 };
    W5100NameResolver resolver([&calls](const std::string&) {
        ++calls;
        return 0x0400007Fu;
    });

    // A burst is allowed — the bucket starts full.
    for (int i = 0; i < W5100NameResolver::kResolveBurst; ++i) {
        const auto r = resolver.resolve("burst" + std::to_string(i) + ".test",
                                        1000);
        assert(r.status == Status::Resolved);
    }
    assert(calls == W5100NameResolver::kResolveBurst);

    // The next NEW name is refused rather than sent, and the lookup is not
    // even attempted — which is the property that matters.
    const auto refused = resolver.resolve("payload.attacker.test", 1000);
    assert(refused.status == Status::Refused);
    assert(calls == W5100NameResolver::kResolveBurst);

    // A name already in the cache still answers instantly: the limit meters
    // what leaves the machine, not what the guest asks for.
    const auto cached = resolver.resolve("burst0.test", 1000);
    assert(cached.status == Status::Resolved);
    assert(calls == W5100NameResolver::kResolveBurst);

    // And the bucket refills: a short wait buys another lookup.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto later = resolver.resolve("later.test", 1000);
    assert(later.status == Status::Resolved);

    std::printf("  new lookups are rate limited, cache hits are free: OK\n");
}

} // namespace

int main()
{
    std::printf("W5100 virtual-DNS resolver\n");
    testCacheAvoidsSecondLookup();
    testFailureIsCachedToo();
    testSlowLookupTimesOutThenArrives();
    testInFlightLookupsAreCapped();
    testNewLookupsAreRateLimited();
    std::printf("OK\n");
    return 0;
}
