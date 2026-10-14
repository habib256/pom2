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

## 3.5" SmartPort

`attachSmartPort(slot)` plugs POM2's block-level `SmartPortCard` with two
`SmartPort35Unit`s (ProDOS drive 1 / drive 2 of the slot) and its **synthetic**
slot ROM; no Liron dump is read. It identifies as a ProDOS block device
(`$Cn07 = $01`, `$CnFF` → driver), so ProDOS 8 drives it through the slot
driver and never enumerates SmartPort units 3+. `insertSmartPortDisk` takes an
819 200-byte `.po` or an 800 KiB `.2mg`; `bootSlot(n)` cold-boots at `$Cn00`
after checking the `$Cn01/$Cn03/$Cn05` boot signature, and a Disk II left in
slot 6 does not get in the way.

**Timing**: the card has no mechanical model — no seek, no rotation, no motor
spin-up. A block costs exactly the CPU cycles of the slot ROM's 512-byte copy
loop against `$C0n3`, so the same image booted twice reaches the same state on
the same cycle (pinned).

**Writes** land in the unit's in-memory image and reach the file on
`flushSmartPortDisk(unit)` (atomic temp + rename), on an insert over the disk,
on an eject, and when the `Core` is destroyed. The facade runs no autosave
thread — it has no threads. The write default follows `MediaWritePolicy.h` like
the Disk II's: writable, unless the image file is read-only or
`POM2_MEDIA_WRITE_DEFAULT=protected`.

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
