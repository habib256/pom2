<div align="center">

# 🍏 POM2 v0.9.7 — Apple II Family Emulator

[![Try POM2 directly in your Browser](https://img.shields.io/badge/Try%20POM2%20directly%20in%20your%20Browser-Play%20now-blue?style=for-the-badge)](https://habib256.github.io/pom2/wasm/)

**Live demo — one click, no installation.**

### A floppy disk. A glowing screen. One more game.

Play, explore and create on the Apple II — on your desktop or in your browser.
POM2 is written in modern C++ with a Dear ImGui interface and cycle-accurate
6502/65C02 cores.

[![Download POM2](https://img.shields.io/badge/Download-POM2-2ea44f?style=for-the-badge)](https://github.com/habib256/pom2/releases/latest)

![POM2 running Wilderness on a PAL Apple //c, with a Monitor II frame, keyboard and drive controls](pic/POM2-default-desktop.png)

Linux · macOS · Windows · Raspberry Pi · WebAssembly · GPL-3.0

</div>

**[Start playing](#start-playing) · [Technical overview](#technical-overview) ·
[Bundled disks](#bundled-disks) · [Build it](#build-it) · [Documentation](docs/README.md)**

> **v0.9.7:** [What's new](docs/releases/v0.9.7.md) ·
> [Downloads and checksums](https://github.com/habib256/pom2/releases/latest)

This checkout also includes untagged changes described in [CHANGELOG.md](CHANGELOG.md),
including v7 sessions that reconstruct their machine and a bank-aware debugger
with source correspondence. Tagged packages follow their release notes.

## Start playing

**In a browser:** [launch POM2](https://habib256.github.io/pom2/wasm/).
It opens a PAL Apple //c with the DIX-fix demo disk. The browser build follows
`main`, so it may differ from a tagged release.

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
chmod +x POM2-v0.9.7-x86_64.AppImage
./POM2-v0.9.7-x86_64.AppImage
```

Then pick a machine in **Machine → Profile**, drop a disk image onto the
window, and play. POM2 accepts `.dsk`, `.do`, `.d13`, `.po`, `.nib`, `.woz`,
`.moof`, `.2mg` and `.hdv`. You can also open **File → Disk Library** to try
one of the included disks.

New to the Apple II? At the BASIC prompt, type `PRINT "HELLO, APPLE II!"`
and press Return.

## Technical overview

- **Machines and memory.** Nine profiles span the Apple ][, ][+, unenhanced
  and enhanced //e, //c, //c Plus and three PAL variants. POM2 emulates the
  6502/65C02, Language Card, //e auxiliary memory and RamWorks III. Expansion
  options include the SoftCard Z80, Slinky RAM, TransWarp and Zip Chip.
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
  includes FLUX tracks; 3½-inch GCR WOZ/MOOF captures support native IWM flux writes. SuperDrive block
  writes splice CRC-valid sectors in MFM MOOF captures; GCR block services
  require export to a raw image.
  SuperDrive exposes 400/800K GCR and 1.44 MB MFM images as ProDOS blocks.
- **Files and tools.** The shared ProDOS host folder preserves type/aux type
  in portable `NAME#TTAAAA` names, offers opt-in TXT line-ending conversion
  and saves concurrent edits in `.pom2-conflicts/`. Disk Library search,
  rewind, snapshots, a memory viewer, bank-aware debugger, conditional
  breakpoints, symbols and source correspondence, an interactive 6502/65C02 assembler, Disk II flux inspection
  and CPU profiling support both play and development.
- **Connections and browser.** Desktop builds offer Uthernet I/II, FujiNet
  relay, TNFS and VSDrive. The WebAssembly build stores settings and layout in
  IndexedDB and offers text upload, session import/export and optional
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

Four disks are ready in **File → Disk Library**:

| Disk | Try it for |
|---|---|
| [A2 File Cmd XL v0.9.8](https://github.com/habib256/a2filecmd/releases/tag/v0.9.8) | A ProDOS file manager on a 32 MB hard-disk image; mount in an HDV/SmartPort bay. |
| [DIX-fix](https://github.com/Fr3nchT0uch/DIX/) | French Touch demos; use a PAL //e with Mockingboard in slot 4. |
| [ChromaBreak 1.0](https://github.com/habib256/pom2games/releases/tag/chromabreak-1.0) | A colourful brick-breaking game. |
| [Micro-Sokoban 1.2](https://github.com/habib256/pom2games/releases/tag/1.2) | A small puzzle with many crates to move. |

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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
./build/POM2
```

[Windows, Raspberry Pi and WebAssembly instructions](docs/USER-GUIDE.md#build-from-source) ·
[Tests and contributing](CONTRIBUTING.md) ·
[Third-party notices](THIRD-PARTY.md)
