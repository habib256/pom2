# Apple II Libretro compatibility and loading benchmark

The benchmark runs 12 NTSC Enhanced //e scenarios through the same headless Libretro frontend. Each scenario requires a guest-visible outcome. **Use `pom2-fast` to evaluate the default shipping load mode.** Use `pom2-original` for the same build with original Disk II timing. A third profile runs AppleWin `ra2` with comparable machine settings.

## Reproduce

Run from the POM2 checkout with the regular test media present. `tools/fetch_test_media.py --strict` stages the optional fixtures. The frontend uses Python 3 and the benchmark's screen checks use Pillow. No RetroArch installation is required.

For the two protected scenarios, follow the [protected corpus retrieval recipe](../../tests/corpus/protected.md#fetch-temporarily-and-replay) into a temporary directory outside the checkout. Provide `captain-a.woz`, `captain-b.woz`, `prisoner2a-t33.woz` and `prisoner2a-unpatched.woz`. To get the last file, save the original `Prisoner 2 - Disk 1, Side A.woz` entry from the Prisoner II archive before editing the INFO byte and recomputing the CRC in that recipe. The benchmark checks all four SHA-256s before loading.

```sh
curl -fL https://github.com/AppleWin/AppleWin/files/7645198/Prisoner.2.-.Disk.1.Side.A.zip \
  -o "$media_dir/prisoner-ii.zip"
unzip -p "$media_dir/prisoner-ii.zip" 'Prisoner 2 - Disk 1, Side A.woz' \
  > "$media_dir/prisoner2a-unpatched.woz"
```

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
  --protected-media "$media_dir" \
  --json /tmp/apple2-libretro-bench.json \
  --captures /tmp/apple2-libretro-frames
```

On Linux use `.so` modules. Omit AppleWin arguments for a POM2-only run. Repeat `--scenario` to select workloads. `--json` records library and media SHA-256 digests, frontend options, host frame counts, audio sample and input polling counts, and comparable timing statistics. `--captures` adds PNG evidence at each verified screen and on failure. Missing fixtures report `SKIP`. A missed checkpoint, rejected load, or changed input-copy digest reports `FAIL` and makes the process exit with status 1.

## What is compared

- Both cores load separate temporary read-only copies of identical disk images and receive the same Libretro keyboard, analog and disk-control events. The script compares each copy byte for byte by SHA-256 after unloading. BASIC starts without content and runs `PRINT 1111+1234`. The witness checks the displayed glyphs `2345`.
- POM2 explicitly selects Enhanced //e NTSC and uses the core's read-only mode without a save directory. AppleWin selects Enhanced //e, 60 Hz, RGB video, no expansion cards in slots 3, 4, 5 or 7, and a write-protected Disk II in drive 1. It runs its upstream Libretro execution path with its own disk controller implementation. POM2 reports 60.0544 Hz and AppleWin reports 60.0000 Hz.
- The Enhanced //e system ROM (the upper 16 KiB of POM2's 32 KiB file), character ROM, and Disk II boot PROM match AppleWin's built-in resources by SHA-256. POM2 was also given its P6 sequencer ROM. AppleWin's software sequencer is a separate implementation.
- Screen checks normalize the active video area to 280 × 192, then use expected regions, glyphs and changes. Loading checkpoints need two samples eight frontend frames apart. The benchmark counts calls to `retro_run` to the confirmation sample. Reported seconds divide that count by the core's reported output rate. The host runs without pacing, so this is front-end time at nominal playback rate and independent of the machine's wall-clock throughput.
- ChromaBreak boots the checked ProDOS volume and reads five signed analog X values with Y held at the paddle floor. The benchmark locates the moving paddle in five distinct, increasing screen positions. The two WOZ cases exercise Captain Goodnight's protected Boot A and disk-turn path, and Prisoner II's timing-sensitive protection. The Prisoner II original at timing 32 serves as a 5,400-frame negative control with its own fresh load. The timed result comes from the patched capture at timing 33 reaching the resignation code.
- The scope is the displayed milestones, disk-turn, and analog behavior listed below. Heartbeat and Monster Splash are visual checkpoints with no Mockingboard configured in either profile. PAL, SmartPort, FLUX images, audio quality and full playthroughs require separate runs.

## Local results

Measured on arm64 macOS 15.8.3 with release builds, Python 3.14.7 and Pillow 13.0.0. POM2 identified itself as 1.2.2, from the working tree based on `34e0375cec349dfdf7a76a81a3fbfb1cb9175be4`. AppleWin identified itself as 1.32.1.0 and was built without source changes from `6e6df2cb7503af105422a01cfa08a0ebc48486a3`.

**POM2 original 12/12, POM2 accelerated 12/12, AppleWin 11/12.** All fixtures ran and their disk image digests were unchanged after unloading. The table reports *frontend frames / seconds* to the final witness, including scripted actions and waits.

A second full run reproduced all outcomes and both timing statistics. POM2 reproduced every final frame count in both modes. AppleWin reproduced ten of its eleven successful final frame counts. Its Prisoner II witness appeared at frame 4,634 on the repeat, 32 frames earlier than in the table.

| Scenario | Verified result | POM2 original | POM2 accelerated | AppleWin |
| --- | --- | ---: | ---: | ---: |
| `applesoft-calculate` | No-content Applesoft prompt and exact `2345` result | 104 / 1.73 | 104 / 1.73 | Fail at 720 / 12.00 |
| `dirtbike-race-throttle` | Return opens the track menu and starts the race. Repeated A moves the rider | 1,692 / 28.17 | 844 / 14.05 | 1,188 / 19.80 |
| `sokoban-move-undo` | Return enters tutorial, Right moves the character, U restores the board | 978 / 16.29 | 538 / 8.96 | 874 / 14.57 |
| `chromabreak-prodos-analog` | Boot checked ProDOS volume, J selects joystick, five analog values move the paddle to five positions | 1,302 / 21.68 | 710 / 11.82 | 1,022 / 17.03 |
| `a2filecmd-help` | DOS 3.3 file list, `?` shows commands, Escape returns to file list | 1,908 / 31.77 | 716 / 11.92 | 1,108 / 18.47 |
| `hectic-raster` | Space launches the raster, a later sample verifies animation | 612 / 10.19 | 356 / 5.93 | 500 / 8.33 |
| `midline-intro` | Space launches the full-screen text effect after the DOS loader | 466 / 7.76 | 210 / 3.50 | 346 / 5.77 |
| `lemm-level-one` | Return advances from level introduction to the playfield and HUD | 586 / 9.76 | 482 / 8.03 | 570 / 9.50 |
| `monster-splash-logo` | Loader text gives way to the graphic logo and its reflection | 1,408 / 23.45 | 1,104 / 18.38 | 1,352 / 22.53 |
| `heartbeat-animation` | Space launches the heart raster, a later sample verifies animation | 586 / 9.76 | 330 / 5.50 | 482 / 8.03 |
| `captain-woz-disk-turn` | Protected Boot A shows `TURN DISK OVER`. Libretro eject/select/insert face B, then Space reaches the helicopter arrival | 2,052 / 34.17 | 1,516 / 25.24 | 2,092 / 34.87 |
| `prisoner-woz-timing` | Timing-33 WOZ shows `RESIGNATION FILE` then the code. Timing-32 control stays short of both witnesses | 4,522 / 75.30 | 3,458 / 57.58 | 4,666 / 77.77 |

AppleWin accepted the no-content load and displayed `Apple //e`. Under this frontend and configuration the BASIC prompt was absent after 720 frames, so the script never sent the arithmetic command. All 11 disk scenarios, including both protected WOZ cases, passed on AppleWin. POM2 accelerated reached every common final witness earlier than both comparison profiles.

### Common timings

The paired set consists of the **11 scenarios that passed on all three profiles**. For each profile, sort the 11 elapsed times to their final positive witness, with each scenario given equal weight. The median is the sixth value. P90 uses the nearest-rank definition, the value at one-based rank `ceil(0.90 × 11) = 10`. The BASIC timeout and the negative Prisoner II control are outside this timing sample. The benchmark calculates these statistics from unrounded `host frames / FPS` values.

| Profile | Median | P90 |
| --- | ---: | ---: |
| pom2-original | 21.68 s | 34.17 s |
| pom2-fast | 11.82 s | 25.24 s |
| applewin | 17.03 s | 34.87 s |

For a loading-only comparison, DIRTBIKE's HGR title was confirmed before any input at **1,328 / 22.11** on original POM2, **496 / 8.26** on accelerated POM2, and **824 / 13.73** on AppleWin. These measurements use the external Disk II boot PROM and P6 ROM in the POM2 profiles. The separate C++ module regression uses the core's firmware fallback and verifies the HGR logo on every frame, so its frame numbers differ.

For a control comparison, both POM2 profiles produced ChromaBreak paddle centers at **48.5, 94.5, 138.5, 182.5, and 228.5 px** for the five left-stick positions. AppleWin produced **30.5, 90, 138, 186, and 249.5 px** under its mapping. Every profile reached the positive Prisoner II resignation code and ran the original timing-32 image for 5,400 frames without reaching the file or code screen.

| Binary or matching ROM | SHA-256 |
| --- | --- |
| POM2 module, both profiles | `e48fffa43934c83bb24e52cb6568fd079a4a87e2fc552fd2985a4e998793750b` |
| AppleWin module | `6236a127c4151aa8d7948fa5df71176ab5a1c4e7a57c9b0e2bccc6132434dbad` |
| Enhanced //e system ROM | `aab38a03ca8deabbb2f868733148c2efd6f655a59cd9c5d058ef3e0b7aa86a1a` |
| Enhanced //e character ROM | `52c3b87900ac939f6525402cab1ccfd8f8259290fc6df54da48fb4c98ae3ed0f` |
| Disk II boot PROM | `de1e3e035878bab43d0af8fe38f5839c527e9548647036598ee6fe7ec74d2a7d` |

## After the options rework

Rerun the same day on the core that adds the machine, display and card
options. The benchmark now selects its profile through the new options
(`pom2_machine=iie-enhanced`, `pom2_video_standard=ntsc`,
`pom2_dix_preset=disabled`, `pom2_mockingboard=disabled`,
`pom2_display=composite`), the same machine as before: an Enhanced //e with no
card in slots 3, 4, 5 or 7, matching the AppleWin profile.

**POM2 original 12/12, POM2 accelerated 12/12, AppleWin 11/12**, all three
in one run: AppleWin rebuilt from `6e6df2cb7503af105422a01cfa08a0ebc48486a3`
as above, the protected fixtures fetched by the recipe above (their SHA-256s
checked by the benchmark) and deleted afterwards. AppleWin again showed no
BASIC prompt within 720 frames. Over the 11 common scenarios the medians are
21.68 s original, 11.82 s accelerated and 17.03 s AppleWin (P90 34.17 s,
25.24 s, 34.87 s), the figures of the table above.

Every final frame count is identical to the table above in both POM2 modes
(for example Dirtbike 1,692 / 844, ChromaBreak 1,302 / 710, A2FILECMD
1,908 / 716, Monster Splash 1,408 / 1,104): the rework changed no timing.
Captain Goodnight's disk turn reached the helicopter at 2,052 / 1,516 and
Prisoner II's resignation code at 4,522 / 3,458, as before. Over the ten
unprotected scenarios the medians are 13.24 s original and 8.49 s
accelerated (P90 28.17 s and 14.05 s).

The release package was then checked in the official RetroArch 1.22.2 for
macOS (`RetroArch_Metal.dmg`, SHA-256 `81b79121…fdb434`, mounted read-only)
with `tools/check_libretro_package.py`: fresh install, cold boot, automatic
save, process restart and resumed Dirtbike title, PASS. RetroArch saved all
25 core options to `POM2.opt` and acknowledged achievement support. Separate
RetroArch sessions showed the DIX menu on the RGB monitor (PAL, Mockingboard
slot 2), Dirtbike on the green monitor with drive sounds, Apple II DeskTop
booted from a 32 MiB HDV on a //c with the AppleWin TV display, and the
Apple ][+ BASIC prompt. Its first HDV session mounted the image read-only:
private disk copies were capped at 16 MiB, now 33 MiB (the largest ProDOS
volume and a 2IMG header), after which the copy was made and writable.

### Achievements through rcheevos

A harness built on rcheevos at `f4abfca` (its `rc_libretro` memory mapping,
the code RetroArch runs, and its `rc_runtime`) loaded the module with no
content and mapped its memory for `RC_CONSOLE_APPLE_II`: 128 KiB, main RAM at
`0x00000` and auxiliary RAM at `0x10000`. Three conditions in
RetroAchievements syntax were active from the BASIC prompt: none triggered
before the guest acted; `0xH0300=42` triggered on `POKE 768,42`,
`0xH10400=160` when `PR#3` cleared the 80-column screen in auxiliary RAM, and
`0xH0300=42_0xH10400=160` on the two together.

The same harness on the AppleWin module maps the same 128 KiB, but its BASIC
shows no prompt under this frontend (as in the benchmark), so nothing was
typed. After both booted Dirtbike for 1,500 frames, the RAM read through
`rc_libretro` matched at 82 % of `$0800-$1FFF` and 93 % of `$6000-$95FF`
(game state differs; an offset map would match almost nothing). The language
card and auxiliary RAM, which Dirtbike leaves alone, hold each emulator's
power-on pattern. No published achievement set was run: that needs a
RetroAchievements account.

## A real power-on, and the wide corpus

The core now boots a Disk II by powering the machine on and letting the //e
ROM's reset and slot scan find the drive, as the hardware and AppleWin do,
instead of the SDK's `bootDisk()` shortcut to `$C600`. The shortcut skips the
reset's set-up, and two titles drew nothing after it (below). The ROM's
start costs about one sample (8 frames): most Disk II scenarios end 8 frames
later (Dirtbike 1,692 → 1,700 original; Captain Goodnight 2,052 → 2,060 and
1,516 → 1,524; Prisoner II 4,522 → 4,530 and 3,458 → 3,466), Dirtbike
accelerated (844) and Hectic original (612) do not move, and Monster Splash
and Heartbeat accelerated end 16 frames later (1,120 and 346).
**POM2 12/12 in both modes, AppleWin 11/12**, one run, the
protected fixtures fetched and deleted as above. Medians over the 11 common
scenarios: **21.81 s original, 11.96 s accelerated, 17.03 s AppleWin** (P90
34.30 s, 25.38 s, 34.87 s).

`tools/libretro_corpus.py` boots every local disk image in each core for 30
emulated seconds and records what the guest shows: an error message read from
the 40-column text page (I/O ERROR, UNABLE TO LOAD, ...), a black picture, or
a picture. A coarse witness, applied the same way to both cores, over the 46
images under `disks_5.4`, `disks_3.5` and `hdv` (test media and local
images), POM2 and AppleWin in the benchmark's //e NTSC profile with a
Mockingboard in slot 4:

| | Picture | Black | Error | Not loaded |
|---|---:|---:|---:|---:|
| POM2 | **44** | 1 | 1 | 0 |
| AppleWin | 35 | 1 | 1 | 9 |

AppleWin's nine: the 13-sector `DOS32STD.d13` it rejects, and the six
3.5-inch and two hard disk images its profile has no card for. The black
image (`second_d1.dsk`, Second Reality's first disk) and the error
(`prodos-drivers-a2stuff-v1.8.po`, "UNABLE TO" on a //e with no clock card)
are the same on both cores. Every 5.25-inch image AppleWin boots, POM2 boots.
Before the power-on change POM2 showed black on DIGIDREAM (`DD.dsk`) and
`fastloader.woz`, which AppleWin drew: that difference is what found the
`bootDisk()` problem.

### The same corpus on a //c and in PAL

AppleWin runs neither machine, so these runs compare POM2 with itself, each
profile through the core's options (`--set pom2-iic:pom2_machine=iic`,
`--set pom2-pal:pom2_video_standard=pal`), the Mockingboard on (on the //c,
the Mockingboard 4c on its internal connector) and the No-Slot Clock in
emulated time:

| Profile | Picture | Black | Error |
|---|---:|---:|---:|
| //e Enhanced NTSC | 44 | 1 | 1 |
| //e Enhanced PAL | 44 | 1 | 1 |
| //c NTSC | 42 | 2 | 2 |

PAL changes no outcome. The //c's two extra failures:

- `DOS32STD.d13` reports CHECK DISK DRIVE: the //c's ROM reads 16-sector
  disks only, so a 13-sector DOS 3.2 disk does not boot on one, as on the
  real machine.
- A2osX 0.95 stays black (3.5-inch, through the //c's rear port, which boots
  Apple II DeskTop, AppleShare and CardCat). Over three runs each it boots
  with the Mockingboard 4c off and the clock on, and stays black with the 4c
  on, or with both off. A reading, not verified on a //c: the 4c sleeps until
  a program writes into `$C400-$C4FF` and then answers that page over the
  //c's mouse firmware (MAME `c400_w`); a slot probe that writes there wakes
  it and loses the mouse. Settled by: A2osX on a //c with a Mockingboard 4c.
