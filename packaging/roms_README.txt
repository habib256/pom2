POM2 — bundled Apple ROMs
=========================

This directory ships with every POM2 release. It holds the system firmware,
character generators, peripheral dumps and the floppy_samples/ mechanical
drive sounds that the emulator probes at launch.

You do not need to add anything here for a stock install. To override a
single dump (or add an extra one) in a read-only package:

  Linux AppImage / .deb   ~/.local/share/POM2/roms/
  macOS .app              ~/Library/Application Support/POM2/roms/
  Windows zip             %LOCALAPPDATA%\POM2\roms\
                          (or replace the file beside POM2.exe)

`ResourcePaths` also probes a `roms/` folder next to the executable and the
current working directory (dev layout).

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
    cffa20ee02.bin        CFFA 2.0 6502 firmware
    cffa20eec02.bin       CFFA 2.0 65C02 firmware
    grappler_plus.bin     Orange Micro Grappler+ 4 KB EPROM
    thunderclock_u9_v1.3.bin  ThunderClock+ Rev 1.3 EPROM
    liron.rom             Liron-class SmartPort controller ROM

Notes
-----
* A 12 KB main ROM maps to $D000-$FFFF; a 16 KB one to $C000-$FFFF.
* floppy_samples/ (mechanical drive sounds, BSD-3-Clause) is not firmware —
  leave it in place.
* If only apple2.rom is present for a profile that prefers a machine-specific
  dump, POM2 warns that the ROM may not match the selected machine.
