# Backlog — Architecture and tooling

[Planning index](../../TODO.md) · [Scope decisions](../decisions/project-scope.md)

Reorganised 2026-10-05. Priorities are inherited from the original TODO;
older reports are **To verify**, not confirmed defects on current main.
Before implementation, reproduce the described behavior on current main and
record the result. If already resolved, remove the active entry and link its
resolution in the changelog. Frozen work requires a named software need or
an explicit request before scheduling.

<a id="historical-findings"></a>

## Task index

| Task | Priority | State | Subject |
|---|---|---|---|
| [ARCH-016](#arch-016) | 🟡 | Ready | Documentation drift checker |
| [ARCH-004](#arch-004) | 🟠 | Partial | Six file-size ceilings were raised by the two hunt rounds, and the owed splits are still owed |
| [ARCH-009](#arch-009) | 🟠 | Partial: Uthernet panel only | Remaining ImGui panel concurrency coverage |
| [ARCH-003](#arch-003) | 🟡 | Confirmed open | ThreadSanitizer: the GUI half is still open. |
| [ARCH-008](#arch-008) | 🟡 | Partial: sprite seam landed; paint seam frozen | `hgrpaint/` has no headless harness |
| [ARCH-010](#arch-010) | 🟡 | Confirmed open (post-1.0) | Scattered config |
| [ARCH-011](#arch-011) | 🟡 | Needs decision | `stateMutex` shared CPU+UI |
| [ARCH-007](#arch-007) | 🧊 | Frozen — triage 2026-10-28 (won't do) | Z80/SoftCard cleanup backlog |
| [ARCH-012](#arch-012) | 🟢 | Confirmed open (post-1.0) | Inconsistent `pom2::` namespace |
| [ARCH-013](#arch-013) | 🟢 | Confirmed open | Legacy M6502 style |

<a id="arch-016"></a>

## ARCH-016 — Documentation drift checker

**Priority:** 🟡 · **State:** Ready ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)).

**Acceptance criterion:** A `tools/check_doc_refs.sh` (shape of
`check_version_strings.sh`, run in the CI Linux job, with a `--self-test`
holding negative controls) fails when a Markdown file under the repo root,
`docs/backlog/`, `docs/decisions/` or `docs/audits/` (not `docs/archive/`)
cites a backlog ID with no entry, a `docs/backlog/*.md#anchor` that does not
exist, a ctest name that is not registered, or a catalog key absent from
`SlotCardCatalog`; and when one backlog ID is defined twice across
`docs/backlog/` and `CHANGELOG.md`.

**Evidence:** the 2026-10-28 triage found `CARDS-011` used for two items
(`cards.md` and `CHANGELOG.md:945`), the RVB Graph described three ways in
three documents, and two index/body state disagreements (UI-003,
STORAGE-014). About 30 000 lines of prose (DEV, CHANGELOG, docs/) drift
faster than review can catch.

<a id="arch-004"></a>

## ARCH-004 — Six file-size ceilings were raised by the two hunt rounds, and the owed splits are still owed

**Priority:** 🟠 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): Memory and ImageWriter splits landed; the DiskImage WOZ save half and Apple2Display are owed, and two ceilings have slack.

**Acceptance criterion (after revalidation):** Split the identified responsibilities without increasing file-size ceilings; preserve the affected printer, media and snapshot tests.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-925). Original dates and estimates are retained below.

- 🟠 **Six file-size ceilings were raised by the two hunt rounds, and the owed
  splits are still owed** *(2026-09-07)*. `Apple2Display.cpp` 2294 → 2382,
  `DiskImage.cpp` 2835 → 2914, `ImageWriter.cpp` 2501 → 2531, `M6502.cpp`
  2159 → 2185, `Memory.cpp` 2442 → 2545, `hgrpaint/HgrPaintEditor.cpp`
  2525 → 2647. Every added line is a fix or the comment explaining one, and
  each is recorded at its **exact** current size so the ratchet still fails on
  the next line any of them gains — but the two obvious seams named in
  `tools/file_size_budget.txt` remain open jobs: `Memory.cpp`'s snapshot
  trailer and `DiskImage.cpp`'s WOZ writer.

**Revalidation 2026-10-06 (`wc -l` against `tools/file_size_budget.txt`):**
the debt has grown, not shrunk — `Apple2Display.cpp` 2536, `DiskImage.cpp`
3293, `ImageWriter.cpp` 2809, `M6502.cpp` 2359, `Memory.cpp` 2766,
`hgrpaint/HgrPaintEditor.cpp` 2656; neither named seam has been cut.
`Memory.cpp` is **over** its recorded ceiling of 2751 (+15, from `95cf8bd`),
so `tools/check_file_sizes.sh` failed on that revision, as did the
2026-10-06 push CI's Linux job.

**2026-10-24:** the `Memory.cpp` seam is cut — the snapshot trailer
(`appendSnapshotState` / `loadSnapshotState`) is `MemorySnapshot.cpp`, and
the ceiling went 2789 → 2354. Still owed: `DiskImage.cpp`'s WOZ writer,
`ImageWriter.cpp`, `Apple2Display.cpp`, `hgrpaint/HgrPaintEditor.cpp`.

**Documentation recheck 2026-10-20:** the current file-size guard passes;
the historical ceiling violation is not a current failure. Responsibility
splits remain a separate review task; passing a size ceiling does not prove
that the older architectural debt has been resolved.

**Additional original evidence:**

- 🟡 **Two ceilings were raised instead of splitting — the debt.** *(~1 d for
  ImageWriter, less for Memory; post-1.0, ruling
  [R4](../decisions/project-scope.md#standing-rulings))* <a id="file-size-debt"></a>The file-size
  ratchet had been failing on `main` since **2026-08-29**, in 15 s, before the
  Linux job compiled anything — so it was also hiding that job's build and its
  GLES tier behind a red X nobody could see past. On 2026-08-31 the two
  ceilings were raised to their exact current sizes to get the gate green
  again. That is what `tools/file_size_budget.txt` is for (its script's header
  says editing it is precisely the moment someone should be asked), and it is
  the lesser half of the answer.

  - **`src/ImageWriter.cpp` 2152 → 2501 (+349)** — the printer work put the
    ImageWriter head, the paper tray, PDF export and the PostScript /
    screen-dump seams in one translation unit. Those are already separate
    concerns with clean edges; this one wants splitting, and the project's own
    rule ("new code for an existing window group belongs in its own
    translation unit") says so.
  - **`src/Memory.cpp` 2402 → 2442 (+40)** — smaller, and not one change.
    Exactly **one** line of it is the foreign-bus dispatch that lets a
    coprocessor card run the 6502 over its own map
    (`docs/PERFORMANCE.md` § 9); the other 39 predate it.

  Both are recorded at their **exact** current size, so the ratchet still
  fails on the next line either of them gains — the debt cannot quietly grow.

<a id="arch-009"></a>

## ARCH-009 — Remaining ImGui panel concurrency coverage

**Priority:** 🟠 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): only `Uthernet_ImGui` runs under the replug harness.

**Acceptance criterion (after revalidation):** Run headless ImGui frames during card replug and coordinator capture, detecting recursive-lock deadlocks without timeouts.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2228). Original dates and estimates are retained below.

The historical recursive-lock report motivated
`tools/check_coordinator_locks.sh` and the runtime coverage below.
`stateMutex` remains non-recursive: coordinator captures must not be nested
inside an existing machine lock. The concurrency harness is implemented;
extending it to other direct panel routes is the remaining work.

**Implementation 2026-10-17:**
`tests/frontend_device_panel_concurrency_test.cpp` now renders real
`Uthernet_ImGui` frames from `DevicePanelCoordinator` snapshots while a second
thread swaps Uthernet I/II cards. It covers both a snapshot rendered after its
card is destroyed and captures racing with slot changes. Five local CTest
runs passed. This covers the Ethernet panel seam; direct `MainWindow` panel
routes and other card panels remain outside this harness.

**CI evidence 2026-10-17:** `frontend_device_panel_concurrency` passed in
[Linux run 39063373838](https://github.com/habib256/pom2/actions/runs/39063373838).
Other direct `MainWindow` panels remain outside the harness and should be
considered separately from the release Ethernet gate.

**Current interaction coverage 2026-10-19:** `session_file_picker` drives
ImGui session save/load, cancellation, overwrite and error paths;
`drive_rack_interaction` drives media-panel interactions. These complement
the Ethernet concurrency harness, rather than proving all panels race-free.
The blanket claim that no test drives ImGui is obsolete.

<a id="arch-003"></a>

## ARCH-003 — ThreadSanitizer: the GUI half is still open.

**Priority:** 🟡 · **State:** Confirmed open ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): no GUI test in the nightly TSan filter.

**Acceptance criterion (after revalidation):** Drive ImGui panels, screenshots, slot replug and rewind under TSan without races or deadlocks.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-883). Original dates and estimates are retained below.

- 🟡 **ThreadSanitizer: the GUI half is still open.** The controller half is
  done and clean (2026-08-17) — a harness drove the real thread shape without a
  GUI: CPU worker, UI transport verbs, an AI-server thread doing `lockState()`
  reads plus snapshot capture/restore and key injection, the live miniaudio
  callback, and a Mockingboard fed by a guest loop so the emuCycles AY queue is
  exercised. Zero races. **Caveat worth keeping**: TSan instruments the
  interpreter's hot loop, so the CPU manages ~400-1 400 emulated cycles/s — the
  *lock protocol* is covered thoroughly, *emulated execution* thinly. What
  remains is ImGui panels, `demodMutex`, slot re-plug under load — which is the
  same thing G5-10 unblocks.

**Additional original evidence:**

- 🟡 **ThreadSanitizer — the GUI half** *(2026-08-02 bug-hunt follow-up)*.
  Gated on [G5-10](release-1.0.md#g5), which is what makes
  a GUI harness possible at all. The controller half is done and clean; the
  analysis is kept in
  [Open, and known to be open](../archive/todo-2026-10-05.md#open-and-known-to-be-open). That sweep's ASan+UBSan build (156 test binaries, ~24 000
  hostile-input cases, ~6 M random instructions) returned **zero**
  diagnostics, yet code reading found a UI deadlock, two use-after-frees and
  three unlocked cross-thread reads in the same tree. ASan cannot see data
  races and the headless tests cannot reach the GUI, which is exactly where
  the defects were. Needs a TSan build driving the GUI with the AI server
  polling `/screen.ppm`, slot reconfiguration, and rewind under load. Would
  also retire the two findings that could not be pinned (`saveScreenshot`'s
  `demodMutex` ordering, and the threaded half of `disk_path_snapshot`).
  - **The controller half is done and clean** (2026-08-17, bug hunt 8): a TSan
    harness drove the real thread shape without a GUI — CPU worker, a UI thread
    running the transport verbs (rewind scrub/seek/resume, cassette, 3.5"
    mount/eject, speed, mode toggles, a `lockState()` read per frame), an
    AI-server thread doing `lockState()` reads + snapshot capture/restore + key
    injection through `kbMutex`, the live miniaudio callback, and a Mockingboard
    in slot 4 fed by a guest loop so the emuCycles AY queue (the one real
    CPU↔audio producer/consumer) is exercised. Zero races. **Caveat worth
    keeping**: TSan instruments every load/store in the interpreter's hot loop,
    so the CPU manages only ~400-1 400 emulated cycles/s — the *lock protocol*
    is covered thoroughly, *emulated execution* thinly. What remains is the GUI
    half: ImGui panels, `demodMutex`, slot re-plug under load.

<a id="arch-008"></a>

## ARCH-008 — `hgrpaint/` has no headless harness

**Priority:** 🟡 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): the sprite seam landed (`HgrSpriteEditorTestAccess`, `135051ff`); the paint-editor seam waits for a report (editors are frozen).

**Acceptance criterion (after revalidation):** Drive editor tools through a headless host stub; share an additive test seam with the sprite editor and POM1.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2210). Original dates and estimates are retained below.

- 🟡 **`hgrpaint/` has no headless harness** *(2026-08-14)* — the editor's
  state (mode flags, shadow buffer, tools) is private and only reachable
  through `render()`, i.e. through an ImGui frame, so nothing in `ctest`
  can exercise it: `dhgr_paint_model` pins the free functions in
  `HgrPaintModel.h`, not `HgrPaintEditor`. That is how three
  out-of-bounds accesses on the DLGR shadow survived from the DLGR page's
  arrival (2026-07-12) to 2026-08-14 with a green suite. Cheapest fix: a
  test-only seam (a friend fixture, or a small `EditorTestAccess` struct)
  driving the tools against a stub `IHgrPaintHost` — bearing in mind
  `hgrpaint/` is shared verbatim with POM1, so the seam must be additive.
  *~1 d.*
  - **`hgrsprite/` had the same shape and cost the same kind of bug**
    (2026-08-17, bug hunt 8): no test at all, and its ca65 DHGR export read
    32 bytes past the pair buffer at the UI maxima. The byte layer is now
    pinned by `hgr_sprite_blit` — including the two helpers the export's
    clipping moved into (`dhgrExportRowBytes`, `extractDhgrPlanes`) — but
    `HgrSpriteEditor` itself is still only reachable through an ImGui frame,
    exactly like `HgrPaintEditor`. One seam would serve both.

<a id="arch-010"></a>

## ARCH-010 — Scattered config

**Priority:** 🟡 · **State:** Confirmed open ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)); post-1.0.

**Acceptance criterion (after revalidation):** Resolve env, CLI, settings and defaults through one documented precedence chain with equivalent behavior on each entry path.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2245). Original dates and estimates are retained below.

- 🟡 **Scattered config** — ruling [R3](../decisions/project-scope.md#standing-rulings): one `Config`
  (env → CLI → Settings → defaults), env vars listed in `--help`. *1 d.*
  Post-1.0.

<a id="arch-011"></a>

## ARCH-011 — `stateMutex` shared CPU+UI

**Priority:** 🟡 · **State:** Needs decision ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): the lock is held across Apply, but the CPU worker is stopped there — close as by-design, or wait for ARCH-003.

**Acceptance criterion (after revalidation):** Demonstrate reduced CPU/UI lock contention under GUI TSan while preserving slot-rebuild and audio synchronization.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2248). Original dates and estimates are retained below.

- 🟡 **`stateMutex` shared CPU+UI** — `MainWindow_Slots` takes this lock
  during plug/unplug, an audio-jitter risk. Partition long-term, and only
  after the GUI TSan half above.

**Revalidation note 2026-10-06:** the plug path has since moved to
`MainWindow_SlotConfig.cpp` (`MainWindow::plugSlotsFromSettings`, which takes a
`const pom2::StateAccess&`); the lock is still held across plug/unplug.

<a id="arch-007"></a>

## ARCH-007 — Z80/SoftCard cleanup backlog

**Priority:** 🧊 · **State:** Frozen — [triage 2026-10-28](../audits/backlog-triage-2026-10-28.md). Closed as won't-do: `project-scope.md` classes this list as quality, not correctness, "do not schedule it". Reopen only when someone edits those files for another reason.

**Acceptance criterion (after revalidation):** Choose one cleanup seam at a time and preserve Z80 differential, CP/M boot and snapshot behavior.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2169). Original dates and estimates are retained below.

- 🟢 **Z80/SoftCard cleanup backlog** (2026-07-12 bug-hunt survivors — quality,
  not correctness): SoftCardZ80 SFZ2 blob → `pom2::byteio` putU16/Reader like
  every other card (3 hand-synced layout copies today);
  `softcard_cpm_boot_test` → `pom2::findResource` + `Apple2Display::
  textRowAddress` instead of private copies (6th in-repo transcription of the
  text-row interleave); Z80.cpp decoder dedup: rp-selector switch pasted 8×
  (readRP/writeRP helpers), JR cc's inline condition test vs `ccTest`,
  `memEA` body re-inlined twice for special timings (chargeless
  `indexedEA()` split); drop the per-instruction `mem_` null tests in the
  dmaRun hot loop (guard once at entry). *Done 2026-10-24:* `xlate` is a
  16-entry page table, checked against MAME's chain on all 64 K addresses
  in `softcard_toggle`. Boot test could also pump a public
  controller slice hook instead of re-implementing arbitration.

<a id="arch-012"></a>

## ARCH-012 — Inconsistent `pom2::` namespace

**Priority:** 🟢 · **State:** Confirmed open ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): 375 of 484 `src/` files and 23 of 424 tests use `namespace pom2`; post-1.0.

**Acceptance criterion (after revalidation):** Migrate the selected namespace scope without changing exported API or test behavior.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2254). Original dates and estimates are retained below.

- 🟢 **Inconsistent `pom2::` namespace** — 255/336 top-level files (was
  163/233 when this was written); `tests/` does not use it. Mechanical
  migration, ruling [R3](../decisions/project-scope.md#standing-rulings). Post-1.0.

<a id="arch-013"></a>

## ARCH-013 — Legacy M6502 style

**Priority:** 🟢 · **State:** Confirmed open ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)); a comment/declaration-only pass, verified by unchanged Harte/Klaus and `display_golden_hash`.

**Acceptance criterion (after revalidation):** Apply targeted style modernization with no CPU timing or semantic changes.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2257). Original dates and estimates are retained below.

- 🟢 **Legacy M6502 style** — FR/EN comments, C-style casts,
  `void(void)`. Targeted `clang-format` + `clang-tidy modernize-*`.

<a id="arch-014"></a>

## ARCH-014 — No getter to assert every clock-bearing device was retuned

**Priority:** 🟢 · **State:** Ready.

**Acceptance criterion:** `device_standard_clock` enumerates every device `setVideoStandard` retunes — the speaker, the cassette and the floppy-sound device included — and fails when one is skipped.

**Evidence:** [original report](../archive/todo-2026-10-05.md) G5-3, "Not done": the speaker, cassette and floppy-sound retunes have no getter to assert on. Recovered from the archive 2026-10-07; it had no home after the migration.

<a id="arch-015"></a>

## ARCH-015 — The window's slot hooks are not covered by the slot-config tests

**Priority:** 🟢 · **State:** Ready.

**Acceptance criterion:** `MainWindow::plugSlotsFromSettings` (still GUI-side) is either reached by a headless test or its policy moved into `SlotConfigurationCoordinator`, where `slot_configuration_coordinator` already pins it.

**Evidence:** [original report](../archive/todo-2026-10-05.md) G5-6, "Not covered". The 2026-10-06 Videx-on-//e bug lived exactly in this seam (the picker refused, the plug path did not). Recovered from the archive 2026-10-07.
