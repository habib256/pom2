POM2 — Apple ROMs
=================

A POM2 package carries NO firmware: Apple's ROMs and every card's are
copyrighted and are not redistributed. This folder ships with the
floppy_samples/ drive sounds only.

At the first launch POM2 offers to download them from POM2's ROM
archive (archive.org/details/pom2_roms_202610), with the RetroBIOS collection
(github.com/Abdess/retrobios) as a fallback; each file is checked against
its SHA-256. Any time later: Help -> ROM Status -> Download missing ROMs. A dump you already own can be dropped onto the POM2 window (or
use Import ROM file... in ROM Status): POM2 recognises it by content and
installs it under the right name, whatever it is called.

Downloads and imports go into this folder when POM2 can write to it (the
Windows zip, a portable tarball). Otherwise — the AppImage, a .deb install,
and always on macOS, so that the .app stays intact and an update keeps your
ROMs — they go to the per-user data directory, which POM2 searches first
(and where your own overrides belong):

  Linux                   ~/.local/share/POM2/roms/
  macOS                   ~/Library/Application Support/POM2/roms/
  Windows                 %LOCALAPPDATA%\POM2\roms\

`ResourcePaths` also probes a `roms/` folder next to the executable and the
current working directory (dev layout).

The override must use the filename the active profile prefers. Filename
preference is checked before directory preference: for example, a bundled
apple2p.rom wins over a per-user apple2.rom on the II+ profile. Put your II+
override at roms/apple2p.rom under the per-user data directory. An existing
but invalid preferred main ROM refuses a profile change; it does not silently
continue to a later alias. See docs/USER-GUIDE.md in the source repository
for the profile filename table, and Help -> ROM Status for the live inventory.

Expected files
--------------
  Main system ROMs
    apple2.rom            generic fallback (12 KB $D000-$FFFF or 16 KB $C000-)
    apple2o.rom           Apple ][ Original (1977)
    apple2p.rom           Apple ][+ (1979)
    apple2e.rom           Apple //e Enhanced (1985) + PAL profile
    apple2e_unenh.rom     Apple //e Unenhanced (1983)
    apple2c-32Kv0.rom     Apple //c (1984)   (or apple2c-16K.rom)
    3420033a.256          Apple //c third probe — MAME's apple2c0 part,
                          the "//c (UniDisk 3.5)" variant
    a2c.128               Apple //c 16 KB ROM dump
    apple2cp.rom          Apple //c Plus (1988)

  Character ROMs
    apple2_char.rom       Apple ][ / ][+ character generator
    apple2e_char.rom      Apple //e character generator
    apple2e_char_*.rom    regional / unenhanced variants
    342-0274-a.e9         international //e video ROM — one 8 KB part
                          holding two 4 KB banks (FR-CA low, US high)
    Videx Lower Case Chip ROM.bin
                          Videx LOWER CASE CHIP — the 1980 drop-in
                          generator that gave a ][ / ][+ lowercase

  Peripheral ROMs
    disk2.rom             Disk II 16-sector boot PROM (slot 6 auto-boot)
    disk2_13.rom          Disk II 13-sector boot PROM
    diskii_p6.rom         Disk II P6 LSS PROM (16-sector)
    diskii_p6_13.rom      Disk II P6 LSS PROM (13-sector)
    mouse_341-0270-c.bin  AppleMouse II card firmware
    mouse_341-0269.bin    AppleMouse II 68705 MCU mask ROM
    ssc_341-0065-a.bin    Super Serial Card EPROM (341-0065-A)
    cffa20ee02.bin        CFFA 2.0 6502 firmware
    cffa20eec02.bin       CFFA 2.0 65C02 firmware
    grappler_plus.bin     Orange Micro Grappler+ 4 KB EPROM
    grappler_eps-1.bin    Orange Micro Grappler (1981) 2 KB EPROM
    341-0057.bin          Apple II Parallel Interface Card PROM
    thunderclock_u9_v1.3.bin  ThunderClock+ Rev 1.3 EPROM
    liron.rom             Liron-class SmartPort controller ROM
    slinky_341-0344a.bin  Apple Memory Expansion (Slinky) 4 KB firmware
    ae_transwarp_1.4.bin  TransWarp v1.4 firmware/shadow ROM
    341-0358-A.bin        Workstation Card's 64 KB coprocessor firmware
    videx_videoterm_v24_60hz.bin  Videoterm v2.4 firmware (60 Hz)
    videx_videoterm_v24_50hz.bin  alternate 50 Hz firmware
    videx_videoterm_char_normal.bin   normal character set
    videx_videoterm_char_inverse.bin  alternate/inverse character set

Notes
-----
* A 12 KB main ROM maps to $D000-$FFFF; a 16 KB one to $C000-$FFFF.
  A 20 KB combined dump skips the leading 4 KB and maps its upper 16 KB.
  A 32 KB dump uses the profile's //e versus //c bank layout; a character
  generator embedded in a combined file is not loaded as a separate charset.
* floppy_samples/ (mechanical drive sounds, BSD-3-Clause) is not firmware —
  leave it in place.
* If only apple2.rom is present for a profile that prefers a machine-specific
  dump, POM2 warns that the ROM may not match the selected machine.
* The ROM Status panel shows the current inventory, identities and fallback
  effects for catalogued entries. RomCatalog.h, SystemProfile.cpp and the
  individual card loaders define accepted paths and probe order; the list
  above describes common filenames, not every accepted alias.
* A .pom2session may carry its own supported firmware. Loading one does not
  install or replace files in this directory.

Virtual II sounds (optional, macOS): POM2 also detects original recordings
in /Applications/Virtual ][.app/Contents/Resources and the same path under
~/Applications. The configured sound folder and roms/virtual_ii_sons take
priority. AIFF and original mono 16-bit little-endian 44100 Hz RAW files
are read directly; no conversion or copying is required.
