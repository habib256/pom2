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

The review does not establish new physical-card/display/audio fidelity,
successful remote CI runs, execution on Windows/Pi/Intel Mac, completed
commercial games or photograph redistribution rights. Those remain explicit
tasks or limits. Historical performance and coverage numbers were not
remeasured. Remaining hardware/IDE/browser features are documented as such;
the documentation changes do not implement them.
