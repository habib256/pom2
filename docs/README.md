# POM2 documentation

The reference pages describe the current source checkout, including untagged
changes. Downloaded packages follow their own release notes. Dates on audits,
measurements and plans identify the evidence they record; they do not assert
that the current candidate has repeated those runs.

## Using and embedding POM2

| Subject | Reference |
|---|---|
| Installation, controls, machines, media and builds | [User guide](USER-GUIDE.md) |
| Supported session devices, firmware, detached media, automatic resume and refusal troubleshooting | [Session contract](session-format.md) |
| Bank-qualified stops, symbols, profiling and source maps | [Debugger guide](debugger-banks.md) |
| Rendering algorithms and captured examples | [Graphics comparison](graphics_modes_comparison.md) |
| Hardware abstraction and intentional limits | [LLE/HLE matrix](lle_vs_hle.md) |
| Printer identification, CLI and HTTP controls | [Printer detection](printer-detection.md) |
| Installed C++ facade and consumer recipe | [SDK](SDK.md) |
| Profiling and optimization methodology | [Performance](PERFORMANCE.md) |
| Internal architecture and implementation contracts | [Developer reference](../DEV.md) |

The SDK example is [a standalone CMake consumer](../examples/pom2_core_consumer/CMakeLists.txt)
with [a small C++ host](../examples/pom2_core_consumer/main.cpp).
The application session/debugger workflows are separate from that public facade.

## Work, decisions and evidence

[TODO](../TODO.md) owns the action queue and links every domain backlog:
CPU/memory, display, audio, storage, cards, cassette, network, input, UI/debugger,
WebAssembly, architecture, validation and parked projects, plus release gates.
[Scope decisions](decisions/project-scope.md) distinguish core, supported,
frozen and deliberately excluded behavior. A historical bug report in a
backlog requires current verification before being treated as an open defect.

| Evidence | How to read it |
|---|---|
| [Parity dashboard](audits/emulation-parity.md) | Current implementation boundaries with dated reference grades |
| [Non-IIgs implementation report](audits/non-iigs-parity-2026-10-17.md) | Implemented work, remaining contracts and separate dated validation runs |
| [Extension-card review](audits/extension-cards-2026-10-17.md) | Card contracts; later capability corrections are marked explicitly |
| [Software corpus](test_corpus.md) | Manual/integration targets, configurations and observed milestones |
| [Protected-disk runs](audits/protected-disks-2026-10-12.md) | Recorded gameplay milestones, not completed-game or hardware certification |
| [Mockingboard review](mockingboard_mame_audit.md) | Pinned-source comparison and the measurements made during that review |
| [Risk measurements](audits/todo-risk-2026-09-05.md) | Historical coverage/size figures; remeasure before using them |
| [Documentation reconciliation](audits/documentation-2026-10-20.md) | Scope, source checks, corrections and limits of the documentation review |

The [FujiNet](fujinet_plan.md), [Chat Mauve](chatmauve_plan.md),
[first printer](printer_plan.md) and [second printer](printer_plan_2.md) plans
retain design rationale and original proposals. Their status notes and current
DEV/domain entries distinguish completed work, bounded research and proposals.

## Distribution and maintenance

- [Package manifest](../packaging/bundle.manifest), [ROM inventory note](../packaging/roms_README.txt),
  [curated disks](../packaging/disks/README.txt) and [Raspberry Pi guide](../packaging/raspberry/README.md).
- [Host-folder instructions](../prodos_folder/README.txt), [Floppy Emu folder](../floppyemu/README.txt)
  and [mechanical sound samples](../roms/floppy_samples/README.txt).
- [Contributing](../CONTRIBUTING.md), [security policy](../SECURITY.md),
  [third-party notices](../THIRD-PARTY.md) and [implementation index](../CLAUDE.md).
- [Release notes](releases/README.md) describe tagged packages;
  [archived documents](archive/README.md) preserve earlier evidence.
  [CHANGELOG](../CHANGELOG.md) records implemented changes.

The French [machine/profile quiz](../resources/fr/qcm_apple_ii.md) contains
code-facing teaching material. The [people/history quiz](../resources/fr/qcm_histoire_createurs_apple_ii.md)
and its [image credits](../resources/fr/apple_ii_history_assets/CREDITS.md)
are educational resources, not emulator feature specifications.

When changing behavior, update the user-facing page, its technical/domain
contract and affected integrated help together. Use the actual source tables
(`SystemProfile`, `SlotCardCatalog`, `RomCatalog`, session schemas and public
headers) rather than copying an old audit's counts or pending-feature list.
