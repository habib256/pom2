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
| [UI-002](#ui-002) | 🟠 | To verify | No native file picker. |
| [UI-001](#ui-001) | 🟡 | To verify | Step-over is a temp breakpoint at `PC + 3`, which never fires for the Apple II's dominant call convention |
| [UI-003](#ui-003) | 🟡 | To verify | Bug hunt #16's debugger residue |
| [UI-004](#ui-004) | 🟢 | To verify | Deeper guided tutorials |
| [UI-006](#ui-006) | 🟢 | To verify | Airier default layout |
| [UI-007](#ui-007) | 🟢 | To verify — premise false on 2026-10-06 | `isDuplicate` flags cffa/smartport35 duplicates |
| [UI-005](#ui-005) | 🧊 | Frozen | MicroM8-style 3D voxel view ("Voxel Cube") |
| [UI-008](#ui-008) | 🧊 | Frozen | On-screen touchscreen / virtual joystick |

<a id="ui-002"></a>

## UI-002 — No native file picker.

**Priority:** 🟠 · **State:** To verify.

**Acceptance criterion (after revalidation):** Use the native open/save dialog and verify cancellation, chosen paths and fallback behavior on supported hosts.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-905). Original dates and estimates are retained below.

- 🟠 **No native file picker.** Every "open"/"save as" in POM2 is an ImGui
  browser over the working tree. Reviewed 2026-09-07 and **declined for now**:
  no host-dialog helper exists anywhere in the tree, so this is a new
  dependency (portal/AppKit/Win32 per platform) rather than a fix. Worth doing,
  but as a scoped feature with its own platform matrix — not as part of a bug
  hunt. `Pom2HgrPaintHost` therefore still does not override `pickFilePath`,
  and there is no sprite catalogue to reach because POM2 ships no sprites.

<a id="ui-001"></a>

## UI-001 — Step-over is a temp breakpoint at `PC + 3`, which never fires for the Apple II's dominant call convention

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Step over ProDOS/SmartPort inline parameters and nested calls without leaving the machine running indefinitely.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-835). Original dates and estimates are retained below.

- 🟡 **Step-over is a temp breakpoint at `PC + 3`, which never fires for the
  Apple II's dominant call convention** (bug hunt #4). `JSR $BF00 / DB cmd /
  DW params` returns to `pc + 6`, and the SmartPort `$Cn00` dispatch is the
  same shape, so the transient lands on a parameter byte no opcode fetch ever
  reads — and Step Over has already set the machine Running, so it runs free
  until the user presses Stop. Recursion has the mirror problem. The honest fix
  needs the stack pointer inside `M6502DebugHook::onInstruction`, which is the
  per-instruction path `docs/PERFORMANCE.md` §§ 8.2/8.5 guards; the limit is
  documented at the site instead.

<a id="ui-003"></a>

## UI-003 — Bug hunt #16's debugger residue

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Exercise endpoint method/range validation, flat-bus watches and step-over cleanup with explicit expected responses.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1978). Original dates and estimates are retained below.

- 🟡 **Bug hunt #16's debugger residue** *(2026-09-09; read, not fixed)*:
  `POST /screen.ppm` is answered (no method check); `POST /cpu` masks an
  out-of-range `pc`/`p` silently; write watches are short-circuited under
  `flatBus_` while read watches are not; a watchpoint hit during a Step
  reports `pc=$0000`; a step-over transient survives an unrelated stop
  (MAME drops temporaries); `/status`'s `disks` is Disk-II-only. *2 h.*

<a id="ui-004"></a>

## UI-004 — Deeper guided tutorials

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Provide and walk through the chosen tutorials from a fresh profile.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2065). Original dates and estimates are retained below.

- 🟢 **Deeper guided tutorials** — the Welcome / no-ROM panel covers a first
  launch without a ROM; step-by-step tutorials do not exist.

<a id="ui-006"></a>

## UI-006 — Airier default layout

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Check the proposed default layout at supported window sizes while preserving user-saved layouts.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2101). Original dates and estimates are retained below.

- 🟢 **Airier default layout** — ImGui Docking or
  `SetNextWindowPos` adaptive cascade.

<a id="ui-007"></a>

## UI-007 — `isDuplicate` flags cffa/smartport35 duplicates

**Priority:** 🟢 · **State:** To verify — premise false on 2026-10-06.

**Revalidation 2026-10-06 (code reading):** `isDuplicate` in
`src/MainWindow_Slots.cpp:216-228` returns false for any key
`SlotConfigurationCoordinator::isMultiInstance` accepts, and that predicate
lists `diskii`, `cffa`, `smartport35` and `liron`
(`src/SlotConfigurationCoordinator.cpp:119-125`, since `70cc8e9`, 2026-08-26),
so two CFFA or two `smartport35` cards are no longer flagged. Confirm in the
Slot Config window, then remove this entry.

**Acceptance criterion (after revalidation):** Verify duplicate detection against each card's actual multiple-instance policy; archive claims contradicted by current support.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2103). Original dates and estimates are retained below.

- 🟢 **`isDuplicate` flags cffa/smartport35 duplicates** in the Slot
  Config assignment column — cosmetic.

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
