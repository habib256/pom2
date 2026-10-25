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

Three photographs ship in the packages, as named in
`packaging/bundle.manifest`: `pic/Apple_II_plus.jpg` (About),
`pic/Keyboard_AppleIIe.jpeg` (the clickable //e keyboard) and
`pic/Keyboard_AppleIIc.png` (the //c-class keyboard). The October 13 changelog
records the //c image as a supplied photo; that alone does not record its
photographer or redistribution terms. Packages also carry `pic/retro/`,
whose illustration notices are described separately below. Other reference
photographs under `pic/` are not included by the manifest.

The keyboard's right-arrow legend was corrected with an image-generation
edit in October 2026; the image dimensions and hotspot coordinates are
unchanged. This edit does not establish the original photograph's provenance.

**Provenance and redistribution terms of these three photographs are not
recorded in this tree.**
Until the author states where they come from and under which terms, treat
them as *all rights reserved* by their unknown photographers; a package
maintainer who needs certainty should replace them with photographs of
known origin. (Filed in [`docs/backlog/release-1.0.md` § G1](docs/backlog/release-1.0.md#g1).)

## Retro monitor and drive illustrations

`pic/retro/` ships the Monitor II/Disk II artwork derived from
Oscar Casamitjana's apple2e-emulator at commit
`dd1c57a24e35d179726a51401581d3f313fd3793`, with its
[MIT license](pic/retro/LICENSE), [attribution/adaptation notice](pic/retro/NOTICE.txt)
and editable SVG sources. The notice also describes POM2's Apple 3.5,
Disk //c and hard-disk front illustrations and reuse of the MIT rainbow
geometry. Their reference photographs are not distributed in this directory.

## Test-time downloads (never shipped)

| Corpus | Licence | Use |
|---|---|---|
| [Tom Harte ProcessorTests](https://github.com/SingleStepTests/ProcessorTests) | MIT | The 6502 / 65C02 per-opcode oracle, a curated subset fetched at configure time (`tests/tomharte_*.manifest`). |
| Klaus Dormann's 6502 functional tests | GPL-3.0 | The CPU functional suite, fetched at test time. |
| zexall / zexdoc | GPL | The Z80 exerciser, fetched at test time. |
| [DIX](https://github.com/Fr3nchT0uch/DIX/) (French Touch) | GPL-3.0 | `disks_3.5/DIX.po`, the priority benchmark. Sources published by the authors. |

## Bundled disks (`bundled_disks/`)

| Disk | Licence | Source |
|---|---|---|
| A2FC XL v0.9.8 (6502 edition) | GPL-3.0 | [A2FileCommander](https://github.com/habib256/a2filecmd/releases/tag/v0.9.8) |
| DIX-fix | GPL-3.0 | [French Touch DIX](https://github.com/Fr3nchT0uch/DIX/); reproduced with `tools/make_dix_fix.py`. Browser default disk. |
| Dirtbike 1.1 | GPL-3.0 | [POM2 games](https://github.com/habib256/pom2games/releases/tag/dirtbike-1.1) |
| ChromaBreak 1.1 | GPL-3.0 | [POM2 games](https://github.com/habib256/pom2games/releases/tag/chromabreak-1.1) |
| Micro-Sokoban 1.3 | GPL-3.0 | [POM2 games](https://github.com/habib256/pom2games/releases/tag/micro-sokoban-1.3) |

The shipped README records machine requirements and source links; SHA256SUMS
records the pristine images. User disk libraries remain excluded.

## ROM dumps and system software (shipped in `roms/`)

POM2 ships firmware dumps so that a package boots as downloaded, the way
established Apple II emulators do. That is a practice, not a licence: none of
the rights holders below has granted permission, and a redistributor who needs
one has to obtain it. The decision (2026-09-05, [`docs/backlog/release-1.0.md` § G1](docs/backlog/release-1.0.md#g1)) is to keep the
dumps and say so plainly.

| Dump | Rights holder | Notes |
|---|---|---|
| `apple2o.rom`, `apple2p.rom`, `apple2.rom`, `apple2e.rom`, `apple2e_unenh.rom`, `apple2c-16K.rom`, `apple2c-32Kv0.rom`, `apple2cp.rom`, `3420033a.256`, `a2c.128` | Apple Computer, Inc. | System firmware for every profile. |
| `apple2_char.rom`, `apple2e_char*.rom` (fifteen character generators, incl. the French, German, UK and Canadian variants and the French Touch block-ASCII set) | Apple Computer, Inc. (the block-ASCII set: French Touch) | Character generators. |
| `disk2.rom`, `disk2_13.rom`, `diskii_p6.rom`, `diskii_p6_13.rom` | Apple Computer, Inc. | Disk II boot PROM and P6 sequencer, 16- and 13-sector. |
| `mouse_341-0269.bin`, `mouse_341-0270-c.bin` | Apple Computer, Inc. | Mouse Card MCU and EPROM. |
| `ssc_341-0065-a.bin` | Apple Computer, Inc. | Super Serial Card EPROM (341-0065-A). |
| `341-0358-A.bin` | Apple Computer, Inc. | Apple II Workstation Card firmware. |
| `341-0438-a.bin`, `342-0274-a.e9`, `342-0326-a.f12` | Apple Computer, Inc. | //c-class and peripheral firmware. |
| `liron.rom` | Apple Computer, Inc. | Liron / Apple Disk 3.5 controller EPROM, from the public BMOW/Yellowstone dump. |
| `cffa20ee02.bin`, `cffa20eec02.bin` | R&D Automation (Rich Dreher) | CFFA 2.0 firmware, distributed by its author on his site; no written permission on record here. |
| `grappler_plus.bin` | Orange Micro, Inc. (defunct) | Grappler+ EPROM; no permission on record. |
| `grappler_eps-1.bin` | Orange Micro, Inc. (defunct) | Original Grappler (1981) EPROM, from MAME's a2grappler set; no permission on record. |
| `341-0057.bin` | Apple Computer, Inc. | Apple II Parallel Interface Card PROM. |
| `thunderclock_u9_v1.3.bin` | Thunderware, Inc. (defunct) | ThunderClock+ EPROM; no permission on record. |
| `ae_transwarp_1.4.bin` | Applied Engineering (defunct) | TransWarp boot firmware (MAME `warprom`); no permission on record. |
| `Videx Lower Case Chip ROM.bin` | Videx, Inc. | Lower-case character chip; no permission on record. |
| `videx_videoterm_v24_60hz.bin`, `videx_videoterm_v24_50hz.bin`, `videx_videoterm_char_normal.bin`, `videx_videoterm_char_inverse.bin` | Videx, Inc. | Videoterm firmware v2.4 (both CRTC tables) and character generators, from MAME's `a2vidtrm` set; no permission on record. |

Apple system software on disk (the DOS 3.x masters, `AppleShare IIe
Workstation.po`, Apple Présente //c) sits in the repository under
`disks_5.4/` and `disks_3.5/`, which no package ships (`deny` list in
`packaging/bundle.manifest`). Commercial titles were removed from the working
tree on 2026-09-05; they remain in the git history, which
[`docs/backlog/release-1.0.md` § G1](docs/backlog/release-1.0.md#g1)
records as an open decision.

## MAME TMS5220C synthesis core and coefficient ROM

`src/Tms5220.cpp`, `src/Tms5220.h` and `src/Tms5220Tables.h` adapt
MAME's `src/devices/sound/tms5220.cpp`, `tms5220.h` and `tms5110r.hxx`,
revision `d404316732fb5970cc0170aefc69ef1a85e14fce` (retrieved 2026-10-18).
These components are BSD-3-Clause; the other MAME-derived models keep their
existing individual licenses. POM2 removes the unrelated device framework
and VSM wiring, retaining the TMS5220C LPC interpolation, lattice, chirp/noise
and DAC path used by the Echo+ board. Card bus mapping follows
`src/devices/bus/a2bus/a2mockingboard.cpp` at the same revision.

```text
Copyright (c) Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu,
Raphael Nabet, Couriersud and Michael Zapf.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```
