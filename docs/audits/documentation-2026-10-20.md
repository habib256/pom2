# Documentation reconciliation — 2026-10-20

Reviewed against base revision `a31980a` plus the existing working-tree
implementation changes. The CMake version is 0.9.7; the checkout includes
untagged session/debugger work. This review updates descriptions and help,
not the emulator's behavior or test assertions.

## Scope and document ownership

The [documentation index](../README.md) is the entry point. Review includes
repository manuals, policies, plans, backlogs, audit status, distributed text
instructions, code-facing educational material and integrated help. Third-party
license texts and imported reference material retain their attribution.

| Documents | Source/contract used for reconciliation |
|---|---|
| Root README, USER-GUIDE, CLAUDE, DEV | `SystemProfile`, `SlotCardCatalog`, `RomCatalog`, CLI/controller/UI implementations and package manifest |
| session-format, debugger-banks, SDK | Session schemas/reconstruction, debugger/symbol/source-map code and `include/pom2/core.hpp`; installed SDK consumer |
| graphics_modes_comparison, lle_vs_hle, mockingboard_mame_audit | Display mode enum/renderers, abstraction catalog, AY/VIA/SSI/TMS code and registered regression targets |
| PERFORMANCE | Current benchmark/build interfaces; measurements remain tied to their recorded campaigns |
| printer-detection, printer plans 1/2, fujinet_plan, chatmauve_plan | Current card/transport/printer interfaces, implementation status and deliberately incomplete hardware paths |
| TODO and every domain backlog | Current implemented paths and test registration; old bug reports retain explicit verification requirements |
| project-scope and parity/card/non-IIgs reports | Catalog scopes, implemented capabilities, supported formats and separately dated validation runs |
| Protected-disk and risk reports | Preserved dated software observations/measurements, linked from current validation/planning |
| CONTRIBUTING, SECURITY, THIRD-PARTY, issue template | Build/test entry points, parser/session boundaries, manifest inventory and local attribution notices |
| Package ROM/disks/Pi notes; Floppy Emu, host-folder and sample READMEs | Actual probe/filter tables, volume builder, mechanical-sound wiring and release/native-Pi scripts |
| French machine/profile quiz | Nine-profile enumeration, machine configuration and firmware package contents; question/answer consistency |
| French people/history quiz and image credits | Educational scope and local references; biographical facts and external image licenses were not independently re-researched |
| Release notes and archives | Explicit historical scope via their indexes; old results are not rewritten as current test results |

Machine-readable inputs such as benchmark goldens, coverage floors, media
checksums and deduplication manifests are data, not prose to rewrite. Historical
test-selection lists remain evidence of their original runs.

## Corrected discrepancies

- Sessions: v7 reconstruction, embedded firmware/devices, writable detached
  floppy/block media, host-folder block copies and program tapes; distinct
  `.pom2snap` and rewind contracts; persistent Disk II drive-2 startup paths.
- Debugging: bank-qualified stops, watches, symbols, profiling and `.p2map`
  correspondence; actual remaining native-debug-info/variable/Z80 limits.
- Hardware/audio/video: Echo+ LPC, Phasor dual speech, Slinky, Zip Chip, S.A.M.,
  SNES MAX, VidHD, current ROM/test coverage, mute continuity, PAL pacing,
  capture-writing boundaries and multiple images versus image partitions.
- Interfaces: native/session choosers, partial OS associations, existing docking
  presets, shared duplicate-card policy, HTTP session routes, accurate CLI
  action ordering and host-dependent speed.
- Distribution: three photographs, separately attributed retro artwork, four
  curated disks, current ROM examples, Pi/GLES package distinctions and
  single-threaded browser build/link dependencies.
- Previously overlooked text: the host-folder README no longer describes a
  read-only/flat/128-KiB prototype; 3.5-inch mechanical samples are no longer
  described as unused; Floppy Emu lists current filters; the teaching quiz
  no longer claims eight profiles or absent packaged firmware.
- CI descriptions now include native tests, the Windows subset and Chrome
  session/persistence checks. Printer controls distinguish internal C++
  interfaces from the installed SDK and describe log/spool cursor behavior.
  CPU coverage counts refer to matching corpus files, including unofficial
  NOPs, rather than mislabelling all 178 as documented instructions. The old
  file-size failure is explicitly historical; the current guard passes.
- Build instructions separate the Windows application from the portable test
  subset, use generator-independent probe commands and keep launch commands
  at the repository root. Pin/panel/probe descriptions defer to source tables
  rather than obsolete counts.
- Printer sound references now include the optional local Virtual ][ loop
  alongside the default synthesis. Corpus entries name the actual diagnostic
  target and CTest registrations, distinguish historical observations from
  current evidence, and account for the //c Mockingboard 4c expansion option.
- Cassette documentation drops the obsolete CLI suffix defect and states
  the current ACI/WAV precedence. SmartPort header documentation describes
  the implemented 2–14-unit chain, eight-unit default and ROM overlay;
  card/expansion-window descriptions include Slinky and the actual //c SSCs.
- Performance instructions use runnable curated-media paths rather than shell
  redirection placeholders. They specify instruction-boundary cycle overshoot,
  the main-RAM hash range and optional floating-point framebuffer hashes.
  They distinguish headless capture mode from interactive operation; benchmark
  help states the actual ROM probes, cycle budget and RAM-hash range.
- Package notes and nearby source comments describe the current browser boot
  (`iic-pal`, `bundled_disks/DIX-fix.po`) and the curated disks shared by desktop
  and browser builds. URL examples encode the II+ name correctly and explicitly
  disable the boot image when requested. Removed/external script references
  are identified as historical or belonging to another checkout.
- SDK media descriptions follow `SmartPort35Unit`/`Disk35Image`, including
  raw/2IMG 400/800 KiB and protected GCR captures. The facade's two-unit,
  non-MFM setup is distinguished from the application chain. Initialization,
  bus access, boot-failure side effects, multi-Mockingboard audio and direct
  cassette-save suffix behavior are documented alongside the public header.
- FujiNet backlogs distinguish the implemented timeout slider/remote-call
  budget from pending peer/TNFS and responsiveness evidence. HTTP findings
  distinguish existing CPU-register validation from the screenshot-method
  policy and the separate disk/persistence status fields.

## Validation record

The current native macOS core library rebuilt. The SDK was installed into an
isolated temporary prefix; the standalone consumer configured, built and ran
against it using `find_package(pom2_core CONFIG REQUIRED)`. Output:

```text
PC=F800 cycles=35 RAM[0300]=42 frame=280x192 audio=256
```

The SDK recipe is in [SDK.md](../SDK.md#building-a-consumer). This verifies the
local consumer procedure, not a Windows/Linux SDK execution matrix.

The native `pom2_imgui` application rebuilt successfully; `POM2 --version`
reported v0.9.7 and `POM2 --help` exited successfully with the revised help.
Local file and heading references across all 69 Markdown documents passed.
Version-string, file-size and whitespace checks passed, as did shell syntax
checking for `build_wasm.sh` and SHA-256 verification of the four bundled
disks. No full CTest suite or browser/platform matrix was rerun.

A fresh temporary macOS configuration with `POM2_ENABLE_TESTS=ON` and
`POM2_PORTABLE_TESTS_ONLY=ON` succeeded. CTest JSON discovery registered the
expected twelve cases, including `debugger` and `session_machine`; this checks
the documented subset selection, not MSVC compilation or test execution.
Shell syntax checks for `setup_imgui.sh`/`imgui_pin.env` and workflow pin checks
also passed.

The current `pom2_bench` rebuilt and all four documented sample workloads
completed with protected media: ROM banner, curated Disk II boot, OE-CPU
rendering and IIe paging. OE-CPU reported `fb=<fp-dependent>` as documented.
Bundled-disk SHA-256 checks passed after these runs. No callgrind campaign,
PGO training or historical timing comparison was performed.

Four URL examples were evaluated against the argument-construction code in
`wasm/shell.html`: default boot, profile-only override, II+ without a disk and
explicit profile/disk selection. All matched. This checks shell argument
selection, not a new browser emulator run. Manifest/headless/shell changes in
this follow-up are comments only.

The final documentation pass rechecked all 69 Markdown files' local paths and
heading references with no broken references. All six contributor source guards
passed: file sizes, version strings, settings keys, workflow pins, coordinator
lock scopes and includes. Whitespace checks also passed. The changelog entry
summarizes the resulting documentation contracts; detailed evidence stays here.

## Integration checks before commit

After the request to commit and merge all working-tree changes, the seven
affected regression targets were rebuilt and passed through CTest:
`debugger`, `session_machine`, `session_snapshot`, `session_iic`,
`session_writable_media`, `session_block_media` and `pom2_core_api`.
This is a focused integration run, not the full native or browser suite.

## Remaining evidence and limits

### Follow-up against merged revision `d2f0140`

A further source comparison corrected the omitted session actions in DEV's CLI
inventory, the VSDrive settings/session ambiguity and the contributor recipe's
working-directory change. Session instructions now identify unsupported default
desktop cards, idle built-in versus expansion serial ports, SmartPort-based
card coverage, save suffixes and recovery from capture/load refusals. The
first-launch guide explains Ctrl-Reset when an empty drive keeps booting.
Sources checked include `SessionIIc.cpp`, `SessionMachine.cpp`, `SessionFiles.cpp`,
`SuperSerialCard.cpp`, `SlotConfigurationCoordinator.cpp`, `BrowserSessionStore.cpp`
and the CLI/UI settings paths.

All six contributor source guards passed again, as did local Markdown path and
heading checks and `git diff --check`. These were documentation-only changes;
no new emulator build, CTest execution or browser run was performed in this pass.

The follow-up also compared `ResourcePaths.cpp`, `Settings.cpp`,
`include/pom2/core.hpp`, `Pom2Core.cpp`, CMake's minimum version and the Pi
workflow/build/training scripts. Configuration/output directories, ROM search
precedence, SDK defaults and buffer/index conventions, Pi filename tags,
PGO workload selection and benchmark examples were aligned with these sources.
The Pi tarball still needs a graphical session; it installs no display server.

### Follow-up against revision `b9bfa84`

Compared the debugger manuals with `EmulationController::runCpuSlice`,
`stepBusMaster`, `debugResume`, `debugStepOver`, `Debugger`, `Debugger_ImGui`
and `MemoryCpuView`. DEV still described the now-corrected DMA-release stop
defect, single-stepping the parked 6502 during Z80 DMA, unqualified-only
watches and a main-RAM-only Step Over. Those descriptions are superseded.
The user/debugger guides now describe run controls, return-frame tracking,
one-instruction resume handling, transient cancellation and the distinction
between stepping the active bus master and instrumenting the 6502.
Inspection documentation also identifies mirror fallbacks for soft switches,
MIG windows and cards without side-effect-free peeks.

The existing `debugger` regression includes
`testBreakpointAfterDmaRelease`, which checks stopping before INX, toolbar
resume and idle-hook detachment. This follow-up changes documentation only;
it does not claim a new execution of that regression or a new emulator build.
All six contributor source guards passed, along with `git diff --check`.
The 131 local file/heading references in the five edited Markdown files
were checked successfully.

### Broader storage, hardware and interface follow-up

Source comparison against the same revision extended beyond debugging:

- `ProDOSHardDiskCard::buildRom` uses `$Cn07=$3C`; the abstraction seams and
  parity dashboard still described the superseded `$01` autostart limitation.
  `hdv_boot_scan` contains the real-ROM regression for the unenhanced IIe.
- `IIcClassProfile::servesExternalSmartPort`, `Memory::memRead` and
  `bootFromSlot` distinguish the 16 KB //c stub from the 32 KB //c and //c+
  firmware/bus paths. DEV still assigned //c+ HDV boots to the stub.
- `classifyDiskForSlot`, the Library filters and `Disk35Image::loadFile`
  establish the documented routing and raw/2IMG/WOZ/MOOF limits. DiskCopy
  4.2 header/tag decoding is absent. The storage format-documentation task
  is now closed, with its anchor retained.
- `MediaWritePolicy`, `SmartPortHdvUnit`, `SlotCardCatalog`,
  `MainWindow_AudioPanels` and `PrinterSoundDevice` establish writable
  defaults, the remaining host-folder opt-in, picker labels and optional
  local printer recordings. Scope decisions remain unchanged.
- `SpOverSlipLink::transact` distinguishes a closed transport from a silent
  connected peer, local versus remote call budgets and the consecutive-timeout
  disconnect. The uniform 250 ms descriptions in the parity/seam tables are
  superseded; existing outer budgets may shorten a call.
- Keyboard dispatch and the Machine menu distinguish F12 hard reset from
  **Cold boot (wipe RAM)**. README and user controls now use those names.
  Browser instructions distinguish session/text import from the still-missing
  direct disk-image upload flow.

These are source and existing-test comparisons, not new emulator execution
or platform-validation results.
All six contributor source guards passed again, as did `git diff --check`.
Local file and heading checks passed for all 241 references in the eleven
edited Markdown files, including the retained STORAGE-011 anchor.

### HTTP and integrated-help follow-up

Compared DEV's endpoint inventory and `AiControlServer.h` with every registered
route and its handler. Removed the superseded register-masking claim, split
memory reads from RAM-only writes, documented response framing, request/path
limits, speed budgets, mouse variants and missing run-control routes. The
examples use decimal addresses and the actual query/body split. The user
guide and documentation index link to that reference.

Welcome help now distinguishes browser session import from desktop image
drop/CLI workflows and lists current 3.5-inch geometries. Abstraction Levels
help distinguishes the 16 KB //c fallback from real //c-family SmartPort
firmware, the separate Liron implementation and //c+ reset boot. FujiNet help
accounts for locally answered controller status. `Debugger.h` comments now
describe current lock ownership, watch diversion and frame-based call stepping.
These changes affect prose, comments and displayed help, not emulation logic.

Native C++17 syntax checks passed for `AbstractionLevels_ImGui.cpp` and
`MainWindow_MiscPanels.cpp`; the latter used a temporary `Version.h` expanded
from the checked-in template and current CMake version. No executable link,
interactive help walkthrough, browser build or HTTP endpoint execution is
claimed by these checks.
The six contributor source guards and whitespace check passed. File/heading
checks across all 71 tracked Markdown files passed for 888 local references.

### Graphics and distributed Floppy Emu instructions

Compared `FloppyEmuDevice::parseFavorites`/`acceptsFile` and
`MainWindow::renderFloppyEmuWindow` with the SD-folder README, DEV, user guide
and header comments. The parser records `automount`, but no UI/startup caller
uses it; selecting an image manually mounts and boots it. Favorites are
bounded to the SD root after both lexical and canonical checks. Dumb 3.5-inch
mode routes to the //c+ external Sony, rather than Disk II; intelligent modes
use the storage coordinator. The documentation now records these distinctions
and the current WOZ/MOOF extension filters.

Compared the graphics comparison with `effectivePhosphorDecay`,
`textLitColor`, `Apple2Display::render` and the AppleWin renderer. Monochrome
graphics retain 560 dots, text has tint without persistence, green/amber decay
follows emulated frames and repeated paused repaints preserve history.
AppleWin TV skips an invalid prior frame. The old broad 60 FPS claim
contradicted its own historical 25 ms OE-CPU row and has been removed;
no new timing measurements are implied. A stale test comment about green
having no persistence was corrected without changing test logic.

The existing `floppy_emu_smoke_test.cpp` was compiled with
`FloppyEmuDevice.cpp` as an isolated C++17 executable and passed all five
groups: mode identities, format filters, root-bounded navigation, favorite
parsing and symlink containment. All six contributor guards and whitespace
checks passed again. Across 71 tracked Markdown files, all 891 local
file/heading references passed. No display render or timing campaign was
rerun in this follow-up.

### Input, cassette and deferred CLI follow-up

Compared `JoystickInput` and `pollJoystickAndPushToMemory` with DEV's stale
claim that paddles 2/3 were always centred. The reference now describes
mapped right-stick axes, raw-device fallback and the user guide's button
mapping. `CassetteDevice::loadTape`, its WAV decoder, deck controls and
`Memory`'s //c I/O fences establish the new cassette instructions: supported
input/output formats, armed playback, capture clearing, reset preservation
and the difference between sessions and CPU/RAM snapshots/rewind.

`runDeferredActions` calls `requestStep` and proceeds immediately. The CLI
reference/help now distinguishes ordered submissions from completed guest
instructions; a later save is not a post-step barrier. `runLoad` validates
address-space/I/O boundaries and checks Language Card read-back without
rolling back prior writes. The guide records that limit and the CLI/HTTP
speed-bound distinction. Tape-format help follows `resolveSaveTapePath`.
Settings references in DEV and nearby comments now use the filename from
`Settings::resolveStorePath` rather than obsolete JSON/CFG spellings.
Native C++17 syntax checks passed for `CliDispatcher.cpp` and
`MainWindow_SettingsPanels.cpp`. The six contributor guards, whitespace check
and all 891 local references across 71 tracked Markdown files passed again.
No new gamepad, cassette, CLI-runner or browser execution is claimed here.

### ROM selection and browser durability follow-up

Compared `SystemProfile.cpp`, `findFirstResource`, `findResource` and
`switchProfile` with the user guide and distributed ROM note. Filename
preference is the outer loop; directory precedence is applied per filename.
The guide now lists all profile main-ROM candidates and explains why a
generic per-user fallback cannot override a preferred packaged filename.
An invalid existing candidate refuses a profile switch rather than trying
the next alias. Header comments also record the actual temp-output fallback
placement in the asset search order.

Compared `wasm/shell.html`, `PersistentFs` and `BrowserSessionStore` with
DEV and the session contract. Preloaded MEMFS resource paths are independent
of `/persistent`; the IDBFS mount is not an extra ROM search root. Storage
startup failures can leave the emulator running without recovered durable
state. Automatic resume alternates two archives and publishes their index
and running mode in a marker; filesystem publication is distinct from the
asynchronous IndexedDB flush. Corruption suspends resume instead of selecting
the other archive. These are source checks, not new browser execution.
The six documented main-ROM candidate lists were compared programmatically
with `SystemProfile.cpp` and matched in order. All six contributor guards
and `git diff --check` passed. No new profile-switch or browser test run is
claimed in this follow-up.

### SDK and FujiNet service-boundary follow-up

Compared the SDK prose and public header comments with `Pom2Core`,
`Memory::loadAppleIIRomImpl` and the CPU reset/halt paths. The public loader
accepts the 20 KiB combined layout as well as its documented 12/16/32 KiB
forms, plus best-effort 2–64 KiB inputs. SDK attachment/replacement and boot
entry semantics are now explicit. Reset comments distinguish preserved A/X/Y
from changed PC/P/SP; elapsed cycle counts can include halted idle steps.
A stale constructor comment claiming the Language Card was unmodelled was
corrected without changing memory logic.

Compared `FujiNetCard::serveBuiltInNetwork`, `answerDeviceCount`,
`FujiNetNetDevice` and startup slot settings with the user guide, DEV and
abstraction/design references. Controller count status is local, and desktop
settings enable the local plain-HTTP `N:` service by default. Disks/CONFIG/
clock remain peer-backed; the external firmware's full network services need
the local override disabled. Historical relay proposals remain labelled as
such. No new remote networking or SDK runtime execution is claimed here.
The standalone consumer passed a native C++17 syntax check against the public
header. All six contributor source guards and `git diff --check` passed.

### Memory viewer follow-up

Compared the viewer controls, search loops, edit history and coordinator
write callback with the debugger guide. The guide now distinguishes the
CPU read view from the write target, describes forward-only hex/ASCII
search and records that Undo/Redo replay values through the current mapping
rather than restoring bank selection or device state. This is a source
comparison; no new interactive viewer execution is claimed.

### Orientation reference follow-up

Aligned `CLAUDE.md` with `CliRunner`, `classifyDiskForSlot`, the reset paths
and the browser session implementation. Its index now includes
`BrowserSessionStore`; its summaries distinguish queued steps from completed
execution, the //c+ reset scan from direct //c slot-ROM entry, and kiosk
settings suppression from disk protection. The reset vector is identified
as the source of PC, and the media routing summary includes all three raw
3.5-inch geometries. These changes affect documentation only.

### TNFS cache and transfer follow-up

Compared `TnfsMedia`, `TnfsFetchLimits` and the positional startup path in
`main.cpp` with DEV, the user guide and header comments. Corrected claims
that cache hits validate remote size or that transfers have only per-request
timeouts. The documentation now explains offline cache reuse, local-only
guest saves, explicit refresh, the 32 MiB download cap, the default 60-second
deadline and interrupt semantics. Pruning follows new downloads rather than
cache hits, with a 512 MiB budget and mtime refresh on reuse. No network
execution or new transfer timing measurement is claimed in this follow-up.

### Headless command reference follow-up

Compared `pom2_headless` argument parsing, capture selection, boot and setup
paste with DEV and usage text. Documented screenshot-only 300-frame capture,
`--frames 0`, output-before-flat-colour-failure ordering, interactive SSC
defaults and the distinction between selecting a ROM file and a machine
profile. Removed comments claiming packages ship no disks or reset enters
`$C600` directly. Help/comments changed without changing execution logic.
The translation unit passed a native C++17 syntax check; all six contributor
guards and `git diff --check` passed. No new linked headless capture or
interactive telnet run is claimed.

### Public keyboard input follow-up

Compared `Core::queueKey`/`pasteText`, the `Memory` wrappers and
`Keyboard::pushChars`/`readLatch` with the SDK contract. Documented latch
replacement versus queued paste, normalization/transliteration, pending-input
capacity and the distinction between accepted output characters and consumed
UTF-8 input bytes. Corrected the internal comment that attributed promotion
to strobe clearing rather than the following latch read. These are prose
and comment changes; no keyboard implementation change is included.

### Public audio output follow-up

Compared the public audio comments and SDK guide with `Core::pullAudio`,
`Core::pullSpeakerAudio`, `SpeakerDevice::fillAudioBuffer` and the shared
`AudioMix` law. Removed the unsupported amplitude-bound promise for the
direct speaker source, while retaining the final stereo mixer's clamp.
Documented full requested-frame returns, invalid-input zero returns,
CPU scheduling ownership and fixed master-mixer defaults. This is a source
comparison, not a new amplitude measurement or audio playback test.

### HTTP input and framebuffer follow-up

Compared `handleKeyboard`, `Keyboard::pasteRawKeys` and `handleScreen` with
the HTTP reference. Documented text-before-raw ordering, normalized output
counts, shared pending-input capacity and raw 7-bit semantics. The screen
contract now specifies P6 RGB, native dimensions, missing-display refusal
and the OE-GPU CPU capture path. No new HTTP execution, screenshot comparison
or display-fidelity measurement is claimed.

### Raspberry Pi tuning instructions follow-up

Compared the deployment guide with `pi_tuning.sh` installation and uninstall
branches. Replaced the claim of fully automatic reversal with the actual
service removal, whole-file boot-argument backup restoration, unchanged live
governor and retained swap configuration. Documented `--no-irq-pin`, existing
IRQ-argument preservation and the dphys-swapfile-only scope. The system script
was read, not executed; no host configuration or Pi hardware test changed.

### Local packaging instructions follow-up

Compared Linux/macOS/Windows packaging entrypoints and CPack filename rules
with the user guide. Documented build-versus-staging responsibilities,
case-sensitive output directories, optional Linux formats, architecture
selection and desktop staging dependencies. Updated Linux script help
comments that still referred to a separate release-infrastructure branch
and an x86-only AppImage name. No packages were built or published in this
follow-up, and no downloads or signing operations were performed.

### Standalone snapshot format follow-up

Compared `SnapshotIO` constructors/header parsing and transactional
`restoreMachineState` with format comments and the user guide. Corrected
the current 17-byte CPU section, version range, rewind-only SLOT handling
and temporary-file publication contract. Documented the v1 missing-MEX
compatibility boundary, v2 refusal, CPU-silicon check and the separate
64 MiB snapshot-file read cap. No new snapshot runtime or crash test is
claimed in this follow-up.

### Final working-tree consistency review

Rechecked all 891 local references across 71 tracked Markdown files; target
files and Markdown anchors resolved. External URLs were not fetched.
Compared the changed C++ files against HEAD with comments/whitespace removed:
17 files matched; the five integrated-help files were reviewed separately
for wording and the browser-specific Welcome text. The test-file change is
a comment only. `git diff --check` passed. This review does not add a
full-suite or cross-platform runtime result.

The review does not establish new physical-card/display/audio fidelity,
successful remote CI runs, execution on Windows/Pi/Intel Mac, completed
commercial games or photograph redistribution rights. Those remain explicit
tasks or limits. Historical performance and coverage numbers were not
remeasured. Remaining hardware/IDE/browser features are documented as such;
the documentation changes do not implement them.
