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

No open task: every entry below is implemented (the last on 2026-11-06)
and kept only so incoming links still resolve.

<a id="input-007"></a>

## INPUT-007 — Sirius Joyport, Atari mode

**State:** Atari mode implemented, 2026-11-06; removed from the active index.
Centre-selector physical polarity remains a possibility in TODO, explicitly
provisional in the UI. Named software originally: **Bandits** (Sirius,
1982), tested with a Joyport in David H. Ahl's contemporary review in
[Creative Computing Video & Arcade Games](https://www.atarimagazines.com/cva/v1n1/applegames.php).
Follow-up requested to the Casso input ideas; independent implementation.

Implement the Atari digital interface on II/II+/IIe, selected at bus-read
time by AN0/AN1, with the physical left/centre/right override. No rear
analogue controllers in this first scope. Preserve active-low contacts,
floating-bus low bits, aliases, reset behaviour and snapshots. Two selected
host pads and the optional keyboard supply switches; no retained velocity
position or duplicated D-pad keyboard events. IIc has no annunciator socket.

Sources: Sirius Joyport User Manual pp. 10–13 (**B**), including its AN0
contradiction (prose/BASIC versus figure 5/Pascal); Casso `2d899adf` (**D**)
as a comparison, not a source of hardware truth. Resolve and document the
chosen mapping against the named software before calling centre validated.

Validation uses **Wavy Navy**, whose Z option selects the digital Joyport
driver. The optional local-media probe boots it and verifies actual boat
movement through both fixed sockets. **Bandits gameplay is now validated**
on both Asimov images: boot and keyboard selection, neutral/left/right,
single/repeated fire, up for shield, down and inactive-socket isolation,
plus 30 seconds of firing, shield consumption, death and respawn. Its probe
waits for the intro's Space poll and sends NUL in play (Ctrl+@ in POM2).
No guest code/state was patched to obtain these results.
Computer Foosball scans two button banks in analogue mode;
that does not resolve Atari polarity. See the [hardware report](../hardware/sirius-joyport.md)
for source contradictions, media hashes, driver addresses and reproduction.

Desktop build and input/bus/snapshot/session regressions passed. A new bus
test covers immediate AN selection, aliases, floating low bits, open paddle
inputs, reset, IIc exclusion, file/rewind restoration and malformed-state
refusal. Host polling tests cover independent digital sticks, D-pad,
velocity independence, cancellation and disconnects. Session reconstruction
also preserves Joyport across machine profiles. No physical controller play
or browser build was run; no third-party game/firmware was added to the tree.

<a id="input-006"></a>

## INPUT-006 — Paddle velocity and absolute axes

**State:** Implemented, 2026-11-06; removed from the active task index.
Requested follow-up to INPUT-005; see [CHANGELOG](../../CHANGELOG.md).

Per-paddle modes: legacy joystick (default), velocity with retained position,
or absolute throttle. Velocity uses independent deadzone/inversion, adjustable
full-range travel per second and a centre action. Absolute uses the full raw
axis range without radial deadzone, notch or square-gate coupling. A mixed-mode
pair also treats its remaining joystick axis independently. Host-time control
speed is independent of emulator turbo; pause, focus loss, UI capture and
overriding keyboard/mouse sources stop accumulation. Disconnect/rebind/source
or mode change resets velocity to centre. Persist modes and sensitivities,
not the transient position. Independent host policy; no Casso code borrowed.

Acceptance: equal travel at different frame rates; bounded finite output,
no movement at rest/while suspended; independent absolute endpoints and
inversion; no stale position after reconnect or remap; existing joystick and
two-controller tests remain green.

Validation: `pom2_imgui` built; `joystick_paddle_modes`,
`joystick_two_players`, `joystick_navigation`, `joystick_square_gate`,
`keyboard_joystick` and `key_chord_policy` passed. The new test drives the
real poll/routing path with simulated controllers, comparing 30/144 Hz,
end stops, rest, suspension, reconnect, remap, two-player independence,
absolute endpoints/inversion, mixed modes and non-finite inputs. Physical
controller play and browser compilation were not exercised.

<a id="input-005"></a>

## INPUT-005 — Two host controllers

**State:** Implemented, 2026-11-06; removed from the active task index.
Follow-up requested after INPUT-004; see [CHANGELOG](../../CHANGELOG.md).

Keep the single-pad default. An optional second-controller selector offers
automatic assignment or an explicit host slot. The first pad drives PDL0/1,
the second PDL2/3 with its own source choices. Host policy: A/B (raw button 0)
fire PB0 for player 1 and PB1 for player 2; neither sources PB2. Preserve the
single-pad button mapping when the option is off. A disconnected or conflicting
binding stays neutral; no fallback to the first player's right stick. Only
the first controller navigates menus or types keys. Persist selections and
sources; test real poll/routing with simulated pads, including hotplug and
automatic assignment. No Casso code or hardware behaviour is borrowed.

Validation: desktop `pom2_imgui` built; `joystick_two_players`,
`joystick_navigation`, `joystick_square_gate`, `keyboard_joystick` and
`key_chord_policy` passed. The new test drives production GLFW polling with
three simulated devices: independent axes and fire, source/inversion changes,
hotplug, explicit conflicts, raw-pad fallback, menu/key isolation for player 2,
automatic preferences, and returning to single-pad mode. Physical controllers
and the browser build were not exercised. Persisted identities are host slots.

<a id="input-004"></a>

## INPUT-004 — Keyboard joystick

**State:** Implemented, 2026-11-06; removed from the active task index.
Requested from the Casso possibilities; see [CHANGELOG](../../CHANGELOG.md).

Host-side policy, implemented independently: no Casso code or hardware claim.
Opt-in in the Joystick panel, persisted as `joystick_keyboard`. Arrows drive
PDL0/1, X drives PB0 and Z PB1. Opposing directions centre the axis. Keyboard
axes take precedence over mouse/gamepad axes; fire buttons combine with the
existing sources. UI keyboard capture, kiosk menus and focus loss release
the keys; resuming requires a fresh press. Modified shortcuts remain usable.

Acceptance: no Apple keyboard strobe or AKD from consumed keys, layout-aware
X/Z on desktop, no stuck direction/fire across release, capture or disable,
diagonals reach both endpoints, settings survive restart. Build the desktop
application and run the input regression tests. Browser uses the existing
physical-key fallback because GLFW key-name lookup is unavailable there.

Validation: desktop target `pom2_imgui` built; `keyboard_joystick`,
`key_chord_policy`, `joystick_square_gate` and `joystick_navigation` passed.
The new policy test exercises opposing keys, full diagonals, independent
buttons, AZERTY mapping, changed modifiers/layout at release, shortcuts,
suspension and stale auto-repeat. Interactive play and a browser build were
not run for this change.

<a id="input-002"></a>

## INPUT-002 — PADL(2)/PADL(3) host binding

**State:** Implemented; removed from the active task index on 2026-10-19.

`JoystickInput::update` maps standard gamepads' right-stick X/Y to paddles
2/3. `paddleValue` handles both axis pairs with their corresponding inversion
settings and shared deadzone/square-gate policy; `MainWindow_Input` forwards
all four axes to memory. Unmapped devices use only their first two raw axes
and keep 2/3 centered, because extra raw axes may be triggers.
`joystick_square_gate` covers independent sticks, right-axis inversion,
deadzone and disconnect fallback. The historical unbound-axis report is
superseded; see [CHANGELOG](../../CHANGELOG.md).
