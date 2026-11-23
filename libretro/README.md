# POM2 for RetroArch

POM2 emulates the Apple ][, ][+, //e, //e Enhanced, //c and //c Plus, each
in NTSC (60 Hz) or European PAL (50 Hz) timing. It starts in BASIC or boots
a 5.25-inch floppy, a 3.5-inch SmartPort disk or a ProDOS hard disk. A
Mockingboard, an AppleMouse and a Le Chat Mauve RGB card can be added, and
the picture can be any of eleven monitors, from a composite colour TV to
a green phosphor screen. The DIX profile adds PAL timing, a stereo
Mockingboard in slot 2 and a Le Chat Mauve Féline RGB monitor in slot 7.

## Build and install

If you have a release ZIP for your operating system and architecture, put
the file from `cores/` in RetroArch’s **Cores** directory, the file from
`info/` in **Core Info** and the `system/pom2` folder in **System/BIOS**.
Find these locations under **Settings → Directory**.

From a fresh source checkout, use CMake 3.16 or later and a native C++17
toolchain. The module-only build uses the same core as the SDK and skips
the desktop and browser frontends, including their ImGui setup and
GLFW, OpenGL and vcpkg dependencies. Set `POM2_ENABLE_SLIRP=OFF` to omit
the optional networking backend.

```sh
cmake -S . -B build-libretro -DCMAKE_BUILD_TYPE=Release -DPOM2_LIBRETRO_ONLY=ON -DPOM2_ENABLE_SLIRP=OFF
cmake --build build-libretro --config Release --target pom2_libretro --parallel
```

Copy `pom2_libretro.dylib` (macOS), `pom2_libretro.so` (Linux) or
`pom2_libretro.dll` (Windows) into RetroArch's **Cores** directory. Copy
`pom2_libretro.info` into its **Core Info** directory. Both files are in the
build directory, or its `Release` subdirectory with a multi-configuration
generator. RetroArch lists these directories under **Settings → Directory**.

For a combined desktop and module build, use `-DPOM2_BUILD_LIBRETRO=ON`
with the [desktop build dependencies](../README.md#build-it).

With the [C++ SDK](../docs/SDK.md) installed from the same checkout, the
frontend can also be configured on its own:

```sh
cmake -S libretro -B build-retro -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build build-retro --config Release --parallel
```

## Firmware and disks

In RetroArch's **System/BIOS** directory, create a subdirectory named
`pom2` and put the ROMs there. Names are case-sensitive on Linux. You need
the system ROM and the character ROM of at least one machine; the
**Machine** option set to Automatic picks the first one present in the order
of this table.

| Machine | System ROM | Character ROM |
|---|---|---|
| Apple //e Enhanced | `apple2e.rom` | `apple2e_char.rom` |
| Apple //e | `apple2e_unenh.rom` or `342-0135-b.64.rom` | `apple2e_char_2k.rom` |
| Apple //c | `apple2c-32Kv0.rom`, `apple2c-16K.rom` or `3420033a.256` | `apple2e_char.rom` |
| Apple ][+ | `apple2p.rom` or `apple2.rom` | `apple2_char.rom` |
| Apple //c Plus | `apple2cp.rom` | `apple2e_char.rom` |
| Apple ][ (1977) | `apple2o.rom` | `apple2_char.rom` |

A WOZ 2 disk names the machines it runs on. With Automatic, the core starts
the first of them whose ROMs are present. The original Apple ][ has no
autostart ROM: it powers on to garbage and the monitor's `*`, as the real
one did.

POM2's built-in Disk II PROMs can boot every disk. To use Apple's, put
`disk2.rom` (P5) and `diskii_p6.rom` (P6) in the same `pom2` subdirectory.
Reference firmware SHA-256 hashes are in the
[test media manifest](../tools/test_media.sha256).

In RetroArch, choose **Load Core → Apple II (POM2)**. Choose **Start Core**
to reach the BASIC prompt. Try `PRINT 2+2` and press Return. To boot a disk,
use **Load Content** and select an image. The type and size choose the drive:

| Image | Drive |
|---|---|
| `.dsk`, `.do`, `.po` (140 KiB), `.nib`, `.woz`, `.d13`, 5.25-inch `.2mg` | Disk II, slot 6 |
| `.po` or `.2mg` of 400 or 800 KiB | 3.5-inch SmartPort, slot 5 |
| `.hdv`, or a larger `.po` or `.2mg` | ProDOS hard disk, slot 7 |

On a //c the 3.5-inch and hard disks hang off the rear disk port and boot
through the //c's own firmware.

A `.zip` or `.gz` opens as its contents. A zip with one disk loads that
disk; with several, a playlist of them in name order, ready for Disc Control;
with its own `.m3u`, that playlist. The core unpacks it once into
`pom2/archives` in the save directory, so its disks keep their private copies
from one session to the next.

A program loads without a disk, at the BASIC prompt:

| File | Loaded | Started with |
|---|---|---|
| `.bas` | an Applesoft listing in text, tokenized to `$0801` | `RUN` |
| `.bin` | at the address its name gives (`GAME#062000.bin`, CiderPress's convention), its AppleSingle or DOS 3.3 header gives, or `$0803` | `CALL` to that address |

## Core options

Open **Quick Menu → Core Options**. The options are grouped in five
categories, and RetroArch shows them in French when its language is French.
Options marked *(restart)* take effect at the next **Load Content** or
**Restart**; the others apply at once.

| Category | Option | Values |
|---|---|---|
| System | Machine *(restart)* | Automatic, //e Enhanced, //e, ][+, ][, //c, //c Plus |
| | Video standard *(restart)* | Automatic, NTSC 60 Hz, PAL 50 Hz |
| | Processor *(restart)* | The machine's own, NMOS 6502, 65C02 (][, ][+ and //e) |
| | Accelerator *(restart)* | Automatic (the //c Plus at 4 MHz), None, Zip Chip, FASTChip |
| | No-Slot Clock *(restart)* | Emulated time, Host clock, Absent |
| | Auxiliary memory *(restart)* | 64 KiB, RamWorks 256 KiB, 512 KiB, 1, 3 or 8 MiB (//e) |
| | DIX preset *(restart)* | Automatic, Always, Never |
| Video | Display | Automatic, three composite colour tables, OpenEmulator composite signal, AppleWin NTSC monitor, TV or idealized, monochrome white, green or amber, RGB |
| | Le Chat Mauve RGB card *(restart)* | Automatic (DIX), Absent, Installed |
| Audio | Sound card *(restart)* | Mockingboard A/C, Mockingboard with SSI263 speech, Phasor, Absent |
| | Mockingboard slot *(restart)* | Automatic (4, or 2 beside the mouse), 4, 5, 2, 1, 7 (the Phasor's too) |
| | Speaker volume, Mockingboard volume | 0 to 200 % |
| | Disk drive sounds *(restart)* | Off, On |
| | Disk drive sound volume | 0 to 200 % |
| Input | Analog dead zone | 0 to 30 % |
| | Analog sensitivity | 50 to 200 % |
| | Joystick axes | Normal, invert X, Y or both, swap X and Y |
| | Swap joystick buttons | No, Yes |
| | Alt keys are the Apple keys | Yes, No |
| | Uppercase keyboard | Automatic (][ and ][+), Always, Never |
| | AppleMouse in slot 4 *(restart)* | Absent, Installed |
| | AppleMouse model *(restart)* | Automatic, Apple MCU firmware, AppleWin model |
| | Mouse speed | 25 to 300 % |
| Disks | Fast disk loading | Enabled, original drive timing |
| | Disk II drive 2 *(restart)* | Empty, next disk of the playlist |
| | Disc Control drive | Drive 1, Drive 2 |
| | Write-protect every disk | No, Yes |
| | Disk save interval | 1 to 30 seconds |
| | Disk II firmware *(restart)* | Apple's when present, POM2's own |

Disk drive sounds play MAME's recordings of the motor, the head and the
door. The release ZIP carries them in `system/pom2/floppy_samples`: copy
that folder next to the ROMs. With run-ahead each head step is heard once, on
the frame RetroArch shows, and loading a state picks up the motor where the
state left it.

Every configuration has save states, the //c Plus, the speech Mockingboard
and hard disks included. A hard disk's state holds only the blocks the game
changed since the original image, up to 8 MiB of them; a //c's hard disk on
its rear port is the exception. RetroArch
says so in the notification it shows when the content starts, which also
names the machine and its cards.

## Controllers

Choose the controller type in **Quick Menu → Controls → Port 1 Controls →
Device Type**.

| Port 1 type | D-pad and left stick | B | A |
|---|---|---|---|
| Apple II joystick (default) | Joystick axes | Button 0 | Button 1 |
| Keyboard: arrows | Arrow keys, repeating while held | Space | Return |
| Keyboard: I J K M | I, J, K, M | Space | Return |
| Keyboard: W A S D | W, A, S, D | Space | Return |

In every mode Start types Return, Select types Escape, L3 opens the
on-screen keyboard and R3 inserts the next disk of a playlist. Port 2's
joystick drives paddles 2 and 3 and push button 2, for two-player paddle
games.

## Achievements and cheats

The core gives RetroArch a memory map in the layout RetroAchievements uses
for the Apple II: main RAM at `$00000`, the language card's `$D000` bank 1
at `$0C000`, bank 2 at `$0D000` and its upper RAM at `$0E000`, then the same
in auxiliary RAM from `$10000`. Achievement sets written for that layout and
RetroArch's own cheat search work on it.

Cheats use the same addresses, in hexadecimal: `0400:C1` writes `$C1` at
`$0400` before every frame, `0400?C5:C1` only while the byte holds `$C5`,
and `AAAAAAVV` is the eight-digit form of cheat files. Join several codes
with `+`.

## Save and resume

Save normally inside the game. POM2 stores each writable side in its own
private copy under `pom2/disks/` in RetroArch's **Save Files** directory.
The original images retain their original bytes. Reopen the same image or
playlist from the same location to resume using those disks. A locked image
or read-only file reports write protection to the guest.

**Quick Menu → Save State / Load State** captures the running machine,
including its active disk, keyboard, audio and position within the video
frame. This works with BASIC, Disk II, DIX and Apple II DeskTop.
Enable **Settings → Frame Throttle → Rewind** to move back through play.
State availability depends on the attached cards. Use in-game disk saves
for configurations where RetroArch reports that states are unavailable.
Disk changes made on another side persist across a rewind. When two
instances open the same saved side, the first holds the write lock and the
second can read its latest committed data.
Restore a state with the same media paths, playlist order, firmware and
hardware configuration that produced it.

The state buffer is fixed for each play session. Starting BASIC still
reserves room for a hot-inserted 800 KiB SmartPort disk. For a larger WOZ,
load it directly or list it in the playlist at startup so the core can
reserve enough space. A disk that outgrows the buffer can still be played,
and the first failed save reports the limit. Embedded WOZ snapshots support
images up to 16 MiB. Apple II DeskTop snapshots preserve SmartPort 2IMG
sector data and AppleMouse motion. Writable 2IMG disks use the existing
private working copy for write-back after restoration.

## Change disks during a game

For a game with multiple disks, open an `.m3u` playlist through **Load
Content**. Put one disk path on each line, in playing order. Relative paths
start in the playlist's folder. To label a disk in the menu, add a vertical
bar and a name after its path. A playlist for a two-sided game might look
like this.

```text
Adventure A.dsk|Side A
Adventure B.dsk|Side B
```

When the game asks for the other side, open **Quick Menu → Disc Control**,
choose **Eject Disc**, change **Current Disc Index**, then **Insert Disc**.
Resume the game to continue from the same point. If you started from a single
disk, **Load New Disc** lets you select another image during play.
[RetroArch disc swapping guide](https://docs.libretro.com/guides/disc-swapping/).

Games that use two 5.25-inch drives (Ultima, Wizardry, The Bard's Tale) can
have both: set **Core Options → Disk II drive 2** to **Next disk of the
playlist** and reload; the playlist's second disk starts in drive 2. Set
**Disc Control drive** to **Drive 2** to change the disk in drive 2 with Disc
Control and R3, and back to **Drive 1** for the boot drive. Each drive's disk
keeps its own private copy, and a save state holds both.

Frontends with LED support can map **LED 0** to the Disk II read indicator.

## DIX and AppleMouse

Open the supplied DIX 800 KiB image. The core recognizes both documented
DIX-fix images by their contents and starts the PAL configuration. At the
menu, press Left then Return to start the automatic tour on the reference
image. Other releases can use **Core Options → DIX preset → Always**.
Reload the content after changing profiles. The framebuffer uses the card's
RGB palette, and the core delivers 44.1 kHz stereo at 50.08 video frames/s.

To use a mouse in Apple II DeskTop, set **Core Options → AppleMouse in slot
4** to **Installed**, reload the disk and move the host mouse. Put
`mouse_341-0270-c.bin` in the same firmware folder as the //e ROMs. With
`mouse_341-0269.bin` present the core also emulates the card's MCU firmware.

## Play Dirtbike

Select [DIRTBIKE.dsk](../packaging/disks/DIRTBIKE.dsk) as the content. Centre
the left stick during startup so the game calibrates the joystick. Use a
RetroPad on port 1. The text “DIRTBIKE – APPLE II+ 48K” appears while the
disk loads. At the graphical title, press Start to open the track menu and
Start again to begin the race.

**Quick Menu → Core Options → Fast disk loading** speeds up active floppy
reads by default. Choose **Original drive timing** to turn it off.

| Control | Apple II input | Dirtbike |
|---|---|---|
| Left stick or D-pad | Joystick axes | Lean left/right, move through menus |
| B | Push button 0 | Throttle, select |
| A | Push button 1 | Brake |
| Start | Return | Confirm |
| Select | Escape | Menu |
| L3 | Show/hide on-screen keyboard | Type a name |
| R3 | Insert the next disk in the list | Change sides |

With the on-screen keyboard open, use the D-pad to select a key and B to
type it. Y switches case and symbols, X deletes, Select types a space and
Start enters Return and closes the keyboard. The **Analog paddle dead zone**
and **Analog paddle sensitivity** core options take effect during play.

The computer keyboard also works. In Dirtbike, J/L lean, A applies throttle
and Space brakes. If RetroArch intercepts letter keys as hotkeys, turn on
**Game Focus** (Scroll Lock by default).

The module uses the MIT-licensed
[libretro API header at ae22debd](https://github.com/libretro/libretro-common/blob/ae22debd425c33fe355ac2e51d9a140d7e8b3fa6/include/libretro.h).

## Compatibility and distribution

The [compatibility report](../docs/audits/libretro-compatibility-2026-11-23.md)
lists the firmware, media, guest actions, timings and reproduction commands
for POM2 and AppleWin. To replay the complete DIX tour through the built
module, run this command from the source checkout.

```sh
python3 tests/libretro_profiles.py build-libretro/pom2_libretro.dylib roms dix packaging/disks/DIX-fix.po --full
```

The [native package workflow](../.github/workflows/libretro.yml) builds
Linux x64, Linux arm64 (Raspberry Pi and other ARM boards), macOS arm64,
Windows x64 and Android arm64 (`pom2_libretro_android.so`, built with the NDK
and not run in CI). Each runner checks the extracted
module, starts Dirtbike in RetroArch, closes the process during boot and
verifies that the next launch resumes from the saved state. Core releases
use the `libretro-v*` tag series and include checksums and verification
results. The core metadata, firmware names and user guide are ready for
upstream review. Inclusion in the Core Downloader is handled by Libretro.
[Upstream core integration](https://docs.libretro.com/development/cores/developing-cores/#add-your-core-to-libretro-infrastructure).
