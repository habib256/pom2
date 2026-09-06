<div align="center">

# 🍏 POM2 v0.9.0 — Apple II Emulator

### *Nine machines from 1977 to 1988, beam-raced to the scanline — then tilted into 3D and rewound through time.*

🎂 **Celebrating 50 years of Apple (1976 → 2026)** with a cycle-accurate Apple II family emulator: **9 one-click machine presets** (][ · ][+ · //e · //e enhanced · //c · //c+ · PAL //e unenhanced · PAL //e · PAL //c Le Chat Mauve), MAME-faithful CPU and hardware ports, OpenEmulator-grade composite NTSC, a MicroM8-style **3D voxel view**, **time-travel rewind**, mechanical floppy sounds, and a stack of expansion cards from Mockingboard to Phasor — all running in the browser too.

Built with Dear ImGui & OpenGL — fast, lightweight, cross-platform.

[![⬇ Download for your machine](https://img.shields.io/badge/⬇%20Download-Linux%20•%20macOS%20•%20Windows%20•%20Pi-2ea44f?style=for-the-badge)](https://github.com/habib256/pom2/releases/latest)
[![▶ Play in browser (no install)](https://img.shields.io/badge/▶%20Play%20in%20browser-WebAssembly-blueviolet?style=for-the-badge)](https://habib256.github.io/pom2/wasm/)

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Linux%20•%20macOS%20•%20Windows%20•%20Web-lightgrey.svg)](#-download)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-orange.svg)](#)
[![9 machines](https://img.shields.io/badge/Machines-9-success.svg)](#-machine-profiles)
[![MAME-parity](https://img.shields.io/badge/Hardware-MAME--faithful-yellowgreen.svg)](DEV.md)

![Apple II Plus](pic/Apple_II_plus.jpg)

</div>

---

## 🌟 Why POM2?

> *Most Apple II emulators give you a screen and a disk drive. POM2 gives you the bus, the beam, the phosphor — and a time machine.*

- 🎞️ **Beam-raced to the scanline.** Soft-switch flips mid-frame land on the exact scanline the CPU touched them — split-screen demos, hi-res/text mode swaps and AN3 DHGR toggles render correctly. The composite NTSC path beam-races too, so artifact colour follows the same event log the RGBA path does.
- 🧊 **Tilt the screen into 3D.** A MicroM8-style **voxel view** lifts the Apple II framebuffer off the glass into an orbiting 3D scene — pixels become extruded voxels you can fly around with a real camera.
- ⏪ **Rewind time.** A snapshot ring buffer records the machine as it runs; scrub backwards through your last seconds of execution and resume from any point. Same serializer feeds the AI-control HTTP `/snapshot` endpoints, so they can never drift.
- 📺 **CRT you can dial in.** OpenEmulator-style composite NTSC shader *and* AppleWin's CPU IIR-LUT NTSC, plus mono phosphor with an adjustable **phosphor curve** (luminance γ) and **persistence** (temporal glow), barrel distortion, hue/BCS — the full *Display → CRT Settings* panel.
- 💾 **Disks that sound right.** Cycle-stamped mechanical floppy samples: the Disk II head step and the Sony 3.5" drive whir, timed off the CPU clock — disk-turbo collapses wall-clock gaps but the nibble stream stays cycle-correct via an event-driven LSS.
- 🌐 **Online, from 1984.** The **Uthernet II**'s W5100 is a hardware TCP/IP stack, so POM2 runs it straight on host sockets — point a period IRC, telnet or FTP client at the real internet with no extra dependency and no privileges, on Linux, macOS **and Windows**. The **Uthernet I** (CS8900A) is there too for IP65 / Contiki, bridged by optional libslirp (Linux/macOS).
- 🎵 **A whole sound-card era.** Speaker, cassette, Mockingboard A/C, Mockingboard C **Sound II** with SSI263 speech, the Applied Engineering **Phasor** (2×VIA / 4×AY), and the Cricket / Echo SSI263 line.
- 🔬 **MAME is the source of truth.** Every hardware port cites the MAME file + line range in a comment and is pinned with a smoke test under `tests/`. CPU → audio/UI events carry a CPU-cycle stamp, never wall-clock.
- 🌐 **Runs in your browser.** The full emulator builds to WebAssembly — [play it now](https://habib256.github.io/pom2/wasm/), no install.

---

## ⚡ 60-second tour

Five things to try **right after first boot**:

1. **Boot a disk in one drag** → drop a `.woz`/`.dsk` on the window, or `POM2 path/to/game.woz`. POM2 routes it to Disk II, SmartPort or ProDOS HDV automatically.
2. **Switch machines live** → `Machine → Profile` (or `--preset iie`). Each switch is a clean cold reset that re-plugs built-in cards and re-mounts your disks.
3. **Tilt into 3D** → open the **3D voxel view** and orbit the running framebuffer with the camera. Lo-res, hi-res and text all extrude into voxels.
4. **Rewind** → let something run, then scrub the rewind ring backwards and resume from an earlier instant (a UI feature — note the CLI `--rewind` is unrelated: it rewinds the cassette tape).
5. **Tune the CRT** → `Display → CRT Settings`: swap composite NTSC ↔ mono phosphor, push the phosphor curve and persistence, add scanline glow and barrel.

---

## ⬇️ Download

**[→ Latest release](https://github.com/habib256/pom2/releases/latest)** — pick
the package for your machine. Each one is self-contained: unpack it, open it,
and the emulator is ready to boot a disk. Nothing else to install, no runtime
to add.

| Package | For | Notes |
| --- | --- | --- |
| `POM2-v0.9.0-x86_64.AppImage` | Linux (Intel/AMD) | Runs on Mint 19+, Debian 12, Ubuntu 20.04+ (glibc **2.27** floor) |
| `POM2-v0.9.0-aarch64.AppImage` | Linux on ARM64 (desktop/server) | Desktop OpenGL, glibc **2.39** — Ubuntu 24.04+, Fedora 40+, Debian trixie |
| `POM2-v0.9.0-raspberry-aarch64.AppImage` | Raspberry Pi 3 → 5 | OpenGL **ES 3.0**, Raspberry Pi OS bookworm (glibc **2.36**). **Take this one on a Pi** |
| `POM2-v0.9.0-pi400-aarch64.AppImage` | Raspberry Pi 4 / Pi 400 only | Same, plus `-mcpu=cortex-a72` + PGO/LTO (**≈ −39 %** CPU). Will **not** start on an older core |
| `POM2-macOS-v0.9.0.dmg` | macOS 10.15+ | **Universal 2** — Apple Silicon *and* Intel in one file |
| `POM2-Windows-v0.9.0.zip` | Windows 10/11 x64 | One `POM2.exe`, no DLL beside it |
| `POM2-v0.9.0-web-wasm.zip` | any static web host | The browser build — unzip, serve the folder, open `index.html` |
| `SHA256SUMS.txt` | everyone | `sha256sum -c SHA256SUMS.txt` to verify what you downloaded |

Every package carries the emulator's ROM set, its fonts and its artwork, so it
boots with nothing else installed. None of them carries a disk library — bring
your own images (§ Disk images).

**🐧 Linux / 🍓 Raspberry Pi**

```bash
chmod +x POM2-v0.9.0-x86_64.AppImage
./POM2-v0.9.0-x86_64.AppImage
```

If your distro no longer ships `libfuse2`, either install it or run the image
without it: `./POM2-v0.9.0-x86_64.AppImage --appimage-extract-and-run`.
On a **Pi 4 or Pi 400**, take `-pi400-aarch64` rather than `-raspberry-`: it is
the same build compiled for that exact core with two profile-guided passes and
LTO, worth roughly **40 %** on the emulation core. It will not start on a
Pi 3's older cortex-a53 — take `-raspberry-` there. Other cores (a PGO build
tuned for the Pi 5's cortex-a76, the Pi 3's cortex-a53) and the Pi OS Lite
tarball are built on demand — recipe under [Releases](#-releases).

**🍏 macOS** — open the `.dmg`, drag **POM2** into *Applications*. The app is
signed **ad-hoc** (no paid Developer ID), so the first launch is refused with
*"POM2 can't be opened because Apple cannot check it for malicious software"*.
That is Gatekeeper reacting to the absent signature, not to the app. Either:

- **right-click** the app → **Open** → **Open** in the dialog (once), or
- clear the quarantine flag:
  `xattr -dr com.apple.quarantine /Applications/POM2.app`

**🪟 Windows** — unzip anywhere and run `POM2.exe`. The binary is unsigned, so
SmartScreen shows *"Windows protected your PC"*: click **More info** →
**Run anyway**. Keep `POM2.exe` together with the folder it came in.

**🌐 Nothing to download** — [play it in the
browser](https://habib256.github.io/pom2/wasm/) instead; the whole emulator
compiles to WebAssembly.

Building from source instead? That is the [Quick Start](#-quick-start) below.
Cutting a release is [further down](#-releases).

---

## 🚀 Quick Start

### 🐧 Linux / 🍏 macOS

```bash
git clone https://github.com/habib256/pom2.git
cd pom2
./setup_imgui.sh                    # fetch Dear ImGui + install deps (one-time)
cd build && cmake .. && make -j
cd .. && ./run_emulator.sh          # cwd = repo root so roms/ probes resolve
```

`setup_imgui.sh` covers macOS, Debian/Ubuntu, Fedora and Arch.

### 🪟 Windows

Prereqs: [Visual Studio](https://visualstudio.microsoft.com/) (C++ workload), [CMake](https://cmake.org/download/), [Git](https://git-scm.com/download/win), [vcpkg](https://vcpkg.io/).

```batch
git clone https://github.com/habib256/pom2.git
cd pom2
git clone --depth 1 https://github.com/ocornut/imgui.git
vcpkg install --triplet x64-windows-static
cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_INSTALLATION_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

Dependencies come from the repo's `vcpkg.json` manifest, so `vcpkg install`
takes **no package argument**. The `x64-windows-static` triplet matches POM2's
static CRT and yields the self-contained no-DLL `POM2.exe`; the default
`x64-windows` triplet would put DLLs back beside it.

(`setup_imgui.sh` does the Dear ImGui clone and `build/` creation on
Linux/macOS; on Windows do them manually as shown — the CMake configure
hard-fails if `imgui/` is missing.)

### 🌐 WebAssembly

**Play directly:** [POM2 in your browser](https://habib256.github.io/pom2/wasm/)

<details><summary>Build it yourself</summary>

```bash
./build_wasm.sh                     # build
./build_wasm.sh --serve             # build + local server
./build_wasm.sh --with-data         # also bundle the disks_3.5/ library
```

The browser build preloads `roms/`, `fonts/`, the About and //e keyboard photos, and `floppyemu/` — including the shipped firmware dumps. Telnet and the AI-control HTTP server are unreachable under WASM (the browser sandbox has no listening sockets), so their UI entries are hidden.
</details>

### 💿 ROMs and media

Release packages (and the repo) ship the full `roms/` tree; `floppyemu/` is
created on first run. The media folders are a convention POM2 reads when
present — create the ones you use, no package ships them:

```text
roms/         Apple II firmware dumps (bundled in releases)
disks_5.4/    5.25" disk images (yours)
disks_3.5/    3.5" disk images (yours)
hdv/          ProDOS hard-disk images (yours)
floppyemu/    Floppy Emu / BMOW media (auto-created)
```

```bash
POM2 path/to/game.woz               # mount + cold-boot
POM2 --kiosk path/to/game.dsk       # exclusive full-screen, chrome-free
```

---

## ⌨️ Keyboard Shortcuts

| Host | Apple II | Host | Apple II |
|------|----------|------|----------|
| Enter | Return | Left Alt | Open-Apple (`$C061`) |
| Backspace | Left arrow | Right Alt | Solid-Apple (`$C062`) |
| Arrows | Apple II arrows | `Ctrl-A..Z` | `$01..$1A` |
| Esc | ESC | F9 | Screenshot → `screenshot_NNN.ppm` |
| `Ctrl+Alt+F` / F10 | **Full screen ⇄ windowed** (kiosk toggle) | F11 | Soft reset / Ctrl-Reset |
| F12 | Hard reset / power-cycle | `Ctrl+Alt+G` | **Capture / release the mouse** |
| F6 | **Hold to rewind** (time-travel) | `Ctrl+Shift+P` | **Command palette** |
| `Ctrl+V` | Paste clipboard into the Apple II | Tab | `$09` |

`Ctrl-V` is the one exception in the `Ctrl-A..Z` range — the host intercepts it for clipboard paste. To send the Apple II's own Ctrl-V, open *Devices → Apple //e Keyboard*, latch **CONTROL**, then click **V** (the Edit menu only carries the host clipboard actions). F9 / F10 / F11 / F12, `Ctrl+Alt+F`, `Ctrl+Alt+G`, `Ctrl+Shift+P` and both Alt keys are routed unconditionally — even when ImGui holds keyboard focus. GLFW gamepads are hot-plugged and auto-bound.

**Mouse capture.** Put the card in **slot 4** — on a //e the internal 80-column firmware owns `$C300`, so a mouse card in slot 3 is invisible to software (A2DeskTop, MousePaint and MultiScribe all decide there is no mouse and run keyboard-only); Slot Configuration flags it. With a Mouse Card plugged (`mouse` or `mouseaw`), **`Ctrl+Alt+G` or a middle click (wheel)** hands the host pointer to the guest and takes it back again: the OS cursor disappears and motion becomes unbounded, so the emulated cursor can always reach the edges of its own clamp window instead of stalling when your real pointer runs out of screen. Alt-Tabbing away also releases it. A left click never captures — it always goes to the guest, so clicks mean what they look like. The status bar shows a `GRAB` badge while captured, and spells out the way back for half a minute after; nothing is drawn over the Apple II screen. `View → Capture mouse` toggles it from the menu.

Clicks that land on POM2's own interface stay POM2's, even where it sits on top of the screen: an open dropdown, a popup or a panel docked over the Apple II display owns its clicks, so picking a menu item neither reaches the guest nor captures the pointer behind the menu.

---

## 🖥️ Machine Profiles

Nine one-click machines spanning the line — six NTSC plus three **PAL (50 Hz)** variants. Switch from `Machine → Profile` or `--preset`. Each switch is a full cold reset that re-plugs built-in cards and re-mounts inserted disks where possible.

| Profile | CPU | Mode | Main ROM probes | Built-in slots (locked) |
|---|---|---|---|---|
| **Apple ][ Original** (1977) | NMOS 6502 | — | `apple2o.rom`, `apple2.rom` | — |
| **Apple ][+** (1979) | NMOS 6502 | — | `apple2p.rom`, `apple2.rom` | — |
| **Apple //e Unenhanced** (1983) | NMOS 6502 | IIe | `apple2e_unenh.rom`, `342-0135-b.64.rom`, `apple2e.rom` | AUX = Ext. 80-col (built-in) |
| **Apple //e Enhanced** (1985) | 65C02 | IIe | `apple2e.rom` | AUX = Ext. 80-col (built-in) |
| **Apple //c** (1984) | 65C02 | IIe | `apple2c-32Kv0.rom`, `apple2c-16K.rom`, `3420033a.256` | sl1/2 SSC · sl4 Mouse (AppleWin HLE) · sl5 SmartPort · sl6 Disk II |
| **Apple //c Plus** (1988) | 65C02 | IIe | `apple2cp.rom`, `apple2c-plus.rom`, `apple2c-32Kv0.rom` | sl1/2 SSC · sl4 Mouse (AppleWin HLE) · sl5 SmartPort 3.5" · sl6 Disk II |
| **Apple //e Unenhanced PAL** (50 Hz) | NMOS 6502 | IIe | `apple2e_unenh.rom`, `342-0135-b.64.rom`, `apple2e.rom` | AUX = Ext. 80-col (built-in) · **PAL timing** |
| **Apple //e Enhanced PAL** (50 Hz) ← *default* | 65C02 | IIe | `apple2e.rom` | AUX = Ext. 80-col (built-in) · **PAL timing** |
| **Apple //c PAL** (Le Chat Mauve) | 65C02 | IIe | `apple2c-32Kv0.rom`, `apple2c-16K.rom`, `3420033a.256` | same as //c **+ sl7 built-in Le Chat Mauve RGB** · **PAL timing** |

Two of the probe names above — `342-0135-b.64.rom` and `apple2c-plus.rom` — are fallbacks for **user-supplied** dumps under those MAME/community filenames; POM2 ships neither, so on a stock tree they never resolve and the next name in the row wins.

Aliases for `--preset`: `apple2`/`ii`, `apple2plus`/`ii+`, `iie-u`, `apple2e`/`iie`, `apple2c`/`//c`, `apple2cplus`/`//c+`, `iie-u-pal`/`frenchtouch`, `iie-pal`, `iic-pal`/`chatmauve`.

**First launch** boots **Apple //e Enhanced PAL** with **Composite (OpenEmulator)** video and this slot map — a European //e with the peripherals most software expects:

| sl1 | sl2 | sl3 | sl4 | sl5 | sl6 | sl7 |
|---|---|---|---|---|---|---|
| Grappler+ | Mockingboard A/C | *empty* | Mouse (AppleWin HLE) | SmartPort 3.5" | Disk II | Le Chat Mauve |

Slot 3 is empty because the //e's 80-column card isn't a slot card: the firmware is internal ROM at `$C300` and the Extended 80-Column Text Card sits on the AUX connector — both come with the profile. Everything here is a *default*: change any slot in Slot Configuration (or the profile in `Machine → Profile`) and your choice is what loads next time. POM2 falls back to **Apple ][+** if no //e ROM is found.

> The three PAL profiles carry 312-line / ~50 Hz timing — the cadence French Touch / DIX demos and the Le Chat Mauve RGB Péritel adapter were built for. The **Unenhanced PAL //e** (`--preset frenchtouch`) is the machine the 6502-only intros (OLDSKOOL FORT ET VERT…) were written on: their per-scanline cycle counts hold only on an NMOS 6502 — a 65C02 runs the `LSR abs,X` line dispatcher one cycle short and the raster bars drift one character cell per line.

> **ROM identity check** — when the generic `apple2.rom` fallback resolves (no profile-specific dump present), the loader warns that the ROM may not match the selected machine.

---

## ✨ Hardware

**Core** — MAME-faithful **6502 / 65C02 / Rockwell / WDC** CPU; full IIe paging, Language Card + aux LC, and **RamWorks III up to 8 MB**; running at `POM2_CPU_CLOCK_HZ = 1 022 727` (14.31818 MHz / 14), 17045 cycles/frame (//c+ defaults to 4× for its Zip-style accelerator).

| Subsystem | Highlights |
|---|---|
| 📺 **Video** | Text · lo-res · hi-res · double hi-res · 80-column. **Beam-raced** mid-scanline soft switches. Composite NTSC (OpenEmulator-style shader) · AppleWin NTSC (CPU IIR-LUT) · mono phosphor with adjustable curve + persistence · Video-7 RGB · Le Chat Mauve RGB. |
| 🧊 **3D voxel view** | MicroM8-style — framebuffer extruded into orbiting voxels with a real camera (`Voxel3DRenderer` + `Mat4`). |
| ⏪ **Rewind** | MicroM8-style snapshot ring buffer; scrub back and resume. Shares its serializer with the AI-control `/snapshot` endpoints. |
| 🔊 **Audio** | Speaker · cassette · Mockingboard A/C · Mockingboard C **Sound II** (SSI263 speech) · Applied Engineering **Phasor** (2×VIA / 4×AY) · Cricket / Echo SSI263 · Echo+ TMS5220 scaffold · cycle-stamped Disk II + Sony 3.5" mechanical sounds. |
| 💾 **Storage** | `.dsk` `.do` `.d13` `.po` `.nib` `.2mg` `.woz` `.hdv` · DOS 3.x · ProDOS · SmartPort · CFFA 2.0. WOZ uses the real Disk II P6 LSS sequencer; detection is content-driven (MacBinary, DOS/ProDOS skew, WOZ/2IMG write-protect handled). |
| 🌐 **Ethernet** | **Uthernet II** (WIZnet W5100 hardware TCP/IP — runs on host sockets, so period IRC / telnet / FTP clients work with no extra dependency and no root; Linux, macOS and Windows) · **Uthernet I** (CS8900A NIC, raw frames, bridged to the host by optional libslirp user-mode NAT — Linux and macOS only, see below). |
| 🔌 **Peripherals** | Super Serial (+ telnet bridge) · parallel printer with host spool · Orange Micro Grappler+ · **Apple ImageWriter II** printer with a rendered paper tray (colour ribbon, bit-image graphics, PNG + multi-page PDF export) · ProDOS Clock / ThunderClock+ · Mouse Card (MAME + AppleWin HLE) · joystick / paddles · Floppy Emu (BMOW) · on-board //c devices. |
| 🛠️ **Tools** | Disk Library · Slot Configuration · screenshots · memory viewer · snapshots · kiosk mode · CLI · AI-control HTTP server. |

---

## 🃏 Expansion Cards

Assign cards, mount media, eject or boot from `Machine → Slot Configuration`. A typical II / II+ / //e setup: **sl2** Super Serial · **sl4** Mockingboard/Phasor · **sl5** HDV or SmartPort · **sl6** Disk II · **sl7** Le Chat Mauve RGB. On //c and //c+ the built-in slots are locked.

| Key | Card | Key | Card |
|---|---|---|---|
| `diskii` | Disk II | `clock` | ProDOS Clock / ThunderClock+ |
| `hdv` | ProDOS HDV | `chatmauve` | Le Chat Mauve RGB |
| `cffa` | CFFA 2.0 IDE | `mouse` / `mouseaw` | Mouse Card (MAME / AppleWin HLE) |
| `smartport35` | SmartPort 3.5" | `mockingboard` | Mockingboard A/C |
| `liron` | Liron 3.5" — real EPROM + IWM over the SmartPort bus | | |
| `ssc` | Super Serial Card | `mockingboard_c` | Mockingboard C Sound II + SSI263 |
| `printer` | Parallel printer (host spool) | `phasor` | Applied Engineering Phasor |
| `grappler` | Orange Micro Grappler+ | `echoplus` | Cricket / Echo SSI263 |
| `uthernet` | Uthernet I (CS8900A NIC) | `echoplus_tms` | Echo+ TMS5220 + 2×AY scaffold |
| `uthernet2` | Uthernet II (W5100 TCP/IP) | `softcard` | Microsoft SoftCard Z80 (CP/M) |
| `fujinet` | FujiNet relay (SP over SLIP) | `workstation` | Apple II Workstation Card (LocalTalk) |
| `4play` | 4play — 4 digital joysticks (Lukazi) | `transwarp` | TransWarp accelerator (Applied Engineering) |

**TransWarp (Applied Engineering, 1986).** A 3.58 MHz 65C02 on a card, which
is simply the machine's processor moved onto a faster clock — same program,
same memory. Plug it in any slot and the Apple runs at **3.5×** (or 1.75× on
the half-speed DIP switch). There is no register to read and nothing to
configure to make it work: it watches the bus, and software that needs real
1 MHz timing asks for it by writing `$C074`. The shipped DIP defaults leave
**slot 6 at stock speed** — that is the Disk II, the one slot AE did not
trust at full speed. Optional ROM: `roms/ae_transwarp_1.4.bin` (AE's
speed-corrected Monitor, overlaid on `$F000-$FFFF`); the card accelerates
without it.

**Apple II Workstation Card.** The board that put a IIe on LocalTalk, and the
only card here that is a **computer of its own**: a 65C02, 28 KB of RAM, a
Zilog 8530 SCC and 64 KB of banked ROM, all running inside the card while your
Apple II gets on with its own program. POM2 runs Apple's real 341-0358-A
firmware on it — it completes the card's power-on self-test, including a
255-byte loopback check of the SCC, and configures the chip for LocalTalk at
230.4 kbit/s. Needs `roms/341-0358-A.bin` (64 KiB); without it the slot stays
empty rather than presenting a card that cannot work.

It gets further than "configured": with SDLC framing in place — modelled from
the Zilog manual, since MAME does not emulate it — the card's firmware
**acquires a LocalTalk node address and starts transmitting**. `0B 0B 81` is
LLAP's node-address enquiry; once it holds node `$0B` it broadcasts AppleTalk
datagrams. And if you boot **CardCat** on the emulated //e, it names the card
in slot 4, which is the check that matters: real 1980s-descended software
identifying it by its firmware signature.

And the software agrees: **AppleShare's own IIe Workstation disk boots, passes
the card's power-up diagnostics and reaches its menu**. Its driver reaches the
card at `$Cn14` and the two CPUs complete their handshake — which they do by
rewriting each other's code in a shared page, an arrangement worth seeing
once. Note the card runs a second 6502 at the Apple II's own rate, so it
roughly doubles the emulation work while plugged.

**FujiNet.** POM2 does not emulate a [FujiNet](https://fujinet.online/) — it **relays** to a real one. Put the `fujinet` card in **slot 7** (the //e scans it before the Disk II in slot 6, so the machine boots straight into FujiNet's CONFIG) and point it at either:

- a **FujiNet desktop build** running alongside POM2 — POM2 listens on `127.0.0.1:1985`, the same port the FujiNet project itself uses, so an existing configuration needs no changes; or
- a **real FujiNet board** plugged into your USB port, over its CDC-ACM serial device.

Because every Apple II FujiNet function is a SmartPort unit, that one connection carries all of them: disk images mounted from TNFS or the internet, the `N:` network device (HTTP/HTTPS, TCP, UDP, SSH, Telnet, JSON), the clock, the printer, the modem and CP/M. Guest-side software — `fujinet-apps`, `fujinet-lib`, CONFIG, the BBS and high-score clients — runs unmodified. With no FujiNet answering, the card steps aside and lets the autostart scan carry on to slot 6, so it is safe to leave plugged in. CLI: `--fujinet[=PORT]`, `--fujinet-serial[=DEVICE]`, `--fujinet-slot N`. II+ / //e only (a //c's forced INTCXROM hides slot ROM), and note that **Rewind cannot rewind the FujiNet** — data it wrote stays written.

On Linux, a serial FujiNet needs your user in the `dialout` group (`sudo usermod -aG dialout $USER`, then log out and back in).

**Ethernet, per platform.** The **Uthernet II** needs nothing installed: its W5100 is a hardware TCP/IP stack that POM2 runs on host sockets, so TCP and UDP work out of the box on **Linux, macOS and Windows**. The **Uthernet I** is a plain NIC — its guest software (IP65, Contiki, ADTPro-ethernet) carries its own stack and hands the card raw frames — so it needs the optional **libslirp** user-mode NAT backend, available on **Linux and macOS only**. The Uthernet II's own raw modes (MACRAW / IPRAW) go through the same backend and have the same limitation.

---

## 🖨️ ImageWriter II — see what you printed

The **ImageWriter II** is a printer, not a card, so it has no slot key: it
hangs off whichever printer interface card you plugged (`printer` or
`grappler`) and picks up everything that card spools. Open it with
*Devices → ImageWriter II (printout)* and print from BASIC:

```
PR#1
PRINT "HELLO IMAGEWRITER II"
PR#0
```

The page appears as it would come out of the printer — dot by dot. It
interprets the real ImageWriter control language, so pitch and style
changes (`ESC E/N/Q/p/P`, bold, underline, double-width, italics), line
spacing, margins and tabs, the **four-band colour ribbon** (`ESC K`, with
overprint mixing magenta + yellow into red the way real ribbons do), and
**bit-image graphics** (`ESC G`/`ESC C` — what screen dumps and Print Shop
actually send) all land on paper.

Front panel: **Form feed** ejects the sheet onto the stack (a blank sheet
is not ejected, like the real button), **Reset printer** returns it to
factory settings, and **Save sheet as PNG** / **Save all sheets** /
**Save PDF** (the whole job as one multi-page PDF) write to
the per-user POM2 data directory under `printouts/`. *Printer settings* holds the paper size, the page raster
resolution, and **Auto line-feed after CR** — leave that on for Apple II
software (which sends CR and never LF); turn it off if a driver sends both
and everything double-spaces.

---

## 📺 Video — beam, NTSC, phosphor & 3D

POM2's renderer is **event-driven, not frame-snapshot**. Soft-switch writes carry a CPU-cycle stamp; the display reconstructs the frame from that event log, so mid-scanline mode changes (TEXT-over-HGR splits, beam-raced page splits, AN3 DHGR pulses through the Le Chat Mauve FIFO) land on the right scanline. The log is published once per *video* frame, so a 60 Hz window showing 50 Hz PAL content never drops a frame's worth of switches. Unsynced double-buffer page flips (DROL-style) are told apart from beam-raced page splits and shown tear-free. The composite signal beam-races on the same log — pinned by `beam_race_composite`.

- **Composite NTSC** — OpenEmulator-style fragment shader (`NtscPostProcessor` / `OpenGLShader`): barrel → hue → BCS → phosphor curve → glow.
- **AppleWin NTSC** — the alternative CPU-side IIR-LUT colour path (`AppleWinNtsc`).
- **Mono phosphor** — adjustable **phosphor curve** (`ntsc_phosphor_gamma`, luminance half of the CRT model) and **persistence** (temporal half), tunable in *View → CRT Settings*.
- **RGB cards** — Le Chat Mauve (Féline · Adaptateur //c · Eve with its `$C0B0-$C0BF` switches) and the Video-7 AppleColor, one card with a variant setting (`chatmauve_variant`), for IIe-class machines.
- **3D voxel view** — lift the whole framebuffer into an orbiting voxel scene.
- **HGR/DHGR Paint editor** (*Tools → HGR Paint Editor*) — MacPaint-style painting straight into live video RAM (HGR, GR lo-res, and DHGR on IIe-class machines), rendered through the real NTSC pipeline. Imports PNG/JPG with ii-pix-style CAM16-UCS perceptual dithering; loads/saves raw pages (8 KB HGR, 1 KB GR, 16 KB A2FC DHGR) and PNG exports.
- **HGR Sprite Editor** (*Tools → HGR Sprite Editor*) — draw hi-res sprites over live video RAM and export them as ca65 `.byte` tables.

---

## 🔊 Audio — speaker to Phasor

Every audio event is cycle-stamped, so tempo follows emulation speed, not wall-clock — disk-turbo's ~60× collapse of wall-clock gaps stays inaudible. The bus carries the **Speaker** and **Cassette**, plus a full card stack: **Mockingboard A/C** (6522 VIA + AY-3-8910), **Mockingboard C Sound II** with the **SSI263** speech chip, the **Phasor** (2 VIAs driving 4 AYs), and the **Cricket / Echo** SSI263 line. Mechanical **floppy sounds** (`FloppySoundDevice`) consume the cycle stamp emitted by `DiskIICard::seekPhaseW`, so head-steps and drive whir line up with the LSS nibble stream.

The output is **stereo**, wired the way the hardware is: a Mockingboard puts AY1 in the left channel and AY2 in the right, a Phasor splits its four chips one VIA-pair per side, and speech sits centred. Music written with a deliberate stereo image — Digidream 1, for instance — plays as its authors intended. Speaker, cassette and floppy sounds are centred by default with a pan knob per channel in the **Audio Mixer**, and a **Mono** switch there folds everything back to a centred image for mono playback gear.

---

## 💾 Storage — disks, SmartPort, CFFA

Supported images: `.dsk` `.do` `.d13` `.po` `.nib` `.2mg` `.woz` `.hdv`. Detection is **content-driven** — MacBinary wrappers, DOS/ProDOS sector skew and WOZ/2IMG write-protect flags are all handled. WOZ playback runs the genuine Disk II **P6 LSS sequencer** (`diskii_p6.rom` optional — the embedded 341-0028-A default is used when absent). ProDOS block devices back the HDV / CFFA 2.0 / SmartPort paths.

Accepted main ROM sizes: 12 KB, 16 KB, 20 KB system packs (with 4 KB filler) and 32 KB system+video ROMs each get their own layout; any other dump between 2 KB and 64 KB is loaded best-effort at the top of the address space, so its reset vectors land at `$FFFA-$FFFF`. Anything outside that range is rejected (`Memory::loadAppleIIRom`).

| File | Role |
|---|---|
| `apple2e.rom` | //e firmware (+ optional charset) |
| `apple2cp.rom` | //c+ banks 0 + 1 |
| `apple2_char.rom` | II/II+ character ROM (also the IIe-class fallback) |
| `apple2e_char.rom` / `apple2e_char_2k.rom` | //e character ROMs — Enhanced 4 KB (mousetext) / Unenhanced 2 KB |
| `apple2e_char_<locale>.rom` | Locale character sets (US, UK, DE, FR, FR-CA, …) offered in the character-set dropdown |
| `342-0274-a.e9` | International //e video ROM 342-0274-A — one 8 KB part holding **two** 4 KB banks (FR-CA low, US high), as fitted to the French //e; either bank is selectable from the same dropdown |
| `Videx Lower Case Chip ROM.bin` | Videx LOWER CASE CHIP — the 1980 drop-in generator that gave a II/II+ lowercase |
| `disk2.rom` / `disk2_13.rom` | Disk II boot PROMs (16- / 13-sector; embedded 341-0027-A default for 16-sector) |
| `diskii_p6.rom` / `diskii_p6_13.rom` | Disk II P6 LSS sequencer PROMs (embedded default when absent) |
| `liron.rom` | Liron / SmartPort controller firmware (real $Cn0D dispatch identity) |
| `cffa20ee02.bin` / `cffa20eec02.bin` | CFFA 2.0 firmware |
| `mouse_341-0270-c.bin` / `mouse_341-0269.bin` | Mouse Card slot ROM / 68705 MCU mask ROM |
| `grappler_plus.bin` | Grappler+ EPROM |
| `thunderclock_u9_v1.3.bin` | ThunderClock+ firmware |
| `roms/floppy_samples/*.wav` | Mechanical drive samples |

---

## 🎛️ Command line

```bash
POM2 <disk-image>                   # mount into the right slot + cold-boot
POM2 tnfs://host/path/image.po      # fetch from a TNFS server into a local cache, then boot
POM2 --kiosk <disk-image>           # exclusive full-screen, chrome-free, settings-read-only
POM2 --preset ii|ii+|iie-u|iie|iic|iic+|iie-u-pal|iie-pal|iic-pal
POM2 --snapshot-save out.pom2snap
POM2 --snapshot-load in.pom2snap
```

More flags: `--speed`, `--cpu-max`, `--ii-plus` (alias `--ii+`), `--ai-control[=PORT]`, `--display <ntsc|chatmauve|mono-white|mono-green|mono-amber>`, `--tape`, `--save-tape` / `--save-tape-format aci|wav`, `--35-disk1`, `--35-disk2` (//c+ Sony 3.5"), `--prodos-folder <dir>`, `--load addr:file`, `--run <addr>`, `--step N`, `--paste`, `--play`, `--rec`, `--rewind`, `--rgb-card-invert-bit7[=on|off]`, `--fujinet[=PORT]` / `--fujinet-serial[=DEV]` / `--fujinet-slot N`. `POM2 --help` is the full list. Full architecture → [`CLAUDE.md`](CLAUDE.md).

### 🕹️ Kiosk mode

**Full screen *is* kiosk mode.** Press **Ctrl+Alt+F** — or **F10**, View →
Full screen, or the command palette — to switch between the windowed GUI and
the chrome-free full-screen view, in either direction, at any time. (Both
bindings do the same thing: F10 is claimed by the window manager on some
desktops and never reaches POM2 there.) The emulated machine is never touched
by the switch — kiosk changes only the window, the render path and
settings-writing — so a game keeps playing across it and nothing is lost.
From inside kiosk, the in-game menu also offers **EXIT KIOSK (WINDOWED)** for
anyone who doesn't know the shortcut.

`POM2 --kiosk <disk-image>` turns POM2 into a distraction-free appliance — think arcade cabinet, museum exhibit, or a dedicated retro corner. It:

- Opens **exclusive full-screen** on the primary monitor at its native video mode (falls back to a plain window if there's no monitor to grab).
- Draws **only the Apple II screen**, centred and letterboxed on black — no menu bar, no toolbar, no panels or dialogs.
- **Boots the disk image** you pass (5.25" / 3.5" / HDV, slot auto-picked from the file type) under your saved profile — or override with `--preset`, `--display`, `--cpu-max`, …
- Is **read-only**: it never writes your `state.cfg` settings or `imgui.ini` window layout, so a kiosk session can't disturb your desktop setup. (An HDV with no HDV/SmartPort card in your saved config gets one auto-plugged **for that session only**.)

Everything the machine needs keeps running: keyboard, joystick/paddles, auto-turbo during disk I/O, **F11 / F12** (soft / hard reset), **F9** (screenshot), **Left / Right Alt** (Open / Solid Apple), and **F6** (hold to rewind — inert while the in-game menu is up).

**In-game menu.** Press **Start** on a gamepad — or **F1** — for the Start menu: the games list on the left (every 5.25" / 3.5" / HDV image in the booted disk's folder plus your extra ROM folders; a 5.25" hot-swaps in place for flip-disk games, a 3.5"/HDV mounts and boots straight away) and an action column on the right (**Restart · Keyboard · ROM folders · Exit kiosk · Quit**). **Select** — or **K** — toggles a live on-screen keyboard band without pausing the game. D-pad / arrows move, **A / Enter** validates, **B / Esc** backs out. The machine is paused while the menu is up (except under the keyboard band), and menu presses never leak into the running game. **Alt-F4** still quits directly (POM2 handles the combo itself, so it works even in exclusive full-screen).

```bash
POM2 --kiosk "Lode Runner.dsk"                 # boot a game, full-screen
POM2 --kiosk --preset iic --cpu-max game.hdv   # //c profile, run flat-out
```

---

## 📦 Releases

*Looking for a build to run? That is [Download](#-download) at the top. This
section is how a release gets made.*

**Cutting a release** — write the notes for the version in
`docs/releases/v<ver>.md`, then push a version tag: the `Release packages`
workflow builds every platform natively, attaches the artifacts (plus a
`SHA256SUMS.txt`) to the GitHub Release, and uses that file as the release
body (falling back to auto-generated notes when it is absent):

```bash
git tag v0.9.0 && git push origin v0.9.0  # `0.9.0` without the v works too
```

Use **Run workflow** on the Actions tab for a dry run: same builds, artifacts
uploaded, no Release created.

| Package | Runner | Notes |
| --- | --- | --- |
| `POM2-v<ver>-x86_64.AppImage` | `ubuntu-latest` + pinned **bionic** container | glibc floor **2.27** — runs on Mint 19+, Debian 12, Ubuntu 20.04+ |
| `POM2-v<ver>-aarch64.AppImage` | `ubuntu-24.04-arm`, native | ARM desktops/servers — desktop GL, glibc floor **2.39** |
| `POM2-v<ver>-raspberry-aarch64.AppImage` | `ubuntu-24.04-arm` + **bookworm** container | Raspberry Pi 3→5 — GLES 3.0, glibc floor **2.36**, no desktop libGL |
| `POM2-v<ver>-pi400-aarch64.AppImage` | same + `-mcpu=cortex-a72`, PGO + LTO | Pi 4 / Pi 400 only — will not start on an older core |
| `POM2-macOS-v<ver>.dmg` | `macos-15` | **Universal 2** (arm64 + x86_64), static GLFW, ad-hoc signed |
| `POM2-Windows-v<ver>.zip` | `windows-latest` + vcpkg static triplet | one self-contained `POM2.exe`, **no DLL** beside it |
| `POM2-v<ver>-web-wasm.zip` | `ubuntu-latest` + emsdk | the browser bundle, for any static host |

The Linux package is built inside a frozen container on purpose: an AppImage
never bundles glibc, so its floor is whatever the *build* machine had. Building
on `ubuntu-latest` stamps `GLIBC_2.38`, which will not start on Debian 12 or
Ubuntu 22.04. POM2 **reuses POM1's** `pom1-bionic-builder` image (pinned by
digest) — the requirements are identical, so there is one image to maintain
rather than two. Rebuild it from POM1, then re-pin the digest in both repos.

The three aarch64 packages are not redundancy: each targets a floor the others
cannot serve, and the file name says which to take. The variant tag in the
name is enforced per job — the publish step flattens every artifact into one
directory, so two same-named packages would overwrite each other in silence.

**What ships inside a package** is declared once, in
`packaging/bundle.manifest`: the full ROM set plus a generated
`roms/README.txt` inventory note, the two UI fonts, the About photo, and the
//e keyboard photo the clickable-keyboard window's hotspots are measured
against. CMake reads it for the `install()` rules *and* for the WASM
`--preload-file` list; `packaging/stage_data.sh` reads it for the macOS `.app`
and the Windows `.zip`. Every release job then runs
`stage_data.sh --verify` on the staged tree — everything the manifest promises
is there, and nothing from its `deny` list (the disk libraries, your snapshots
and printouts) leaked in. Both failures are otherwise silent.

Past `--help`, each job also runs a **boot smoke** against the packaged
binary: `pom2_headless --frames 300 --screenshot boot.ppm` boots the bundled ROM,
executes 300 frames and fails if the captured frame is a single flat colour.
That is what proves a package resolved its *own* ROMs and ran 6502 code —
`--help` would pass just as happily with an empty `roms/`.

**Local builds** (no CI needed):

```bash
./build_dist.sh                     # relocatable tarball + .deb (+ AppImage if linuxdeploy present)
./build_dist.sh --tests             # build + run the pinned smoke tests
packaging/linux/build_appimage.sh   # just the AppImage, from POM2's own install rules
./package_macos_release.sh          # .app + .dmg (on a Mac)
package_windows_release.bat         # staged folder + .zip (on Windows, after a build)
```

Every artifact bundles the full `roms/` tree. To override a dump in a read-only
package (AppImage/.dmg/.deb), drop a replacement into the per-user data
directory — `~/.local/share/POM2/roms/` on Linux (`$XDG_DATA_HOME` honoured),
`~/Library/Application Support/POM2/roms/` on macOS,
`%LOCALAPPDATA%\POM2\roms\` on Windows. The AppImage creates the Linux folder
with a README on first run; `ResourcePaths` also probes beside the executable.

**Raspberry Pi** builds on the OpenGL **ES 3.0** tier — Mesa's V3D caps
*desktop* GL at 3.1 on Pi 4/5, so the default GL 3.2 core request cannot
succeed there:

```bash
cmake -S . -B build -DPOM2_GLES=ON     # needs libgles2-mesa-dev libegl1-mesa-dev
```

Same sources, no renderer fork: only the GL headers, the shader `#version`
prologue and the context request differ (`src/Pom2Build.h`). WASM turns the
same tier on by itself — WebGL 2 *is* GLES 3.0. (On a *desktop*-GL build the
CRT/NTSC shaders negotiate their dialect at run time — 150 → 140 → 130 — so a
driver capped below GLSL 1.50 no longer disables the effect stack.)

For a Pi you actually intend to run games on, take the **core-specific PGO
package** rather than the generic aarch64 AppImage — worth roughly 40 % on the
emulation core. CI builds it for you on a native ARM64 runner, so the Pi never
compiles anything:

```bash
gh workflow run pi400.yml -f mcpu=cortex-a72     # or cortex-a76 for a Pi 5
gh run download <run-id> -n POM2-pi400-aarch64   # AppImage + /opt tarball
sudo packaging/raspberry/pi_tuning.sh            # governor, IRQs, swap
```

(`packaging/raspberry/build_native_pi.sh --pgo --install` does the same build
on the Pi itself when you are iterating on the source.)

→ [`packaging/raspberry/README.md`](packaging/raspberry/README.md) for the
how-to, [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) for the measurements and
the profiling recipe.

---

## ⚠️ Known Limitations

- Mouse absolute position can drift under A2Desktop / MGTK.
- Some anti-//e copy-protected titles refuse to boot on //e/c/c+ hardware.
- **//c+ 3.5"/SmartPort: two paths, and only the drive-side firmware is out of scope.** POM2 boots 3.5" and HDV images on the //c+ through a host-served SmartPort block device at the built-in slot 5, *and* through the silicon: the IWM state machine and the Sony GCR drives are ported (`IWMDevice`, `Sony35Drive` — `--35-disk1/2` mounts 800K images in the //c+ Sony bays), and since 2026-09-01 the //c+ firmware's own on-board boot path drives them end to end — the ROM works the MIG, the MIG selects the drive, the IWM walks the bit cells, ProDOS 8 boots off the internal bay (pinned by the `iicplus_boot35` test). What stays out of scope is the UniDisk 3.5's drive-side 65C02 firmware: POM2 answers its *protocol* instead. (The Liron-class controller firmware itself is no longer the obstacle it once was — the BMOW/Yellowstone dump is public, POM2 ships it, and the slot card presents its real identity on //e-class machines; MAME's *WANTED* entry is simply stale.)

---

## 🛠️ Developer Notes

- [`CLAUDE.md`](CLAUDE.md) — always-loaded orientation index (build, memory map, profiles, reset architecture, CLI).
- [`DEV.md`](DEV.md) — implementation deep-dives, MAME-parity ports, internals, gotchas, pinned tests.
- [`TODO.md`](TODO.md) — active backlog + MAME ↔ POM2 parity dashboard.
- [`CHANGELOG.md`](CHANGELOG.md) — resolved items and the **why** behind non-obvious fixes.

**Conventions**: one concern per `.cpp/.h` pair · MAME = source of truth (cite the file + line range, pin a smoke test under `tests/`) · `emuCycles` everywhere — CPU → audio/UI events carry a cycle stamp, never wall-clock · reach the emulated state through `controller->lockState()`, which hands back `Memory` and the CPU *through* the state lock, so the access cannot be written without it (bare `stateMutex()` is for mutual exclusion that touches neither).

---

## 👏 Credits

- **Arnaud Verhille** — POM2 emulator & Dear ImGui port.
- **The MAME team** — the hardware reference POM2 ports cite line-by-line.
- **AppleWin**, **OpenEmulator** and **MicroM8** — NTSC colour models, LSS sequencing, and the 3D-voxel / rewind inspiration.
- **Steve Wozniak & Steve Jobs** — for creating the Apple II 🍎

## 🔗 Resources

- [POM2 in your browser](https://habib256.github.io/pom2/wasm/) — WebAssembly build.
- **[DIX](https://github.com/Fr3nchT0uch/DIX/)** — French Touch demo anthology (29+ min, GPLv3 sources). The **gold-standard integration test** for cycle-accurate Apple II emulation: vapor lock, mid-scanline video, Mockingboard, 128 KB aux, SmartPort/Unidisk. If DIX runs clean, you're there.
- Architecture → [CLAUDE.md](CLAUDE.md) · Internals → [DEV.md](DEV.md) · Backlog → [TODO.md](TODO.md) · Edge-case test corpus → [`docs/test_corpus.md`](docs/test_corpus.md).

---

## 📄 License

GPL-3.0 — see [LICENSE](LICENSE).

<div align="center">

*Made with ❤️ for the Apple II community*

</div>
