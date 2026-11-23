# POM2 for RetroArch

POM2 emulates an Apple //e Enhanced. It starts in Applesoft BASIC or boots
a Disk II floppy in slot 6 or a 3.5-inch SmartPort disk in slot 5. The DIX
profile adds PAL timing, a stereo Mockingboard in slot 2 and a Le Chat Mauve
Féline RGB monitor in slot 7.

## Build and install

If you have a release ZIP for your operating system and architecture, put
the file from `cores/` in RetroArch’s **Cores** directory and the file from
`info/` in **Core Info**. Find both locations under **Settings → Directory**.

From a checkout with the [POM2 build dependencies](../README.md#build-it),
run:

```sh
cmake -S . -B build-libretro -DCMAKE_BUILD_TYPE=Release -DPOM2_BUILD_LIBRETRO=ON -DPOM2_ENABLE_SLIRP=OFF -DPOM2_ENABLE_TESTS=OFF
cmake --build build-libretro --config Release --target pom2_libretro --parallel
```

Copy `pom2_libretro.dylib` (macOS), `pom2_libretro.so` (Linux) or
`pom2_libretro.dll` (Windows) into RetroArch's **Cores** directory. Copy
`pom2_libretro.info` into its **Core Info** directory. Both files are in the
build directory, or its `Release` subdirectory with a multi-configuration
generator. RetroArch lists these directories under **Settings → Directory**.

With the [C++ SDK](../docs/SDK.md) installed from the same checkout, the
frontend can also be configured on its own:

```sh
cmake -S libretro -B build-retro -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build build-retro --config Release --parallel
```

## Firmware and disks

In RetroArch's **System/BIOS** directory, create a subdirectory named
`pom2` and put these two ROMs there. Names are case-sensitive on Linux.

| File | Firmware |
|---|---|
| `pom2/apple2e.rom` | Apple //e Enhanced system ROM |
| `pom2/apple2e_char.rom` | Apple //e character generator |

POM2's built-in Disk II PROMs can boot the example disk. To use Apple Disk II
PROMs, put `disk2.rom` (P5) and `diskii_p6.rom` (P6) in the same `pom2`
subdirectory. Reference firmware SHA-256 hashes are in the
[test media manifest](../tools/test_media.sha256).

In RetroArch, choose **Load Core → Apple II (POM2)**. Choose **Start Core**
to reach the BASIC prompt. Try `PRINT 2+2` and press Return. To boot a disk,
use **Load Content** and select an uncompressed image. Disk II accepts
`.dsk`, `.do`, `.po`, `.nib`, `.woz` and 5.25-inch `.2mg` or `.2img`.
A 400 or 800 KiB `.po` or 2IMG payload boots through SmartPort.

## Save and resume

Save normally inside the game. POM2 stores each writable side in its own
private copy under `pom2/disks/` in RetroArch's **Save Files** directory.
The original images retain their original bytes. Reopen the same image or
playlist from the same location to resume using those disks. A locked image
or read-only file reports write protection to the guest.

For BASIC and Disk II games, **Quick Menu → Save State / Load State**
captures the running machine, including the active disk and the keyboard.
Enable **Settings → Frame Throttle → Rewind** to move back through play.
State availability depends on the attached cards. Use in-game disk saves
for configurations where RetroArch reports that states are unavailable.

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

Frontends with LED support can map **LED 0** to the Disk II read indicator.

## DIX and AppleMouse

Open the supplied DIX 800 KiB image. The core recognizes both documented
DIX-fix images by their contents and starts the PAL configuration. At the
menu, press Left then Return to start the automatic tour on the reference
image. Other releases can use **Core Options → Machine profile → dix-pal**.
Reload the content after changing profiles. The framebuffer uses the card's
RGB palette, and the core delivers 44.1 kHz stereo at 50.08 video frames/s.

To use a mouse in Apple II DeskTop, set **Core Options → AppleMouse in slot
4** to **enabled**, reload the disk and move the host mouse. Put
`mouse_341-0270-c.bin` in the same firmware folder as the //e ROMs. With
`mouse_341-0269.bin` present the core also emulates the card's MCU firmware.

## Play Dirtbike

Select [DIRTBIKE.dsk](../packaging/disks/DIRTBIKE.dsk) as the content. Centre
the left stick during startup so the game calibrates the joystick. Use a
RetroPad on port 1. The text “DIRTBIKE – APPLE II+ 48K” appears while the
disk loads. At the graphical title, press Start to open the track menu and
Start again to begin the race.

**Quick Menu → Core Options → Fast disk loading** speeds up active floppy
reads by default. Choose **Disabled** to use original drive timing.

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
