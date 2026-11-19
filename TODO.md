# POM2 — TODO

Action queue reconciled **2026-11-18** (after the research of the same day:
competing emulators, hardware references, developer tooling, distribution,
community needs). Current release: **v1.2.1**. The index covers **48 active
domain entries**, **10 release items** and **30 frozen entries**; each task's
acceptance criterion and evidence live in its domain entry, not here.

The [scope ruling](docs/decisions/project-scope.md#the-scope-ruling) still
applies: core correctness is proactive; supported devices are fixed on report;
no new card without a named piece of software or an explicit request.
How a task moves and what must not be scheduled again:
[docs/backlog/README.md](docs/backlog/README.md). Ideas that are not tasks:
[docs/backlog/possibilities.md](docs/backlog/possibilities.md).

## 0. Chosen direction — in this order

Chosen on 2026-11-18. The research found POM2 ahead of AppleWin, MAME and
izapple2 on the emulated machine and behind them on the host side; this
track makes it the best-tooled Apple II emulator for developers and the
demoscene. One at a time, each closed by its own acceptance test.

| Order | Task | What closes it |
|---|---|---|
| 1 | [UI-015](docs/backlog/ui-debugger.md#ui-015) — MCP + debugger verbs | Claude Code drives `/mcp`: breakpoint, run-to, registers, wait for screen text — in a ctest. Read the current MCP revision first. |
| 2 | [AUDIO-013](docs/backlog/audio.md#audio-013) — VGM/YM + WAV, then [UI-021](docs/backlog/ui-debugger.md#ui-021) video | A VGM round-trips the exact AY writes; captures are frame-identical to `pom2_corpus --frames`. |
| 3 | [UI-018](docs/backlog/ui-debugger.md#ui-018) — DAP for VS Code | Needs UI-015. A cc65 program stops on a `.s` breakpoint and steps by source line, both banks. |

Next on the same track, any order: [UI-017](docs/backlog/ui-debugger.md#ui-017)
trace file + callgrind, [ARCH-017](docs/backlog/architecture-tooling.md#arch-017)
paravirtual test port, [UI-016](docs/backlog/ui-debugger.md#ui-016)
expression conditions / access counters / code-data log,
[UI-019](docs/backlog/ui-debugger.md#ui-019) reverse debugging (design first).

## 1. Doable alone, now

**Reach and users** — each small, independent:

| Task | Concrete next action |
|---|---|
| [AUDIO-009](docs/backlog/audio.md#audio-009) | Passport MIDI is implemented: run Ultima V with the card in slot 2 and record a song; then run the WinMM and ALSA backends (both compile in CI). |
| [WASM-004](docs/backlog/wasm.md#wasm-004) | Per-drive URL parameters + "copy link to this setup" (two-disk games, DIX). |
| [RELEASE-013](docs/backlog/release.md#release-013) | Personal Homebrew tap, AUR `pom2-bin`, Scoop and winget manifests. |
| [STORAGE-017](docs/backlog/storage.md#storage-017) | A2R 3.x import into the FLUX engine; ShrinkIt; multi-disk zips as a swap list. |
| [WASM-005](docs/backlog/wasm.md#wasm-005) | `?embed=1` + `postMessage` events. |
| [UI-020](docs/backlog/ui-debugger.md#ui-020) | Speak what changes on screen, from the existing screen-text decoder. |
| [CARDS-017](docs/backlog/cards.md#cards-017) | Hayes AT layer on the SSC for ProTERM BBS dialling. |

**Correctness and internals** — recommended order (durability and testable
correctness before cleanup):

| Order | Task | Concrete next action / completion gate |
|---|---|---|
| 1 | [DISPLAY-002](docs/backlog/display.md#display-002) | Measure the alleged OE-GPU fallback upload before changing it. |
| 2 | [AUDIO-003](docs/backlog/audio.md#audio-003) | Re-record isolated AY and speaker streams through one output mapping; compare with the 0.30 stereo setting. |
| 3 | [ARCH-003](docs/backlog/architecture-tooling.md#arch-003), [ARCH-009](docs/backlog/architecture-tooling.md#arch-009) | Extend the existing replug/ImGui harness to another card panel and screenshot/`demodMutex`; collect Linux TSan evidence. No second concurrency framework. |
| 4 | [VALIDATION-005](docs/backlog/validation.md#validation-005) | Run A2 File Cmd 1.0 six times in the GUI on a copied image; diagnose the DHGR failure or close the stale report. |
| 5 | [ARCH-016](docs/backlog/architecture-tooling.md#arch-016) | Typed references for ctest names/card keys before extending the documentation guard. |
| 6 | [ARCH-010](docs/backlog/architecture-tooling.md#arch-010) | Document env vars in `--help` first; config-precedence unification is a separate step. |
| 7 | [ARCH-004](docs/backlog/architecture-tooling.md#arch-004) | Split the remaining Apple2Display responsibility and lower slack ceilings (`Memory.cpp` and `HgrPaintEditor.cpp` are over theirs today). |

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
| [RELEASE-015](docs/backlog/release.md#release-015) | **Update `pom2_media_202611`** with deater's seven `.dsk` (paths and SHA-256 in the entry), then add them to `tools/test_media.sha256` in the same commit; tag `deater-hectic` `ci`. |
| [RELEASE-012](docs/backlog/release.md#release-012) | Corrected `apple2p.rom` in `pom2_firm.zip` + RetroBIOS report; the repaired Service Disk rides in the RELEASE-015 upload. |

**Decisions:**

| Task | Decision / recommended treatment |
|---|---|
| [RELEASE-011](docs/backlog/release.md#release-011) | Windows signing: **SignPath Foundation** (free for open source) is the recommended route. macOS notarisation stays deferred. |
| [RELEASE-014](docs/backlog/release.md#release-014) | Whether POM2 offers third-party disks (Total Replay) by download at all. |
| [UI-009](docs/backlog/ui-debugger.md#ui-009) | Captured screen vs cover art. Recommendation: saved screenshots, text fallback, lazy load; WASM-004's catalogue would reuse it. |
| [STORAGE-002](docs/backlog/storage.md#storage-002) | Whether a slot+bay `/media` API is needed (fold into UI-015's verbs if so). Secondary-bay boot refusal is a safeguard, not a bug. |
| [ARCH-011](docs/backlog/architecture-tooling.md#arch-011) | Close Apply's lock as intentional unless ARCH-003 finds a concrete problem. |
| [NETWORK-001](docs/backlog/network.md#network-001), [NETWORK-007](docs/backlog/network.md#network-007) | Name a use case for native FujiNet phases 2–4 or //c support, or freeze them. |
| [NETWORK-004](docs/backlog/network.md#network-004) | Name a live delayed TNFS peer, or close the extra validation under the Supported rule. |
| [RELEASE-010](docs/backlog/release.md#release-010) | A pre-push check and GL-call guard beyond the existing CI guards — implement one concrete guard or close. |
| A libretro core | Needs a scope-ruling row first ([possibilities](docs/backlog/possibilities.md)). |

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
| [VALIDATION-003](docs/backlog/validation.md#validation-003) | Gameplay-level protected checks against hardware (Captain, Prince, Bandits, Phantasie, Prisoner 2 boot/entry exist). |
| [VALIDATION-004](docs/backlog/validation.md#validation-004) | DOS 3.1/3.1.1 on a ][+ with 13-sector PROMs, or an independent setup. |
| [MEMORY-003](docs/backlog/cpu-memory.md#memory-003) | Real Language Card power-on bank with distinct bytes in both banks. |
| [DISPLAY-001](docs/backlog/display.md#display-001) | Real VidHD SHR 320/640/fill and `$C022`/`$C034` captures (Total Replay 6.x's SHR art makes it more pressing). |
| [DISPLAY-011](docs/backlog/display.md#display-011) | Eve Leonard, for a third Eve frame. |
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
| Storage | [STORAGE-005](docs/backlog/storage.md#storage-005) multi-partition; [STORAGE-007](docs/backlog/storage.md#storage-007) half-track NIB; [STORAGE-008](docs/backlog/storage.md#storage-008) Floppy Emu modes; [STORAGE-010](docs/backlog/storage.md#storage-010) NIB revolution; [STORAGE-018](docs/backlog/storage.md#storage-018) 8.17-cycle re-basing (needs a re-bless). |
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
