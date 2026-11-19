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

**Priority:** ✅ · **State:** Done 2026-11-19; removed from the active index.

`?preset=`, `?disk=`, `?url=` mount one image. Missing: a second 5.25"
drive, the 3.5" and HDV drives, and a "copy link to this setup" button —
without them a two-disk game or a DIX setup cannot be shared. Apple2TS's
`s6d1` / `s6d2` / `s7d1` scheme is the model.

**Acceptance criterion:** a link with two 5.25" disks boots disk 1 with disk
2 in drive 2; the button reproduces the current profile and mounted URLs.

**Done 2026-11-19:** `?sNdM=` per drive (Apple2TS's keys, a URL or a
bundled path), `?boot=sNdM`, default boot = drive 1 of the highest slot;
the others reach the machine as the new CLI flag `--mount sNdM:<path>`
(also on the desktop; the mount writes nothing, the drive is remembered at
quit like any mounted disk, and a restored disk in the drive is replaced). File ▸ Copy link to
this setup (browser) writes the profile and every linkable drive to the
clipboard and the address bar; a disk imported from the computer cannot be
linked and is counted. Planner and link builder in `wasm/setup_link.js`,
pinned under node by `wasm_setup_link`; `--mount` parsing by `cli_kiosk`;
the desktop two-disk boot checked through `/status`. In Chrome,
`tools/wasm_links_smoke.mjs` (CI WASM job) opens a two-disk link, sees disk
1 boot and disk 2 mounted in drive 2, presses Copy link
(`pom2_copy_setup_link`) and gets back the very link it opened.

<a id="wasm-005"></a>

## WASM-005 — Embed mode (?embed=1 + postMessage events)

**Priority:** ✅ · **State:** Done 2026-11-19; removed from the active index.

Infinite Mac's `/embed` mode lets a page host the emulator and hear its
events. POM2: `?embed=1` hides the chrome, and `postMessage` reports boot,
pause and errors to the host page.

**Acceptance criterion:** a test page embeds POM2 in an iframe and receives
the boot event; nothing is posted unless `embed=1`.

**Done 2026-11-19:** `?embed=1` passes `--kiosk` (in the browser: the
chrome-free render in the page's canvas, no fullscreen request; settings
untouched) and hides the page's own link; `window.parent.postMessage({source:
'pom2', event, detail})` reports `boot` (first frame, `{preset}`), `pause` /
`running` (polled from `pom2_browser_running`) and `error` (abort, rejected
promise, lost WebGL, a disk that could not load). Nothing is posted without
`embed=1` or outside a frame. Pinned in Chrome by `tools/wasm_links_smoke.mjs`:
a host page framing `?embed=1` receives `boot`; one framing the page without
it receives nothing. **Not exercised by a test:** `pause` / `running`.

