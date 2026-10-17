# Non-IIgs emulator parity implementation — 2026-10-17

Requested scope: address the emulator comparison gaps, excluding IIgs. This
request reopens relevant frozen parity work; it does not establish that all
the work below is complete. Base checkout: `3e91cc7` plus existing local
card-fidelity changes, which were retained. This report distinguishes code
implemented in this pass from outstanding hardware/product projects.

## Implemented in this pass

| Area | Behavior | Checks |
|---|---|---|
| Step Over | Follow the outer RTS and stack frame rather than PC+3; inline parameters, nested/recursive calls, stack wrap and interrupt entry are covered. | `debugger` |
| Stop handling | The single-threaded frame driver stops at a debugger stop; resuming a watchpoint cannot skip a PC breakpoint at the next instruction. | `debugger` |
| Step Out | Run until the current subroutine returns, using the same frame tracking. | `debugger` |
| Conditional breakpoints | A/X/Y/P/SP/PC, six comparisons; predicates are visible in the UI and removed with their breakpoint. | `debugger` |
| Symbols | Transactional AppleWin-style, VICE/cc65 `al`, and `NAME =`/`EQU` imports; labels in disassembly and breakpoint entry. Reject banked/out-of-range addresses instead of truncating them. | `debugger` |
| Profiling/history | CPU-step visits/cycles by PC and the last 256 post-step register states. Includes interrupt-entry steps; this is not a source debugger or a bank-aware profiler. Disabled instrumentation leaves the ordinary CPU loop unchanged. | `debugger`, full CPU/benchmark regressions |
| Uthernet II LISTEN | Nonblocking local TCP accept using `127.0.0.1:Sn_PORT`, under the existing loopback opt-in. Publish peer registers/CON; use existing TCP rings. Denied ports/bind failures return CLOSED+TIMEOUT. | `w5100_socket_seam`, actual inbound exchange in `uthernet2_w5100_smoke` |
| Host-folder ProDOS files | Import tree files up to 16 MiB minus one, including master/index blocks and bitmap allocation; total staging bounded by volume capacity. Existing type/aux metadata conventions remain. | `prodos_volume_smoke`, boundary/max-size byte round trips |
| Native choosers | AppKit, Win32, zenity/kdialog; drive insertion, paint/sprite editors and symbol selection. Cancel differs from unavailable backend. | Native macOS build, existing browser interaction test; external-helper protocol test registered on Linux |
| SNES MAX | Catalog/factory/live topology, two serial controllers, seven-slot-clock latch, mirrored ports, host gamepad mapping and rewind state. Accelerator clocks do not shorten the pulse; snapshots retain its fractional phase. Start/Select belong to the card; use keyboard F1/F2 for kiosk controls. | `snesmax_card`, catalog snapshot/topology contracts |
| Windows test entry point | `POM2_PORTABLE_TESTS_ONLY` now builds eleven core regressions, including environment, semantic preflight and resumable sessions, without configuring POSIX-only harnesses. Windows CI builds/runs this subset with assertions in Release. | Local native build/run of the same subset; MSVC execution still pending |
| Session prerequisite: environment identity | File captures now record a versioned ENV section: canonical motherboard/internal I/O/character ROMs, alternate //c firmware, memory configuration and PAL/NTSC. ENV and CPU incompatibilities fail before live-state mutation; rewind omits ENV. Legacy files load with a warning. This does not add slot/media restoration. | `snapshot_environment`: DMA/video-event preservation on refusal, ROM and configuration changes, malformed/reordered ENV, legacy acceptance and TransWarp shadow transitions |
| Session prerequisite: semantic preflight | MEX geometry and all nested device sections validate without applying live state. Direct Memory, //c profile and external SmartPort loads refuse corrupt state before mutation. | `snapshot_preflight`: each nested section, truncation, geometry disagreement, preserved IRQ/DMA/video/port state and successful valid restore |
| Resumable sessions v1 | Distinct `.pom2session` archive with configuration/firmware checks, bounded records, CRC32 and embedded read-only non-WOZ Disk II media. II/II+/IIe, Mockingboard/Sound II and SNES MAX; CLI and local API save/load. Original image files are not needed on restore. | `session_snapshot`, CLI parser/runner and HTTP server smoke; [scope and format](../session-format.md) |
| Desktop session workflow | File menu and command palette save/open sessions through the native chooser or manual path fallback; extensionless saves append the suffix, existing destinations require confirmation and errors remain available for correction. | `session_file_picker`: real ImGui clicks with controller save/load, cancellation, overwrite, malformed input and unavailable native backend |
| Session WOZ media | Session v2 embeds canonical protected WOZ1/WOZ2/FLUX media without host paths; preserves quarter-tracks and timing and reads v1 sessions. | `session_snapshot`, WOZ loaders/timing/write-back and MacBinary regressions |

SNES MAX references: MAME
[`snesmax.cpp`](https://github.com/mamedev/mame/blob/master/src/devices/bus/a2bus/snesmax.cpp)
and [`joypad.cpp`](https://github.com/mamedev/mame/blob/master/src/devices/bus/snes_ctrl/joypad.cpp).
The port covers standard SNES joypads, not every device on the SNES connector.
W5100 reference: WIZnet W5100 datasheet Sn_CR/Sn_SR/interrupt semantics,
as already named in `W5100Device.h`.

## Still outstanding

| Work | Concrete remaining requirement |
|---|---|
| Complete sessions | A usable v2 bundle (v1 compatible) now restores supported cards and embedded read-only Disk II disks, including WOZ/FLUX on a matching II/II+/IIe configuration, with desktop menu/palette and native/path dialogs. Still required: automatic configuration reconstruction, writable/block/3.5-inch media, //c-class sessions, other cards/transports and tapes. Browser session import/export also remains open. `.pom2snap` keeps its separate CPU/RAM contract. |
| Development tools | Bank-aware breakpoints/watches/profiles, source mappings, assembler/IDE and richer script integration. Current profiling is by 16-bit PC and can combine different banks. |
| Speech/audio | Actual TMS5220 Echo+ audio, complete SSI263 inflection/articulation, older SSI263P silicon behavior and physical/reference validation. Phasor dual SSI263AP speech is now implemented separately (see the 2026-10-17 changelog); analog routing fidelity remains open. |
| Video | Complete VidHD/Le Chat Mauve behaviors and beam-timed state; independently captured hardware images. Reproduce old split reports before implementing speculative fixes. |
| Preservation/storage | WOZ FLUX writing, partitioned image/unit mapping, relevant DiskCopy/raw capture formats, CHD and additional historical controllers. |
| Network | LAN/interface/port mapping, libslirp forwarding/TAP/pcap, LocalTalk/AppleShare and self-contained FujiNet; browser transport has separate limitations. |
| Expansion hardware | Saturn/RamFactor, UltraTerm/Second Sight, MIDI/music/sampler cards, SCSI/Corvus/Sider, other coprocessors, digitizers/IEEE-488, light pen/trackball and dongles. SNES MAX alone was added here. |
| Clones | Actual ROM/memory/keyboard/device profiles and regression software for Pravets, TK3000, Base64A, Basis 108, Laser and Franklin variants. |
| Product tools | Multi-machine UI, portable game projects, video/mix recording, user scripting/creative-language tools, and native chooser integration in the remaining export panels. |
| Web/mobile | Touch controls, virtual joystick, browser persistence round trips and measured audio latency. PAL wall-time pacing already exists. |
| Distribution/compatibility evidence | Real MSVC/Windows, Pi/ARM and Intel Mac execution; interactive chooser checks on every OS; signing/notarization; a larger comparable protected-software corpus. |

The ProDOS root's 51-entry limit and per-file 24-bit EOF are filesystem
limits. Recursive subdirectories already exist; they are not a missing
feature. Four host paddle axes, printing and voxel/rewind support also
predate this pass and should not be reintroduced as pending features.

## Validation record

Native macOS build with AppleClang and libslirp: all default targets,
including `POM2`, rebuilt successfully. The full 358-case CTest run took
313.40 s: 341 passed; 16 network/GL cases were blocked by sandbox access;
the remaining ProDOS safety fixture still expected a 200 KB host file to be
hidden. That fixture now uses a sparse 16 MiB file, beyond the actual EOF
limit, preserving its original overwrite-protection assertion.

All 16 named network/GL cases passed outside the sandbox in 39.57 s.
The final targeted pass ran eight cases in 5.61 s, all passing:
`snesmax_card`, `card_snapshot_contract`, `slot_configuration_coordinator`,
`debugger`, `prodos_volume_smoke`, `prodos_decode_safety`, `bench_identity`
and `drive_rack_interaction`. It includes the final accelerated-latch and
fractional-snapshot changes. Across these executions every registered case
passed; this is a combined result, not one sandbox-free full-suite run.

An independent Release build using `POM2_PORTABLE_TESTS_ONLY=ON` built and
passed all eight selected cases in 8.66 s on macOS. MSVC execution is still
pending; the Windows workflow now runs that same selection.

`git diff --check`, `tools/check_includes.sh`, `tools/check_file_sizes.sh`
and the CTest skip guards passed. Logs for this session are under
`/tmp/pom2-parity-*.log`; they are not permanent fixtures.

No sanitizer, Windows/Pi/Intel-hardware run, new physical-card capture or
browser interactive run is claimed. Native macOS chooser code compiled;
interactive open/save/cancel still needs platform evidence. The Linux
external-helper protocol test is registered only there and was not run on
this macOS host.

### Follow-up: snapshot environment identity

The native `POM2` application rebuilt successfully. Seventeen targeted
regressions passed: ten environment/identity/rewind/TransWarp/fuzz cases in
5.36 s, then seven snapshot I/O, memory, paging, MIG, disk-path and partial
apply cases in 4.06 s. The independent portable Release build passed all
nine selected cases in 4.18 s; its Windows CI target list now includes the
new environment test. This follow-up did not rerun the full 359-case suite
or execute MSVC/Windows.

Include declarations, file-size limits, CTest skip guards and
`git diff --check` passed. Temporary logs are
`/tmp/pom2-session-foundation-*.log` and `/tmp/pom2-session-portable-*.log`.

### Follow-up: non-mutating memory and //c device validation

MEX preflight no longer applies a candidate to live memory or restores a
validation checkpoint. Geometry and device sections are checked first;
malformed direct Memory, //c profile and external-port loads also leave
state untouched. Torn ROMBANK, XSP and S35 tails are refused instead of
being mistaken for complete legacy layouts. The detached port decoder lives
on the heap because its IWM contains a 512 KiB buffer; the portable test
fixture likewise avoids exceeding MSVC's default stack. No media/session
container or SLOT restoration was added in this follow-up.

The current native `POM2` rebuilt successfully. All 21 targeted native
regressions passed across two final runs: five preflight, //c SmartPort,
MIG/hub and snapshot-fuzz cases in 9.97 s, then sixteen snapshot, rewind,
clock, mouse, accelerator and disk-path cases in 11.02 s. The independent
portable Release build passed all ten cases in 8.22 s. The new preflight
test covers every nested section, malformed geometry, core/section/tail
truncation, repeated inert validation, successful restore and preservation
of IRQ, DMA, pending video events and the external-port transaction.

Include declarations, file-size limits, CTest skip guards and
`git diff --check` passed. Logs are `/tmp/pom2-session-preflight-*.log`.
This is targeted macOS evidence, not a full 360-test run, MSVC execution,
browser execution or physical-device validation.

### Follow-up: resumable read-only sessions v1

The native `POM2` application and session/CLI/HTTP targets rebuilt successfully.
The three session/parser/runner cases passed in 0.74 s; the three HTTP
regressions passed outside the socket-restricting sandbox in 2.27 s. The
independent portable Release build passed all eleven selected cases in
2.37 s, including the new session test. Windows CI builds the same eleven
targets; MSVC execution is not claimed here.

`session_snapshot` restores CPU/RAM, Mockingboard A/C and Sound II, SNES MAX
and embedded Disk II media after deleting the source image. It compares
the resumed read stream with both the P6 bit-level sequencer and legacy
nibble path, and checks all bytes/track lengths for 13-sector and NIB media.
Damage, malformed clocks/timers, wrong ROM/card topology and outgoing dirty
disks are refused without changing the live machine. Failed saves preserve
the previous bundle; unsupported cards and armed cassette recording are
refused. CLI ordering and actual HTTP save/load requests are covered too.

Include declarations, file-size limits, CTest skip guards and
`git diff --check` passed. Temporary evidence is under
`/tmp/pom2-session-v1-*.log`. This follow-up did not run the full 361-case
native suite, a sanitizer, browser execution or physical-device comparisons.
Writable/WOZ/block/3.5-inch media, //c, other cards, automatic reconstruction
and GUI session controls remain outside this first supported bundle format.

### Follow-up: desktop session workflow

The native macOS `POM2` application rebuilt with the File-menu/palette and
session-dialog integration. Five targeted regressions passed in 1.16 s:
`session_file_picker`, `session_snapshot`, `drive_rack_interaction`,
`cli_kiosk` and `cli_runner`. The new test drives actual ImGui button events
and the real session file/controller adapter, with a controlled native
chooser response so it needs no OS window. It checks cancelled selection,
extensionless save, cancelled and accepted overwrite, actual CPU restoration,
corrupt input preserving the CPU and the path fallback after backend failure.

Include declarations, file-size limits, CTest skip guards and
`git diff --check` passed. Logs are `/tmp/pom2-session-gui-*.log`.
Interactive native chooser execution, Windows/Linux GUI runs and browser
session import/export are not claimed. The eleven-case portable core subset
is unchanged and was not rerun for this frontend-only follow-up. The full
362-case native suite was not rerun.

### Follow-up: protected WOZ/FLUX sessions and encoding repair

Session captures now write v2; v1 nibble archives still load. Both drives
retain the canonical WOZ payload and restore with permanent physical write
protection, including when the original notch came from the host file.
The ordinary image loader and session decoder share an in-memory WOZ parser;
the former no longer reopens WOZ files after format detection. The archive
remains capped at 32 MiB, each WOZ payload at 16 MiB and expanded media
storage at 128 MiB across the staged slot images.

The native application rebuilt. Ten targeted native regressions passed in
5.44 s: session, GUI picker, CLI parser/runner, MacBinary, WOZ load/timing,
flux writes, write-back and nibble-write tests. The independent portable
Release build passed all eleven cases in 21.88 s. Extended session tests
remove both drives' source, compare resumed reads and full bit/flux/period
views across all 160 quarter-tracks for WOZ1, WOZ2 and FLUX, exercise
nondefault timing and host-notch-only protection, recapture byte equality,
v1 compatibility, records above the old 1 MiB limit and invalid nested WOZ
with a valid outer checksum.

The drive-menu separator/ellipsis and hard-reset hint had doubly encoded
UTF-8. Their source strings are repaired; the rebuilt executable contains
the correct hint and no corrupted separator/ellipsis strings. An already
running process needs a relaunch to use them. No interactive OS chooser or
display screenshot comparison was performed.

Include, size, CTest skip and whitespace guards passed. Logs are
`/tmp/pom2-session-woz-*.log`. This is targeted macOS evidence, not the full
362-case suite, MSVC execution or a new protected-software/hardware corpus.
Writable-session persistence, //c, block/3.5-inch media, other devices and
automatic configuration reconstruction remain open.
