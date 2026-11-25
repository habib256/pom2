# POM2 Libretro v1.2.4

Built from the same emulator as POM2 v1.2.4.

- **Netplay and run-ahead in RetroArch.** The `.info` declares
  `savestate_features = "deterministic"`, which RetroArch requires for both
  (#24). The core was already deterministic: netplay is checked frame by
  frame in CI. The v1.2.3 zips were corrected the same way.
- **The Windows core no longer needs the Visual C++ runtime.** v1.2.3's DLL
  did not open on a PC without the VC++ redistributable, which RetroArch
  does not install. The runtime is now built in, and CI refuses a core that
  needs it.
- **Disk saves a frontend can find** (RetroBat, RetroBat-Official/emulatorlauncher#1498). The
  private copy of a disk is now `saves/pom2/disks/<game>/<disk file>`, for
  example `Ultima IV/Ultima IV side B.dsk` for a disk of `Ultima IV.m3u`.
  Names are plain ASCII, the same on every platform, so a save folder synced
  between a PC and a phone matches; a name that had to change (accents,
  characters Windows refuses) keeps `[<8 hex digits>]`. A second game with
  the same names, or a new revision of an image, gets its own copy,
  `<disk> [<8 hex digits>]`, instead of booting the first one's save. A copy
  saved by v1.2.3 moves to its new name, progress included, the first time
  its game is opened; if two v1.2.3 copies claim one name, the one with
  progress plays and the other is kept beside it as `[old …]`.
- **The web core shows a message instead of ending the page** on a zip with
  no disk or a broken playlist (RetroArch's web build cannot catch errors).
- **Run-ahead with a disk.** A restore no longer reads and hashes the disk
  image again (it cost 12 ms a frame for a 3.5" disk, 188 ms for a hard
  disk), and "Run-Ahead: use second instance" no longer turns itself off
  when a disk can be written. Joining a netplay game no longer fails on a
  disk the host changed to.
- **Keyboard games on a RetroPad.** 39 games have their keys put on the pad,
  read off their manuals and reference cards. A joystick game keeps the
  D-pad and B/A on the joystick and gets its keys on the other buttons; a
  keyboard-only game gets its movement keys on the D-pad. RetroArch's
  controls menu names each key. Core option *Game pad profiles*; it applies
  when port 1 is the Apple II joystick. In netplay the profile travels with
  the state, so a player who joins takes the host's.
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

Before this release, four rounds of review over everything since v1.2.3
fixed 110 defects ([the list](https://github.com/habib256/pom2/blob/libretro-v1.2.4/docs/audits/bug-hunt-2026-11-25.md)).

The core also builds for iOS, tvOS, RetroArch's web player and 32-bit ARM
Linux. Each build is checked in CI, the web one by booting a game under
Node and the ARM one under qemu. Those builds will come from libretro's
buildbot once the core is accepted there (libretro/libretro-super#2223); this
release carries the five packages below.
