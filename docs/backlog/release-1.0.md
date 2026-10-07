# Backlog — Release 1.0

[Planning index](../../TODO.md) · [Scope decisions](../decisions/project-scope.md)

Reorganised 2026-10-05. Past gate completions remain in the
[historical report](../archive/todo-2026-10-05.md#the-road-to-10).
Old estimates and CI claims require revalidation. Release checks are rerun
against the candidate build.

## What 1.0 means

1. README, Welcome and the card catalog describe the shipped build accurately.
2. The core, supported and frozen scope is explicit, with deliberate limits recorded once.
3. The release builds reproducibly from a clean checkout and its payload can be distributed under the recorded decisions.
4. No known defect silently loses media, settings or snapshots; the named durability regressions pass.

Fidelity refinements and new card ports remain post-1.0 by default.
The Videx Videoterm already shipped on 2026-09-29; it is not a pending port.
The original rationale and past gate completions remain in the
[historical report](../archive/todo-2026-10-05.md#what-10-means).

<a id="the-road-to-10"></a>

## Remaining gates

| Gate | State from existing evidence | Acceptance criterion |
|---|---|---|
| [G1](#g1) — Distribution | Partial; decisions remain | Record the history-distribution decision and provenance/terms for both photographs. |
| [G2](#g2) — Data durability | Closed in the original report | Keep the named storage, snapshot and reset regressions green on the candidate. |
| [G3](#g3) — Accurate claims | Closed in the original report | Reconcile the candidate README, Welcome and card catalog with shipped behavior. |
| [G4](#g4) — Repeatable packaging | Closed in the original report | A current rehearsal builds all seven packages; build_dist and payload checks pass. |
| [G5](#g5) — Test coverage | Partial | Validate alert delivery, software GL/UI concurrency, browser persistence and libslirp transport. |
| [G6](#g6) — Platform evidence | To verify | Record actual tests or explicit limitations for Windows, ARM/Pi and Intel macOS. |

<a id="g1"></a>

## G1

<a id="release-001"></a>

### RELEASE-001 — Commercial software tracked in git — removed from the work tree 2026-09-05, still in history.

**Priority:** 🟠 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-153).

**Acceptance criterion:** Record an explicit decision on historical media distribution and align the repository and release payload with it.

- ◔ **Commercial software tracked in git — removed from the work tree
  2026-09-05, still in history.** 907 files (286 MB) moved to
  `/Volumes/TEST/pom2-media/`, each copy verified by SHA-256 before the
  original was deleted, with a `MANIFEST.tsv` and a `RESTORE.sh` alongside:
  the whole 4am/Asimov WOZ collection (`disks_5.4/woz`, 757 titles), the
  personal game collection (`disks_5.4/gist`, 109), the commercial half of
  `disks_5.4/dsk` (31), four `hdv/` volumes (**Nox Archaist**, AppleWorks,
  Total Replay II, Wizard Replay) plus three volumes of unestablished
  provenance, five `disks_3.5/` (Oregon Trail, both
  Print Shop, Multiscribe, TheBestGames) and `floppyemu/Total Replay
  v6.1.hdv`. Kept deliberately: the French Touch demos with their sources,
  the Purplesoft / Chat Mauve preservation disks (the `purplesoft_eve_screens`
  oracle), Apple system software, and the author's own projects.
  Full suite re-run after the move: **240/240 green**.
  **What is left, and it is the larger half**: the blobs are still in the
  packfile, so GitHub still serves every one of them to anyone who clones.
  A tip-only deletion does not undo that. *~1 d to rewrite history (306 MB
  packfile), or accept it and document the risk.*
  The three `hdv/` volumes whose contents were never established
  (`2018-01-23 - ProDOS8.2mg`, `Bad.Apple.hdv`, `Mouseapps Apple2.hdv`, 96 MB)
  went the same way on the same day, so `hdv/` now holds only the author's own
  projects. **910 files, 382 MB** in the manifest.

<a id="release-002"></a>

### RELEASE-002 — The two shipped photographs have no recorded provenance.

**Priority:** 🟠 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-194).

**Acceptance criterion:** Record origin, rights holder and redistribution terms for both shipped photographs in THIRD-PARTY.md.

- 🟠 **The two shipped photographs have no recorded provenance.**
  `pic/Apple_II_plus.jpg` (About panel, README) and
  `pic/Keyboard_AppleIIe.jpeg` (the keyboard panel). `THIRD-PARTY.md` says
  so and treats them as all-rights-reserved until the author states their
  origin and terms — one sentence each from the author closes this. *~5 min
  of the author's memory.*

<a id="g2"></a>

## G2

Closed implementation details remain in the [original gate report](../archive/todo-2026-10-05.md#g2--the-three-defects-that-reach-a-users-data--2026-09-06). Use the candidate checks below to validate the release.

<a id="g3"></a>

## G3

Closed implementation details remain in the [original gate report](../archive/todo-2026-10-05.md#g3--make-the-words-true-). Use the candidate checks below to validate the release.

<a id="g4"></a>

## G4

Closed implementation details remain in the [original gate report](../archive/todo-2026-10-05.md#g4--a-release-that-can-be-rehearsed-). Use the candidate checks below to validate the release.

<a id="g5"></a>

## G5

<a id="release-003"></a>

### RELEASE-003 — G5-9 · Notify on a red nightly

**Priority:** 🟠 · **State:** Shipped (verify) — see the 2026-10-06 note below.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-450).

**Acceptance criterion:** Attach evidence that a scheduled sanitizer failure creates an issue and a later failure updates the existing issue.

- ◑ **G5-9 · Notify on a red nightly** *(2026-09-16)*. `ci.yml` job
  `sanitizers-alert` (`needs: sanitizers`, `if: failure()` on `schedule`,
  `issues: write`) opens an issue titled "Nightly sanitizers are red", or
  comments on the open one, with the run URL — `gh` on the runner, no new
  action to pin. The YAML parses and the lookup query was run against the
  repository; **the create/comment path itself has not run**, and will first
  run on the next red night. Close the issue once it has fired once.

**Revalidation 2026-10-06 — acceptance evidence exists (Shipped, verify):**
both paths have run. Issue [#12](https://github.com/habib256/pom2/issues/12)
"Nightly sanitizers are red" was opened by `github-actions[bot]` on
2026-09-17, and the bot commented on it after the red nightlies of
2026-09-28, 09-29, 10-01 and 10-04. The issue is still open — and the
sanitizer legs it reports were red on those nights (green again on 10-05 and
10-06), which is a separate matter from this alert item. No CHANGELOG entry
records the first firing; the 2026-09-16 "G5-9: a red nightly opens an issue"
entry covers the implementation.

<a id="release-004"></a>

### RELEASE-004 — G5-10 · A CI leg with software GL

**Priority:** 🟠 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-457).

**Acceptance criterion:** Record a green Linux software-GL run without skipped rendering checks and drive panel frames while cards are replugged.

- ◑ **G5-10 · A CI leg with software GL** *(2026-09-17, first run
  pending)*. `crt_barrel_view` is a ctest now (skips without GL; its mask
  pitch and bandwidth checks had no pin), and the CI `gl-software` job runs
  it and `crt_glass_resample` under Xvfb + Mesa llvmpipe, failing if either
  skips. Both pass on a local GPU; **the Linux job has not run yet**. Still
  open from this entry: the headless ImGui frame driven while cards are
  replugged (`frontend_device_panel_concurrency`), which this leg makes
  possible.

**Revalidation note 2026-10-06:** the first half has evidence — the CI job
"GL tests on Mesa llvmpipe (Xvfb)" passed on the push runs of 2026-10-05
(`e8f3ce1`) and 2026-10-06 (`ca7abcb`); the job fails on any `Skipped` line
(`.github/workflows/ci.yml` `gl-software`). `frontend_device_panel_concurrency`
still does not exist in `tests/` (owned by
[ARCH-009](architecture-tooling.md#arch-009)).

<a id="release-006"></a>

### RELEASE-006 — G5-13 · The transports at 0 %

**Priority:** 🟠 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-480).

**Acceptance criterion:** Run libslirp transport checks in a CI environment that installs libslirp; skips do not count as passes.

- ◑ **G5-13 · The transports at 0 %** *(2026-09-17, two of three)*.
  `ssc_tcp_transport` opens the Super Serial Card's real listener on
  127.0.0.1 — both directions in raw mode, a reconnect, `stop()` with a client
  attached, a restart on the same port, a port in use — and
  `sp_serial_transport` drives the FujiNet serial transport through a pty —
  refusal with a reason, open/no-reopen, both directions, the read timeout,
  `shutdown()` waking a parked reader, drop and reopen. Both mutation-checked.
  **Still open:** `SlirpNetworkBackend`, which needs libslirp — the CI image
  does not install it, so its one test (`slirp_loopback_fence`) skips there.

**Revalidation note 2026-10-06:** push CI still skips it (allowlisted in
`tools/check_ctest_skips.sh:25`). The release workflow's `quality` job does
install `libslirp-dev` (`.github/workflows/release.yml:144`) and runs the full
ctest, so the test runs at release time — without a skip guard there.

<a id="release-005"></a>

### RELEASE-005 — G5-12 · WASM CI is compile-only.

**Priority:** 🟢 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-473).

**Acceptance criterion:** Boot the browser module, write a setting, persist it, reload and recover the value in an automated check.

- 🟢 **G5-12 · WASM CI is compile-only.** *≈1 d.* The job builds and checks
  three files are non-empty; it never boots the module, never touches
  `PersistentFs`/IDBFS. The browser build **never destroys its `MainWindow`**,
  so the only thing that persists a visitor's state is a 10-second heartbeat. A
  regression that compiles but breaks the mount passes green and surfaces as
  *"my browser forgets everything."* A node smoke — load, run N frames, write a
  setting, `pom2_persist_now()`, reload, assert — closes it.

<a id="g6"></a>

## G6

<a id="release-007"></a>

### RELEASE-007 — Windows ships a binary against which no test has ever run.

**Priority:** 🔴 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-516).

**Acceptance criterion:** Record a passing MSVC core test subset, or accurately document Windows as build-verified only.

- 🔴 **Windows ships a binary against which no test has ever run.** Both
  `ci.yml:230-236` and `release.yml:707` set `-DPOM2_ENABLE_TESTS=OFF`, and the
  CI comment concedes: *"the suite has never been built for MSVC."* README
  lists Windows as a first-class platform. **Either** get a core subset green
  under MSVC (*~1 d for a first 20 headless tests; 2-4 d for the unknown
  portability backlog*) **or** say plainly in README that Windows is
  build-verified only. The first is better; the second is honest; the current
  state is neither.
  *(Line numbers on 2026-10-06: `ci.yml:426-433`, `release.yml:753`; still
  `-DPOM2_ENABLE_TESTS=OFF`.)*

<a id="release-010"></a>

### RELEASE-010 — "It builds here" is a macOS-only statement, and it has now turned CI red twice.

**Priority:** 🟠 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-530).

**Acceptance criterion:** Verify the fast Linux build and define whether a pre-push check and GL-call guard are still needed.

- 🟠 **"It builds here" is a macOS-only statement, and it has now turned CI red
  twice.** Development happens on macOS, where libc++ pulls in transitive
  includes the other two standard libraries do not and Apple's `gl3.h`
  declares desktop GL entry points POM2 otherwise reaches through its own
  loader. 2026-08-22 was the include story; 2026-09-07 was
  `CrtEffectStack.cpp` calling `glDeleteProgram` directly instead of
  `pom2::deleteShaderProgram`, which built clean locally and broke Linux,
  Windows **and** the coverage job (`4c3b97d`). The class is narrow enough to
  grep for — GL names outside `OpenGLShader.h`, POSIX-only calls, missing
  transitive includes — and a Linux container build before pushing costs
  minutes. Worth a pre-push hook or a fast Linux compile-only CI leg that runs
  before the full matrix.
  **◑ 2026-09-17:** a `linux-quick` CI job (GCC, `-Werror`, the three
  application targets, no tests) answers in minutes instead of after the
  full leg; `tools/check_includes.sh` already covers the transitive-include
  class. Not done: a pre-push hook, and a grep for GL calls outside
  `OpenGLShader.h`.

<a id="release-008"></a>

### RELEASE-008 — Linux aarch64 / Raspberry Pi are never built outside a release

**Priority:** 🟡 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-524).

**Acceptance criterion:** Record launch and rendering results for the ARM/Pi packages on the target hardware.

- 🟡 **Linux aarch64 / Raspberry Pi are never built outside a release** and
  never tested at all — three of the seven shipped packages. The rehearsal in
  G4 covers the build; rendering on real hardware cannot be proven by CI and
  should be a checklist line.

<a id="release-009"></a>

### RELEASE-009 — macOS x86_64 slice is never executed

**Priority:** 🟡 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-528).

**Acceptance criterion:** Execute and smoke-test the Intel macOS slice; document any remaining verification limitation.

- 🟡 **macOS x86_64 slice is never executed** — no Rosetta on the runner, so
  the universal binary gets a structural `lipo` check only.

<a id="release-011"></a>

### RELEASE-011 — Notarization / signing.

**Priority:** 🟢 · **State:** To verify / decision pending.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-547).

**Acceptance criterion:** Record a platform signing decision and test first launch on a clean machine under the chosen distribution policy.

- 🟢 **Notarization / signing.** Both macOS and Windows refuse the first launch;
  README documents the workaround. Absent from this file entirely until now. A
  1.0 where two of three desktop platforms show a security warning reads as
  unfinished. *~1 d + $99/yr Apple, $200-400/yr Windows EV.*

## Candidate release checklist

Checks previously completed on a September build are not current candidate
sign-off. Re-run them and attach evidence; checking these boxes never rewrites
the historical completions.

### Legal — all must be YES before tagging

- [ ] No commercial disk images in the work tree AND none
      reachable in git history, or the history risk accepted in writing
- [ ] The web demo boots a disk that exists and is freely licensed (record the actual demo disk)
- [ ] The web demo's boot disk is freely licensed; provenance recorded (GPLv3, THIRD-PARTY.md)
- [ ] The bundled-firmware decision is made, and README + RomStatus_ImGui +
      MainWindow_MiscPanels + packaging/roms_README.txt all agree with the candidate payload
- [ ] THIRD-PARTY.md exists (MAME, AppleWin, Dear ImGui, GLFW, DejaVu,
      Font Awesome, the two pic/ photos — the photos' provenance still owed)
- [ ] fonts/ ships its two license files

### Version

- [ ] CMakeLists.txt project(... VERSION 1.0 ...)
- [ ] docs/releases/v1.0.md written — the filename MUST equal PROJECT_VERSION
- [ ] All version locations listed in CLAUDE.md and vcpkg.json agree with the release;
      tools/check_version_strings.sh passes
- [ ] grep -c 'v0\.9' README.md == 0

### Build repeatability

- [ ] Release rehearsal green within the last 7 days (attach a current run URL)
- [ ] emsdk pinned; debian:bookworm pinned by digest; actions/* pinned by SHA (tools/check_workflow_pins.sh)
- [ ] ghcr.io/habib256/pom2-bionic-builder (pom2's own mirror) pulled by the rehearsal
- [ ] ./build_dist.sh (.deb + tarball) builds on the release candidate
- [ ] packaging/stage_data.sh --self-test passes

### Platform truth

- [ ] Windows: core ctest subset green under MSVC, OR README says
      "build-verified only"
- [ ] libslirp/Uthernet I: README's per-platform claim matches each package
- [ ] Raspberry Pi packages launched on real hardware
- [ ] README § Known Limitations reconciled against this file

### First run

- [ ] Fresh profile + empty roms/ on each platform: Welcome opens, no crash
- [ ] A roms/ holding only apple2p.rom resolves to a working ][+
- [ ] Every internal README anchor resolves
- [ ] The live demo serves the 1.0 build

### Tag

- [ ] 7 packages + SHA256SUMS.txt attached; body is v1.0.md, not generated notes
- [ ] Download one package per platform and launch it

