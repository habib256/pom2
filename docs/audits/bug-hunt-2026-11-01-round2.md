# Bug hunt — 2026-11-01, round 2 (eleven domains) + a test-quality audit

Eleven Opus agents swept eleven domains **read-only** on `b9818f5d`, then every
finding was re-verified by hand in the sources before anything was changed.
Seven domains were bug hunts, four were audits of the test suite itself. This
page is the dated record: what landed, what was verified and deferred, and what
nobody could verify. The fixes themselves are in the CHANGELOG.

**Baseline:** 425/425 ctest, every `tools/check_*.sh` green, `clang --analyze`
over all 270 first-party TUs (53 warnings, **no real defect** — the one
tempting null-dereference, `JoystickInput.cpp:151`, is a false positive: the
analyser loses what it knew about `d.rawCount` across the `std::array::fill`
two lines above).

Every fix below was **watched failing first**: its pinning test was mutated
back to the defect and seen red, then restored and seen green. The harness
sleeps a second before rebuilding, because a restore and a rebuild inside the
same second leave Make with a stale object — the trap already recorded in this
project's notes, which bit this hunt twice before it was handled.

---

## Landed

| # | Defect | Pin |
|---|---|---|
| UI-1 | The //c / //c+ **Chat Mauve adapter could never be removed**: `resolve()` re-applied the pre-split `slot_7_card` carry-over on every resolve while the persist guards were handed the raw (absent) `iic_rgb_adapter`, so `slotKeyIsUserChoice` refused to write the removal and the adapter came back on the next rebuild and every later launch. One helper, `savedSlotCardKey`, now answers both sides. | `slot_configuration_coordinator` |
| UI-2 | **One visit to the //c PAL profile rewrote `hi_res_mode` for good.** The profile forces its on-board RGB mode and nothing restored the user's pipeline on the way out; the live mode is persisted at quit. Back on the //e the RGB decode kept rendering, artifact colour was gone and the CRT sliders did nothing — on that launch and on every later one. `Apple2Display::modeForProfileSwitch` is the pure rule, and it deliberately leaves an RGB mode the user picked themselves alone. | `display_dirty_skip` |
| UI-3 | A **refused Apply wrote `""` over absent slot keys.** `getString(key, "")` collapses the two cases `has()` exists to tell apart; a refused rebuild on a fresh install turned all seven slots into explicit blanks, so the next launch came up with no Disk II, no mouse and no sound card. `Settings::capture`/`restore` restore absent as absent. Same fix in `swapSlotCardVariant`. | `settings_roundtrip` |
| UI-4 | `chatmauve_variant` was persisted on a //c because the guard read the **persisted** `system_profile`, which every restore path deliberately never writes — and `profileFromKey("")` answers a ][+, i.e. "has slots". The coordinator now asks for the live profile. | `frontend_device_panel_concurrency` |
| UI-5 | The CCS 7710 and DAN ][ **firmware toggles saved the key before the rebuild and never undid it**: a refused rebuild kept the old firmware live while the next launch quietly ran the new one. Both now go through `SettingRebuild.h`, which saves only once the machine took it. | `settings_roundtrip` |
| SES-1 | **A session load destroyed a live Premium SoftCard //e instead of refusing.** `outgoingSafe` looped slots 1-7, `SlotBus::clear()` empties slot 0 too, and `restoreSession`'s own slot-0 guard ran *after* that clear — so it found an empty slot, passed, and the card (its Z80, its `$C07E`, its live CP/M) was gone with the UI reporting "Session loaded". `docs/session-format.md` promises the opposite in those words. | `session_machine` |
| SP-2 | SmartPort **`CONTROL` answered `$00` for a unit in range whose bay holds no unit object**, where STATUS, READ, WRITE, FORMAT and the bus responder all answer `$28`. A fresh //e card has 8 bays and 7 nulls, so a chain enumerator probing with CONTROL code `$00` was told they exist. The full suite caught one assertion that had to move with it: `liron_smartport_dispatch` asserted `$00` for that case while its own setup comment and its own READ assertion say `$28`. | `smartport_liron_parity`, `liron_smartport_dispatch` |
| DBG-3 | **The OS-calls panel's "Clear" was inert.** `clear()` emptied the list without moving `emitted_`, the only change signal the panel watches, so the cleared lines stayed on screen until the guest happened to make another traced call. | `os_call_trace` |
| DBG-5 | **Stack-wrap off-by-one in four trace decoders.** `0x0101 + uint8_t(s + 1)` puts the wrap outside the 8-bit add: at `SP=$FE` the return address's high byte was read from `$0200` instead of `$0100` (and the low byte at `SP=$FF`), so the caller address, the command byte and the parameter-list pointer all came from an unrelated page. Fixed in MLI, SmartPort, Monitor GETLN and CP/M-65. | `os_call_trace` |
| MED-2 | `Dos33Disk::payload` formed `raw.begin() + 4` on an **empty** vector (a catalog entry whose T/S list is 122 all-zero pairs) — undefined, and an abort under the nightly's hardened standard library. | `dos33_disk_fs` (the UB half is nightly-only; the test pins the length clamp instead — see below) |

### Test quality, landed

| # | Change |
|---|---|
| TQ-H1 | **`tools/check_ctest_skips.sh` saw only `^SKIP`.** Two audits independently found ~33 sites in 24 files that skip a sub-case, print a word the detector could not see, and exit 0 — `[SKIP]`, lowercase `skip:`, `WARN`, `NOTE: … skipped`, `(skipped: …)`, `[ OK ] … (SKIP …)`. The one guard built to stop "a missing fixture reads as a pass" was blind to all of them. The detector now matches the announcement shapes case-insensitively, anchored (a bare substring match also hits ctest's framing lines and a test that legitimately talks about skipping), and the three CCS 7710 announcements — one of them a bare wordless `return;` — now start with `SKIP`. |
| TQ-D | `dos33_disk_fs` **computed a 77 and threw it away**, decorating a string while exiting 0. It returns it. |
| TQ-C3 | The one-line hole the mutation audit ranked highest: nothing asserted `EzColorCard::takesMonitor() == false` or the Videoterm's `true`. Reverting that one-line override restored a reported bug — a Videoterm's 80 columns vanishing behind an E-Z Color — with the whole suite green. Each half is now pinned in the test that links that card. |

---

## Verified, deferred

Each of these was re-read and confirmed in the sources. They are deferred for
size, not doubt. Severity is this project's usual order: silent data loss >
wrong behaviour a user sees > wrong behaviour software can see > hygiene.

### Highest

- **CPU-1 — a non-stock `--speed` scales the CPU clock but not the bus clock.**
  `EmulationController.cpp:89` hands every card `setStandardClock(vt.cpuClockHz)`,
  the *unscaled* nominal, while the CPU clock is scaled by
  `baseCyclesPerFrame_ / vt.cyclesPerFrame`. At toolbar 2× a 6522 T1 latch needs
  twice the cycles while the beam still gets one frame's worth, so a
  frame-synced Mockingboard tune drops to half tempo against the raster — and
  the speaker *does* scale, so the `$C030` tone doubles in the same buffer. At
  MAX the ratio is ≈0.017: the VIAs effectively stop and a program waiting on a
  Mockingboard IRQ hangs. The same divider feeds the Disk II motor clock, the
  SSI263 XCK and the Sony platter. This is **not** the accelerator case
  `SlotBusClock.h` exists for — `applyAcceleratorClock` leaves
  `setVideoClockRatio` at 1.0 for a speed change, i.e. the crystal is modelled
  as faster too. `device_clock_fanout` only ever exercises ratio 1, which is why
  nothing caught it.
- **MED-1 — one host folder can be served into two bays, and both write it
  back.** `StorageCoordinator::mountBlockBytes` never applies the "one image,
  one drive" rule its file-image sibling applies at `:690`. Each bay keeps its
  own snapshot, so after the first guest write each flush sees the other's
  output as host-newer and copies every differing file into
  `/work/.pom2-conflicts/`, for ever. Worse, the synth branch of
  `commitWriteBack` takes only the per-backing mutex, never `imageCommitMutex`,
  so two decodes of one tree can interleave their writes with the losing blocks
  already retired clean — real loss, not just conflict copies. Reachable from
  `POM2 --prodos-folder /work boot.hdv` plus a GUI mount of `/work`. Note that
  calling the existing helper is not enough: for a synth volume `path_` is the
  label `"[host folder] /work"`, so `sameImageFile` would compare strings and
  miss `/work` vs `/work/`.
- **NET-1 — `DISCON` from `SOCK_CLOSE_WAIT` drops the TX tail the guest was
  told was sent.** `W5100Device.cpp:719-730` gates the graceful close on
  `SOCK_ESTABLISHED` only. `sendData` allows CLOSE_WAIT, so a short write parks
  the remainder in `pendingTx` *and raises SEND_OK*; the guest then issues
  DISCON, takes the `else` arm, and `clearSocket` discards the tail with the fd
  closed. The ordinary HTTP/1.0 and IRC shape the file's own comments describe.
  `kW5100SnSrLastAck` is already defined and unused.

### Then

- **DBG-1 — the resume amnesty is spent on an offer an interrupt preempts**, so
  Run re-stops at the same breakpoint having executed nothing; with a
  level-triggered IRQ still asserted, Run can never make progress. The fix is
  the mirror of the one the 2026-11-01 hunt applied to `stepSkipFirst_` —
  retire the flag in `onStepCompleted`, not on the offer — and it needs
  `debugger_banks_cases.h:70-72` updated, which asserts today's offer-based
  semantics while the real run loop always calls `onStepCompleted`.
- **DBG-2 — the OS trace decodes a call twice when its entry is offered
  twice** (a debugger stop at `$BF00`, or an interrupt taken there), and
  `abandon()`'s `<=` then closes the first copy with a bogus `-> (no return)`.
  `onZ80Instruction` and `monStep` already carry the guard; MLI, SmartPort and
  CP/M-65 were missed.
- **NET-2 — MACRAW/IPRAW frames are staged only when the guest reads
  `Sn_RX_RSR`.** `poll()` drives the receive side for host-socket modes only, so
  the mode-independent `Sn_IR` RECV the 2026-09-08 fix added still never fires
  on its own and an interrupt-shaped raw driver waits for ever. The documented
  intent is in `DEV.md` § Uthernet II; the register plumbing landed, the driving
  did not.
- **NET-3 — the LISTEN port-squat probe is a blocking `::connect()` on the CPU
  thread under `stateMutex`** (`W5100HostSockets.cpp:139-146`). Against a
  saturated loopback accept backlog the SYN is dropped rather than refused, so
  it blocks for the kernel's retry ladder — tens of seconds with the machine
  *and* the window frozen together. `connectBounded` is one include away.
  Needs the `uthernet_allow_loopback` opt-in.
- **MED-3 — `pom2_cpm` ignores the CP/M R/O attribute** and deletes or
  overwrites a file CP/M itself refuses to touch; `list()` already decodes the
  flag and `ls` prints it. The DOS 3.3 sibling honours its lock bit.
- **MED-4 — the ProDOS decode charges its 512 MB budget for files it then
  skips**, so a volume of mostly-unchanged sparse files exhausts the budget,
  reports `filesUnsaved`, and fails the whole write-back permanently — naming a
  file that is fine.
- **SP-1 — `SmartPortHub::recalcActiveDevice` calls `setSony35(nullptr)`**,
  which also clears the IWM's 5.25" `DiskImage` pointer — the exact call the
  surrounding comment forbids and `releaseSony35()` exists to avoid. Reachable
  on a stock //c+ (the external Sony drives are null by default). No
  guest-visible symptom could be constructed, because the //c-class authority
  split already withholds those accesses from the Disk II; report as hardening.
- **NET-4 — `LISTEN` from a non-INIT state tears the socket down** instead of
  being ignored, killing a live accepted connection and raising TIMEOUT on it.
  The stock `ioLibrary_Driver` guards the register write itself, so real
  software is unlikely to reach it.
- **SES-2 — `reconstructSession` performs its irreversible teardown before
  three fallible steps**, none of which republishes: a failure there would leave
  the AI control server detached for the process's life, *no* audio source
  registered at all, and the Disk II panels gone. No reachable trigger was
  found — all three are functions of bytes already validated on the candidate —
  and SES-1's fix removes the one near-miss. Make the post-detach sequence
  infallible, or publish before returning false.
- **DBG-4 — the Debugger panel's snapshot holds `stateMutex` for 65 536
  `debugBank()` calls**, a full profile-map sort and up to 4 000 string copies
  **every frame**, including for collapsed sections. The block's own comment
  still budgets for the 64 KiB memcpy it opened with. Only ~40 disassembly rows
  and the listed breakpoints ever read `snap.banks`. Not measured — the claim is
  from call counts, not a profile.
- **SES-3 — `noteTimeJump()` skips slot 0** while the rewind snapshot captures
  and restores `SLOT0`. No current slot-0 card overrides `onTimeJump`, so
  nothing is lost today; it is a one-token divergence from the capture loop.
- **CPU-2 — `M6502::softReset()`/`hardReset()` wipe the wire-OR IRQ mask**,
  which models nothing (RESET does not clear a 6502's IRQ *input*) and desyncs
  the CPU's view of the line from the one card that deliberately models a chip
  with no reset pin — the CCS 7710's 6850, the only IRQ-asserting card with no
  `onReset` override. Narrow: a rebooting guest usually re-inits the control
  register, which republishes.
- **CPU-3 — `FastChip::loadSnapshotState` restores `powerOnSpeed_`**, a host
  setting the sibling `ZipChip` deliberately refuses to restore for the same
  reason. The Machine menu then disagrees with `state.cfg` until the slider is
  touched again.
- **MED-5 — `ProDOSHostNameMap`'s registry keys on the raw folder string**,
  bypassing the `hostNameMapKey()` normaliser that exists for it. All four
  producers pass the same string today, so no live failure could be built;
  latent trap plus a small unbounded leak when a build's mount then fails.
- **CPU-4 — both socket accelerators decode only `$C070`**, not the
  `$C071-$C07F` PTRIG mirrors POM2's own `Memory` honours (citing MAME
  `apple2.cpp:555 .mirror(0xf)`). Unverified against MAME's accelerator hooks —
  no MAME source tree on the machine — so possibly MAME-faithful and only an
  internal inconsistency.

---

## Test quality, deferred

Ranked by how much false confidence each removes.

1. **~33 sub-case skips exit 0.** With the detector widened (TQ-H1) the next
   step is the tests themselves: roughly 21 gate on fixtures that **are** in
   `tools/test_media.sha256` and should be fatal, and ~12 are genuine
   host-capability gates (symlinks, hard links, `chmod` as root, a
   decimal-comma locale, Ghostscript, libslirp, Windows) that want a
   `SKIP`-prefixed line plus an allow-list entry. The worst offenders:
   `diskii_lss_smoke` (7 of 11 cases `return true` on a missing
   `roms/diskii_p6.rom` — including the two the Disk II data-separator fixes
   are pinned with), `floppy_sound_smoke` (14 of 15 behind a **working
   directory** probe), `iicplus_accelerator` (the whole test), `rom_import`
   and `pom2_core_api` (which *fail* instead of skipping),
   `mouse_card_applewin` (4 cases that self-disable on the ROM loader they
   test), `card_snapshot_contract` (an empty blob disables 6 of 7 contract
   phases with no failure, and 9 firmware-carrying cards are dropped past the
   coverage guard).
2. **One fixture is referenced and in no manifest:**
   `disks_3.5/The New Print Shop 800K.po`. Case E of `iic_external_smartport`
   — one of the "five boots" CLAUDE.md claims are pinned — has therefore never
   run on a runner. This is a manifest action (add it to
   `tools/test_media.sha256` and the `pom2_media_202611` item), independent of
   any test edit.
3. **`POM2_TEST_TIMEOUT_SCALE` does not reach inside the tests.** CI's nightly
   sanitizer job sets `=6` on a machine the project itself measured as 5.9×
   slower, while ~15 in-test wall-clock bounds are plain literals
   (`sp_over_slip_link:521` `ms < 60`, `ssc_acia_smoke:1071` `n <= 120`,
   `w5100_name_resolver:96` a 20 ms failure TTL between two calls). One
   `ENVIRONMENT` line plus a `pom2test::timeScale()` helper fixes the category.
   `POM2_WALLCLOCK_TESTS` is also missing eight tests that now assert wall
   clock.
4. **The portable/Windows leg bypasses three mechanisms** — no
   `SKIP_RETURN_CODE 77`, no TIMEOUT floor/scale, no `test_property_sweep` —
   because `tests/CMakeLists.txt:42-46` returns before reaching them. Concrete
   consequence: `bus_fastpath:122` and `memory_cpu_view:215` `return 77`, a
   *skip* on Linux and a *hard failure* on Windows from the same source line.
   And `AtomicFileReplace.h`'s six `_WIN32` branches — which every media
   write-back goes through — are executed by **no CI job at all**, because
   `atomic_file_replace` is not in the portable set.
5. **The ctest count floor is 300 against 360 registered tests**, so every
   configure-time-gated test (Klaus, zexall/zexdoc, Tom Harte, the three
   headless ROM tests) can vanish silently. Assert the gated names
   individually.
6. **Loose assertions where the specific value is the point.**
   `disk_2mg_writeback` is a documented tautology — every assertion holds for a
   file `saveDirty()` never touched, and it is the only test of the 2IMG write
   path. `sony35_iwm_read_path` asserts the *cardinality* of the working IWM
   mode set, not its identity, so any permutation of the window table leaves
   exactly one mode working and the test green. `storage_lock_budget` cases B
   and C discard their return values, so a no-op measures 0 µs and passes — the
   file's own comment records that this already bit it once.
   `display_golden_hash` never compares `results.size()` against
   `kGolden.size()`, so 185 pinned paths can decay to 1. `printer_glyph:271`
   asserts `k.count >= 0` on a monotonically incremented `int`.
7. **`POM2_MEDIA_WRITE_DEFAULT=protected` stops at ctest's edge**, and
   `pom2_core_api_test:862` turns it off process-wide and then mounts a
   hash-pinned WOZ from the test-media archive. Nothing is dirtied today, so
   nothing is written — one future edit away from a hash mismatch failing a
   *different* job.
8. **`tools/coverage_linked_sources.txt` records 120 sources; CMake puts ~214
   on a test target's link line.** The file was armed from a real measured run
   on 2026-09-15 (its header records the three data-table files that run
   failed on), so it is **not** the stale hand seed it first looks like — but
   the four later additions were hand-appended, and any source linked since
   then that no measured run recorded sits outside the drop guard. Only one
   `tools/coverage.sh --update` run settles how many; whichever it is, the fix
   is to commit the measured list.
9. **88 tests name a scratch file `temp_directory_path() / "<fixed literal>"`**
   instead of `pom2test::tempPath()`. No in-suite collision exists today; the
   exposure is two runs on one box and stale debris from a killed run.
   `child_process_test:589` uses a literal `/tmp/...` outside `TMPDIR`.

### From the mutation audit of the last two hunts

42 fixes PINNED, 8 WEAK, 8 UNPINNED. The unprotected ones, worst first:
`MediaSource`'s extension-less-URL archive unpacking and its soft-404 refusal
(both live inside `download()`, which shells out to real curl — they need the
injection seam `RomFetchSources::download` already has); the audio buffer
ladder's *climb* (`AudioCoordinator::tick`, `learnedFrames_`,
`audio_buffer_learned` are reached by no test, so the Pi/phone crackle can
return silently); `TMS9918`'s `~regs[3]` UB fix, which is value-identical and
therefore unpinnable by construction — its guard is the nightly job, not the
425-case suite; the held paste's hash hoist; the Machine-menu Premium SoftCard
tick; `mount35`'s lost `click` argument; and the E-Z Color's in-instruction
bus-clock conversion.

---

## Could not verify

- Nothing in the read-only sweeps was executed: every agent claim was static
  reading. Each **landed** fix above was then verified by hand and by a mutation
  control; the **deferred** ones were verified by reading only.
- MAME line-level parity for `IWMDevice.cpp`'s `controlAccess`/`sync` port and
  for the accelerators' `$C070` decode: no MAME source tree on this machine,
  only the 0.288 binary.
- Whether libslirp 4.9.3 routes a guest-built **IPv6** frame naming `::1` with
  `in6_enabled = false`. `SlirpBackend::framesNamesHostLoopback` inspects IPv4
  only, so POM2's own filter would not refuse it. Worth a check against
  `libslirp/src/slirp.c`; the one-line hardening is to refuse `86 DD` outright
  while the card has no IPv6 story.
- Whether any shipping guest probes a SmartPort chain with `CONTROL` code `$00`
  (SP-2's scenario). The code inconsistency was certain and is fixed; the field
  impact is inferred.
- Whether MSVC honours `-UNDEBUG` on the Windows portable leg. If it does not,
  all 12 Windows cases assert nothing.

---

# Round 3 — seven fresh domains (same day)

Seven more domains, chosen because no earlier hunt had touched them and
because a v1.1 release was next: packaging/CI/release, CLI/headless/SDK,
the browser build, printers end-to-end, the paint/sprite toolkit, the video
cards, and the card sound chips + cassette. Same method: read-only sweep,
hand verification, mutation control on every fix.

Two of these agents went further than reading. The video-card one fetched
MAME's `mc6845.cpp` / `a2ultraterm.cpp` / `a2videoterm.cpp` and AppleWin's
`VidHD.cpp`, and then read the tracked UltraTerm firmware's bytes; the
CLI one ran the built `pom2_bench` and measured. Those are the two reports
whose findings needed the least re-derivation.

## Landed

| # | Defect | Pin |
|---|---|---|
| PKG-5 | **A two-component `project(VERSION 1.1)` would have broken the Windows release leg** — CMake leaves an unspecified component as the EMPTY STRING, so `POM2.rc.in`'s `FILEVERSION @MAJOR@,@MINOR@,@PATCH@,0` expands to `1,1,,0`, which `rc.exe`/`windres` reject. Nothing upstream objects: the release job compares the tag to `PROJECT_VERSION` as a string, `check_version_strings.sh` reads `[0-9.]*`, and the publish job has an explicit two-component branch — so the whole release path assumes `1.1` is legal and only Windows would die, after every other package was built. The components are normalised to 0 before `configure_file`. **Verified with `cmake -P`**, which the agent could not do: `1,1,,0` before, `1,1,0,0` after. | — (a `cmake -P` check is noted as the honest pin) |
| PKG-1 | **The release workflow's quality gate never ran `check_version_strings.sh`.** Its own header forbids relying on a separate, concurrently-running CI workflow — and that was the only place the guard ran. A v1.1.0 tag would have published seven packages with a README still naming the 1.0.0 AppImage and a Release body fallen back to a generated commit list, ~80 minutes before `ci.yml` went red. Added, with the workflow-pin guard, *before* the 180-minute build. | the guards themselves |
| PKG-2 | **`check_workflow_pins.sh`'s image rule was a recognise-an-image regex blind to every Docker Hub short name.** I reproduced it: `emscripten/emsdk:3.1.64`, `node:20`, `myorg/b:latest`, `python:3.12-slim` and a tagless `docker run alpine` all passed while the script printed "passed". The rule is inverted — any `image:`/`container:`/`*_IMAGE:` value and any `docker run\|pull` argument must carry `@sha256:` — continuation lines are joined before scanning, and the guard finally has the `--self-test` it was the only one in the pre-build block to lack, with all five blind shapes as negative controls. Writing that self-test also found that **`grep -n` prints no filename for a single file**, so the whole parser shifted on a one-workflow tree and reported nothing; every grep now uses `-H`. | `--self-test`, 11 controls |
| WASM-1 | **Printing one page in the browser tore the whole tab down.** `PrinterHistory` hands each ejected sheet to a worker thread; the browser build has no pthreads by design, so `std::thread`'s constructor throws `ENOTSUP` — and `-fignore-exceptions` is emcc's default with nothing turning it off, so the throw reaches `abort()`. I confirmed the committed `wasm/POM2.js` carries **zero** `invoke_` thunks and no `__cxa_increment_exception_refcount`: there are no landing pads at all. A guest `PR#1` with a form feed, or the palette's "Print screen", was enough. Now encoded inline under `__EMSCRIPTEN__`, the shape `BlockWriteBackExecutor` already uses. | — (see WASM-3 below for the guard that would catch the class) |
| WASM-2 | **The LaserWriter head did the same, and spawned before looking for Ghostscript.** `findPostScriptInterpreter()` now runs first and the empty result is returned synchronously — which fixes the browser without an `#ifdef` (`ChildProcess` is already stubbed there) and stops a desktop with no Ghostscript paying a thread per job to produce one error string. | — |
| PRN-1 | **The ImageWriter II's draft rate was 180 cps where everything else says 250.** `git` shows the commit that introduced the per-model table (`d20871c3`) replaced `kDraftCps = 250.0` with 180 in that one row — the correspondence tier's rate, selected from the host's *Draft* rung — and left the Specifications comment, `speedName`'s "Draft (250 cps)", the header doc, DEV.md and the test's own arithmetic at 250. Every draft print took 39 % too long and a back-pressured guest sat in its ACK loop 39 % longer. Every other row carries its own published figure, so the default was the only one out. | `imagewriter_smoke` (the pacing case asserted `> 0 && < 11`, true for both rates; it now asserts exactly 4 characters per 60 Hz frame — 3 at 180) |
| PRN-2 | **A guest could freeze the UI thread with a two-byte sequence.** The Diablo 630's `ESC -` pushed a vertical tab stop with no dedup and no cap and re-sorted the whole vector on every insert; `ESC 1` deduped but was also uncapped. Neither costs mechanism time, so the paced drain allows ~8192 pushes per UI frame. Both racks are now bounded (32/16, like the ESC/P head's), deduped, and inserted at `lower_bound`. | `imagewriter_escape` |
| VID-1 | **Every row of the UltraTerm's four interlaced modes lost its last scanline.** `Hd6845Crtc` latched R8 and read it from nowhere, so a character row was always R9 + 1 rasters; on an HD6845S in interlace-and-video mode (R8 bits 1-0 = 11) it is R9 + 2. The witness is the card's own firmware, and I read the bytes: the three non-interlaced tables use R9 = 8 — the nine rasters a 9×9 glyph needs — and the four interlaced ones use R8 = 3 with a *smaller* R9 (14, 11, 7, 11) precisely because the chip adds 2. So the 80×48 mode, the card's headline feature, asked for nine rasters and got eight: the ninth raster of every glyph on all 48 rows was never painted, the picture was 48 scanlines short, and the frame rate was 33.9 Hz against MAME's 30.1, skewing the cursor blink by ~12 %. | `videx_ultraterm`, driven from the firmware's own 80×48 table |
| SND-2 | `MockingboardCard` never called `Ssi263::prepareTimedPlayback()`, so whenever the audio-side shadow was unprimed (power cycle, cycle rollback, snapshot load, `ctlEvents_` overflow) it seeded itself from the **post-write** live registers and voiced them ~40 ms early. The Phasor has always primed first. | — |
| SND-3 | `PhasorCard`'s realtime render `resize`d its speech scratch from `frameCount`, so every rung the device-buffer ladder climbs cost a heap allocation **inside the audio callback** — at the moment the callback is already late, which is what triggered the climb. Reserved in `setSampleRate` and grown only, as the Mockingboard it was extracted from has always done. | — |

### SND-1 — fixed, but the reported symptom is NOT reproduced

The SSI263 playback queue was stamped `lastSyncCycle_` (CPU cycles) while the
render cursor is anchored to `busNow_` and paced at `ayClockHz()` — the comment
over `cyclesPerSample` states the rule ("the stamps are slot-bus ticks") and the
PSG queue beside it has always honoured it. Under an accelerator
`SlotBusClock::ratio()` drops below 1 and the two counters really do diverge, so
the stamps were in the wrong domain and both call sites now use `busNow_`.

**But the reported consequence — "speech goes mute under any accelerator" — is
unverified.** A new case drives a Sound II at 1×, 3.5× and 4× for ~0.4 s of
emulated time; the measured energy is 468.47 at every multiplier, and reverting
the stamp leaves the suite green and that number unchanged. So the fix is a
consistency correction with a sound argument and no demonstrated user-visible
symptom. The case is kept as a property test and labelled as such in the source.
If someone builds a reproduction it belongs there.

## Verified, deferred — round 3

- **WASM-3 — the browser build has no exception handling at all.**
  `-fignore-exceptions` is emcc's default, nothing in the `if(EMSCRIPTEN)`
  block turns it off, and the committed glue proves it. So every `catch` in
  `src/` is dead there: `Settings.cpp`'s malformed-numeric fallback among
  them, which means one non-numeric value in a persisted `/persistent/state.cfg`
  would abort the module on **every** later load — the page dies before
  anything can rewrite the file, so that origin is bricked for that visitor.
  No live cross-type key exists today, so it is latent. Adding `-fexceptions`
  costs bundle size and speed on the shipped demo, which is a **decision for
  the maintainer**, not a drive-by fix; WASM-1 and WASM-2 remove the two live
  aborts regardless. The cheap CI guard is a `grep -q "invoke_" wasm/POM2.js`
  in the wasm job.
- **WASM-4 — `userDataDir()` has no Emscripten branch**, so F9 screenshots,
  saved BASIC listings, printouts and the media cache land on a MEMFS path
  outside the one IDBFS mount and vanish on reload, while the status bar
  reports a host path as though it had been written. Its sibling
  `userConfigDir()` does return `/persistent`. The BASIC listing additionally
  claims "(and copied)" while emscripten's `glfwSetClipboardString` is a
  no-op — the visitor is told twice that something happened.
  `downloadBrowserFile()` already exists and none of these paths uses it.
- **WASM-5 — pinning a device-buffer rung *below* the browser default is
  accepted, shown, persisted and ignored for ever.** The constructor discards
  a remembered rung that is not strictly larger than the platform default, a
  clamp written for a stale *learned* value and applied to an explicit *pin*;
  the panel then promises "(512 frames at the next load)" on every load,
  permanently.
- **WASM-6 — `key_chord_policy`'s abort-stub scan has drifted from the pinned
  toolchain in both directions**: ~13 aborting GLFW entry points are missing
  from its list, three of the six it names are silent no-ops in emsdk 6.0.8,
  it covers 7 of ~540 files, it mishandles `#if !defined(__EMSCRIPTEN__)`
  (false alarm on the guarded branch, blind to the real one), and it only
  knows GLFW names — so it cannot see the `std::thread` aborts of WASM-1/2.
  Deriving the list from `$EMSDK`'s `libglfw.js` at test time and scanning all
  of `src/` would make it catch that whole class.
- **PKG-3 — a `wasm` manifest entry is the one payload kind no parser
  validates.** `stage_data.sh` collects them without an existence check, CMake
  drops a missing `--preload-file` with no diagnostic, and the release job's
  only WASM gate is `test -s` on four files plus a 100 MB ceiling — so the
  browser bundle can ship with its //c + //e ROMs silently absent and the
  Chrome smoke, which waits for the splash to hide and checks settings
  persistence, would not notice. All four are present today.
- **PKG-4 — a poisoned test-media cache is permanent.** `fetch_test_media.py`
  replaces a cached file that is not a zip, but never a cached zip whose
  members fail the manifest; the Actions cache is keyed on the manifest hash
  alone. One truncated-but-valid download to the first job that misses would
  be restored by every later job of every later run, failing them all
  identically, until someone edits the manifest or clears the cache by hand —
  while the log says "using the cached archive". The action's own stated
  invariant is "a cache is never trusted further than a download".
- **CLI-1 — `pom2_bench` takes two numbers straight off argv with no error
  channel.** Measured on the built binary: `--cycles-per-frame abc` silently
  runs the 17045 default, and `--read-watch zzz` or `--read-watch 10000` both
  arm a read watchpoint at `$0000` the user never asked for, dropping the
  measured speed from ~134 to ~91 MHz — in the binary whose whole contract is
  reproducibility, and which is the callgrind subject and the PGO training
  driver. A value flag written without its value is also reported as an
  unknown flag. `pom2_headless.cpp` carries both fixes, and the rationale, one
  file away.
- **CLI-2 — `pom2_bench --dump-text` is parsed and is what `docs/PERFORMANCE.md`
  tells the reader to use, but `usage()` never mentions it** (confirmed:
  `--help | grep -c dump-text` is 0). CLAUDE.md calls the usage text the source
  of truth, and `checkUsageParserSymmetry` is only pointed at `CliDispatcher.cpp`
  and `pom2_headless.cpp`.
- **CLI-3 / CLI-4 — two probes write outside `probeOutDir()`.**
  `u5_woz_save_probe` writes `<base>.hgr1/2` into the **current directory** —
  which that probe requires to be the repo root — and `artifact_phase_probe`
  hard-codes `/tmp/test4_signal.bin`, ignoring `$TMPDIR` and
  `$POM2_PROBE_OUT`. Both swallow a failed `fopen` and still print their
  success line. `ProbeOutDir.h` states the policy and the incident that
  produced it. One source-scan guard over `POM2_PROBE_TARGETS` covers both.
- **CLI-5 — `--fujinet-slot N` alone is validated, recorded and silently
  ignored**: every consumer is gated on a transport that flag never sets. It is
  the one accepted-but-inert input in a parser that otherwise refuses every
  near-miss loudly, and it is exactly the flag someone reaches for when they
  already believe a card is being plugged.
- **VID-2 — the UltraTerm's device-select *write* decode is three addresses too
  wide.** It dispatches on `low4 & 3`, so `$C0n4-$C0nF` writes reach the 6845
  and both control registers — and in Videoterm-emulation mode (the reset
  state) `$C0n4/8/C` are precisely the bank-select addresses, so `STA $C0n6`
  rewrites control 1 and can take the monitor away mid-draw. MAME decodes
  `case 0,1,2,3` exactly and POM2's own `VidexVideotermCard` already does too —
  the two sibling cards disagree. The documented A0-A1 mirror is about *reads*.
  The agent disassembled the tracked firmware and found it only ever uses
  offsets 0-3, so the card's own driver cannot trigger it.
- **PAINT-1 — Ctrl+Z during an open pencil drag corrupts the sprite editor's
  undo stack** and resurrects an already-undone stroke, because
  `HgrSpriteEditor` has no `flushOpenStroke`. The paint editor has exactly that
  guard, for exactly this reason, and states the invariant in its header.
- **PAINT-2 — the HGR Fill tool floods against the user's canvas *pipeline*.**
  Connectivity is exact-RGB equality of a page rendered through the host, and
  the host renders through the selected pipeline — including two CPU composite
  demods, whose output for a periodic field varies with each dot's NTSC phase.
  The flood's premise and its measured edge constants only hold for the MAME
  artifact LUT, and the one test hard-codes `ColorNTSC`. The portable module
  already carries a pipeline-independent decode (`hgrDecodeScanlineRgb`, pinned
  byte-identical to the display's) that the flood should use instead — which
  would also make `fillRegion` headless-testable. Failure *shape* unverified.
- **PAINT-3 / PAINT-4** — the status bar prints an HGR-interleave address while
  editing a lo-res GR page (`$23D0` for a block that lives at `$07D0`), and the
  file browser highlights 8 KB files when loading a 1 KB GR page, so selecting
  the highlighted one loads the top-left of an HGR picture as lo-res nibbles.
  Both pure-ImGui, both one arm short of a `switch` that already covers DHGR
  and DLGR.
- **PRN-3 — ESC/P `ESC D NUL` restores the power-on tab stops instead of
  cancelling them**, because "empty `epsonHTabs_`" is the encoding for
  *defaults*. The vertical side has the `epsonVTabsSet_` flag that distinguishes
  them; the horizontal side has no counterpart. ESC/P says HT is ignored when no
  tab is set after the current position.
- **PRN-4 — the Diablo 630's `ESC L` and `ESC FF n` have none of the "one line
  must fit" floor** that `ESC H` and ESC/P's `ESC N` carry (and whose comments
  record the incident: "`ESC H 0000` set a zero-length page, so every single
  line feed ejected"). A driver emitting its page setup at home turns every
  subsequent LF into a blank sheet — and `archiveNewPrinterPages` writes those
  into the durable print history. Faithfulness cuts the other way here (a real
  630 would also end the page), so the finding is the inconsistency plus the
  durable side effect.
- **SES/CPU/MED leftovers from round 2** are unchanged above.

## Could not verify — round 3

- Whether MSVC honours `-UNDEBUG`; whether `actions/cache/save` on a reserved
  key warns or fails; the Windows `Compress-Archive` and macOS `.dmg` paths.
- The browser findings were read, not observed: no agent could run a real
  browser. WASM-1's chain (`ENOTSUP` → no landing pad → `abort()` →
  `Module.onAbort`) is inferred from the toolchain's own stub and the committed
  glue, both of which I checked directly.
- Diablo 630 escape parameter counts (`ESC CR`, `ESC DC1`, `ESC SYN`, `ESC SUB`,
  `ESC SO`, `ESC EM`, `ESC CAN`, `ESC .`) — no 630 SPI document available. If
  any takes no parameter, that handler eats the next byte of the document.
- `AppleParallelCard::slotRomRead`'s PROM address twiddle against MAME
  `a2pic.cpp:286-293`; the FX-80's default HT stop spacing; the Eve's and RVB
  Graph's register semantics beyond internal consistency; the Féline dot rules
  against fenarinarsa's captures.
- `Ssi263::renderSamples`'s DUR=1 advance averages 1.2 source samples where the
  frame ratio implies 4/3 — worth one line of AppleWin diff.
- Whether any shipping UltraTerm software writes `$C0n6` (VID-2's worst case),
  and whether any guest probes a SmartPort chain with `CONTROL` code `$00`.

---

# The WASM exception question, measured

I framed `-fexceptions` as "costs bundle size and speed", and was asked the
sharper question: if the browser build does not handle exceptions, they should
not be wasting space either. Measured on the committed artifacts rather than
estimated.

**The expensive half is already gone.** `wasm/POM2.js` contains zero
`invoke_*` thunks, zero `setThrew`, zero `__cxa_begin_catch` and zero
`__cxa_find_matching_catch` — emcc's default (`DISABLE_EXCEPTION_CATCHING=1`
plus `-fignore-exceptions`) strips every landing pad and discards every
`catch` block. There is no thunk wall to reclaim.

**What remains is 414 bytes.** Fifteen strings in a 5 521 125-byte
`POM2.wasm` — 0.0075 % — and they are libc++ type names (`St9bad_alloc`,
`NSt3__212system_errorE`, `NSt3__210filesystem16filesystem_errorE`, …) that
ride in the prebuilt library because libc++ itself still throws.
`-sDISABLE_EXCEPTION_THROWING=1` would not remove them. (An earlier count of
21 KB was wrong: the pattern was catching every libc++ mangled type name,
`shared_ptr` and `std::function` included, not the exception types.)

So there is nothing meaningful to reclaim, and no size argument either way.
What the question does expose is the *real* cost, which is not bytes: since
the build neither pays for exceptions nor gets them, **a throw is an immediate
`abort()`**, and that is what let "print one page → dead tab" ship. That is
free to enforce, so it now is:

- `key_chord_policy`'s source scan was extended from GLFW abort stubs to
  **thread spawns** (`pom2::guardedThread` / `std::thread`), over the TUs that
  hand work to a worker. It catches `PrinterHistory.cpp` and
  `PostScriptRender.cpp` the moment their `__EMSCRIPTEN__` guard is removed
  (verified by removing it), and the correctly-guarded spawners —
  `BlockWriteBackExecutor`, `EmulationController`'s `WriteBackQueue`, the
  socket workers — stay invisible to it, which is the point.
- That scan also exposed a weakness in **my own** WASM-2 fix: moving the
  Ghostscript lookup ahead of the spawn makes the browser path return early at
  *runtime*, but the spawn was still compiled and the property rested on a
  runtime invariant. `PostScriptRender` now carries a compile-time
  `__EMSCRIPTEN__` guard as well.

`-fexceptions` stays unapplied: it would cost size and speed to buy back
`catch` blocks on paths that, with the two spawns guarded, no longer throw.
The remaining latent case (a malformed numeric in a persisted
`/persistent/state.cfg` reaching `std::stoi` with its `catch` compiled out,
which would brick that origin) is recorded under WASM-3 and is unreachable
today.

## Six more "cannot fail" tests, fixed

| Test | Was | Now |
|---|---|---|
| `iicplus_accelerator_test.cpp:57` | lowercase `skip:` + `return 0` — the WHOLE test (CGGA 4 MHz, `cpuSpeedMultiplier` after reset, Esc-at-reset, the IWM drive-disable delay the 2026-10-25 bug produced) passed silently without `roms/apple2cp.rom` | `SKIP` at line start + `return 77` |
| `iwm_mig_snapshot_test.cpp:310` | lowercase partial skip, `main` always 0 — the only case pinning the 3.5" revolution anchoring could stop running with no signal | a flag, and `main` returns 77 |
| `printer_glyph_test.cpp:271` | `assert(k.count >= 0)` on an `int` that starts at 0 and is only ever `++`'d — a tautology, under a comment claiming "the bundled font has a glyph so something must print" | **the comment was false**: row `0x7F` of `kBBFontCp437` is all zeros, so a blank is correct. Asserts the blank, with the font table as the oracle, plus a positive control (`$B0`, the light-shade block, 8 non-zero rows) so "inks nothing" cannot pass for a dead printer |
| `printer_grain_pileup_test.cpp:94` | ceilings only (`maxStep`, `peak`), and `drain()` answers `{0,0}` for silence — a printer making no sound at all certified as crackle-free, which is the regression *next to* the one the file pins | a floor as well: `peak > 0.01` |
| `mouse_card_applewin_smoke_test.cpp:590` | `FirmwareRig::ok` conflated "dump absent" with "`loadAppleIIRom`/`loadRom` refused it", so a regression in the loaders silently disabled the four firmware cases that would have caught it | present-but-unloadable now asserts; absent prints `SKIP` and `main` returns 77 |
| `key_chord_policy_test.cpp:179` | a source scanner with **no positive control**: a broken `PpFrame` model would inspect zero lines and pass — the state in which a newly added unguarded call ships and `abort()`s the module | two fixtures with a known answer (reachable → caught, `#ifndef __EMSCRIPTEN__` → not), a `g_linesInspected` assertion, and the thread-spawn scan above |
