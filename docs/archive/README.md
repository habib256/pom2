# Archive — historical working documents

One-off snapshots of finished efforts, kept for **provenance** (the *why* behind
a fix, the audit method). They are **not** maintained: their line references may
be stale and their conclusions are already folded into the living docs. Do not
treat them as current state. Original wording and language are preserved;
the collection includes English and French material. Current documentation is
indexed in [the documentation guide](../README.md).

| File | What | Status → where the current info lives |
|---|---|---|
| `video_parity_audit_2026-05-30.md` | Video/colour/effects parity audit (9 findings, TOP-10 actions) | Headline gaps **resolved** (square-filter, OE GPU/CPU). Living doc: [`../graphics_modes_comparison.md`](../graphics_modes_comparison.md). Residuals → `TODO.md` § [Display] *CRT parity refinements*. |
| `video_parity_revalidation_2026-05-30.md` | Post-WIP re-validation of the audit above + F0–F9 implementation cards | Same. The F2/F3/F4/F6/F7 cards still open are listed in `TODO.md` § [Display]. |
| `oe_gpu_cpu_parity.md` | Agent notes on the OE GPU≠CPU colour bug | **Resolved + pinned** `oe_demod_gpu_cpu_parity`. Living detail: `DEV.md` § Composite NTSC shader + that archived changelog, 2026-05-30. |
| `CHANGELOG-2026-05.md` | Pre-v0.7 changelog entries (2026-05-14 → 2026-05-30) | Split out of the living `CHANGELOG.md` to keep it light. Current changelog → [`../../CHANGELOG.md`](../../CHANGELOG.md). |
| `todo-2026-10-05.md` | Original planning/audit snapshot with stable evidence anchors | Current queue: [TODO](../../TODO.md); tasks: [domain backlogs](../../TODO.md#backlog); standing decisions: [project scope](../decisions/project-scope.md). |

Full canonical history → `git log` + [`../../CHANGELOG.md`](../../CHANGELOG.md).
