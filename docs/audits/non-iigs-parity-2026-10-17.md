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
| Windows test entry point | `POM2_PORTABLE_TESTS_ONLY` builds eight core regressions without configuring POSIX-only harnesses. Windows CI now builds/runs this subset with assertions in Release. | Local native build/run of the same subset; MSVC execution still pending |

SNES MAX references: MAME
[`snesmax.cpp`](https://github.com/mamedev/mame/blob/master/src/devices/bus/a2bus/snesmax.cpp)
and [`joypad.cpp`](https://github.com/mamedev/mame/blob/master/src/devices/bus/snes_ctrl/joypad.cpp).
The port covers standard SNES joypads, not every device on the SNES connector.
W5100 reference: WIZnet W5100 datasheet Sn_CR/Sn_SR/interrupt semantics,
as already named in `W5100Device.h`.

## Still outstanding

| Work | Concrete remaining requirement |
|---|---|
| Complete sessions | Versioned bundle of configuration, firmware/media identity, full card state and coherent media contents; restore validation before any live mutation. Existing `.pom2snap` files still exclude slot state. |
| Development tools | Bank-aware breakpoints/watches/profiles, source mappings, assembler/IDE and richer script integration. Current profiling is by 16-bit PC and can combine different banks. |
| Speech/audio | Actual TMS5220 Echo+ audio, Phasor speech chips, complete SSI263 inflection/articulation and physical/reference validation. Existing local Phasor routing corrections are separate from this pass. |
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
