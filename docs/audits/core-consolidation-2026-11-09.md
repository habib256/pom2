# Core consolidation — 2026-11-09

Working tree based on `b07f3a0a`, macOS ARM64 Release, Apple Clang with `-O3`
and thin LTO. This is local execution evidence, not a Windows/Linux run or a
physical-hardware comparison. Existing unrelated CI/PLASMA work was retained.

## Clock domains

Phasor, MouseCard, E-Z Color, SNES MAX and the Echo+ TMS5220 VIA now consume
the unpaced machine CPU clock. Menu pacing accelerates their work alongside
the guest; a hardware accelerator still changes the CPU/bus ratio. Phasor's
AY event replay and jitter buffer scale with pacing while AY pitch retains
the phase-0 clock. Echo+ speech and PCM retain their paced conversion.

The new Phasor timer matrix failed against the old implementation at the x2
case, then passed with the fix. The final targeted run passed all six tests:
`accelerator_clock_chunk`, `phasor_timeline`, `snesmax_card`,
`echoplus_tms5220`, `diskii_insert_thread_stack`, `session_writable_media`.
Timer coverage includes PAL/NTSC, x1/x2/x4, half speed and hardware
acceleration; the controller sound-card matrix additionally exercises MAX.
Echo+ compares its audio against a reference at the same paced CPU rate.
This first batch does not certify every peripheral's clock conversion.

### Follow-up: coprocessors and guest timers

Nine more cards consume the unpaced machine clock: SoftCard Z80, SoftCard II,
Premium SoftCard, Applicard, ALS CP/M, Liron, Workstation Card, ClockCard and
MouseCardAppleWin. The first seven only assign their cycle-conversion rate;
the controller supplies paced then machine clocks before executing work.
ClockCard and MouseCardAppleWin instead ignore paced notifications once bound
to machine time, preserving pending phases without a double rescale.
ClockCard also rescales DATAOUT and TP phase on a hardware-clock transition;
its calendar deliberately remains host civil time.

Regression coverage checks retired Z80 work, the Liron motor-disable deadline,
Workstation firmware's LocalTalk node acquisition and pending clock/mouse
edges. The controller integration exercises Premium SoftCard in auxiliary
slot 0 across PAL/NTSC, menu changes including MAX, and a TransWarp.

All ten targeted tests passed, with no internal skips: `softcard_toggle`,
`softcard2_card`, `premium_softcard`, `applicard_card`, `als_cpm_card`,
`liron_boot35`, `workstation_card_pacing`, `clock_card_smoke`,
`mouse_card_applewin_smoke`, `accelerator_clock_chunk`. The first run exposed
an old aux-slot getter assertion comparing against the paced clock; it now
checks the machine clock, and the full integration test passes. Real CP/M
boot subcases ran. `bench_identity` also passed; no new timing measurement
was taken for this follow-up. Protected corpus replay kept **3/3 titles,
9/9 checks, no skips**, without changing expectations. The default corpus
also replayed unchanged: **10/10 runnable titles, 29/29 checks**, with only
the existing absent `service-disk-1` image skipped. File-size, include,
documentation-reference and whitespace guards passed.

CPU-1 remains open: SSI263 phoneme timing still uses the paced denominator;
Porte-Parole and Echo TMS5220 mix chip progression with audio playback;
Super Serial Card combines guest timers with wall-time external transport.
Read-only review also identified a pre-existing ALS CP/M limitation:
`nextTick_` is not rescaled across a live hardware-accelerator change. The ALS
test covers menu transitions at fixed hardware rates, not that pending-edge
case. None of these remaining cases is certified by this batch. The ALS
pending-edge case was subsequently reproduced and fixed in the
[follow-up bug hunt](core-bug-hunt-2026-11-09.md).

## DiskImage ownership and persistence

`sizeof(DiskImage)` changes from **253624 to 20688 bytes** on this build.
Its 35 × 6656-byte nibble buffers move to a fixed-count vector; copies are
deep, moving transfers buffer ownership. The constrained-stack test keeps
three values on a 512 KiB thread stack and checks independent mutation,
copy/move assignment, reload and drive insertion/ejection.

Two consequences are covered explicitly:

- Reloading a moved-from dirty image clears its stale mount state before
  attempting a flush. The new test first exposed this as a crash.
- Session accounting includes the heap buffers. After moving its media, the
  session validator ejects its source objects so its destructor cannot flush
  an in-flight write burst through transferred buffers. A writable session
  with a pending controller transition exercises this path.

Seven storage/grammar checks plus `corpus_ci` passed in a separate eight-test
run: `disk_writeback_smoke`, `woz_writeback_smoke`, `d13_roundtrip_smoke`,
`disk_2mg_writeback_smoke`, `media_external_change`, `corpus_manifest`,
`bench_identity`, `corpus_ci`. Total across the two targeted runs: **14/14**.
This is targeted validation, not a claim that the entire CTest suite ran.

## Real protected media

Sources and temporary-download/replay recipe:
[protected corpus](../../tests/corpus/protected.md).
All commercial images remain outside the repository. Only manifests,
SHA-256s, firmware fingerprints and frame hashes are checked in.

Final replay without `--bless`:

| Title | Profile | Checks | Observed milestone |
|---|---|---:|---|
| Captain Goodnight | `iie-u` | 4/4 | Title, turn-disk prompt, face-B arrival, running game |
| Prince of Persia | `iie` | 3/3 | Title, first dungeon, frame after input |
| Bandits | `iie-u` | 2/2 | Title, game after SPACE |

**3/3 titles, 9/9 checks; no skips.** Every reference frame was inspected.
`swap 1 <path> sha256=...` inserts Captain's face B at the scripted moment
without reboot. All swap images are copied and hash-checked before boot.
Missing face B and a deliberately wrong face B each returned 77, with zero
checks run and an explicit skip reason.

Captain and Prince use the same SHA-256s as the
[October observations](protected-disks-2026-10-12.md). Captain has half-track
positions; this Prince capture has whole tracks only. Neither is a WOZ FLUX
capture. Bandits is:

- Source: [WOZ-a-day Bandits](https://archive.org/details/wozaday_Bandits).
- SHA-256: `0a2472f65c73eee0afb9b9ec795892a22b967e545fb21ae34dc7f12faf241687`.
- WOZ2 INFO v3; FLUX chunk at byte 831488, length 160;
  INFO `fluxBlock=1624`, matching `1624 × 512`.
- 19 FLUX-mapped quarter-track positions: 6, 10, …, 78.

A temporary copy of `protected_disk_boot_probe.cpp` stepped one instruction
at a time and decoded `LDA abs/abs,X/abs,Y` and `BIT abs`, calculating the
actual address before execution. Between 3 and 8 emulated seconds it logged
`LDA abs,X` at PC `$04E9`, effective address `$C0EC`, motor active, at **each
of the 19 FLUX positions**. Their event arrays held 37892–38712 entries.
This establishes data-register reads on the actual FLUX tracks, beyond
merely loading a file containing an unused FLUX chunk.

The diagnostic uses `roms/apple2e.rom` with NMOS CPU; the corpus uses its
normal `iie-u` firmware. Both reached the game. Diagnostic source/logs and
captures were kept in `/tmp/pom2-real-flux`; corpus media/captures in
`/tmp/pom2-core-protected`. These are temporary evidence, not permanent media
fixtures. Coverage stops at boot and entry into gameplay: no completed-game,
later-level or general copy-protection certification is claimed.

The existing default corpus also replayed unchanged: **10/10 runnable titles,
29/29 checks**, including the DIX tour. One existing optional title,
`service-disk-1`, skipped because its repaired image was absent.

## Performance measurements and limits

A freshly built baseline was saved as `/tmp/pom2-core-bench-before` before
runtime edits. Candidate: `build-release-tests/pom2_bench`, same ARM64 Release
flags (`-O3 -DNDEBUG -flto=thin`), no PGO. Every run used
`POM2_MEDIA_WRITE_DEFAULT=protected`, from the repository root.

Initial series: nine adjacent A/B pairs per workload, alternating order,
after one warm-up of each binary. Values below are seconds and percent
change (negative is faster). All cycle counts, RAM hashes and framebuffer
hashes matched within each comparison.

| Arguments (plus `--quiet`) | Best baseline | Best candidate | Best delta | Median paired delta |
|---|---:|---:|---:|---:|
| `--frames 30000` | 5.052 | 5.253 | +3.98% | -0.47% |
| `--rom roms/apple2e.rom --iie --frames 30000` | 5.620 | 5.563 | -1.01% | -1.77% |
| `--disk packaging/disks/MICRO-SOKOBAN.dsk --frames 6000` | 2.317 | 2.352 | +1.51% | +5.55% |

The disk result prompted a confirmation on the final rebuilt candidate:
18000 frames on the same MICRO-SOKOBAN disk, five ABBA/BAAB blocks (two runs
of each binary per block) after warm-up. Block means:

| Block | Baseline seconds | Candidate seconds | Delta |
|---|---:|---:|---:|
| 1 | 9.0590 | 10.5820 | +16.81% |
| 2 | 6.3960 | 6.1510 | -3.83% |
| 3 | 6.3760 | 6.3265 | -0.78% |
| 4 | 7.0320 | 6.3345 | -9.92% |
| 5 | 6.8825 | 6.5280 | -5.15% |

Median block delta: **-3.83%**. The initial apparent
disk slowdown did not reproduce in four of five longer blocks. There were
**74 timed runs** across both series, with matching cycles/main-RAM/frame
hashes for each workload. No project build or other project test was run
concurrently with either timing series.

**No speed-up is claimed.** This shared host showed substantial timing drift
and an unrelated `wasm-opt` process was active during the first series. Best
runs and paired statistics disagree; these measurements cannot establish a
small performance effect or predict another host. The change is retained
for stack safety and buffer ownership, with no reproducible slowdown in the
confirmation, not as a measured throughput improvement. Raw observations
remain in `/tmp/pom2-core-benchmark.json` and
`/tmp/pom2-core-disk-confirm.json`.
