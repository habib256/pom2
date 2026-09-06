# POM2 — TODO

Status 2026-09-05 · **v0.9.0 → 1.0**. This file lists **open work only**: an item
that ships is deleted here and its "why" is written up in `CHANGELOG.md`. MAME
refs → `DEV.md`.

**Format**: `🔴 blocks 1.0 · 🟠 high · 🟡 medium · 🟢 low · 🧊 frozen` at the head
of each item. Indicative effort in *italics*. File/line in `backticks`.

**Read in this order**:

1. [What 1.0 means](#what-10-means) — the gate. Everything below is ordered
   against it, and nothing else in this repo answered it before 2026-09-05.
2. [The road to 1.0](#the-road-to-10) — six gates, ordered. This is the work.
3. [The scope ruling](#the-scope-ruling) — core / supported / frozen, per
   subsystem. It is what makes the rest of this file finite.
4. [The shape of the risk](#the-shape-of-the-risk) — the measurement the
   ordering rests on. Read it before disagreeing with the ordering.
5. [Standing rulings](#standing-rulings) — decisions, not work. Do not
   re-litigate without new evidence.
6. [MAME ↔ POM2 parity](#mame--pom2-parity-dashboard) — the fidelity dashboard.
7. [Backlog](#backlog) — **post-1.0 by default**, by subsystem.

**A method note, kept at the top because it has been earned three times.**
*A mechanism that reports success while doing nothing* is this project's
signature defect: the version header generated to the wrong path, the file-size
ratchet aborting on bash 3.2 **with exit status 0**, `POM2_FOUNDATION_SOURCES`
written down and never read, the TSan fix that passed one green run with the fix
*reverted*. All were found by **running** the guard, never by reading it. Write
the test that makes it FAIL. And prose volume is not verification: a
well-written explanation of a fix is not evidence that it fixes anything.

**A second one, new on 2026-09-05.** This file was audited against the tree and
roughly twenty of its items were stale — three of the four rows in its own
headline drift table were already fixed, an item called the SSC IRQ DIP
unimplemented in two places while `SuperSerialCard.cpp:492` implements it, the
`MainWindow.cpp` god-object item still said 8 319 lines against a file of 1 408,
and a dozen `file:line` citations no longer pointed at what they named. **A TODO
that describes a tree that no longer exists is the same class of defect as a
guard that returns 0** — it reports work that is not there. Re-verify a citation
before acting on it, and delete on landing.

## What 1.0 means

**1.0 is not "no known bugs" and not "the parity dashboard is green".** Neither
is reachable: the dashboard can only grow, and the tail is infinite by
construction. 1.0 is four claims, and each is falsifiable:

1. **Everything POM2 says about itself is true.** README, the card picker, the
   ROM Status panel and the Welcome text describe the tree as it is — no card
   billed as working that is silent, no feature credited to a platform whose
   package does not contain it, no limitation listed that was closed in
   September.
2. **What POM2 does not promise is written down, once.** That is
   [the scope ruling](#the-scope-ruling). Without it the tail is a debt; with
   it, the same code is a documented strength.
3. **The release is repeatable and lawful.** It can be built from a clean
   checkout by someone who is not the author, on a schedule rather than on a
   tag, and everything inside the package may be redistributed.
4. **No known defect silently destroys or corrupts a user's data.** Media
   write-back, settings persistence and snapshots are the only three paths that
   can lose something the user cannot regenerate. All three had a named defect;
   all three are fixed ([G2](#g2--the-three-defects-that-reach-a-users-data--2026-09-06),
   2026-09-06).

**Explicit non-goals for 1.0**, so they stop competing for the same hours: the
MAME `a2bus` port backlog (Videx, Mountain Music, E-Z Color, SCSI, The Mill, PC
Transporter), the analog IIR composite pipeline, ayumi-grade FIR resampling, the
Saturn 128K LC, the native `FujiNetDevice` phases 2-4, the Workstation Card's
LocalTalk endpoint, and every fidelity residual the dashboard lists as 🟢. None
of them is refused; all of them are *after*.

**The one thing 1.0 buys that 0.9 does not** is the right to say no. Today every
open item is implicitly owed. After the ruling, three quarters of them are not.

## The road to 1.0

Six gates. They are **ordered by whether the next gate is worth doing if this
one is wrong** — not by size, and not by appetite. G1 first because a package
that cannot be distributed makes every other gate moot; G2 second because it is
the only class that destroys something the user cannot get back.

Total: roughly **12-16 working days** at this tree's measured pace, of which
G1+G2 is under three.

| Gate | What it is | Cost | Blocks 1.0? |
|---|---|---|---|
| [G1](#g1--what-we-are-allowed-to-ship-) | What we are allowed to ship | 1-2 d + one decision | **yes** |
| [G2](#g2--the-three-defects-that-reach-a-users-data--2026-09-06) | Three defects that reach a user's data | ✅ done | done |
| [G3](#g3--make-the-words-true-) | Make the words true | 1-2 d | **yes** |
| [G4](#g4--a-release-that-can-be-rehearsed-) | A release that can be rehearsed | 1 d | **yes** |
| [G5](#g5--the-donut-policy-without-a-test-) | The donut: policy without a test | 4-6 d | partly |
| [G6](#g6--the-platforms-we-claim-) | The platforms we claim | 1-4 d | decision |

### G1 · What we are allowed to ship 🔴

*The gate nothing else survives. It is not a technical problem and it does not
get cheaper by waiting.*

The audit that produced this gate found that the licensing question recorded in
this file was scoped wrongly: it was filed under [WASM] as *"shipping ROM dumps
in a public web demo"*, a decision to be made before a marketing push. It is
neither WASM-specific nor prospective. **It describes the repository as it
stands, the desktop packages as they ship, and a demo that has been live for
months.**

- ◔ **Commercial software tracked in git — removed from the work tree
  2026-09-05, still in history.** 907 files (286 MB) moved to
  `/Volumes/TEST/pom2-media/`, each copy verified by SHA-256 before the
  original was deleted, with a `MANIFEST.tsv` and a `RESTORE.sh` alongside:
  the whole 4am/Asimov WOZ collection (`disks_5.4/woz`, 757 titles), the
  personal game collection (`disks_5.4/gist`, 109), the commercial half of
  `disks_5.4/dsk` (31), four `hdv/` volumes (**Nox Archaist**, AppleWorks,
  Total Replay II, Wizard Replay) plus three volumes of unestablished
  provenance, five `disks_3.5/` (Oregon Trail, both
  Print Shop, Multiscribe, TheBestGames) and `floppyemu/Total Replay
  v6.1.hdv`. Kept deliberately: the French Touch demos with their sources,
  the Purplesoft / Chat Mauve preservation disks (the `purplesoft_eve_screens`
  oracle), Apple system software, and the author's own projects.
  Full suite re-run after the move: **240/240 green**.
  **What is left, and it is the larger half**: the blobs are still in the
  packfile, so GitHub still serves every one of them to anyone who clones.
  A tip-only deletion does not undo that. *~1 d to rewrite history (306 MB
  packfile), or accept it and document the risk.*
  The three `hdv/` volumes whose contents were never established
  (`2018-01-23 - ProDOS8.2mg`, `Bad.Apple.hdv`, `Mouseapps Apple2.hdv`, 96 MB)
  went the same way on the same day, so `hdv/` now holds only the author's own
  projects. **910 files, 382 MB** in the manifest.
- 🔴 **The web demo now boots a file that no longer exists.**
  `floppyemu/Total Replay v6.1.hdv` (~150 commercial titles) was removed on
  2026-09-05, and `wasm/shell.html:84` still defaults to it — so the next push
  to `main` deploys a demo whose boot disk 404s. **Deliberately left for a
  product decision**, because it is not just a path: the shell also selects a
  **//c profile** to suit Total Replay, and the obvious free replacement (DIX,
  GPLv3) wants //e PAL with a Mockingboard. Cheapest shape that needs no
  manifest or parser change: copy the chosen disk into `floppyemu/` (which
  `bundle.manifest:64` already bundles as the browser-only extra — `disks_3.5`
  is on the `deny` list, so a per-file `wasm` entry would need new parser
  support), then repoint the two knobs in `shell.html`. *~2 h once the disk is
  chosen.*
- 🔴 **42 Apple firmware dumps in every package, while the UI says the
  opposite.** `bundle.manifest:31` ships `apple2*.rom`, fifteen character
  generators, `disk2.rom`, the mouse MCUs, `341-0358-A.bin`, `liron.rom` — and
  `RomStatus_ImGui.cpp:416` and `MainWindow_MiscPanels.cpp:654` tell the user
  *"POM2 ships no ROMs"* / *"Apple II firmware is copyrighted, so POM2 does not
  ship it."* README § Download sells the bundled ROMs as a feature. **Shipping
  them and claiming not to is the worst of the three available positions.**
  **Decided 2026-09-05: keep the dumps, fix the words.** The ROMs stay in
  `roms/` and in every package, and so does Apple's system software on disk
  (the DOS 3.x masters, `AppleShare IIe Workstation.po`, Apple Présente //c) —
  it is the same legal class and the same established emulator practice. What
  changes is the tree stops contradicting itself: `RomStatus_ImGui.cpp:416`,
  `MainWindow_MiscPanels.cpp:654` and the README all say the same true thing.
  *~4 h.* This is now a [G3](#g3--make-the-words-true-) job, not a G1 one.
  Third-party EPROMs sit in the same tree with no permission on record
  (`cffa20ee02.bin`, `grappler_plus.bin`, `thunderclock_u9_v1.3.bin`, the Videx
  chip).
- 🟠 **No attribution file exists.** `find . -iname '*LICENSE*'` returns exactly
  `./LICENSE`. Missing: DejaVu's Bitstream Vera notice, Font Awesome Free's
  SIL OFL 1.1 + CC-BY-4.0, and a root `THIRD-PARTY.md` for MAME (GPL-2+ code,
  BSD-3 samples), AppleWin (GPL-2+), Dear ImGui (MIT), GLFW (zlib) and the two
  `pic/` photographs, which have no recorded provenance at all. Inbound code
  licensing is otherwise **clean** — every port is GPL-2.0-or-later, which
  upgrades to POM2's GPL-3.0 without friction. *~3 h.*

### G2 · The three defects that reach a user's data ✅ (2026-09-06)

*Found by measurement on 2026-09-05, fixed on 2026-09-06. Each was the only
kind of defect 1.0 must not have: it takes something the user cannot make
again. Details and rationale in CHANGELOG.md.*

- ✅ **The status-bar eject wrote the wrong settings key.** Fixed, and the
  scope was WIDER than this entry recorded: it described "two eject paths
  ... in the same file", but an audit of every `ejectDisk`/`ejectBay` call
  site found **four** hand-rolled ejects across three files, three of them
  never written down here:
  * `MainWindow_Media.cpp:98` (status bar) — built `disk_path_slot<N>` for
    BOTH drives, so ejecting drive 2 cleared drive 1's path and left
    `_drive2` set. The documented one.
  * `MainWindow_Slots.cpp:971` (Slot Config) — skipped drive 2 entirely on a
    comment claiming "drive 2 mounts are session-only", untrue since
    `diskIIPathSettingKey` gained `_drive2` and `restoreMediaFromSettings`
    began looping both drives. Its Insert button had the same gap, so a
    drive-2 insert here was lost on restart while the same insert from the
    File menu survived.
  * `MainWindow_StoragePanels.cpp:578` (Library, eject-by-path) and `:797`
    (Library, eject current) — cleared no settings key at all. The 3.5"
    branch twelve lines below `:578` had already been routed through the
    coordinator, with a comment explaining why; the 5.25" sibling was left
    hand-rolled.
  A fifth site was the read-side mirror: the profile-switch remount
  (`MainWindow_Slots.cpp`) restored drive 1 only, so an Apply emptied drive 2
  while its key stayed set. It cannot delegate (it runs inside the `stateMutex`
  scope that keeps the SlotBus rebuild atomic), so `diskIIPathSettingKey` is
  now exported from `StorageCoordinator.h` and used inline — one definition of
  the key instead of a sixth hand-rolled copy.
  All four ejects now delegate to `StorageCoordinator::ejectDiskII` /
  `ejectMediaBay`. Pinned by `storage_coordinator`, which now asserts the
  half that was missing: ejecting drive 2 clears `_drive2` **and leaves
  drive 1's key alone**.
  One sub-claim above was already STALE when re-verified: the status-bar
  save was no longer under `stateMutex` (the lock scope closed on the line
  before it).
- ✅ **A snapshot now carries a machine identity.** The reserved `flags` word
  holds `pom2::snapshotMachineId(profile)` (FNV-1a over the canonical
  persistence key); `--snapshot-load` and the AI server's `/snapshot`
  refuse a mismatch with a message naming both machines. Identity 0 =
  "written before the field existed" and still loads, so no existing
  snapshot is invalidated; rewind frames record none on purpose (the ring
  is already cleared on a profile switch). → [DEV § Snapshot](DEV.md#snapshot).
  Pinned by `snapshot_io` + `system_profile`.
- ✅ **`PhasorCard::onReset` bumps the counter instead of zeroing it.** Now
  matches `Mockingboard.cpp:772`'s documented BUMP-don't-zero contract, so
  a reset always re-seeds the audio thread's generators instead of being a
  no-op whenever the counter was already 0. Pinned by `phasor_card_smoke`
  (verified to fail with the old assignment restored).

### G3 · Make the words true 🔴

*Claim 1 of [What 1.0 means](#what-10-means). Cheaper than any code in this
file, and it is the difference between a project that under-promises and one
that cannot be trusted about anything.*

- 🔴 **The Workstation Card is billed four paragraphs past what it does.**
  README § Expansion Cards describes acquiring a LocalTalk node and reaching
  the AppleShare menu — all true, and all of it one step short of the thing the
  card exists for: **there is no network on the other end**, and `lapACK` does
  not move the node. The card picker is already honest ("boots, host link
  WIP"); the README is not. Rewrite to *"boots, self-tests and is identified by
  period software; it does not netboot."* *~30 min.*
- 🔴 **`echoplus_tms` is listed twice in README as a card.** It cannot make a
  sound — the TMS5220 LPC decoder does not exist. **This closes the standing
  P3-2 question ("ship or hide") as: hide.** Remove it from the README table
  and from the picker, or gate it behind a developer filter. *~1 h.*
- 🔴 **Uthernet I is credited to "Linux and macOS" and is in neither package.**
  `package_macos_release.sh:44` sets `-DPOM2_ENABLE_SLIRP=OFF`, and
  `build_in_bionic.sh` installs no `libslirp-dev` — bionic has none, libslirp
  was split out of QEMU in 2019. So the feature exists **only in the three ARM
  AppImages**, which are the least-downloaded packages. Either restate the
  claim or enable it (macOS needs a universal-2 libslirp recipe, `~1 d`).
  README:269 also lists the card with no platform caveat at all. *~3 h for the
  words.*
- 🟠 **README under-claims in two places, which is the same defect inverted.**
  § Known Limitations still says the //c+ on-board 3.5" boot does not reach a
  bootable disk — closed 2026-09-01, pinned `iicplus_boot35`. And **`liron` is
  absent from the card table entirely**, a card with real firmware, a real IWM
  and two pins. *~30 min.*
- 🟠 **Tag the card picker with its scope bucket**, next to the LLE/HLE level it
  already shows. Five of 24 catalog keys are frozen under the ruling. A user
  choosing a card deserves to know that before filing the report. Same
  hand-kept catalog, one more word. *~2 h.*
- 🟡 **Demote what is billed above its verification.** The 3D voxel view (one
  math test, no other pin) has a hardware-table row plus a video bullet; the
  HGR paint + sprite editors have two bullets, two panels and a Tools group for
  ~2 700 lines **no test can reach by construction** and that are duplicated
  verbatim into POM1. Keep both features; drop the billing and say they are
  frozen. Name Floppy Emu's four supported modes rather than listing it flat.
  *~1 h.*
- 🟡 **Fix the fresh-install `][+` fallback, which is dead code.**
  `MainWindow.cpp:782-786` sets `defaultProfile = iiePresent ? "iie-pal" : ""`,
  and an empty string means `applyProfile` never runs — so
  `cfgAppleIIPlus`'s `roms/apple2p.rom` probe is **never reached on a first
  run**. A user holding only `apple2p.rom`, `apple2o.rom`, `apple2c-32Kv0.rom`
  or `apple2e_unenh.rom` gets "NO ROM", while README:231 promises the fallback.
  The rest of the first-run path is in good shape: Welcome opens, nothing
  crashes, the boot refusal points at Help ▸ Welcome. *~2 h.*
- 🟢 Dead README cross-reference (`§ Disk images` does not exist; it is
  `### 💿 ROMs and media`), and `roms/ae_transwarp_1.4.bin` is advertised while
  the dashboard records it as undumped. *~1 h.*

### G4 · A release that can be rehearsed 🔴

*The sharpest fact in this gate: commit `955fa2b` pinned linuxdeploy after the
moving tag broke the v0.9.0 build — and it touches a file only a release runs,
so **the fix has never been executed**. That is not a hypothetical; it is the
same shape as the ratchet that returned 0.*

- 🔴 **A scheduled release rehearsal.** `grep stage_data|build_dist|
  package_macos|package_windows|build_appimage .github/workflows/ci.yml` → zero
  hits; `release.yml` has seven. Eleven packaging paths run **only** on a tag.
  Add `schedule:` to `release.yml`; the `publish` job is already gated on
  `github.ref_type == 'tag'`, so nothing publishes. Generalisation worth
  keeping: *every path that only runs at release needs a scheduled dry run.*
  *~½ d.*
- 🔴 **Pin the moving dependencies.** Each is a release-day landmine, and the
  publish job requires **exactly seven artifacts** (`release.yml:883`), so one
  dead job kills the whole Release:
  * `emsdk version: latest` (`ci.yml:404`, `release.yml:801`) — a wholly
    unpinned compiler;
  * `debian:bookworm` floating (`release.yml:416,538`, `pi400.yml:76`) — base
    for two of four Linux packages, plus unversioned `apt-get` inside them;
  * every `actions/*` pinned by moving major tag, with `checkout@v4` in ci.yml
    against `@v5` in release.yml — drift already visible;
  * the cross-repo GHCR image `habib256/pom1-bionic-builder` (digest-pinned,
    but a permission change in **another repo** breaks the flagship package —
    the workflow's own comment warns of the 403).
  Already good and worth not disturbing: the imgui commit assertion, the
  sha256-checked AppImage tools, GLFW's tag+hash, the vcpkg baseline, the Tom
  Harte per-file manifest. *~2-3 h.*
- 🟠 **`build_dist.sh` (.deb + tarball) runs in no workflow and is advertised at
  README:500.** Add it to the rehearsal or delete the claim. `stage_data.sh
  --self-test` is likewise called by nothing outside `bundle_manifest`. *~3 h.*
- 🟠 **`bundle_manifest` is thinner than it reads.** It verifies the manifest
  parses and every listed path exists, and negative-tests **only `DENY[0]`**
  (`stage_data.sh:174-186`). It never plants a `denyglob` hit, never tests the
  `wasm`-only rejection path, never cross-checks that CMake's `install()` rules
  and the WASM `--preload-file` list — which parse the same manifest
  independently — agree with the shell script, and never runs against a real
  `.app` / `.zip` / AppImage. Given G1, this is the guard that must actually
  hold. *~3 h.*
- 🟡 **Version strings.** The single-source-of-truth claim **holds for compiled
  code** and the release even asserts tag == `PROJECT_VERSION`. It does not
  hold outside code: README carries **12** hardcoded `v0.9.0` strings including
  package filenames, `CLAUDE.md:307`, and `vcpkg.json:4` is **already stale at
  `"0.8"`** — proof the list is not being walked. `docs/releases/v1.0.md` must
  exist and its name must match `PROJECT_VERSION` exactly, or the Release body
  silently degrades to a generated commit list. Add a CI grep for
  `v${PROJECT_VERSION}` in README. *~2 h + 2 h.*
- 🟢 `roms/341-0358-A.bin` is mode 700 unlike every sibling at 644, copied
  verbatim by `stage_data.sh` — unreadable to other users after a system-wide
  install. *5 min.*
- 🟢 No `CONTRIBUTING.md`, `SECURITY.md` or issue templates; the Pages deploy
  lives in `ci.yml` rather than `release.yml`, so the live demo and the shipped
  `web-wasm.zip` can be different builds and the README never says so.

### G5 · The donut: policy without a test 🟠

*The measurement is in [The shape of the risk](#the-shape-of-the-risk). This
gate is the part of it that 1.0 needs; the rest is an investment that outlives
1.0.*

The distinction that makes this tractable: of the ~21 700 untested UI lines,
**about 5 300 hold policy and about 14 000 hold painting.** Painting genuinely
needs a GL context and genuinely decides nothing. Policy needs no context at
all — it decides whether a session's work reaches the disk. Chase the 5 300.

Densest policy files, none of them linked by any test:

| File | Lines | policy hits / ImGui calls |
|---|---|---|
| `MainWindow_Session.cpp` | 358 | **81 / 0** — the entire shutdown persist |
| `MainWindow_Media.cpp` | 540 | **30 / 0** — mount/eject/boot routing |
| `MainWindow_Slots.cpp` | 1 627 | 65 / 162 — `applyProfile` + `kDefaults[]` |
| `MainWindow_SlotConfig.cpp` | 791 | 8 / 0 |
| `AudioCoordinator.cpp` | 444 | 22 / 0 |
| `CliRunner.cpp` | 238 | 0 / 0 — every deferred CLI action |

- 🟠 **G5-1 · One contract test across the parallel media paths.** *≈1 d.* The
  direct antidote to the drift shape. Enumerate every medium — `DiskImage`,
  `Disk35Image`, `Block512Backing`, the SmartPort units — and assert one rule
  on each: what `isWriteProtected()` reports with write-back off, whether an
  eject clears the persisted path, whether a mount preserves the write-back
  opt-in, whether a flush is a no-op when nothing is dirty. Where a path
  diverges **on purpose**, the test states the divergence. It has a live
  target: `SmartPortUnit.h:92-95` declares the contract as *"physically WP OR
  no write-back opt-in"*, `SmartPort35Unit.h:50` honours it and
  `SmartPortHdvUnit.h:60-62` contradicts it — two bays of one card, one panel,
  one toggle, opposite answers. **That also settles standing ruling R1.**
- 🟠 **G5-2 · The same shape for slot-card snapshots.** *≈4 h.*
  `tests/card_snapshot_state_test.cpp` covers 6 of 21 cards and asserts exactly
  the right thing for each, including *"every loader must ignore a foreign blob
  rather than misparse it"* — the property `MachineSnapshot.cpp:199-201` relies
  on. `LironCard`, `PhasorCard` and `IIcExternalSmartPort` have no round-trip
  test anywhere. Generalise the file into a loop over `SlotCardFactory`'s
  catalog: ~80 lines, covers 21 cards today and every future card for free.
  *(The related fear that cards lack magic/version was checked and is
  unfounded — all ten inspected loaders check both inline.)*
- 🟠 **G5-3 · The same shape for device clocks.** *≈3 h.*
  `EmulationController::setVideoStandard` retunes memory, speaker, cassette and
  every slot card, and `SlotPeripheral::setCpuClock` defaults to a no-op
  (`SlotPeripheral.h:107`). `IWMDevice` and `Sony35Drive` override nothing and
  hardcode the compile-time NTSC constant (`Sony35Drive.cpp:92-96,325,332`), so
  on both PAL profiles 3.5" timing runs ~0.7 % off — the same drift the
  speaker/cassette retune exists to remove, except that one is documented and
  deliberate and this one is neither. Extend `pal_timing_test.cpp` to enumerate
  every clock-bearing device and assert each reports the PAL clock.
- 🟠 **G5-4 · Split `StorageCoordinator`'s pure half.** *≈1 d.*
  `captureRebuildSnapshot`, `persistRebuildSettings`, `persistSessionSettings`
  and the snapshot structs need no `EmulationController`. Extracted, ~600 lines
  become testable without linking the emulator, and
  `tests/storage_rebuild_persist_test.cpp` stops dragging **19** sources. The
  2026-09-04 resync gap would have been impossible to introduce. The coordinator
  itself is otherwise in good shape and worth not re-auditing.
- 🟠 **G5-5 · A CLI test, and a usage/parser symmetry check.** *≈4 h.*
  `CliRunner.cpp` is linked by **zero** test targets — `cli_kiosk` links the
  *parser* only. Unpinned: `--load`, `--run`, `--paste`, `--step`, `--tape`,
  `--save-tape[-format]`, `--35-disk1/2`, `--display`, `--cpu-max`,
  `--ai-control`, `--play/--rec/--rewind`, `--snapshot-save/-load`,
  `--rgb-card-invert-bit7`, `--trace-brk`, `-h`. And `--prodos-folder` **parses
  but is absent from `printUsage()`** — shipped and undocumented, in the file
  CLAUDE.md names as the source of truth. A table-driven test plus a symmetry
  check that fails when a flag exists in one and not the other catches that
  today and the whole class after. **Trap worth knowing**: `pom2_headless` has
  its own second parser, and it is the one CI exercises — the parser users get
  is the one nothing runs.
- 🟡 **G5-6 · `applyProfile`'s 14 steps.** *≈1 d.* `system_profile_smoke`
  states in its own header that it cannot reach `MainWindow::applyProfile`, and
  replays five of fourteen steps by hand — not the ones that can lose data
  (slot-bus rebuild ordering, remount after cold reset, rewind clear,
  video-standard change, persistence gating). Nine profiles, a CLI flag and a
  menu all lead here, and it holds `stateMutex` across file I/O on purpose.
  Extract the fourteen steps into a free function taking
  `(EmulationController&, SlotBus&, Settings&, SystemProfile)` — no ImGui
  needed — and assert per-step post-conditions for //e→//c, NTSC→PAL, //c→//e.
  Same refactor shape as G5-4.
- 🟡 **G5-7 · Settings key symmetry.** *≈3 h.* `settings_roundtrip` pins the
  storage engine thoroughly (`#` mid-value, embedded newlines, float precision,
  oversized-file rejection) and exactly one production key. It proves nothing
  about `system_profile`, `slot_N_card`, `hi_res_mode`, `chatmauve_variant`.
  A latent instance is already in the tree: `mockingboard_volume/_muted`,
  `phasor_*` and `echoplus_*` are **read with a hardcoded default and written by
  nothing** — harmless only because no slider exists yet. Collect every literal
  passed to `Settings::get*`/`set*` and fail on a reader with no writer, or the
  reverse, outside an allowlist.
- 🟡 **G5-8 · Settle the `hdv_path` guard, which is described three ways and
  implemented two.** *≈2 h.* `MainWindow_Session.cpp:70-71` says *skip*; its
  `else` at `:83-85` **clears**; the sibling `hdv_writeback` sixty lines below
  gets it right and claims to implement *"the same guard"*.
  `StorageCoordinator.cpp:474-485` repeats the asymmetry, and
  `storage_coordinator_test.cpp:225-227` **pins the surprising branch** — the
  test locks in the divergence rather than the contract. The `else` also fires
  when no HDV card is plugged at all, which is the shipped default map and both
  //c profiles, so `hdv_path` is cleared on nearly every quit. Damage is bounded
  (a stale path, not data); it is listed because it is this file's own
  "prose substituting for verification" risk caught in the act, in the densest
  policy file in the untested set.
- 🟠 **G5-9 · Notify on a red nightly.** *≈1 h.* `ci.yml:258-261` runs the
  sanitizers on `schedule` only, with no failure path anywhere in the job; push
  CI is a different workflow and stays green. The three defects fixed on
  2026-09-04 sat invisible since 09-03. A few lines of YAML, and the
  cost/benefit is absurd in its favour.
- 🟡 **G5-10 · A CI leg with software GL.** *≈1 d.* Mesa llvmpipe + Xvfb.
  `crt_barrel_view` is `EXCLUDE_FROM_ALL` with **no `add_test`**, so a whole
  half of the render path — mask pitch, bandwidth — rests on review. More
  importantly it is the precondition for ever reaching the 14 000 painting
  lines, and for `tests/frontend_device_panel_concurrency`, the headless ImGui
  frame driven while cards are replugged that would close the UI-deadlock class
  rather than scanning for it with `tools/check_coordinator_locks.sh`.
- 🟢 **G5-11 · Pin the hot path's hash identity.** *≈1 h.* `docs/PERFORMANCE.md`
  § 9 requires every optimisation to leave `pom2_bench`'s RAM and framebuffer
  hashes byte-identical, and the only automated gate for it lives in the
  **Raspberry Pi release job**. One `add_test` against a checked-in golden turns
  the project's central perf discipline from a review convention into a gate.
  The bus fast path itself is exemplary — `bus_fastpath_test.cpp` is
  differential over 1024 paging states × every address, and is the model the
  rest of the tree should copy.
- 🟢 **G5-12 · WASM CI is compile-only.** *≈1 d.* The job builds and checks
  three files are non-empty; it never boots the module, never touches
  `PersistentFs`/IDBFS. The browser build **never destroys its `MainWindow`**,
  so the only thing that persists a visitor's state is a 10-second heartbeat. A
  regression that compiles but breaks the mount passes green and surfaces as
  *"my browser forgets everything."* A node smoke — load, run N frames, write a
  setting, `pom2_persist_now()`, reload, assert — closes it.
- 🟢 **G5-13 · The three transports still at 0 %** (`SlirpNetworkBackend`,
  `SpSerialTransport`, `SuperSerialTcpTransport`). Seams exist for all three
  (`ssc_transport_seam`, `fujinet_link_seam`), so they are testable in a way
  they were not before the 2026-08-27 seam work.
- 🟢 **G5-14 · Extend `-Werror` to the GCC leg.** It is on for macOS only
  (`ci.yml:195`). GCC's warning set is not clang's — transitive includes, the
  `-Wmaybe-uninitialized` family — so build the Linux job once with it, fix
  what it names, then wire it. It is the leg that catches what clang does not.
- 🟢 **G5-15 · `ctest -L rom` + ROM Status "degraded".** No `LABELS rom` exists
  anywhere (only `slow`), so every ROM-gated test SKIPs silently when a dump is
  absent and the L0 path can rot behind a green suite. ROM Status reports
  *missing*, never *"running the synthetic fallback"*. Fold in the related hole:
  nothing asserts the real ClockCard ROM path is taken when the dump is present,
  and the same is true of Disk II P6, the mouse MCU and the Grappler EPROM. →
  `docs/lle_vs_hle.md` § Keeping a level once you have it.

### G6 · The platforms we claim 🟡

*A decision, then possibly work. Either answer is defensible; shipping without
choosing is not.*

- 🔴 **Windows ships a binary against which no test has ever run.** Both
  `ci.yml:230-236` and `release.yml:707` set `-DPOM2_ENABLE_TESTS=OFF`, and the
  CI comment concedes: *"the suite has never been built for MSVC."* README
  lists Windows as a first-class platform. **Either** get a core subset green
  under MSVC (*~1 d for a first 20 headless tests; 2-4 d for the unknown
  portability backlog*) **or** say plainly in README that Windows is
  build-verified only. The first is better; the second is honest; the current
  state is neither.
- 🟡 **Linux aarch64 / Raspberry Pi are never built outside a release** and
  never tested at all — three of the seven shipped packages. The rehearsal in
  G4 covers the build; rendering on real hardware cannot be proven by CI and
  should be a checklist line.
- 🟡 **macOS x86_64 slice is never executed** — no Rosetta on the runner, so
  the universal binary gets a structural `lipo` check only.
- 🟢 **Notarization / signing.** Both macOS and Windows refuse the first launch;
  README documents the workaround. Absent from this file entirely until now. A
  1.0 where two of three desktop platforms show a security warning reads as
  unfinished. *~1 d + $99/yr Apple, $200-400/yr Windows EV.*

### The 1.0 release checklist

Keep this in the repo and walk it. Every line is falsifiable.

```markdown
## Legal — all must be YES before tagging
- [ ] No commercial disk images in the work tree (done 2026-09-05) AND none
      reachable in git history, or the history risk accepted in writing
- [ ] The web demo boots a disk that exists and is freely licensed
- [ ] The web demo's boot disk is freely licensed; provenance recorded
- [ ] The bundled-firmware decision is made, and README + RomStatus_ImGui +
      MainWindow_MiscPanels + packaging/roms_README.txt all agree with it
- [ ] THIRD-PARTY.md exists (MAME, AppleWin, Dear ImGui, GLFW, DejaVu,
      Font Awesome, the two pic/ photos)
- [ ] fonts/ ships its two license files

## Version
- [ ] CMakeLists.txt project(... VERSION 1.0 ...)
- [ ] docs/releases/v1.0.md written — the filename MUST equal PROJECT_VERSION
- [ ] README's 12 version strings, CLAUDE.md § Version string locations,
      vcpkg.json version-string (stale since 0.8)
- [ ] grep -c 'v0\.9' README.md == 0

## Build repeatability
- [ ] Release rehearsal green within the last 7 days
- [ ] emsdk pinned; debian:bookworm pinned by digest; actions/* pinned by SHA
- [ ] ghcr.io/habib256/pom1-bionic-builder digest still pullable from this repo
- [ ] ./build_dist.sh (.deb + tarball) builds — the path CI never touches
- [ ] packaging/stage_data.sh --self-test passes

## Platform truth
- [ ] Windows: core ctest subset green under MSVC, OR README says
      "build-verified only"
- [ ] libslirp/Uthernet I: README's per-platform claim matches each package
- [ ] Raspberry Pi packages launched on real hardware
- [ ] README § Known Limitations reconciled against this file

## First run
- [ ] Fresh profile + empty roms/ on each platform: Welcome opens, no crash
- [ ] A roms/ holding only apple2p.rom resolves to a working ][+
- [ ] Every internal README anchor resolves
- [ ] The live demo serves the 1.0 build

## Tag
- [ ] 7 packages + SHA256SUMS.txt attached; body is v1.0.md, not generated notes
- [ ] Download one package per platform and launch it
```

## The scope ruling

*This answers the question that nothing in this repo answered before
2026-09-05, and it is what makes every list below finite. The value is not the
classification — it is being allowed to say "no" to the tail **in writing,
once**, instead of re-deciding it every session.*

**The criterion.** A subsystem is **core** if and only if a silent regression in
it breaks one of three things: (1) the **DIX** run and the rest of
`docs/test_corpus.md`; (2) the **`Apple //e Enhanced PAL` fresh-install
profile** booting and running a disk; (3) the **user's files** — write-back, the
atomic commit, the persistence policy that decides whether a session's work
reaches the disk. Nothing else is core, however well built. Being
verbatim-from-MAME, being pinned, being in the README, and having been expensive
to write are **not** arguments for core: they are arguments that the thing
works, which is what *supported* means.

**What each bucket obliges.** **Core** — kept at oracle parity, carries goldens
(hashes, not smoke assertions); a regression blocks a release. **Supported** —
it works and is pinned; bugs are fixed on report; **no proactive fidelity work,
no gap-closing, no backlog grooming**. **Frozen** — present and shipped,
documented as frozen in README and in the card picker; no promise; its open
backlog items are closed as *won't do* unless somebody arrives with a need.

**Core is 14 rows out of ~60. That ratio is the point.**

### Core

| Subsystem | Why | What it obliges |
|---|---|---|
| **6502 / 65C02 / Rockwell / WDC** | 100 % Tom Harte on 178 NMOS opcodes + both Klaus suites; Crazy Cycles II uses `JMP (IND)` 5-vs-6 as a CPU *identifier* | A timing change needs a Harte diff, not a smoke test |
| **Memory / IIe paging / aux / LC / floating bus** | `floatingBus()` is what makes vapor lock possible; DIX needs 128 K aux | `bus_fastpath` is the differential oracle for any `memRead` change |
| **Video standard + frame timing** | DIX is PAL-only; `DEFAULT_SYNC_TIMER=7479` places its effects vertically | `pal_timing` + `video_event_publish` are goldens; every `emuCycles` device must take `setCpuClock` (see G5-3) |
| **Display — beam-raced reconstruction** | 14 211 mode events over 2 500 DIX frames; MODPAGE ~1 switch/scanline | `display_golden_hash` (164 pins) may only grow; a re-hash needs a stated reason |
| **Composite NTSC (OpenEmulator)** | The fresh-install pipeline — the first thing every user sees | `oe_demod_gpu_cpu_parity` + `text_oecpu_crisp` block a release |
| **Speaker** | The only audio a default machine has before any card | Verbatim; `speaker_smoke` + `speaker_overflow` block a release |
| **Mockingboard A/C (6522 + AY)** | DIX's whole frame sync is the T1 IRQ; the OLDSKOOL crash and the "DIX raster offset" were both 6522 timing | `via_t1_*`, `via_t2_timing`, `mockingboard_t1_irq_phase` are goldens. **A Mockingboard test is credible only once shown to FAIL against the reverted fix** |
| **SlotBus + wire-OR IRQ** | One aggregation bug silences the whole corpus | `slot_bus_smoke`, `irq_aggregator_smoke` block a release |
| **DiskImage / WOZ / Disk II LSS** | Real P6 PROM + flux model; the boot path of every 5.25" corpus title | Keep the MAME port verbatim; `mame_lss_parity` is the oracle |
| **SmartPort card, Liron-class HLE (`smartport35`)** | DIX's actual boot path (`disks_3.5/DIX.po` at slot 5) | The `$Cn0A`/`$Cn0D` entry convention is frozen contract |
| **Media write-back + durability** | Criterion (3) — the only defect class that destroys what a user cannot regenerate; three paths drifted in one day | **G5-1 is a core obligation, not a nice-to-have** |
| **Host-side storage policy** | Both causes of the 2026-09-04 HDV report, and the G2 eject defect, live here | **G5-4 is a core obligation.** The pure half must test without linking the emulator |
| **System profiles + reset architecture** | Everything above is selected by it; an ordering bug presents as a defect in whatever card loaded last | `system_profile_smoke` + `slot_config_smoke` block a release; G5-6 closes the gap |
| **Le Chat Mauve — Féline + Eve variants only** | The one non-DIX admission, made deliberately: the project's differentiator, two golden suites, its own corpus section | Keep both suites frozen. **Scope is the two variants** — `rvb` and the //c-adapter quirk are frozen |

### Supported — works, pinned, fixed on report, no proactive work

Z80 core + SoftCard *(a finished subsystem — its cleanup list is quality, not
correctness: do not schedule it)* · RamWorks III above 128 K · AppleWin NTSC,
artifact-colour LUT modes, mono phosphor, CRT glass pass *(no ctest at all until
G5-10; say so)* · Cassette · Mockingboard Sound II, SSI263, Cricket/Echo ·
Phasor *(the missing cycle-stamped queue is now a **stated limit**, not an owed
fix)* · Floppy mechanical sounds · Disk II drive 2, `.d13`, `.nib`/`.nib2`,
2MG, MacBinary, skew sniffing · CFFA 2.0 *(**CHD closed as won't-do**)* ·
ProDOS HDV card *(its write-back is core; its device model is not)* · ProDOS
host folder · IWM + Sony 3.5" + SmartPort hub · Liron card *(the fidelity
alternative to the core `smartport35`)* · //c-class on-board SmartPort + the
`$C500` stub + `IIcExternalSmartPort` · Super Serial Card + telnet *(the "real
SSC ROM" move is closed as won't-do)* · Uthernet II *(**`LISTEN` closed as
won't-do**)* · FujiNet relay *(relay side only; three of its five open items are
upstream bugs POM2 has nothing to fix; the native device's phases 2-4 are
unscheduled)* · TNFS media · Grappler+, parallel PrinterCard, ImageWriter II +
PDF, screen dump, print history *(`printer_plan_2`'s remaining phases are
unscheduled; the `ImageWriter.cpp` file-size debt is still owed as a **ratchet**
obligation)* · ThunderClock+ and No-Slot Clock · Mouse Card, both models ·
Joystick / paddles / 4play *(**4play is complete** — mark it so)* · TransWarp ·
Rewind + snapshot *("redo" and writable-WOZ undo closed as won't-do)* ·
Debugger and memory panels · AI control server + SDK *(a break here is urgent —
the project's own verification method depends on it)* · CLI + kiosk · Panel
registry, theme, docking, palette · WASM build · Packaging.

### Frozen — present, shipped, no promise

| Subsystem | Why frozen |
|---|---|
| 🧊 **Echo+ TMS5220** (`echoplus_tms`) | A stub kept for detection; nobody writes the TMS5220. **Closes P3-2 as *hide*** → G3 |
| 🧊 **Apple II Workstation Card** | Steps 2 and 3 are ❌ with no network on the other end; it buys an alternative transport for PostScript the SSC already carries, and doubles emulation cost while plugged. **Close items 2 and 3** |
| 🧊 **Zilog Z8530 SCC** | Its only consumer is the card above; SDLC is datasheet-derived with no MAME oracle. `scc8530_smoke` stays as a build guard |
| 🧊 **Uthernet I + libslirp backend** | No Windows transport (vcpkg's port drags glib into CI), none on WASM, and absent from the .dmg and the x86_64 AppImage. Uthernet II covers the need everywhere with no dependency. **Close both transport items** |
| 🧊 **Le Chat Mauve `rvb` variant + the //c-adapter quirk (P4, P5)** | P4 is gated on a manual that has never surfaced; P5's only source sits behind a proof-of-work wall `WebFetch` cannot pass. Deliberately unmodelled rather than invented |
| 🧊 **3D voxel view** | A framebuffer effect with a math test and no other pin, no corpus, no reports. 431 lines that work. Close the heightfield-mesh option |
| 🧊 **HGR/DHGR paint + sprite editors** | **Unreachable by ctest by construction**, which is how three OOB accesses survived four weeks green — *and* duplicated verbatim into POM1, so every fix must be applied twice with nothing flagging the omission. Ship it, add nothing. The one admissible piece of work is the shared `EditorTestAccess` seam, and only on a report |
| 🧊 **Floppy Emu** | 4 of 6 modes; the two missing were already "out of scope for v1" |
| 🧊 **Printer mechanical sounds** | Synthesised because **no sample set exists** — there is no oracle and never will be |
| 🧊 **PostScript by delegation** | Delegates to a host Ghostscript; document the dependency, no in-process renderer |
| 🧊 **WASM browser extras** (file picker, touch input, worklet latency, 50 Hz RAF) | Open since the WASM build landed; the demo is blocked on G1, not on code |
| 🧊 **Apple ][ Original (1977) profile** | No profile-specific test, no corpus title, no report. Do not chase rev-0 quirks |
| 🧊 **The whole `a2bus` port survey** + Saturn 128K LC + Passport MIDI + 8-bit DAC + Apple II VGA + E-Z Color | **This is the tail the ruling exists to say no to.** A card leaves *Parked* only when a named piece of software needs it |

### The four cards missing from `docs/lle_vs_hle.md`

They have no row in the doc and no key in `abstractionCatalog()`, so the picker
shows no level for them. Land both in one commit — they are kept in step by hand.

| Card | Level | The seam to record |
|---|---|---|
| `liron` | **L2 firmware + L0 media path, H2 drive** — a composite | The UniDisk's drive-side 65C02 is not emulated; the protocol is the contract. Say explicitly that this is the strictly *lower* sibling of the `smartportcard` L2 veneer, since the picker now shows both |
| `workstation` | **L0 CPU + L1 SCC, with no transport** | A third failure category, the one the FujiNet's "no peer" row also names: **the network on the other end does not exist.** Level is high; reach is zero |
| `4play` | **L1 — and complete** | Nothing below the register to model. Needs an explicit *complete* marker, or "L1" reads as "something is missing" |
| `transwarp` | **L1 registers / host retiming** | The doc's first entry whose abstraction is in the **time domain**. Multiplier sampled once per frame: unbiased in aggregate, wrong about where in a frame the slow cycles land |

## The shape of the risk

Measured 2026-09-05, re-measured the same day. Numbers from `src/` excluding
`third_party/`.

**The test net is a donut.** 71 413 lines of tests against 159 339 of source is
a serious ratio, and the emulation core is where they are. **26.5 % of
`src/*.cpp` — 27 816 lines of 105 108 — is linked by no test at all**, and it is
not spread evenly: it is the **host side**. ~21 700 lines of `MainWindow_*` /
`*_ImGui`, plus 1 934 lines of coordinators. 55 of 155 `src/*.cpp` are linked by
nothing.

**But the donut has two halves, and only one matters.** ~5 300 of those lines
hold *policy* — `MainWindow_Session.cpp` is 358 lines with 81 settings calls and
**zero** ImGui calls — and ~14 000 hold *painting*. Policy needs no GL context
and decides whether a session's work reaches the disk; painting needs a context
and decides nothing. The earlier framing ("the UI needs a GL context") was the
reason this region went unexamined; it is wrong for the half that matters.
→ [G5](#g5--the-donut-policy-without-a-test-).

**And the coverage ratchet is structurally blind to it.** `tools/coverage.sh`
says outright that its denominator is *"the code the test suite LINKS, not the
whole program"* — so the 78.39 % floor is 78.39 % of 73.5 %, about **58 % of
`src/`**, and adding a thousand untested `MainWindow_*` lines moves the floor by
**zero**. The metric cannot regress in the region where both 2026-09-04 defects
and the G2 eject defect were found.

**The signature failure is drift between parallel paths** — two siblings, one
updated. The three instances recorded on 2026-09-04 are **all fixed** (write-back
resync `8991c33`; CRT mask pitch `1c38db0`; write-protect, which G5-1 settles).
The four found on 2026-09-05 replaced them, which is the point:

| Contract | Sibling A (right) | Sibling B (wrong) |
|---|---|---|
| Eject → settings key | `StorageCoordinator::ejectMediaBay` | `MainWindow_Media.cpp:98` — wrong drive key, no generic-media branch, no guards, lock held over file I/O |
| AY reset counter | `Mockingboard.cpp:772` bumps | `PhasorCard.cpp:325` zeroes |
| `setCpuClock` fan-out | speaker, cassette, every slot card | `IWMDevice` / `Sony35Drive` hardcode the NTSC constant |
| Atomic write-back | ten call sites incl. the PNG export | `ImageWriterPdf.cpp:181-191` writes straight onto the destination |
| Write-protect (still open) | `SmartPort35Unit.h:50` honours the base contract | `SmartPortHdvUnit.h:60-62` contradicts it |

Each path is tested on its own; nothing asserts that all of them obey the same
rule. **That is what a contract test is for**, and G5-1/2/3 are three of them.
The pattern has now recurred across four unrelated subsystems in two days, which
makes it the tree's most productive place to look.

**Three blind spots, by construction rather than by neglect:**

- *Anything needing a context is unpinned.* `crt_barrel_view` is
  `EXCLUDE_FROM_ALL` with no `add_test`; the WASM job checks three files are
  non-empty and never boots the module. Half the render path and the whole
  browser-durability path rest on review.
- *The nightly can be red without anyone knowing.* Sanitizers run on `schedule`
  with no failure path; push CI is a different workflow and stays green.
- *A path exercised only at release can only break at release.* Eleven packaging
  paths, and the linuxdeploy fix for the v0.9.0 break has itself never run.

**What was checked and found solid**, so it does not need re-auditing:
`StorageCoordinator` itself (three-phase eject, auto-provision guards, generic
keyspace, CFFA-outranks-HDV); the HDV family's now-identical `detachImage`;
`bus_fastpath_test.cpp`; IRQ wire-OR across all seven raising cards; the display
painters' single line-count source of truth; and the foreign-blob magic/version
check in all ten card loaders inspected.

## Standing rulings

Decisions, not work. Do not re-litigate without new evidence.

- **R0 · Do not grow the god-objects.** A new card gets its panel in its own
  `*_ImGui.cpp` and **zero** business logic in `MainWindow.cpp`. `cmake` fails
  if any `src/MainWindow*.cpp` passes 2 000 lines; `tools/check_file_sizes.sh`
  fails if any first-party file passes its recorded ceiling. The rule went from
  5 590 to 11 511 lines while it was only written down — that is why it is wired
  to a mechanism. *(The 2026-08-28 decomposition is done: `MainWindow.cpp` is
  1 408 lines, every "left" sub-item moved, the family is under the cap.)*
- **R1 · Write-protect: one rule per card.** Two bays of the *same* SmartPort
  card answer "can I write?" differently. Either rule is defensible; one card
  doing both is not. Physically write-protect belongs to the medium; the
  counter-argument is that accepting a write with write-back off loses it
  silently. **Settled by G5-1**, which must *state* the divergence where one is
  deliberate rather than paper over it.
- **R2 · Echo+ TMS5220: ship or hide.** A detect-only stub in the catalog is the
  wrong third option. **Answered by the scope ruling: hide** → G3.
- **R3 · One `Config`** (env → CLI → Settings → defaults), consistent `pom2::`
  namespace (255/336 files today, up from 163/233). Hygiene for the second
  contributor. Post-1.0.
- **R4 · The file-size debt is owed, not forgiven.** Two ceilings were raised on
  2026-08-31 to get a gate green that had been red since 08-29 — in 15 s, before
  the Linux job compiled anything, so it was also hiding that build. Both are
  recorded at their **exact** current size, so the ratchet fails on the next line
  either gains. `src/ImageWriter.cpp` 2 501 wants splitting (the head, the paper
  tray, PDF export and the PostScript/screen-dump seams are separate concerns in
  one TU); `src/Memory.cpp` 2 442 is 40 lines, one of which is the foreign-bus
  dispatch. *~1 d for ImageWriter, less for Memory.* Post-1.0.
- **R5 · A card CPU gets a `Memory::ForeignBus`, never a branch in `M6502`.**
  → CLAUDE.md, `docs/PERFORMANCE.md` §§ 8.2/8.5/9.
- **R6 · MAME path drift refresher** — re-check upstream renames ~every 6
  months (recent: `wozfdc.cpp` `bus/a2bus → machine`).

## Open, and known to be open

Findings **deliberately not fixed**, because the reason for leaving them is part
of the finding.

- 🟢 **Blocking work under `stateMutex` — what is LEFT.** The 2026-08-22 audit
  found ~20 sites and fixed the structural cause (`MediaMount.h`: read + decode
  unlocked, swap under the lock). What remains was examined on 2026-08-23 and
  left on purpose:
  * `slotBus().clear()` on a profile switch is not a machine freeze —
    `applyProfile` has already stopped the CPU worker, so only the UI blocks,
    during a modal full cold reset.
  * ~~The FujiNet **Stop / Drop-peer** buttons genuinely need the lock~~ —
    **done 2026-09-06** (bug hunt, `958d86e`): the exclusion moved onto the
    link's own `callMtx_` as described here, `NetworkCoordinator` resolves the
    card under `lockState()` and drives the link off it, `enumerateDevices`
    re-reads its stop flag per unit. `~FujiNetCard` no longer waits the 2 s
    helper grace under `SlotBus::plug` either (`ChildProcess::stopDetached`).
  * **Still under the lock after the 2026-09-06 hunt:**
    `StorageCoordinator::ejectAllMedia` ejects Disk II, block, SmartPort and
    generic bays inline, write-back included, while every single-medium eject
    (`ejectDiskII`, `EmulationController::eject35`, the AI server's `/disk/eject`,
    the firmware 3.5" eject through `WriteBackQueue`) is now two-phase. Same
    fix, four families; a separate job.
  * Deliberate and staying: the profile-switch remount in `MainWindow_Slots.cpp`
    (atomicity against the AI server outranks latency, and the worker is stopped)
    and the outgoing medium's write-back inside `installDisk` (swapping before
    knowing the old medium could be written loses the user's changes).
  * Bounded and documented: the Uthernet II guest DNS wait (`kDnsWaitMs` = 120 ms).
- 🟢 **`persistSession()` reaches `controller->memory()` unlocked, and it is
  safe by construction — but the invariant is unenforced.** `~MainWindow` calls
  `controller->stop()` first, and the WASM heartbeat runs on the CPU-stepping
  thread because the worker does not exist under Emscripten. Nothing in the file
  says so, and nothing stops a third caller from adding a mid-session call on
  the desktop, where the sibling `MainWindow::flushSlotMedia` does take the lock
  around the identical `flushAll`. Worth a comment at minimum.
- 🟡 **ThreadSanitizer: the GUI half is still open.** The controller half is
  done and clean (2026-08-17) — a harness drove the real thread shape without a
  GUI: CPU worker, UI transport verbs, an AI-server thread doing `lockState()`
  reads plus snapshot capture/restore and key injection, the live miniaudio
  callback, and a Mockingboard fed by a guest loop so the emuCycles AY queue is
  exercised. Zero races. **Caveat worth keeping**: TSan instruments the
  interpreter's hot loop, so the CPU manages ~400-1 400 emulated cycles/s — the
  *lock protocol* is covered thoroughly, *emulated execution* thinly. What
  remains is ImGui panels, `demodMutex`, slot re-plug under load — which is the
  same thing G5-10 unblocks.
- 🟡 **`ImageWriterPdf.cpp:181-191` skips `AtomicFileReplace`.** CLAUDE.md says
  every write-back goes through it, and ten call sites do — including the PNG
  export in the same subsystem. Save PDF writes straight onto the destination,
  so a crash mid-save leaves a truncated file where the old one was. Four lines.
  *~1 h.*
- 🟡 **Three divergent copies of the atomic file-write helper** remain:
  `DiskImage.cpp:2311`, `Disk35Image.cpp:329`, `ProDOSVolume.cpp:709` — temp-file
  naming, permission carry-over and error strings hand-repeated. `DiskImage`
  caught up on permission preservation on 2026-08-08 (it had been resetting the
  image's mode to the umask default on every write-back); `ProDOSVolume` still
  has not. The home is `AtomicFileReplace.h`, next to `pom2::replaceFileAtomic`,
  not a new file.

## MAME ↔ POM2 parity (dashboard)

Canonical reference for what is ported and **how faithfully** — the `Parity`
column grades the port against its reference (verbatim → partial-verbatim →
POM2-original → scaffold). The `Known gaps` listed here point to detailed items
in the [backlog](#backlog).

The independent axis — **where the emulation boundary is cut**, at the chip's
pins (LLE) or at the service it provides (HLE) — lives in
[`docs/lle_vs_hle.md`](docs/lle_vs_hle.md). The two do not correlate: a verbatim
port can be high-level (`ImageWriter`) and a POM2-original can be low-level
(`CassetteDevice`).

| #  | Subsystem                  | Parity           | MAME / AppleWin refs                                                     | Known gaps                                                                              |
| --- | ---------------------------- | ---------------- | ------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| 1  | M6502 / 65C02 / Rockwell / WDC | Verbatim         | `om6502.lst`, `ow65c02.lst`; Tom Harte `65x02`                          | 🟢 NMOS 100% Tom Harte on all 178 documented opcodes; 🟢 WDC decimal SBC now silicon-exact (interdigit carry, 2026-07-30); 🟢 $5C 8-cyc = deliberate (matches MAME, not Harte) |
| 1bis | Z80 core (standalone)          | POM2-original (L0) | z80.info/decoding.htm field decomposition (same structure MAME's `z80.cpp` flattens) | — (zexdoc + zexall clean; MEMPTR + X/Y flags modelled, not approximated; pinned `z80_core`, `z80_zexdoc`, `z80_zexall`, `z80_block_io_flags`) |
| 1ter | SoftCard Z80 (key `softcard`)  | Verbatim         | MAME `bus/a2bus/a2softcard.cpp` (R. Belmont, 176 lines; line-cited)      | — (real DMA bus arbitration, 6502 halted per instruction slice; CP/M 2.2 boots MAME-oracle-identical; pinned `softcard_toggle`, `softcard_cpm_boot`, `softcard_cpm_boot_iie`) |
| 2  | Memory + IIe + RamWorks        | Partial-verbatim | `apple2e.cpp:1275-1299`, `a2eramworks3.cpp:108-115`                      | 🟢 Keyboard + PaddleInputs extracted (2026-08-23); `memRead` 256-entry dispatch (perf) still in Memory |
| 3  | Display HGR/DHGR/80-col        | Partial-verbatim | `apple2video.cpp:124-201`, `460-471`, `:751-758`; AppleWin `RGBMonitor.cpp` | 🟢 mono DHGR 1-px (mid-scanline, PAL 50 Hz, floating bus `$C05x`, page-flip DROL, Chat Mauve RGB: done) |
| 3bis | Le Chat Mauve RGB (key `chatmauve`) | POM2 + AppleWin | AppleWin `RGBMonitor.cpp` pixel rules + the Eve/Chat Mauve brevet (MAME has no model) | 🟢 AN3 pulse FIFO decode, Eve Color text `$C0B8/9` + HGR Duochrome `$C0BA/B`, snapshot v2; pinned `le_chat_mauve_smoke` |
| 4  | SpeakerDevice                  | Verbatim         | `spkrdev.cpp:74-327`                                                     | —                                                                                        |
| 5  | CassetteDevice                 | POM2-original    | `apple2.cpp:362`                                                         | —                                                                                        |
| 6  | Mockingboard A/C (6522 + AY)   | Partial-verbatim | `ay8910.cpp:998-1015`, `:1077-1104`, `1309`; `6522via.cpp:959`          | 🟢 Port A read mask by DDR; 6522 subset (SR/PCR; T2 one-shot done, IRQ N+3 MAME)      |
| 6b | Mockingboard "C" Sound II      | POM2 + AppleWin  | AppleWin `source/Mockingboard.cpp` + `source/SSI263.{h,cpp}`             | — (SSI263 at `$Cs40-$Cs44`, A/!R → VIA1.CA1)                                              |
| 7  | FloppySoundDevice              | Verbatim         | `floppy.cpp:1532-1620`, `:2925-3020`                                     | —                                                                                        |
| 8  | SlotBus + IRQ wire-OR          | POM2-original    | MAME slot bus pattern                                                    | —                                                                                        |
| 9  | DiskImage                      | Partial-verbatim | `woz_dsk.cpp`, `flopimg.cpp:2017-2106`                                   | 🟡 WOZ1 splice TRK+6650; 🟢 .nib2/.app, half-tracked NIB (88)                           |
| 10 | DiskIICard                     | Partial-verbatim | `machine/wozfdc.cpp:264-291`, P6 PROM 341-0028-A                         | 🟢 sub-instruction RAII vs per-cycle                    |
| 11 | IWMDevice                      | Verbatim         | `machine/iwm.cpp:1-543`                                                  | 🟢 **window sizes are MAME's own again** (2026-09-01): the state machine runs on the controller's 7.16 MHz clock (`POM2_IWM_TICKS_PER_CPU_CYCLE`), not whole CPU cycles, so 28/14/36/18 are used verbatim and a window edge lands inside a 14.17-tick Sony cell. That plus a flux-query off-by-one is what unblocked the 800K read path — pinned by `sony35_iwm_read_path`. 🟢 Q3 fast clock (Mac/IIgs only) still unmodelled, and no longer load-bearing |
| 12 | SmartPortCard (//e Liron)      | POM2-original (real EPROM → L2) | SmartPort spec + Apple Tech Note; real Liron firmware `roms/liron.rom` (BMOW 4 KB) + real `$Cn0D` dispatch, pinned `liron_smartport_dispatch` | 🟢 multi-partition ProDOS (CFFA3000)                                                     |
| 13 | SmartPortHub + Sony35Drive     | Verbatim         | `apple2e.cpp:638-679`, `mac_floppy.cpp`, `flopimg.cpp:512/967/2017-2106` | —                                                                                        |
| 14 | CFFA (MAME-faithful IDE)       | Verbatim         | `bus/a2bus/a2cffa.cpp`                                                   | 🟢 CHD = phase 2; no media preservation on profile switch                         |
| 14bis | ProDOSHardDiskCard (key `hdv`) | POM2-original (H1) | No MAME/AppleWin analogue — hand-assembled 256 B slot ROM + an invented 4-register streaming port | 🟢 deliberate: mounts `.hdv`/`.2mg` with **no card ROM dump required**; no GCR/flux/ATA below it; `$Cn07 = $01` so the F8 autostart never scans it (use `PR#n` / `bootFromSlot`); pinned `hdv_card_smoke`, `hdv_writeback_smoke`, `hdv_mass_storage_smoke` |
| 15 | ClockCard / ThunderClock+      | Partial-verbatim | `upd1990a.cpp:248-267`, `:312-327`; Thunderware Rev 1.3 EPROM (`roms/thunderclock_u9_v1.3.bin`) | 🟡 MODE_SHIFT lax; 🟡 DATA_OUT live vs MAME latch; 🟢 real EPROM loads from the ctor (synth ROM = fallback, untested from `$C800`) |
| 15bis | NoSlotClock (DS1216E, no slot used) | Verbatim | MAME `ds1216.cpp`; protocol verified against AppleWin `NoSlotClock.cpp` (Nick Westgate csa2 + Dallas datasheet) | 🟢 full 64-bit pattern-match state machine on reads **and** writes (key bit rides on the address); window follows the machine — `$F800-$FFFF` on II/II+, `$C300`/`$C800` on //e + //c-class; injectable time source; pinned `no_slot_clock_smoke` |
| 16 | SuperSerialCard                | Partial-verbatim | `mos6551.cpp:46`, `:542-543`, `a2ssc.cpp:373`                            | — (the SW2:6 IRQ gate landed: `SuperSerialCard.cpp:492` gates `assertIrq` on `irqDipEnabled()`, line-cited to MAME) |
| 17 | MouseCard (MAME)               | Verbatim         | `bus/a2bus/mouse.cpp`, M68705 + MC6821                                   | 🟢 PIA out_a/b without `scheduler.synchronize`                                          |
| 18 | MouseCard (AppleWin HLE)       | Verbatim         | AppleWin `source/MouseInterface.cpp`                                     | — (slot EPROM only, MCU synthesized)                                                      |
| 19 | Phasor (AE — 2×VIA, 4×AY)      | Partial-verbatim | MAME `a2bus/phasor.cpp` + AppleWin                                       | 🟢 EchoPlus mode (=7) routed as native Phasor; stereo L/R per VIA pair done (2026-08-01). 🟡 **no cycle-stamped event queue** — the AY writes are applied when the audio callback runs, not at their `emuCycles` stamp the way Mockingboard's are, so beam-raced register changes quantise to the buffer. Bus decode is verbatim; the audio timeline is not, hence Partial not Verbatim. |
| 20 | SSI263 speech (chip model)     | AppleWin-faithful| AppleWin `source/SSI263.{h,cpp}` (MAME does not implement)                 | 🟢 formant synth → PCM blob, 62 phonemes (AppleWin LGPL → GPL3)                           |
| 21 | EchoPlusCard (Cricket/SSI263, key `echoplus`) | POM2-original | Cricket / Street Elec SSI263 spec (historically mislabelled "Echo+") | 🟢 markadev audit 2026-05-28: the real Echo+ = TMS5220 (see line 21bis)                |
| 21bis | EchoPlusTMS5220Card (key `echoplus_tms`) | Scaffold       | markadev/AppleII-RevEng/Street-Electronics-Corp-ECHO+                  | 🟡 stub register decode (kept for software detection); TMS5220 LPC + AY-3-8913 synth cores deferred. Catalog label says "silent, detect-only" so it does not pose as a working card (2026-08-23). |
| 22 | PrinterCard (parallel synth)  | POM2-original    | Apple II slot 1 convention + Pascal 1.1 sig                              | — (PDF export shipped: `src/ImageWriterPdf.*`, pinned `imagewriter_pdf`)                 |
| 22bis | GrapplerCard (key `grappler`) | Verbatim         | MAME `bus/a2bus/grappler.cpp` (pinned 2026-07-28, line-cited) + markadev 4 KB EPROM (`roms/grappler_plus.bin`) | 🟢 /STROBE 7-clock pulse collapsed to instant (no observer); `ackEffective()` BUSY gate is POM2's back-pressure model |
| 22ter | ImageWriter II printer (host-side, no slot) | Verbatim         | greg-kennedy/ImageWriter (GSport/KEGS/DOSBox lineage) + Apple ImageWriter II/LQ reference manuals | — (full control language, 4-band colour ribbon, 8-/24-pin bit images, paper tray + PNG & multi-page PDF export; fed by `printer` / `grappler` / SSC printer tap (//c PR#1)) |
| 23  | UthernetCard + Cs8900aDevice (key `uthernet`) | Verbatim | MAME `machine/cs8900a.cpp` (VICE lineage) + `bus/a2bus/uthernet.cpp`, line-cited | 🟢 pull-mode RX (POM2 has no `device_network_interface` push bus); inbound frame queue out of snapshot deliberate |
| 23bis | UthernetIICard + W5100Device (key `uthernet2`) | AppleWin-faithful | AppleWin `source/Uthernet2.cpp` + `W5100.h` (MAME has no W5100 device) + WIZnet datasheet v1.2.8 | 🟡 `LISTEN` unimplemented (no inbound path); 🟢 virtual DNS is async, not blocking like AppleWin's |
| 23ter | NetworkBackend (Null / Loopback / libslirp) | POM2-original | AppleWin `Tfe/NetworkBackend.h` shape; libslirp user-mode NAT | 🟢 outbound-only by design (no root); no TAP/pcap path; 🟡 libslirp is Linux/macOS only, so Uthernet I has no transport on Windows |
| 23quater | TranswarpCard (key `transwarp`) | Partial-verbatim | MAME `bus/a2bus/transwarp.cpp` (R. Belmont, 363 lines; line-cited) | 🟢 **deliberate divergence**: MAME runs a SECOND W65C02 DMA-ing the Apple's bus because a MAME card cannot retime the host CPU; POM2 scales `cyclesPerFrame` and keeps the machine's own 6502 — closer to the board and free on the hot path. Register semantics, DIP defaults and the slowdown windows are verbatim. 🟡 multiplier sampled per frame (unbiased in aggregate, wrong for where in a frame slow cycles land); 🟡 ROM shadow gated on an undumped `roms/ae_transwarp_1.4.bin`. Pinned `transwarp_card` |
| 24 | FujiNetCard (key `fujinet`)    | POM2-original (relay) | No MAME device — published SmartPort/SP-over-SLIP spec + the FujiNet AppleWin fork | 🟢 not an emulation: the device is real and off-box, every SmartPort call is forwarded verbatim; no peer → bounded 250 ms stall then SmartPort `$27`; 🟡 **rewind cannot rewind it**; 🟡 not on //c-class (forced INTCXROM masks slot ROM); pinned `fujinet_card` |

## Backlog

**Everything below is post-1.0 by default.** An item leaves this section only
when a gate in [The road to 1.0](#the-road-to-10) names it, or when a report
arrives against a subsystem the [scope ruling](#the-scope-ruling) calls *core*
or *supported*. Items under a **frozen** subsystem are marked 🧊 and are closed
as *won't do* — kept for the reasoning, not as work.

Grouped by subsystem. Severity encoded by 🔴/🟠/🟡/🟢/🧊 at the head of each item.

### [Memory] paging & RAM expansion

- 🧊 **Saturn 128K LC** (Saturn Systems) — 16 banks ×16 KB on LC
  `$D000-$FFFF`, switches `$C080-$C08F` slot-relative. MAME refs
  `bus/a2bus/a2memexp.cpp`. *2-3 d.*
  Frozen by the scope ruling: it leaves *Parked* only when named software needs it.
- 🟡 **`Memory::memRead` hot path** — the multi-level `if` cascade is
  `Memory::memReadSlow`; `memRead` itself is the inline fast path (RAM, ROM
  window, and since 2026-08-20 the //e internal `$C100-$CFFF` ROM;
  `memWrite` has the same split). What remains is the condition chain in
  front of the ROM-window hit (~15 % of a ][+ banner, `PERFORMANCE.md` § 7.4):
  a 256-entry dispatch table per high page would replace it with one indexed
  load, at the price of an invalidation at every paging-state writer. Any
  change here must keep `tests/bus_fastpath_test.cpp` green — it is the
  differential oracle for the fast paths. Prerequisite: `IIcClassProfile`
  extraction (done). Perf job, orthogonal to the `Keyboard`/`PaddleInputs`
  split that already shipped — do not merge the two.
- 🟢 **Dedicated Pascal LC** — 16 KB variant shipped with Apple Pascal,
  minor differences vs IIe LC (write-protect DIP). *1 d.*

### [Display] HGR / DHGR / 80-col

- 🟢 **Golden coverage gaps** (from the 2026-07-12 audit; mostly closed
  2026-07-12 wave 4, table 112 → 164 pins — flash-on phase, PAGE2/80STORE,
  rev-0 HGR+AN3, IIe 80COL+HIRES+MIXED without DHGR, Chat Mauve sub-modes
  all hash-frozen: `iie/text40flash`, `text40page2`, `hgrpage2`,
  `hgr80store2`, `hgran3`, `hgr80colmix`, `textcolorcm`). Remaining:
  ALTCHAR/mousetext + char-ROM glyphs (need a user ROM), PAL beam-raced
  splits (stay behavioural). Also: OE-GPU uploads the unused ~430 KB
  fallback framebuffer every frame (minor perf).
- 🧊 **Pure-analog signal-level composite pipeline** *(deferred, academic)* —
  IIR on the signal itself before demod, against today's 1-bit signal + FIR.
  *5-10 d.* Frozen: an alternative to a pipeline that is already core and green.
- 🟢 **Beam-racing residuals** — `signalPhaseOffset_` stays a per-frame
  constant, so a mid-frame HGR↔DHGR split is approximated; lo-res clips at
  block-row (4 lines), like the RGBA path.
- 🟢 **Mid-scanline split residuals** — 40-col (280) + 80-col (560) mixed on
  the same line is undefined (separate `frame`/`frame80` buffers, scoped
  out). The exact transition cycle at character-clock is no longer a "later
  refinement": DIX showed it as a one-cell error each side. That symptom is
  closed — it was the 6522 T1 read-back bias plus a per-kind column offset,
  both fixed 2026-09-02 (`CHANGELOG.md` of that day; the offset was narrowed
  to AN3/DHGR the same day after it slid MAD EFFECT left). **Back-port to POM1** next (gated: LORES+TEXT rendering on
  GEN2 — HGR-only today — + HBLANK flag Phase 2 per Bernie's spec).
- 🟢 **PAL residuals** — device generator clocks (AY/IWM/SSI263) stay at the
  NTSC nominal (0.7 % delta = inaudible pitch, deliberately not retimed;
  speaker + cassette realtime audio ARE retimed, their queues starve
  audibly otherwise); WASM pacing (RAF 60 Hz) not yet switched to 50 Hz;
  manual NTSC/PAL toggle + auto-PAL when a Chat Mauve card is plugged (the
  two PAL profiles already cover the use case).
- 🟢 **Unidirectional mid-frame page split renders full-page** — assumed
  limit inherited from the DROL page-flip fix; the true remedy is
  incremental per-scanline rendering, MAME-style. → `CHANGELOG.md`.
- 🟢 **"Smooth" interpolated sub-pixel mode** — bilinear/Lanczos on
  HGR/DHGR, UI toggle. Inspired by microM8. *2 d.*
- 🟢 **DHGR mono 1-px alignment + floating-TTL `empty_words` +
  per-scanline mode switch** — cosmetic / out-of-bounds.
- 🟢 **CRT parity refinements vs OpenEmulator** — low-priority residuals from
  the 2026-05-30 video audit (detailed implementation notes →
  `docs/archive/video_parity_revalidation_2026-05-30.md` §4):
  - **F4** POM2 CRT defaults 0.25/0.5/0.4 (scanlines/mask/persistence) vs OE
    ~0.05/0.05/0 — *biggest visual gain*, OR own the "punchy" choice and
    document it (`NtscPostProcessor.h`).
  - **F2** cosine scanline → OE's **sin²** (keep the `scanAA` anti-moiré term).
  - **F7** HGR mono: 280 px average / 3 levels → **560 binary** (copy of the
    DHGR-mono loop already shipped).
  - **F6** row-dim mask ×0.7 ⚠ (make luminance-neutral, don't drop hard).
  - *(Non-items, documented: F1 clamp double > AppleWin float; F8/F9 amber/green
    tints assumed — optional "AppleWin-faithful" preset.)*
- 🟡 **Le Chat Mauve — what is left after P0-P2** (landed 2026-09-01; the
  model as built is `docs/chatmauve_plan.md` § 2.1). **Féline and Eve are
  core** — dot-exact against AppleWin's hardware-validated oracles, two golden
  suites (`purplesoft_eve_screens`, `chatmauve_dot_rules`), the Eve's sixteen
  switches + CPREG auto-write + table IX-1 corrected four times by Purplesoft's
  own code. `rvb` and the //c-adapter quirk are 🧊 **frozen**, both gated on
  sources that have never surfaced (P4's manual; P5's silicium.org thread
  behind a proof-of-work wall `WebFetch` cannot pass). **P3 is closed as
  bounded** (2026-09-02): the PLA is a dot-stream router / cell assembler, NOT
  the colour decoder — reopen only on a schematic or a board trace.
  Remaining and not frozen: **P6's residuals** (the Eve's `$C0Bx` as loggable
  events, the exact in-cell dot position, the TTL-RGBI palette option, DIX /
  PoP golden screens — the first rung, a beam-racing mode latch, landed as
  `chatmauve_latch_split`); the **Extasie and Arlequin goldens** (both boot
  headless now — pin the Arlequin demo-menu screen and an Extasie editor
  screen; Eve Leonard still to find); the **three Féline trim pots** (R/G/B
  gain, manual p. 13), which belong with the `NtscParams::rgbBandwidthMHz`
  pre-pass that gave the card its second connector on 2026-09-04; and **P7's
  last doc step** — fold § 3.4's corrected table back into § 3 prose, and
  rewrite the README's Chat Mauve paragraph.
  Still parked alongside: **Video-7
  AppleColor RGB** (its 160×192 chunky mode and F/B text are the only things
  the plan's Féline decoder does not cover), **Color killer Rev 1**,
  **Strapping RAM 4K→48K**.

### [Audio]

#### Mockingboard output level vs MAME's route gain — open question (2026-08-02)

Raised by "il reste des choses à améliorer au niveau des basses". The
2026-08-02 sweep measured the whole card render chain against MAME and found
it already correct down to 27.5 Hz — volume table within 0.0007 of Westcott's
data, linear channel sum matching `a2mockingboard.cpp`, box integrator with a
sinc gain of 1.0000 below 100 Hz, no stereo cancellation. The one real gap
was the DC blocker (1-pole where MAME uses a 2-pole Butterworth), now ported,
worth ≤0.8 dB below 80 Hz.

That is almost certainly **too small to be what is actually heard**, so the
remaining suspect is level rather than frequency response: POM2 normalises
the per-side chip sum by `/3` (Mockingboard) and `/6` (Phasor), while MAME
routes each AY channel through `add_route(ALL_OUTPUTS, "speaker", 0.5, ch)`.
Those are different scalings, and a card that sits low against the speaker
and disk channels reads as thin. Wants a numbers-first comparison of POM2's
end-to-end output level against MAME's for the same register stream, not more
tuning inside `AyPsgSynth.h`. → `CHANGELOG.md` 2026-08-02.

#### Mockingboard AY-3-8910 rendering — 2026-08-01 pass

Triggered by "DD2's Mockingboard music sounds coarse". Four research
agents (MAME `ay8910.cpp`/`resampler.cpp`, AppleWin + `ayumi`, a POM2
audit, and a headless register trace of the real disks) plus a rendering
rework. Full reasoning → `CHANGELOG.md`; abstraction rationale →
`docs/lle_vs_hle.md`. Probes: `tests/mockingboard_audio_quality`
(spectral purity / DC / write placement / 16 envelope shapes),
`tests/dd2_ay_trace`, `tests/dd1_audio_ab`.

**Landed:**

- ⚠️ **Probe gotcha:** `tests/dd2_ay_trace` calls `Memory::setCpu` but
  never `MockingboardCard::setCpu` (`Mockingboard.h:134`), so the *card's*
  `lastSyncCycle_` stays 0, every event is stamped cycle 0 and the whole
  replay path is silently bypassed. Any new Mockingboard probe must wire
  the card's CPU pointer or it measures nothing.
- ⚠️ **Methodology.** Two successive synthetic harnesses PASSED against
  the bugs they were written to catch (NTSC bursts vs PAL-sized lag; an
  unbroken write stream vs a production gap). The fault needs the audio
  and CPU clocks to be genuinely independent. **A Mockingboard audio
  regression test is only credible once shown to FAIL against the
  reverted fix.**

**Landed 2026-08-01 (stereo pass):**

- 🟢 **SSI263 / Echo+ placement is a guess beyond MAME.** MAME centres
  the Mockingboard's speech chip and gives the Echo+ TMS5220 a
  `front_center` speaker, which is what POM2 does; where a *pair* of
  speech chips would sit (a two-SSI263 Sound II, or Phasor + Echo+ mode)
  has no oracle. Left centred until one turns up.
- 🟢 **Phasor: no cycle-stamped event queue, no `setCpuClock` override.**
  Every register write inside a buffer still collapses to its last value,
  and PAL clocks its AYs 0.7 % fast. Now the only divergence from
  Mockingboard rather than a hidden one.
  Under the [scope ruling](#the-scope-ruling) Phasor is **supported**, so this
  is a stated limit rather than an owed fix — but the dashboard says *Partial*,
  not *Verbatim*, for exactly this reason, and it must keep saying so.
- 🧊 **ayumi-grade resampling** (native clock/8 → 8× quadratic interp →
  192-tap FIR decimation + moving-average DC filter,
  `true-grue/ayumi`, MIT). Strictly better than the box filter and what
  chiptune players use; ~8× the inner iterations plus ~96 MACs per sample
  per channel, ~192 doubles/channel of rewind state and ~2 ms group
  delay — a real cost on the **WASM** target. Only worth it if listening
  shows the box filter is insufficient. Note this would be a deliberate
  departure from "MAME = source of truth" for the audio path.
  Frozen by the scope ruling.
- 🟢 **Analog output stage.** The real Sweet Micro board's LM386 pair
  makes the output *triangular*, not square (deater's scope capture:
  `deater.net/weave/vmwprod/chiptune/mock_problem/`). No emulator models
  it and there is no MAME oracle, so it would have to be an off-by-default
  toggle labelled non-authoritative — and only after band-limiting, since
  a low-pass over an aliased signal muffles rather than removes.
- 🟢 **Mutex contention.** SPSC handoff *if* a profile still shows it after
  the GUI TSan half ([Arch](#arch-refactor--tooling)). `advanceCycles` takes the card mutex on every
  emulated instruction (~1 M/s) and the realtime audio callback needs the
  same one, holding it across the whole SSI263 render on Sound II.
  Classic priority inversion; wants an SPSC handoff.
- 🟢 **Mute drops the queue.** The `isMuted` early-out returns after
  `pending` has been drained, so writes are lost while muted and `vol`
  changes are applied as a hard step at buffer boundaries (click).

- 🧊 **8-bit DAC (Marczewski)** — 8-bit slot latch → R-2R DAC. Niche
  demos (Music Studio, trackers). AppleWin refs `Card::CT_DX1`. *1 d.*
- 🧊 **Passport MIDI Music Card** — 6840 + 6850, Master Tracks Pro /
  Performer. MAME refs `mc6840.cpp` + `acia6850.cpp`. *3 d.*
- 🟢 **AY Port A read mask by DDR** (R14/R15) — academic.

### [Storage] disks & images

- ✅ **//c + ProDOS — resolved 2026-08-30, kept as a record because the "why"
  is nowhere else.** Three layers, each hiding the next: the mount's own
  deadlock (`routeMountHdv`); the stub's `$C800` bank never opened on
  //c-class (`iicCardWindow_`); and — the root — the stub's ProDOS entry at
  `$Cn0A` **without** the real Liron firmware's `BIT $CFFF`. Called by the
  kernel *after* the 80-column firmware has latched INTC8ROM, its `JSR`s to
  `$CD00`/`$CD10` ran the **internal** bank, ProDOS 8 2.4.3's //c scan went off
  into the weeds and its device table stayed empty — `RESTART SYSTEM-$0A` at
  the program's first MLI call. Dissected with the tracer end to end (dispatch
  `$E1C2`, installer `$EE82`, config block `$FExx`) and verified in play:
  SCOSWAMP boots and plays under `--preset iic`. The dissection harness lives
  in the pinned test (`POM2_TRACE_HDV=<hdv> test_iic_onboard_smartport`, plus
  the //e oracle).

- 🟢 **`DiskImage` is a 242 KB object, and the stack-overflow class is only
  patched, not closed.** *1 d, measure first.* The 2026-08-23 macOS SIGBUS
  fix heap-allocates the six insert-path temporaries; any future
  `DiskImage` local on a secondary thread (512 KB on macOS) reintroduces the
  crash, and `diskii_insert_thread_stack` only pins the insert path. Closing
  the class means moving `tracks` (35 × 6656 B, in-object) to the heap —
  which also turns every `DiskImage` move (the install under `stateMutex`)
  from a 233 KB memcpy into a pointer swap. It adds one indirection to the
  bit-stream rebuild and to `writeFlux`, neither per-nibble-hot, but the
  LSS is the emulator's hottest disk code: interleaved best-of-9 on the
  three `pom2_bench` workloads (PERFORMANCE § 8) before and after, or not
  at all. Until then the NOTE on `class DiskImage` is the only guard.

- 🟢 **`decodeTrack` trusts the address field** *(management audit
  2026-08-08)* — the write-back decoder reads vol/track/sector/checksum
  as 4-and-4 but validates none of them: the checksum is discarded, and
  the address field's TRACK number is ignored in favour of the buffer
  index. A guest that rewrites a whole track with a different track
  number in its address fields (sector editors, Locksmith-style
  copiers) therefore lands its sectors at the wrong file offset. `$D5`
  is not a legal GCR data byte so a spurious prologue match can't
  happen, which is why this has never bitten in practice. *~1 h.*
- 🟡 **WOZ1 splice point (TRK+6650)** — `DiskImage::writeFlux` splices
  bit-cells but the full `set_write_splice` handling (TRK +6650
  splice_point/nibble/bit_count fields, parsed at `DiskImage.cpp:867-869`)
  is ignored; IWM call site wired (`IWMDevice.cpp:412`, see the comment
  at `IWMDevice.cpp:56-61`; the stub is `DiskImage.h:283`). Applesauce re-master parity. *1 d.*
- 🟡 **SmartPort ProDOS multi-partition** — 1 image = 1 unit = 1
  volume today; multi-volume CFFA3000-style not supported.
- 🟢 **UI "Force DOS / Force ProDOS"** — backend ready
  (`DiskImage::loadFile(path, SectorOrder)`, log at `DiskImage.cpp:820-827`),
  button missing in `DiskLibrary_ImGui` / `DiskController_ImGui`.
  Auto-detect (extension + vol-dir content sniff `0x400`/`0xB00`)
  already covers 99 % of cases; manual override useful for ambiguous /
  non-standard / debug images. *~30 min.*
- 🟢 **Half-tracked NIB (88)** — deliberately out of scope as long as
  WOZ covers it. Its two former companions are done: **Disk II in
  snapshot** (snapshot v2 with nibble track buffers,
  `DiskIICard` snapshot v2, pinned `rewind_disk_write` — see [UI/UX]
  Rewind) and the
  **Applesauce CNib2 format** (detected at `DiskImage.cpp:544`, pinned
  `disk_cnib2_smoke`) — only the literal `.nib2`/`.app` extensions are
  still missing from `classifyDiskForSlot` / `accept525`.
- 🟢 **Floppy Emu Dual-5.25" + Smartport-Unit-2 modes** — out of scope
  for v1 (4 main modes covered).
- ✅ **Real 3.5" boot — //c+, Liron card and plain //c** *(reopened
  2026-08-28; read path + //c+ boot, then the SmartPort bus, all landed
  2026-09-01)* — every piece was already in the tree and individually
  pinned; what was missing was that no test crossed the seams between them,
  and then one subsystem nobody had named: the drive's side of the SmartPort
  **bus**.

  **//c+ on-board: pinned** (`iicplus_boot35`). The ROM drives the MIG, the
  MIG selects the drive, the IWM walks the cells, ProDOS 8 boots off the
  internal Sony. Two controller faults stood in the way: the IWM clocked in
  whole CPU cycles (a Sony cell is 2.02 of them — it runs on its own 7.16 MHz
  clock now, `POM2_IWM_TICKS_PER_CPU_CYCLE`, MAME's 28/14/36/18 verbatim) and
  a flux query one tick late. Harness: `sony35_iwm_read_path`.

  **Liron card and plain //c: pinned** (`liron_boot35`,
  `smartport_bus_handshake`, `iic_external_smartport`). Both firmwares are
  one code base (`apple2c-32Kv0.rom` bank 1 `$C88C` ≡ the Liron's `$C806`)
  and neither talks to a 3.5" mechanism: the scan at `$C800` holds LSTRB high
  through a status read and polls SENSE — the SmartPort bus handshake, which
  only an intelligent UniDisk 3.5 answers. `SmartPortBusDevice` is that
  device at the byte level (INIT / STATUS / READ / WRITE; the protocol map
  with ROM addresses is in `DEV.md` § The SmartPort bus). `LironCard` puts it
  behind its own IWM and is in the catalog (`liron`); on the 32 KB //c,
  `IIcExternalSmartPort` puts it behind `$C0E0-$C0EF` with a private IWM as a
  register tracker, claiming only bus accesses, so the `DiskIICard` keeps the
  5.25" and `iic_diskii_no_iwm_conflict` still holds. A 5.25" boot now lists
  `S5,D1/D2` next to `S6`; Boot on slot 5 runs the real `$C500`; nothing is
  punched over it while media is mounted. The host-served `$C500` stub stays
  for the 16 KB //c and the //c+'s HDV.

  Deliberately out of scope: the UniDisk 3.5 **drive-side** 65C02 firmware —
  the protocol is the contract and both firmwares boot through it.

### [Cards] slot cards & peripherals

- 🔵 **The MAME `a2bus` backlog — what is worth porting, and why.** *(survey,
  not a task)* <a id="a2bus-backlog"></a>The Workstation Card cost what it did
  **because MAME does not have it**. That is the criterion for everything
  below: a card MAME already models is a port with an oracle; a card it does
  not is reverse engineering. `src/devices/bus/a2bus` crossed against POM2's
  actual gaps:

  **The four that earn their keep.**

  - **Videx VideoTerm** (`a2videoterm`) — *the clearest functional hole.*
    POM2 does the //e's 80 columns (internal, `$C300` firmware + AUX under
    `iieMode`), so a **II/II+ has none at all**. The VideoTerm is the card
    that made AppleWorks, word processors and CP/M usable on a II+. Not
    cheap: it carries its own 6845 and its own dot clock, so it is a second
    complete video path, not a slot peripheral. Cousins if the shape works:
    `a2ultraterm`, `suprterminal`. **The one to do first** — it changes what
    the machine can *do*, not what it can imitate.
  - **Mountain Computer Music System** (`a2mcms`) — the real blind spot for
    an emulator that already has Mockingboard A/C, Sound II, Phasor, SSI263
    and a stereo bus. 16 digital voices, the Apple II's first polyphonic
    synth, and an architecture with nothing in common with the AYs. Plays
    straight to the project's strength.
  - **E-Z Color Graphics Interface** (`ezcgi`) — a **TMS9918 in an Apple II
    slot**: hardware sprites on a machine that has none. Re-ranked after
    looking at POM1: the expensive part is already written there, and better
    than MAME's, with 31 k lines of original software behind it. Scoped,
    estimated and parked → [§ E-Z Color](#ez-color).
  - ~~**Applied Engineering TransWarp** (`transwarp`)~~ **done** —
    `TranswarpCard`, pinned by `transwarp_card`. The estimate held (the
    `cyclesPerFrame` plumbing did the work) but the shape was wrong twice:
    there are no "cache semantics" to model — the board has no cache, it has
    a bus watcher that drops to 1 MHz around slot and paddle accesses — and
    it is not a speed latch in a slot, because it decodes nothing
    slot-relative at all. `$C072`/`$C074` are global, so it needed a bus
    snoop hook rather than a device-select handler.

  **Quick wins, a few hours each.**

  - ~~**4play** (`4play`)~~ **done** — `FourPlayCard`, pinned by
    `fourplay_card`. It was not a shift register: `read_c0nx` returns one
    byte per player and `device_start()` is empty. **SNES MAX** (`snesmax`)
    is still open and is the larger of the two — its controller is serial, so
    the card clocks a latch/shift protocol rather than exposing four ports.
    Both are modern homebrew, so their value is the current Apple II scene
    (and `pom2adventure`), not a period catalogue.
  - **TimeMaster H.O.** (`timemasterho`) — the other common clock beside the
    ThunderClock+ POM2 already has.
  - **Apple Parallel Interface Card** (`a2pic`) — the third printer lineage
    beside the Grappler+ and the synthetic card. Small, and it rounds off the
    printer work.
  - **Memory Expansion Card / RamFactor** (`a2memexp`) — a slot RAM disk,
    distinct from the AUX-slot RamWorks POM2 has. Gives a II+ a RAM disk.

  **Heavier, only if the appetite is there.**

  - **Apple II SCSI / High-Speed SCSI** (`a2scsi`, `a2hsscsi`) — fidelity
    rather than capability, since CFFA and the synthetic HDV already cover
    the need. The point would be running software that talks to the real
    card.
  - **The Mill** (`a2themill`) — a 6809 coprocessor, OS-9 on an Apple II.
    Same shape as the Workstation Card, so **`Memory::ForeignBus` is already
    there for it** (PERFORMANCE § 9).
  - **PC Transporter** (`pc_xporter`) — a whole 8086. MAME has it; it is a
    project in itself.

  **Deliberately skipped.** LANceGS (two Ethernet cards already), the Z80
  variants (`a2applicard`, `softcard3`, `titan3plus2` — the Microsoft SoftCard
  covers it), IEEE-488, ComputerEyes, and the modern storage cards (`a2sd`,
  `booti`, `sider`) that FujiNet + CFFA + HDV already cover.

  **What a hardware archive adds that MAME does not.** Not schematics —
  **DIP-switch and jumper positions with their meanings**. That is exactly
  what POM2 already models for the Grappler+ (its seven printer-type
  positions) and the SSC (its two blocks), and for a Videx or a TransWarp it
  is the source MAME lacks.


- 🧊 **Apple II Workstation Card — it boots, it is identified, and there is no
  network on the other end.** *(Frozen by the [scope ruling](#the-scope-ruling);
  steps 2 and 3 below are closed as won't-do. The title used to say the host
  handshake did not work — it does, see sub-item 1, which is why this item
  contradicted itself for a week.)*
  <a id="apple-ii-workstation-card"></a>The card that put a IIe on LocalTalk,
  so it could netboot from an AppleShare server and reach the LaserWriters on
  the same net. Emulated as `WorkstationCard` (catalog `workstation`), pinned
  by `workstation_card_smoke`.
  → [DEV](DEV.md#apple-ii-workstation-card-workstationcard),
  [plan 2 § 5](docs/printer_plan_2.md#5-the-apple-ii-workstation-card--it-boots)

  **What works.** Apple's real 341-0358-A firmware runs on the card's own
  65C02 — over `Memory::ForeignBus`, so the Apple II's hot path pays nothing
  (measured: PERFORMANCE § 9) — completes the power-on self-test including the
  255-byte SCC loopback, configures the chip for **LocalTalk, SDLC,
  230400 bit/s**, and then **acquires a node address and transmits real LLAP
  frames** (`0B 0B 81` lapENQ, then `FF 0B 84` and short DDP broadcasts). The
  `$Cn00` window, the `$C800-$CFFF` expansion ROM, the `$7C00` ROM banking,
  the interval timer and the snapshot (chip included) all work with the card
  in a real `SlotBus` — and **CardCat, booted on the emulated //e, names the
  card in slot 4**.

  **What remains, in order:**

  1. ✅ **The host handshake works.** AppleShare's `ATINIT` calls the card at
     `$Cn14` in ProDOS-MLI style — `JSR $Cn14 / .BYTE cmd / .WORD block`,
     command `$42` — and POM2 now services it end to end: the command byte
     reaches `$CnDB`, both rendezvous semaphores return to rest, and the call
     returns past its inline parameters. Pinned by `workstation_card_smoke`.

     **The bug was one number**: the `$C800-$CFFF` window was based at file
     `0xC800` instead of `0xC400`, so the page's `JMP $CC00` landed on a
     block-copy loop rather than the driver prologue
     (`CLD / PHP / SEI / LDA #$50 / STA $C080,X`). Nine bases were swept;
     `0xC400` is the only one at which the transaction completes.

     **Two things worth keeping from how long that took.** The card steers
     the host by *patching the host's code* — it writes `$CnBB`/`$CnBC` (the
     operands of the host's `JMP`) and `$CnC3`/`$C4`/`$C6`/`$C7` (the address
     operands of its block-move), and releases the host's spin loops by
     writing `$38` (`SEC`) over the `$18` (`CLC`) it is executing. And the
     "missing bit 6 of `$02EE`" was a **red herring**: wiring it moved the
     card one step further, which made it look right, and it was not. A
     change that unsticks a stuck system is not evidence that it is correct.

     Still open, and now cheap to look at: `$C0nX` reads answer `$FF` and
     writes are ignored, and the transaction completes anyway — so whatever
     the strobes are for, this path does not need them. `hostStrobeLog()`
     records them for whoever wants to find out.

     ✅ **Verified end to end**: `disks_3.5/AppleShare IIe Workstation.po`
     boots in POM2, its ATINIT passes the card's power-up diagnostics and
     the workstation software reaches its menu.

  2. 🧊 **Why lapACK does not move the node.** Answering the card's lapENQ
     with lapACK is accepted by the chip — the FIFO fills, the interrupt fires
     — and the driver enquires again anyway rather than picking another
     address. Timing window, a status bit that is not set, or simply what this
     firmware does on a dead network. Worth an hour before (3). *~0.5 d.*
  3. 🧊 **A host-side LocalTalk endpoint** — bridge the card's frames to a
     real or emulated AppleTalk network. `setFrameCallback` and
     `receiveFrame` are the seam and both work; note the card **disables its
     receiver while transmitting**, so an endpoint must wait for WR3 D0 to
     come back before answering. *~1-2 d.*

  ~~**SDLC framing.**~~ **Done** — datasheet-derived (MAME has no SDLC),
  marked `SDLC (datasheet, not MAME)` at every site, pinned by
  `scc8530_smoke`. ~~**The SCC's register file is not in the snapshot.**~~
  **Done** — `Scc8530Device::appendSnapshot`/`restoreSnapshot`, carried by the
  card's own blob.

  **Smaller gaps, worth knowing.** The interval timer's period is a **chosen**
  1 ms, not a derived one: the dump does not settle it and the firmware boots
  with it. And the card runs a second 6502 at the Apple II's own rate, so
  plugging it roughly doubles the emulation work.

  **Do not "optimise" `advanceCycles`.** Its 24-cycle interleave is
  correctness, not tuning: the POST's self-test has a fixed poll budget, and
  running the CPU for a whole 4096-cycle slot-bus chunk before the SCC moves
  fails it on a timeout no real card would see.

  **Worth knowing before finishing it:** given the LaserWriter's PostScript
  path already ships (plan 2 § 4), this card buys an *alternative transport*
  for PostScript that the Super Serial Card already carries — not a new
  capability. It is worth doing for netboot and AppleShare, and for being the
  way most sites actually wired a LaserWriter; it is not the only way to print
  to one.

- 🟢 **ProDOS `STATUS` — the hand-written ROM family** *(closed 2026-08-28)* —
  <a id="prodos-status-the-hand-assembled-rom-family"></a>kept as a record, not
  as work. All six pages are written with `SlotRomAsm.h`: an address is a
  label, so a mistyped displacement and a routine that moved out from under one
  are both unrepresentable, and two regions claiming the same bytes — the
  SmartPort bug in its purest form — is an error before a byte is written.
  Both halves of the `ProDOSHardDiskCard` finding are fixed, the second by
  making the write routine *shorter*: one `BIT $C0n3` answers "is there media?"
  and "is it locked?" at once, and both transfer routines branch to a shared
  error tail in the gap after boot. Zero slack became eight bytes.
  → `CHANGELOG.md`

  What the audit settled, so nobody re-derives it:
  - `ClockCard` writes nine fixed bytes with no cursor — nothing to assemble.
  - `DiskIICard`'s boot PROM is a verbatim dump.
  - `GrapplerCard` hand-writes only its *fallback stub*; a real `roms/` dump is
    copied verbatim.
  - `SuperSerialCard` was the tightest of the six, for a reason the others did
    not share: `$Cn0D-$Cn10` are the low bytes of four Pascal routines, so a
    routine pushed down lands the interpreter mid-instruction. Those four bytes
    are `byteOf()` now, and cannot disagree with where the routines are.

- 🟡 **SmartPortCard leftovers, still open** (2026-07-12 Liron audit
  follow-ups) — two of the original five remain:
  - boot failure is a silent `JMP $CnE0` loop; real firmware prints an error.
  - CONTROL calls that need the control-list DATA: only code 0 works, because
    the stub has no guest→device list copy. Everything else returns `$21`.

- 🟡 **Write-protect: two bays of one card answer differently** — architect
  **P3-1**. 3.5" units report `fileWriteProtected || !writeBackEnabled` (the
  rule `DiskImage` uses for 5.25"), while HDV bays report only `wpHeader_` and
  stay RAM-writable. On the *same* SmartPort card the guest's "can I write?"
  is decided by media kind plus a host-side toggle that models nothing on the
  machine. Physically, write-protect belongs to the medium; the counter-argument
  is that accepting a write with write-back off loses it silently. Both are
  defensible — one card doing both is not. **Needs a ruling, then ~1 h.**

- 🧊 **EchoPlusTMS5220Card (real Echo+)** — catalog scaffold
  `echoplus_tms`: SlotPeripheral + stub register decode at
  $Cs00-$Cs0F, enough for detection.
  Remaining: TMS5220 LPC10 decoder (chirp ROM + K-parameter
  interpolation) and AY-3-8913 audio synth — the shared AY core it
  needs already exists (`src/AyPsgSynth.h`, extracted 2026-08-01,
  see [Audio]). *~3-5 d.*
  **Ruling [R2](#standing-rulings) is answered by the scope ruling: hide.**
  Taking it out of the README table and the picker is a
  [G3](#g3--make-the-words-true-) item; the chip itself is closed as won't-do.
- 🟡 **Nothing asserts the real ClockCard ROM path is taken** when the dump
  is present — `clock_card_smoke` tolerates its absence so CI stays
  ROM-free, so a regression that silently routed back to the synthetic ROM
  would fail nothing. Same silent-degradation hole as every other
  ROM-driven L path (Disk II P6, mouse MCU, Grappler EPROM); →
  [`docs/lle_vs_hle.md`](docs/lle_vs_hle.md) § Keeping a level once you
  have it. **Folded into [G5-15](#g5--the-donut-policy-without-a-test-)**, with
  Disk II P6, the mouse MCU and the Grappler EPROM. The DOS 3.3 / Applesoft
  tools that pull the driver from `$C800` are still untested.
- 🧊 **Apple II SCSI / High-Speed SCSI + CHD** — MAME
  `a2scsi.cpp` (NCR 5380) / `a2hsscsi.cpp` (53C80). Big lift for a
  niche need (CFFA suffices). *~30-50 h.*
- 🧊 **Apple II VGA / Second Sight (VGA video card)** — slot card that
  shadows the Apple II framebuffer and outputs a clean VGA signal
  (scanline mode + text/HGR/DHGR/lo-res modes). Two incarnations: the
  open-hardware project **markadev/AppleII-VGA** (RP2040, free firmware +
  KiCad, so registers and timing are documented) and the commercial
  **Second Sight** (reactivemicro, Brutal Deluxe manual). POM2 already has
  all the video decode (`Apple2Display`); the value would be modelling the
  card's soft-switches/registers for software detection and an optional
  "VGA-clean" output. Code + doc refs:
  - <https://github.com/markadev/AppleII-VGA> (RP2040 firmware + KiCad)
  - <https://www.brutaldeluxe.fr/documentation/secondsight/secondsight_manual.pdf> (Second Sight manual)
  - <https://downloads.reactivemicro.com/Apple%20II%20Items/Hardware/SecondSite_VGA/> (ReactiveMicro dumps/ROMs)
  - <https://www.apple2history.org/history/ah13/#05> (historical context)
  *~5-10 d (register sourcing + integration mode to be decided).*
- 🧊 **UDC (Apple 1991)** — 4 heterogeneous bays (3.5"/5.25"/HDV).
- 🧊 **Slinky / RamFAST RAM disk** — limited utility vs RamWorks III.
- 🧊 **Apple 3.5" Controller IWM-level** — refactor IWMDevice attached
  to a slot card (rare).

### [Cassette]

- 🧊 **Enriched WAV record/playback** — POM2 supports .wav; missing
  analog tape filtering (hiss, drop-out), VU-meter, timecode.
  MAME refs `apple2.cpp` cassette. *2 d.*

### [Network]

- 🔵 **[FujiNet] built-in FujiNet, native in POM2 — DECIDED 2026-08-21.**
  Chosen architecture: a **native `FujiNetDevice` in POM2's own C++ covering
  the disk perimeter** (host slots, drive slots, a TNFS client, images served
  as SmartPort block devices), driven from the FujiNet panel, **coexisting**
  with the existing SP-over-SLIP relay for a real USB board or a full external
  firmware. The panel gets a source selector: *Built-in / USB board / External
  firmware* — the same "choose the level, not the catalog key" shape as the
  Abstraction Levels panel.
  Why this over the alternatives: hosting the vendor firmware (prebuilt or
  built from source) does **not** remove the class of bug that the CONFIG
  breakage belonged to (fixed 2026-08-21, → `CHANGELOG.md`), because the guest
  still reaches the device through the relay's control plane; when POM2 *is*
  the device, that class of bug cannot exist. Building the firmware into POM2's
  build was re-costed and re-rejected (`docs/fujinet_plan.md` § 8).
  Out of scope for the native path, deliberately: the `N:` network device
  (HTTP/SSH/JSON), the modem and CP/M — those stay the relay's job.
  Phases: (1) TNFS client + tests — **done**, and since 2026-08-28 it has a
  caller: `TnfsMedia.*` fetches an image from a TNFS server into a local cache
  and `POM2 tnfs://host/path/image.po` boots it like any other disk. That is
  not phase 3 — the guest sees an ordinary local image, not a block device
  backed by the network — but it makes the client reachable and useful now;
  (2) the Fuji control device (host/drive slots) so CONFIG sees real state;
  (3) block serving of a mounted image; (4) the panel's source selector.
- 🟡 **[FujiNet] a `CONTROL` to the peer's PRINTER unit kills it** — measured
  2026-08-21, reproducible three runs out of three. The packet is
  byte-identical in shape to the ones units 10-12 answer normally
  (`04 03 0D 00 00 00 …`, an empty control list), yet the peer throws
  `std::length_error` out of `Request::from_packet` and aborts. Upstream bug
  in the printer device — the same unit whose DIB already advertises the
  modem's type byte. POM2 relays faithfully and now REPORTS the death
  (`peer LOST after N s — M call(s) served`), which is what localised it in a
  single run. Worth reporting upstream; POM2 has nothing to fix.
- 🟡 **[FujiNet] the desktop firmware's `N:` device never opens a socket** —
  it answers the guest's open with success (`CONNECTED to
  N:HTTP://THEOLDNET.COM/` appears on the Apple II) and then no outbound TCP
  is ever created, watched live on the peer's own descriptors. NOT POM2: the
  same firmware, same machine, opens real TCP for TNFS. Its WiFi is a
  `DummyWiFiManager` and giving it an SSID does not help. A real FujiNet
  board over USB is the path for `N:`; the relay is unchanged for it.
- 🟡 **[FujiNet] the 250 ms relay timeout is sized for local media only** —
  measured 2026-08-21. `SpOverSlipLink::kDefaultTimeoutMs` is fine while the
  peer answers out of its own SPIFFS (booting `autorun.po` never approaches
  it), but every block read of a TNFS-hosted image crosses the internet and
  overruns it, so the guest gets `FN ERROR` instead of a boot. Workaround
  today is the per-slot `fujinet_timeout_ms_slot<N>` key (3000 works); it has
  no UI and nothing tells the user it is why their network disk will not
  boot. Options: raise the default, or measure the peer's round-trip at
  enumeration and size the timeout from it. *~2 h.* → `DEV.md` § FujiNet.
- 🟡 **[FujiNet] a network-backed SmartPort call freezes the emulator** —
  `transact()` blocks the CPU thread under `stateMutex` by design (see the
  threading note in `SpOverSlipLink.h`, and § 9 of the plan for why that was
  the right call). Invisible at 250 ms over loopback; very visible once the
  timeout is raised for a peer whose media lives on the internet — the UI and
  the AI control server both stall for the length of every read. Wants at
  least a "waiting on FujiNet" indication, and possibly a bounded pump of the
  UI while a call is outstanding.
- 🟢 **[FujiNet] `PR#n` before the peer attaches prints `FN ERROR`** — the
  autostart slot scan handles the no-peer case correctly (the card steps
  aside and the scan carries on to slot 6), but a manual `PR#n` in the same
  state just fails. The card could wait briefly for a peer, or say *why* it
  failed. Cosmetic, but it is the first thing anyone hits.
- 🟢 **[FujiNet] media bays + modem bridge — DECIDED AGAINST.** Surfacing
  the peer's block units as `MountableMediaCard` bays would add rows whose
  Mount/Eject cannot work (the images live on the FujiNet's own SD/TNFS
  storage, which POM2 has no path to write); the FujiNet panel's device
  table already answers "what has it got mounted". Bridging its modem unit
  into the SSC telnet path would fight the FujiNet's own stack, which
  already reaches the network — POM2 dialling out in parallel would break
  the connection state the guest thinks it owns.
- 🟡 **[FujiNet] //c-class support** — the card is II+ / //e only: a
  //c's forced INTCXROM masks all slot ROM. On real hardware the FujiNet
  *is* the SmartPort on the disk port, so the correct integration hangs
  the relay off the on-board `$C500` hole (`exposesIicOnboardRom`,
  see `project_iic_smartport_boot`) rather than a slot card. *~1-2 d.*
- 🟢 **[FujiNet] embedded firmware** — `fujinet-go-apple2-desktop` builds
  the FujiNet firmware as a shared library and `dlopen`s it, so the user
  needs no second program. Deliberately NOT done: it drags in mbedtls,
  expat, a pinned submodule and a patch set anchored to exact upstream
  text. Revisit only if "having to install a second program" turns out to
  be the real blocker.

- 🧊 **Uthernet I has no host transport on Windows** — libslirp is the
  only backend that moves raw frames, and CMake deliberately does not
  look for it on WIN32. vcpkg *does* carry a libslirp port (4.9.1), so
  the library is obtainable; what is missing is POM2's side —
  `SlirpNetworkBackend`'s poll loop is POSIX `poll()` over the fds
  libslirp returns, and porting it cannot be verified without a Windows
  libslirp build to test against. Note the vcpkg port pulls **glib**,
  which is a heavy addition to the Windows CI job. Uthernet II is
  unaffected (hardware TCP/IP on host sockets). *1-2 d + CI budget.*
- 🧊 **Uthernet II inbound (`LISTEN`)** — the W5100 `LISTEN` command is
  decoded but unimplemented: neither transport can route an inbound
  connection to the guest (libslirp is outbound-only without explicit
  port forwarding). Needs a user-configured host port to bind plus a
  slirp `hostfwd`-style mapping. *1 d.*
- 🧊 **Uthernet I on WASM** — the CS8900A model is browser-safe but has
  no transport there (no raw sockets, and libslirp isn't in the
  Emscripten build). A websocket-proxied backend would fix both cards'
  raw modes in the browser. *2-3 d.*

### [Input] joystick / paddles / mouse

- 🟡 **PADL(2)/PADL(3) host binding** — no host axes are bound to the second
  stick; `JoystickInput.cpp:60-75` is the NaN-guarded `[-1,+1] → [0,255]`
  mapper it would feed.
- 🟡 **Mouse → paddles mapping** — paddle 0/1 on host mouse X/Y axes
  (alternative to pads).

### [UI/UX]

- 🟢 **Deeper guided tutorials** — the Welcome / no-ROM panel covers a first
  launch without a ROM; step-by-step tutorials do not exist.
- 🟢 **MicroM8-style Rewind** — continuous state recording +
  scrub/step-back/rewind-live. **Phases 0→5 done** (2026-05-31,
  `CHANGELOG.md`): memory backend `SnapshotIO`, shared `MachineSnapshot`,
  `RewindBuffer` (keyframes + XOR deltas, memory budget), frame-boundary
  capture (`workerLoop` + `tickFrame` WASM), parked-worker transport +
  `Rewind_ImGui` UI (timeline / transport / `F6` rewind-live), `DiskIICard`
  drive state via `SlotPeripheral::*SnapshotState`, audio flush on restore.
  Pinned `snapshot_memory_roundtrip`, `rewind_roundtrip`, `rewind_delta`,
  `rewind_transport`, `rewind_slot_state`, `rewind_audio_state`
  (Mockingboard/Phasor VIA+AY+SSI263 → music **and** speech survive the rewind),
  `rewind_disk_write` (DiskIICard snapshot v2 = nibble track buffers → disk
  writes are undone on rewind). **Remaining**: writes to a writable WOZ not
  undone (`wozRaw` is a separate store; WOZ originals usually write-protected);
  "redo" (replay an undone future) not implemented. Detail → `DEV.md`
  § Rewind / time-travel.
- 🧊 **MicroM8-style 3D voxel view ("Voxel Cube")** — screen **stood up**
  (monitor, XY plane) as a 4:3 slab of **uniform-depth** cubes + per-color "pop"
  relief, orbital camera. NB: the initial luminance extrusion gave flat
  stalactites — fixed after scraping MicroM8 (cf. `CHANGELOG.md` + `DEV.md` § 3D
  voxel view). **Phases 0→3 done** (2026-05-31, `CHANGELOG.md`): `Mat4.h`
  (Vec3+Mat4+OrbitCamera, pinned `voxel3d_math`), `Voxel3DRenderer` (instanced
  cubes, FBO+depth, per-vertex color texture-fetch, derivative shading,
  **anti-moiré supersampling** + contiguous `cubeFill=1`), **native resolution**
  (1 voxel/pixel, 280|560×192), tap **before** `CrtEffectStack` (independent of
  CRT effects), *(P2)* left-drag orbit + middle-button **pan** + wheel zoom,
  *(P3)* View ▸ "3D voxel settings…" panel (depth/pop/fill/AA/ambient/mono/
  per-colour, persisted `voxel_*`), *(P4)* **WASM perf guard** (`ss≤2`+FBO≤2048²+
  `gridW≤280` under Emscripten), *(bonus)* **Mono mode** + **depth by color
  index** (snap lo-res palette `kVoxelPalette`). **WASM build OK** (+ browser
  wheel fix: `emscripten_set_wheel_callback` → `io.MouseWheel`, cf. `main.cpp`).
  **Remaining**: *(P5, deferred on request)* rewind tie-in "freeze + orbit a
  rewound frame" — already works for free (the view samples the live framebuffer
  that rewind restore updates), so doc + polish rather than plumbing; *(option)*
  alternative heightfield-mesh mode. Detail → `DEV.md` § 3D voxel view. *P5≈0.5 d.*
- 🟢 **Airier default layout** — ImGui Docking or
  `SetNextWindowPos` adaptive cascade.
- 🟢 **`isDuplicate` flags cffa/smartport35 duplicates** in the Slot
  Config assignment column — cosmetic.
- 🧊 **On-screen touchscreen / virtual joystick** — ImGui virtual
  joystick for mobile WASM builds (separate from raw touch routing). Two
  thumb-sticks + Open/Solid Apple buttons. Inspired by microM8 / A2TS.
  *2 d.*

### [WASM]

- ✅ **IDBFS settings persistence** — *landed 2026-09-01.* `state.cfg` and
  `imgui.ini` now live in `pom2::userConfigDir()`, which is `/persistent`
  under Emscripten, and reach IndexedDB through a debounced `FS.syncfs`
  (`PersistentFs.h`). Two things the estimate did not know: the browser
  build has **no exit** (`simulate_infinite_loop` never runs `~MainWindow`),
  so the whole persist block had to become `MainWindow::persistSession()` on
  a heartbeat; and the shell's populate had to hold up `run()`, which is a
  boot hang if its callback never fires — hence the watchdog. Verified in
  headless Chrome over CDP, three visits. → `CHANGELOG.md`
- 🧊 **File picker / drop-zone disks** — build-time bundling
  only. HTML5 drop-zone → `FS.writeFile('/uploads/…')` →
  `DiskIICard::insert`. *~1 d.*
- 🧊 **Mobile touch input** — GLFW3 under Emscripten does not map
  touch → mouse off-canvas. JS wrapper `touchstart/move/end` →
  `Module._inject_mouse_*`.
- 🧊 **Audio worklet tuning** — miniaudio Web Audio works but
  latency ~150 ms is audible on speaker click. Explore a custom
  `AudioWorkletNode` or shrink the buffer.
- 🔴 **The licensing call — moved to [G1](#g1--what-we-are-allowed-to-ship-),
  and it was scoped wrongly here.** This item read as prospective, "a marketing
  prerequisite before pushing to r/apple2". It is not. The technical side
  really is done — `roms/` is baked into `POM2.data`, `POM2_WASM_BUNDLE_DISKS`
  bundles `disks_3.5`, and `wasm/shell.html` auto-boots Total Replay from the
  bundled `floppyemu/` (`CMakeLists.txt:965-1001`) — and `ci.yml:437-460` has
  been deploying exactly that to `habib256.github.io/pom2/wasm/` **on every
  push to main**, linked five times from the README. The demo this decision was
  meant to gate has been live for months, so the decision is a 1.0 blocker
  rather than a marketing one.

### [Arch] refactor & tooling

- 🟡 **Two ceilings were raised instead of splitting — the debt.** *(~1 d for
  ImageWriter, less for Memory; post-1.0, ruling
  [R4](#standing-rulings))* <a id="file-size-debt"></a>The file-size
  ratchet had been failing on `main` since **2026-08-29**, in 15 s, before the
  Linux job compiled anything — so it was also hiding that job's build and its
  GLES tier behind a red X nobody could see past. On 2026-08-31 the two
  ceilings were raised to their exact current sizes to get the gate green
  again. That is what `tools/file_size_budget.txt` is for (its script's header
  says editing it is precisely the moment someone should be asked), and it is
  the lesser half of the answer.

  - **`src/ImageWriter.cpp` 2152 → 2501 (+349)** — the printer work put the
    ImageWriter head, the paper tray, PDF export and the PostScript /
    screen-dump seams in one translation unit. Those are already separate
    concerns with clean edges; this one wants splitting, and the project's own
    rule ("new code for an existing window group belongs in its own
    translation unit") says so.
  - **`src/Memory.cpp` 2402 → 2442 (+40)** — smaller, and not one change.
    Exactly **one** line of it is the foreign-bus dispatch that lets a
    coprocessor card run the 6502 over its own map
    (`docs/PERFORMANCE.md` § 9); the other 39 predate it.

  Both are recorded at their **exact** current size, so the ratchet still
  fails on the next line either of them gains — the debt cannot quietly grow.


- 🟢 **Z80/SoftCard cleanup backlog** (2026-07-12 bug-hunt survivors — quality,
  not correctness): SoftCardZ80 SFZ2 blob → `pom2::byteio` putU16/Reader like
  every other card (3 hand-synced layout copies today);
  `softcard_cpm_boot_test` → `pom2::findResource` + `Apple2Display::
  textRowAddress` instead of private copies (6th in-repo transcription of the
  text-row interleave); Z80.cpp decoder dedup: rp-selector switch pasted 8×
  (readRP/writeRP helpers), JR cc's inline condition test vs `ccTest`,
  `memEA` body re-inlined twice for special timings (chargeless
  `indexedEA()` split); `xlate` 5-compare chain → 16-entry per-4K-page
  offset LUT; drop the per-instruction `mem_` null tests in the dmaRun hot
  loop (guard once at entry). Boot test could also pump a public
  controller slice hook instead of re-implementing arbitration.
- 🟡 **ThreadSanitizer — the GUI half** *(2026-08-02 bug-hunt follow-up)*.
  Gated on [G5-10](#g5--the-donut-policy-without-a-test-), which is what makes
  a GUI harness possible at all. The controller half is done and clean; the
  analysis is kept in
  [Open, and known to be open](#open-and-known-to-be-open). That sweep's ASan+UBSan build (156 test binaries, ~24 000
  hostile-input cases, ~6 M random instructions) returned **zero**
  diagnostics, yet code reading found a UI deadlock, two use-after-frees and
  three unlocked cross-thread reads in the same tree. ASan cannot see data
  races and the headless tests cannot reach the GUI, which is exactly where
  the defects were. Needs a TSan build driving the GUI with the AI server
  polling `/screen.ppm`, slot reconfiguration, and rewind under load. Would
  also retire the two findings that could not be pinned (`saveScreenshot`'s
  `demodMutex` ordering, and the threaded half of `disk_path_snapshot`).
  - **The controller half is done and clean** (2026-08-17, bug hunt 8): a TSan
    harness drove the real thread shape without a GUI — CPU worker, a UI thread
    running the transport verbs (rewind scrub/seek/resume, cassette, 3.5"
    mount/eject, speed, mode toggles, a `lockState()` read per frame), an
    AI-server thread doing `lockState()` reads + snapshot capture/restore + key
    injection through `kbMutex`, the live miniaudio callback, and a Mockingboard
    in slot 4 fed by a guest loop so the emuCycles AY queue (the one real
    CPU↔audio producer/consumer) is exercised. Zero races. **Caveat worth
    keeping**: TSan instruments every load/store in the interpreter's hot loop,
    so the CPU manages only ~400-1 400 emulated cycles/s — the *lock protocol*
    is covered thoroughly, *emulated execution* thinly. What remains is the GUI
    half: ImGui panels, `demodMutex`, slot re-plug under load.
- 🟡 **Consolidate the atomic file-write helper** — moved to
  [Open, and known to be open](#open-and-known-to-be-open), where it now sits
  next to the `ImageWriterPdf` divergence found on 2026-09-05. The three copies
  are `DiskImage.cpp:2311`, `Disk35Image.cpp:329` and `ProDOSVolume.cpp:709`.
- 🟡 **`hgrpaint/` has no headless harness** *(2026-08-14)* — the editor's
  state (mode flags, shadow buffer, tools) is private and only reachable
  through `render()`, i.e. through an ImGui frame, so nothing in `ctest`
  can exercise it: `dhgr_paint_model` pins the free functions in
  `HgrPaintModel.h`, not `HgrPaintEditor`. That is how three
  out-of-bounds accesses on the DLGR shadow survived from the DLGR page's
  arrival (2026-07-12) to 2026-08-14 with a green suite. Cheapest fix: a
  test-only seam (a friend fixture, or a small `EditorTestAccess` struct)
  driving the tools against a stub `IHgrPaintHost` — bearing in mind
  `hgrpaint/` is shared verbatim with POM1, so the seam must be additive.
  *~1 d.*
  - **`hgrsprite/` had the same shape and cost the same kind of bug**
    (2026-08-17, bug hunt 8): no test at all, and its ca65 DHGR export read
    32 bytes past the pair buffer at the UI maxima. The byte layer is now
    pinned by `hgr_sprite_blit` — including the two helpers the export's
    clipping moved into (`dhgrExportRowBytes`, `extractDhgrPlanes`) — but
    `HgrSpriteEditor` itself is still only reachable through an ImGui frame,
    exactly like `HgrPaintEditor`. One seam would serve both.
- 🟠 **No test drives the ImGui panels, so a UI-thread deadlock fails nothing**
  *(found the hard way 2026-08-27; gated on
  [G5-10](#g5--the-donut-policy-without-a-test-))* — a coordinator capture placed inside an
  existing `lock_guard(stateMutex())` scope would have hung the UI thread and
  the emulator together, while the full suite stayed green, because nothing
  drives the panels. `stateMutex` is non-recursive and every coordinator
  capture takes it itself.
  **Mitigation in the tree**: `tools/check_coordinator_locks.sh`, run after
  touching any coordinator call site — falsifiable against `44b715f`.
  **Still open**: `tests/frontend_device_panel_concurrency`, a headless ImGui
  frame driven while cards are replugged. That is the only thing that closes
  the class rather than scanning for it. *1 d.*

- 🟢 **W5100 name resolution is still inline** *(2026-08-27)* — deliberately
  left in `W5100Device` when the socket seam landed: an async mailbox with an
  in-flight cap, a bounded wait and its own cache, wired to register reads.
  Its own pass, and not on anything's critical path.
- 🟡 **Scattered config** — ruling [R3](#standing-rulings): one `Config`
  (env → CLI → Settings → defaults), env vars listed in `--help`. *1 d.*
  Post-1.0.
- 🟡 **`stateMutex` shared CPU+UI** — `MainWindow_Slots` takes this lock
  during plug/unplug, an audio-jitter risk. Partition long-term, and only
  after the GUI TSan half above.
- 🟡 **CI `ctest -L rom` + ROM Status "degraded"** — now
  [G5-15](#g5--the-donut-policy-without-a-test-). Re-verified 2026-09-05: no
  `LABELS rom` exists anywhere in `tests/CMakeLists.txt`, only `slow`.
- 🟢 **Inconsistent `pom2::` namespace** — 255/336 top-level files (was
  163/233 when this was written); `tests/` does not use it. Mechanical
  migration, ruling [R3](#standing-rulings). Post-1.0.
- 🟢 **Legacy M6502 style** — FR/EN comments, C-style casts,
  `void(void)`. Targeted `clang-format` + `clang-tidy modernize-*`.
- ✅ **`*Card` raw pointers in MainWindow** — *closed 2026-09-05, the premise
  was wrong.* The `*Card` entries in the header are accessor **functions**
  (`MainWindow.h:1211-1215`), and `MainWindow_Media.cpp:347-351`'s
  `primaryDiskII()` already resolves live through
  `storageCoordinator_->topology(controller->memory().slotBus())` — which is
  exactly the remedy the item proposed.

## Edge-case test corpus

Backlog of **manual / integration tests** with real software that tortures the
corners (cycle-exact CPU↔video sync, protected WOZ flux, VIA IRQ) — beyond the
unit `ctest`s. Curated list + POM2 status + cross-refs to the dashboard's
`Known gaps`: **[`docs/test_corpus.md`](docs/test_corpus.md)**.

- 🟠 **[DIX](https://github.com/Fr3nchT0uch/DIX/) — French Touch demo
  anthology**. **Priority reference** for emulation perfection: chains vapor
  lock, mid-scanline, Mockingboard, 128 KB aux, Unidisk/Liron. Validate DIX
  first before any other corpus title. Full description → `docs/test_corpus.md`.
  - ⚠️ **Some French Touch titles hard-code the Mockingboard at slot 4 — DIX
    itself does not.** DIX **scans** `$C7→$C1` for its 6522 (`boot_unidisk.a`
    `bdet`), so it finds the card wherever it sits. **MAD EFFECT**
    (`disks_5.4/demo/madef/Sources/main.a:176-218`) addresses `$C4xx` with no
    scan, and its whole frame sync is the T1 IRQ — with the card anywhere else
    it arms a timer that never fires and waits forever: a frozen screen after
    the loader, and no code regression. `madef_phase_probe` shows 0 page-flips
    per frame in slot 7 against ~191 in slot 4. Slot 3 is no alternative: the
    //e's internal 80-column firmware owns `$C300-$C3FF` (SLOTC3ROM off), so a
    Mockingboard there is silent. **The fresh-install map is mouse@4,
    Mockingboard@2** (CLAUDE.md § Fresh-install defaults — swapped 2026-09-02
    because Extasie's self-modified `JSR $C4xx` needs the mouse there and DIX
    scans anyway), so a title of MAD EFFECT's kind needs the two swapped by
    hand. `MainWindow_Slots.cpp:428-438` already warns about the **mouse** side
    of this; 🟢 the Mockingboard side has no hint yet.
- 🟡 **Spiradisc / RWTS18** (*Captain Goodnight*, *Prince of Persia*) — spiral
  tracking + weak bits to validate on real WOZ images. → `Gap #9/#10`.

## Parked — wanted, not scheduled

Things with a clear shape and a reason that have no slot. Under the
[scope ruling](#the-scope-ruling) everything here is **frozen**: it leaves this
section when a named piece of software needs it, and not before. The estimate
below is kept because it was done properly and re-costing it would be waste —
not because the work is queued.

### E-Z Color Graphics Interface — a TMS9918 in an Apple II slot

<a id="ez-color"></a>**Estimate: ~3.5-4 days** for a working card
(TMS9918A only). *Card only* — `tmspaint` and `tmssprite` stay in POM1.

MAME has it (`src/devices/bus/a2bus/ezcgi.cpp`, Steve Ciarcia, *BYTE* August
1982 — a construction article, not a product), so the usual "MAME is the
oracle" rule half-applies. Only half, and this is the unusual part: **POM1
already has a better TMS9918 than MAME's.** `pom1/src/TMS9918.{h,cpp}` is
2 586 lines with a `ChipType` dispatch across TMS9918A / 9929A / 9118 / 9128 /
9129 / T7937A / T6950, modelling the Toshiba clones' suppression of sprite
cloning ("Bug N°8"); MAME models plain `tms9918a`. So the oracle here is
POM1 + the datasheet + POM1's own silicon tests, and **that must be written
down at the porting sites** the way SDLC framing is marked
`SDLC (datasheet, not MAME)` in `Scc8530Device` — otherwise a future reader
goes looking in MAME and finds something *less* accurate, which is the worst
kind of trap.

**Why it is worth doing at all**: the Apple II has no sprites, no VRAM of its
own, and colour only as an NTSC artefact. This card brings 16 KB of dedicated
VRAM, 32 hardware sprites and 15 commanded colours. And the software problem
that sinks most curiosity cards does not apply — POM1 carries **31 067 lines
of original 6502 assembly** for this chip (Rogue 6 777, a logo/scroller 5 426,
Galaga 5 127, Maze3D 4 315, plus Sokoban, Snake, Chess, Mandelbrot, Life,
Plasma, Nyan Cat). Porting those to the Apple II is a separate, later job and
is **not** in the estimate below.

**What makes the port cheap, checked rather than assumed:**

- The VDP port addresses are canonical in one place —
  `pom1/dev/lib/tms9918/tms9918.inc`, `VDP_DATA = $CC00` / `VDP_CTRL = $CC01`
  — so POM1's 6 079-line asm library is address-agnostic above two symbols.
- `BeamClock.h` is 63 lines, a pure header depending only on `<cstdint>`, and
  its own comment already names POM2 as an intended consumer.
- Both projects clock at 1 022 727 Hz exactly. No retiming.
- `pom1::Peripheral`'s pure-virtual surface is `name()` alone; everything else
  has a default. Stripping it costs almost nothing.

**What is not free:**

- `SnapshotIO.h` differs between the projects (273 vs 183 lines), so
  `serialize`/`deserialize` must be rewritten against POM2's
  `appendSnapshotState` / `loadSnapshotState` pair. The *content* is already
  enumerated in `TMS9918::Snapshot`, so it is mechanical.
- The beam/CPU sync (`renderBeamCatchUp`, `syncSpriteScanToBeam`) is tied to
  how the host feeds cycles, and is the part to read carefully rather than
  transplant.

**Breakdown:**

| | |
|---|---|
| Import + decouple the VDP core (2 586 lines + diagnostics + BeamClock), snapshot rewrite, build wiring | ~1 d |
| `EzCgiCard : SlotPeripheral` — offset 0 ↔ VRAM, offset 1 ↔ register/status, `$FF` elsewhere, `advanceCycles` feeding the beam. Slot-agnostic by construction | ~3 h |
| Display path — a second source composited into POM2's framebuffer. **The risk item**: not the rasterising, but the interaction with `NtscPostProcessor` / `CrtEffectStack` / the display-mode menu. The VDP's output is RGB and must NOT go through the composite shaders | ~1-1.5 d |
| Tests — a subset of POM1's ten (sprite status, per-scanline, silicon-strict) plus a card smoke at `$C0nX` | ~0.5 d |
| Catalog, plug site, docs (CLAUDE map, DEV section, README, CHANGELOG) | ~0.5 d |

**Deliberately out of v1**: the `ezcgi_9938` / `ezcgi_9958` variants (V9938 /
V9958 with an IRQ line back to the Apple II). MAME itself annotates their
clocks "typical … not verified".

**The standing objection, which grows with every shared file.** `hgrpaint/` is
already duplicated between POM1 and POM2 with no shared build. Adding the VDP
puts ~2 700 more lines in both trees, and a silicon-behaviour fix will then
have to be applied twice with nothing to flag the omission. That is
survivable — `hgrpaint` proves it — but at this volume the question "shared
module or verbatim copy?" deserves deciding on its own, not during a port.
→ [`a2bus` survey](#a2bus-backlog)

## Deliberate skips (documented inline)

Conscious MAME divergences, justified in the code at the relevant spot.
Do not re-litigate without re-reading the original comment.

- 🟢 **`$C040` STRB not gated `!//c`** (MAME `apple2e.cpp:1927`) —
  no sink wired.
- 🟢 **ClockCard DATA_OUT live** vs MAME latch on CLK edge in
  MODE_SHIFT (`ClockCard.cpp:193-200`) — strict would break stock
  ProDOS.
- 🟢 **MouseCard PIA out_a/b without `scheduler.synchronize`** (MAME
  `mouse.cpp:280-294`) — no firmware-visible race.
- 🟢 **ClockCard offset model vs MAME `set_time`** — behaviorally
  equivalent as long as `timeFn()` is lock-step.
- 🔁 **MAME path drift refresher** — re-check ~every 6 months to
  track upstream renames (recent: `wozfdc.cpp`
  `bus/a2bus → machine`).

## Out of scope

Things we will not do unless explicitly requested + clear ROI.

- **Apple IIgs / ProDOS 16** — lives in the separate **pom2gs** project
  (Mega II + FPI + GLU + Ensoniq DOC); never in POM2.
- **Apple ///** + SOS — niche, *20-40 d*.
- **Clones** Franklin / Laser / Pravetz / Basis 108 — *2-5 d/clone*,
  low demand.
- **CFFA CompactFlash** — HDV + host folder suffices; the MAME-faithful port
  is done (`CffaCard`).

## Changelog

See [`CHANGELOG.md`](CHANGELOG.md).
