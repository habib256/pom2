# Backlog — how it works

[Planning index](../../TODO.md) · [Scope decisions](../decisions/project-scope.md)

`TODO.md` is the action queue, sorted by what unblocks each task. The domain
files in this directory own every task's state, acceptance criterion and
evidence. [possibilities.md](possibilities.md) holds ideas that are not tasks.
Moved out of `TODO.md` on 2026-11-17.

## Task lifecycle

- **To verify:** reproduce or refute a dated report on current main; record the command, result and revision.
- **Ready:** confirmed scope, priority and acceptance test; dependencies resolved.
- **In progress:** record the branch and remaining validation in the domain entry.
- **Blocked:** name the missing dependency or decision and the condition that unblocks it.
- **Frozen:** schedule only for a named software need or an explicit request.

A task has one stable ID and one domain entry. Link to it from other domains
instead of duplicating it. Keep its title, priority, state, acceptance criterion
and evidence together. When shipped, remove it from the active backlog and
record the result and rationale in [CHANGELOG.md](../../CHANGELOG.md).
A ruled-out feature belongs in the scope decisions.

## Evidence and decisions

- [Backlog triage of 2026-10-28](../audits/backlog-triage-2026-10-28.md) — the verdicts behind this queue.
- [Emulation parity dashboard](../audits/emulation-parity.md) — fidelity references; revalidate the affected row after an implementation change.
- [Risk measurements from 2026-09-05](../audits/todo-risk-2026-09-05.md) — historical measurements, not today's coverage.
- [Original TODO snapshot](../archive/todo-2026-10-05.md) — full reports, completed tasks and their proof, preserved during this migration.
- [Integration corpus](../test_corpus.md) — real software and validation procedures.
- [Scope and standing decisions](../decisions/project-scope.md) — core/supported/frozen, intentional divergences and excluded projects.

## Maintenance

Recheck old source citations before acting. A migration is not proof of a
bug, a completed task, or a current CI result. Acceptance tests should fail
against the reverted defect; a successful command alone is insufficient.
Historical snapshots remain unchanged after migration. New audit evidence
belongs in `docs/audits/` and the task links to it.

## Already done — do not schedule again

**2026-11-18 batch** (CHANGELOG of that date): cc65 `.dbg` import into the
debugger's Source and Symbols sections; deater's six NTSC //e productions as
`deater` corpus titles (their media upload is RELEASE-015); Phantasie in the
protected corpus; the `$C050` switching-read check from AppleWin #1386 pinned
in `floating_bus_measured`; the `$C019` VBL edge checked against Hectic (no
change needed); the Passport MIDI card (AUDIO-009, real-software run
pending). **Looked at and rejected:** Tom Harte's `rockwell65c02` and
`synertek65c02` sets — redundant on the shared opcodes, and describing chips
no POM2 machine has where they differ (the Apple's CPU is the NCR65C02).

**2026-11-17 core batch:** STORAGE-014 now has deterministic phase-2 write
coverage and fixes the reproduced false host conflict; ARCH-014 adds atomic
clock inspection and mutation-checked fan-out tests; ARCH-015 moves the real
slot composition policy into a headless-tested coordinator. The final locked
host-folder flush after four captures remains an explicit, tested limit.
See [evidence and limits](../audits/core-priorities-2026-11-17.md).

Do not schedule these again: heap-backed DiskImage storage (STORAGE-003),
compatibility corpus (VALIDATION-006), input bindings/controllers, browser
file import, screen text/PNG/firmware guidance/CRT preview (UI-011–014),
Porte-Parole and E-Z Color integration. Closed domain anchors remain for
incoming links. The November 16 test-audit fixes include named ctest inventory,
Windows atomic-file coverage, stronger assertions, and the audio buffer ladder.
The round-2 skip detector is already broadened; its old blind spots are not
an instruction to implement another detector.

The [2026-10-28 triage](../audits/backlog-triage-2026-10-28.md),
[round-2 hunt](../audits/bug-hunt-2026-11-01-round2.md), and
[November 9 consolidation](../audits/core-consolidation-2026-11-09.md)
remain historical evidence. Missing media (notably the repaired Service Disk)
are separate from missing implementation. Gates G2–G5 and RELEASE-003–007
mostly call for a fresh candidate run, not new CI systems.
