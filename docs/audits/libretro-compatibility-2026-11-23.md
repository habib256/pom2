# Apple II Libretro compatibility and loading benchmark

The benchmark runs 12 NTSC Enhanced //e scenarios through the same headless Libretro frontend. Each scenario requires a guest-visible outcome. Two POM2 profiles exercise original Disk II timing and accelerated loading. A third profile runs AppleWin `ra2` with comparable machine settings.

## Reproduce

Run from the POM2 checkout with the regular test media present. `tools/fetch_test_media.py --strict` stages the optional fixtures. The frontend uses Python 3 and the benchmark's screen checks use Pillow. No RetroArch installation is required.

```sh
cmake -S . -B build -DPOM2_BUILD_LIBRETRO=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pom2_libretro --parallel

# AppleWin is optional. Use this revision to reproduce the comparison.
git clone --recurse-submodules https://github.com/audetto/AppleWin.git /tmp/applewin-audit
git -C /tmp/applewin-audit checkout 6e6df2cb7503af105422a01cfa08a0ebc48486a3
git -C /tmp/applewin-audit submodule update --init --recursive
cmake -S /tmp/applewin-audit -B /tmp/applewin-audit/build \
  -DBUILD_LIBRETRO=ON -DENABLE_NETWORKING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/applewin-audit/build --target applewin_libretro --parallel

python3 tools/libretro_bench.py \
  --core pom2-original=build/pom2_libretro.dylib \
  --core pom2-fast=build/pom2_libretro.dylib \
  --core applewin=/tmp/applewin-audit/build/source/frontends/libretro/applewin_libretro.dylib \
  --applewin-source /tmp/applewin-audit \
  --json /tmp/apple2-libretro-bench.json \
  --captures /tmp/apple2-libretro-frames
```

On Linux use `.so` modules. Omit AppleWin arguments for a POM2-only run. Repeat `--scenario` to select workloads. `--json` records library and media SHA-256 digests, frontend options, host frame counts, audio sample and input polling counts. `--captures` adds PNG evidence at each verified screen and on failure. Missing fixtures report `SKIP`. A missed checkpoint, rejected load, or changed input-copy digest reports `FAIL` and makes the process exit with status 1.

## What is compared

- Both cores load separate temporary copies of identical disk images and receive the same Libretro keyboard events. The script compares each copy byte for byte by SHA-256 after unloading. BASIC starts without content and runs `PRINT 1111+1234`; the witness checks the displayed glyphs `2345`.
- POM2 explicitly selects Enhanced //e NTSC. AppleWin selects Enhanced //e, 60 Hz, RGB video, no expansion cards in slots 3, 4, 5 or 7, and a write-protected Disk II in drive 1. It runs its upstream Libretro execution path with its own disk controller implementation. POM2 reports 60.0544 Hz and AppleWin reports 60.0000 Hz.
- The Enhanced //e system ROM (the upper 16 KiB of POM2's 32 KiB file), character ROM, and Disk II boot PROM match AppleWin's built-in resources by SHA-256. POM2 was also given its P6 sequencer ROM. AppleWin's software sequencer is a separate implementation.
- Screen checks normalize the active video area to 280 × 192, then use expected regions, glyphs and changes. Loading checkpoints need two samples eight frontend frames apart. The benchmark counts calls to `retro_run` to the confirmation sample. Reported seconds divide that count by the core's reported output rate. The host runs without pacing, so this is front-end time at nominal playback rate and independent of the machine's wall-clock throughput.
- This audit checks the screens and actions listed below. It does not grade pixel fidelity, audio accuracy, copy protection, two-drive titles, or PAL/SmartPort DIX. Heartbeat and Monster Splash are visual checkpoints with no Mockingboard configured in either profile.

## Local results


Measured on arm64 macOS 15.8.3 with release builds, Python 3.14.7 and Pillow 13.0.0. POM2 identified itself as 1.2.2, from the working tree based on `93ad35bc4097f247731a85eb8aaf30fbe8a84258`. AppleWin identified itself as 1.32.1.0 and was built without source changes from `6e6df2cb7503af105422a01cfa08a0ebc48486a3`.

**POM2 original 12/12, POM2 accelerated 12/12, AppleWin 11/12.** No fixture was skipped or input disk copy modified. The table reports *frontend frames / seconds* to the final witness, including scripted actions and waits. A second full run on the same binaries reproduced the outcome and final frame count of every scenario. Individual animated screenshots sometimes differed.

| Scenario | Verified result | POM2 original | POM2 accelerated | AppleWin |
| --- | --- | ---: | ---: | ---: |
| `applesoft-calculate` | No-content Applesoft prompt and exact `2345` result | 104 / 1.73 | 104 / 1.73 | Fail at 720 / 12.00 |
| `dirtbike-track-menu` | HGR title, Return opens two-column track menu | 1,354 / 22.55 | 530 / 8.83 | 850 / 14.17 |
| `dirtbike-race-throttle` | First race starts, rider is stationary before throttle, repeated A moves it | 1,692 / 28.17 | 844 / 14.05 | 1,252 / 20.87 |
| `sokoban-help` | H opens help from the title | 922 / 15.35 | 482 / 8.03 | 818 / 13.63 |
| `sokoban-move-undo` | Return enters tutorial, Right moves the character, U restores the board | 978 / 16.29 | 538 / 8.96 | 874 / 14.57 |
| `chromabreak-paddle` | Space starts level one, Right moves the paddle across the lower field | 1,266 / 21.08 | 674 / 11.22 | 978 / 16.30 |
| `a2filecmd-help` | DOS 3.3 file list, `?` shows commands, Escape returns to file list | 1,908 / 31.77 | 716 / 11.92 | 1,108 / 18.47 |
| `hectic-raster` | Space launches the raster, a later sample verifies animation | 612 / 10.19 | 356 / 5.93 | 500 / 8.33 |
| `midline-intro` | Space launches the full-screen text effect after the DOS loader | 466 / 7.76 | 210 / 3.50 | 346 / 5.77 |
| `lemm-level-one` | Return advances from level introduction to the playfield and HUD | 586 / 9.76 | 482 / 8.03 | 570 / 9.50 |
| `monster-splash-logo` | Loader text gives way to the graphic logo and its reflection | 1,408 / 23.45 | 1,104 / 18.38 | 1,352 / 22.53 |
| `heartbeat-animation` | Space launches the heart raster, a later sample verifies animation | 586 / 9.76 | 330 / 5.50 | 474 / 7.90 |

AppleWin accepted the no-content load and displayed `Apple //e`. Under this frontend and configuration the BASIC prompt was absent after 720 frames, so the script never sent the arithmetic command. All 11 disk scenarios passed on AppleWin. POM2 accelerated was faster than both alternatives on these 11 scenarios and tied original mode on the no-disk case.

For a loading-only comparison, DIRTBIKE's HGR title was confirmed before any input at **1,328 / 22.11** on original POM2, **496 / 8.26** on accelerated POM2, and **824 / 13.73** on AppleWin. These measurements use the external Disk II boot PROM and P6 ROM in the POM2 profiles. The separate C++ module regression uses the core's firmware fallback and verifies the HGR logo on every frame, so its frame numbers differ.

| Binary or matching ROM | SHA-256 |
| --- | --- |
| POM2 module, both profiles | `3b3f20a4e6d0980566a56c71bff91c82e3171f01389db579b7654e860cac5166` |
| AppleWin module | `6236a127c4151aa8d7948fa5df71176ab5a1c4e7a57c9b0e2bccc6132434dbad` |
| Enhanced //e system ROM | `aab38a03ca8deabbb2f868733148c2efd6f655a59cd9c5d058ef3e0b7aa86a1a` |
| Enhanced //e character ROM | `52c3b87900ac939f6525402cab1ccfd8f8259290fc6df54da48fb4c98ae3ed0f` |
| Disk II boot PROM | `de1e3e035878bab43d0af8fe38f5839c527e9548647036598ee6fe7ec74d2a7d` |
