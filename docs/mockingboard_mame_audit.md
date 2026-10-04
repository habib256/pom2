# Mockingboard audit — 2026-10-03

Reference: [MAME revision a2b6ba2d4be70dabf7ff7a642749dda0c6e70498](https://github.com/mamedev/mame/commit/a2b6ba2d4be70dabf7ff7a642749dda0c6e70498).
The comparison used source downloaded at this revision, rather than moving
master or search-engine line numbers. This is a source-level differential
check, not an end-to-end recording comparison with a running MAME binary.

## Confirmed defects corrected

| Trigger | Before | After / reference |
|---|---|---|
| Reduce a tone period below the elapsed counter | Counter discarded; output toggled once | Preserve remainder and transition parity; `sound/ay8910.cpp:1073-1084` |
| Write the same ORB byte again with AY WRITE held | No dispatch, losing a repeated envelope trigger | Dispatch each store with enabled outputs; `machine/6522via.cpp:847-853`, `bus/a2bus/a2mockingboard.cpp:391-410` |
| Access AY data immediately after reset, without latching an address | Register 0 accepted writes and answered reads | Deselected: writes ignored, reads $FF; `sound/ay8910.cpp:1305-1308,1356-1393` |

The tone fix collapses MAME's subtraction loop into quotient parity and
remainder, so dropping from period $FFF to 1 never creates a long loop.
The reset fix reuses the existing deselection bit in the saved address
byte. No snapshot layout changes. The generator and VIA are shared with
Phasor; the same corrections apply there.

## Regression evidence

`tests/ay_mame_parity_test.cpp` contains an independent transcription of
MAME's tone loop and envelope state machine. Tone modulation covers every
old period 2..4095, eight new periods (including 0, 1 and $FFF), three
channel phases and eight subsequent ticks. Envelope checks cover all 16
shapes at periods 0, 1, 2, 17 and $FFFF, plus same-value shape retriggers
that preserve the running period counter. Tone, reset and bus cases each
failed against the original implementation; the envelope comparison already
passed before the changes.

`mockingboard_bus_edges` also compares repeated held-command R13 writes
with explicitly pulsed writes through both actual PSGs and their rendered
audio. This catches a fix that dispatches telemetry but loses the audio
retrigger. Existing noise, spectral purity, bass-response, digi timeline,
IRQ, accelerated-clock and rewind tests complement these new cases.

## Fidelity limits and deliberate differences

- **VIA timer timing:** preserve POM2's documented latch+2 recurring T1
  period and counter readback. DIX's MAD EFFECT/TRIBU/OLDSKOOL regression
  cases establish why these differ from the MAME reference. This audit
  does not replace those hardware/corpus contracts with MAME values.
- **Analogue amplitudes:** POM2 uses a normalized measured 16-level table.
  MAME builds volume and envelope tables from its resistance model and
  output flags. The reference Mockingboard retains the default
  `AY8910_LEGACY_OUTPUT` flag (`ay8910.cpp:1580`); three streams do not
  disable normalization. The old POM2 comment claiming otherwise was
  corrected. Removing MAME's fixed-volume baseline and normalizing its
  default 1 kOhm curve gives level 12 = 0.4725 versus POM2's 0.5128
  (the largest absolute difference, about 4% of full scale). These are
  not sample-identical analogue models; envelope DC offsets and board
  loading still need measurement before retuning.
- **Resampling:** POM2 integrates piecewise-constant chip output over each
  device sample. MAME synthesizes at clock/8 and resamples the stream.
  Box integration suppresses aliasing but is not a full reconstruction
  filter. The existing spectral test verifies one operating point; it
  cannot establish zero aliasing for every tone, noise or modulation rate.
- **Data-bus callbacks:** MAME's PA callback updates its data latch; PB
  dispatch executes the command. POM2 also reapplies held commands on
  changed PA pins, an existing documented modelling choice. The new ORB
  correction does not settle the physical transparency of PA-held strobes.
- **VIA subset:** shift-register serial modes, CA2/CB1/CB2 handshakes and
  PB6 pulse counting remain outside the current wired-card model. This is
  not a complete 6522 implementation.
- **Sound II speech:** SSI263 fidelity cannot be inferred from this AY
  comparison; MAME's SSI263 path is itself documented as unsupported in
  the reference card source.

A future fidelity pass should measure the analogue table and run a
frequency/modulation sweep before choosing a reconstruction filter. Those
changes require an audio oracle beyond these generator-level checks.

## Validation completed — 2026-10-04

- Native `POM2`, affected test executables and all 56 shared-core test
  consumers rebuilt successfully.
- 21 focused AY/VIA/Mockingboard/Phasor/PAL/rewind tests passed.
- Entire 342-entry suite: 341 tests passed, one skipped
  (`videx_videoterm_boot`, missing firmware). Sixteen socket, subprocess
  and OpenGL tests initially failed under the execution sandbox; all 16
  passed on an authorized rerun outside it. No source fixes were needed
  for those environment failures.
- Audio quality measurements: 0.51% inharmonic energy for TP=16
  (about 3995 Hz), residual DC -0.000001, PWM 510.0 Hz against
  511.4 Hz expected, and bass fundamental response within 0.01 dB of
  the analytic MAME 2-pole high-pass at the tested frequencies.
- File-size, version-string, settings-key and workflow-pin guards passed;
  `git diff --check` passed. `bench_identity` passed in the full suite.

The spectral and bass measurements are existing regression measurements,
not claims of sample-identical output to a MAME recording.

Follow-up: the four Videx dumps were fetched from the catalogued RetroBIOS
archive and verified against their CRC32 and SHA-256 values. The formerly
skipped `videx_videoterm_boot` now passes, completing all 342 tests across
these validation runs. The corrected local macOS binaries and verified
ROMs were installed in `/Applications/POM2.app`; the previous application
was retained as a backup. The bundle is ad-hoc signed and its non-system
dynamic libraries are included inside `Contents/Frameworks`.
