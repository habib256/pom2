# POM2 — TODO

Action queue updated 2026-10-17 · **v0.9.4 → 1.0**.
This page answers what to do next. Detailed tasks and acceptance criteria live
in `docs/backlog/`; dated reports and decisions have their own documents.

The requested non-IIgs emulator comparison work is tracked in
[the implementation report](docs/audits/non-iigs-parity-2026-10-17.md),
including implemented changes and the remaining hardware/product projects.

## Action queue

This is a short ordering of the next actions, not a second task record. The
linked domain entry owns the task's state, evidence and acceptance criterion.
Recheck dated claims against current main before implementing a reported gap.
Move an item when its next action or dependency changes.

### Now — unblock the 1.0 candidate

| Task | Next action |
|---|---|
| [RELEASE-001](docs/backlog/release-1.0.md#release-001) | Record the decision on commercial media still present in Git history, then align repository and release distribution with it. |
| [RELEASE-002](docs/backlog/release-1.0.md#release-002) | Obtain the origin and redistribution terms of the two shipped photographs from their owner; record them in `THIRD-PARTY.md`. |
| [RELEASE-004](docs/backlog/release-1.0.md#release-004) | Land the Ethernet panel concurrency test, capture its CI result, then decide whether other panel routes need the same harness. |
| [RELEASE-006](docs/backlog/release-1.0.md#release-006) | Land the Linux CI libslirp guard and record a passing run with no loopback-test skip. |

### Next — complete the remaining evidence

| Task | Next action |
|---|---|
| [VALIDATION-002](docs/backlog/validation.md#validation-002) | Boot DIX with the documented PAL profile and slot map; record menu and demo results before other corpus titles. |
| [RELEASE-005](docs/backlog/release-1.0.md#release-005) | Automate a browser setting write, persistence, reload and readback. |
| [RELEASE-008](docs/backlog/release-1.0.md#release-008) | Record launch and rendering on ARM/Pi hardware, or document the hardware limitation. |
| [RELEASE-009](docs/backlog/release-1.0.md#release-009) | Execute the Intel macOS slice and record a smoke test, or document the execution limitation. |

### Later — sign off the candidate

Once the gates above are resolved, run the
[candidate release checklist](docs/backlog/release-1.0.md#candidate-release-checklist)
against the actual 1.0 build, including packages, first launch and data
durability. Other domain work stays post-1.0 unless a release gate or a
current core defect brings it forward.

## Release 1.0

1. Describe shipped behavior accurately.
2. State the supported scope and deliberate limits.
3. Produce a repeatable, distributable release.
4. Preserve user data: media, settings and snapshots.

[Release plan and gates](docs/backlog/release-1.0.md) ·
[Scope and standing decisions](docs/decisions/project-scope.md).
G2–G4 implementation work is recorded as closed in the prior report;
candidate verification still has to run.

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
