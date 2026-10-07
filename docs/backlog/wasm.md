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
| [WASM-001](#wasm-001) | 🧊 | Frozen | File picker / drop-zone disks |
| [WASM-002](#wasm-002) | 🧊 | Frozen | Mobile touch input |
| [WASM-003](#wasm-003) | 🧊 | Frozen | Audio worklet tuning |

<a id="wasm-001"></a>

## WASM-001 — File picker / drop-zone disks

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2121). Original dates and estimates are retained below.

- 🧊 **File picker / drop-zone disks** — build-time bundling
  only. HTML5 drop-zone → `FS.writeFile('/uploads/…')` →
  `DiskIICard::insert`. *~1 d.*

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
