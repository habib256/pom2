# Extension-card implementation review — 2026-10-17

Base revision: `3e91cc7`, plus the local fidelity corrections described below.
Initial scope: all 29 non-empty keys in `SlotCardCatalog.h`, the hidden Echo+ TMS
scaffold, and the related Zip Chip, RamWorks, language-card and no-slot-clock
devices. This is a source/contract/test review, not certification against
physical examples of every board. No new hardware captures were available.
The later 358-case run also includes the SNES MAX added concurrently; that
work and the W5100 LISTEN addition are described in the separate
[non-IIgs parity report](non-iigs-parity-2026-10-17.md).

## Findings

### Corrected: VidHD signature decoded in the wrong window

`VidHDCard` exposed `$24 $EA $4C` at `$C0n0-2`; a program scanning slot ROM
at `$Cn00-02` could not find it. The implementation and its old test shared
the same mistaken address map.

Reference: AppleWin [`VidHD.cpp`, InitializeIO/IORead](https://github.com/AppleWin/AppleWin/blob/6c579da5bd9460ebf7c6051a6d635efbd1375c85/source/VidHD.cpp)
registers its handler as `IOReadCx`, whose argument position is explicit in
[`Memory.h`, RegisterIoHandler](https://github.com/AppleWin/AppleWin/blob/6c579da5bd9460ebf7c6051a6d635efbd1375c85/source/Memory.h).

The signature and debugger peek now use slot ROM. Remaining ROM bytes and
all device-select reads return the floating bus; the card does not claim
C800. The revised `vidhd_card` failed before the correction. It also checks
all seven slot positions and the //e slot-3 ROM-selection switch.

### SSI263 fidelity corrections completed in this worktree

- Host mute leaves Sound II and Cricket/Echo playback running; Cricket/Echo
  gain changes are ramped without callback scratch allocation.
- Guest AMP=0 and FILFREQ=$FF preserve the PCM cursor instead of pausing it.
- Sound II bus RESET preserves speech registers, repeating requests,
  pending writes and the shared PCM timeline while clearing VIA/AY state.
  Cold boot invokes a separate slot power-cycle hook to clear speech.
- CTL H-to-L latches function and IRQ enable. DUR=00 disables interrupts
  without replacing the previous function; running DURPHON writes do not
  change those latches.
- Typical power-on DURPHON=$C0 and FILFREQ=$FF follow AppleWin's observed
  defaults. Waking the chip alone can produce the detection request.
- INFLECT/RATEINF acknowledgement permits a request at the next repeat
  boundary. Sound II's VIA2.CA1 is gated by SSI enableInts, independently
  of the completion status D7 and VIA IER.
- Versioned MBS v3 / EP v2 blobs preserve the new latches and accept the
  old MBS v1/v2 / EP v1 layouts. Missing old repeat phase is restarted;
  an unknown old function with DUR=00 defaults to transitioned inflection.

Reference: AppleWin `6c579da5bd94`, `SSI263.cpp` Write,
SetDeviceModeAndInts, Update, SetSpeechIRQ and RepeatPhoneme;
`SSI263.h` ResetState. Tests cover chip behavior, both card IRQ paths,
audio continuity, old-format migration and truncated-envelope rejection.
The subsequent duration refinement below replaces the old timer formula
with the primary PCM path's output-frame count.

### Corrected: Phasor rejected-snapshot audio state

Review found `loadSnapshotState` clearing its event queue/generation and
reset-held flags before checking magic, version and payload length. An
ignored foreign/short snapshot can therefore change future audio even
though its serialized registers appear untouched. The common snapshot
contract compares register blobs, so it cannot detect this transient loss.

Timeline invalidation and reset-held flag updates now happen only after
all framing checks pass. `phasor_timeline` compares the subsequent output
with an untouched twin for null, foreign, unknown-version and truncated
blobs in both compatibility and native modes. Before the correction, even
the null-blob case diverged on the first rendered block.

## VidHD/SHR assessment

The implementation is a small, bounded frame painter behind
`CardVideoSource`, with no direct display-to-Memory dependency. Register
broadcasts are confined to four write addresses; motherboard cassette,
speaker and //c ROMBANK behavior remains outside the card. The palette
and line indexes stay inside a 64 KiB aux bank: 200 lines of 160 bytes,
SCBs at $9D00 and 16 palettes at $9E00-$9FFF.

The tests exercise the 320 mode nibble order and horizontal doubling,
320 fill propagation, 640 mode's four palette quarters (8/12/0/4), vertical
doubling, display ownership, reset, register-only snapshots and the selected
RamWorks bank. Color expansion is 4-bit to 8-bit via multiplication by 17;
AppleWin's older painter uses 16. This is a rendering policy difference,
not evidence of a wrong palette index.

Verdict: a useful **partial SHR contract model**, with a real detection
defect fixed by this review. It is not a complete VidHD implementation.
Remaining: text/border color rendering, SHADOW behavior, B&W DHGR,
extended text resolutions, the card's own enable switch, ][/][+ aux
mirroring and beam-timed pixel/SCB/palette updates. An independent capture
from real VidHD hardware is still owed. The manufacturer's
[manual 1.2](https://www.callapple.org/docs/vidhd/VidHD_Manual_1.2.pdf)
describes a broader product than the subset implemented here.

## Per-card evidence and boundaries

Every catalog key is listed; a passing smoke test is evidence for its
asserted contract, not proof that all board features are implemented.

| Key | Main checks selected for this review | Boundary / quality observation |
|---|---|---|
| `diskii` | `diskii_write_integrity`, `diskii_write_burst`, `media_contract` | Real P6/LSS path; media identity/write-back contracts matter more than register smoke alone. |
| `hdv` | `hdv_card_smoke`, `hdv_boot_scan`, `hdv_two_drives` | Synthetic block interface and ROM; a functional service, not an ATA/SCSI board model. |
| `cffa` | `cffa_card_smoke`, `media_contract`, snapshot contract | Real ROM/IDE model; no CHD requirement. External media is deliberately outside register snapshots. |
| `smartport35` | `smartport_card_smoke`, `smartport_eight_units`, `smartport_bus_device` | Synthetic service/firmware veneer; CONTROL data-list support remains limited. |
| `liron` | `liron_chain`, `smartport_bus_device`, factory snapshot checks | Real controller firmware and IWM; drive-side CPU is replaced by a protocol model. Nested malformed snapshots have weaker atomicity. |
| `ssc` | `ssc_acia_smoke`, `ssc_firmware`, `ssc_tcp_transport` | Real EPROM when present; host TCP cannot express electrical BREAK/RTS behavior. |
| `printer` | `printer_card_smoke`, `printer_detection` | Synthetic always-ready spool interface, intentionally higher level than a parallel board. |
| `grappler` | `grappler_card_smoke`, `grappler_printer_state`, snapshot contract | Real firmware when available; fallback and instantaneous strobe are documented approximations. |
| `grappler1` | `parallel_cards`, `printer_detection`, snapshot contract | ROM-gated, edge-driven Centronics latch and pending-byte state. |
| `pic` | `parallel_cards`, `printer_detection`, snapshot contract | ROM-gated; strobe pulse timing, polarity switches and jumpers are incomplete. |
| `videoterm` | four `videx_videoterm_*` suites | Firmware/CRTC/VRAM contract; whole-frame painting, no CRTC raster timer. |
| `vidhd` | `vidhd_card`, factory/profile and snapshot contracts | Partial SHR model; detection fix and limits above. |
| `clock` | `clock_card_smoke`, clock fan-out and standard-clock checks | Real ThunderClock EPROM path exists; live DATA_OUT behavior is a deliberate divergence. |
| `uthernet` | `uthernet_cs8900_smoke`, `slirp_loopback_fence` | CS8900A frame model; actual connectivity requires a compiled backend. Frozen scope. |
| `uthernet2` | `uthernet2_w5100_smoke`, W5100 receive/socket/DNS suites | Host-socket TCP/UDP; inbound LISTEN arrived in the concurrent parity pass. Host sockets are not rewindable. |
| `fujinet` | `fujinet_card`, `fujinet_net_device`, `fujinet_link_seam` | Relay to an external device, not a full local FujiNet emulation; tests do not certify an external peer. |
| `softcard` | `softcard_toggle`, snapshot contract | Actual Z80 bus hand-off; this review's subset is not a fresh CP/M corpus run. |
| `chatmauve` | `le_chat_mauve_smoke`, `chatmauve_dot_rules`, `video7_parity_smoke` | Strong pixel rules for supported paths; Eve mid-line state, RVB and //c adapter have documented limits. |
| `mouse` | mouse smoke, quadrature and axis parity | Real firmware/MCU/PIA; ROM-dependent behavior is distinct from the HLE path. |
| `mouseaw` | AppleWin mouse smoke and axis parity | HLE MCU behavior with real host EPROM; do not present it as the same silicon-level implementation. |
| `mockingboard` | smoke, audio-quality/timeline, clock and snapshot contracts | AY/VIA timing is tested; analog output stage is not a physical waveform model. |
| `mockingboard_c` | same card checks plus SSI suites | SSI modes/requests and PCM-derived durations corrected; inflection/articulation remain incomplete. |
| `phasor` | card smoke, timeline, mode-scale and snapshot contracts | Four-AY audio and two SSI263AP speech chips; fixed-PCM and analog mix limits remain. Rejected-snapshot audio and compatibility-mode pair reset corrected. |
| `echoplus` | `echoplus_card_smoke`, SSI suites | This key is Cricket/SSI speech, not the real TMS5220 Echo+. Same fixed-PCM waveform limits. |
| `workstation` | card/CardCat/pacing and SCC firmware checks | Runs its coprocessor firmware; no LocalTalk network peer or working network boot. Frozen scope. |
| `4play` | `fourplay_card`, snapshot contract | Small, complete digital input register contract; host buttons are intentionally not rewind state. |
| `snesmax` | `snesmax_card`, catalog snapshot contract in the later 358-case run | Added concurrently: two standard SNES controllers and serial latch/clock behavior; see the separate parity report. |
| `transwarp` | card, chunk-sampling and clock checks | Host CPU retiming with real shadow ROM; not a second independently clocked card CPU. |
| `slinky` | `slinky_card`, snapshot contract | Real firmware, address-latch/data-port behavior and sparse RAM snapshots; no new AppleWorks hardware comparison. |
| `sam` | `sam_card`, clock and snapshot contracts | Cycle-stamped DAC latch/audio model; no physical analog reconstruction claim. |

Related checks include `zip_chip`, `ramworks_smoke`, language-card banking
and `no_slot_clock_smoke`. Hidden `echoplus_tms` remains a silent scaffold
with a provisional address map, covered for lifecycle/snapshot safety, not
for real speech or correct hardware decoding.

## Remaining priorities

1. Validate SSI inflection/articulation and Phasor/Cricket reset behavior
   against physical hardware or a firmware oracle. Timer and PCM are now
   reconciled against AppleWin's primary audio path; fixed samples still do
   not synthesize all hardware register effects.
2. Obtain an independent VidHD reference image and state which missing
   features are required by actual software before expanding the model.
3. Keep capability descriptions honest: frozen TMS Echo+ and Workstation
   networking are not implemented; Phasor speech uses fixed PCM, with an
   approximate mono output mix.
4. Complete abstraction metadata for newer catalog keys. The UI's
   `abstractionCatalog()` still lacks several keys already described in
   `docs/lle_vs_hle.md` (CARDS-001); this is a documentation/UI gap rather
   than proof of a bus failure.

The malformed nested snapshot findings are corrected: Workstation validates
its SCC first, and Liron's LIR2 format validates every nested section before
resetting live state. Catalog P3 hybrids are now hard failures. The file
restore path still refuses SLOT sections; the format change strengthens
in-memory rewind and future slot-restore support too.

The Phasor native GAL discrepancy discovered during the general hunt is
also corrected below: primary PB4 / secondary PB3, native latch coupling,
OR reads, full-page VIA aliases and Echo+ single-VIA/stereo routing. The
previously missing Phasor SSI speech chips are now integrated below.

## Validation

The initial card audit, before the duration follow-up, rebuilt all 79
selected targets successfully. Its 79-entry CTest run
completed in 52.89 s: 73 passed and six network tests failed because the
sandbox refused local socket binding. Those six were rerun outside the
sandbox and all passed in 15.49 s: `fujinet_net_device`,
`uthernet2_w5100_smoke`, `w5100_udp_recv`, `fujinet_card`,
`ssc_tcp_transport` and `slirp_loopback_fence`. Across the two runs every
selected test passed; none was skipped. This includes the revised VidHD
and Phasor regressions and the SSI snapshot compatibility checks.

The [exact 79-test selection](extension-cards-2026-10-17.tests.txt) spans
all catalog families and shared factory, profile, clock, IRQ, media and
snapshot contracts. After building the listed test executables, rerun with
a CTest version supporting `--tests-from-file`:

```sh
ctest --test-dir build \
  --tests-from-file "$PWD/docs/audits/extension-cards-2026-10-17.tests.txt" \
  --output-on-failure -j 4
```

Native macOS RelWithDebInfo build with libslirp 4.9.3 available; no sanitizer
enabled for this run. The initial audit did not run the entire 357-test
suite (the later general hunt below did), other OS builds or new
physical-hardware/corpus comparisons. Source checks passed:
`git diff --check`, `tools/check_file_sizes.sh` and
`tools/check_includes.sh` (814 first-party files).

The SSI duration follow-up rebuilt and passed 15 affected suites in
10.34 s: SSI chip/map/timeline, Cricket/Echo, Mockingboard smoke/quality/
timeline, stereo/mix law, card snapshot contract, rewind audio, atomic
diagnostics, standard-clock/fan-out and audio coordinator. The map suite
checks 1536 code/DUR/function/clock combinations against an independent
read/average/skip transcription, plus all 15 nonzero amplitudes. No new
hardware capture was used. Source snapshot offsets remain unchanged.

The Sound II warm-reset follow-up built the native `POM2` application and
passed 17 affected suites in 12.01 s: the previous 15 plus `slot_bus_smoke`
and `ay_noise_reset_state`. Before the fix the new warm-reset regression
failed at all three host rates (22.05/44.1/48 kHz). It now verifies identical
audio against an uninterrupted twin across queued writes and consecutive
resets, retained SSI request with cleared VIA IRQ, exact next-repeat timing,
and cold silence. The slot-bus regression separately reproduced and fixed
the expansion-ROM latch surviving power loss; ordinary RESET retains it.
The reference is AppleWin's Mockingboard-C wiring notes and SSI263 `Reset()`
at the pinned revision. No Phasor SSI263P/AP or Cricket reset parity is
claimed. File-size/include checks and `git diff --check` passed.

The subsequent refinement reproduced Phasor's secondary AY surviving PB2
reset in compatibility mode. The reset now reaches both attached AYs in
all modes and carries both reset events to audio. Its smoke regression
spans two VIAs, two modes and four select masks; the timeline regression
compares an audible native reset with a compatibility reset followed by
return to native mode. Reference: AppleWin's hardware finding
[#1197](https://github.com/AppleWin/AppleWin/issues/1197), implemented at
`6c579da5bd94` in `Mockingboard.cpp:253,331-335`.

The general hunt then reproduced Workstation's partial restore with a
complete card header/RAM/CPU and an incomplete SCC tail. The SCC parser's
atomic rejection now runs before card RAM/CPU changes. The new card test
checks every SCC-tail truncation, foreign SCC magic/version and valid
restores. The catalog contract now fails, rather than warns, if a
Workstation hybrid returns. Liron's weaker nested snapshot handling remains
open; the file restore path still refuses SLOT sections.

### General-hunt validation after both corrections

`cmake --build build -j 8` completed for all default targets, including
`POM2`; a final incremental pass included the Workstation change made
during the initial rebuild. All 357 registered CTest cases ran in 458.11 s:
341 passed, while 14 local-socket cases failed under sandbox restrictions
and two OpenGL cases timed out on inaccessible macOS services.
`ctest --test-dir build --rerun-failed --output-on-failure -j 4` outside
the sandbox then passed all 16 in 39.82 s:

`tnfs_client`, `tnfs_client_hostile`, `fujinet_net_device`, `socket_compat`,
`ai_control_server_smoke`, `crt_glass_resample`, `crt_barrel_view`,
`ai_mouse_hle_probe`, `uthernet2_w5100_smoke`, `w5100_udp_recv`,
`sp_over_slip_link`, `fujinet_card`, `child_process`, `ssc_tcp_transport`,
`slirp_loopback_fence` and `ai_control_mem_bank`.

Across the two executions all 357 cases passed. CTest reported zero skipped
tests; `tools/check_ctest_skips.sh` passed on both logs. The onboard
SmartPort suite still prints its two allowlisted optional HDV-trace skips
because `POM2_TRACE_HDV` was unset; its normal synthetic-HDV boot assertions
ran. No sanitizer, new physical capture or other-OS run was added.
The remaining P3 hybrid warnings are Liron/Liron+ROM, both at 112/211 bytes;
Workstation no longer warns. Source format, size and include checks passed.

### Liron/GAL correction and further fidelity checks

The next pass first reproduced Liron state loss at prefix 8/211 and the
Phasor primary select reaching the wrong AY. Liron now emits `LIR2` with a
required complete tail and preflights all nested parsers before mutation.
Its IWM validation mode publishes no callbacks; bus/mechanism probes have
no media attached. LIR1's known optional-tail variants remain readable,
deriving a missing card clock from the saved IWM anchor. New tests cover
every new-format prefix, nested magic/length corruption and legacy layouts.
The catalog no longer tolerates hybrid restores as warnings.

Phasor now follows the pinned AppleWin hardware GAL notes: PB4 primary,
PB3 secondary; secondary LATCH selects/addresses both, primary LATCH
deselects secondary, primary READ/WRITE reaches a selected secondary, and
READ ORs actual drivers. Each broadcast WRITE is delivered once. Reserved
AY-latch bit 6 preserves GAL selection in PHS v1/v2 snapshots. All 128
mode/page combinations cover full-page VIA decode, Echo+ VIA2 aliases and
undefined modes. Echo+ retains independent AY address latches.

Further regressions reproduced Phasor mute freezing generator phase and
Echo+ failing to mirror its pair to both speakers. Both are corrected;
same-rate Echo routing changes use the stamped clock-event stream, keeping
future mode changes out of earlier audio. Tests compare muted/audible twins
in all three modes and check initial/future Echo stereo routing. These
corrections do not add SSI speech chips or certify analog waveforms.

All default targets were rebuilt after the corrections. The complete CTest
configuration had grown to 358 cases during concurrent repository work,
including `snesmax_card`. Run outside the sandbox for local sockets and
macOS OpenGL, it passed 357 cases in 189.39 s. Its only failure was an old
ProDOS decode-safety fixture assuming a 200 KB host file was excluded:
concurrent tree-file support had made that file representable. The fixture
now uses a sparse file beyond the 24-bit EOF limit, preserving its actual
assertions: the hidden host file is untouched and the colliding guest file
is saved under a fresh name. Its CTest rerun passed in 0.02 s. Across these
executions all 358 cases passed; the catalog log contains no P3 hybrid
warnings. The skip guard reported no unexpected skips (the previously
documented optional HDV traces remain unset). These results describe the
compiled state of that run, not later concurrent edits or other OS builds.


## Phasor SSI263AP follow-up

A native phoneme completion failed to assert IRQ before this change
(`testPhasorSpeechRequest`, reproduced against the existing card). Both
speech chips now have independent registers/timers, full-page write aliases,
D7-only native reads, native direct IRQ and compatibility CA1 wiring.
The fixed AP reset model distinguishes warm reset from power-on; older
P silicon quirks remain outside this implementation. Echo mode hides access
and direct speech IRQ without stopping the chips. PHS v3 serializes both
chips and rejects every truncated prefix without modifying live state;
legacy v1/v2 default speech to the silent power-on state.

Audio uses the shared emulated-cycle cursor. The first queued write seeds
its replay state before changing CPU registers, preventing future CTL writes
from sounding early. FILFREQ aliases 5-7 now also update the shared audio
replay bank. Both voices use an approximate mono sum with headroom;
analog output routing/levels and inflection/articulation are not certified.
Reference: AppleWin 6c579da5bd9460ebf7c6051a6d635efbd1375c85,
Mockingboard.cpp:698-711,800-819 and SSI263.cpp:806-869,930-957.

`phasor_card_smoke` now includes every write address in all eight modes,
read aliases/floating bits, independent/broadcast acknowledgements, IRQ
routing, PAL/accelerated completion, AP resets, truncated/legacy snapshots,
and audible speech from either chip with no future event leakage.

Validation of this follow-up: all default targets built successfully.
The 358-case suite passed 357 cases; `controller_mode_races` timed out
across a long host interruption (6473 s elapsed versus its 300 s timeout).
Its isolated rerun passed in 70.68 s. No unexpected skips were reported.
Logs: `/tmp/pom2-phasor-speech-ctest.log`,
`/tmp/pom2-phasor-controller-rerun.log`; file-size, include and whitespace
checks passed. These are local macOS results, not Windows CI evidence.
