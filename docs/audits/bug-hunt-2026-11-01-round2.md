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
