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
| [UI-002](#ui-002) | 🟡 | Integrated: cross-platform GUI checks pending | Native picker integration and platform verification |
| [UI-004](#ui-004) | 🧊 | Frozen — triage 2026-10-28 | Deeper guided tutorials |
| [UI-006](#ui-006) | 🧊 | Frozen — triage 2026-10-28 | Airier default layout |
| [UI-005](#ui-005) | 🧊 | Frozen | MicroM8-style 3D voxel view ("Voxel Cube") |
| [UI-008](#ui-008) | 🧊 | Frozen | On-screen touchscreen / virtual joystick |
| [UI-009](#ui-009) | 🟢 | Needs decision: cover art vs captured screen | Disk Library miniatures |
| [UI-010](#ui-010) | 🟢 | Implemented: installed-package checks pending | OS file associations for disk images |
| [UI-015](#ui-015) | 🟠 | Ready — chosen next | MCP facade and debugger verbs on the AI control server |
| [UI-016](#ui-016) | 🟢 | Ready | Debugger: expression conditions, range watches, access counters, code/data log |
| [UI-017](#ui-017) | 🟢 | Ready | Trace log to a file and a callgrind export of the profiler |
| [UI-018](#ui-018) | 🟢 | After UI-015 | Debug Adapter Protocol server (VS Code) |
| [UI-019](#ui-019) | 🟡 | Needs design | Reverse debugging over the rewind ring |
| [UI-020](#ui-020) | 🟢 | Ready | Speak the screen (accessibility) |
| [UI-021](#ui-021) | 🟢 | Ready | Video capture (GIF/MP4) of the screen |

<a id="ui-002"></a>

## UI-002 — Remaining native file picker integration and platform checks

**Priority:** 🟡 · **State:** Integrated (2026-11-17); interactive cross-platform verification pending.

**Acceptance criterion (after revalidation):** Use the native open/save dialog and verify cancellation, chosen paths and fallback behavior on supported hosts.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-905). Original dates and estimates are retained below.

`NativeFileDialog` provides AppKit, Win32 and external zenity/kdialog
backends, with ImGui fallback when unavailable. Integrated into drive
insertion, HGR/DHGR paint/sprite open/save and debugger-symbol selection.
Desktop session open/save also uses the native chooser, with overwrite
confirmation and a manual-path fallback when the backend is unavailable.
Cancellation is distinguished from backend unavailability.

**2026-11-17:** SmartPort, Disk II/HDV/Sony dialogs, Media panel bays,
cassette load/save, VSDrive, paste-from-file and text-printer paths now offer
native browsing through `FilePathPicker.h`. ImageWriter single-sheet PNG and
PDF exports offer a save chooser; batch sheets retain their generated names.
Cancelled or unavailable browsing preserves manual input. Fixed buffers reject
oversize selections instead of truncating; string fields retain long UTF-8
paths. The shared chooser interaction is exercised with fake backend results
in `file_path_picker`.

**Local validation 2026-11-17:** full macOS ARM64 Release build passed.
The 492-test registered suite yielded 491 passes and one optional-media skip
(`arekkusu_disks`), after rerunning the 20 network/GL cases blocked by the
sandbox with local sockets/graphics permitted. `file_path_picker` passed; a
fault injection that replaced the remembered path on cancellation failed its
assertion. File-size, version, settings-key, documentation-reference, workflow,
coordinator-lock and include guards passed. No native OS-dialog interaction or
Windows execution is established by those fake-backend tests.

Remaining: obtain interactive cancel/open/save evidence on every OS. Windows
is not validated by a macOS build.

UI-001 (inline-argument step-over) was corrected on 2026-10-17; see CHANGELOG
and the expanded `debugger` regression suite.

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

**Priority:** 🟢 · **State:** Needs decision ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): cover art or captured screen. The prerequisite (search/sort, "All media") exists. The earlier thumbnail-first order was superseded on 2026-11-17 by the explicit request for UI-010 followed by UI-002; thumbnails are not scheduled.

**Acceptance criterion:** Decide whether a thumbnail is user-supplied cover art
or a captured guest screen, then display it without loading every disk or
blocking the library's scan. Missing art must leave a readable text row.

<a id="ui-010"></a>

## UI-010 — OS file associations for disk images

**Priority:** 🟢 · **State:** Implemented (2026-11-17); installed-package platform verification pending.

**Acceptance criterion:** Register supported image extensions in each desktop
package, open a disk from the host file manager into the correct drive, and
verify behavior on macOS, Windows and Linux without overriding another app's
default association.

**Code reconciliation 2026-10-19:** `packaging/macos/Info.plist.in` declares
`.dsk`, `.do`, `.po`, `.nib`, `.woz`, `.d13`, `.2mg` and `.hdv` (and `.moof`
since).
`HostOpenFiles_mac.mm` receives Finder/Dock open events and `main.cpp` drains
the queue into the normal file-drop path. Linux installs `POM2.desktop`
(`Exec=POM2 %f`) and `pom2-disk.xml`; `.po` has a lower MIME weight to avoid
winning against gettext files. Linux/Windows positional disk arguments are
handled by the CLI. This is implemented plumbing, not proof of an installed
package's association behavior. **2026-11-17:** macOS uses `LSHandlerRank=Alternate`. The Windows ZIP ships
`FileAssociations.ps1` to register/remove a per-user `POM2.DiskImage` ProgID
and `OpenWithProgids` entries, with a quoted executable and file argument.
Registration is explicit, needs no elevation, and does not change extension
defaults or UserChoice. The release workflow runs an isolated-registry test of
registration, quoting, repeated registration/removal and preservation of other
handlers/defaults. Instructions live in the user guide.

Remaining: clean-host open/default-association checks on macOS, Windows and
Linux; no interactive Explorer validation is claimed from a macOS build.

<a id="ui-011"></a>

## UI-011 — Screen text capture integrations (completed 2026-11-06)

File → Copy screen text, GET `/screen/text` and headless `--screen-text`
now share the RAM decoder. Decoder and HTTP regressions pass, desktop/headless
builds pass, and local DOS 3.3 BASIC runs validate 40/80 columns, mixed-mode
text and empty output in pure graphics. `--text-dump` remains an alias.
See [CHANGELOG](../../CHANGELOG.md) and the
[usage and limitations](../USER-GUIDE.md#copy-and-inspect-screen-text).

<a id="ui-012"></a>

## UI-012 — PNG screenshots with capture metadata (completed 2026-11-06)

F9, the toolbar and command palette now export native RGB PNG with machine,
monitor and resolved CRT metadata, explicitly labelled before GL post-effects.
Numbering includes legacy PPMs. Encoding and atomic file writes run outside
machine locks. Pixel/UTF-8/CRC/numbering/error tests pass, macOS ImageIO decodes
identical pixels, and the desktop build passes. Interactive F9 and browser
execution were not exercised. See [CHANGELOG](../../CHANGELOG.md) and the
[screenshot guide](../USER-GUIDE.md#screenshots-and-settings).

<a id="ui-013"></a>

## UI-013 — Missing firmware guidance on the machine screen (completed 2026-11-06)

The screen shows the active profile and recovery actions while its system
ROM is absent. Desktop buttons open the existing downloader/status panel and
reload the system ROM after re-probing paths. Welcome shares that reload path:
file reads occur outside the controller lock, installation uses staged bytes.
Kiosk offers an exit to setup; the browser explains bundled profiles/sessions
without offering a download. Loaded firmware/session state removes the notice.
Guest mouse ownership is cleared while the notice is shown.

Desktop build and the five existing ROM/profile/kiosk/input regressions pass.
Interactive screen/kiosk/browser behavior was not exercised. See
[CHANGELOG](../../CHANGELOG.md) and the [user guide](../USER-GUIDE.md).

<a id="ui-014"></a>

## UI-014 — See the picture while adjusting CRT sliders (completed 2026-11-06)

Floating CRT panels use 25% alpha during a mouse slider drag, preserving the
active label/slider row. The draw list is rebuilt on release, so no transient
opacity can be saved or left behind. Docked panels and numeric text input
are excluded; global style and popups are unaffected. Desktop build and diff
checks pass; visual mouse/keyboard/browser interaction was not exercised.
See [CHANGELOG](../../CHANGELOG.md) and the
[display guide](../USER-GUIDE.md#the-glow-of-the-screen).

<a id="ui-015"></a>

## UI-015 — MCP facade and debugger verbs on the AI control server

**Priority:** 🟠 · **State:** Ready; chosen as the next large task on 2026-11-18 (research of 2026-11-18).

The loopback HTTP server (`AiControlServer`) already does screen, keys,
memory, disks and snapshots. Missing: (1) **debugger verbs** — breakpoints
(bank-qualified), step / step over / run until / continue with a timeout,
registers, symbol lookup (incl. the cc65 `.dbg` import), wait-for-screen-text;
(2) an **MCP endpoint** (`POST /mcp`, Streamable HTTP) exposing them and the
existing routes as tools, behind the same token and `Host` fence. Read the
current spec revision before writing it (2026-07-28 at the time of writing:
per-request metadata, the `initialize` handshake only for older revisions).
Prior art: AppleWin [PR #1519](https://github.com/AppleWin/AppleWin/pull/1519)
(2026-09, unmerged), apple2-mcp (Bobbin), vice-mcp, mesen-agent; microM8
announced one on 2025-08-03.

**Acceptance criterion:** Claude Code (`claude mcp add --transport http`)
boots a disk, sets a bank-qualified breakpoint, runs to it, reads registers
and memory and waits for a screen string, in a ctest that drives `/mcp` over
loopback; the fence and token tests pass unchanged for `/mcp`.

<a id="ui-016"></a>

## UI-016 — Debugger: expression conditions, range watches, access counters, code/data log

**Priority:** 🟢 · **State:** Ready (research of 2026-11-18).

What Mesen, Altirra and MAME offer and POM2's debugger does not: breakpoint
conditions as **expressions** (registers, memory, symbols; today one
register comparison per address), **range** watches, per-address **read /
write / execute counters** shown as a bank-aware heat map (the existing
Memory Map shows page occupation, not accesses), and a **code/data logger**
that marks bytes executed vs read, exportable for a disassembler.

**Acceptance criterion:** each feature pinned in `debugger` with a
bank-switched case (the same address in main and aux counted apart), and no
cost when off (`bus_fastpath` and `pom2_bench` unchanged within noise).

<a id="ui-017"></a>

## UI-017 — Trace log to a file and a callgrind export of the profiler

**Priority:** ✅ · **State:** Done 2026-11-19; removed from the active index.

An instruction trace written to a file (format string, bank, registers,
cycles), and the profiler's per-address cycles exported as a callgrind file
for KCachegrind — what a2trace builds by post-processing MAME traces.

**Acceptance criterion:** a trace of a known 6502 loop matches its expected
lines byte for byte; KCachegrind (or `callgrind_annotate`) reads the export
of a profiled run with the cc65 `.dbg` symbols.

**Done 2026-11-19:** `InstructionTrace` (behind the header-only
`InstructionTraceSink`, so no other binary's link line changed): one line
per instruction, registers read going IN, format tokens `%PC %BANK %BYTES
%DIS %A %X %Y %P %SP %FLAGS %CYC %TOTAL %LABEL`. `callgrindProfile`
(`ProfileExport.h`): Cycles and Visits per address, `ob=` the physical
bank, `fn=` the nearest symbol (cc65 `.dbg` or symbol file) else the page.
Debugger panel ▸ Profiler: Trace to file / Export callgrind; headless
`--trace-file`, `--trace-format`, `--profile`, `--symbols`. Pinned by
`instruction_trace` (a known loop byte for byte; callgrind read back) and
`headless_trace_profile` (the cc65 program with its `.dbg`: `fn=_main`,
`fn=_add`). **Limit:** not opened in KCachegrind / `callgrind_annotate`
(neither on the dev machine); flat costs only, no call graph.

<a id="ui-018"></a>

## UI-018 — Debug Adapter Protocol server (VS Code)

**Priority:** 🟢 · **State:** Blocked on UI-015 (its debugger verbs).

Source-level debugging in an IDE: a DAP server mapping breakpoints and
stepping onto the debugger, with source lines from `.p2map` / cc65 `.dbg`
(`Cc65DebugInfo.h`, 2026-11-18). No Apple II emulator offers one (research of 2026-11-18);
VS64 does it for the C64 over VICE's binary monitor.

**Acceptance criterion:** VS Code stops on a breakpoint set in a `.s` file
of a cc65 program, steps by source line and shows registers, in both banks.

<a id="ui-019"></a>

## UI-019 — Reverse debugging over the rewind ring

**Priority:** 🟡 · **State:** Needs design.

Step back / run back to a breakpoint using the rewind ring. Today a rewind
clears the debugger's measurements and the ring drops at every irreversible
media write (CLAUDE.md, rewind rule), so the design must say what survives.

**Acceptance criterion:** a written design first; then "step back" over a
JSR returns to the identical register and memory state, pinned in `debugger`.

<a id="ui-020"></a>

## UI-020 — Speak the screen (accessibility)

**Priority:** 🟢 · **State:** Ready (research of 2026-11-18).

Dear ImGui exposes nothing to screen readers (ocornut/imgui #8022, #7131).
The realistic route: feed the existing screen-text decoder (Copy screen
text, `GET /screen/text`) to the OS speech engine on the desktop and to a
hidden `aria-live` region in the browser, speaking what changed.

**Acceptance criterion:** with the option on, typing `CATALOG` on DOS 3.3
speaks the new lines once, not the whole screen every frame; off by default.

<a id="ui-021"></a>

## UI-021 — Video capture (GIF/MP4) of the screen

**Priority:** ✅ · **State:** Done 2026-11-19 (in `pom2_corpus`); removed from the active index.

POM2 has PNG screenshots only; MAME has `-aviwrite`/`-mngwrite`. Frames from
the emulated video frame (not the host refresh), so a capture under x4 or a
stall has the program's timing. Pairs with AUDIO-013's WAV.

**Acceptance criterion:** a 5 s capture of a corpus title has 250 (PAL) or
300 (NTSC) frames whose hashes match `pom2_corpus --frames` for the same run.

**Done 2026-11-19:** `pom2_corpus --video DIR` writes every video frame of
each title as `DIR/<title>/NNNNN.png` + `frames.txt` (index, hash), and an
MP4 at the standard's exact rate when `ffmpeg` is on the PATH (560x384,
nearest-neighbour). Frames come from the emulated beam: the new SDK call
`Core::lastVideoFrameStart()` changes once per finished frame, polled after
each 4096-cycle slice. The acceptance is enforced on every recording run:
a check whose hash differs from the last frame written fails the title.
Pinned by `corpus_video` (DIX's menu: 2803 frames for 56 s at 50.08 fps,
both checks equal to their frames). **Not done:** a recorder in the GUI
(the screen as the user sees it, with the CRT pass), and GIF.

