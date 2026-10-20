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
| [WASM-001](#wasm-001) | 🧊 | Frozen | Disk-image drop-zone (picker implemented) |
| [WASM-002](#wasm-002) | 🧊 | Frozen | Mobile touch input |
| [WASM-003](#wasm-003) | 🧊 | Frozen | Audio worklet tuning |

<a id="wasm-001"></a>

## WASM-001 — Disk-image drop-zone (picker implemented)

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2121). Original dates and estimates are retained below.

Direct disk-image selection was implemented on 2026-10-20 in the untagged
checkout: File > Import disk and boot
and empty-drive Insert actions open the local browser selector. Uploads have
unique MEMFS paths and use the normal media routing and loaders. The 64 MiB
upload bound is separate from drive and session bounds.
See the [user-facing import contract](../USER-GUIDE.md#browser-disk-import).
Local validation built the native and WebAssembly applications and passed
the DOM/FS tests and Chrome smoke. The smoke exercises import/boot from File,
same-name import into drive 2 without rebooting, preservation of drive 1 and
both media in a session archive. This does not establish published-package
availability or a candidate CI result.

Desktop drag-and-drop mounting remains frozen. The browser is
not restricted to build-time content: `BrowserFiles` and
`wasm/browser_files.js` already provide text upload and `.pom2session`
import/export. Sessions can embed disk contents; automatic resume stores
supported configurations in IndexedDB. The disk-image picker is separate
from these text/session transfers. See the
[session contract](../session-format.md) and [RELEASE-005](release-1.0.md#release-005)
for the implemented browser persistence checks.

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
