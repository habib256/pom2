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
| [ARCH-004](#arch-004) | 🟠 | To verify | Six file-size ceilings were raised by the two hunt rounds, and the owed splits are still owed |
| [ARCH-009](#arch-009) | 🟠 | To verify | No test drives the ImGui panels, so a UI-thread deadlock fails nothing |
| [ARCH-003](#arch-003) | 🟡 | To verify | ThreadSanitizer: the GUI half is still open. |
| [ARCH-005](#arch-005) | 🟡 | To verify | Bug hunt #16's tooling residue |
| [ARCH-006](#arch-006) | 🟡 | To verify | Bug hunt #15's host residue |
| [ARCH-008](#arch-008) | 🟡 | To verify | `hgrpaint/` has no headless harness |
| [ARCH-010](#arch-010) | 🟡 | To verify | Scattered config |
| [ARCH-011](#arch-011) | 🟡 | To verify | `stateMutex` shared CPU+UI |
| [ARCH-001](#arch-001) | 🟢 | To verify | Blocking work under `stateMutex` — what is LEFT. |
| [ARCH-002](#arch-002) | 🟢 | To verify — premise partly false on 2026-10-06 | `persistSession()` reaches `controller->memory()` unlocked, and it is safe by construction — but the invariant is unenforced. |
| [ARCH-007](#arch-007) | 🟢 | To verify | Z80/SoftCard cleanup backlog |
| [ARCH-012](#arch-012) | 🟢 | To verify | Inconsistent `pom2::` namespace |
| [ARCH-013](#arch-013) | 🟢 | To verify | Legacy M6502 style |

<a id="arch-004"></a>

## ARCH-004 — Six file-size ceilings were raised by the two hunt rounds, and the owed splits are still owed

**Priority:** 🟠 · **State:** To verify.

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
so `tools/check_file_sizes.sh` fails on HEAD and so did the 2026-10-06 push
CI's Linux job.

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

## ARCH-009 — No test drives the ImGui panels, so a UI-thread deadlock fails nothing

**Priority:** 🟠 · **State:** To verify.

**Acceptance criterion (after revalidation):** Run headless ImGui frames during card replug and coordinator capture, detecting recursive-lock deadlocks without timeouts.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2228). Original dates and estimates are retained below.

- 🟠 **No test drives the ImGui panels, so a UI-thread deadlock fails nothing**
  *(found the hard way 2026-08-27; gated on
  [G5-10](release-1.0.md#g5))* — a coordinator capture placed inside an
  existing `lock_guard(stateMutex())` scope would have hung the UI thread and
  the emulator together, while the full suite stayed green, because nothing
  drives the panels. `stateMutex` is non-recursive and every coordinator
  capture takes it itself.
  **Mitigation in the tree**: `tools/check_coordinator_locks.sh`, run after
  touching any coordinator call site — falsifiable against `44b715f`.
  **Still open**: `tests/frontend_device_panel_concurrency`, a headless ImGui
  frame driven while cards are replugged. That is the only thing that closes
  the class rather than scanning for it. *1 d.*

<a id="arch-003"></a>

## ARCH-003 — ThreadSanitizer: the GUI half is still open.

**Priority:** 🟡 · **State:** To verify.

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

<a id="arch-005"></a>

## ARCH-005 — Bug hunt #16's tooling residue

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Check source-list uniqueness, monotonic coverage updates and file-size guard extension coverage with failing mutation controls.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1968). Original dates and estimates are retained below.

- 🟡 **Bug hunt #16's tooling residue** *(2026-09-09; read, not fixed)*:
  `POM2_CORE_SOURCES` lists three files twice; `coverage.sh --update`
  lowers the floor unconditionally although its header says the floor may
  only rise; `check_file_sizes.sh` is blind to `.hpp`/`.cc`/`.inl` and to
  anything outside `src/` + `tests/`. *1 h.*

**Revalidation note 2026-10-06 (code reading):** all three still hold —
`POM2_CORE_SOURCES` lists `W5100HostSockets.cpp`, `W5100NameResolver.cpp`
and `IIcExternalSmartPort.cpp` twice; `coverage.sh --update` writes
measured − 0.5 whatever the old floor was (`tools/coverage.sh:398-412`);
`check_file_sizes.sh` scans `src tests` for `*.cpp`/`*.h` only (line 53).

<a id="arch-006"></a>

## ARCH-006 — Bug hunt #15's host residue

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Test settings-list round trips, media search-path consistency and catalog key uniqueness.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2009). Original dates and estimates are retained below.

- 🟡 **Bug hunt #15's host residue** *(2026-09-09; read, not fixed)*:
  `SettingsList` packs with `0x1F` and escapes nothing, so a path holding
  that byte splits into two `library_recents` entries (any escape scheme
  added now mis-reads existing values; cost is one dead recent entry);
  `restoreMediaFromSettings` probes `../` and `../../` for SmartPort and
  generic bays but not for the Disk II, HDV and CFFA; `PanelCatalog` has no
  uniqueness check on `command`/`settingsKey` (none collide today).

<a id="arch-008"></a>

## ARCH-008 — `hgrpaint/` has no headless harness

**Priority:** 🟡 · **State:** To verify.

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

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Resolve env, CLI, settings and defaults through one documented precedence chain with equivalent behavior on each entry path.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2245). Original dates and estimates are retained below.

- 🟡 **Scattered config** — ruling [R3](../decisions/project-scope.md#standing-rulings): one `Config`
  (env → CLI → Settings → defaults), env vars listed in `--help`. *1 d.*
  Post-1.0.

<a id="arch-011"></a>

## ARCH-011 — `stateMutex` shared CPU+UI

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Demonstrate reduced CPU/UI lock contention under GUI TSan while preserving slot-rebuild and audio synchronization.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2248). Original dates and estimates are retained below.

- 🟡 **`stateMutex` shared CPU+UI** — `MainWindow_Slots` takes this lock
  during plug/unplug, an audio-jitter risk. Partition long-term, and only
  after the GUI TSan half above.

**Revalidation note 2026-10-06:** the plug path has since moved to
`MainWindow_SlotConfig.cpp` (`MainWindow::plugSlotsFromSettings`, which takes a
`const pom2::StateAccess&`); the lock is still held across plug/unplug.

<a id="arch-001"></a>

## ARCH-001 — Blocking work under `stateMutex` — what is LEFT.

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Measure the remaining lock-held operations before changing them; show that any approved extraction preserves lock ordering and media state.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-851). Original dates and estimates are retained below.

- 🟢 **Blocking work under `stateMutex` — what is LEFT.** The 2026-08-22 audit
  found ~20 sites and fixed the structural cause (`MediaMount.h`: read + decode
  unlocked, swap under the lock). What remains was examined on 2026-08-23 and
  left on purpose:
  * `slotBus().clear()` on a profile switch is not a machine freeze —
    `applyProfile` has already stopped the CPU worker, so only the UI blocks,
    during a modal full cold reset.
  * ~~The FujiNet **Stop / Drop-peer** buttons genuinely need the lock~~ —
    **done 2026-09-06** (bug hunt, `958d86e`): the exclusion moved onto the
    link's own `callMtx_` as described here, `NetworkCoordinator` resolves the
    card under `lockState()` and drives the link off it, `enumerateDevices`
    re-reads its stop flag per unit. `~FujiNetCard` no longer waits the 2 s
    helper grace under `SlotBus::plug` either (`ChildProcess::stopDetached`).
  * ✅ **Closed 2026-09-07**: `ejectAllMedia` was the last inline holdout and
    now takes the same three-phase shape as every single-medium eject
    (capture locked → commit unlocked → re-resolve and retire, or put the
    medium back on failure). `flushAll`'s Liron bays and the host-folder mount
    followed. The firmware 3.5" eject waits on the `WriteBackQueue` sink
    (`Sony35Drive::ejectPending_`) instead of ejecting on the spot, so a failed
    commit leaves the disk loaded and dirty rather than gone.
  * Deliberate and staying: the profile-switch remount in `MainWindow_Slots.cpp`
    (atomicity against the AI server outranks latency, and the worker is stopped)
    and the outgoing medium's write-back inside `installDisk` (swapping before
    knowing the old medium could be written loses the user's changes).
  * Bounded and documented: the Uthernet II guest DNS wait (`kDnsWaitMs` = 120 ms).

**Revalidation note 2026-10-06:** the profile-switch remount now lives in
`pom2::switchProfile` (`src/ProfileSwitch.cpp`, the documented exception at
line 108), not `MainWindow_Slots.cpp`.

<a id="arch-002"></a>

## ARCH-002 — `persistSession()` reaches `controller->memory()` unlocked, and it is safe by construction — but the invariant is unenforced.

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Document and enforce the stopped-worker or single-thread condition at every persistSession caller.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-876). Original dates and estimates are retained below.

- 🟢 **`persistSession()` reaches `controller->memory()` unlocked, and it is
  safe by construction — but the invariant is unenforced.** `~MainWindow` calls
  `controller->stop()` first, and the WASM heartbeat runs on the CPU-stepping
  thread because the worker does not exist under Emscripten. Nothing in the file
  says so, and nothing stops a third caller from adding a mid-session call on
  the desktop, where the sibling `MainWindow::flushSlotMedia` does take the lock
  around the identical `flushAll`. Worth a comment at minimum.

**Revalidation 2026-10-06 — premise partly false:** `persistSession`
(`src/MainWindow_Session.cpp:69`) now flushes through `flushSlotMedia` and takes
`stateMutex` around every media/storage capture (lines 112-116 and the 3.5"
block after it). The one unlocked `controller->memory()` left is the FujiNet
lookup (`src/MainWindow_Session.cpp:219-225`), a UI-thread SlotBus topology
read with a comment saying the destructor runs after `controller->stop()` —
one of the unlocked reads CLAUDE.md allows. What remains is the *enforcement*
half (nothing asserts the stopped-worker condition).

<a id="arch-007"></a>

## ARCH-007 — Z80/SoftCard cleanup backlog

**Priority:** 🟢 · **State:** To verify.

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
  `indexedEA()` split); `xlate` 5-compare chain → 16-entry per-4K-page
  offset LUT; drop the per-instruction `mem_` null tests in the dmaRun hot
  loop (guard once at entry). Boot test could also pump a public
  controller slice hook instead of re-implementing arbitration.

<a id="arch-012"></a>

## ARCH-012 — Inconsistent `pom2::` namespace

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Migrate the selected namespace scope without changing exported API or test behavior.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2254). Original dates and estimates are retained below.

- 🟢 **Inconsistent `pom2::` namespace** — 255/336 top-level files (was
  163/233 when this was written); `tests/` does not use it. Mechanical
  migration, ruling [R3](../decisions/project-scope.md#standing-rulings). Post-1.0.

<a id="arch-013"></a>

## ARCH-013 — Legacy M6502 style

**Priority:** 🟢 · **State:** To verify.

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

