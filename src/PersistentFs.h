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

// PersistentFs — the second half of browser persistence.
//
// `pom2::userConfigDir()` answers WHERE a durable file goes; under Emscripten
// that is `/persistent`, an IDBFS mount. This header answers WHEN it actually
// becomes durable, which is the part that has no native analogue: a write to
// IDBFS lands in an in-memory image of the mount and reaches IndexedDB only
// when something calls `FS.syncfs(false, …)`. Without that call every write
// succeeds, every read back within the session succeeds, and the whole lot
// evaporates on reload — the failure mode POM2's own backlog names most
// often: a mechanism that reports success while doing nothing.
//
// The flush is asynchronous and not free (it walks the mount), so it is
// **debounced**: writers call `markPersistentStateDirty()`, and the frame
// loop calls `pumpPersistentState()` once per frame, which starts at most
// one flush per `kFlushDebounceSeconds` and never starts one while another
// is still in flight. Coalescing matters because the settings store is saved
// from ~14 call sites, several of which fire together when a profile switch
// rewrites the slot map.
//
// `FS.syncfs` cannot be made synchronous, so no entry point here blocks —
// `flushPersistentStateNow()` skips the debounce but still returns before the
// data has landed, and a `beforeunload` handler could not wait for it either.
// Durability therefore comes from flushing shortly AFTER each write rather
// than just before the tab closes, which is why the debounce is a couple of
// seconds and not a minute.
//
// Everything here compiles to nothing on native builds: the desktop write
// path is `AtomicFileReplace` (rename + fsync), already durable when it
// returns.

#ifndef POM2_PERSISTENT_FS_H
#define POM2_PERSISTENT_FS_H

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#endif

namespace pom2 {

/// Seconds between two flushes of the persistent store. Long enough to
/// coalesce a burst of settings writes, short enough that a user who
/// changes something and immediately reloads keeps the change.
inline constexpr double kFlushDebounceSeconds = 2.0;

/// Record that something durable was written. Cheap; safe to call from any
/// write path, including ones that run on every frame.
inline void markPersistentStateDirty()
{
#ifdef __EMSCRIPTEN__
    EM_ASM({ Module.pom2StateDirty = true; });
#endif
}

/// Give the persistence layer a chance to run. Call once per frame from the
/// render loop. No-op unless something is dirty, the debounce has elapsed,
/// and no flush is already in flight.
inline void pumpPersistentState()
{
#ifdef __EMSCRIPTEN__
    // The whole policy lives in this one EM_ASM rather than being split
    // across C++ and JS: the state it tests (in flight? dirty? when was the
    // last one?) is owned by the async callback, and reading it back into
    // C++ every frame just to decide would cost a call per frame for a
    // decision JS can make in three comparisons.
    EM_ASM({
        if (!Module.pom2StateDirty || Module.pom2FlushInFlight) return;
        var now = Date.now();
        if (Module.pom2LastFlush &&
            now - Module.pom2LastFlush < $0 * 1000) return;
        // Clear the flag BEFORE the flush, not after: a write that lands
        // while the flush is walking the mount may or may not be included,
        // and clearing afterwards would mark that write clean without having
        // stored it. Clearing first costs at most one redundant flush.
        Module.pom2StateDirty = false;
        Module.pom2FlushInFlight = true;
        FS.syncfs(false, function(err) {
            Module.pom2FlushInFlight = false;
            Module.pom2LastFlush = Date.now();
            if (err) {
                // Quota exceeded, or a private-mode browser refusing
                // IndexedDB. Nothing POM2 can do about it, but silence here
                // is what makes "my settings do not stick" unreportable.
                console.warn('POM2: could not persist settings:', err);
                Module.pom2PersistFailed = true;
                // A failed flush stored nothing: the state is still dirty,
                // and clearing the flag before starting had already said
                // otherwise. Without this, one quota error made every later
                // write invisible to the pump until something else set the
                // flag again.
                Module.pom2StateDirty = true;
            }
            // Re-arm for anything that asked while this was in flight:
            // flushPersistentStateNow() sets the flag instead of starting a
            // second sync, and the pump would otherwise not look at it again
            // until the NEXT write — so a pagehide during a running flush
            // dropped exactly the data it was trying to save.
            if (Module.pom2FlushRequested) {
                Module.pom2FlushRequested = false;
                Module.pom2StateDirty = true;
            }
        });
    }, kFlushDebounceSeconds);
#endif
}

/// Start a flush right now, ignoring the debounce (but still not while one
/// is in flight). For the moment the user is leaving: a `visibilitychange`
/// to hidden, or a `pagehide`. Still asynchronous — nothing can make
/// `FS.syncfs` synchronous — so this is best-effort by construction, which
/// is why the debounced pump above exists rather than a flush-on-exit.
inline void flushPersistentStateNow()
{
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (Module.pom2FlushInFlight) {
            // Ask the in-flight flush's callback to run another one. Setting
            // only `pom2StateDirty` was not enough: the pump ignores it for
            // `kFlushDebounceSeconds` after the running flush completes, and
            // this call site is "the user is leaving" — there is no next
            // frame to be debounced into.
            Module.pom2StateDirty = true;
            Module.pom2FlushRequested = true;
            return;
        }
        Module.pom2StateDirty = false;
        Module.pom2FlushInFlight = true;
        FS.syncfs(false, function(err) {
            Module.pom2FlushInFlight = false;
            Module.pom2LastFlush = Date.now();
            if (err) {
                console.warn('POM2: could not persist settings:', err);
                Module.pom2StateDirty = true;   // stored nothing — still dirty
            }
            if (Module.pom2FlushRequested) {
                Module.pom2FlushRequested = false;
                Module.pom2StateDirty = false;
                Module.pom2FlushInFlight = true;
                FS.syncfs(false, function(e2) {
                    Module.pom2FlushInFlight = false;
                    Module.pom2LastFlush = Date.now();
                    if (e2) {
                        console.warn('POM2: could not persist settings:', e2);
                        Module.pom2StateDirty = true;
                    }
                });
            }
        });
    });
#endif
}

} // namespace pom2

#endif // POM2_PERSISTENT_FS_H
