# Protected WOZ disks in POM2, MAME and AppleWin — 2026-11-23

37 copy-protected images were booted in three emulators, each the same way:

- **POM2**: the libretro core, through `tools/libretro_corpus.py`, in two
  modes. Default fast disk loading runs extra frames per host frame while
  the drive reads. Original timing does not.
- **AppleWin**: its libretro core, audetto `6e6df2cb` (1.32.1.0), through the
  same frontend.
- **MAME 0.288**: `apple2ee` / `apple2p` / `apple2`, ROM sets from RetroBIOS
  `441d4129`, a snapshot after `-seconds_to_run`.

The captures were compared by eye. Each image was copied read-only per run.
The titles come from 4am's woz-a-day collection on archive.org; the boot
witnesses and known emulator problems come from the WOZ test-suite readme
(`woz_test_images_suite_2019_02`), izapple2's notes, and the AppleWin and
MAME issues each row cites. Nothing was committed to the tree except the
three corpus titles at the end. The images stay outside the checkout.

## Results

**Enhanced //e**, 45 s:

| Title | Protection | POM2 | AppleWin | MAME |
|---|---|---|---|---|
| Autoduel, Balance of Power, Blazing Paddles, Buzzard Bait, Crisis Mountain, Dino Eggs, Flip Out, Gruds in Space, Hadron, Hard Hat Mack, Marble Madness, Miner 2049er II, Run For It, Shogun, Space Quest, Stickybear Town Builder, The Bilestoad, Wavy Navy, Wings of Fury | weak bits, cross-track sync, latch timing, half tracks… | boots | boots | boots |
| Congo Bongo, Free Fall | weak bits / oversize track; Sirius | boots (fast mode is already in the demo) | boots | boots |
| Cyclod, Minotaur, Fly Wars, Jellyfish | **WOZ 2.1 FLUX** | **boots** | **no** (no FLUX support) | boots |
| The Newsroom | non-standard bit timing | boots | boots | **"DISK ERROR"** |
| **Mr. Do** | weak bits on T0, INFO bit timing 29 | **no** (boot PROM, `$C65E`) → **fixed** | boots | boots |
| **Glutton** | nibble count between `$D7` marks | **no** | boots | boots (slowly) |
| Choplifter, Zenith, Seafox, Star Maze, Computer Foosball | ][ / ][+ / unenhanced only | black screen, as on the iron | same | same, or still on the title |

**][+**, 60 s, for the ][-only titles and the two failures:

| Title | POM2 | AppleWin | MAME |
|---|---|---|---|
| Choplifter, Computer Foosball, Free Fall, Seafox, Star Maze, Zenith | boots | boots | boots (Choplifter still on its title: mametesters #07955) |
| **Lady Tut** (side A) | **title after `N`**, on a ][+ and on a ][ | reaches the Mockingboard question | game on a ][+; **`@` garbage on a ][** |
| Mr. Do | fixed, see below | boots | boots |
| Glutton | **no** | boots | boots |

Choplifter and Zenith end on a black screen after their title on an enhanced
//e, in POM2 and AppleWin alike (MAME stays on the title). They are ][+ games;
on a ][+ all three run them. POM2's fast mode only gets there sooner.

Lady Tut's `00playable.woz` on archive.org is **Anti-M**, 4am's loader for an
original disk; the game is `Lady Tut side A.woz` (`00playable2.woz`).

## Mr. Do: a declared bit timing faster than one revolution

The image (written by Passport.py) declares INFO +39 = 29, i.e. a 3.625 µs
cell, for tracks of 53,367 bits. Played at that cell, a revolution lasts
193.8 ms, faster than 300 RPM. POM2 honours +39 ([B] WOZ 2.1: "the ideal rate
that bits should be delivered to the disk controller card"). MAME never reads
it and spreads each track over 200 ms (3.748 µs). AppleWin ignores it too.

What decides it is the latch, not the sequencer. An independent Python
simulation of the 341-0028 P6 PROM dump, written for this audit and kept
apart from POM2's code, gives this:

| Cell (LSS cycles) | 6.75 | 7.0 | 7.25 | 7.5 | 8.0 | 8.25 |
|---|---|---|---|---|---|---|
| Clean GCR decoded | **no** (1157 bad nibbles / 20 runs) | yes | yes | yes | yes | yes |
| Shortest valid-nibble window (CPU cycles) | — | 6.0 | 6.5 | 7.0 | 8.0 | — |

The boot PROM polls with `LDA $C08C,X / BPL`, a 7-cycle loop. At 29 the window
can be shorter than one poll, so nibbles are lost. A synthetic, entirely
standard DOS 3.3 WOZ made from `DIRTBIKE.dsk` reproduces it: it boots at 30
and 32, and not at 29, 28 or 26. So POM2's failure at 29 was the silicon's
behaviour for a 3.625 µs cell. The disk booting on the iron means its cell is
not 3.625 µs on the medium. The revolution says 3.748.

**Fix** (`src/WozBitTiming.h`): a declared cell faster than 32 is raised by
one 125 ns step when the median track fills a revolution at exactly that
step. The field counts whole steps, and 29 for a 29.98 track is a value
rounded down. 32 and slower, and any larger mismatch, are never touched, so
every image in the corpus plays as before, Prisoner 2's 33 included. Mr. Do now reaches its joystick calibration. Pinned by
`woz_bit_timing_smoke` and the protected corpus title `mr-do`.

## Glutton: not fixed — what is known

The loader (`$BB91`) waits for a `$D7` nibble, counts nibbles to the next
`$D7`, retries while the count is under 256, then compares it with a table.
Track 7 (TRK 13) holds two `$D7` marks 24 nibbles apart: one after a run of
9,696 one-bits (`FF … FD D7`), one in a `FE D7 D5 DD` header. After the first
failed count, the loader restarts right after the second mark, so it next
sees the first one and counts 24 again, forever.

The way out is a **double read**. MAME's debugger (a Lua probe on `$BBB5`,
`$BBB7` and `$BAD0`) shows it failing 85 times with a count of 23. Then comes
one failure with a count of **0**: the counting loop read the same `$D7` a
second time, 10 cycles after the wait loop saw it. The next attempt then
starts after the second mark, counts 6,346 nibbles, and the table check
passes.

**The raw flux capture.** woz-a-day's "extras" zip holds Applesauce's A2R3
capture, `Glutton - Disk 1, Side A.a2r` (v1.59b3, 125 ns ticks, a drive at
300.9 RPM). Fed to the independent P6 simulation at the true LSS clock, it
gives:

- both `$D7` marks on every revolution, so framing is not the cause;
- a latch window of **12 cycles** for the first mark (followed by `BA`),
  against 8 for the second. The normalised WOZ bitstream gives the same
  12.5;
- **[A] single cells measuring 28 to 36 ticks** (bins 28 and 32 dominate),
  i.e. ±0.5 LSS cycle around 32. This is jitter measured on the iron.

So the loader succeeds when its 7-cycle wait loop reads the first `$D7` in
the first ~2 cycles of that 12-cycle window. The 9,696 one-bits before the
mark lock the CPU's polling onto the nibble grid. With perfectly regular
pulses, the landing point is the same on every revolution. In POM2 a cell is
exactly 8 LSS cycles and a nibble exactly 32 CPU cycles, so it never lands
early.

Measured in POM2:

| Change | Result |
|---|---|
| Original grid, 60 s | never passes |
| Bit cells on the true bus clock (8.18 cycles), 150 s | 706 failures, never a double read |
| The A2R converted to a WOZ 2.1 FLUX image (one revolution per track) | still fails: the same revolution is replayed identically |

What is missing is the jitter: the drive's flux intervals vary from cell to
cell and from revolution to revolution, so the landing point wanders until it
lands early. MAME gets there by its non-integer cell. Other emulators get
there with explicit jitter (AppleWin #1021).

**Not fixed in this round.** The fix belongs to the 5.25" timeline re-basing
recorded above `DiskImage::lssCyclesPerCell()`: bit cells on the true LSS
clock plus a per-revolution, deterministically seeded read jitter of
±2 ticks (the A2R's measured spread). That changes the read timing of every
WOZ, so it needs its own pass with the protected corpus re-blessed. Backlog:
STORAGE-018.

## Other observations

- MAME fails The Newsroom ("DISK ERROR") and Lady Tut on a ][. POM2 boots both.
- AppleWin fails every FLUX image. POM2 boots all five.
- `tools/libretro_corpus.py` did not stage `apple2o.rom`, so a ][ profile was
  refused. It also failed to write its report for media outside the checkout.
  Both are fixed.
