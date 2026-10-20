Floppy Emu (BMOW) — SD card folder
==================================

This folder is the virtual "SD card" for POM2's Floppy Emu device
(Devices -> Floppy Emu), modeling four supported modes of the Big Mess o'
Wires Floppy Emu disk emulator. Drop your disk images here and pick them
from the Floppy Emu's on-screen OLED browser.

It is kept SEPARATE from the Disk Library folders (disks_5.4/, disks_3.5/,
hdv/) on purpose — the Floppy Emu has its own card, just like the real
device. You can subfolder freely (e.g. floppyemu/GAMES/, floppyemu/OS/);
the OLED file browser walks into subdirectories.

Which images show up depends on the selected emulation mode:

  Apple II 5.25 Floppy   .dsk .do .po .nib .woz .2mg   (140K)
  Apple II 3.5 Floppy    .dsk .do .po .moof .woz .2mg  (supported 3.5" media)
  Unidisk 3.5            .dsk .do .po .moof .woz .2mg  (supported 3.5" media)
  Smartport Hard Disk    .po .hdv .2mg                 (up to 32 MB)

These are browser extension filters, not a promise that every capture can be
mounted in every mechanism. The selected drive/controller validates the file.
Dual-5.25" and SmartPort-Unit-2 modes are not implemented.

Optional: a "favdisks.txt" file in this folder turns on the Favorites
menu. List one image path per line, relative to this folder. Absolute paths
are accepted only when they still name something inside the configured SD
root; paths that escape it, including through symlinks, are ignored.
Blank lines and lines beginning with # are ignored.

The parser accepts an optional first meaningful line "automount N"
(0 = never, 1 = first favorite, 2 = most recently used), preserving the
real Floppy Emu's format. The current POM2 interface does not use that
directive to mount a disk at startup. Select a favorite manually to mount
and boot it. The selected mode determines the target drive; file extensions
are browser filters and do not override the mode.

The folder location can be changed via the floppyemu_sd_root setting in
state.cfg.
