# POM2 — TODO

Action queue reconciled **2026-11-17**, against `ce79bb0e` plus the desktop
association/native-picker implementation merged in `b48e230a`. Current
release: **v1.2.1**. This is a prioritisation of recorded work, not a claim
that old reports were reproduced.

**Sorted by what unblocks each task** — what can be done alone now, what waits
for a decision, what waits for a machine or a real measurement, what waits for
a report. Domain entries own acceptance criteria and evidence. This index
covers all **34 non-frozen domain index entries**, **7 remaining release
items**, and **28 formally frozen entries**. Non-frozen does not mean
scheduled. Completed entries retained for stable links are excluded from
those counts.

The [scope ruling](docs/decisions/project-scope.md#the-scope-ruling) still
applies: core correctness is proactive; supported devices are fixed on report.
No new card or peripheral without a named piece of software that needs it.

How a task moves, what is already done and must not be scheduled again, and
the evidence documents: [docs/backlog/README.md](docs/backlog/README.md).
Ideas that are not tasks (the Casso survey, game companions):
[docs/backlog/possibilities.md](docs/backlog/possibilities.md).

## 1. Doable alone, now

Nothing here waits on anyone. The first table is in recommended order: it
favours data durability and testable correctness over cosmetic cleanup.
Estimates and priority colours in old reports are historical.

| Order | Task | Concrete next action / completion gate |
|---|---|---|
| 1 | [DISPLAY-002](docs/backlog/display.md#display-002) | Add PAL split frame goldens. Separately measure the alleged fallback upload before changing it; ALTCHAR/character-ROM goldens are already done. |
| 2 | [AUDIO-003](docs/backlog/audio.md#audio-003) | Re-record isolated AY and speaker streams through one reference output mapping; compare relative gain/bass with the current 0.30 stereo setting. |
| 3 | [ARCH-003](docs/backlog/architecture-tooling.md#arch-003), [ARCH-009](docs/backlog/architecture-tooling.md#arch-009) | Extend the existing replug/ImGui harness to another card panel and screenshot/`demodMutex` interaction; collect Linux TSan evidence. Do not create a second overlapping concurrency framework. |
| 4 | [VALIDATION-005](docs/backlog/validation.md#validation-005) | Run A2 File Cmd 1.0 six times in the GUI on a copied image; reproduce and diagnose the DHGR failure or close the stale report. |
| 5 | [ARCH-016](docs/backlog/architecture-tooling.md#arch-016) | Define explicit typed references for ctest names/card keys before extending the documentation guard. Links and task IDs are already checked. |
| 6 | [ARCH-010](docs/backlog/architecture-tooling.md#arch-010) | First document env vars in `--help`; a full config-precedence unification is a larger separate step, not just that table. |
| 7 | [ARCH-004](docs/backlog/architecture-tooling.md#arch-004) | Split the remaining Apple2Display responsibility and lower actual slack ceilings. Paint-editor splitting waits for relevant work; Memory and DiskImage WOZ extraction are already done. |

**Existing audit residue, distinct from CPU-001:** the non-stock `--speed`
clock-domain issue (CPU-1 in the [round-2 audit](docs/audits/bug-hunt-2026-11-01-round2.md))
is partly fixed. Remaining SSI263, hybrid speech/PCM and serial timing are
explicitly open in the [2026-11-09 follow-up](docs/audits/core-bug-hunt-2026-11-09.md).
Reproduce each at normal/x2/x4 before proposing the next fix; the many already
fixed card clocks are not work remaining.

Doable alone, but gated on a measurement or on other work:

| Task | Gate and next action |
|---|---|
| [CPU-001](docs/backlog/cpu-memory.md#cpu-001) | Large optional second CPU core; design only, no implementation. Establish a concrete DIX differential, then per-cycle Harte bus ordering, Klaus and unchanged fast-core identities. This is not the next quick win. |
| [MEMORY-002](docs/backlog/cpu-memory.md#memory-002) | Profile the ROM-condition share first. Proceed only with ≥10% attributable share and ≥3% repeatable workload gain, without >1% repeatable cross-workload regressions; two reorder prototypes were rejected. |
| [AUDIO-006](docs/backlog/audio.md#audio-006) | After ARCH-003, measure actual mutex wait before changing the audio handoff; a lock in source alone is not measured contention. |
| [ARCH-013](docs/backlog/architecture-tooling.md#arch-013) | Comment/declaration-only M6502 cleanup when that code is touched, with unchanged Harte/Klaus/display goldens. |
| [ARCH-012](docs/backlog/architecture-tooling.md#arch-012) | Namespace migration one directory at a time when doing related work; avoid an isolated repository-wide rewrite. |

## 2. Waiting for a decision from the user

| Task | Decision / recommended treatment |
|---|---|
| [UI-009](docs/backlog/ui-debugger.md#ui-009) | Decide captured screen vs supplied cover art. Recommendation: saved screenshots and a text fallback, loaded lazily. Do not boot disks during a library scan. Thumbnail work has not been authorised yet. |
| [STORAGE-002](docs/backlog/storage.md#storage-002) | Decide whether a slot+bay `/media` API is needed. Secondary-bay boot is deliberately refused today; do not label that safeguard a remaining wrong-disk bug. |
| [ARCH-011](docs/backlog/architecture-tooling.md#arch-011) | Close Apply's lock as intentional while the worker is stopped, unless ARCH-003 produces a concrete concurrency problem. |
| [NETWORK-001](docs/backlog/network.md#network-001), [NETWORK-007](docs/backlog/network.md#network-007) | Name a use case for native FujiNet phases 2–4 or //c support; otherwise freeze the expansions. Phase 1 and the external relay already work. |
| [NETWORK-004](docs/backlog/network.md#network-004) | Name a live delayed TNFS peer/version and run mount/read/write with bounded failure; otherwise close the additional validation under the Supported rule. Fake-peer timeout tests already exist. |
| [RELEASE-010](docs/backlog/release-1.0.md#release-010) | Decide whether a pre-push check and GL-call guard add value beyond the existing quick Linux CI and include guard; implement a concrete guard or close. |
| [RELEASE-001](docs/backlog/release-1.0.md#release-001) | Record the repository/history media-distribution decision against the current tracked inventory. A history rewrite, if chosen, is a separate destructive operation requiring explicit approval. |
| [RELEASE-002](docs/backlog/release-1.0.md#release-002) | Establish origin and redistribution terms for all three shipped machine/keyboard photographs, or replace them with cleared assets. |
| [RELEASE-011](docs/backlog/release-1.0.md#release-011) | Record a Windows signing policy; the clean-machine first launch then belongs to the Windows session below. macOS notarisation is deferred by an existing decision, not a task to restart. |
| [RELEASE-012](docs/backlog/release-1.0.md#release-012) | Preparing the corrected ][+ ROM archive, upstream report and aligned source digests can be done alone; **publishing** needs explicit upload/report authorisation. Upload the repaired Service Disk copy through the same media-maintenance process. Keep the old-dump runtime errata. |

## 3. Waiting for a machine or a real measurement

### Another platform to run on

These are surviving release obligations, not a claim that v1.0 is unreleased.
Rerun the candidate checklist for each actual release; do not reopen already
implemented CI infrastructure as new feature work.

| Task | What is implemented | What remains to close it |
|---|---|---|
| [UI-010](docs/backlog/ui-debugger.md#ui-010) | Finder alternate rank; Windows per-user Open with registration/removal shipped in the ZIP; Linux MIME/desktop integration already existed. | Open representative 5.25-inch, 3.5-inch and HDV images from installed packages on macOS/Windows/Linux; preserve another application's defaults. Confirm the Windows registry test ran in Windows CI. |
| [UI-002](docs/backlog/ui-debugger.md#ui-002) | Native Browse in SmartPort, Media, Disk II/HDV/Sony, cassette, VSDrive, paste and text-spool panels; PNG/PDF save destinations. | Interactive open/save/cancel on the supported desktops, unavailable-backend manual entry, and error/overwrite checks. Shared fake-backend UI tests do not replace OS-dialog execution. |
| [NETWORK-009](docs/backlog/network.md#network-009) | The built-in NAT is a desktop transport already. | Run Uthernet I using it on a Windows build. A new libslirp port is not the remainder. |
| [RELEASE-008](docs/backlog/release-1.0.md#release-008) | ARM/Pi packages are built. | Launch and render them on target hardware, or document the execution limitation. |
| [RELEASE-009](docs/backlog/release-1.0.md#release-009) | The universal binary passes its structural check. | Execute the Intel macOS slice and record a boot/render smoke, or document the limitation. A structural check is not execution evidence. |

### A real Apple II, card or recording

| Task | Missing evidence |
|---|---|
| [VALIDATION-001](docs/backlog/validation.md#validation-001) | Real //c head movement for PH1→PH3 gaps of 4…512 cycles during motor coast. |
| [VALIDATION-003](docs/backlog/validation.md#validation-003) | Extend protected-title checks beyond boot/entry to gameplay and compare with hardware. Captain/Prince/Bandits boot/gameplay-entry corpus checks already exist. |
| [VALIDATION-004](docs/backlog/validation.md#validation-004) | DOS 3.1/3.1.1 master boots on a ][+ with 13-sector PROMs, or an identified independent emulator/ROM setup; distinguish disk behavior from an emulator defect. |
| [MEMORY-003](docs/backlog/cpu-memory.md#memory-003) | Real Language Card power-on bank with distinct bytes in both banks; its F8-ROM expansion remains frozen with the original ][ profile. |
| [DISPLAY-001](docs/backlog/display.md#display-001) | Real VidHD SHR 320/640/fill and `$C022`/`$C034` color captures. |
| [DISPLAY-011](docs/backlog/display.md#display-011) | Obtain Eve Leonard and pin a third Eve frame. Other board-specific residuals stay conditional on a hardware oracle. |
| [AUDIO-011](docs/backlog/audio.md#audio-011) | Real SSI263 inflection/rate/articulation/level recordings; independent Echo+ board/pin validation is a separate residual. |
| [AUDIO-012](docs/backlog/audio.md#audio-012) | Electrical capture across the //e speaker for a known toggle/square wave before fitting a physical response model. |

## 4. Waiting for a report or a named need

| Task | Reopen condition and next action |
|---|---|
| [NETWORK-005](docs/backlog/network.md#network-005) | On a reported delayed FujiNet need, add an atomic in-flight indicator visible without the blocked machine lock in the UI and `/status`; measure responsiveness. Do not call that an asynchronous I/O implementation. |
| [NETWORK-010](docs/backlog/network.md#network-010) | LAN inbound mapping only after a named need. Local `LISTEN` already works; specify interface/host port and forwarding policy first. |
| [DISPLAY-010](docs/backlog/display.md#display-010) | F6 luminance-neutral row dim only on report. F4 is a deliberate choice; F2 overlaps the unscheduled scanline possibilities. |
| [ARCH-008](docs/backlog/architecture-tooling.md#arch-008) | Paint editor test seam only on a concrete report; sprite seam already landed and editors remain frozen. |

## 5. Frozen — no implementation scheduled

All **28** formal frozen entries are listed here. They need the domain's
reopening condition (usually named software or an explicit request), not
merely an attractive estimate.

| Domain | Frozen entries |
|---|---|
| Storage | [STORAGE-005](docs/backlog/storage.md#storage-005) multi-partition; [STORAGE-007](docs/backlog/storage.md#storage-007) half-track NIB; [STORAGE-008](docs/backlog/storage.md#storage-008) additional Floppy Emu modes; [STORAGE-010](docs/backlog/storage.md#storage-010) NIB revolution normalisation. |
| Audio/cassette | [AUDIO-004](docs/backlog/audio.md#audio-004) resampling; [AUDIO-005](docs/backlog/audio.md#audio-005) analog stage; [AUDIO-008](docs/backlog/audio.md#audio-008) DAC; [AUDIO-009](docs/backlog/audio.md#audio-009) MIDI; [CASSETTE-001](docs/backlog/cassette.md#cassette-001) enriched cassette playback. |
| Display | [DISPLAY-003](docs/backlog/display.md#display-003) analog composite; [DISPLAY-008](docs/backlog/display.md#display-008) smooth interpolation; [DISPLAY-012](docs/backlog/display.md#display-012) 80-column color phase without an oracle. |
| Cards | [CARDS-002](docs/backlog/cards.md#cards-002) Workstation/LocalTalk extension; [CARDS-006](docs/backlog/cards.md#cards-006) SCSI; [CARDS-007](docs/backlog/cards.md#cards-007) VGA; [CARDS-008](docs/backlog/cards.md#cards-008) UDC; [CARDS-010](docs/backlog/cards.md#cards-010) IWM-level controller. |
| Interface | [UI-004](docs/backlog/ui-debugger.md#ui-004) deeper tutorials; [UI-005](docs/backlog/ui-debugger.md#ui-005) extra voxel polish; [UI-006](docs/backlog/ui-debugger.md#ui-006) unspecified layout refresh; [UI-008](docs/backlog/ui-debugger.md#ui-008) virtual joystick. |
| Network/browser | [NETWORK-008](docs/backlog/network.md#network-008) in-process FujiNet firmware; [NETWORK-012](docs/backlog/network.md#network-012) browser Ethernet transport; [WASM-002](docs/backlog/wasm.md#wasm-002) mobile touch; [WASM-003](docs/backlog/wasm.md#wasm-003) AudioWorklet tuning. |
| Architecture/projects | [ARCH-007](docs/backlog/architecture-tooling.md#arch-007) Z80 cleanup; [PARKED-001](docs/backlog/parked.md#parked-001) further card ports; [PARKED-003](docs/backlog/parked.md#parked-003) bare-metal Raspberry Pi. |

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
| Release and distribution | [release-1.0.md](docs/backlog/release-1.0.md) |
| Parked projects | [parked.md](docs/backlog/parked.md) |
| How the backlog works · already done · evidence | [README.md](docs/backlog/README.md) |
| Possibilities (Casso survey, game companions) — not tasks | [possibilities.md](docs/backlog/possibilities.md) |
