# POM2 core SDK policy

POM2 exposes one optional C++17 facade: `include/pom2/core.hpp`, implemented by
`src/Pom2Core.cpp`. It is a PIMPL wrapper over `Memory` + `M6502` + the small
set of devices an embedding host needs (display, speaker, cassette, Disk II,
3.5" SmartPort, Mockingboard, Apple Mouse, the //c's built-in mouse), so a
consumer never sees an internal definition.

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
at construction, with an initially empty expansion bus — or a named
`Machine` (see [Machines](#machines-and-the-c)).

The default `CoreConfig{}` selects NMOS 6502, NTSC and non-IIe memory. It does
not select the desktop's enhanced PAL //e profile. Instances are movable,
noncopyable and must not be accessed concurrently. `run(0)` or a negative
budget returns zero without executing an instruction.
Returned cycles measure elapsed emulated time, not retired instructions:
a halted CPU still advances device time in two-cycle idle steps. Check
`cpuState().halted` when deciding whether guest execution can make progress;
reset clears that halt state.

`loadRom` and `loadCharacterRom` load the paths supplied by the host; they do
not run the desktop profile-selection workflow or start execution. Choose the
construction settings, load firmware, attach devices and then call `coldBoot`
or a boot method. `read`/`write` perform real emulated bus accesses, including
soft-switch side effects; they are not memory-inspector peeks. Failed
load/attach/save/boot operations report their error through `lastError()`;
queries such as `diskInserted()` can return `false` as an ordinary result.

System ROM loading recognizes 12/16/20/32 KiB layouts: 12 KiB maps at
`$D000`, 16 KiB at `$C000`, and a 20 KiB dump skips its first 4 KiB before
mapping the remaining 16 KiB. For 32 KiB, `lowerBankFor32K=false` selects
the upper firmware half of a combined //e dump; `true` selects //c bank 0
and retains its alternate bank. Other accepted sizes from 2–64 KiB use
best-effort placement at the top of memory; acceptance alone does not make
an arbitrary dump compatible with the chosen machine. Character ROM data
is loaded separately through `loadCharacterRom`.

`setPaddle(0..3, value)` and `setPaddleButton(0..2, pressed)` expose the full
game port. `setJoystickAxes(x, y)` is a convenience for paddles 0/1 only;
deadzone, inversion, right-stick mapping and host gamepad discovery belong
to the embedding frontend. `queueKey`/`pasteText` inject characters, while
`setKeyHeld`/`releaseHeldKeys` supply physical IIe AKD state separately.

`queueKey` models a single hardware latch: without pending paste bytes,
another call replaces an unread character. While paste bytes remain queued,
live keys append after them. Values are masked to 7 bits; II/II+ memory mode
also folds ASCII lowercase to uppercase. Use `pasteText` for a whole string
rather than calling `queueKey` repeatedly before running the guest.

`pasteText` normalizes CR/LF/CRLF to CR, drops controls below `$20` except
CR and tab, and applies the same II/II+ case fold. A well-formed UTF-8 buffer
uses best-effort ASCII transliteration of supported accents/punctuation;
unsupported Unicode characters are omitted. Other buffers use Apple
high-ASCII masking. New paste input is limited by a 4096-character capacity
including pending input and can be truncated. The return value counts
accepted output characters, not consumed input bytes or characters already
read by the guest; it cannot serve as a UTF-8 retry offset. Delivery follows
guest keyboard polling: after clearing the strobe, the next `$C000` read
promotes a queued character. `pasteText` does not run the CPU or wait for
the text to be consumed.

`renderFrame()` returns a borrowed CPU-rendered framebuffer without OpenGL;
the next render, move-assignment or destruction invalidates the view. For
audio, configure the sample rate before running cassette code. Pull either
the speaker-only stream or the complete stereo mix for a given span of guest
time, because both consume the same speaker timeline.

Framebuffer pixels are packed as `0xAABBGGRR` in `uint32_t` values; use the
returned width and height. `pullSpeakerAudio` takes one float per frame;
`pullAudio` takes two interleaved floats per frame, left then right. Their
default rate is 44100 Hz. Allocate the caller-owned buffers accordingly.

Both audio pulls fill and return the requested frame count, including silent
samples; the return value is not an available-audio count. A null buffer,
zero frame count or zero sample rate returns zero. Pulling audio does not
advance the CPU. The host must coordinate emulation and output scheduling;
the facade exposes no desktop pause/suspend or master-mixer controls.

`pullAudio` uses the application's shared mix law with unity master gain,
unmuted stereo output and a final clamp to `[-1, +1]`. It visits the speaker,
cassette and every attached card audio source. `pullSpeakerAudio` bypasses
that final mixer clamp and returns the filtered speaker signal with its
source gain applied; it does not promise the same amplitude bound. Apply
any host output limit required by the destination format.

Disk II drive indices and SmartPort unit indices are **0 and 1**, while
expansion slot numbers are **1 through 7**. The facade provides one Disk II
controller in slot 6, one optional SmartPort controller (slot 5 by default),
and multiple Mockingboards in separate empty slots (slot 4 by default).

`attachDiskII` can replace this core's existing Disk II controller after
successfully loading the new PROMs; it does not retain its mounted disks.
A different card occupying slot 6 refuses attachment. SmartPort can be
attached only once, and SmartPort/Mockingboard attachment requires an empty
destination slot. `bootDisk()` cold-boots and enters `$C600`; its success
means that the controller/PROM/media preconditions passed, not that the guest
has completed booting. It exposes no drive-2 boot selector.

Soft reset preserves A/X/Y, but reloads PC from the reset vector, updates P
and decrements SP. Hard reset clears A/X/Y while preserving RAM; cold boot
also resets RAM to POM2's power-on pattern. Reset calls do not reopen media
or reload firmware files.

The desktop/browser `.pom2session` workflow, automatic machine reconstruction,
bank-aware debugger and source panel are not methods on this public facade.
They belong to the application/controller layer. Linking `POM2::core` does
not expose them through `include/pom2/core.hpp`; see
[sessions](session-format.md) and [debugging](debugger-banks.md) for those
application contracts.

## CPU registers

`setCpuState(state)` overwrites A, X, Y, P, S and PC at the current
instruction boundary; the next `step()`/`run()` executes at the new PC.
`cycles` and `halted` in the struct are ignored, and nothing else moves: the
cycle counter, every soft switch (80STORE, RAMRD/RAMWRT, ALTZP, the language
card, video) and a pending IRQ stay as they were — what a trampoline through
the RESET vector cannot promise, since a //e reset clears them. P's bit 5
reads 1 on every 6502 and is forced. A host calls a guest routine by writing
the return address minus one on page 1, setting S and PC, and stepping until
the PC comes back (pinned by `pom2_core_api`, NMOS and 65C02).

## Mouse

`attachMouse(slot, model, romDirectory)` installs the Apple Mouse Interface
Card in an empty slot (4 by convention), reading `mouse_341-0270-c.bin`
(and, for `MouseModel::AppleMouse`, the MC68705 mask ROM
`mouse_341-0269.bin`) from `romDirectory`. `AppleMouse` runs Apple's firmware
on both chips, as MAME does; `AppleMouseAppleWin` runs the slot EPROM
against AppleWin's high-level MCU. Movement, button and VBL interrupts
(at the beam's line 192) reach the 6502 through the slot bus, as in the
application. One mouse per core.

`moveMouse(dx, dy, button)` is relative motion — what the card's hardware
receives — and exact on both models; on `AppleMouse` keep each axis within
±127 between two run slices, because the MCU reads an 8-bit counter.
`setMouse(x, y, button)` aims at an absolute position in the firmware's clamp
window (the units of the slot's screen holes `$0478+n`/`$04F8+n`). It is a
closed loop, the application's (`MouseSync.h`): each call pushes at most ±127
per axis toward the target, and pushes nothing while the previous correction
has not been consumed and published — the MCU replays a push as quadrature
over several frames, and correcting against a stale position overshoots.
Call it once per frame; it returns true when the card reports the target.
Pinned through Apple's own firmware (INITMOUSE, SETMOUSE, READMOUSE, and
about one VBL interrupt per frame) on both models.

## Machines and the //c

`CoreConfig::machine` defaults to `Machine::Custom`, which takes `cpu` and
`iieMemory` as given (the struct's last member, so the positional
`{cpu, timing, iieMemory}` form keeps its meaning). `AppleIIPlus`,
`AppleIIe` (NMOS) and `AppleIIeEnhanced` (65C02) set those two fields.
`AppleIIc` selects the 65C02 and //e memory, loads a 32 KiB system ROM as a
//c dump (bank 0 at reset; 16 KiB rev-255 dumps load as usual) and plugs the
//c's IOU mouse on port 4: its firmware is in the system ROM, so
`moveMouse`/`setMouse` drive it with no `attachMouse`, which refuses on a
//c. The ROM's IRQ handler integrates the motion, so it moves while the
guest runs with interrupts enabled. `attachMockingboard()` on a //c is the
Mockingboard 4c on the internal connector, whatever slot is passed: dormant
until the guest writes into `$C400-$C4FF`, then answering that page — over
the mouse firmware, entry table included — until a power cycle (a Ctrl-Reset
leaves it awake, as MAME's `reset_w` does). Pinned with the 32 KiB rev-0
and 16 KiB rev-255 ROMs. The //c's serial ports and the external SmartPort
chain are not part of the facade.

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
