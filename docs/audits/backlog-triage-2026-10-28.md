# Backlog triage — 2026-10-28

Revision: `main` at `aad2fee5`. Scope: every non-frozen entry marked *To verify*,
*Partial*, *Mitigated*, *Known limit*, *Ready…* or *Blocked* in
`docs/backlog/` except `release-1.0.md`.

**Method.** Code reading, `grep`, `git log`, `wc -l`, and the test binaries
already present in `build/`, run through `ctest -R <name>`. **Nothing was
rebuilt**, so a binary may predate `aad2fee5` by a few commits (the recent
ones are CP/M only). A verdict of *Resolved* means the claim no longer holds
on this revision, not that a reverted-fix control was run. This document is
the evidence; the domain entries carry the resulting state and the
[TODO](../../TODO.md) carries the ordering.

Verdicts: **Confirmed** (still open) · **Partial** (partly resolved) ·
**Resolved** · **Oracle** (needs a real-hardware measurement or a primary
document) · **Decision** (needs a scope ruling) · **Unknown** (cannot tell
without a build or an interactive run).

## CPU, memory, display, audio

| ID | Verdict | Evidence | Tier | Next action |
|---|---|---|---|---|
| MEMORY-002 | Confirmed | The inline condition chain in front of the ROM-window hit is unchanged (`Memory.h:309-345`); no per-page dispatch table. `bus_fastpath`, `bench_identity` pass. | Core | Re-profile a ][+ banner (`PERFORMANCE.md` § 7.4) to confirm the ~15 % share, then prototype the 256-entry table against `bus_fastpath`. |
| MEMORY-003 | Oracle (bank) / Decision (F8 ROM) | POM2 resets to bank 2 (`Memory.cpp:685`); no "LC fitted" option. | Core (bank); F8 ROM frozen with the ][ Original | On a real 16K Language Card in a ][/][+, pre-write distinct bytes to banks 1 and 2, power-cycle, read `$D000` without touching `$C08x`. |
| DISPLAY-001 | Partial + Oracle | IIgs text and border colours landed 2026-10-21 (`VidHDCard.h:63-71`, `borderColour()` :151). Extended text modes, ][/][+ aux mirror, SHADOW and mid-frame SCB/palette changes are not modelled (`paintPicture` reads aux at paint time, `VidHDCard.cpp:85`). `vidhd_card` passes. | Supported | Keep open only for a real VidHD capture (SHR 320/640/fill test image + a `$C022`/`$C034` colour screen). |
| DISPLAY-011 | Partial (rest has no oracle) | Video-7 is implemented (`LeChatMauveCard.h:109,128,148`); P7 is done (`chatmauve_plan.md:13,674`). Eve `$C0Bx` switches apply with no beam event (`eveDecode`, `.cpp:213-222`); no TTL-RGBI palette, no trim-pot gain; no Eve Leonard disk in `disks_5.4/chatmauve/`. `chatmauve_*`, `purplesoft_eve_screens` pass. | Core (Féline/Eve goldens); residuals frozen | Fetch the Eve Leonard image as the one actionable golden; freeze the rest until an oracle. |
| DISPLAY-002 | Confirmed | `display_golden_hash_test.cpp` has no ALTCHAR/MouseText and no PAL pins. `uploadScreenTexture()` runs unconditionally from `drawScreenImage()` (`MainWindow_Screen.cpp:291` → `:211`). The "needs a user ROM" blocker is gone: `roms/apple2e_char*.rom` are tracked. | Core | Add `iie/altchar` + MouseText hash pins, then measure skipping the fallback upload on the OE-GPU path. |
| DISPLAY-008 | Confirmed | No interpolation toggle; LINEAR only inside the CRT glass pass (`CrtEffectStack.cpp:737-745`). | Frozen | Freeze: cosmetic, no oracle, no named need. |
| DISPLAY-010 | Partial | F4 documented as deliberate (`DEV.md:1676-1680`); F2 is cos² vs sin², a phase-only difference (`CrtEffectStack.cpp:304`); F6 row dim not luminance-neutral (`:351`). | Supported | Close F2 and F4; keep F6 as a fix-on-report one-liner. |
| AUDIO-011 | Oracle | `Ssi263.cpp:278`: inflection, articulation, analog filtering not modelled; `inflect_` stored, unused. `ssi263_smoke`, `phasor_timeline` pass. | Supported | Needs recordings of a real SSI263 sweeping INFLECT/RATE/ARTIC, reset scope traces, Phasor/Echo+ levels. |
| AUDIO-003 | Confirmed (partial) | No change since the 2026-10-27 entry (`CHANGELOG.md:219-221`); model ratio holds (`audio_stereo`: 0.2940 vs 0.2996). | Core | Re-record MAME with each source isolated through one output mapping; compare with 0.30. |
| AUDIO-005 | Confirmed absent + Oracle | No LM386 / output-stage model in `src/`; MAME has none. | Frozen | Reopen with a calibrated scope capture of a Sweet Micro board for a known AY square. |
| AUDIO-006 | Confirmed (design), not measured | `M6502.cpp:2263` → `MockingboardCard::advanceCycles` locks `mtx` (`Mockingboard.cpp:1227`); the audio callback holds it across `fillAudioTimed` (`:583-590`). | Supported | Measure lock wait during Sound II speech after ARCH-003; SPSC only on a measured wait. |

## Storage, validation, UI

| ID | Verdict | Evidence | Tier | Next action |
|---|---|---|---|---|
| STORAGE-014 | Confirmed (mitigated residue) | `StorageCoordinator.cpp:886-964` runs phases 1-2 at most four rounds; phase 3's `ejectBay` (`:981`) still flushes leftovers inline under the lock. No test writes during phase 2. | Core | Test that dirties a host-folder bay during each phase-2 commit and checks phase 3 saves the residue; otherwise record as accepted limit. |
| STORAGE-002 | Partial | `pom2::bootMountedMedia` (`MediaBoot.cpp`, `boot_configuration` passes) refuses `bay != 0` before any reset — the wrong-medium risk is gone. `/disk` is Disk II only (`AiControlServer.cpp:1317-1380`); no Sony/SmartPort mount or boot endpoint. | Supported | Decide on a slot+bay `/media` endpoint reusing `bootMountedMedia`, or reword to the API gap. |
| STORAGE-005 | Confirmed | `Block512Backing::kMaxBlocks = 0x10000` (`Block512Backing.h:98`); nothing maps several volumes per image. | Frozen | Won't-do until a named multi-partition image is needed. |
| STORAGE-010 | Confirmed (known limit) | `DiskImage::computeCellWidths` (`DiskImage.cpp:1637-1641`) keeps 4 µs cells; no rescale to 200 ms. | Frozen limit | Deliberate limit: a rescale would re-base the LSS timeline and every golden. |
| STORAGE-003 | Confirmed | `std::array<TrackBuffer, kTracks> tracks;` (`DiskImage.h:525`); no `DiskImage` stack locals remain; `diskii_insert_thread_stack` pins only the insert path. | Core | Best-of-9 `pom2_bench` with `tracks` on the heap; adopt only without regression. |
| VALIDATION-001 | Oracle | `kStepperResponseCycles = 256` (`DiskIICard.h:680`, applied `DiskIICard.cpp:803`); MAME moves the head, AppleWin does not. | Supported (//c port) | On a real //c, while the motor coasts: PH1, then PH3 4 cycles later, release; read the address field; repeat for gaps 4…512 cycles. |
| VALIDATION-003 | Partial | `protected-disks-2026-10-12.md` § "Rerun 2026-10-27" reaches the Captain Goodnight A/B and Prince of Persia A milestones; WOZ captures are in a scratch dir, no automated golden. PoP side B, later levels and quarter tracks unverified. | Supported | Record WOZ SHA-256s and probe commands as a repeatable corpus entry (feeds VALIDATION-006). |
| VALIDATION-004 | Oracle (or MAME) | Both masters are tracked (`disks_5.4/dsk/DOS 3.1 Master.d13`, `Apple DOS 3.1.1 Master.d13`). MAME is installed but `mame -verifyroms apple2p` → romset not found. | Supported | Boot both on a ][+ with 13-sector PROMs (real, or MAME once romsets exist); repeat on Integer-BASIC ][. |
| VALIDATION-005 | Unknown (GUI only) | One run in three, never reproduced headless. The bundled A2 File Cmd moved to 1.0 in `f17b9206`. | Supported | Six GUI runs on a *copy* of the 1.0 image; close as stale if it does not reproduce. |
| UI-002 | Confirmed (partial) | Four callers use `NativeFileDialog`; typed paths remain in cassette (`MainWindow_MiscPanels.cpp:917,967`), SmartPort (`SmartPort_ImGui.cpp:164`), Disk/HDV/3.5 (`MainWindow_StoragePanels.cpp:1450,1517,1805`), printer (`MainWindow_DevicePanels.cpp:459`). Windows not checked by hand. | Supported | Convert the listed panels with the existing fallback; Windows cancel/open/save check. |
| UI-003 | Partial | `/cpu` validation and debugger items fixed. `handleScreen(...)` (`AiControlServer.cpp:1763`) has no method check; POST is answered. | Supported (AI server: urgent on break) | GET-only check (405 otherwise) + smoke assertion; full media inventory out of scope (`block_storage` / `floppy_storage` exist). |
| UI-004 | Decision | No tutorial code; only `PanelId::Welcome` (`PanelCatalog.h:274`). | Frozen | Won't-do until one or two tutorials are named with an acceptance walk-through. |
| UI-006 | Decision | Presets with fixed split ratios (`MainWindow.cpp:1160-1186`); default desktop reworked in v0.9.6 (`f3bece5e`); "airier" has no measurable criterion. | Frozen | Reopen with a concrete window size and the panel that misbehaves. |
| UI-007 | Resolved | `isDuplicate` (`MainWindow_Slots.cpp:217-225`) calls the same `SlotConfigurationCoordinator::isMultiInstance` (`SlotConfigurationCoordinator.cpp:131-150`) that `resolve()` uses (`:289`). `slot_configuration_coordinator` passes. | Supported | Archived. |
| UI-009 | Decision | Prerequisite present (`DiskLibrary_ImGui.h:22,42,289`: search/sort, "All media" tab); no thumbnail code. User-requested order: miniatures, then file associations. | Supported | Decide cover art vs captured screen. |
| UI-010 | Partial | `.moof` now declared on macOS and Linux. Windows ships a bare `.zip` (`release.yml:12,840`), no association. macOS uses `LSHandlerRank` **Owner** (`Info.plist.in:33`), contrary to "without overriding another app's default". | Supported | Rank `Alternate` for `.po`/`.dsk`; document Windows "Open with"; clean-host check on macOS/Linux. |

## Network, cards, misc

`cassette.md` and `parked.md` hold only frozen tasks. `ctest -R "fujinet|sp_over_slip|w5100|uthernet2|als_cpm|abstraction"` (11/11) and `ctest -R "tnfs|transwarp|fourplay|4play"` (5/5) pass.

| ID | Verdict | Evidence | Tier | Next action |
|---|---|---|---|---|
| NETWORK-001 | Confirmed (phase 1 done, 2-4 not started) | No native Fuji control device, block serving or source selector; `FujiNet_ImGui.cpp:118-126` offers TCP / Serial / Off. Built-in `N:` (`FujiNetNetDevice`) and TNFS client shipped. | Supported (relay); native 2-4 frozen | Record phases 2-4 as frozen until a named need. |
| NETWORK-002 | Resolved (POM2 side) | `FujiNetCard.cpp:565-584` answers CONTROL `$00` to the printer unit locally; peer death reported (`SpOverSlipLink.cpp:398`). | Supported | Archived; report the upstream abort separately. |
| NETWORK-003 | Resolved (POM2 side) | POM2 serves `N:` itself (`FujiNetNetDevice.h`), on by default (`MainWindow_SlotConfig.cpp:518-519`), covered by `fujinet_card` (`fujinet_card_smoke_test.cpp:617,681`). | Supported | Archived; the desktop-firmware bug is upstream. |
| NETWORK-004 | Partial + needs a peer | 250 ms default, `kRemoteFujiTimeoutMs = 5000` (`SpOverSlipLink.cpp:532,604`); delayed-fake-peer tests (`sp_over_slip_link_test.cpp:401,428`) pass. | Supported | One live delayed-TNFS run against a named peer version, or close. |
| NETWORK-005 | Confirmed | `transact()` blocks the CPU thread under `stateMutex` up to 5 s (`SpOverSlipLink.h:53-58,219-227`); no in-flight indicator. | Supported | Atomic in-flight flag shown in the UI and `/status`. |
| NETWORK-007 | Confirmed | `fujinet` is slot-only (`SlotCardCatalog.h:138`); `noPhysicalSlots` picker allows only chatmauve and the Mockingboards (`MainWindow_Slots.cpp:253-257`); no hook in `IIcExternalSmartPort`. | Supported or frozen | Decide whether to schedule. |
| NETWORK-008 | Decision (already rejected) | Rejection recorded in `docs/fujinet_plan.md` § 8; external-helper design remains (`FujiNetCard::startHelper`). | Frozen | Frozen; reopen if installing a second program is shown to be the blocker. |
| NETWORK-010 | Confirmed (LAN mapping only) | `LISTEN` binds `INADDR_LOOPBACK` only (`W5100HostSockets.cpp:128`) under `allowLoopback_` (`W5100Device.cpp:693`); no hostfwd. `w5100_socket_seam`, `uthernet2_w5100_smoke` pass. | Supported | Unscheduled until a named need. |
| CARDS-001 | Partial | `abstractionCatalog()` (`AbstractionLevels_ImGui.cpp:119+`) has no `workstation`, `4play`, `transwarp` key, though `docs/lle_vs_hle.md:153-155` documents them; also missing: `slinky`, `zipchip`, `sam`, `videoterm`, `vidhd`, `grappler1`, `pic`. | Supported | Add the `AbsEntry` rows mirroring `lle_vs_hle.md`. |
| CARDS-011 | Resolved | `alscpm` exists; `als_cpm_card` passes; `CHANGELOG.md:33` has the entry. | Supported | Move the register map to a DEV.md section, then archive (CLAUDE.md and the CHANGELOG link to the entry today). |
| WASM-001, INPUT-002 | Resolved (anchors only) | Already out of the task indexes. | — | None. |

## Architecture and tooling

| ID | Verdict | Evidence | Tier | Next action |
|---|---|---|---|---|
| ARCH-004 | Partial | `Memory.cpp` 2352 (`MemorySnapshot.cpp` cut); WOZ flux writer in `DiskImageWozFlux.cpp` (`97705ab8`), but `DiskImage.cpp` is 3314 vs a 3316 ceiling with its WOZ `takeWriteBack`/`commitWriteBack` branch still at `:2849-3000`; `ImageWriter.cpp` 2424, `Apple2Display.cpp` 2502, `HgrPaintEditor.cpp` 2656. `check_file_sizes.sh` exits 0. | Supported (ratchet R4) | Lower the stale ceilings; list what is still owed (DiskImage WOZ save half, Apple2Display; HgrPaintEditor frozen). |
| ARCH-009 | Partial | `frontend_device_panel_concurrency`, `session_file_picker`, `drive_rack_interaction` pass; the concurrency harness covers `Uthernet_ImGui` only. | Supported | One more card-pointer panel under replug, or close under the Supported rule. |
| ARCH-003 | Confirmed | The nightly TSan leg (`.github/workflows/ci.yml:472-485`) runs no ImGui panel, `/screen.ppm` + `demodMutex` or replug test. | Supported | Add `frontend_device_panel_concurrency` and `drive_rack_interaction` to the TSan filter. |
| ARCH-008 | Partial | Sprite seam landed (`HgrSpriteEditor.h:67`, `hgr_sprite_undo_test.cpp`, `135051ff`); `HgrPaintEditor.h` has none. | Frozen (editors) | Record the sprite seam; mirror it for the paint editor only on a report. |
| ARCH-010 | Confirmed | No config class; 25 files call `getenv` (~30 `POM2_*`); `--help` (`CliDispatcher.cpp:180`) lists only `POM2_AI_CONTROL_TOKEN`. | Supported | Post-1.0; first step: an env-var table in `printUsage()`. |
| ARCH-011 | Confirmed (lock held) + Decision | `MainWindow_Slots.cpp:1579-1584` holds `lockState()` across `plugSlotsFromSettings`; the comment at `:1560-1562` says the CPU worker is stopped, so the audio-jitter premise does not hold. | Supported | Close as "worker stopped, by design", or wait for ARCH-003. |
| ARCH-001 | Resolved | Every item done or a documented exception (`ProfileSwitch.cpp:131`, `DiskIICard.cpp:497-512`, `kDnsWaitMs = 120` in `W5100Device.h:229`); `storage_lock_budget` passes. | Core (shape) | Archived. |
| ARCH-007 | Confirmed (quality only) | `SoftCardZ80.cpp:169-187` hand-packs bytes; duplicated helpers in `softcard_cpm_boot_test.cpp:72,123`; repeated `switch (p)` in `Z80.cpp`. | Supported — scope says "do not schedule" | Closed as won't-do (frozen). |
| ARCH-012 | Confirmed | `namespace pom2` in 375 of 484 `src/*.{h,cpp}` (entry said 255/336); 23 of 424 tests. | Supported | Update counts; post-1.0; one directory at a time. |
| ARCH-013 | Confirmed | `name(void)` declarations (124 in `M6502.h`, 112 in `.cpp`), French comments (`M6502.h:58,158,166,176`), ~28 C-style casts. | Core (CPU) | Comment/declaration-only pass verified by unchanged Harte/Klaus and `display_golden_hash`; leave hot-path casts. |
| ARCH-014 | Confirmed (Ready) | `device_standard_clock_test.cpp` never mentions speaker, cassette or floppy. | Supported | As written. |

## Cross-cutting findings

- **Duplicate ID.** `CARDS-011` names the ALS CP/M card in `cards.md` and,
  separately, "The ImageWriter residue (CARDS-011, closed)" at
  `CHANGELOG.md:945`. ARCH-016 proposes a uniqueness check.
- **RVB Graph drift.** `src/LeChatMauveCard.cpp:132-159` models the RVB Graph
  from its manual (commit `3c091891`): control register at `$C0F0+S+U+K`
  (low nibble S/U/K, high nibble 15−CT) and eight colour registers at
  `$C70F/$C71F/$C72F/$C73F − Bn`. `docs/audits/emulation-parity.md` row 3bis
  ("four strobes only"), `docs/decisions/project-scope.md` (rvb "gated on a
  manual that has never surfaced") and DISPLAY-011 were stale. Corrected in
  this pass.
- **Ratchet slack.** `tools/file_size_budget.txt` still allows
  `ImageWriter.cpp` 2809 (actual 2424, after `fe596777`) and
  `Apple2Display.cpp` 2536 (actual 2502). Both can regrow without failing
  `check_file_sizes.sh`. Owed under ARCH-004.
- **Abstraction catalog.** No `AbsEntry` for `workstation`, `4play`,
  `transwarp`, `slinky`, `zipchip`, `sam`, `videoterm`, `vidhd`, `grappler1`,
  `pic`. Owed under CARDS-001.
- **MAME without Apple II romsets.** `/opt/homebrew/bin/mame` is installed
  but has no `apple2p`/`apple2c` romset; adding them makes VALIDATION-004 and
  MAME's side of VALIDATION-001 local checks.
- **Index vs body.** UI-003 (index *Partial*, body *To verify*) and
  STORAGE-014 (index *Mitigated*, body *Confirmed by reading*) disagreed.
  Aligned in this pass.
- **Upstream FujiNet bugs.** NETWORK-002 and NETWORK-003 are worked around in
  POM2; the upstream `Request::from_packet` abort and the desktop firmware's
  `N:` socket are FujiNet's to fix.
