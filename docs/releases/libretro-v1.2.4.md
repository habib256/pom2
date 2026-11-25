# POM2 Libretro v1.2.4

Built from the same emulator as POM2 v1.2.4.

- **Netplay and run-ahead in RetroArch.** The `.info` declares
  `savestate_features = "deterministic"`, which RetroArch requires for both
  (#24). The core was already deterministic: netplay is checked frame by
  frame in CI. The v1.2.3 zips were corrected the same way.
- **Disk saves a frontend can find** (RetroBat, emulatorlauncher#1498). The
  private copy of a disk is now `saves/pom2/disks/<game>/<disk file>`, for
  example `Ultima IV/Ultima IV side B.dsk` for a disk of `Ultima IV.m3u`.
  A second game with the same names, or a new revision of an image, gets
  its own copy, `<disk> [<8 hex digits>]`, instead of booting the first
  one's save. A copy saved by v1.2.3 moves to its new name, progress
  included, the first time its game is opened.
- **Keyboard games on a RetroPad.** 39 games have their keys put on the pad,
  read off their manuals and reference cards. A joystick game keeps the
  D-pad and B/A on the joystick and gets its keys on the other buttons; a
  keyboard-only game gets its movement keys on the D-pad. RetroArch's
  controls menu names each key. Core option *Game pad profiles*; it applies
  when port 1 is the Apple II joystick.
- **A memory map RetroArch reads in full.** The 48 KB below the language
  card was one block, which RetroArch's map preprocessing refuses, so its
  cheat search saw nothing. It is now two blocks, in main and aux RAM.
  RetroAchievements reads every address `$00000-$1FFFF`, in the layout
  RAppleWin uses.
- **Android.** Content picked through Android's storage picker
  (`saf://`) now boots: the core opens it through RetroArch's VFS.
- **A netplay mouse.** Port 4 has "Mouse (for netplay)": RetroArch's
  netplay carries a mouse only from a port set to a mouse device. Checked
  with Apple II DeskTop, at 150 % mouse speed.

The core also builds for iOS, tvOS, RetroArch's web player and 32-bit ARM
Linux. Each build is checked in CI, the web one by booting a game under
Node and the ARM one under qemu. Those builds will come from libretro's
buildbot once the core is accepted there (libretro/libretro-super#2223); this
release carries the five packages below.
