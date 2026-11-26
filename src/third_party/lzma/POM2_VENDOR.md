# LZMA SDK — the 7z decoder POM2 opens `.7z` disks with

**Upstream**: 7-Zip 24.09 by Igor Pavlov, <https://www.7-zip.org/> — the
`C/` directory of his repository's tag `24.09`,
<https://github.com/ip7z/7zip/archive/refs/tags/24.09.tar.gz> (SHA-256
`e4757b307925227724a2044651193664ba3d04e9ac13b8e631f1667896014bbf`).
`License.txt` and `lzma.txt` are its `DOC/License.txt` and `DOC/lzma.txt`.

**Licence**: every file taken here states *"Igor Pavlov : Public domain"* in
its header (the LGPL in `License.txt` covers other parts of 7-Zip — the C++
archivers, the RAR code — none of which is here). Public domain sits inside
POM2's GPL-3.0-or-later without friction.

**What is taken**: the decoder half only — the 7z container (`7zArcIn.c`,
`7zDec.c`, `7zStream.c`, `7zBuf.c`, `7zAlloc.c`, `7zCrc*.c`), the LZMA,
LZMA2 and PPMd (H) decoders, the BCJ / BCJ2 / ARM / ARM64 / PPC / SPARC /
IA64 / RISC-V branch filters and Delta, and `CpuArch.c` (which the CRC code
asks for hardware CRC-32). No encoder, no assembler, no threads, no AES:
an encrypted `.7z`, or one packed with Deflate, Deflate64 or BZip2, is
refused with `SZ_ERROR_UNSUPPORTED`, and POM2 says so.

**Unchanged**: every file is byte-identical to that archive (checked with
`cmp`, all 34 files and the two notes, 2026-11-26). The
only POM2 files in this directory are this note and `Pom2LzmaPrefix.h`.

**Build** (`cmake/Pom2Lzma.cmake`): an OBJECT library `pom2_lzma`, C, with
`Z7_PPMD_SUPPORT` and `Z7_EXTRACT_ONLY`, hidden visibility, position
independent, and `Pom2LzmaPrefix.h` forced first into every file — it
renames each external name `pom2_lzma_<name>`, because RetroArch links its
own older copy of this SDK and a static libretro core would otherwise
collide with it. POM2 reaches it through one file only, `SevenZipArchive.cpp`.

**Upgrading**: replace the `.c`/`.h` files from a newer `C/`, rebuild, and
check `nm -g --defined-only` over the objects: every defined name must start
with `pom2_lzma_`. A new one goes into `Pom2LzmaPrefix.h`.
