# POM2 — TODO

Action queue reconciled **2026-11-17**, against `ce79bb0e` plus the desktop
association/native-picker implementation dated 2026-11-17. Current release:
**v1.2.1**. This is a prioritisation of recorded work, not a claim that old
reports were reproduced or that remote CI ran today.

Domain entries own acceptance criteria and evidence. This index covers all
**34 non-frozen domain index entries**, **7 remaining release items**, and
**28 formally frozen entries**. Non-frozen does not mean scheduled: some
entries require a report, a decision, or a hardware reference. Completed
entries retained for stable links are excluded from those counts.

The [scope ruling](docs/decisions/project-scope.md#the-scope-ruling) still
applies: core correctness is proactive; supported devices are fixed on report.
No new card or peripheral without a named piece of software that needs it.

The user's current order is **file associations, then remaining native file
pickers**. Both implementations are in this working tree; platform acceptance
is still open. Thumbnail work has not been authorised by that order.

## 1. Finish acceptance of the requested desktop work

| Task | What is implemented | What remains to close it |
|---|---|---|
| [UI-010](docs/backlog/ui-debugger.md#ui-010) | Finder alternate rank; Windows per-user Open with registration/removal shipped in the ZIP; Linux MIME/desktop integration already existed. | Open representative 5.25-inch, 3.5-inch and HDV images from installed packages on macOS/Windows/Linux; preserve another application's defaults. Execute the Windows registry test in Windows CI. |
| [UI-002](docs/backlog/ui-debugger.md#ui-002) | Native Browse in SmartPort, Media, Disk II/HDV/Sony, cassette, VSDrive, paste and text-spool panels; PNG/PDF save destinations. | Interactive open/save/cancel on the supported desktops, unavailable-backend manual entry, and error/overwrite checks. Shared fake-backend UI tests do not replace OS-dialog execution. |

## 2. Next core work — recommended order, not started

This order favours data durability and testable correctness over cosmetic
cleanup. Estimates and priority colours in old reports are historical.

| Order | Task | Concrete next action / completion gate |
|---|---|---|
| 1 | [DISPLAY-002](docs/backlog/display.md#display-002) | Add PAL split frame goldens. Separately measure the alleged fallback upload before changing it; ALTCHAR/character-ROM goldens are already done. |
| 2 | [DISPLAY-011](docs/backlog/display.md#display-011) | Obtain Eve Leonard and pin a third Eve frame. Other board-specific residuals stay conditional on a hardware oracle. |
| 3 | [AUDIO-003](docs/backlog/audio.md#audio-003) | Re-record isolated AY and speaker streams through one reference output mapping; compare relative gain/bass with the current 0.30 stereo setting. |
| 4 | [ARCH-003](docs/backlog/architecture-tooling.md#arch-003), [ARCH-009](docs/backlog/architecture-tooling.md#arch-009) | Extend the existing replug/ImGui harness to another card panel and screenshot/`demodMutex` interaction; collect Linux TSan evidence. Do not create a second overlapping concurrency framework. |

**Existing audit residue, distinct from CPU-001:** the non-stock `--speed`
clock-domain issue (CPU-1 in the [round-2 audit](docs/audits/bug-hunt-2026-11-01-round2.md))
is partly fixed. Remaining SSI263, hybrid speech/PCM and serial timing are
explicitly open in the [2026-11-09 follow-up](docs/audits/core-bug-hunt-2026-11-09.md).
Reproduce each at normal/x2/x4 before proposing the next fix; the many already
fixed card clocks are not work remaining.

## 3. Decisions that unlock a bounded task

| Task | Decision / recommended treatment |
|---|---|
| [UI-009](docs/backlog/ui-debugger.md#ui-009) | Decide captured screen vs supplied cover art. Recommendation: saved screenshots and a text fallback, loaded lazily. Do not boot disks during a library scan. |
| [STORAGE-002](docs/backlog/storage.md#storage-002) | Decide whether a slot+bay `/media` API is needed. Secondary-bay boot is deliberately refused today; do not label that safeguard a remaining wrong-disk bug. |
| [ARCH-016](docs/backlog/architecture-tooling.md#arch-016) | Define explicit typed references for ctest names/card keys before extending the documentation guard. Links and task IDs are already checked. |
| [ARCH-011](docs/backlog/architecture-tooling.md#arch-011) | Close Apply's lock as intentional while the worker is stopped, unless ARCH-003 produces a concrete concurrency problem. |
| [NETWORK-001](docs/backlog/network.md#network-001), [NETWORK-007](docs/backlog/network.md#network-007) | Name a use case for native FujiNet phases 2–4 or //c support; otherwise freeze the expansions. Phase 1 and the external relay already work. |
| [NETWORK-004](docs/backlog/network.md#network-004) | Name a live delayed TNFS peer/version and run mount/read/write with bounded failure; otherwise close the additional validation under the Supported rule. Fake-peer timeout tests already exist. |
| [RELEASE-010](docs/backlog/release-1.0.md#release-010) | Decide whether a pre-push check and GL-call guard add value beyond the existing quick Linux CI and include guard; implement a concrete guard or close. |

## 4. Distribution work and platform evidence

These are surviving release obligations, not a claim that v1.0 is unreleased.
Rerun the candidate checklist for each actual release; do not reopen already
implemented CI infrastructure as new feature work.

| Task | Exact remainder |
|---|---|
| [RELEASE-001](docs/backlog/release-1.0.md#release-001) | Record the repository/history media-distribution decision against the current tracked inventory. A history rewrite, if chosen, is a separate destructive operation requiring explicit approval. |
| [RELEASE-002](docs/backlog/release-1.0.md#release-002) | Establish origin and redistribution terms for all three shipped machine/keyboard photographs, or replace them with cleared assets. |
| [RELEASE-012](docs/backlog/release-1.0.md#release-012) | Prepare the corrected ][+ ROM archive, upstream report and aligned source digests; publish only within explicit upload/report authorisation. Upload the repaired Service Disk copy through the same media-maintenance process. Keep the old-dump runtime errata. |
| [RELEASE-008](docs/backlog/release-1.0.md#release-008) | Launch and render the ARM/Pi packages on target hardware, or document the execution limitation. |
| [RELEASE-009](docs/backlog/release-1.0.md#release-009) | Execute the Intel macOS slice and record a boot/render smoke, or document the limitation. A universal-binary structural check is not execution evidence. |
| [RELEASE-011](docs/backlog/release-1.0.md#release-011) | Record a Windows signing policy and clean-machine first launch. macOS notarisation is deferred by an existing decision, not a task to restart. |
| [NETWORK-009](docs/backlog/network.md#network-009) | Run Uthernet I using the built-in NAT on a Windows build. There is already a desktop transport; a new libslirp port is not the remainder. |
| [VALIDATION-005](docs/backlog/validation.md#validation-005) | Run A2 File Cmd 1.0 six times in the GUI on a copied image; reproduce and diagnose the DHGR failure or close the stale report. |
| [VALIDATION-003](docs/backlog/validation.md#validation-003) | Extend protected-title checks beyond boot/entry to gameplay and compare with hardware. Captain/Prince/Bandits boot/gameplay-entry corpus checks already exist. |

## 5. Fidelity questions waiting for an external reference

| Task | Missing evidence |
|---|---|
| [VALIDATION-001](docs/backlog/validation.md#validation-001) | Real //c head movement for PH1→PH3 gaps of 4…512 cycles during motor coast. |
| [VALIDATION-004](docs/backlog/validation.md#validation-004) | DOS 3.1/3.1.1 master boots on a ][+ with 13-sector PROMs, or an identified independent emulator/ROM setup; distinguish disk behavior from an emulator defect. |
| [MEMORY-003](docs/backlog/cpu-memory.md#memory-003) | Real Language Card power-on bank with distinct bytes in both banks; its F8-ROM expansion remains frozen with the original ][ profile. |
| [DISPLAY-001](docs/backlog/display.md#display-001) | Real VidHD SHR 320/640/fill and `$C022`/`$C034` color captures. |
| [AUDIO-011](docs/backlog/audio.md#audio-011) | Real SSI263 inflection/rate/articulation/level recordings; independent Echo+ board/pin validation is a separate residual. |
| [AUDIO-012](docs/backlog/audio.md#audio-012) | Electrical capture across the //e speaker for a known toggle/square wave before fitting a physical response model. |

## 6. Conditional work and low-return maintenance

| Task | Reopen/proceed condition and next action |
|---|---|
| [CPU-001](docs/backlog/cpu-memory.md#cpu-001) | Large optional second CPU core; design only, no implementation. Establish a concrete DIX differential, then per-cycle Harte bus ordering, Klaus and unchanged fast-core identities. This is not the next quick win. |
| [MEMORY-002](docs/backlog/cpu-memory.md#memory-002) | Profile the ROM-condition share first. Proceed only with ≥10% attributable share and ≥3% repeatable workload gain, without >1% repeatable cross-workload regressions; two reorder prototypes were rejected. |
| [ARCH-004](docs/backlog/architecture-tooling.md#arch-004) | Split the remaining Apple2Display responsibility and lower actual slack ceilings. Paint-editor splitting waits for relevant work; Memory and DiskImage WOZ extraction are already done. |
| [ARCH-010](docs/backlog/architecture-tooling.md#arch-010) | First document env vars in `--help`; a full config-precedence unification is a larger separate step, not just that table. |
| [ARCH-013](docs/backlog/architecture-tooling.md#arch-013) | Comment/declaration-only M6502 cleanup when that code is touched, with unchanged Harte/Klaus/display goldens. |
| [ARCH-012](docs/backlog/architecture-tooling.md#arch-012) | Namespace migration one directory at a time when doing related work; avoid an isolated repository-wide rewrite. |
| [AUDIO-006](docs/backlog/audio.md#audio-006) | After ARCH-003, measure actual mutex wait before changing the audio handoff; a lock in source alone is not measured contention. |
| [NETWORK-005](docs/backlog/network.md#network-005) | On a reported delayed FujiNet need, add an atomic in-flight indicator visible without the blocked machine lock in the UI and `/status`; measure responsiveness. Do not call that an asynchronous I/O implementation. |
| [NETWORK-010](docs/backlog/network.md#network-010) | LAN inbound mapping only after a named need. Local `LISTEN` already works; specify interface/host port and forwarding policy first. |
| [DISPLAY-010](docs/backlog/display.md#display-010) | F6 luminance-neutral row dim only on report. F4 is a deliberate choice; F2 overlaps the unscheduled scanline possibilities. |
| [ARCH-008](docs/backlog/architecture-tooling.md#arch-008) | Paint editor test seam only on a concrete report; sprite seam already landed and editors remain frozen. |

## 7. Frozen — no implementation scheduled

All **28** formal frozen entries are listed here. They need the domain's
reopening condition (usually named software or an explicit request), not
merely an attractive estimate. The possibilities sections below are additional
ideas without task IDs; they are not commitments either.

| Domain | Frozen entries |
|---|---|
| Storage | [STORAGE-005](docs/backlog/storage.md#storage-005) multi-partition; [STORAGE-007](docs/backlog/storage.md#storage-007) half-track NIB; [STORAGE-008](docs/backlog/storage.md#storage-008) additional Floppy Emu modes; [STORAGE-010](docs/backlog/storage.md#storage-010) NIB revolution normalisation. |
| Audio/cassette | [AUDIO-004](docs/backlog/audio.md#audio-004) resampling; [AUDIO-005](docs/backlog/audio.md#audio-005) analog stage; [AUDIO-008](docs/backlog/audio.md#audio-008) DAC; [AUDIO-009](docs/backlog/audio.md#audio-009) MIDI; [CASSETTE-001](docs/backlog/cassette.md#cassette-001) enriched cassette playback. |
| Display | [DISPLAY-003](docs/backlog/display.md#display-003) analog composite; [DISPLAY-008](docs/backlog/display.md#display-008) smooth interpolation; [DISPLAY-012](docs/backlog/display.md#display-012) 80-column color phase without an oracle. |
| Cards | [CARDS-002](docs/backlog/cards.md#cards-002) Workstation/LocalTalk extension; [CARDS-006](docs/backlog/cards.md#cards-006) SCSI; [CARDS-007](docs/backlog/cards.md#cards-007) VGA; [CARDS-008](docs/backlog/cards.md#cards-008) UDC; [CARDS-010](docs/backlog/cards.md#cards-010) IWM-level controller. |
| Interface | [UI-004](docs/backlog/ui-debugger.md#ui-004) deeper tutorials; [UI-005](docs/backlog/ui-debugger.md#ui-005) extra voxel polish; [UI-006](docs/backlog/ui-debugger.md#ui-006) unspecified layout refresh; [UI-008](docs/backlog/ui-debugger.md#ui-008) virtual joystick. |
| Network/browser | [NETWORK-008](docs/backlog/network.md#network-008) in-process FujiNet firmware; [NETWORK-012](docs/backlog/network.md#network-012) browser Ethernet transport; [WASM-002](docs/backlog/wasm.md#wasm-002) mobile touch; [WASM-003](docs/backlog/wasm.md#wasm-003) AudioWorklet tuning. |
| Architecture/projects | [ARCH-007](docs/backlog/architecture-tooling.md#arch-007) Z80 cleanup; [PARKED-001](docs/backlog/parked.md#parked-001) further card ports; [PARKED-003](docs/backlog/parked.md#parked-003) bare-metal Raspberry Pi. |

## What is already done

**2026-11-17 core batch:** STORAGE-014 now has deterministic phase-2 write
coverage and fixes the reproduced false host conflict; ARCH-014 adds atomic
clock inspection and mutation-checked fan-out tests; ARCH-015 moves the real
slot composition policy into a headless-tested coordinator. The final locked
host-folder flush after four captures remains an explicit, tested limit.
See [evidence and limits](docs/audits/core-priorities-2026-11-17.md).

Do not schedule these again: heap-backed DiskImage storage (STORAGE-003),
compatibility corpus (VALIDATION-006), input bindings/controllers, browser
file import, screen text/PNG/firmware guidance/CRT preview (UI-011–014),
Porte-Parole and E-Z Color integration. Closed domain anchors remain for
incoming links. The November 16 test-audit fixes include named ctest inventory,
Windows atomic-file coverage, stronger assertions, and the audio buffer ladder.
The round-2 skip detector is already broadened; its old blind spots are not
an instruction to implement another detector.

The [2026-10-28 triage](docs/audits/backlog-triage-2026-10-28.md),
[round-2 hunt](docs/audits/bug-hunt-2026-11-01-round2.md), and
[November 9 consolidation](docs/audits/core-consolidation-2026-11-09.md)
remain historical evidence. Missing media (notably the repaired Service Disk)
are separate from missing implementation. Gates G2–G5 and RELEASE-003–007
mostly call for a fresh candidate run, not new CI systems.

<a id="casso-possibilities"></a>

## Possibilities — what is left of the Casso survey

Recorded 2026-11-06. Casso (MIT, read at `2d899adf`) is another Apple II
emulator — grade **D** evidence like any other, and for most of this list
not evidence at all: these are host-side conveniences and test methods, not
claims about the machine. What POM2 already took from the survey is in
[CHANGELOG.md](CHANGELOG.md) (the built-in NAT, the per-monitor CRT presets,
the SSI263 formant voice, the audio items, the write-integrity policy, the
disk tools).

**None of these is a task.** They have no ID, no domain entry and no
acceptance criterion, and nothing here is scheduled. Taking one means giving
it an ID in its domain file first, and re-reading Casso at a named revision
if the *design* is what is being borrowed. Sizes are estimates made while
reading, not measurements.

### Sound

| Possibility | Size | Note |
|---|---|---|
| Cassette: a more tolerant decoder, AIFF input, automatic speed-up while the tape runs, audio that stays in step under turbo | medium | Four separable pieces; the decoder is the one a real tape recording would exercise. |
| Per-drive pan for the floppy sounds (drive 1 left, drive 2 right) | medium | The stereo bus already pan-places mono sources. |

### Display

| Possibility | Size | Note |
|---|---|---|
| Scanlines with exact-area filtering (the `crt-pi` approach) | small | Overlaps [DISPLAY-010](docs/backlog/display.md#display-010) and the F2 item (cosine → sin²) in `display.md`; decide between them, do not stack them. |

### Disks

| Possibility | Size | Note |
|---|---|---|
| Offer to download the DOS 3.3 System Master and ProDOS, the way the ROMs are offered | medium | **Needs a decision first**: both are Apple's, and nothing copyrighted is hosted or packaged by POM2 (CLAUDE.md § Build & run). Until a source with a stated licence is named this stays a possibility. |

### Input and interface

| Possibility | Size | Note |
|---|---|---|
| Sirius Joyport: centre selector polarity | small | Atari mode with fixed sockets is implemented ([INPUT-007](docs/backlog/input.md#input-007)), validated with Wavy Navy. AN0 follows the manual's prose/BASIC, contradicted by its table/Pascal; verify both physical Atari sockets with a two-controller title before treating centre as validated. Rear Apple analogue sockets remain outside this scope. |
| Drive widget: replace a disk by a click or a drop on the drive, say **why** a disk is locked (notch, header, damaged image, diverted), show the head position | small | The reasons already exist as states (`MediaNotch.h`, `DiskImage::integrityWarning()`, `diverted`). |
| Key repeat as the //e does it (its own delay and rate); none on a ][+, which has a `REPT` key instead | small | Check the repeat timing against the //e keyboard encoder's documentation before porting Casso's numbers. |
| Full screen: the menus come back when the pointer reaches the screen edge | small to medium | Kiosk has its own menu; this is for a full-screen session that is not a kiosk. |
| Notice of a new release — a notice only, no download, no install | medium | A network request at launch: opt-in, and absent from the browser build. |

### Test method

| Possibility | Size | Note |
|---|---|---|
| "Seen by the guest" tests for the host disk tools: the tool writes, the real DOS / ProDOS reads it back | medium | The open item it would settle: `pom2_prodos` volumes have not been read by a real ProDOS. |
| Harte vectors over **every** opcode on every build | small to medium | Today a SHA-256-pinned subset runs (`tests/tomharte_*.manifest`). |
| A single-site mutation script (flip one line, expect one named test to fail) | small | Mutation controls are done by hand today. |
| Corpus titles: Karateka, Choplifter, Lode Runner, Space Quarks, Border Zone, and the six FLUX-track titles | varies | Bandits now supplies one real-FLUX capture in the optional protected corpus (19 FLUX positions read during boot); other captures remain useful. Feeds [VALIDATION-006](docs/backlog/validation.md#validation-006). |

### Seen and not taken

- **Inbound port forwarding for the built-in network** — it opens a listening
  port on the host, which is the opposite of the loopback fence (CLAUDE.md §
  "The guest is inside the loopback perimeter"). Reopen only on a named need,
  together with [NETWORK-010](docs/backlog/network.md#network-010), the
  Uthernet II's own inbound `LISTEN`.

<a id="game-companions"></a>

## Possibilities — game companions

Recorded 2026-11-06, on the same terms as the list above: **not a task**, no
ID and nothing scheduled. A companion is a window that reads one game's RAM
to show what the game knows. It would be the first game-specific code in
POM2, so taking it means deciding first that such code belongs here
([scope decisions](docs/decisions/project-scope.md)) and keeping it out of
the core (`src/companions/<game>/`).

**The map is the point.** The idea worth keeping is an **automap**: a window
that draws each map as the party explores it, with the avatar, a map picker
and per-tile notes. Build it as **one engine fed by a per-game profile**, not
as Nox-only code. A profile is data: how to recognise the game, the named
addresses, and the map's regions. The engine comes in two levels:

- **Trail map**: needs only position, i.e. map, X, Y, floor and facing. It
  draws the squares visited and the walking direction. Any game whose position
  is known gets one.
- **Tile automap**: also needs the game's view window in RAM and its tile
  graphics. The squares seen are drawn with the game's own tiles.

**Apple II games only**, and only their Apple II release. Every address comes
from that release running in POM2, never from a PC, C64 or GOG version of the
same title. GridCartographer-Mapping mixes them: its Ultima IV profile reads
the PC `avatar.exe`, its Ultima I-III profiles target GOG and its Realms of
Antiquity profile the TI-99. None of those apply.

| Possibility | Size | Note |
|---|---|---|
| The automap engine + **Nox Archaist** as its first profile (tile level) | medium | Modelled on [NoxArchaistCompanion](https://github.com/hasseily/NoxArchaistCompanion) (an AppleWin fork, read at `77fe10d`). It has **no licence**: reimplement from the RAM addresses, copy no code. Its region table derives from [GridCartographer-Mapping](https://github.com/hasseily/GridCartographer-Mapping) (MIT), which can be regenerated from there instead. Addresses checked: below. |
| Profiles for **Ultima V** and **Wizardry I** (Apple II releases) | small each, once the engine exists | The MIT GameLink profiles in GridCartographer-Mapping already name their Apple II position bytes: Ultima V on page zero (`$00/$01` X/Y, `$09` floor, `$0A` location, `$0F` facing, `$1F8/$1F9` dungeon position), Wizardry I at `$7594-$759A`. These are grade **E** until read on the running game, the way Nox's were. Both give a trail map at once. Whether Ultima V also has a tile window in RAM is not known. The user has Ultima V. |
| A **Deathlord** profile, Apple II release (tile level) | medium | [Deathlord-Companion](https://github.com/hasseily/Deathlord-Companion) (AGPL-3.0) has an automap: 64×64 maps, and dungeon levels of 16×16 packed four to a map, hence its zoom per quadrant. Reimplement from the addresses, as for Nox. |
| A **RAM search** panel: search for a value, filter by "changed", "unchanged", "increased"…, over main and aux, results sent to the debugger's watches | small to medium | POM2 has none. It is how a new game's map gets a profile, e.g. the Apple II Wizardry V, which has no published one. It is game-agnostic, so it raises no scope question. "Freeze a value" would come with it and stays opt-in. |

Seen around the same tools, **not** for now: a game text log from a hook on
the game's print routine (NAC's conversation log), RetroAchievements (needs
the RA team's approval, and its hardcore mode forbids rewind, state loads and
`POST /mem` writes), and a GameLink bridge to Grid Cartographer (Windows
only). Out of scope: changing a game's rules through CPU hooks
([Deathlord-Relorded](https://github.com/hasseily/Deathlord-Relorded)).

**What is already checked** — 2026-11-06, on a copy of the user's
*Nox Archaist v1.3.7 MB4c* HDV, booted on `--preset iic` with
`iic_expansion_card=mockingboard`, and read through `GET /mem`. The MB4c build
moved none of these addresses:

- Main RAM: map id `$2AF9`, map type `$267D` (`$FF` = combat, never observed),
  avatar X/Y `$6CEB`/`$6CEC`, food and gold `$704F`/`$7051`. The 17×11 view is
  at `$0800` and its visibility mask at `$08BB` (0 = visible). The avatar is
  **not** in the view buffer.
- Aux RAM: party names at `$1600 + k·$80 + $4B`, the "a game is loaded" gate.
  Tiles are 14×16 HGR, 2 bytes per line, LSB on the left: `$7000 + N·$20` for
  N < `$80`, `$8000 + (N−$80)·$80` frame 0 for animated ones. A view rebuilt
  from these matched the screen.
- NAC samples the RAM on the game's `$C000` read: sampled anywhere else, the
  visibility mask is caught half cleared. In POM2 that would be a null hook on
  `Memory`'s `$C0xx` slow path, checked with `pom2_bench` (unchanged hashes,
  no measurable cost; `docs/PERFORMANCE.md` §§ 8.2/8.5).

**Outline if taken**:
1. The engine and the profile format, with no ImGui and pinned by a test on
   synthetic RAM.
2. The `$C000` hook, or whatever sampling point the profile names.
3. A Tools panel (`PanelId::AutoMap`, `MainWindow_AutoMap.cpp`) that colours
   tiles with POM2's own HGR decoder.

Discovered maps are saved under `userConfigDir()`, **one set per game and per
party** rather than NAC's single global set, so two games do not mix. The game
and its version are told from RAM, not from the file name as NAC does.

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
