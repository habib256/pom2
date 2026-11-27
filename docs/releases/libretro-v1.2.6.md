# POM2 Libretro v1.2.6

Built from the same emulator as POM2 v1.2.6.

- **`.7z` content.** A disk packed in a `.7z` loads as a `.zip` does: its
  first disk is mounted (or the one its own `.m3u` names first), the others
  are beside it in Disc Control, and the archive is never written. LZMA,
  LZMA2 and PPMd — what 7-Zip, p7zip and libarchive write by default; a
  Deflate, BZip2 or encrypted `.7z` is refused in those words.

The emulator's own changes in this version are in the debugger, which a
frontend does not show: nothing else changes for a RetroArch player since
v1.2.4.
