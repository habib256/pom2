# POM2 Libretro v1.2.3

The first release of POM2's libretro core, built from the same emulator as
POM2 v1.2.3.

- **Every Apple II**: ][, ][+, //e, //e Enhanced, //c and //c Plus, NTSC or
  PAL, chosen by the core options or by what a WOZ disk says it runs on.
- **Disks**: Disk II (WOZ with flux tracks, NIB, DSK, D13), 3.5-inch
  SmartPort, ProDOS hard disks, two drives, M3U playlists, private writable
  copies of every disk, and ShrinkIt, DiskCopy 4.2, A2R flux captures, zip
  and gzip archives opened as they are. 13-sector (DOS 3.2) disks boot with
  no Apple firmware, on the original ][ when the WOZ names it.
- **Tapes and programs**: a `.wav` or `.aci` tape is read before the machine
  starts, which picks it (Integer BASIC: the original ][) and types `LOAD`
  and `RUN`; an Applesoft listing or a binary loads at the BASIC prompt.
- **Cards**: Mockingboard (with SSI263 speech), Phasor, AppleMouse, Le Chat
  Mauve RGB, Passport MIDI (to RetroArch's MIDI output), RamWorks, Zip Chip,
  FASTChip, No-Slot Clock, the DIX preset.
- **Picture**: eleven monitors from the composite colour TV to OpenEmulator's
  signal and AppleWin's NTSC, plus POM2's CRT glass (scanlines, curvature,
  phosphor afterglow, mask, glow) as slang presets in the ZIP's `shaders`
  folder: copy it into RetroArch's Shaders directory and turn shaders on.
- **Play**: save states and rewind on every configuration, run-ahead, a
  RetroPad keyboard and an on-screen one, analog paddles, cheats and the
  RetroAchievements memory map; frameskip on RetroArch's audio buffer for
  slow devices.
- **Netplay**: two machines given the same inputs keep the same state byte
  for byte, a rollback replays exactly and a joining player stays in step,
  on every machine (the //c Plus at 4 MHz too) and every disk type; CI checks
  it on every package. For keyboard games set **Port 3 → Keyboard (for
  netplay)**: RetroArch's netplay carries a keyboard only as a device.

Compatibility: on 4am's woz-a-day collection (2245 titles in scope), POM2
boots 99.3 % to a picture, against 98.7 % for MAME and 98.2 % for AppleWin,
and every title either of them boots,
all three judged alike ([the audit](https://github.com/habib256/pom2/blob/main/docs/audits/woz-a-day-2026-11-24.md)).
