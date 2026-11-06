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
  *2026-11-06: a fix (the speed-scaled clock as the bus clock) was written and
  WITHDRAWN before commit — `setStandardClock` is also the absolute rate the
  Sony platter, the FLUX time base and the SSI263 XCK convert physical time
  with, and those must stay nominal or a 3.5" / FLUX disk stops reading at 2×.
  Needs machine time and wall time separated in the card clock API (DEV §
  Mockingboard). Still open.*
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
  back.** *Fixed 2026-11-06 (CHANGELOG): the mount refuses a second bay on one
  folder, and the synth commit takes `imageCommitMutex`.* `StorageCoordinator::mountBlockBytes` never applies the "one image,
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
  told was sent.** *Fixed 2026-11-06 (CHANGELOG): LAST_ACK until the tail
  drains; the deferred close raises DISCON.* `W5100Device.cpp:719-730` gates the graceful close on
  `SOCK_ESTABLISHED` only. `sendData` allows CLOSE_WAIT, so a short write parks
  the remainder in `pendingTx` *and raises SEND_OK*; the guest then issues
  DISCON, takes the `else` arm, and `clearSocket` discards the tail with the fd
  closed. The ordinary HTTP/1.0 and IRC shape the file's own comments describe.
  `kW5100SnSrLastAck` is already defined and unused.

### Then

- **DBG-1 — the resume amnesty is spent on an offer an interrupt preempts**,
  *fixed 2026-11-06 (CHANGELOG: retired in `onStepCompleted`)*, so
  Run re-stops at the same breakpoint having executed nothing; with a
  level-triggered IRQ still asserted, Run can never make progress. The fix is
  the mirror of the one the 2026-11-01 hunt applied to `stepSkipFirst_` —
  retire the flag in `onStepCompleted`, not on the offer — and it needs
  `debugger_banks_cases.h:70-72` updated, which asserts today's offer-based
  semantics while the real run loop always calls `onStepCompleted`.
- **DBG-2 — the OS trace decodes a call twice when its entry is offered
  twice** *Fixed 2026-11-06 (`OsCallTrace::reoffered`).* (a debugger stop at `$BF00`, or an interrupt taken there), and
  `abandon()`'s `<=` then closes the first copy with a bogus `-> (no return)`.
  `onZ80Instruction` and `monStep` already carry the guard; MLI, SmartPort and
  CP/M-65 were missed.
- **NET-2 — MACRAW/IPRAW frames are staged only when the guest reads
  `Sn_RX_RSR`.** *Fixed 2026-11-06 (`poll()` stages one frame per service while the rings have room).* `poll()` drives the receive side for host-socket modes only, so
  the mode-independent `Sn_IR` RECV the 2026-09-08 fix added still never fires
  on its own and an interrupt-shaped raw driver waits for ever. The documented
  intent is in `DEV.md` § Uthernet II; the register plumbing landed, the driving
  did not.
- **NET-3 — the LISTEN port-squat probe is a blocking `::connect()` on the CPU
  thread under `stateMutex`** *Fixed 2026-11-06 (non-blocking, 50 ms bound; no answer = taken).* (`W5100HostSockets.cpp:139-146`). Against a
  saturated loopback accept backlog the SYN is dropped rather than refused, so
  it blocks for the kernel's retry ladder — tens of seconds with the machine
  *and* the window frozen together. `connectBounded` is one include away.
  Needs the `uthernet_allow_loopback` opt-in.
- **MED-3 — `pom2_cpm` ignores the CP/M R/O attribute** *Fixed 2026-11-06.* and deletes or
  overwrites a file CP/M itself refuses to touch; `list()` already decodes the
  flag and `ls` prints it. The DOS 3.3 sibling honours its lock bit.
- **MED-4 — the ProDOS decode charges its 512 MB budget for files it then
  skips** *Fixed 2026-11-06 (the write budget is charged at the write; a separate 1 GiB read bound).*, so a volume of mostly-unchanged sparse files exhausts the budget,
  reports `filesUnsaved`, and fails the whole write-back permanently — naming a
  file that is fine.
- **SP-1 — `SmartPortHub::recalcActiveDevice` calls `setSony35(nullptr)`** *Fixed 2026-11-06 (`releaseSony35()` for an empty connector).*,
  which also clears the IWM's 5.25" `DiskImage` pointer — the exact call the
  surrounding comment forbids and `releaseSony35()` exists to avoid. Reachable
  on a stock //c+ (the external Sony drives are null by default). No
  guest-visible symptom could be constructed, because the //c-class authority
  split already withholds those accesses from the Disk II; report as hardening.
- **NET-4 — `LISTEN` from a non-INIT state tears the socket down** *Fixed 2026-11-06 (ignored outside SOCK_INIT; grade F, settled by a real card).* instead of
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
- **SES-3 — `noteTimeJump()` skips slot 0** *Fixed 2026-11-06.* while the rewind snapshot captures
  and restores `SLOT0`. No current slot-0 card overrides `onTimeJump`, so
  nothing is lost today; it is a one-token divergence from the capture loop.
- **CPU-2 — `M6502::softReset()`/`hardReset()` wipe the wire-OR IRQ mask**,
  *fixed 2026-11-06 (the mask is left to the devices; all 13 IRQ cards
  checked)*,
  which models nothing (RESET does not clear a 6502's IRQ *input*) and desyncs
  the CPU's view of the line from the one card that deliberately models a chip
  with no reset pin — the CCS 7710's 6850, the only IRQ-asserting card with no
  `onReset` override. Narrow: a rebooting guest usually re-inits the control
  register, which republishes.
- **CPU-3 — `FastChip::loadSnapshotState` restores `powerOnSpeed_`** *Fixed 2026-11-06.*, a host
  setting the sibling `ZipChip` deliberately refuses to restore for the same
  reason. The Machine menu then disagrees with `state.cfg` until the slider is
  touched again.
- **MED-5 — `ProDOSHostNameMap`'s registry keys on the raw folder string** *Fixed 2026-11-06.*,
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

1. **~33 sub-case skips exit 0.** *2026-11-06: the remaining quiet ones fixed
   — `floppy_sound_smoke`, `card_snapshot_contract`, six //c ROM loops; the
   rest already print a shape the detector sees.* With the detector widened (TQ-H1) the next
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
3. **`POM2_TEST_TIMEOUT_SCALE` does not reach inside the tests.** *Fixed
   2026-11-06 (`tests/TestTimeScale.h`, eight tests added to
   `POM2_WALLCLOCK_TESTS`).* CI's nightly
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

---

# Round 4 — seven more domains, and the method changed what came back

Seven domains no earlier round had touched: documentation drift, the video
post-processing, the slot bus, the pointing devices and clocks, the Workstation
Card + Z8530, the disk image FORMAT layer, and the beam/video timebase.

Mid-round the owner restated the project's method, and it is now standing
guidance (`[[feedback-silicon-is-reference]]`): **aim at the real silicon,
fetch the primary document from the net rather than recalling it — including
re-checking POM2's own `file:line` citations, since upstream moves — and
report what is NOT known as a deliverable.** Every brief was reissued with it,
and the difference in what came back is the most useful result of the round.

Primary sources actually fetched and quoted this round: MAME `apple2e.cpp`,
`a2bus.h/.cpp`, `a2ultraterm.cpp`, `a2videoterm.cpp`, `a2ssc.cpp`,
`a2thunderclock.cpp`, `grappler.cpp`, `ds1215.cpp`; OpenEmulator
`libemulation` `OEVector.cpp`, `OpenGLCanvas.cpp`, `AppleIIVideo.cpp`,
`CanvasInterface.h` and the *AppleColor Composite Monitor IIe* XML; AppleWin
`NTSC.cpp`, `MouseInterface.cpp`; the **WOZ 1, WOZ 2 and MOOF specs** and the
**2IMG spec**; the **Apple II Reference Manual (1979)** § EXPANSION ROM and
Table 33; **ProDOS 8 Technical Note #20**; the Csa2 FAQ R034 mouse
programming notes; cc65's `a2.stdmou.s`; applefritter threads on the PAL
crystal and clock.

## Landed

| # | Defect | Oracle that settled it | Pin |
|---|---|---|---|
| DOC-1 | **`README.md` told every downloader the packages contain firmware.** They contain none: the manifest says "Desktop packages carry NO firmware dump", its only `roms/` lines are `wasm` entries, `git ls-files roms/` outside `floppy_samples/` is empty, and `stage_data.sh --verify` actively *fails* a package that has any. An offline machine could not boot at all, and the README contradicted itself 30 lines later. | the manifest + the tree | — |
| DOC-2 | The **"Known problem in v1.0.0"** box was shipping into 1.1 saying "the regular packages, which ship every ROM" (they ship none) and "all **49** dumps" (the catalogue is **57**), with a forward-looking sentence about a fix that already landed. | `src/RomFetch.cpp` (57 entries), `POM2_RETROBIOS_COMMIT` | — |
| VID-1 | **The CRT scanlines were a half-row out of phase: the dark notch fell through the CENTRE of every scanline the Apple drew**, and the bright ridge straddled the gap between two rows. Single-row HGR detail and the horizontal strokes of glyphs were notched out while the empty interlines were lit. Invisible at exactly 2× (`fwidth(outRow) == 1` zeroes `scanAA`), which is why eyeballing never caught it. | OpenEmulator's own glass shader, fetched: `float scanline = sin(PI * textureSize.y * q.y); c *= mix(1.0, scanline*scanline, scanlineLevel);` — `sin²` is **1 at the row centre**. By the half-angle identity that is exactly `0.5 − 0.5·cos(π·outRow)` for POM2's coordinate; verified equal to 1e-12 at rows 0, 1, 5, 97. A sign. | needs a GL context (`crt_glass_resample` returns 77 without one) — recorded, not added |
| FMT-1 | **A single-sided 400K WOZ 2 3.5" image mounted as a scrambled 800K volume.** `singleSide` was derived from the MOOF rule (`moof && diskType == 1`), never true for a WOZ, so a 400K capture got 1600 blocks and `headCount() == 2` and `blockIndexFor` doubled every track's base. Track 0 is unaffected, so **the volume mounted with its real name and catalogue** while track 1 sector 0 landed at block 24 instead of 12. Silent read corruption on a healthy-looking image. | WOZ 2 spec, fetched: INFO **+1 Disk Type** is *"1 = 5.25, 2 = 3.5"* and nothing else; sidedness is **+37 Disk Sides** (INFO v2), *"A 3.5 disk can be 1 or 2."* | `woz35_load` (the fixture gained a `sides` knob) |
| BUS-1 | **Two bus-arbitration comments cited MAME for a rule MAME does not have.** `dmaClaimant()` claimed "the DMA daisy chain gives priority to the lowest slot; MAME a2bus does the same". MAME's `set_dma_line` is three lines that forward the signal with **no per-slot mask and no arbitration** — unlike `set_irq_line`, which does keep one — and the 1979 manual documents only "higher/lower priority" over pins 24/27 that are *"usually connected"* card-to-card. Both comments now own the convention and say what would settle it. | MAME `a2bus.cpp`; Apple II Reference Manual Table 33 | — (comment) |
| BUS-2 | The **`takesC800()` roster was wrong in two places**: `CLAUDE.md` omitted `VidexUltratermCard`, and `MemorySnapshot.cpp` — whose comment is the *justification for the snapshot byte* — said "Seven cards take it" and named seven when **eleven** card classes override it (twelve counting `Cffa3000Card`). The open UltraTerm question is settled in the card's favour. | MAME `a2ultraterm.cpp:135` `take_c800() → true`, serving 4 KB of firmware through the window; the 1979 manual's § EXPANSION ROM is written for exactly that class of card | — (a roster test is recorded as the right pin) |
| INP-1 | **The on-screen //e keyboard's CONTROL latch produced no control code for `@ ^ _ [ \ ]`**, and `Ctrl-@` ($00), `Ctrl-^` ($1E) and `Ctrl-_` ($1F) were **unreachable from the panel at all** — while the correct table, `pom2::keychord::ctrlPunctCode`, already sat one include away, cited to IIe Tech Ref Table 2-3. The panel is POM2's documented route when `keyboard_alt_apple_keys` is off or the host layout cannot form the chord, i.e. for exactly the users with no other way to type them. | POM2's own already-pinned table | the table is pinned by `key_chord_policy`; the panel branch now calls it |
| INP-2 | Both paste paths **reported truncation against the cap instead of the free room**. `Keyboard::pushChars` caps on the LIVE queue, so a second Ctrl-V while 4000 bytes were still draining dropped most of the clipboard and still said "N chars queued" with no note. The dropping is the documented DoS guard; only the message lied. | the code's own cap arithmetic | — |
| DOC-3 | `DEV.md` described the AppleWin **Tv sub-mode** as "+ 50 % blend with the previous frame's same scanline (`appleWinPrev80`)". It blends with the previous Apple scanline **of this frame**, at 3/4 + 1/4, and `appleWinPrev80` no longer exists. The code is the faithful one. | AppleWin `NTSC.cpp`'s `updateFramebufferTVDoubleScanline`, whose two-line mean is exactly 3/4 + 1/4 | — |

## Verified, deferred — round 4

Highest first. Full detail in the agents' reports; the short form:

- **`NoSlotClock` drops the DS1216's 12/24-hour mode and PM bits**, so a
  12-hour set-time of 9 PM (`$A9`) masks to `$29`, decodes as hour **29**, and
  `mktime` rolls the date forward a day — `offsetSec_` then wrong by 8 hours
  *and* a date. The read path never re-emits bit 7 either. MAME's `ds1215.cpp`
  (fetched) masks reg 3 with `0xBF` and does `(hour + 12) % 24` on the PM flag.
  Certain divergence; **unverified** whether any Apple II SmartWatch utility
  uses 12-hour mode.
- **The //c clamp screen holes POM2 reads conflict with Apple's documented
  convention**, and index 5 on a //c is the **built-in SmartPort's** screen-hole
  column, which the disk firmware also writes. Csa2 R034 says `$04F8/$05F8` is
  a *max*, not `minY`, and the `$0678/$0778/$06F8/$07F8` POM2 reads are not
  clamp holes in any document found. If the measurement behind the comment is
  stale, the //c absolute cursor sync projects onto garbage. **The single
  highest-value hardware capture in that domain** — see the unknowns below.
- **"Apple IIe SuperDrive" is offered on four profiles where the factory
  refuses it**, and the refusal is silent: `unavailableReason` has no case for
  it, `resolve()` does not drop it, `plugSmartPort35` discards the factory's
  warning, so the badge shows "1 staged change" for ever and every Apply
  re-arms a cold boot. Same shape as hunt #21.
- **A Ctrl-Reset can permanently disconnect a card's IRQ from the CPU.**
  `SlotBus::reset()` never re-publishes a card's contribution while
  `M6502::softReset/hardReset` zero the wire-OR mask, and `assertIrq`'s
  idempotence makes the desync permanent. The CCS 7710's 6850 — which has no
  reset pin, deliberately — is the documented counter-example to the invariant
  `M6502.cpp`'s own comment asserts. MAME's `reset_w`, read in full, never
  touches its IRQ mask. (Same root cause as round 2's CPU-2.)
- **The ESC/P `ESC D NUL`, Diablo bottom-margin, WOZ `boot_sector_format`,
  FLUX `block_count` bound and TMAP 0..159 range** findings — four more
  format-layer items, each with a fetched spec line, including one where a
  crafted TMAP entry of 200 reads 8 bytes of *bit data* as a TRK header.
- **Four live demodulation controls are greyed out when "CRT effects" is off**
  — including **PAL composite**, which has no other UI anywhere, so a PAL user
  who wants clean pixels cannot tick the one control that describes their
  machine rather than a look.
- **The Triad/Dot mask's vertical gap is neither anti-aliased nor faded** where
  every other high-frequency pattern in that shader is; **barrel and vignette
  are elliptical** where OE's are circular (OE divides the vertical axis by the
  display aspect) — the second changes the shipped look and wants a decision.
- **The OE demod kernel lives in four hand-typed copies** and the GLSL one is
  checked only by a hand transcription in a test; a typo inside the shader
  string ships with all 425 cases green.
- **`SlotBus`'s active-card-cache staleness guard never executes in any
  configured build** (`#ifndef NDEBUG`, and every documented build is Release
  or RelWithDebInfo), so the regression it exists to make loud would ship.
- `ClockCard`'s free-running DATA_OUT pulse phase is not in the snapshot;
  `IIcMouse`'s `releasedFor_` is not either; `~SlotBus` is `= default`, the one
  teardown path that skips `onUnplug()`; window focus loss releases held keys
  but never a held mouse button; `Sony35Drive::ensureCacheFor` omits
  `Woz35` from a three-kind list two siblings include; the MacBinary envelope
  is stripped only by the 5.25" loader; `lle_vs_hle.md` calls four firmware
  dumps "in-repo" when the repo tracks none; the LLE/HLE panel has six rows the
  Markdown master table does not; the ProDOS block-device signature is cited to
  a manual that predates ProDOS by four years (it is **ProDOS 8 TN #20**).

## What we do not know — the round's real deliverable

### Measured, and the evidence favours POM2 over its own cited oracle

**The European PAL CPU clock.** Four candidates, and POM2's was the least
sourced while being the best founded. Recorded in `CpuClock.h` with the
numbers: POM2 1 015 625 Hz (the PAL line rate, 15 625 × 65, exact); MAME
1 016 966 Hz **under a `// FIXME: some of these values are probably wrong`
that sits directly above it** (verified on master — and its NTSC sibling there
is 1 021 800, not the 1 022 727 everyone uses, a second reason not to treat
that block as an oracle); crystal ÷ 14 = 1 017 889 Hz; and **~1 015 300 Hz
measured on a real PAL //e** ("1.0153 MHz on a good 12 digit counter using a
100 ms gating time", applefritter). Distances from the measurement: POM2
+0.032 %, MAME +0.164 %, crystal/14 +0.255 %. So POM2 is **5× closer** than
the figure it deferred to, and the measurement also says the PAL board does
*not* simply divide its crystal by 14. **Still unknown:** that is one hobbyist
reading of one machine, and no source for the PAL divider chain was found.
*Settled by:* a frequency counter on φ0 of two different European //e boards,
or the schematic. Until then, expect ~0.13 % drift when diffing a DIX capture
against `apple2eefr` — **and do not move the constant on the strength of that
diff.**

### Derived vs taste, in the video pipeline

**Derived, and now proven:** the 17-tap `lumaK` / `chromaSoft` kernels. The
agent fetched OE's `OEVector.cpp`, re-implemented
`chebyshev(17,50) → normalise → × lanczos(17,fc) → normalise` in Python at
fs = 4·315/88 MHz, and reproduced POM2's hard-coded coefficients **to all five
printed decimals**. The monitor XML confirms Y'UV at 2.0 MHz luma / 0.6 MHz
chroma, so a single chroma kernel for U and V is right. The YUV→RGB matrix is
verbatim OE. The NTSC master-clock figures are exact.

**Taste, and now written down as such:** `chromaSharp` (= 2 × luma, no hardware
analogue — and the lower half of that slider is a documented no-op while the
tooltip claims "more composite bleed"); `persistence = 0.4` (OE's Apple II XML
says persistence **0**); `phosphorGamma` (POM2-only, OE runs the glass in
gamma space with no curve); `scanlines`, mask strength, barrel, centre
lighting (OE's XML is 0.05/0.05/0.05/1/1, and POM2's barrel *number* is on a
different scale from OE's because the radius normalisation differs); the mask's
0.5/1.5 and `kMaskUnitsX = 1120` (a convention, where a real tube's pitch is a
millimetre figure); `rgbBandwidthMHz` for the Péritel cable (a brickwall where
the real cable is a 1st-order RC).

**And the AppleWin IIR path is tuned by its own authors with provenance
unknown** — `NTSC.cpp` itself carries *"Should this be 7.15909 MHz ?"* and
*"What filter is this ??"*. POM2 ports it faithfully; nobody knows what it
models. *Settled by:* a frequency sweep of the real filters, or the mkfilter
parameters AppleWin's author used.

**The biggest gap: PAL composite is a look, not a model.** POM2 generates the
signal at 4·fsc(NTSC) = 14.31818 MHz *always* and demodulates at fs/4, then
flips the V tap on odd lines. That flip *is* verbatim OE, but it is only
meaningful on a 4×fsc grid — and PAL fsc is 4.43361875 MHz, so fsc/fs ≈ 0.311,
not 0.25. European machines' composite colour was genuinely marginal for this
reason. *Settled by:* a composite capture of a French //e or //c into a
Péritel-era monitor, plus the PAL encoder schematic.

### Silicon vs contract, in the disk formats

**The real magnetic surface:** WOZ 1/2 (5.25") keeps all 160 quarter-track
slots, scales cells by `optimal_bit_timing`, models weak bits on MAME's
amplifier rule deterministically in (revolution, zone, interval), and **fully
honours the WOZ 2.1 FLUX chunk** — 125 ns tick positions per quarter-track,
read, spliced on write, and re-emitted as a delta stream including growing the
TRKS allocation. MOOF / WOZ 2 (3.5") likewise keeps cells at 125 ns.

**An idealisation, and here is the cost.** `.dsk` / `.do` / `.po` / `.d13` /
2IMG-sector images are **synthesised** from sectors at mount — a fixed 14-byte
sync gap, `D5 AA 96`, `D5 AA AD`, `DE AA EB`, 6112 of 6656 nibbles. Nothing of
the original surface survives, so these containers cannot represent *at all*:
non-standard prologues/epilogues, altered volume/track/sector/checksum bytes,
nibble-count timing (spiral/track-arc), half- and quarter-tracks, weak or
unformatted-sector checks, synchronised tracks, mixed 13/16-sector disks, or
any sector count but 13 and 16. **And the reconstruction's one constant is
fitted to POM2's own encoder, not to hardware**: `kSyncMinRun = 5` decides
which `$FF` run is self-sync, so it is wrong in both directions by
construction. *Settled by:* an Applesauce flux capture of a disk written by
real DOS 3.3 RWTS and one by real ProDOS, re-deriving the threshold instead of
fitting it.

Also written down: the 3.5" GCR is decoded to blocks **at load**, and the 12
tag bytes of every 524-byte sector are decoded and **discarded**, so a
MOOF→`.po` export loses them irrecoverably (unknown whether any Apple II-side
software reads them); 1.44 MB MFM is sector-level and cannot be formatted from
inside the emulator; `.nib`/CNib2 infer sync position and CNib2 has a
272-nibble per-track write black hole POM2 refuses rather than loses.

**Does POM2's write path produce flux a real Disk II would read back?** The
repo's claim is narrower than yes, and the honest answer is now recorded: the
whole 5.25" timeline runs **~2.2 % fast**, deliberately —
`lssCyclesPerCell()` is an integer, so a 4 µs cell is laid at 8 LSS cycles
where MAME's 2 043 600 Hz makes it 8.17; a revolution is 195.6 ms instead of
200. Any protection measuring absolute rotational time rather than relative
byte counts reads ~2 % off. And POM2 **ignores the WRIT chunk entirely** while
mutating BITS — WRIT is exactly the chunk that tells an imager how to lay a
track on physical media, including the BITS checksum that says whether BITS
changed since WRIT was made. *Settled by:* writing a POM2-modified WOZ back to
a floppy with an Applesauce and booting it on real hardware.

### Silicon vs contract, in the input devices

`MouseCard` + `M68705P3` + `MC6821` is the card: the real mask ROM executed at
510 900 machine cycles/s. `IIcMouse` is the IOU circuit at the latch/edge
level, with one deliberate divergence (back-pressure, because a host pointer
moves faster than any physical mouse and MAME's unconditional emission loses
edges). `ClockCard` and `NoSlotClock` are bit-level.

`MouseCardAppleWin` is an HLE **of the 68705's firmware**, not of the 68705,
and what that cannot reproduce is now written down: the timing of a
command/reply (POM2 answers in zero emulated time, so firmware that races the
handshake sees the wrong answer); the undocumented commands and MCU RAM (`$F0`
answers the eight clamp bytes and **0 for every other MCU address**);
quadrature at all (no X0/X1/Y0/Y1, no sub-unit motion, no direction-bit phase);
the VBL phase, which POM2 *reconstructs* from the cycle counter and, on a //c,
**patches three ROM bytes** to skip INITMOUSE's `$C019` calibration loop
entirely; the clamp-window edge case where `setClampX/Y` clamps the live
position though Csa2 R034 says the real //c and AppleMouse card do not; and the
PIA-side interrupts, wired in MAME and dead here. `MouseSync`'s absolute closed
loop is not hardware at all.

### Documents nobody verifying this code has read

**Apple II Mouse Technical Notes #2, #3 and #7.** Every mirror tried returned
404, 402 or an expired certificate. So POM2's citations to them — the default
60 Hz rate on a European machine, TIMEDATA latching only at the next INIT,
TN #3's "the position retains its last values", TN #7's `$47-$4E` clamp map —
rest on code comments, cc65's comments and developer slides. *Settled by:* a
readable copy (the Asimov `technotes/mouse/` tree, or the Technical Notes CD).

~~Also unreachable: the **uPD1990AC** datasheet ... the **DS1216/DS1216E**
datasheet ...~~ **Both were found on 2026-11-01** — see *Round 5* below. What
is still unread: **Sather** in full text (the scan has no text layer; the two
pages that mattered were rendered and read), and the **Apple IIe Technical
Reference Table 2-3** that `ctrlPunctCode` is cited to — if that table actually
requires Shift for `@`/`^`/`_`, POM2 emits control codes on three unshifted
keys it should not.

### Bus questions the documents cannot settle

Whether **/RESET clears a card's own `$C800` enable flip-flop** is *per-card*:
the 1979 manual's Figure 9 shows the `$CFxx` decode driving the flip-flop's
reset input, not bus pin 31, so a 74LS74 with /RESET unconnected keeps it
(POM2's model, which matches MAME exactly) and one with it wired drops it.
Whether the window is claimed by /IOSEL or /DEVSEL is **self-contradictory in
the primary source**: § EXPANSION ROM says the flip-flop "should be turned on
by the DEVICE SELECT signal (the one which enables the 256-byte PROM)" — but
Table 33 defines DEVICE SELECT as pin 41 (`$C0nX`) while the signal that
enables the PROM is I/O SELECT, pin 1 (`$CnXX`). And two cards both touched is
electrically indeterminate on real hardware; MAME's own comment calls
first-one-wins a modelling choice, which POM2 copies and says so.

### Citations that had rotted

POM2 cites MAME by line number with **no MAME version pinned anywhere in the
tree**. Most citations verified exact this round, but two had moved:
`a2bus.h`'s `take_c800()` is at ~142 on master, not the 145 `CLAUDE.md` gives,
and `ow65c02.lst`'s `reset_c` is at ~886, not 814. Both *claims* are true —
only the coordinates rotted. The convention should cite the **symbol** and pin
one MAME tag.


# Round 5 — the three documents the method said to go and get

Round 4 closed with a list of documents "nobody verifying this code has read".
Three of them were hunted down the same day, with `scrapling` walking directory
indexes rather than guessing file URLs. All three are on **bitsavers** (mirror
`ftp.mirrorservice.org/sites/www.bitsavers.org`), none of them where a search
engine points:

| Document | Where it actually is | Pages that matter |
|---|---|---|
| **Diablo 630 / 630ECS API Interface Manual**, 90440-00A Rev A (12/82) | `pdf/diablo/printer/630/` | § 3.5 command summary (3-12/3-13), § 3.9.4, § 3.13, § 3.24 |
| **Diablo 630 / 630ECS Maintenance Manual**, 90443-02A (3/83) | same directory | reference, not yet mined |
| **DS1216E SmartWatch ROM 64/256K** data sheet | inside `components/dallasSemiconductor/_dataBooks/1992_Dallas_Semiconductor_Product_Data_Book.pdf` | **pp. 5-22..5-25** |
| **uPD1990AC Serial I/O Calendar & Clock CMOS LSI** data sheet | inside `components/nec/_dataBooks/1983_NEC_Integrated_Circuits_for_Consumer_Use.pdf` | **pp. 792-805** (+ app. note p. 1106) |

Three dead ends worth recording so the next search skips them: Dallas lives
under `components/dallasSemiconductor/`, **not** `components/dallas/`, and has
no per-part datasheet directory at all — only the three data books. NEC's
`components/nec/_dataSheets/` has twenty-odd parts and **not** the 1990A; the
clock/calendar LSIs are filed as *consumer* ICs, so neither the Microcomputer
Catalogs (1982, 1984) nor the Intelligent Peripheral Devices book contain it.
And analog.com / maximintegrated.com no longer serve the DS1216 datasheet at
any of the four obvious URL shapes (404/522); the data book is the way in.

## Landed — round 5

**The Diablo 630's hours of `ESC S` and `ESC X` were dropped on the floor**
(`src/ImageWriter.cpp`). § 3.9.4.1: *"While the terminal is in remote HMI mode,
the SPACING switch is ignored. An ESC S sequence will return control of HMI to
the SPACING switch."* POM2 dropped `ESC S`, so a driver that tightened the HMI
for one passage printed **the whole rest of the job** at that HMI. And
§ 3.24.11 lists what `ESC X` cancels — Auto Underscore, Bold Overprint, Shadow
Print, Program Mode, Offset Selection, Auto Justify, Auto Center — closing with
*"Increased carriage settling time and proportional space mode are **not**
cancelled by ESC X"*, which is the one thing a naive implementation gets wrong.
Both decoded now, proportional space deliberately spared. Pinned
`imagewriter_escape` → `testDiabloEscSandX`, with two mutation controls (each
`case` neutered separately, each kills the test) and a non-degenerate HMI: the
first draft used 12/120" = 0.1", which **is** this head's own 10 cpi cell, so
it passed with `ESC S` neutered.

**The Diablo 630's remote reset did nothing at all.** § 3.23.1: *"This command
[`ESC SUB I`] will cause the Model 630 to unconditionally execute an initialize
sequence regardless of any error conditions... unlike the corresponding remote
reset sequence `ESC CR P` which is queued along with other commands. The Model
630 will default to the same conditions that exist at power-up."* And § 3.9.4:
`ESC CR P` *"may also be used here to cancel all optional format factors and
return to the format selected by the SPACING switch."* POM2 consumed the
parameter byte of both and threw it away, so a driver that opened its job with
a remote reset — the normal way to clear whatever the last job left behind —
**inherited the previous job's HMI, VMI, tab racks and styles**. POM2's input
stream *is* the queue, so the immediate/queued distinction has no analogue
here and both now land on `resetPrinter()`, the power-up path `ESC c` takes on
the Epson head. `ESC SUB`'s other sub-commands stay untouched and the reason is
in the source: `R` resets error conditions POM2 has none of, `SO` is a memory
test, and `1` / `3` request a status byte back over the serial line, for which
this head has no return path. Pinned `imagewriter_escape` →
`testDiabloRemoteReset`, both spellings, measured against an undisturbed head
so the test never has to predict where `resetPrinter()` leaves the paper.

**The DS1216E's hours register carries the 12/24-hour mode, and POM2 called
bit 7 reserved** (`src/NoSlotClock.cpp`). Data sheet, § AM-PM/12-24 MODE:
*"Bit 7 of the hours register is defined as the 12- or 24-hour mode select bit.
When high, the 12-hour mode is selected. In the 12-hour mode, bit 5 is the
AM/PM bit with logic high being PM. In the 24-hour mode, bit 5 is the second
10-hour bit (20-23 hours)."* POM2 masked the register with `0x3F` and decoded
BCD. In 24-hour mode that is right; in 12-hour mode it dropped the mode bit and
then read the AM/PM bit as the 20-hour bit — a `SET TIME` of **3 PM (0xA3)
landed as 23:00**, eight hours out, and only ever for PM. Now decoded per the
section, including the two wraps the mode needs (12 AM is midnight, 12 PM is
noon). Pinned `no_slot_clock_smoke` → `testTwelveHourMode`, seven cases, two of them
24-hour positive controls; it was watched failing on exactly the 3 PM case
before the fix.

## Confirmed, not changed — round 5

The documents also settled a pile of things POM2 had right on someone else's
authority. Recorded because "verified against the silicon's own data sheet" is
a different claim from "matches AppleWin":

**DS1216E.** The 64-bit comparison pattern is `C5 3A A3 5C C5 3A A3 5C` sent
*"LSB to MSB"* (Figure 1, p. 5-24 — and the **prose NOTE under that figure
drops a byte**, listing only seven; the bit table is the authority, which is
now said in `NoSlotClock.h` so nobody "fixes" the constant from the sentence).
A2 low = write with data on A0, A2 high = read on DQ0. *"Prior to executing the
first of 64 write cycles, a read cycle should be executed"* — the pointer
reset POM2 does. *"If a match is not found, the pointer does not advance and
all subsequent write cycles are ignored"* — the sticky disable. *"If a read
cycle occurs at any time during pattern recognition, the present sequence is
aborted and the comparison register pointer is reset"* — the way out, which
round 4 had down as "AppleWin's word". The eight registers are 0.01 s /
seconds / minutes / hour / day / date / month / year, BCD, stepped *"starting
with bit 0 of register 0 and ending with bit 7 of register 7"*. All as POM2
had them.

**uPD1990AC.** *"Binary Coded Decimal (except 'Month' which is Hexa-Decimal
Code)"* — POM2's month-as-4-bit-binary in the high nibble, which round 4 had
flagged as resting on a POM2-internal inference from the ThunderClock's own
firmware, is NEC's own specification. Fig. 1 is the register map and says
**"40 Bit Shift Register"** in so many words, with output *"from LSB of
Second"*; two comment lines in `ClockCard.cpp` said 48 and are corrected (the
code always used 40). The command table on p. 796 matches POM2's mode decode
and MAME's enum exactly — `000` Register Hold / DATA OUT = 1 Hz / TEST
released, `001` Shift / DATA OUT = LSB, `010` Time Set & Counter Hold / LSB,
`011` Time Read / **0.5 Hz**, `100`/`101`/`110` TP = 64 / 256 / 2048 Hz each
releasing TEST, `111` TEST — and it states the group rule POM2 models:
*"Groups '0' and '1' hold their functions independently"*, so a group-0 command
cannot move the TP rate. There is no 4096 Hz mode, confirming POM2's note that
`MODE_TP_4096HZ` is uPD4990A-only.

**Diablo 630.** Every parameter count POM2 had guessed is in § 3.5 and § 3.9.4:
HMI = (n − 1) × 1/120" via `ESC US`, VMI = (n − 1) × 1/48" via `ESC RS` (so
round 2's swap fix was right, now on the primary source rather than on a
recollection of it); `ESC FF n` sets lines per page to **n itself**, 1..126;
`ESC HT n` → *"Horizontal Position = (ASCII character − 1) × HMI"*, leftmost
position = 1; `ESC VT n` → *"Vertical Position = (ASCII character − 1) × VMI"*,
top line = 1; `ESC L` and `ESC T` take **no** parameter. A comment block in
`ImageWriter.cpp` claiming `ESC FF n` and `ESC L` were left undecoded *because*
their parameter counts were uncertain was doubly wrong — the counts are
documented and the code decoded both anyway — and is replaced by the citations.
`ESC O` for bold and `ESC &` to clear it are § 3.24.4's (the summary table's
"ESC a" and "ESC de" are OCR damage; the prose section is clean).

## What we still do not know — round 5

**The DS1216E's day-register control bits, deliberately not modelled.** Bits 4
and 5 of the day register are the RESET-pin enable and the oscillator control
— *"when set to logic 1, the oscillator is turned off"* — and the data sheet
says both **ship as 1**. POM2 reads them back as 0, which is the state of a
running clock whose RESET pin is live, and ignores them on a write, so a guest
cannot stop POM2's clock the way it could stop the chip's. Honouring the
oscillator bit would mean any driver that writes the day register with bit 5
set freezes the user's clock, and no Apple II NSC driver POM2 has read does
that deliberately. *Settled by:* a disassembly of one shipped `NS.CLOCK`
set-time driver showing what it puts in register 4.

**The uPD1990AC's power-on TP rate.** NEC documents no default for group 1;
POM2 takes MAME's. *Settled by:* a scope on TP of a cold ThunderClock+ before
the firmware's first `STB`.

**The uPD1990AC's leap year, where POM2 is deliberately better than the
chip.** *"The correction for length of the month is automatically performed and
leap year is not automatically performed. But it is possible to set calendar
FEB. 29, and the day after, it will be Mar. 1."* POM2 synthesises the nibbles
from the host clock on every read, so its calendar is Gregorian: a real
ThunderClock+ left running across 29 February in a non-leap year rolls to
1 March and POM2 does not. Modelling that would mean carrying the chip's own
counter instead of the host's, which is the one thing that makes POM2's clock
survive a rewind. Stated in `ClockCard.cpp` so it is not mistaken for fidelity.

**Diablo `ESC W` (shadow print) and `ESC % / ESC N` (carriage settling
time).** § 3.24.5 and § 3.24.6 describe both precisely — shadow is a second
strike 1/120" over, settling time goes to 20 ms — and POM2 models neither,
because its page model has no shadow bit and no settling time to spend.
Dropped with their `ESC`, which is what the LaserWriter's subset did with
commands it did not carry, and now said out loud instead of hidden in "only
the unambiguous commands are decoded".


# Round 6 — Sather, read at last

Round 4 listed *"Sather in full text"* among the documents nobody verifying
this code had read: the scan found then was a 136 MB image PDF with no text
layer, and only two pages were rendered, for the `$18` HBL offset. Round 6
went back with `tesseract` available and found the better copy:
**archive.org item `utaii`** carries *Understanding the Apple II* (Jim Sather,
Quality Software, 1983) as a 35 MB PDF **and a 1 MB `_djvu.txt` OCR layer** —
searchable, and the figures rendered from the PDF when OCR mangles a table.

**And the IIe book too, on the owner's instruction** (the 1985 original has
been out of print for forty years; A.P.P.L.E.'s 2024 remaster is a different
edition). archive.org item **`Understanding_the_Apple_IIe`** is open and
carries a 1.46 MB OCR layer *and* a 29 MB searchable `_text.pdf`; the Asimov
mirror has the scan too. Sather never wrote a **IIc** book — the //c oracle is
Apple's own *Apple IIc Technical Reference*, fetched as archive.org
`AppleIIcTechnicalReference2ndEd`.

**A method note the owner supplied mid-round, and it changed an outcome.**
This book's OCR is visibly noisy — "lOU" for "IOU", "ail" for "all",
"occur.s." — and a behaviour change was about to rest on one OCR'd sentence.
The page was rendered and read instead. The sentence survived verbatim, but
the page number did not (7-3, not the 7-6 inferred from the text offset), and
the rendered page carried Sather's own hedge, which the OCR run had buried.

## Landed — round 6

**The floating bus had no address pin at all, and now has a printed one.**
`Memory::floatingBus` is a verbatim port of MAME's `apple2video.cpp`
`scanner_address` — a port of a port — and the only test on it
(`floatingbus_page2_smoke`) checked the PAGE2 / 80STORE gating, not a single
address. Yet the addresses are the whole point: every vapor lock and every
floating-bus RNG depends on them.

Sather prints the answer twice. **Figure 5.6, p. 5-11** is the TEXT/LORES
scanning map — HBL base and display base for all 32 character rows, pages 1
and 2, in hex *and* decimal. **Figure 5.9, p. 5-45** is the HIRES map, per
scanline. POM2 reproduces **every value in both** (64 + 128 checked), plus the
five rules p. 5-11 states above its table and the figure's footnote:

| | Sather | POM2 |
|---|---|---|
| 1 | HBL is A12 in TEXT/LORES, so HBL and HBL' memory are disjoint — *"this is not true in HIRES mode"* | ✅ and Figure 5.9 shows the HIRES overlap: HBL base `$2068`, display base `$2000`, same page |
| 2 | HBL memory begins **`$18` before** display memory **plus `$1000`**, 128-byte wraparound | ✅ (the `& 0x0F` on the offset sum *is* the wraparound) |
| 3 | *"The first address of HBL is always addressed twice consecutively, because H0-H5 is in the all zero state for two consecutive scans"* | ✅ `h_clock - (h_clock > 0)` — and the table's own column header draws it: `00123456789ABCDEF01234567` |
| 4 | In VBL the offset sum is H5-H4-H3 **minus 0100**, against H4-H3-H2 **minus 0011** at screen top: *"$400 minus $8 gives $478; not $3F8"* | ✅ exactly, from `addend2` going to `0b1111` when V3 and V4 are both set |
| 5 | Wraps at the 128-byte boundary: before `$400` is `$47F` in VBL, **`$147F`** outside | ✅ both |
| footnote | 32 rows a frame, each scanned 8 times except the last, scanned **14** (24×8 + 7×8 + 14 = 262) | ✅ |

Pinned `video_scanner_sather` — which recovers the scanner's address by
walking a marker through RAM rather than reimplementing the formula it is
checking — with four mutation controls (the `0x0D` offset constant, the HBL
A12 bit, the two-zero-states rule, and the HIRES `v_A` bit; each kills it).

## An erratum in the primary source

Rule 2's printed Applesoft sequence ends `30 HBL = HBL+1024`. It must be
**+4096**. The prose one line above says "plus `$1000`", and Sather's own
table settles it against his program: row 0 has BASE `$400` and HBL `$1468`;
`$400 - 24` wrapped by step 20 is `$468`, and `$468 + 1024` is `$868` — text
page 2, which is not what the table prints, while `$468 + 4096` is `$1468`,
which is. POM2 and MAME set A12 and are right. Recorded in the test, in
`Memory.cpp` and in DEV so nobody "corrects" the code toward the program.

## Confirmed, not changed — round 6

**The long cycle, which POM2 had derived for itself.** `CpuClock.h`'s
912-periods-per-line argument — the one the PAL clock rests on — was POM2's
own reasoning. It is now Sather's, in two independent places. Ch. 3: *"The
normal 6502 machine cycle lasts 14 PERIODS or about .978 microseconds. The
long cycle lasts 16 PERIODS or about 1.12 microseconds. There are three
frequencies involved here: the primary frequency at which the 6502 is operated
for 64 out of 65 cycles, 1.0227 MHz; the secondary frequency at which the 6502
operates for 1 out of 65 cycles, .8949 MHz; and the composite frequency which
actually is the number of machine cycles per second, **1.0205 MHz**."* And
ch. 8 from the other side: *"560 out of 912 14M periods are displayed in a
horizontal cycle (remember the long cycle)"*.

**And why POM2 does not need to model the stretch.** Ch. 3 gives the long
cycle its purpose: there are 3.5 COLOR REFERENCE cycles in a scanner cycle, so
the burst phase alternates 180° every cycle — *"a seven dot HIRES pattern
represents different colors when it is stored in an even RAM address than when
it is stored in an odd RAM address"* — and with 65 cycles to a line, an odd
number, the START phase would alternate every line too. *"By delaying video
shift timing half a color reference period once every horizontal line, the
same beginning phase relationship occurs every horizontal line."* POM2's
renderer restarts each line at the same phase, which **is** that effect; and a
6502 counting cycles sees 65 either way, the long cycle being one cycle, just a
longer one.

**The paddle 558's no-retrigger rule**, which POM2 models on MAME's authority,
is Sather's too: *"the timer pulse may still be high from the previous PREAD
routine which read Timer 0, and the timers are not retriggered by C07X if they
have not yet reset from the previous trigger."* He also notes the 558's RESET
input *"is not utilized in the Apple"* and that all four triggers hang off the
single `$C07X` line — which is why POM2 re-arms all four axes, not the
addressed one.

## Acted on, and what is still open — round 6

**POM2's NTSC and PAL clock constants are not the same KIND of number, and
now both are sourced.** NTSC ships 1 022 727 (= 14.31818/14, Sather's
*primary*, the frequency for 64 of every 65 cycles); PAL ships 1 015 625, the
long-cycle **average**. Sather's composite — *"which actually is the number of
machine cycles per second"* — is 1.0205 MHz, i.e. 1 020 484 Hz. So POM2's NTSC
cycles→seconds conversions (audio queues, SSI263 phoneme lengths, the
cassette) run **0.22 % fast** against hardware where its PAL ones do not, and
its NTSC wall clock is 60 Hz where the machine is 59.92. **Not changed**: the
error is inaudible, the constant is in `CLAUDE.md` and pinned by a dozen tests,
and the emulated BEAM is unaffected — `advanceCyclesVideo` and `floatingBus`
take the scanline from 65 × 262 = 17 030 cycles, not from this constant.
*Settled by:* deciding whether POM2 wants the primary or the composite
frequency, and making both standards agree.

**The display was the wrong shape in all three modes, so there is now a
fourth** *(added on the owner's instruction)*. § ASPECT RATIO IN THE APPLE
DISPLAY: NTSC displays 53.6 µs of a 63.6 µs line (84.3 %) and 242.5 of 262.5
lines (92.4 %), so a 4:3 raster gives HD/VD = 1.46; the Apple displays 560 of
912 periods (61.4 %) and 192 of 262 lines (73.3 %), so *"the display window is
**1.225** times as wide as it is high"* and its pixels are ~0.84 as wide as
tall. `Crt43` over-stretches by ~8.8 %, `Square` by ~19 %.

`AspectMode::Measured` joins them — View ▸ Display ▸ Aspect ratio, the command
palette, and `aspect_mode=measured` in `state.cfg`. **Not** the default: 4:3 is
what every other emulator and every comparison screenshot uses, so the shape
stays an explicit choice. It follows the **video standard** rather than being a
constant, and the menu label carries the live number, because the same
derivation on PAL gives **~1.507** — a PAL Apple puts 192 lines in a 312-line
field, so its window is *wider* than square pixels where an NTSC one is much
narrower. That is why European machines showed those deep top and bottom
borders. **The PAL figure is POM2's own application of Sather's method, not
his** — he wrote about an NTSC machine — and `measuredWindowAspect` states the
PAL raster numbers it used (52 µs of 64, ~287.5 of 312.5 lines) so the
derivation can be checked. *Settled by:* a ruler on a Péritel-era monitor
showing a known-square test pattern.

**The paddle pulse had no floor, which Sather's formula gives it** *(fixed on
the owner's instruction)*. `(RP + 100) × .022` µs: the `+ 100` is a series
term, so the network emits a ~2.2 µs pulse **even with the pot at its stop**.
POM2 and MAME (`apple2e.cpp:1007`, 10 800 ns a count, no offset) both dropped
it, so a paddle at 0 produced a zero-length pulse. PREAD cannot tell — its
first sample is 10 cycles after the strobe — which is why it went unnoticed,
and which is also what makes the fix safe. `pulseCycles()` is now
`value * 11 + 2`, pinned by `input_io_smoke` → `testPaddlePulseFloor`, with
mutation controls at 0 and at 12 (the latter would push PDL(n) off by one).

**What was NOT done, and why, because it is the more obvious reading of the
same formula.** A 150 kΩ pot sweeps to **3300 µs**, i.e. 3300/10.8 ≈ **306**
PREAD counts, where POM2's axis tops out at 255. Mapping the 0..255 host axis
onto the full electrical sweep would reproduce the hardware exactly — and
would put the stick **centre at 154 counts instead of 128** and leave the top
~17 % of travel all reading 255, because PREAD saturates (`INY` wraps, `BNE`
falls through, `DEY` leaves $FF). That is faithful to an *unadjusted* paddle.
Apple's own setup instructions, every game, and MAME all assume a controller
trimmed so full travel is 0..255, so POM2's axis stays the adjusted-controller
count — now said in `PaddleInputs.h` with the 306 written down as a constant
rather than left as a claim in a comment.

## Two more findings, from the IIe book (fetched later in the round)

**The //e's missing HBL phantom row was MAME's word; now it is Sather's,
twice over.** p. **5-10**, § TEXT/LORES Scanning: *"In TEXT/LORES, A15, A14,
A13, and A12 equivalents are false (low, ground, zip). The A10 equivalent is
(80STORE' . PAGE2)', and the A11 equivalent is 80STORE' . PAGE2."* No A12
term — and that A10/A11 pair is exactly POM2's `Page2 = page2 && !80store`
base select, which the table at the foot of the same page spells out.

The second confirmation is the better one: **the two editions differ exactly
as the hardware does.** The 1983 book's rule 1 is *"HBL scanned memory begins
$18 bytes before display scanned memory **plus $1000**"* with a THREE-step
Applesoft sequence; the 1985 //e book's is *"HBL scanned memory begins $18
bytes before display scanned memory"* with a **two**-step one — `30 HBL =
HBL+1024` is simply absent, because a //e has no A12 image to add. Not a
defect in POM2, but the test could stop refusing to assert it, and does.

**A bonus from that page: the canonical vapor lock, in Sather's own code.**
*"If you zero out all scanned memory except for the bytes in the blanking
period preceding a given horizontal display line, you can detect the beginning
of that horizontal scan with the following loop"* — `PAGE1 EQU $C054` /
`WAIT LDA PAGE1` / `BPL WAIT`. That is the shape `vapor_lock` and DROL's
`LDA $C050 / CMP #$80` both take, and it is the primary-source justification
for DEV § Soft switches' rule that an undriven `$C0xx` read must return the
scanner byte.

**A second, minor erratum, caught by having both editions.** The 1983 book's
rule 4 reads *"H5-H4-H3 minus 0100. This is almost the same as the top of the
displayed screen (**H4-H3-H2** minus 0011)"*; the //e book's same sentence
reads **H5-H4-H3** in both halves. The //e wording is the self-consistent one
and the one that matches the code (`addend1 = (h_5<<2)|(h_4<<1)|h_3`), so the
1983 `H4-H3-H2` is a typo. Recorded, not acted on — POM2 already agrees with
the arithmetic either way.

**A //e RESET clears the DHGR (AN3) latch, and POM2 kept it.** This one *is* a
defect, and the citation was the giveaway: `resetSoftSwitches(warm)` preserved
`display.dhgr` citing MAME `apple2e.cpp:1459-1471` **and** Sather Fig. 7.1,
and both say the opposite. Sather p. 7-3, note 1 to Figure 7.1 — read off a
**render** of the page, per the owner's warning about this book's OCR:
*"All soft switches except KEYSTROBE, TEXT, and MIXED are reset when the
RESET' line drops low. All soft switches appear to be cleared at power up."*
Three names; AN3, one of the IOU's four annunciator soft switches and what
drives DHGR on a //e, is not among them. MAME agrees with its own citation —
`reset_w` carries the comment *"Certain IOU switches off (80STORE, 80COL,
ALTCHR, PAGE2, HIRES, AN0, AN1, AN2, AN3) // Sather, Fig 7.1"* and then does
`m_an0 = m_an1 = m_an2 = m_an3 = false;`. So Ctrl-Reset out of a double-hi-res
program left POM2 in DHGR where the hardware drops to plain HGR, shredding the
first frame after the reset. Fixed, pinned by `video_scanner_sather` →
`testIIeResetClearsDhgr` (which also asserts TEXT and MIXED still survive, so
the fix is not a blanket wipe), mutation-controlled.

**Checked and correct: `$C019` polarity on a //e, and the frame length.**
p. 3-23, § SWITCHING SCREEN MODES IN TIMED LOOPS, rendered because the OCR
scrambled its two listings into contradicting each other: `VBLOFF EQU $C019
MINUS => VBL', PLUS => VBL`. Bit 7 SET means VBL' — *not* blanking, i.e. the
beam is in the displayed region — which is POM2's `nowActive ? 0x80 : 0`.
(The //c path correctly differs: there `$C019` is the latched VBLINT flag per
Apple IIc Tech Note #9, not the live beam.) The same paragraph states the two
figures POM2's video geometry uses: *"A horizontal scan in the Apple takes
exactly 65 machine cycles of the 6502. A vertical scan takes exactly **17030**
machine cycles"* — not the 17045 the worker budgets per UI tick, which is the
split `CpuClock.h` already documents. It also hands over the canonical
beam-raced split-screen idiom (poll `$C019`, then count 65 cycles a line),
which is what POM2's `vbl_edge_phase` and MAD EFFECT work against.

**The bound on that claim, from the same page:** Figure 7.1 is *"only a
functional representation resulting primarily from my investigations of Apple
IIe operational features. Hopefully Figure 7.1 is functionally correct, but it
does not correctly depict such circuit details as true arrangement of logic
gates and flip-flops."* It is a very good functional model and it agrees with
MAME; it is not a die shot. *Settled by:* a scope on AN3 across a Ctrl-Reset.

**Two other POM2 citations into these books were checked while here and are
correct**: `DEV.md`'s *"Sather Fig 5.13"* for the language-card reset state
(5.13a is the MMU functional diagram, showing reset detection and the soft
switches, and the numbered note beside it reads *"When a system reset occurs,
all MMU soft switches are reset (turned off). High RAM is disabled for reading
and enabled for writing. PRE-WRITE is reset, and bank 2 is selected"* —
exactly POM2's `lcBank2Active=true`, `lcPrewrite=false`), and `CpuClock.h`'s
`$18` HBL offset, cited to *Understanding the Apple IIe* **p. 5-10**, which
the rendered page confirms is the right page.
