# Backlog — Input

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
| [INPUT-001](#input-001) | 🟡 | To verify | Host Shift is not wired to `$C063`, and the dead code that would wire it has the polarity backwards |
| [INPUT-002](#input-002) | 🟡 | To verify | PADL(2)/PADL(3) host binding |
| [INPUT-003](#input-003) | 🟡 | To verify | Mouse → paddles mapping |

<a id="input-001"></a>

## INPUT-001 — Host Shift is not wired to `$C063`, and the dead code that would wire it has the polarity backwards

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Test PB2 polarity with Shift up/down and the SHK option enabled/disabled for the applicable machine profile.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-816). Original dates and estimates are retained below.

- 🟡 **Host Shift is not wired to `$C063`, and the dead code that would wire it
  has the polarity backwards** (bug hunt #4). `PaddleInputs::button2(iieMode)`
  folds a `shift_` atomic into PB2 and `Memory::setShiftKey` forwards to it;
  **nothing in the tree calls either**. Wiring it up as written would make wrong
  behaviour visible instead of invisible, twice over. MAME's `c000_r` returns
  `0x80` when Shift is **UP** — the SHK jumper grounds PB2, so pressing the key
  pulls the line low — and it does so only when the machine option
  `m_kbd_shift_mod` is set, which is a IIe Platinum machine option POM2 has no concept of.
  A real fix owns `PaddleInputs.h`, inverts the sense, and adds the machine
  option; a lone `onKey` branch does not.

<a id="input-002"></a>

## INPUT-002 — PADL(2)/PADL(3) host binding

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Bind and read both second-paddle axes through PADL(2)/PADL(3), including calibration and persistence.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2057). Original dates and estimates are retained below.

- 🟡 **PADL(2)/PADL(3) host binding** — no host axes are bound to the second
  stick; `JoystickInput.cpp:60-75` is the NaN-guarded `[-1,+1] → [0,255]`
  mapper it would feed.
  *Revalidated 2026-10-06:* still true — `JoystickInput::paddleValue`
  returns 128 for paddles 2/3 ("2/3 are not wired", `src/JoystickInput.cpp:290-302`);
  the mapper is `axisToPaddle01` at `:58-73`.

<a id="input-003"></a>

## INPUT-003 — Mouse → paddles mapping

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Map host mouse axes to paddles with explicit sensitivity, bounds and an enable/disable control.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2060). Original dates and estimates are retained below.

- 🟡 **Mouse → paddles mapping** — paddle 0/1 on host mouse X/Y axes
  (alternative to pads).
