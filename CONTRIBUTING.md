# Contributing to POM2

Thanks for looking. POM2 is a cycle-accurate Apple II emulator, and most of
what follows exists to keep it that way.

## Build and test

```bash
./setup_imgui.sh                              # once: dependencies + pinned Dear ImGui
cmake -S . -B build -DPOM2_ENABLE_TESTS=ON
cmake --build build -j3
cd build && ctest -j2                          # run from build/; tests find roms/ themselves
```

`make probes` (from `build/`) builds the eyeball tools that are deliberately
not tests. The WebAssembly build is `./build_wasm.sh`.

## Before you open a pull request

- **The whole suite passes**, and a change that fixes a defect comes with a
  test that fails without it. Check that it does: put the old line back and
  watch the test fail. A test that cannot fail is the defect this project
  keeps finding in its own guards.
- **The text guards pass** — CI runs them, and so can you:
  `tools/check_file_sizes.sh`, `tools/check_version_strings.sh`,
  `tools/check_settings_keys.sh`, `tools/check_workflow_pins.sh`.
- **The emulation did not move by accident.** `ctest -R bench_identity`
  compares `pom2_bench`'s cycle counts and RAM hashes with
  `tests/bench_golden.txt`. A change *meant* to alter emulation regenerates
  the golden (`tools/check_bench_identity.sh build/pom2_bench --update`) and
  says which fix did it.
- **Hardware follows MAME.** When you port or correct a device, cite the MAME
  file and line range in a comment. Where POM2 deliberately differs, say why
  at the line.

## Conventions

Read `CLAUDE.md` first — it is short and it is the index. The ones people
trip on:

- one concern per `.cpp/.h` pair; the size ratchet refuses growth of the
  large files without a written reason in `tools/file_size_budget.txt`;
- reach emulated state through `controller->lockState()`, and never hold that
  lock across file I/O (mount, eject and flush have two-phase forms);
- CPU-to-audio/UI events carry an emulated-cycle stamp, never wall-clock;
- every long-lived thread goes through `pom2::guardedThread`;
- documentation is written in English.

`DEV.md` holds the why behind each subsystem; `TODO.md` holds the open work
and the scope ruling — check it before starting something large, because
some subsystems are deliberately frozen.

## Reporting bugs

Use the issue template. Security problems go through `SECURITY.md`, not the
issue tracker.
