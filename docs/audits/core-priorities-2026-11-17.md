# First core-priority batch — 2026-11-17

Scope: STORAGE-014, ARCH-014 and ARCH-015, following the prioritised TODO.
Baseline: `ce79bb0e` plus the desktop association/native-picker working-tree
changes. No CPU, device timing or display golden was intentionally changed.

## Host-folder eject: a reproduced defect

`storage_folder_eject_race` subclasses the real ProDOS HDV card only to wrap
the pending payload's existing `CommitBarrier`. Its `wait()` injects a guest
write under the machine lock, after capture but before the unlocked commit.
A non-blocking lock assertion checks that phase 2 releases the machine lock.
The decoder, card, backing and coordinator are production implementations;
no runtime test hook or sleep is added.

The baseline failed with one racing write:

```text
phase-2 writes=1: expected AB000, got A0000
notice: 1 conflict(s): guest copies in .pom2-conflicts/
```

The first decode updated the host file's timestamp but did not update the
mounted backing's timestamp. Its own save became a supposed external edit
on the next capture. Success now adopts the published timestamp just as
partial failure does, in both `ejectMediaBay` and `ejectAllMedia`. Completion is applied only to the original mount
identity; a replacement inherits neither its timestamp nor failed dirty set.

After the fix:

| Guest writes after successive captures | Captures | Main file | Dirty at final eject |
|---|---|---|---|
| 0 | 1 | `A0000` | No |
| 1 | 2 | `AB000` | No |
| 3 | 4 | `ABCD0` | No |
| 4 | 4 | `ABCDE` | Yes; final eject flushes it |
| 1, eject-all command | 1 | `AB000` | Yes; final eject flushes it |

The eject-all variant was added during final review. It reproduced the same
`AB000` → `A0000` false conflict after the single-bay fix: its successful
phase-2 commit also skipped timestamp adoption before the final inline flush.
That path now adopts success/failure timestamps only for the original mount.

Each scenario requires successful eject, an unloaded/clean bay and no
`.pom2-conflicts` directory. The fourth case's inline flush measured about
0.65 ms on the local Mac, on a five-byte file in a 72-block synthetic volume.
**This is not a latency bound for a large folder.** The four-round retry
policy and final locked decode are retained, with the residual now tested.
This is not an asynchronous or lock-free eject implementation.

## Audio clock fan-out

Atomic, read-only getters expose the speaker reconstructor clock, cassette
monitor clock and each floppy sound's stamp clock. They read the same fields
the audio code consumes. No retune or synthesis behavior changes.

`device_standard_clock` poisons each audio clock before a retune and covers
NTSC → PAL → NTSC at stock/x2/x4 paced rates. Witness cards in slots zero
and seven verify the generic paced/machine/crystal fan-out. Existing Sony,
Liron and Disk II FLUX period checks remain. The speaker getter represents
its reconstruction clock, which is stock time after an accelerator transition;
the cassette getter represents realtime monitoring, not tape-file timing.

Four isolated fault injections into `EmulationController::setVideoStandard`
remove the speaker, cassette, 5.25-inch sound or 3.5-inch sound retune. Every
mutant fails `device_standard_clock` (exit 1).

## Slot composition policy

`SlotConfigurationCoordinator_Build.cpp` now owns the sequence used by
MainWindow's actual composer:

1. Dispatch the resolved plan, preserving profile and uniqueness policy.
2. Add the enabled, profile-eligible Premium auxiliary card.
3. Run runtime overlays (printer sound registration and CLI FujiNet in the GUI).
4. Restore saved media only after the whole topology exists.
5. Obtain fresh clocks and retune every plugged card, including slot zero.

The coordinator requires `StateAccess` and never takes the machine lock.
Media restoration retains the established stopped-worker rebuild exception.
Bus teardown still belongs to `SlotRebuildCoordinator`; per-card UI/audio/
transport injection remains with MainWindow. This extraction is not a claim
that every frontend callback or transport is now covered headlessly.

`slot_configuration_build` exercises the shared policy using real Disk II and
HDV cards, two Disk II cards, both drives of the primary card and an overlay
that changes the primary HDV. It checks saved incompatible settings survive,
missing paths return their warnings/keys, no media is restored during card
construction, and clocks are obtained after restoration. Auxiliary enable/
profile eligibility is checked on II+, IIe, IIe PAL, IIc and IIc+.

Four isolated mutations fail the test (assertion abort): omit media restore,
omit runtime overlays, start the clock loop at slot one, or drop the Premium
SoftCard enable condition. Mutants are compiled in a temporary directory
against the actual test objects/library; the working-tree sources are not
mutated during the build.

## Validation

The complete local macOS ARM64 Release build passed. The 494-test suite
completed with **493 passes, one optional-media skip (`arekkusu_disks`), no
failures**, in 576.56 s. This run used permitted local sockets and graphics;
the skip guard reported no unexpected skips. `bench_identity`, the new folder
race and slot composition tests, and `device_standard_clock` all passed.
All eight clock/composition mutation controls failed as expected.

The eject-all extension was added after that full run. Its test failed before
the extra fix and passed afterwards. The final incremental build passed,
followed by **18/18 storage/profile/clock regressions** in 5.48 s, including
`storage_folder_eject_race`, `storage_coordinator`, `storage_lock_budget`,
`storage_deferred_diversion`, `prodos_synth_failed_flush`, `media_contract`,
`profile_switch`, both slot-configuration tests, `device_standard_clock` and
`bench_identity`. This focused rerun validates the final eject-all extension;
the complete-suite result above predates that extension.

File-size, version, settings-key, workflow-pin, include, documentation-reference,
coordinator-lock and ctest-inventory guards passed. No size ceiling was raised;
all 494 expected local test names are registered.
No Windows/Linux execution or interactive native-dialog acceptance is implied.
