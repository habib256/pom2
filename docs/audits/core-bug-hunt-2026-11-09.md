# Core bug hunt — 2026-11-09

Follow-up to the [core consolidation](core-consolidation-2026-11-09.md), on
the same uncommitted working tree. Existing unrelated CI/PLASMA changes were
preserved. Three focused reviews used GPT-6.1 Sol agents; findings below are
local executable regressions, not physical-hardware measurements.

## ALS CP/M: pending interrupt after an accelerator change

`nextTick_` retained its old absolute CPU-cycle deadline when the hardware
clock changed. A transition from 1x to 4x delivered the pending 60 Hz tick
too early in machine time; the reverse delivered it late. The regression
failed four checks before the fix (both directions, PAL and NTSC).

The setter now rescales the remaining interval, retaining an already-due
edge. Once machine time is supplied, paced notifications no longer rebase
the deadline. Tests include repeated menu changes around the hardware
transition. Rounding is upward by at most one CPU cycle per transition;
the snapshot format is unchanged.

## Floppy save: hidden diversion after an external edit

Both `DiskImage::saveDirty()` and `Disk35Image::saveDirty()` discarded the
successful commit's `divertedTo` result. The external file and the guest's
sibling copy were safe, but persistence status could incorrectly say
`saved`, without telling the user where the guest's data went.

Both inline save paths now retain the same sticky diversion notice as the
background autosave path. This does not replace an in-flight operation or
change its write serial. Tests verify the external bytes, complete sibling
contents, repeated saves and the visible sibling path. Before the fix,
the 5.25-inch test failed four status/path checks and the 3.5-inch test
failed its diverted-status assertion.

The deferred `StorageCoordinator::commitDeferredFlushes` path was subsequently
reproduced too: its commit omitted the diversion result and notified the
still-mounted Liron bay only on failure. A successful diverted flush therefore
looked saved. It now forwards the sibling path under a fresh state lock,
checking the captured mount identity before publishing the notice. A same-path
remount must not inherit the old mount's notice, and writes made after capture
remain dirty. The dedicated `storage_deferred_diversion` regression checks
these three cases and verifies both the external file and complete sibling
contents; its first case failed on the pre-fix code.

## Corpus: partial execution must not overwrite a complete baseline

With `--bless`, a successful check followed by a failed disk swap replaced
all expectations for that title with the partial results and exited 0.
This was reproduced with a synthetic disk and an invalid WOZ: the first
check survived, the later expectation disappeared, and the runner printed
`Blessed: 1` despite reporting a failed swap.

The runner now tracks script completion separately from frame comparison.
An interrupted script retains its old expectations and exits unsuccessfully;
a completed script with changed frame hashes can still be intentionally
blessed. A run with no runnable titles returns 77 and leaves expectations
alone. The new `corpus_bless` integration test invokes the actual runner
with synthetic fixtures, requiring no downloaded media. It fails against
the pre-fix binary and covers partial failure, legitimate changed hashes,
replay and an entirely skipped run.

## Scope

These fixes do not complete CPU-1: SSI263 timing, hybrid speech/PCM and serial
timing remain separate work. No throughput improvement is claimed and no
timing benchmark was run during this hunt.

## Validation

Local macOS ARM64 Release validation: `als_cpm_card` passed with its real
CP/M boots and no internal skips; `media_external_change` and
`disk35_atomic_save` passed; `corpus_manifest` and the new `corpus_bless`
passed. Cross-review found no additional regression in the changed paths.
After rebuilding the shared core, `accelerator_clock_chunk` and
`session_writable_media` also passed: **7/7 targeted tests** across these
runs. File-size, include, documentation-reference and whitespace guards
passed. This was targeted validation, not the full CTest suite.
The protected corpus replayed unchanged: **3/3 titles, 9/9 checks, no skips**.
The default corpus also replayed unchanged: **10/10 runnable titles,
29/29 checks**, including the complete DIX tour; the existing absent
`service-disk-1` diagnostic image was the sole skip.
No expected-frame file or source media was modified.

Deferred-flush follow-up: `storage_deferred_diversion` and
`storage_coordinator` were rebuilt against the changed core and passed.
`media_external_change`, `disk35_atomic_save`, `corpus_bless` and
`session_writable_media` also passed: **6/6 targeted tests**. The new
regression additionally commits a post-capture write and verifies the updated
sibling and unchanged external file. File-size, include and documentation
reference guards passed; `git diff --check` was clean. No full-suite or
cross-platform result is claimed.

### Replacement-mount follow-up

`flushOutgoingBay` had a separate whole-image commit which also dropped the
diversion path. After an external rewrite, attempting to mount a missing
replacement saved the outgoing Liron image to its sibling, retained the old
mount, but reported neither its diverted status nor a command notice. A
successful replacement similarly omitted the outgoing sibling from its result.
The expanded `storage_deferred_diversion` test reproduced the missing notice
before the fix. Both replacement outcomes now preserve the notice; a failed
replacement also leaves the original mount visibly diverted. The successful
replacement starts with its own clean status. All five regression scenarios
verify the external file and complete sibling contents.

Rebuilt `storage_deferred_diversion` and `storage_coordinator`; both pass.
`media_external_change` and `disk35_atomic_save` also pass: **4/4 targeted
tests** for this follow-up. `git diff --check` passes; the coordinator remains
below its existing file-size ceiling. No full-suite result is claimed.

### Invalid session cleanup

The Disk II session validator had a side effect on rejection: its destructor
called `flushPendingWrites`, which executed a captured burst even when the
preceding timeline check had rejected its timestamps. A synthetic writable
NIB session with an active burst, `lssCycle = 2^34` and `cpuCycleTotal = 64`
reproduced process termination by an uncaught `std::length_error` during
cleanup. The existing timeline guard recognized the invalid gap but could
not prevent its destructor from consuming that gap as a write span.

The validator now has a deleter which disables write-back before destruction
on every exit. Validation cannot flush its temporary media, regardless of
whether parsing, timeline or canonical-layout checks fail. Production card
destructors retain their normal flush behavior. `session_writable_media`
covers the malformed input and a valid active pending burst; the latter is
now explicitly active even on hosts without a P6 ROM. Both scenarios pass.

Validation for this fix: rebuilt `session_writable_media`, `session_snapshot`,
`card_snapshot_contract` and `storage_deferred_diversion`; **4/4 passed**.
`git diff --check` passed. No complete suite or cross-platform run was made.

### Oversized pending writes with consistent clocks

A second malformed Disk II session used the same `2^34`-cycle pending write
but paired it with `cpuCycleTotal = 2^33`. This passed the existing absolute
clock and catch-up checks: the CPU/LSS clocks agreed, and every transition
was between the write start and the LSS clock. The regression failed because
`prepareSessionState` accepted the record. Deferring the burst until resume
would still reach the nibble cell-count overflow demonstrated in the cleanup
crash above.

Pending session writes now have an explicit 400000-LSS-cycle work limit,
matching the existing catch-up budget. Both capture and load enforce it:
capture refuses unsupported live spans without changing the output buffer,
instead of producing an archive that cannot load. Tests cover the formerly
accepted huge span, acceptance at 400000, rejection at 400002 and capture
refusal. This is a session-format work limit; older records above it are now
refused, and this change does not bound normal runtime writes.

Rebuilt and passed `session_writable_media`, `session_snapshot` and
`card_snapshot_contract`: **3/3 targeted tests**. `git diff --check` passed.
No full-suite or cross-platform result is claimed.

### Replacement during an unlocked bay eject

`ejectMediaBay` re-resolved the bay by slot after its unlocked save, but did
not check the mount identity. A deterministic regression held the media-commit
mutex, waited for the outgoing Liron image's capture to retire its dirty flag,
mounted another image, then released the save. Before the fix the new disk
was ejected; the assertion that it remained mounted failed.

The command now retains the first captured mount's identity and checks it
before repeat captures, after each unlocked save and before the final eject.
A changed mount returns an error without updating settings or invalidating
rewind for a successful eject. Cards without a mount identity fall back to
loaded state and path, as the existing eject-all protection does.

The regression covers both a different file and a same-path remount, with a
new unsaved block on the replacement. Both remain mounted and dirty, the
replacement's new block is not flushed, and the outgoing file contains its
captured write. Rebuilt `storage_deferred_diversion` and
`storage_coordinator`: **2/2 targeted tests passed**. `git diff --check` passed;
the coordinator remains below its existing file-size ceiling. No TSan,
full-suite or cross-platform result is claimed.

### SmartPort type change after a concurrent replacement

`setMediaBayType` had the same missing mount-identity check on a separate
path. With an outgoing SmartPort 3.5-inch save paused at the media-commit
mutex, the test mounted another image and dirtied a block on it. Releasing
the save let the pending type-clear command flush and remove the newcomer.
The pre-fix regression failed because that replacement was no longer mounted.

The type command now compares the captured mount identity under the final
state lock before flushing remaining writes or destroying the unit. A changed
mount returns an error before settings are updated. Cards without mount IDs
retain the loaded-state/path fallback. The regression covers a different
file and a same-path remount; both remain mounted and dirty, while only the
outgoing captured block reaches disk. Existing normal type-change cases in
`storage_coordinator` still pass.

Rebuilt `storage_deferred_diversion` and `storage_coordinator`: **2/2 targeted
tests passed**. `git diff --check` passed, and the coordinator remains below
its existing size ceiling. No TSan or full-suite run was performed.

### Stale mount command after an outgoing save

`mountMediaBay` also re-resolved a bay without verifying its mount identity.
A command to mount image C captured dirty image A; while that save was paused
at the commit mutex, the test installed B and dirtied a block on it. When A's
save resumed, the stale command flushed B and installed C. The regression
failed because B was no longer mounted.

`flushOutgoingBay` can now return the identity observed under the same lock
as payload capture. `mountMediaBay` checks it before adopting or loading the
requested image. A replacement returns an error before touching its bytes,
settings or successful-mount rewind invalidation. The deterministic test
covers both B as a different file and a same-path remount of A, retaining the
replacement's unsaved block and verifying that only A's captured write reached
disk. Ordinary successful and failed mounts remain covered by the existing
coordinator tests.

Rebuilt `storage_deferred_diversion` and `storage_coordinator`: **2/2 targeted
tests passed**. `git diff --check` passed; the coordinator stays below its
existing size ceiling. No TSan or full-suite run was performed.

### Routed SmartPort 3.5-inch mount

`mountDisk35` has a separate SmartPort route which calls
`mountSmartPortUnitAs` rather than `mountMediaBay`. It therefore bypassed the
generic mount-identity guard. Pausing its outgoing save, installing and
writing another disk, then resuming the command reproduced an unwanted
replacement; the pre-fix regression failed because the newcomer disappeared.

The routed command now retains the primary SmartPort slot and captures its
outgoing mount identity with the write-back payload. Under the final state
lock it refuses a changed primary slot or a changed medium before calling
`mountSmartPortUnitAs`. The deterministic regression covers replacement by a
different file and a same-path remount, preserving both the mount and its
pending writes while checking the outgoing file's complete captured data.
The new route-change check itself has not been exercised by this regression.

Rebuilt `storage_deferred_diversion` and `storage_coordinator`: **2/2 targeted
tests passed**. `git diff --check` passed; the coordinator remains within its
existing size ceiling. No TSan or full-suite run was performed.

### Routed HDV mount

`mountHdv` separately chose a preferred block card or SmartPort unit, saved
its outgoing medium unlocked, then re-selected and replaced whatever the bay
held. The regression paused a SmartPort 3.5-inch outgoing save, installed and
dirtied another disk, then resumed a request to mount a 2 MiB HDV. Before the
fix the command changed the unit to HDV and the replacement disappeared; its
mounted/path assertion failed.

The command now retains the initially selected slot and the outgoing mount
identity captured with its payload. Under the final lock it refuses a changed
route or changed medium before adoption, fallback loading or unit-type change.
The regression covers both a different replacement file and a same-path
remount, retaining the newcomer's pending block and verifying that only the
outgoing capture reached its file. The same guard covers the preferred-block
branch, but this new race regression exercises the SmartPort route only and
does not directly exercise a topology change.

Rebuilt `storage_deferred_diversion` and `storage_coordinator`: **2/2 targeted
tests passed**, including existing ordinary routed HDV mounting.
`git diff --check` passed; the coordinator stays below its existing size
ceiling. No TSan or full-suite run was performed.

## Pre-merge targeted replay

A final local replay passed **23/23 selected CTest entries** covering the
changed peripheral clocks, disk persistence and stack behavior, corpus
manifest/blessing, writable session handling, card snapshots and coordinator
operations. This includes `storage_deferred_diversion` with its deterministic
replacement races. The full 487-entry suite and remote workflows were not run.
