<div align="center">

# 🍏 POM2 v1.0.0 — Apple II Family Emulator

[![Try POM2 directly in your Browser](https://img.shields.io/badge/Try%20POM2%20directly%20in%20your%20Browser-Play%20now-blue?style=for-the-badge)](https://habib256.github.io/pom2/wasm/)

**Live demo — one click, no installation.**

### A floppy disk. A glowing screen. One more game.

Play, explore and create on the Apple II — on your desktop or in your browser.
POM2 is written in modern C++ with a Dear ImGui interface and 6502/65C02
cores whose per-instruction cycle counts match the Tom Harte test vectors.

[![Download POM2](https://img.shields.io/badge/Download-POM2-2ea44f?style=for-the-badge)](https://github.com/habib256/pom2/releases/latest)

![POM2 running Wilderness on a PAL Apple //c, with a Monitor II frame, keyboard and drive controls](pic/POM2-default-desktop.png)

Linux · macOS · Windows · Raspberry Pi · WebAssembly · GPL-3.0

</div>

**[Start playing](#start-playing) · [Technical overview](#technical-overview) ·
[Bundled disks](#bundled-disks) · [Build it](#build-it) · [Documentation](docs/README.md)**

> **v1.0.0:** [What's new](docs/releases/v1.0.0.md) ·
> [Downloads and checksums](https://github.com/habib256/pom2/releases/latest)

Untagged changes on `main` after a release are described in [CHANGELOG.md](CHANGELOG.md);
tagged packages follow their release notes.

## Start playing

**In a browser:** [launch POM2](https://habib256.github.io/pom2/wasm/).
It opens a PAL Apple //c with the DIX-fix demo disk. The browser build follows
`main`, so it may differ from a tagged release.
Drop a disk image from your computer onto the browser canvas to boot it,
or onto an empty drive to insert without rebooting.
**File → Import disk and boot...** also opens a local file selector.
Save a session or enable Automatic resume to keep supported disks and changes;
see [browser disk import](docs/USER-GUIDE.md#browser-disk-import) for limits.

**On your computer:** [download a package](https://github.com/habib256/pom2/releases/latest),
unpack it and launch POM2. Firmware, fonts and artwork are included. Keep the
executable and its accompanying folders together.

| Platform | Package |
|---|---|
| Linux x86-64 | `POM2-v<version>-x86_64.AppImage` |
| Linux ARM64 desktop | `POM2-v<version>-aarch64.AppImage` |
| Raspberry Pi 3–5 | `POM2-v<version>-raspberry-aarch64.AppImage` |
| Raspberry Pi 4 / Pi 400 | `POM2-v<version>-pi400-aarch64.AppImage` |
| macOS 10.15+ | `POM2-macOS-v<version>.dmg` — Intel and Apple Silicon |
| Windows 10/11 x64 | `POM2-Windows-v<version>.zip` |
| Self-hosted browser | `POM2-v<version>-web-wasm.zip` |

On Linux, make the AppImage executable before opening it:

```bash
chmod +x POM2-v1.0.0-x86_64.AppImage
./POM2-v1.0.0-x86_64.AppImage
```

Then pick a machine in **Machine → Profile**, drop a disk image onto the
window, and play. POM2 accepts `.dsk`, `.do`, `.d13`, `.po`, `.nib`, `.woz`,
`.moof`, `.2mg` and `.hdv`. You can also open **File → Disk Library** to try
one of the included disks.

New to the Apple II? At the BASIC prompt, type `PRINT "HELLO, APPLE II!"`
and press Return.

### Missing ROMs and the ROM download

From the next release, POM2 redistributes no copyrighted firmware: desktop
packages carry no ROM at all, and the first time it starts the desktop
version offers to download them in one click. The browser demo, which
cannot download them, keeps the //c and //e ROMs that run the disks POM2
ships. The source repository has no ROM either (see [Build it](#build-it)).

If a ROM is missing or damaged, **Help → ROM Status** says which one, and
**Download missing ROMs** fetches it from POM2's
[ROM archive](https://archive.org/details/pom2_roms_202610) on archive.org, with
the [RetroBIOS](https://github.com/Abdess/retrobios) collection as a
fallback. POM2 checks every downloaded file's size and SHA-256 before
installing it.

Already have the dump under another name, or as a MAME romset `.zip`? Drop
it onto the POM2 window (or use **Import ROM file...** in ROM Status). POM2
recognises it by its SHA-256, installs it under the name it looks for, and
loads it at the next machine switch or restart. A file it cannot identify
is left alone.

> **Known problem in v1.0.0:** on 20–22 October 2026 RetroBIOS reorganised
> its collection and removed many of the files POM2 points to. From a
> folder with no ROM, the v1.0.0 button now installs only 19 of 45 dumps:
> enough for the //e and the ][, but not for the //c, the //c Plus, the
> mouse, the Super Serial Card, the Liron or the international character
> sets. This does not affect the regular packages, which ship every ROM.
> If you deleted `roms/` or use a ROM-free build, copy your own dumps into
> the folder ROM Status shows. The next release reads RetroBIOS at a fixed
> revision instead, and downloads all 49 dumps POM2 uses.

## Technical overview

- **Machines and memory.** Nine profiles span the Apple ][, ][+, unenhanced
  and enhanced //e, //c, //c Plus and three PAL variants. POM2 emulates the
  6502/65C02, Language Card, //e auxiliary memory and RamWorks III. Expansion
  options include Slinky RAM, TransWarp and Zip Chip.
- **CP/M.** Two Z80 cards run CP/M on the Apple: the Microsoft SoftCard
  (CP/M 2.2) and the ALS CP/M Card (CP/M Plus 3.01C, 6 MHz, its own 64 KB,
  booting from a floppy or from ProDOS on a hard disk, with a RAM disk).
  The `pom2_cpm` tool lists, extracts, adds and deletes files on CP/M disk
  images without cpmtools.
- **Video.** Text 40/80, lo-res, hi-res and their double-resolution modes
  follow mode changes during a scanline. Choose composite NTSC, AppleWin,
  monochrome or CRT rendering; Le Chat Mauve/Video-7 RGB, Videx Videoterm,
  VidHD and a 3D voxel view add other ways to see the machine.
- **Sound and input.** The speaker, cassette, drive sounds, stereo Mockingboard
  A/C, //c Mockingboard 4c, Phasor, SSI263 and Echo+ TMS5220C speech, and S.A.M. DAC share the
  mixer. Mouse, paddles, gamepads, 4play and SNES MAX are supported. Serial
  and parallel cards connect to an ImageWriter emulator with PNG/PDF export.
- **Storage.** Disk II, Sony 3½-inch, SmartPort/Liron, HDV, CFFA 2.0,
  CFFA3000 SmartPort, SuperDrive and Floppy Emu handle `.dsk`, `.do`, `.d13`,
  `.po`, `.nib`, `.2mg`, `.woz`, `.moof` and `.hdv`. Writable 5¼-inch WOZ 2.1
  includes FLUX tracks; 3½-inch GCR WOZ/MOOF captures support native IWM
  flux writes. SuperDrive exposes 400/800K GCR and 1.44 MB MFM images as
  ProDOS blocks; its block writes splice CRC-valid sectors in MFM MOOF
  captures, while GCR block services require export to a raw image.
- **Files and tools.** The shared ProDOS host folder preserves type/aux type
  in portable `NAME#TTAAAA` names, offers opt-in TXT line-ending conversion
  and saves concurrent edits in `.pom2-conflicts/`. Disk Library search,
  rewind, snapshots, a memory viewer, bank-aware debugger, conditional
  breakpoints, symbols and source correspondence, an interactive 6502/65C02
  assembler, Disk II flux inspection and CPU profiling support both play and
  development.
- **Connections and browser.** Desktop builds offer Uthernet I/II, FujiNet
  relay, TNFS and VSDrive. The WebAssembly build stores settings and layout in
  IndexedDB and offers direct disk-image import (File → Import disk and boot...),
  text upload, session import/export and optional
  [automatic resume for supported sessions](docs/session-format.md#using-it).
  Sessions reconstruct supported profiles, embedded firmware and cards, including
  writable floppy/block media and program tapes. Restored disks retain unsaved
  data and further guest writes in RAM; save another session to retain them.
  Its //c starts with a Mockingboard 4c and pauses while the tab is hidden.
  Host socket networking is unavailable in the browser.

Some cards expose a high-level service instead of running every original chip
or firmware. See the [LLE/HLE matrix](docs/lle_vs_hle.md) for each card's
emulation level and the [user guide](docs/USER-GUIDE.md) for compatibility,
write-back rules and setup.

## Bundled disks

Five disks are ready in **File → Disk Library**:

| Disk | Try it for |
|---|---|
| [A2 File Cmd XL v1.0](https://github.com/habib256/a2filecmd/releases/tag/v1.0) | A ProDOS file manager on a 32 MB hard-disk image; mount in an HDV/SmartPort bay. |
| [DIX-fix](https://github.com/Fr3nchT0uch/DIX/) | French Touch demos; use a PAL machine with 128 KB and Mockingboard. The recorded //e setup uses slots 4 (sound) and 5 (SmartPort); the browser uses a //c with Mockingboard 4c. |
| [Dirtbike 1.1](https://github.com/habib256/pom2games/releases/tag/dirtbike-1.1) | A motocross time trial across 30 tracks; use II+ or later with 48 KB. |
| [ChromaBreak 1.1](https://github.com/habib256/pom2games/releases/tag/chromabreak-1.1) | A colourful brick-breaking game. |
| [Micro-Sokoban 1.3](https://github.com/habib256/pom2games/releases/tag/micro-sokoban-1.3) | A small puzzle with many crates to move. |

[Disk setup and sources](packaging/disks/README.txt) ·
[SHA-256 checksums](packaging/disks/SHA256SUMS.txt)

## Handy controls

| Key | Action |
|---|---|
| **F6** (hold) | Rewind (enable Record in Machine → Rewind first) |
| **F10** | Toggle fullscreen |
| **F9** | Save a screenshot |
| **F11 / F12** | Soft reset / hard reset |
| **Ctrl+Shift+P** | Open the command palette |
| **Ctrl+V** | Paste text into the Apple II |

In fullscreen, **F1** opens the game menu and **F2** opens the on-screen
keyboard. The [user guide](docs/USER-GUIDE.md#controls) has the full list.

## Build it

On Linux or macOS, install Git, CMake 3.16 or newer, a C++17 compiler and GLFW, then:

```bash
git clone https://github.com/habib256/pom2.git
cd pom2
./setup_imgui.sh
python3 tools/fetch_test_media.py   # ROMs + test disks, from archive.org
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
./build/POM2
```

The repository carries no copyrighted ROM or third-party disk image.
`tools/fetch_test_media.py` downloads the ROMs and the disks the tests use
from POM2's [test-media archive](https://archive.org/details/pom2_testmedia_202610),
checking every file against `tools/test_media.sha256`; skip it and the
first launch offers the ROM download instead.

The same build makes `build/pom2_headless`, the machine without a window: it
boots a disk and serves the Super Serial Card over telnet, and can capture
the screen, type at given moments and plug a SoftCard or ALS card
(`--softcard`, `--als`) to test CP/M programs. `--help` lists the options.

[Windows, Raspberry Pi and WebAssembly instructions](docs/USER-GUIDE.md#build-from-source) ·
[Tests and contributing](CONTRIBUTING.md) ·
[Third-party notices](THIRD-PARTY.md)
