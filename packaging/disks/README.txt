POM2 — bundled disks

A2FILECMD-PRODOS-XL-0.9.8.2mg — A2FC XL v0.9.8 (6502 edition).
Source: https://github.com/habib256/a2filecmd/releases/tag/v0.9.8
Mount in a ProDOS hard-disk bay (HDV / SmartPort), then boot that slot.
Requires 128 KB and 80 columns; runs on unenhanced and enhanced IIe profiles.

DIX-fix.po — French Touch anthology with POM2's 12-byte menu patch.
Source: https://github.com/Fr3nchT0uch/DIX
Built reproducibly from DIX.po with tools/make_dix_fix.py; patch details:
https://github.com/habib256/pom2/blob/main/docs/test_corpus.md
The recorded //e setup uses PAL timing, 128 KB, slot-4 Mockingboard and a
slot-5 SmartPort/Liron 3.5-inch bay. DIX's loader scans for its Mockingboard;
standalone demos such as MAD EFFECT may instead hard-code slot 4. The browser
uses a PAL //c with its Mockingboard 4c expansion and SmartPort media.
RETURN/SPACE at the fresh menu starts the automatic demo sequence.

DIRTBIKE.dsk — Dirtbike 1.1.
Source: https://github.com/habib256/pom2games/releases/tag/dirtbike-1.1
Use II+ or later with 48 KB; mount as a 5.25-inch disk and boot it.
Keyboard, joystick and paddles supported; best times and scores save to disk.

CHROMABREAK.po — ChromaBreak 1.1.
Source: https://github.com/habib256/pom2games/releases/tag/chromabreak-1.1
Use //e enhanced or //c with 128 KB; mount as a 5.25-inch disk and boot it.

MICRO-SOKOBAN.dsk — Micro-Sokoban 1.3.
Source: https://github.com/habib256/pom2games/releases/tag/micro-sokoban-1.3
Use II+ or later; mount as a 5.25-inch disk and boot it.

Sources and license notices are available in the linked author repositories.
SHA256SUMS.txt records the pristine images shipped with this release.
The Disk Library lists these alongside your own disks. Games can write saves
in their images, so keep a backup or enable disk protection when preferred.
