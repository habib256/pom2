# POM2 — TODO

Action queue reconciled **2026-11-23**, after the three-way round against MAME
and AppleWin ([protected WOZ](docs/audits/protected-woz-three-way-2026-11-23.md))
and the [ROM gap audit](docs/audits/rom-gaps-vs-mame-2026-11-23.md). Current
release: **v1.2.2**. The index covers **46 active domain entries**, **11
release items** and **30 frozen entries**. Each task's acceptance criterion
and evidence live in its domain entry, not here. The Apple IIgs is not
POM2's: it is the separate **pom2gs** program
([scope](docs/decisions/project-scope.md#out-of-scope)).

The [scope ruling](docs/decisions/project-scope.md#the-scope-ruling) still
applies: core correctness is proactive; supported devices are fixed on report;
no new card without a named piece of software or an explicit request.
How a task moves and what must not be scheduled again:
[docs/backlog/README.md](docs/backlog/README.md). Ideas that are not tasks:
[docs/backlog/possibilities.md](docs/backlog/possibilities.md).

## 0. Chosen direction — in this order

**Done: [UI-022](docs/backlog/ui-debugger.md#ui-022) — the memory heatmap**
(owner, 2026-11-27; GROUiK's request, fenarinarsa's tool from French
Touch's AppleWin fork), landed 2026-11-28: FT's map, rails, colours (its exact
palette as an option) and Monitoring-card bars ($C066-$C069), every bank of
the machine (ROM & I/O, both $D000 banks, RamWorks, DMA), a max-pooled map
with a minimap, zoom to the byte, a status line with the byte's screen
position, Go to with history, labels on the map, inspector, range
breakpoints by bank, Freeze, PNG. Left: the map across the rewind history.

**Silicon parity: POM2 runs everything the iron runs** (owner, 2026-11-23).
On 37 protected disks POM2 now boots every title MAME and AppleWin boot. It is
the only one of the three that boots both The Newsroom and the FLUX images,
and Lady Tut on a ][. One at a time, each closed by its own acceptance test:

| Order | Task | What closes it |
|---|---|---|
| 1 | [AUDIO-015](docs/backlog/audio.md#audio-015) — Votrax SC-01A | A Mockingboard Sound/Speech I title speaks and its A/R handshake advances. |
| 2 | [VALIDATION-007](docs/backlog/validation.md#validation-007) — wide three-way corpus | ≥ 200 titles on ][+, //e, //c and PAL with a pass count per emulator; each POM2-only failure filed. |

Done on 2026-11-24: Glutton, the one protected title POM2 alone failed,
boots since the platter's speed drifts (STORAGE-018's first half; the
true-clock half is frozen again until a title needs it).

The developer-tooling track chosen on 2026-11-18 comes next:
[UI-015](docs/backlog/ui-debugger.md#ui-015) MCP + debugger verbs, then
[UI-018](docs/backlog/ui-debugger.md#ui-018) DAP for VS Code (needs UI-015).
After them, in any order: [UI-016](docs/backlog/ui-debugger.md#ui-016)
expression conditions / access counters / code-data log, and
[UI-019](docs/backlog/ui-debugger.md#ui-019) reverse debugging (design first).

## 1. Doable alone, now

**Reach and users** — each small, independent:

| Task | Concrete next action |
|---|---|
| [AUDIO-009](docs/backlog/audio.md#audio-009) | Passport MIDI is implemented: run Ultima V with the card in slot 2 and record a song; then run the WinMM and ALSA backends (both compile in CI). |
| [RELEASE-013](docs/backlog/release.md#release-013) | Personal Homebrew tap, AUR `pom2-bin`, Scoop and winget manifests. |
| [STORAGE-017](docs/backlog/storage.md#storage-017) | Left: A2R 3 checked on a real capture (A2R 2/3, ShrinkIt, DiskCopy and multi-disk archives shipped 2026-11-24). |
| [UI-020](docs/backlog/ui-debugger.md#ui-020) | Speak what changes on screen, from the existing screen-text decoder. |
| [UI-023](docs/backlog/ui-debugger.md#ui-023) | Idea (2026-11-28): a disk view — the head's position on the platter and what it reads (fields, sectors, reads/writes fading); design first. |
| [UI-009](docs/backlog/ui-debugger.md#ui-009) | Decided 2026-11-25: hover preview from libretro's Apple II thumbnails. First the woz-a-day DAT (libretro-database), then the thumbnail set; a disk with no picture shows nothing (no fallback capture). |
| [CARDS-017](docs/backlog/cards.md#cards-017) | Hayes AT layer on the SSC for ProTERM BBS dialling. |

**ROMs MAME has and POM2 lacks** ([audit](docs/audits/rom-gaps-vs-mame-2026-11-23.md)), each from
RetroBIOS at the pinned commit; a new catalogue entry also goes into `pom2_firm.zip`:

| Task | Concrete next action |
|---|---|
| [DISPLAY-013](docs/backlog/display.md#display-013) | The nine Videoterm character sets (French among them): catalogue + a setting per socket. |
| [INPUT-008](docs/backlog/input.md#input-008) | The //e and //c encoder ROMs as the test oracle for the key map. |
| [DISPLAY-014](docs/backlog/display.md#display-014) | Serve Apple's 342-0133-A instead of the clone file; settle the `$60-$7F` question. |
| [CARDS-019](docs/backlog/cards.md#cards-019) | Explain why AppleWin's parallel printer PROM differs from MAME's, then run the real one. |

**Correctness and internals** — recommended order (durability and testable
correctness before cleanup):

| Order | Task | Concrete next action / completion gate |
|---|---|---|
| 1 | [AUDIO-003](docs/backlog/audio.md#audio-003) | Re-record isolated AY and speaker streams through one output mapping; compare with the 0.30 stereo setting. |
| 2 | [ARCH-003](docs/backlog/architecture-tooling.md#arch-003), [ARCH-009](docs/backlog/architecture-tooling.md#arch-009) | Extend the existing replug/ImGui harness to another card panel and screenshot/`demodMutex`; collect Linux TSan evidence. No second concurrency framework. |
| 3 | [ARCH-016](docs/backlog/architecture-tooling.md#arch-016) | Typed references for ctest names/card keys before extending the documentation guard. |
| 4 | [ARCH-010](docs/backlog/architecture-tooling.md#arch-010) | Env vars are in `--help` (2026-11-19); next: design one precedence chain. |
| 5 | [ARCH-004](docs/backlog/architecture-tooling.md#arch-004) | Split the remaining Apple2Display responsibility and lower slack ceilings (`Memory.cpp` and `HgrPaintEditor.cpp` are over theirs today). |

The non-stock `--speed` clock-domain residue (SSI263, hybrid speech/PCM,
serial timing — [2026-11-09 follow-up](docs/audits/core-bug-hunt-2026-11-09.md))
stays open: reproduce at normal/x2/x4 before proposing a fix.

**Gated on a measurement or on other work:**

| Task | Gate and next action |
|---|---|
| [CPU-001](docs/backlog/cpu-memory.md#cpu-001) | Optional cycle-stepped core: design only. Harte's per-cycle bus data is the ready oracle; establish a concrete DIX differential first. |
| [MEMORY-002](docs/backlog/cpu-memory.md#memory-002) | Profile the ROM-condition share; proceed only with ≥10 % share and ≥3 % repeatable gain. |
| [AUDIO-006](docs/backlog/audio.md#audio-006) | After ARCH-003, measure actual mutex wait before changing the audio handoff. |
| [ARCH-013](docs/backlog/architecture-tooling.md#arch-013), [ARCH-012](docs/backlog/architecture-tooling.md#arch-012) | Only alongside related work: M6502 comment cleanup with unchanged goldens; namespace migration one directory at a time. |

## 2. Waiting for the owner

**Uploads and authorisations:**

| Task | What is needed |
|---|---|
| [RELEASE-012](docs/backlog/release.md#release-012) | POM2's own archives serve the corrected `apple2p.rom` since 2026-11-19; only the RetroBIOS report remains, deferred by the owner. |
| [RELEASE-016](docs/backlog/release.md#release-016) | libretro buildbot PR and RetroAchievements hardcore request (public steps); the shader presets are done. |

**Decisions:**

| Task | Decision / recommended treatment |
|---|---|
| [RELEASE-011](docs/backlog/release.md#release-011) | Windows signing: **SignPath Foundation** (free for open source) is the recommended route. macOS notarisation stays deferred. |
| [RELEASE-014](docs/backlog/release.md#release-014) | Whether POM2 offers third-party disks (Total Replay) by download at all. |
| [STORAGE-002](docs/backlog/storage.md#storage-002) | Whether a slot+bay `/media` API is needed (fold into UI-015's verbs if so). Secondary-bay boot refusal is a safeguard, not a bug. |
| [ARCH-011](docs/backlog/architecture-tooling.md#arch-011) | Close Apply's lock as intentional unless ARCH-003 finds a concrete problem. |
| [NETWORK-001](docs/backlog/network.md#network-001), [NETWORK-007](docs/backlog/network.md#network-007) | Name a use case for native FujiNet phases 2–4 or //c support, or freeze them. |
| [NETWORK-004](docs/backlog/network.md#network-004) | Name a live delayed TNFS peer, or close the extra validation under the Supported rule. |
| [RELEASE-010](docs/backlog/release.md#release-010) | A pre-push check and GL-call guard beyond the existing CI guards — implement one concrete guard or close. |

Set aside by the owner and not proposed as next steps:
[RELEASE-001](docs/backlog/release.md#release-001) (history distribution),
[RELEASE-002](docs/backlog/release.md#release-002) (photograph terms).

## 3. Waiting for a machine or a real measurement

### Another platform

Release obligations: rerun the candidate checklist for each real release.

| Task | What remains |
|---|---|
| [UI-010](docs/backlog/ui-debugger.md#ui-010) | Open 5.25", 3.5" and HDV images from installed packages on macOS/Windows/Linux without stealing other defaults; confirm the Windows registry test ran in CI. |
| [UI-002](docs/backlog/ui-debugger.md#ui-002) | Interactive open/save/cancel on each desktop, manual-entry fallback, overwrite checks. |
| [NETWORK-009](docs/backlog/network.md#network-009) | Uthernet I over the built-in NAT on a Windows build. |
| [RELEASE-008](docs/backlog/release.md#release-008) | Launch the ARM/Pi packages on target hardware, or document the limitation. |
| [RELEASE-009](docs/backlog/release.md#release-009) | Execute the Intel macOS slice (boot/render smoke), or document the limitation. |

### A real Apple II, card or recording

| Task | Missing evidence |
|---|---|
| [VALIDATION-001](docs/backlog/validation.md#validation-001) | Real //c head movement for PH1→PH3 gaps of 4…512 cycles during motor coast. |
| [VALIDATION-003](docs/backlog/validation.md#validation-003) | Gameplay-level protected checks against hardware (Captain, Prince, Bandits, Phantasie, Prisoner 2, Mr. Do, Lady Tut, Cyclod boot/entry exist). |
| [VALIDATION-004](docs/backlog/validation.md#validation-004) | DOS 3.1/3.1.1 on a ][+ with 13-sector PROMs, or an independent setup. |
| [MEMORY-003](docs/backlog/cpu-memory.md#memory-003) | Real Language Card power-on bank with distinct bytes in both banks. |
| [DISPLAY-001](docs/backlog/display.md#display-001) | Real VidHD SHR 320/640/fill and `$C022`/`$C034` captures (Total Replay 6.x's SHR art makes it more pressing). |
| [DISPLAY-011](docs/backlog/display.md#display-011) | Eve Leonard, for a third Eve frame. |
| [DISPLAY-015](docs/backlog/display.md#display-015) | Colour-burst constants: a scope on CLRGATE', PAL monitor photos of 2/3-state and isolated burst-less lines. |
| [AUDIO-011](docs/backlog/audio.md#audio-011) | Real SSI263 inflection/rate/articulation/level recordings. |
| [AUDIO-012](docs/backlog/audio.md#audio-012) | Electrical capture across the //e speaker before any physical model. |

## 4. Waiting for a report or a named need

| Task | Reopen condition |
|---|---|
| [NETWORK-005](docs/backlog/network.md#network-005) | A reported delayed FujiNet need → an atomic in-flight indicator outside the machine lock. |
| [NETWORK-010](docs/backlog/network.md#network-010) | A named need for LAN inbound mapping. |
| [DISPLAY-010](docs/backlog/display.md#display-010) | F6 luminance-neutral row dim, on report. |
| [ARCH-008](docs/backlog/architecture-tooling.md#arch-008) | Paint editor test seam, on a concrete report. |

## 5. Frozen — no implementation scheduled

All **30** formal frozen entries. Each needs its domain's reopening condition
(named software or an explicit request), not an attractive estimate.

| Domain | Frozen entries |
|---|---|
| Storage | [STORAGE-005](docs/backlog/storage.md#storage-005) multi-partition; [STORAGE-007](docs/backlog/storage.md#storage-007) half-track NIB; [STORAGE-008](docs/backlog/storage.md#storage-008) Floppy Emu modes; [STORAGE-010](docs/backlog/storage.md#storage-010) NIB revolution; [STORAGE-018](docs/backlog/storage.md#storage-018) bit tracks on the true LSS clock (the speed drift shipped). |
| Audio/cassette | [AUDIO-004](docs/backlog/audio.md#audio-004) resampling; [AUDIO-005](docs/backlog/audio.md#audio-005) analog stage; [AUDIO-008](docs/backlog/audio.md#audio-008) DAC; [AUDIO-014](docs/backlog/audio.md#audio-014) AY-3-8913 voicing; [CASSETTE-001](docs/backlog/cassette.md#cassette-001) enriched playback. |
| Display | [DISPLAY-003](docs/backlog/display.md#display-003) analog composite; [DISPLAY-008](docs/backlog/display.md#display-008) smooth interpolation; [DISPLAY-012](docs/backlog/display.md#display-012) 80-column colour phase. |
| Cards | [CARDS-002](docs/backlog/cards.md#cards-002) Workstation/LocalTalk; [CARDS-006](docs/backlog/cards.md#cards-006) SCSI (MAME reference since 2026-08, no named software); [CARDS-007](docs/backlog/cards.md#cards-007) VGA; [CARDS-008](docs/backlog/cards.md#cards-008) UDC; [CARDS-010](docs/backlog/cards.md#cards-010) IWM-level controller; [CARDS-018](docs/backlog/cards.md#cards-018) SuperSprite (disks + manual found 2026-11-19; frozen on value, the overlay is display work). |
| Interface | [UI-004](docs/backlog/ui-debugger.md#ui-004) tutorials; [UI-005](docs/backlog/ui-debugger.md#ui-005) voxel polish; [UI-006](docs/backlog/ui-debugger.md#ui-006) layout refresh; [UI-008](docs/backlog/ui-debugger.md#ui-008) virtual joystick. |
| Network/browser | [NETWORK-008](docs/backlog/network.md#network-008) in-process FujiNet; [NETWORK-012](docs/backlog/network.md#network-012) browser Ethernet; [WASM-002](docs/backlog/wasm.md#wasm-002) mobile touch; [WASM-003](docs/backlog/wasm.md#wasm-003) AudioWorklet. |
| Architecture/projects | [ARCH-007](docs/backlog/architecture-tooling.md#arch-007) Z80 cleanup; [PARKED-001](docs/backlog/parked.md#parked-001) further card ports (PC Transporter has a MAME reference since 2026-09, no named software); [PARKED-003](docs/backlog/parked.md#parked-003) bare-metal Raspberry Pi. |

<a id="backlog"></a>

## Backlog by domain

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
| Release and distribution | [release.md](docs/backlog/release.md) |
| Parked projects | [parked.md](docs/backlog/parked.md) |
| How the backlog works · already done · evidence | [README.md](docs/backlog/README.md) |
| Possibilities (not tasks) | [possibilities.md](docs/backlog/possibilities.md) |
