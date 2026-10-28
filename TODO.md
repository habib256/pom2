# POM2 — TODO

Action queue updated 2026-10-28 · **post-1.0 (v1.0.0 released)**.
This page answers what to do next. Detailed tasks and acceptance criteria live
in `docs/backlog/`; dated reports and decisions have their own documents.

Current sessions reconstruct supported machines and embed writable media,
firmware and program tapes; see the [v7 contract](docs/session-format.md).
The debugger supports bank-qualified stops, profiling and source correspondence;
see the [debugger guide](docs/debugger-banks.md).

## Triage of 2026-10-28

Every open backlog entry was rechecked against `aad2fee5`
([evidence](docs/audits/backlog-triage-2026-10-28.md)): 4 closed (ARCH-001,
NETWORK-002, NETWORK-003, UI-007 — see CHANGELOG), 8 frozen with a reopen
condition, 3 added (CPU-001, VALIDATION-006, ARCH-016), and the RVB Graph
drift corrected in the parity dashboard and the scope ruling. No entry is left
in the bare *To verify* state.

The queue below sorts what remains by **scope tier** (the
[scope ruling](docs/decisions/project-scope.md#the-scope-ruling): core is
worked proactively, supported is fixed on report) and by **what unblocks it**.
**Admission rule:** no new card or peripheral without a named piece of
software that needs it.

## Action queue

The linked domain entry owns the task's state, evidence and acceptance
criterion. Move an item when its next action or dependency changes.

### Core — in progress (at most 5)

| Task | Next action |
|---|---|
| [CPU-001](docs/backlog/cpu-memory.md#cpu-001) | Design the per-cycle microsequences from MAME's `.lst` tables as a separate core; Harte per-cycle bus ordering is the gate. |
| [VALIDATION-006](docs/backlog/validation.md#validation-006) | Build the headless corpus runner, DIX first; record the first pass rate. |
| [DISPLAY-002](docs/backlog/display.md#display-002) | Add ALTCHAR / MouseText hash pins from the tracked `apple2e_char*.rom`. |
| [MEMORY-002](docs/backlog/cpu-memory.md#memory-002) | Re-profile a ][+ banner, then prototype the page-dispatch table against `bus_fastpath`. |
| [STORAGE-014](docs/backlog/storage.md#storage-014) | Test a host-folder bay dirtied during each phase-2 commit, or record the residue as an accepted limit. |

### Core — ready

| Task | Next action |
|---|---|
| [STORAGE-003](docs/backlog/storage.md#storage-003) | Best-of-9 `pom2_bench` with `DiskImage::tracks` on the heap. |
| [AUDIO-003](docs/backlog/audio.md#audio-003) | Re-record MAME with each source isolated; compare with `audio_stereo`'s 0.30. |
| [ARCH-016](docs/backlog/architecture-tooling.md#arch-016) | Write the doc-reference checker with negative controls; wire it into the CI Linux job. |
| [ARCH-004](docs/backlog/architecture-tooling.md#arch-004) | Lower the slack ceilings (`ImageWriter.cpp`, `Apple2Display.cpp`); split the DiskImage WOZ save half. |
| [ARCH-013](docs/backlog/architecture-tooling.md#arch-013) | Comment/declaration-only pass on `M6502`, verified by unchanged Harte/Klaus. |

### Needs a hardware oracle

Group these into one session with the machines (or a MAME with Apple II
romsets, where noted).

| Task | What to measure |
|---|---|
| [VALIDATION-001](docs/backlog/validation.md#validation-001) | Real //c: PH1→PH3 gaps of 4…512 cycles while the motor coasts; did the head move? |
| [VALIDATION-004](docs/backlog/validation.md#validation-004) | DOS 3.1 / 3.1.1 masters on a ][+ with 13-sector PROMs (or MAME `apple2p`). |
| [MEMORY-003](docs/backlog/cpu-memory.md#memory-003) | Language Card power-on bank: distinct bytes in banks 1/2, power-cycle, read `$D000`. |
| [DISPLAY-001](docs/backlog/display.md#display-001) | A real VidHD capture: SHR 320/640/fill and a `$C022`/`$C034` colour screen. |
| [DISPLAY-011](docs/backlog/display.md#display-011) | Fetch Eve Leonard for a third Eve golden; the other residuals need a board. |
| [AUDIO-011](docs/backlog/audio.md#audio-011) | Recordings of a real SSI263 sweeping INFLECT / RATE / ARTIC. |

### Needs a decision

| Task | Question |
|---|---|
| [UI-009](docs/backlog/ui-debugger.md#ui-009) | Miniatures from cover art or a captured screen? (Requested order: this, then [UI-010](docs/backlog/ui-debugger.md#ui-010).) |
| [ARCH-011](docs/backlog/architecture-tooling.md#arch-011) | Close as "worker stopped during Apply, by design", or wait for ARCH-003? |
| [STORAGE-002](docs/backlog/storage.md#storage-002) | Add a slot+bay `/media` mount/boot endpoint, or narrow the entry to the API gap? |
| [NETWORK-001](docs/backlog/network.md#network-001) | Freeze native FujiNet phases 2-4 until a named need? |
| [NETWORK-007](docs/backlog/network.md#network-007) | Schedule FujiNet on //c-class profiles, or freeze? |
| [NETWORK-004](docs/backlog/network.md#network-004) | One live delayed-TNFS run against a named peer, or close? |

### Supported — fixed on report, or quick wins

| Task | Next action |
|---|---|
| [UI-003](docs/backlog/ui-debugger.md#ui-003) | `/screen` answers POST: GET-only check (405) with a smoke assertion. |
| [UI-010](docs/backlog/ui-debugger.md#ui-010) | macOS `LSHandlerRank` Owner → Alternate for `.po`/`.dsk`; document Windows "Open with". |
| [UI-002](docs/backlog/ui-debugger.md#ui-002) | Move the typed-path panels to `NativeFileDialog`; Windows cancel/open/save check. |
| [CARDS-001](docs/backlog/cards.md#cards-001) | Add the ten missing `AbsEntry` rows from `docs/lle_vs_hle.md`. |
| [ARCH-003](docs/backlog/architecture-tooling.md#arch-003) | Add `frontend_device_panel_concurrency` and `drive_rack_interaction` to the TSan leg. |
| [ARCH-009](docs/backlog/architecture-tooling.md#arch-009) | One more card-pointer panel under replug, or close. |
| [ARCH-010](docs/backlog/architecture-tooling.md#arch-010) | An env-var table in `printUsage()`. |
| [ARCH-012](docs/backlog/architecture-tooling.md#arch-012) | Namespace migration, one directory at a time. |
| [ARCH-014](docs/backlog/architecture-tooling.md#arch-014) | Clock getter for speaker, cassette and floppy sound. |
| [ARCH-015](docs/backlog/architecture-tooling.md#arch-015) | Cover `plugSlotsFromSettings` headless or move its policy into the coordinator. |
| [ARCH-008](docs/backlog/architecture-tooling.md#arch-008) | Paint-editor seam on a report only (editors are frozen). |
| [NETWORK-005](docs/backlog/network.md#network-005) | Atomic in-flight flag shown in the UI and `/status`. |
| [NETWORK-010](docs/backlog/network.md#network-010) | LAN mapping, when someone names a need. |
| [AUDIO-006](docs/backlog/audio.md#audio-006) | Measure lock wait after ARCH-003; change only on a measured wait. |
| [VALIDATION-003](docs/backlog/validation.md#validation-003) | Record WOZ SHA-256s and probe commands (feeds VALIDATION-006). |
| [VALIDATION-005](docs/backlog/validation.md#validation-005) | Six GUI runs on a copy of A2 File Cmd 1.0; close as stale if quiet. |
| [DISPLAY-010](docs/backlog/display.md#display-010) | F6 luminance-neutral row dim, on report. |

### Release

Remaining 1.0 release gates and the candidate checklist:
[release-1.0.md](docs/backlog/release-1.0.md).

| Task | Next action |
|---|---|
| [RELEASE-008](docs/backlog/release-1.0.md#release-008) | Record launch and rendering on ARM/Pi hardware, or document the hardware limitation. |
| [RELEASE-009](docs/backlog/release-1.0.md#release-009) | Execute the Intel macOS slice and record a smoke test, or document the execution limitation. |

For the Disk Library follow-up, the requested order is
[miniatures](docs/backlog/ui-debugger.md#ui-009), then
[desktop file associations](docs/backlog/ui-debugger.md#ui-010).

<a id="backlog"></a>

## Backlog by domain

Each file separates items to verify from frozen work and retains links to the
original evidence.

| Domain | Details |
|---|---|
| CPU and memory | [cpu-memory.md](docs/backlog/cpu-memory.md) |
| Display | [display.md](docs/backlog/display.md) |
| Audio | [audio.md](docs/backlog/audio.md) |
| Storage | [storage.md](docs/backlog/storage.md) |
| Slot cards and peripherals | [cards.md](docs/backlog/cards.md) |
| Cassette | [cassette.md](docs/backlog/cassette.md) |
| Network and FujiNet | [network.md](docs/backlog/network.md) |
| Input | [input.md](docs/backlog/input.md) |
| UI and debugger | [ui-debugger.md](docs/backlog/ui-debugger.md) |
| WebAssembly | [wasm.md](docs/backlog/wasm.md) |
| Architecture and tooling | [architecture-tooling.md](docs/backlog/architecture-tooling.md) |
| Integration validation | [validation.md](docs/backlog/validation.md) |
| Parked projects | [parked.md](docs/backlog/parked.md) |

## Task lifecycle

- **To verify:** reproduce or refute a dated report on current main; record the command, result and revision.
- **Ready:** confirmed scope, priority and acceptance test; dependencies resolved.
- **In progress:** record the branch and remaining validation in the domain entry.
- **Blocked:** name the missing dependency or decision and the condition that unblocks it.
- **Frozen:** schedule only for a named software need or an explicit request.

A task has one stable ID and one domain entry. Link to it from other domains
instead of duplicating it. Keep its title, priority, state, acceptance criterion
and evidence together. When shipped, remove it from the active backlog and
record the result and rationale in [CHANGELOG.md](CHANGELOG.md).
A ruled-out feature belongs in the scope decisions.

## Evidence and decisions

- [Backlog triage of 2026-10-28](docs/audits/backlog-triage-2026-10-28.md) — the verdicts behind this queue.
- [Emulation parity dashboard](docs/audits/emulation-parity.md) — fidelity references; revalidate the affected row after an implementation change.
- [Risk measurements from 2026-09-05](docs/audits/todo-risk-2026-09-05.md) — historical measurements, not today's coverage.
- [Original TODO snapshot](docs/archive/todo-2026-10-05.md) — full reports, completed tasks and their proof, preserved during this migration.
- [Integration corpus](docs/test_corpus.md) — real software and validation procedures.
- [Scope and standing decisions](docs/decisions/project-scope.md) — core/supported/frozen, intentional divergences and excluded projects.

## Maintenance

Recheck old source citations before acting. A migration is not proof of a
bug, a completed task, or a current CI result. Acceptance tests should fail
against the reverted defect; a successful command alone is insufficient.
Historical snapshots remain unchanged after migration. New audit evidence
belongs in `docs/audits/` and the task links to it.
