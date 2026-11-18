# Backlog — WebAssembly

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
| [WASM-002](#wasm-002) | 🧊 | Frozen | Mobile touch input |
| [WASM-003](#wasm-003) | 🧊 | Frozen | Audio worklet tuning |
| [WASM-004](#wasm-004) | 🟢 | Ready | Shareable links: per-drive URL parameters and a copy-link button |
| [WASM-005](#wasm-005) | 🟢 | Ready | Embed mode (?embed=1 + postMessage events) |

<a id="wasm-001"></a>

## WASM-001 — Local disk selection and drop mounting (implemented)

**State:** Implemented; locally verified on 2026-10-20. Anchor retained for
existing links; this is not pending drop-zone work.

The [original report](../archive/todo-2026-10-05.md#source-2121) and later
claims that desktop file drops were unavailable missed the browser support
provided by GLFW/Emscripten. `main.cpp` registers `glfwSetDropCallback` in the
WebAssembly build; Emscripten installs the canvas DOM handlers and loads
local files into MEMFS before invoking POM2's existing drop/mount route.
A local headless Chrome check dispatched file-backed drag events, confirmed
that a canvas drop mounted and booted drive 1, then dropped another image onto
empty drive 2 while paused and confirmed insertion without rebooting.

The explicit file selector was added separately on 2026-10-20: File > Import
disk and boot and empty-drive Insert actions. Selector uploads have unique
MEMFS paths and a 64 MiB per-file bound. GLFW drops use filename-based paths
under `/.glfw_dropped_files/`; they do not use that selector's bound or
same-name isolation. Both paths retain normal loader checks and can preserve
supported media in sessions. See the
[user-facing import contract](../USER-GUIDE.md#browser-disk-import).

The selector's native/WebAssembly builds, DOM/FS tests and Chrome smoke also
passed locally, including same-name drive-2 import and both media in a
session archive. The same smoke has since passed in CI (2026-10-23); see
[RELEASE-005](release.md#release-005).

<a id="wasm-002"></a>

## WASM-002 — Mobile touch input

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2124). Original dates and estimates are retained below.

- 🧊 **Mobile touch input** — GLFW3 under Emscripten does not map
  touch → mouse off-canvas. JS wrapper `touchstart/move/end` →
  `Module._inject_mouse_*`.

<a id="wasm-003"></a>

## WASM-003 — Audio worklet tuning

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2127). Original dates and estimates are retained below.

- 🧊 **Audio worklet tuning** — miniaudio Web Audio works but
  latency ~150 ms is audible on speaker click. Explore a custom
  `AudioWorkletNode` or shrink the buffer.
- 2026-11-01: the buffer is now a ladder (`AudioBufferPolicy.h`): the
  browser starts at 2048 frames, a phone at 4096, and three late callbacks
  in ten seconds double it, up to 8192 — so a slow Android stops crackling
  at the price of latency, and the Audio panel shows which rung it is on.
  The worklet remains the only way to take the callback off the main
  thread; it needs `-sAUDIO_WORKLET` + pthreads, hence cross-origin
  isolation, which GitHub Pages serves only through a service-worker shim.

<a id="wasm-004"></a>

## WASM-004 — Shareable links: per-drive URL parameters and a copy-link button

**Priority:** 🟢 · **State:** Ready (research of 2026-11-18).

`?preset=`, `?disk=`, `?url=` mount one image. Missing: a second 5.25"
drive, the 3.5" and HDV drives, and a "copy link to this setup" button —
without them a two-disk game or a DIX setup cannot be shared. Apple2TS's
`s6d1` / `s6d2` / `s7d1` scheme is the model.

**Acceptance criterion:** a link with two 5.25" disks boots disk 1 with disk
2 in drive 2; the button reproduces the current profile and mounted URLs.

<a id="wasm-005"></a>

## WASM-005 — Embed mode (?embed=1 + postMessage events)

**Priority:** 🟢 · **State:** Ready (research of 2026-11-18).

Infinite Mac's `/embed` mode lets a page host the emulator and hear its
events. POM2: `?embed=1` hides the chrome, and `postMessage` reports boot,
pause and errors to the host page.

**Acceptance criterion:** a test page embeds POM2 in an iframe and receives
the boot event; nothing is posted unless `embed=1`.
