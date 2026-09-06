# POM2 core SDK policy

POM2 exposes one optional C++17 facade: `include/pom2/core.hpp`, implemented by
`src/Pom2Core.cpp`. It is a PIMPL wrapper over `Memory` + `M6502` + the small
set of devices an embedding host needs (display, speaker, cassette, Disk II,
Mockingboard), so a consumer never sees an internal definition.

Its contract is pinned by the `pom2_core_api` test, which drives the facade
through a boot, a display frame, audio pulls, a disk mount and a full cassette
record/save/clear cycle, and asserts the header stays self-contained.

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
- `pom2_core_test` is the same source list built for the test binaries. Since
  2026-09-07 it carries `POM2_HAVE_SLIRP` alongside `pom2_core`, so the tests and
  the SDK compile the network backend that actually ships rather than its stub.

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
