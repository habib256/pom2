# POM2 — TODO

Reorganised 2026-10-05 · **v0.9.4 → 1.0**.
This is the planning index. Detailed tasks live in `docs/backlog/`; dated
reports and decisions have their own documents.

## Priorities

Priorities below are inherited from the previous TODO. Historical reports
must be reproduced on current main before they become implementation work.

| Order | Work | Completion criterion | Details |
|---|---|---|---|
| 1 | Distribution decisions | Record the repository-history decision and terms/provenance for the two photographs. | [G1](docs/backlog/release-1.0.md#g1) |
| 2 | Platform evidence | Obtain Windows test evidence or document its limitation; record ARM/Pi and Intel macOS validation. | [G6](docs/backlog/release-1.0.md#g6) |
| 3 | Remaining validation seams | Verify nightly alerts, GL/UI concurrency, browser persistence and libslirp transport. | [G5](docs/backlog/release-1.0.md#g5) |
| 4 | Corpus validation | Run DIX first; record remaining protected-disk and real //c hardware observations. | [Validation](docs/backlog/validation.md) |
| 5 | Candidate release | Complete checks against the actual 1.0 build, including packages and first launch. | [Checklist](docs/backlog/release-1.0.md#candidate-release-checklist) |

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

Domain work is **post-1.0 by default**, unless a release gate names it or a
current report affects a core subsystem. Each file separates items to verify
from frozen work and retains links to the original evidence.

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
