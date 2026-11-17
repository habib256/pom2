# Backlog — Possibilities

[Planning index](../../TODO.md) · [Scope decisions](../decisions/project-scope.md)

Moved out of `TODO.md` on 2026-11-17 so that the index holds only the action
queue. **Nothing on this page is a task**: no ID, no domain entry, no
acceptance criterion, nothing scheduled. Taking one means giving it an ID in
its domain file first.

<a id="casso-possibilities"></a>

## Possibilities — what is left of the Casso survey

Recorded 2026-11-06. Casso (MIT, read at `2d899adf`) is another Apple II
emulator — grade **D** evidence like any other, and for most of this list
not evidence at all: these are host-side conveniences and test methods, not
claims about the machine. What POM2 already took from the survey is in
[CHANGELOG.md](../../CHANGELOG.md) (the built-in NAT, the per-monitor CRT presets,
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
| Scanlines with exact-area filtering (the `crt-pi` approach) | small | Overlaps [DISPLAY-010](display.md#display-010) and the F2 item (cosine → sin²) in `display.md`; decide between them, do not stack them. |

### Disks

| Possibility | Size | Note |
|---|---|---|
| Offer to download the DOS 3.3 System Master and ProDOS, the way the ROMs are offered | medium | **Needs a decision first**: both are Apple's, and nothing copyrighted is hosted or packaged by POM2 (CLAUDE.md § Build & run). Until a source with a stated licence is named this stays a possibility. |

### Input and interface

| Possibility | Size | Note |
|---|---|---|
| Sirius Joyport: centre selector polarity | small | Atari mode with fixed sockets is implemented ([INPUT-007](input.md#input-007)), validated with Wavy Navy. AN0 follows the manual's prose/BASIC, contradicted by its table/Pascal; verify both physical Atari sockets with a two-controller title before treating centre as validated. Rear Apple analogue sockets remain outside this scope. |
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
| Corpus titles: Karateka, Choplifter, Lode Runner, Space Quarks, Border Zone, and the six FLUX-track titles | varies | Bandits now supplies one real-FLUX capture in the optional protected corpus (19 FLUX positions read during boot); other captures remain useful. Feeds [VALIDATION-006](validation.md#validation-006). |

### Seen and not taken

- **Inbound port forwarding for the built-in network** — it opens a listening
  port on the host, which is the opposite of the loopback fence (CLAUDE.md §
  "The guest is inside the loopback perimeter"). Reopen only on a named need,
  together with [NETWORK-010](network.md#network-010), the
  Uthernet II's own inbound `LISTEN`.

<a id="game-companions"></a>

## Possibilities — game companions

Recorded 2026-11-06, on the same terms as the list above: **not a task**, no
ID and nothing scheduled. A companion is a window that reads one game's RAM
to show what the game knows. It would be the first game-specific code in
POM2, so taking it means deciding first that such code belongs here
([scope decisions](../decisions/project-scope.md)) and keeping it out of
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
