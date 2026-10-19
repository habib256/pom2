# POM2 core SDK policy

POM2 exposes one optional C++17 facade: `include/pom2/core.hpp`, implemented by
`src/Pom2Core.cpp`. It is a PIMPL wrapper over `Memory` + `M6502` + the small
set of devices an embedding host needs (display, speaker, cassette, Disk II,
3.5" SmartPort, Mockingboard), so a consumer never sees an internal definition.

Its contract is pinned by the `pom2_core_api` test, which drives the facade
through a boot, a display frame, audio pulls, a disk mount, a slot-5 boot off an
800 KiB image with a write that is flushed and read back from the host file, and
a full cassette record/save/clear cycle, and asserts the header stays
self-contained.

## Scheduling, input and frontend boundaries

`Core` is single-threaded and has no wall-clock pacing or host device loop.
The embedding host schedules `run(minCycles)` or `step()`; `run` returns the
actual executed cycles and may overshoot the requested budget at an instruction
boundary. `CoreConfig` selects CPU silicon, PAL/NTSC timing and IIe memory
at construction, with an initially empty expansion bus.

`loadRom` and `loadCharacterRom` load the paths supplied by the host; they do
not run the desktop profile-selection workflow or start execution. Choose the
construction settings, load firmware, attach devices and then call `coldBoot`
or a boot method. `read`/`write` perform real emulated bus accesses, including
soft-switch side effects; they are not memory-inspector peeks. Failed
load/attach/save/boot operations report their error through `lastError()`;
queries such as `diskInserted()` can return `false` as an ordinary result.

`setPaddle(0..3, value)` and `setPaddleButton(0..2, pressed)` expose the full
game port. `setJoystickAxes(x, y)` is a convenience for paddles 0/1 only;
deadzone, inversion, right-stick mapping and host gamepad discovery belong
to the embedding frontend. `queueKey`/`pasteText` inject characters, while
`setKeyHeld`/`releaseHeldKeys` supply physical IIe AKD state separately.

`renderFrame()` returns a borrowed CPU-rendered framebuffer without OpenGL;
the next render, move-assignment or destruction invalidates the view. For
audio, configure the sample rate before running cassette code. Pull either
the speaker-only stream or the complete stereo mix for a given span of guest
time, because both consume the same speaker timeline.

The desktop/browser `.pom2session` workflow, automatic machine reconstruction,
bank-aware debugger and source panel are not methods on this public facade.
They belong to the application/controller layer. Linking `POM2::core` does
not expose them through `include/pom2/core.hpp`; see
[sessions](session-format.md) and [debugging](debugger-banks.md) for those
application contracts.

## 3.5" SmartPort

`attachSmartPort(slot)` plugs POM2's block-level `SmartPortCard` with two
`SmartPort35Unit`s (ProDOS drive 1 / drive 2 of the slot) and its **synthetic**
slot ROM; no Liron dump is read. It identifies as a ProDOS block device
(`$Cn07 = $01`, `$CnFF` → driver), so ProDOS 8 drives it through the slot
driver and never enumerates SmartPort units 3+. `insertSmartPortDisk` takes an
underlying `Disk35Image`: raw ProDOS-order 400/800 KiB media, supported 2IMG
wrappers and GCR WOZ/MOOF captures decoded as blocks. It is not limited to the
800 KiB `.po` used by the API regression. The facade does not enable SuperDrive
MFM/1.44 MB mode and provides no HDV-unit or chain-size setter; its two units
remain distinct from the application's configurable 2–14-unit chain.

`bootSlot(n)` checks the card's `$Cn01/$Cn03/$Cn05` boot signature before
resetting, then cold-boots and checks that the ROM is visible on the CPU bus.
A missing signature leaves RAM untouched; failure of the post-reset visibility
check returns `false` after the cold boot. A Disk II left in slot 6 does not
prevent a slot-5 SmartPort boot.

**Timing**: the card has no mechanical model — no seek, no rotation, no motor
spin-up. A block costs exactly the CPU cycles of the slot ROM's 512-byte copy
loop against `$C0n3`, so the same image booted twice reaches the same state on
the same cycle (pinned).

**Writes** to writable block media land in the unit's in-memory image and reach the file on
`flushSmartPortDisk(unit)` (atomic temp + rename), on an insert over the disk,
on an eject, and when the `Core` is destroyed. The facade runs no autosave
thread — it has no threads. The write default follows `MediaWritePolicy.h` like
the Disk II's: writable, unless the image file is read-only or
`POM2_MEDIA_WRITE_DEFAULT=protected`.
GCR WOZ/MOOF captures are write-protected through this block interface; the
application's native-IWM capture writes are a separate path. A failed explicit
flush/eject/replacement reports an error and keeps the outgoing medium mounted.
Flush explicitly when the host needs to observe save errors before destroying
the core.

## Mockingboard and cassette

`attachMockingboard` accepts multiple cards in different empty slots. The
complete audio pull mixes all attached cards; `setMockingboardVolume` applies
to every attached card, not only the last one. The public models are A/C and
Sound II; the desktop card catalogue is not the facade's attachment API.

`saveCassette` passes its path directly to the cassette device: `.wav` selects
WAV and other suffixes select ACI. It does not append a suffix as the desktop
CLI does. Supply `.aci` or `.wav` explicitly when saving through the SDK.

## What ships

The installable half **landed** with the core-boundaries work (`77a57c9`) and is
in this tree:

- **`pom2_core`**, a static archive built from `POM2_CORE_SOURCES`, with
  **`POM2::core`** as its in-tree alias. `EXPORT_NAME core` makes the *installed*
  package spell it `POM2::core` as well — without that a consumer would have to
  write `POM2::pom2_core` and the two spellings would diverge silently.
- **`install(EXPORT pom2_coreTargets ... NAMESPACE POM2::)`** plus a
  `configure_package_config_file` / `write_basic_package_version_file` pair, so
  `find_package(pom2_core)` works against an install tree. Version compatibility
  is `SameMinorVersion`.
- Everything is one install **component**, `pom2_core_sdk`: `cmake --install
  --component pom2_core_sdk` gives a consumer the archive, the headers and the
  package files and nothing else — no ROMs, no fonts, no emulator binary.
- **`examples/pom2_core_consumer/`** is a standalone project (its own
  `CMakeLists.txt` + `main.cpp`) that `find_package`es the installed package and
  links `POM2::core`. It runs in CI as the **`pom2_core_sdk_consumer`** ctest,
  which is what makes the export contract a tested claim rather than a written
  one. It is deliberately excluded from `tools/coverage.sh`: it links a separate
  project against the installed archive with plain flags, which cannot work
  against an instrumented build, and it measures the export contract rather than
  POM2's code.
- `pom2_core_test` is the same source list built for the test binaries. When
  libslirp is found it carries `POM2_HAVE_SLIRP` alongside `pom2_core`, so the
  tests and the SDK compile the network backend that actually ships rather than
  its stub.

## What is deliberately not here

For the installed-consumer recipe, see [Building a consumer](#building-a-consumer).

- **No shared library, and no stable ABI.** `pom2_core` is a static archive
  compiled by the consumer's toolchain against the same C++17 headers. There is
  no versioned `.so`/`.dylib`/`.dll`, no symbol-visibility map, and no promise
  that two POM2 releases are binary-compatible — `SameMinorVersion` on the
  package file is a *source*-compatibility statement.
- **No C API and no language bindings.** The facade is C++17 only.
- **No package-manager recipe.** There is no vcpkg port, Conan recipe or system
  package for `pom2_core`; `vcpkg.json` in the root describes POM2's own
  dependencies, not a published POM2 package.
- **No registry of external consumers and no compatibility matrix.** That is not
  proof no external user exists, but it is not evidence for maintaining several
  public binary interfaces either. Until one appears, the policy stays: one
  facade, one archive, source compatibility, and the consumer example as the
  only contract that must keep passing.

## Building a consumer

After configuring POM2 using the [build guide](USER-GUIDE.md#build-from-source),
build and install only the SDK component, then configure the standalone example:

```sh
cmake --build build --config Release --target pom2_core --parallel 3
cmake --install build --config Release --prefix /tmp/pom2-sdk --component pom2_core_sdk
cmake -S examples/pom2_core_consumer -B /tmp/pom2-core-consumer -DCMAKE_PREFIX_PATH=/tmp/pom2-sdk
cmake --build /tmp/pom2-core-consumer --config Release
```

Use writable staging paths appropriate to the host. The example links
`POM2::core` through `find_package(pom2_core CONFIG REQUIRED)` and needs no
ROM to exercise basic RAM, stepping, framebuffer and audio API calls. Loading
and booting actual Apple II software requires the embedding host to supply
its firmware and media through the facade.
