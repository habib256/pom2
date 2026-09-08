# Third-party code, assets and dumps in POM2

POM2 itself is GPL-3.0-or-later (`LICENSE`). Everything below is what it
carries from elsewhere — in the source tree, in the packages, or fetched at
build/test time — with the licence it came under and where in this tree it
lives. If something is missing from this file, that is a bug: report it.

## Code ported into POM2's sources

Every port cites its upstream file and line range in a comment at the point
of use; these are the projects, not the individual files.

| Upstream | Licence | What POM2 took |
|---|---|---|
| [MAME](https://github.com/mamedev/mame) | GPL-2.0-or-later | The hardware models: 6502/65C02 timing, Disk II LSS and flux (`wozfdc`), IWM, 6522 VIA, AY-3-8910, SSI263, Mockingboard and Phasor, Z80 core, SoftCard, CFFA/ATA, Uthernet I CS8900A (itself from Spiro Trikaliotis' VICE model), 2IMG, TransWarp register semantics, the Apple II reset architecture. GPL-2.0-or-later upgrades to POM2's GPL-3.0 without friction. |
| MAME `samples/floppy` | BSD-3-Clause | The floppy mechanical sound samples, `roms/floppy_samples/` (see its `README.txt`). |
| [AppleWin](https://github.com/AppleWin/AppleWin) | GPL-2.0-or-later | The Uthernet II / W5100 model, the AppleWin-HLE mouse card, the NTSC IIR-LUT colour path, the SSI263 phoneme PCM (`Ssi263PhonemeData`, which AppleWin ships under LGPL — compatible), the SY6551/SSC status semantics. |
| [OpenEmulator](https://github.com/openemulator/openemulator) | GPL | The composite NTSC demodulation shader (`NtscPostProcessor`, `OpenGLShader`), ported and documented as such. |
| [apple2js](https://github.com/whscullin/apple2js) | MIT | The 256-byte Disk II P6 sequencer table (`DiskIICard`, `SEQUENCER_ROM_16`) used as the embedded default when no `diskii_p6.rom` dump is present. |
| POM1 (the author's own) | GPL-3.0 | The HGR/DHGR paint and sprite editors, shared verbatim with POM1. |
| [MicroM8](https://paleotronic.com/software/microm8/) | *inspiration only* | The 3D voxel view and the rewind ring are POM2's own code, modelled on MicroM8's ideas; nothing was copied. |

## Libraries bundled in the tree or in the packages

| Library | Licence | Where |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) `docking` branch, commit `b334d19` (1.92.9) | MIT | Fetched at build time by `setup_imgui.sh` / `tools/fetch_imgui_pinned.sh` (`imgui_pin.env`); compiled into every binary. |
| [IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders) | zlib | `src/IconsFontAwesome6.h`, the icon code-point header. |
| [miniaudio](https://miniaud.io/) 0.11.25 | MIT-0 / public domain (dual) | `src/third_party/miniaudio.h` — the audio output device. |
| [stb_image_write](https://github.com/nothings/stb) 1.16 | MIT / public domain (dual) | `src/stb_image_write.h` — PNG export. |
| [GLFW](https://www.glfw.org/) 3.3+ | zlib | System package or vcpkg; linked into the desktop binaries, bundled in the AppImages. |
| [libslirp](https://gitlab.freedesktop.org/slirp/libslirp) | BSD-3-Clause | Optional user-mode NAT for the Uthernet I and the Uthernet II raw modes; built into the aarch64, Raspberry Pi and Pi 400 AppImages only (see README § *Ethernet, per platform*). |
| [Emscripten](https://emscripten.org/) 6.0.9 runtime | MIT / Expat | The JavaScript glue and the `.wasm` of the browser build. |

## Fonts (shipped in `fonts/`)

| Font | Licence | File |
|---|---|---|
| DejaVu Sans | Bitstream Vera licence + public-domain DejaVu changes (`fonts/LICENSE-DejaVu.txt`) | `fonts/DejaVuSans.ttf` |
| Font Awesome Free 6, Solid | SIL OFL 1.1 for the font file, CC BY 4.0 for the icon designs, MIT for the CSS (`fonts/LICENSE-FontAwesome.txt`) | `fonts/fa-solid-900.ttf` |

The licence files ship next to the fonts in every package (`packaging/bundle.manifest` bundles `fonts/` whole).

## Photographs

Two photographs ship in the packages (`packaging/bundle.manifest` names
them): `pic/Apple_II_plus.jpg` (the About panel and the README) and
`pic/Keyboard_AppleIIe.jpeg` (the clickable //e keyboard panel, whose
hotspots were measured on it). The other files under `pic/` are working
material that no package carries.

**Provenance of the two shipped photographs is not recorded in this tree.**
Until the author states where they come from and under which terms, treat
them as *all rights reserved* by their unknown photographers; a package
maintainer who needs certainty should replace them with photographs of
known origin. (Filed in `TODO.md` § G1.)

## Test-time downloads (never shipped)

| Corpus | Licence | Use |
|---|---|---|
| [Tom Harte ProcessorTests](https://github.com/SingleStepTests/ProcessorTests) | MIT | The 6502 / 65C02 per-opcode oracle, a curated subset fetched at configure time (`tests/tomharte_*.manifest`). |
| Klaus Dormann's 6502 functional tests | GPL-3.0 | The CPU functional suite, fetched at test time. |
| zexall / zexdoc | GPL | The Z80 exerciser, fetched at test time. |
| [DIX](https://github.com/Fr3nchT0uch/DIX/) (French Touch) | GPL-3.0 | `disks_3.5/DIX.po`, the priority benchmark — and, as `floppyemu/DIX.po`, the disk the browser demo boots. Sources published by the authors. |

## ROM dumps and system software (shipped in `roms/`)

POM2 ships firmware dumps so that a package boots as downloaded, the way
established Apple II emulators do. That is a practice, not a licence: none of
the rights holders below has granted permission, and a redistributor who needs
one has to obtain it. The decision (2026-09-05, `TODO.md` § G1) is to keep the
dumps and say so plainly.

| Dump | Rights holder | Notes |
|---|---|---|
| `apple2o.rom`, `apple2p.rom`, `apple2.rom`, `apple2e.rom`, `apple2e_unenh.rom`, `apple2c-16K.rom`, `apple2c-32Kv0.rom`, `apple2cp.rom`, `3420033a.256`, `a2c.128` | Apple Computer, Inc. | System firmware for every profile. |
| `apple2_char.rom`, `apple2e_char*.rom` (fifteen character generators, incl. the French, German, UK and Canadian variants and the French Touch block-ASCII set) | Apple Computer, Inc. (the block-ASCII set: French Touch) | Character generators. |
| `disk2.rom`, `disk2_13.rom`, `diskii_p6.rom`, `diskii_p6_13.rom` | Apple Computer, Inc. | Disk II boot PROM and P6 sequencer, 16- and 13-sector. |
| `mouse_341-0269.bin`, `mouse_341-0270-c.bin` | Apple Computer, Inc. | Mouse Card MCU and EPROM. |
| `341-0358-A.bin` | Apple Computer, Inc. | Apple II Workstation Card firmware. |
| `341-0438-a.bin`, `342-0274-a.e9`, `342-0326-a.f12` | Apple Computer, Inc. | //c-class and peripheral firmware. |
| `liron.rom` | Apple Computer, Inc. | Liron / Apple Disk 3.5 controller EPROM, from the public BMOW/Yellowstone dump. |
| `cffa20ee02.bin`, `cffa20eec02.bin` | R&D Automation (Rich Dreher) | CFFA 2.0 firmware, distributed by its author on his site; no written permission on record here. |
| `grappler_plus.bin` | Orange Micro, Inc. (defunct) | Grappler+ EPROM; no permission on record. |
| `thunderclock_u9_v1.3.bin` | Thunderware, Inc. (defunct) | ThunderClock+ EPROM; no permission on record. |
| `Videx Lower Case Chip ROM.bin` | Videx, Inc. | Lower-case character chip; no permission on record. |

Apple system software on disk (the DOS 3.x masters, `AppleShare IIe
Workstation.po`, Apple Présente //c) sits in the repository under
`disks_5.4/` and `disks_3.5/`, which no package ships (`deny` list in
`packaging/bundle.manifest`). Commercial titles were removed from the working
tree on 2026-09-05; they remain in the git history, which `TODO.md` § G1
records as an open decision.
