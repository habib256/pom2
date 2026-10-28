# Backlog — UI and debugger

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
| [UI-002](#ui-002) | 🟡 | Partial: panels still use typed paths | Native picker integration and platform verification |
| [UI-003](#ui-003) | 🟡 | Partial: `/screen` method check | HTTP screen-method and status-inventory review |
| [UI-004](#ui-004) | 🧊 | Frozen — triage 2026-10-28 | Deeper guided tutorials |
| [UI-006](#ui-006) | 🧊 | Frozen — triage 2026-10-28 | Airier default layout |
| [UI-005](#ui-005) | 🧊 | Frozen | MicroM8-style 3D voxel view ("Voxel Cube") |
| [UI-008](#ui-008) | 🧊 | Frozen | On-screen touchscreen / virtual joystick |
| [UI-009](#ui-009) | 🟢 | Needs decision: cover art vs captured screen | Disk Library miniatures |
| [UI-010](#ui-010) | 🟢 | Partial: Windows + macOS rank | OS file associations for disk images |

<a id="ui-002"></a>

## UI-002 — Remaining native file picker integration and platform checks

**Priority:** 🟡 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): cassette, SmartPort, Disk/HDV/3.5 and printer panels still take typed paths; Windows unchecked.

**Acceptance criterion (after revalidation):** Use the native open/save dialog and verify cancellation, chosen paths and fallback behavior on supported hosts.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-905). Original dates and estimates are retained below.

`NativeFileDialog` provides AppKit, Win32 and external zenity/kdialog
backends, with ImGui fallback when unavailable. Integrated into drive
insertion, HGR/DHGR paint/sprite open/save and debugger-symbol selection.
Desktop session open/save also uses the native chooser, with overwrite
confirmation and a manual-path fallback when the backend is unavailable.
Cancellation is distinguished from backend unavailability.

Remaining: extend other standalone file/export panels and obtain interactive
cancel/open/save evidence on every OS. Windows is not validated by a macOS build.

UI-001 (inline-argument step-over) was corrected on 2026-10-17; see CHANGELOG
and the expanded `debugger` regression suite.

<a id="ui-003"></a>

## UI-003 — Bug hunt #16's debugger residue

**Priority:** 🟡 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): `/cpu` and debugger items fixed; `/screen` still answers POST.

**Acceptance criterion (after revalidation):** Define and verify the screenshot endpoint's method contract and the intended media-status inventory; retain existing CPU-validation and debugger regressions.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1978). Original dates and estimates are retained below.

- 🟡 **Bug hunt #16's debugger residue** *(2026-09-09; read, not fixed)*:
  `POST /screen.ppm` is answered (no method check); `POST /cpu` masks an
  out-of-range `pc`/`p` silently; write watches are short-circuited under
  `flatBus_` while read watches are not; a watchpoint hit during a Step
  reports `pc=$0000`; a step-over transient survives an unrelated stop
  (MAME drops temporaries); `/status`'s `disks` is Disk-II-only. *2 h.*

**Reconciliation 2026-10-19:** The paragraph above is a historical report,
not a current defect list. `Debugger` now records the actual instruction PC
on an explicit step, drops transients on unrelated stops, and applies flat-bus
write/high-address read watches. Bank-qualified stops/profiling and `.p2map`
source correspondence are also implemented; see the
[debugger guide](../debugger-banks.md).

**HTTP source check 2026-10-20:** `handleCpuSet` validates every supplied
register before changing state: `pc` is 0–65535, the other five registers
0–255, and malformed/out-of-range values receive 400. The old masking claim
is superseded. `handleScreen` still does not check the method; deciding and
verifying its method contract remains. `/status.disks` describes the attached
Disk II controller, while `block_storage` and `floppy_storage` separately
report persistence state. It is inaccurate to call all of `/status`
Disk-II-only; a complete mounted-media inventory remains a separate scope
choice. No new endpoint execution is claimed by this source reconciliation.

<a id="ui-004"></a>

## UI-004 — Deeper guided tutorials

**Priority:** 🧊 · **State:** Frozen — [triage 2026-10-28](../audits/backlog-triage-2026-10-28.md). Reopen when one or two tutorials are named with an acceptance walk-through.

**Acceptance criterion (after revalidation):** Provide and walk through the chosen tutorials from a fresh profile.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2065). Original dates and estimates are retained below.

- 🟢 **Deeper guided tutorials** — the Welcome / no-ROM panel covers a first
  launch without a ROM; step-by-step tutorials do not exist.

<a id="ui-006"></a>

## UI-006 — Airier default layout

**Priority:** 🧊 · **State:** Frozen — [triage 2026-10-28](../audits/backlog-triage-2026-10-28.md). "Airier" has no measurable criterion. Reopen with a concrete window size and the panel that misbehaves there.

**Acceptance criterion (after revalidation):** Check the proposed default layout at supported window sizes while preserving user-saved layouts.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2101). Original dates and estimates are retained below.

ImGui docking and Emulation, Debug, Audio and Reset layout presets are
implemented by `MainWindow::applyDockLayout`. The remaining proposal is to
review their default spacing and behavior at supported window sizes while
preserving saved layouts. It is not a request to introduce docking.

<a id="ui-005"></a>

## UI-005 — MicroM8-style 3D voxel view ("Voxel Cube")

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2082). Original dates and estimates are retained below.

- 🧊 **MicroM8-style 3D voxel view ("Voxel Cube")** — screen **stood up**
  (monitor, XY plane) as a 4:3 slab of **uniform-depth** cubes + per-color "pop"
  relief, orbital camera. NB: the initial luminance extrusion gave flat
  stalactites — fixed after scraping MicroM8 (cf. `CHANGELOG.md` + `DEV.md` § 3D
  voxel view). **Phases 0→3 done** (2026-05-31, `CHANGELOG.md`): `Mat4.h`
  (Vec3+Mat4+OrbitCamera, pinned `voxel3d_math`), `Voxel3DRenderer` (instanced
  cubes, FBO+depth, per-vertex color texture-fetch, derivative shading,
  **anti-moiré supersampling** + contiguous `cubeFill=1`), **native resolution**
  (1 voxel/pixel, 280|560×192), tap **before** `CrtEffectStack` (independent of
  CRT effects), *(P2)* left-drag orbit + middle-button **pan** + wheel zoom,
  *(P3)* View ▸ "3D voxel settings…" panel (depth/pop/fill/AA/ambient/mono/
  per-colour, persisted `voxel_*`), *(P4)* **WASM perf guard** (`ss≤2`+FBO≤2048²+
  `gridW≤280` under Emscripten), *(bonus)* **Mono mode** + **depth by color
  index** (snap lo-res palette `kVoxelPalette`). **WASM build OK** (+ browser
  wheel fix: `emscripten_set_wheel_callback` → `io.MouseWheel`, cf. `main.cpp`).
  **Remaining**: *(P5, deferred on request)* rewind tie-in "freeze + orbit a
  rewound frame" — already works for free (the view samples the live framebuffer
  that rewind restore updates), so doc + polish rather than plumbing; *(option)*
  alternative heightfield-mesh mode. Detail → `DEV.md` § 3D voxel view. *P5≈0.5 d.*

<a id="ui-008"></a>

## UI-008 — On-screen touchscreen / virtual joystick

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2105). Original dates and estimates are retained below.

- 🧊 **On-screen touchscreen / virtual joystick** — ImGui virtual
  joystick for mobile WASM builds (separate from raw touch routing). Two
  thumb-sticks + Open/Solid Apple buttons. Inspired by microM8 / A2TS.
  *2 d.*

<a id="ui-009"></a>

## UI-009 — Disk Library miniatures

**Priority:** 🟢 · **State:** Needs decision ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): cover art or captured screen. The prerequisite (search/sort, "All media") exists. User-requested order: this, then UI-010.

**Acceptance criterion:** Decide whether a thumbnail is user-supplied cover art
or a captured guest screen, then display it without loading every disk or
blocking the library's scan. Missing art must leave a readable text row.

<a id="ui-010"></a>

## UI-010 — OS file associations for disk images

**Priority:** 🟢 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): `.moof` declared; macOS uses `LSHandlerRank` Owner (should be Alternate for `.po`/`.dsk`); Windows has no association.

**Acceptance criterion:** Register supported image extensions in each desktop
package, open a disk from the host file manager into the correct drive, and
verify behavior on macOS, Windows and Linux without overriding another app's
default association.

**Code reconciliation 2026-10-19:** `packaging/macos/Info.plist.in` declares
`.dsk`, `.do`, `.po`, `.nib`, `.woz`, `.d13`, `.2mg` and `.hdv`.
`HostOpenFiles_mac.mm` receives Finder/Dock open events and `main.cpp` drains
the queue into the normal file-drop path. Linux installs `POM2.desktop`
(`Exec=POM2 %f`) and `pom2-disk.xml`; `.po` has a lower MIME weight to avoid
winning against gettext files. Linux/Windows positional disk arguments are
handled by the CLI. This is implemented plumbing, not proof of an installed
package's association behavior. Remaining: Windows registration, extension
coverage (including `.moof`) and clean-host open/default-association checks.
