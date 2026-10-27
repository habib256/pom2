# Protected-disk integration evidence — 2026-10-12

POM2 reached gameplay with the WOZ-a-day images of **Captain Goodnight**
and **Prince of Persia**. This is a manual, visually checked integration
result, not certification of every protection check or a completed game.

## Inputs and machine

Images were fetched from the [WOZ-a-day preservation mirror](https://mirrors.apple2.org.za/ftp.apple.asimov.net/images/games/collections/woz-a-day/),
extracted under `/tmp/pom2-protected-validation-20261012`, and not added to
the repository or package. META identifies the collection as `wozaday`;
the creator is `Passport.py by 4am`, rather than a sector-image conversion
performed for this test.

| Image | Format | SHA-256 |
|---|---|---|
| Captain Goodnight side A | WOZ2, INFO v2 | `85dde3ce4b6ca17cb30db8d0be060c916092332853568abb8d941594571aa332` |
| Captain Goodnight side B | WOZ2, INFO v2 | `6f522c7d0cb9919db127a864b440acb7d18bfc25d422dba26b43268ca732d465` |
| Prince of Persia side A | WOZ1, INFO v1 | `7e31597f1b1c11a025238fa75027150c1b4329936bbeb4da7e33134b8ac8cdb7` |
| Prince of Persia side B | WOZ1, INFO v1 | `091e5186128fb3e0270ad262af1ee7a46d951245f72729e7c65ed8ef9ec78ee4` |

Both runs use a 128 KB Apple IIe, bundled `apple2e.rom` and
`apple2e_char.rom`, slot-6 `disk2.rom`, embedded P6 sequencer, NTSC, and
Disk II without IWM hooks. Write-back is disabled explicitly. Captain
Goodnight uses NMOS 6502; Prince of Persia uses CMOS 65C02.

## Observed milestones

| Run | Evidence |
|---|---|
| Captain Goodnight side A | At 30 emulated seconds, the Broderbund/Captain Goodnight title is visible. SPACE at 30 s reaches `TURN DISK OVER AND PRESS ANY KEY`. Sampled head positions span quarter tracks 0–84. |
| Captain Goodnight side B | Replace drive 1 at 40 s and send SPACE again. At 60 s the helicopter/landing scene, fuel gauge and `CAPTAIN GOODNIGHT HAS ARRIVED!` are visible. The next loading step reaches quarter track 20. HGR page flips continue through 90 s. |
| Prince of Persia side A | At 30 s the palace/title screen is visible. SPACE at 30 s triggers further disk reads (head moves from quarter track 136 to 132). At 60 s the first dungeon room and the prince are visible. |

Captain side A contains 49 populated TMAP positions, including half-track
positions, and 46 distinct bit lengths. Both Prince of Persia images have
35 populated positions, all on whole tracks. The selected PoP capture
therefore does **not** validate quarter-track support merely by booting.

The probe also records a misleading existing warning for Captain side A:
`13-sector disk mounted ... it won't boot`, despite INFO specifying
16-sector boot and the successful title/gameplay. `DiskImage::loadWoz`
currently sniffs for a 13-sector prologue even when INFO declares format 1.
This diagnostic/routing issue remains open; no ROM workaround was needed
for the above runs.

## Reproduce

From the source root:

```sh
cmake --build build --target protected_disk_boot_probe -j6

build/tests/protected_disk_boot_probe '/path/Captain Goodnight side A.woz' \
  /tmp/captain-evidence 90 30 ' ' nmos '/path/Captain Goodnight side B.woz' 40

build/tests/protected_disk_boot_probe '/path/Prince of Persia side A.woz' \
  /tmp/prince-evidence 60 30 ' ' cmos
```

Output directories must be new. The probe writes `timeline.csv`, PPM screen
captures and 128 KB main/aux RAM dumps every five seconds, at input/swap
times and at the end. Head extrema are sampled after CPU slices of up to
256 cycles; they are diagnostics, not an exhaustive stepper trace. The
captures precede input or media changes at the corresponding timestamp.
It is included in `make probes`, and deliberately has no automatic
pass/fail test for commercial software. Exit 0 means the diagnostic ran.

Local evidence from this session is in `/tmp/pom2-captain-boot-2` and
`/tmp/pom2-prince-boot-1`; these temporary files are not permanent fixtures.

Still unvalidated: PoP side B and later levels, anti-copy consequences
late in gameplay, completion of either game, other editions/captures,
and comparison against physical hardware. Synthetic flux/cadence tests
remain the automated regression gates.

## Rerun 2026-10-27

The same four images (SHA-256 identical to the table above, fetched again
from the WOZ-a-day mirror into a scratch directory) and the same two probe
command lines, on a tree that had just moved the Disk II's LSS, spindle and
stepper to stock bus time and given `.nib` images MAME's sync rule. Every
milestone above reproduced: Captain side A's head range 0–84 and the side-B
`CAPTAIN GOODNIGHT HAS ARRIVED!` scene at 60 s, HGR page flips through
90 s ending on quarter track 20; Prince of Persia's title at 30 s and the
first dungeon room with the prince at 60 s.

