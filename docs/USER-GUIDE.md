# POM2 user guide

[← Back to the README](../README.md)

## Install and first launch

Desktop packages include the firmware, fonts and interface artwork. Keep the
executable and its accompanying folders together. On Linux, make an AppImage
executable before launching it; if FUSE is unavailable, use
`--appimage-extract-and-run`. On macOS, drag POM2 from the DMG into Applications.
The app is ad-hoc signed, so macOS may require **System Settings → Privacy &
Security → Open Anyway** after the first attempt. On Windows, extract the ZIP
and launch `POM2.exe`; if SmartScreen appears, choose **More info → Run anyway**.
Release downloads include `SHA256SUMS.txt` for integrity checks.

The first desktop launch uses an enhanced PAL //e. Choose another machine in
**Machine → Profile**, then drop an image onto the window or open **File → Disk
Library**. Selecting a profile resets the emulated machine. **F10** toggles
fullscreen. For an empty disk drive, the Applesoft prompt is ready for BASIC.

The **Debugger > Assemble instruction** section accepts one 6502/65C02
instruction at a time. Stop the CPU, enter a hexadecimal address and a mnemonic
such as `LDA #$00`, then choose **Assemble and advance**. The byte preview uses
the selected CPU mode; out-of-range branches are rejected. Operands accept
`$hex`, `%binary` and decimal; four hex digits force absolute addressing.
Patches target RAM below `$C000` through the current CPU write mapping.

The debugger also supports bank-qualified breakpoints and watches, such as
`aux1:$0800` or `lc2:$D000`, separate profiling/history for each code bank,
and source-line correspondence through `.p2map` files. Unqualified addresses
continue to match every bank. See [Bank-aware debugger](debugger-banks.md)
for the bank names, symbol syntax and source-map format.

Select **Echo+ (TMS5220 + 2xAY)** in a free slot for the real Street
Electronics speech/music card. Its TMS5220C speech is distinct from the
Cricket/SSI263 card; both music chips and speech appear in the mixer.

Native IWM drives can now write GCR WOZ/MOOF captures while retaining flux
timing and unrelated tracks/metadata. Header and file write protection still
apply. SuperDrive writes update existing CRC-valid sectors in 1.44 MB
MFM MOOF captures, preserving the rest of the track and container. GCR
SmartPort block mounts remain protected; use
`.po` conversion for block editing.

The Disk II panel's **Magnetic transitions and head** section shows either
drive's quarter-track head position, transition density over a revolution and
a zoomed transition timeline. The angular cursor follows controller time.
Sector images display reconstructed flux rather than an original capture.

In the browser, **Edit > Paste from file... > Choose text file...** uploads a
text listing through the browser selector. Choose **Paste** to queue it at the
Apple II keyboard. Files up to 1 MiB are accepted; the existing paste queue
limit is 4096 characters and truncation is reported. **File > Save/Open
session...** now transfers session archives through browser downloads/uploads.
See [session support and automatic resume](session-format.md#using-it) for
supported configurations and IndexedDB checkpoint behavior.

Desktop file opening also depends on the package's host integration. The
macOS bundle declares common Apple II disk extensions and receives Finder/Dock
open events. Linux provides a desktop launcher and an Apple II disk MIME type;
desktop integration must be installed for **Open With** to discover it. On
Windows, pass an image to `POM2.exe` or drop it on the window; the ZIP package
does not register file associations. `.moof` is supported by the emulator but
is not declared in the current macOS/Linux association metadata.

## Make it yours

### The glow of the screen

Choose composite colour, crisp RGB, white, green or amber phosphor. In
**Display → CRT Settings**, adjust the curve, persistence and screen effects
until it feels like the monitor you remember. **Display → Monitor II frame**
adds the familiar case around the picture.

POM2 follows video changes as the emulated CPU makes them. That matters for
split-screen games and demos that change modes while the beam crosses the
screen. Monochrome keeps the Apple II's fine light-and-dark dot patterns,
including the dotted appearance behind orange and blue hi-res colours.

For a different perspective, open the **3D voxel view** from Display and orbit
the running picture. The old pixels have a little depth now.

### The chatter of the drive

Open **Devices → Storage → Drives** for a vertical stack of your configured
drives. Slot cards appear from slot 7 down to slot 1,
with each card's drives in numerical order. This matches the //e's slot scan;
the //c normally tries its internal 5.25-inch drive, then SmartPort, without
automatically trying the external 5.25-inch drive. The //c+ keeps its internal and
external 3.5-inch mechanisms at the top. Click an empty drive picture to choose an image, or drop a file onto
it from your desktop or the Disk Library. POM2 inserts it into that drive
without restarting your game. Eject a mounted disk before inserting another.
The Disk Library's **All media** tab searches 5¼-inch, 3½-inch, hard-disk and
Floppy Emu images together. Results stay grouped by medium, so a WOZ offered
for more than one drive keeps the correct mount options in each group. The
other tabs narrow the same library to one medium.
The file browser lets you explore your whole disk: jump to Home, the root or
mounted volumes, enter a folder path directly, and reveal hidden folders.
The doors, activity lights and mounted disk names follow the machine.

The Audio Mixer's **Automatic** sound mode uses **MAME recordings for classic
Disk II drives** and **Virtual II recordings for //c-family 5¼-inch drives**
when you've supplied them. Without those recordings, it uses MAME. Automatic
mode keeps MAME sounds for 3½-inch drives; manual bank selection is available.

<details>
<summary>Use your own Virtual II recordings</summary>

In **Audio Mixer**, choose **Import sounds...**, enter your sound-folder path,
and click **Validate and use**. Restart POM2 to activate the recordings.
**Help → ROM Status → Virtual II recordings** shows which files POM2 finds in
that folder, including accepted `.wav` alternatives.

Required files, relative to that folder:

```text
lecteur/Disk Rotation.wav
lecteur/Move arm.aiff
lecteur/Disk Insertion.aiff
lecteur/Disk Removal.aiff
```

WAV alternatives to the AIFF files are accepted. The dialog lists missing or
unreadable files. Optional sounds include those under `interface/` and
`imprimante/Matrix Printer.wav`.

POM2 reads your recordings in place. They are user-supplied and are excluded
from release packages.

</details>

### One more chance

Hold **F6** to rewind, or open **Rewind (time-travel)** to move back through
recent execution and resume. Snapshots let you save and reload a machine state.

File snapshots restore CPU, RAM and memory/display switches. New captures
check the motherboard and character ROMs, memory configuration and PAL/NTSC
standard before loading; older files load with a compatibility warning.
Cards, mounted media and external connections are not restored by `.pom2snap`
files.

For supported II/II+/IIe and //c-class configurations, `.pom2session` additionally bundles
floppy media, including unsaved writes, WOZ/FLUX and 3.5-inch WOZ/MOOF,
Mockingboard, SNES MAX, Slinky RAM and ThunderClock+ state. On //c-class
machines it also restores the built-in ACIAs, IOU mouse, RGB adapter,
onboard Sony media and SmartPort transfers. Session v6 also embeds both
ProDOS HDV drives and HDV units in SmartPort chains, including dirty blocks.
Sparse volumes up to 32 MiB fit when the full archive stays within 32 MiB;
v7 also supports CFFA/ATA, Zip Chip, TransWarp, additional audio/game cards,
program tapes and detached copies of host-folder volumes. Use desktop
**File > Save session... / Open session...**, `--session-save` / `--session-load`
or the local `/session/save` and
`/session/load` API. Loading automatically rebuilds the recorded profile,
ROMs, CPU, memory and supported cards; unsupported devices are refused. Restored writable disks keep further changes in RAM;
save a new session or use automatic resume to retain them. Their original
files are not overwritten by a restore. See [resumable sessions](session-format.md).

Disk saves and external devices have their own history: rewind cannot undo a
write already saved to disk or an action performed by a connected FujiNet.

### Joysticks, keys and paper

Mapped gamepads use the left stick for `PDL(0/1)` and the right stick for
`PDL(2/3)`. The Joystick panel has deadzone, square-gate and axis-inversion
controls. Unmapped desktop joysticks use their raw X/Y axes and leave the extra
paddles centred.

With a **SNES MAX** card installed, the first two mapped gamepads feed its
two SNES ports (D-pad, four face buttons, Select/Start and shoulder buttons).
They stop driving the Apple game port and keyboard shortcuts. Use **F1/F2**
for kiosk controls: the pads' Start/Select now belong to the guest.

The clickable **Apple Keyboard** selects the //c photo for //c, //c PAL and
//c+ machines, and the //e photo for other profiles. Both skins use the same
2578 × 908 image dimensions, with their own clickable key positions. It lets
you type with the mouse and latch Shift, Control and the Apple keys (Option
on the //c). It is also handy for sending guest Ctrl+V,
which the host keyboard normally uses for clipboard paste.

And yes, you can print. Open **Devices → ImageWriter II (printout)**, connect
a printer interface, and watch the dots appear on the page. The printer handles
text, graphics and colour, with PNG and multi-page PDF export.

## Controls

| Key or action | What it does |
|---|---|
| **Alt+Enter** / Alt+keypad Enter | Switch between fullscreen with mouse capture and the windowed interface with a free pointer |
| **F10** / **Ctrl+Alt+F** | Switch between fullscreen and the windowed interface |
| **Ctrl+Alt+G** / middle mouse button | Toggle mouse capture for an installed Mouse Card |
| **F6**, held | Rewind |
| **F9** | Save a screenshot |
| **F11** | Soft reset / Ctrl-Reset |
| **F12** | Hard reset / power-cycle |
| **Ctrl+Shift+P** | Open the command palette |
| **Ctrl+V** | Paste the host clipboard into the Apple II |
| Left / right Alt | Open-Apple / Solid-Apple, when enabled |
| Enter / Backspace / arrow keys | Return / left arrow / Apple II arrows |
| Ctrl+A…Z | Apple II control characters; Ctrl+V is intercepted for paste |

A Mouse Card normally belongs in **slot 4** on a //e: slot 3's internal
80-column firmware can hide it from guest software. **View → Capture mouse**
also controls capture. Switching away from POM2 releases the pointer; clicks
on POM2 menus and panels stay with the interface.

### Fullscreen evenings

Fullscreen is POM2's **kiosk mode**: the Apple II picture fills the view,
without the desktop panels. Switching views leaves the emulated machine running.

In kiosk mode, **F1 / gamepad Start** opens the game menu; **F2 / Select** opens
the on-screen keyboard. Navigate with arrows or the D-pad, confirm with
Enter / A, and go back with Esc / B. The menu can restart a game, browse disk
folders or return to the windowed interface. **Alt+F4** quits.

```bash
POM2 --kiosk "my-game.dsk"
```

Kiosk sessions leave your saved settings and window layout alone. Disk images
can still receive game saves.

## Choose your Apple II

From the original ][ to the //c Plus, choose a profile in **Machine → Profile**.
NTSC profiles run at about 60 Hz; PAL profiles at about 50 Hz. Pick the machine
and timing your software expects, especially for demos.

| Machine | CPU | CLI preset |
|---|---|---|
| Apple ][ (1977) | 6502 | `ii` |
| Apple ][+ (1979) | 6502 | `ii+` |
| Apple //e Unenhanced (1983) | 6502 | `iie-u` |
| Apple //e Enhanced (1985) | 65C02 | `iie` |
| Apple //c (1984) | 65C02 | `iic` |
| Apple //c Plus (1988) | 65C02 | `iic+` |
| Apple //e Unenhanced PAL | 6502 | `iie-u-pal` / `frenchtouch` |
| Apple //e Enhanced PAL — first-launch default | 65C02 | `iie-pal` |
| Apple //c PAL — Le Chat Mauve | 65C02 | `iic-pal` / `chatmauve` |

The default //e Enhanced PAL has Grappler+ in slot 1, Mockingboard in slot 2,
Mouse Card in slot 4, SmartPort in slot 5, Disk II in slot 6 and Le Chat Mauve
in slot 7. Slot 3 is empty; extended 80-column memory uses the AUX connector.

On IIe profiles, a storage card in slot 3 cannot boot through its slot entry
after reset: the motherboard's 80-column firmware owns that ROM page. Use
another slot. The //c family forces its built-in devices, including SmartPort
in slot 5 and the 5¼-inch controller in slot 6, over saved expansion choices.
The //c+ adds its physical Sony drives; //c PAL also forces its RGB adapter.

**Machine → Slot Configuration** lets you change cards and mount or boot disks.
For 1.44 MB MOOF or ProDOS `.po`/`.2mg` images on an enhanced Apple IIe, select
`superdrive` in a free slot, then choose a 3.5-inch media unit and mount the
image. The card provides the ProDOS block interface and accepts 400K/800K GCR
and 1.44 MB MFM images. The 1.44 MB path emulates the card's block service;
direct programming of the original SWIM chip and execution of its 65C02 ROM
are not implemented. The regular SmartPort/Liron and //c+ Sony drives reject
1.44 MB MFM media.
For CFFA3000 storage, choose `cffa3` in a free slot (usually slot 7) and
assign images to its eight SmartPort bays. To use its floppy role, configure
Disk II in another slot (usually slot 6) and mount the floppy image there.
The CFFA3000's CF/USB file browser and automatic virtual-slot assignment
are not emulated. Floppy Emu remains a separate device with its own panel.
To serve VSDrive volumes, put a Super Serial Card in a free slot (slot 2 is
the usual choice), open **Super Serial**, enter one or two raw ProDOS-order
image paths (`.po` or `.hdv`), and select **Start VSDrive**. POM2 then serves
the ADTPro VSDrive block protocol locally on that card; the Apple II program
still needs a VSDrive driver, such as the one in A2 File Cmd. Writes go to
the selected image files unless **Read only** is selected. Stop VSDrive before
changing their paths. The selected paths and active state are restored with
the session.
Some programs expect specific slots: DIX, for example, needs its Mockingboard
moved to slot 4. The //c family's built-in devices are locked to their hardware
positions. Profile changes reset the machine and remount disks where possible.
An absent or unreadable main ROM refuses a profile change and keeps the
current machine and its configuration.

At power-on, the supplied ROMs choose boot devices in this order:

| Machine | Automatic boot |
|---|---|
| Original Apple II | No disk autostart in the original ROM |
| II+ / unenhanced IIe (NTSC or PAL) | Slots 7 → 1, requiring the older four-byte boot signature |
| Enhanced IIe (NTSC or PAL) | Slots 7 → 1; HDV cards qualify too |
| //c (NTSC or PAL, SmartPort-capable ROM) | Internal S6D1, then SmartPort S5; no automatic S6D2 attempt |
| //c+ | Internal Sony, connected external Sonys, external SmartPort, then first 5¼-inch drive |

The Drives window groups controllers and their drives; its row order is not
a per-drive boot sequence. **Insert + Boot** selects the mounted controller.
If its firmware cannot select the requested drive or unit, POM2 reports why;
on //c+, booting an external SmartPort image requires ejecting Sony disks first.

## Explore the hardware

POM2 uses MAME as a hardware reference, with regression tests for its CPU and
device behaviour. The interface leaves room to explore the cards that made
these machines so versatile.

| Area | Available hardware and tools |
|---|---|
| **Video** | Text, lo-res, hi-res, double hi-res and 80 columns; composite NTSC, AppleWin NTSC, monochrome, Video-7, Le Chat Mauve RGB and VidHD Super Hi-Res on a //e |
| **Memory** | Language Card, IIe auxiliary memory, RamWorks III up to 8 MB and Slinky 1 MB expansion |
| **Sound** | Speaker, cassette, Mockingboard A/C, Sound II with SSI263 speech, Phasor with dual SSI263AP, Cricket / Echo, Echo+ TMS5220C and S.A.M. DAC; stereo mixing and mono output |
| **Storage** | Disk II, Sony 3½-inch mechanisms, SmartPort, Liron, ProDOS HDV, CFFA 2.0, CFFA3000 SmartPort HLE and Floppy Emu |
| **Input and printing** | Mouse Card, joysticks/paddles, 4play, SNES MAX, Super Serial, parallel interfaces and ImageWriter II |
| **Expansion** | Microsoft SoftCard Z80, TransWarp, Zip Chip socket accelerator, Videx Videoterm, clocks and Apple II Workstation Card |
| **Connections** | Uthernet I/II, FujiNet relay, TNFS disk downloads and a ProDOS host-folder volume |
| **Creative tools** | HGR/DHGR Paint Editor, HGR Sprite Editor, screenshots and print exports |
| **Inspection** | Memory viewer, bank-aware debugger with symbols, conditional breakpoints, step over/out, source correspondence, profiling and history; snapshots, resumable sessions and an optional AI-control HTTP API |

For card keys, firmware details and implementation notes, see
[CLAUDE.md](../CLAUDE.md) and [DEV.md](../DEV.md).

<details>
<summary>Networking and a few hardware boundaries</summary>

- **Uthernet II:** TCP/UDP use host sockets on desktop Linux, macOS and Windows.
  With `uthernet_allow_loopback=true`, `LISTEN` accepts local TCP clients at
  `127.0.0.1` on the guest's chosen port (1024+; 1985/6502/6503 excluded).
  LAN listening/port forwarding is not implemented.
  Uthernet I and raw Ethernet modes require libslirp, included in the ARM64,
  Raspberry Pi and Pi 400 Linux packages. Source builds can enable it with
  `-DPOM2_ENABLE_SLIRP=ON` when the library is available.
- **FujiNet:** The macOS, Windows x64 and Linux x64 desktop packages include
  [FujiNet-PC Apple v1.5.2](https://github.com/FujiNetWIFI/fujinet-firmware/releases/tag/v1.5.2)
  and its CONFIG disk. POM2 relays SmartPort calls to that program or a
  physical USB board. Use an II+ / //e profile, add the FujiNet card in slot 7,
  then open **FujiNet** and select **Boot FujiNet / CONFIG**. The button starts
  the TCP link and bundled program, waits for FujiNet to connect, then boots
  its first drive. The bundled program is copied to POM2's per-user data
  directory before launch so its configuration and SD card remain writable.
  To use a physical USB board, select the serial transport instead. Linux ARM
  packages have no bundled FujiNet-PC binary because upstream supplies none;
  a physical board can still be used. The bundled program requires macOS 14+
  on Apple Silicon, macOS 15+ on Intel, or Ubuntu 22.04 compatible libraries
  on Linux x64. See `POM2 --help` for connection options.
  The FujiNet panel's **Timeout (ms)** control adjusts local calls from
  50 to 5000 ms. Remote host/directory operations and mounted-image block I/O
  receive up to five seconds; a slow call can pause the interface while it
  waits for the peer.
- **Workstation Card:** Apple's firmware boots and passes its diagnostics.
  A LocalTalk network and AppleShare netboot are not implemented.
- **//c and //c+:** SmartPort units are supported; the //c+ also boots through
  its internal Sony drive's IWM/MIG path. The UniDisk drive-side 65C02 firmware
  is represented by its protocol rather than emulated as another CPU.
- **Compatibility:** some protected games require older II/II+ hardware.
  Mouse positioning can drift in A2DeskTop / MGTK. Use the issue tracker for
  reproducible problems, including the image format and selected profile.

</details>

## Your disks and saves

Images are **writable by default**. Games can save scores and progress back to
them. Storage panels report pending, saving, saved or failed writes; disk writes
are saved automatically in the background.

Enable **Write-protected** in a media panel or Slot Configuration, or right-click
an image in the Disk Library. This changes the host file's write permissions;
WOZ/2IMG header protection also applies. For a protected collection by default,
launch with `POM2_MEDIA_WRITE_DEFAULT=protected`.

You can organise your own collection with these folders:

```text
bundled_disks/  The four disks selected for release packages
disks_5.4/     Your 5¼-inch images
disks_3.5/     Your 3½-inch images
hdv/          Your hard-disk images
floppyemu/    Floppy Emu media (created on first run)
roms/         System and peripheral firmware
```

### Share a ProDOS folder

Put files in `prodos_folder/`, then mount its **[host folder]** entry from the
HDV panel. It appears to ProDOS as `/HOST/`. Host modification times and
read-only permissions are reflected in the volume; ProDOS file type and aux
type come from extensions or portable `NAME#TTAAAA` names (`TT` is the file
type and `AAAA` the auxiliary type, both hexadecimal). For example,
`PIC#062000` is a BIN file loaded at `$2000`. A name tag is used on export
whenever an extension alone would lose either value; it survives copying the
folder between macOS, Linux and Windows. Enable **Convert host-folder
TXT line endings on next mount** to exchange ProDOS TXT (`$04`) files as CR
inside the Apple II and LF on the host. The option is off by default and
does not change binary files.

Guest edits are held in memory until ejection or quit. Enable **Sync to host
folder (on eject)** in the HDV panel to save them. The panel reports pending
guest edits and the mounted text mode. If a host file changes during a mount,
the host version stays in place and the volume's version is saved in the hidden
`.pom2-conflicts/` folder. Repeated distinct conflicts get numbered copies;
review them manually. Conflict detection is conservative: a host file newer
than the mount is kept even if the volume still holds its original snapshot.
Files deleted by the guest are left on the host.

Screenshots and printouts go into POM2's per-user data directory:

| System | Data directory |
|---|---|
| Linux | `~/.local/share/POM2/` (`XDG_DATA_HOME` is honoured) |
| macOS | `~/Library/Application Support/POM2/` |
| Windows | `%LOCALAPPDATA%\POM2\` |

Replacement ROMs can go in `roms/` under that directory. Firmware and other
third-party material have their own rights and notices; see
[THIRD-PARTY.md](../THIRD-PARTY.md).

## Command line

```bash
POM2 "my-game.woz"                          # mount and boot
POM2 --preset iic "my-game.dsk"             # choose a machine
POM2 --kiosk "my-game.dsk"                  # fullscreen
POM2 tnfs://host/path/image.po              # fetch, cache and boot
POM2 --snapshot-save session.pom2snap
POM2 --snapshot-load session.pom2snap
POM2 --session-save game.pom2session        # bundle supported machine/media
POM2 --session-load game.pom2session        # reconstruct the saved machine
POM2 --help                                # all options
```

Actions such as snapshot/session load/save, stepping and paste run in command-line
order after startup settles. `--speed` sets a cycles-per-frame budget: normal
speed is 17045 for NTSC or 20313 for PAL. `--cpu-max` runs without normal pacing;
the achievable emulation speed depends on the host.

Cassette output saves as ACI by default or WAV with `--save-tape-format wav`.
An explicit `.aci`/`.wav` suffix wins over that hint; other suffixes receive
an appended format suffix, so `--save-tape out.mp3` creates `out.mp3.aci`.
MP3 is an input format, not a recording encoder.

Other options cover speed, display mode, cassette recording, blank disks,
ProDOS host folders, FujiNet and automation. Optional values use `=`:
`--ai-control=6503`, for example. CLI `--rewind` rewinds the **cassette**;
F6 and the Rewind panel control machine time-travel.

## Build from source

### Linux and macOS

You need Git, a C++17 compiler, CMake and GLFW. The setup script installs
platform dependencies and fetches the pinned Dear ImGui docking revision.

```bash
git clone https://github.com/habib256/pom2.git
cd pom2
./setup_imgui.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
./build/POM2
```

On Raspberry Pi, configure with `-DPOM2_GLES=ON` and install the GLES/EGL
development packages. See the [Raspberry Pi guide](../packaging/raspberry/README.md)
for native builds and performance tuning.

<details>
<summary>Windows build</summary>

Install Visual Studio's C++ workload, CMake, Git and vcpkg. Fetch the same
Dear ImGui revision used by setup and CI; the pin lives in `imgui_pin.env`.
Run these commands in PowerShell from the repository root:

```powershell
git clone https://github.com/ocornut/imgui.git
$imguiPin = ((Get-Content imgui_pin.env | Where-Object { $_ -match '^IMGUI_COMMIT=' }) -split '=', 2)[1]
git -C imgui checkout $imguiPin
vcpkg install --triplet x64-windows-static
cmake -S . -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DPOM2_ENABLE_TESTS=OFF
cmake --build build --config Release --parallel 3
./build/Release/POM2.exe
```

The manifest supplies the dependencies. The static triplet matches the release
build's CRT and avoids shipping extra DLLs.

This recipe builds the application. To run the same portable core tests as
Windows CI, use a separate build directory with the same toolchain/triplet:

```powershell
cmake -S . -B build-portable "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DPOM2_ENABLE_TESTS=ON -DPOM2_PORTABLE_TESTS_ONLY=ON
cmake --build build-portable --config Release --parallel 3
ctest --test-dir build-portable -C Release --output-on-failure --no-tests=error --parallel 2
```

The registered subset is defined in `tests/cmake/Pom2PortableTests.cmake`;
the full suite includes POSIX-specific harnesses and is run on Linux/macOS.

</details>

<details>
<summary>WebAssembly build</summary>

Activate an [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
and populate `imgui/` with the pinned revision, then:

```bash
./build_wasm.sh
./build_wasm.sh --serve
```

The local server supplies COOP/COEP headers, but the current browser build is
single-threaded and does not require SharedArrayBuffer or a pthread worker.
The asset
list comes from [the package manifest](../packaging/bundle.manifest), including
the curated disks. `--with-data` adds the local `disks_3.5/` collection for
**development only**; do not publish that larger personal bundle.

The browser opens a PAL Apple //c and boots DIX-fix by default, with the
Apple II Screen docked on the left and Drives on the right. URL parameters
`?preset=...&disk=...` override the machine and boot image; `?nodisk=1`
starts without a disk. Changing only `preset` keeps the default DIX-fix image.
For an II+ without a boot image, use `?preset=ii%2B&nodisk=1` (`%2B` encodes
the plus sign in the profile name).

Host socket networking and the listening AI-control HTTP server are unavailable
in the browser.

</details>

### Tests and packages

On Linux and macOS, configure and run the full native suite:

```bash
cmake -S . -B build -DPOM2_ENABLE_TESTS=ON
cmake --build build --parallel 3
ctest --test-dir build --parallel 2 --output-on-failure
```

[CONTRIBUTING.md](../CONTRIBUTING.md) explains the test suite, source guards and
requirements for fixes. [PERFORMANCE.md](PERFORMANCE.md) covers
measurements and profiling.

Release notes live in `docs/releases/`. A version tag triggers the
[release workflow](../.github/workflows/release.yml); a manual workflow run builds
artifacts without publishing a release. All platforms read the same
[package manifest](../packaging/bundle.manifest).

Local packaging: `./build_dist.sh` on Linux, `./package_macos_release.sh` on macOS,
and `package_windows_release.bat` on Windows.

## Credits and project links

POM2 is by Arnaud Verhille. It draws on MAME, AppleWin, OpenEmulator and
MicroM8 hardware and video work; the Monitor II and Disk II artwork is based
on [sscanf/apple2e-emulator](../pic/retro/). French Touch and other Apple II
authors made the included demos, games and tools. Full licenses and notices
are in [THIRD-PARTY.md](../THIRD-PARTY.md).

[Report an issue](https://github.com/habib256/pom2/issues) ·
[Security reports](../SECURITY.md) ·
[License](../LICENSE) ·
[Changelog](../CHANGELOG.md) ·
[Developer guide](../DEV.md) ·
[Backlog](../TODO.md)
