# RetroBIOS gap — what POM2 needs that RetroBIOS `main` no longer serves

**Date:** 2026-10-29. **RetroBIOS `main`:** `e9ef630b3a` (2026-10-28).
**POM2 pin:** `POM2_RETROBIOS_COMMIT` = `f71b006` (`src/RomFetch.h`).

## Why this matters now

Release packages stopped carrying the full `roms/` tree on 2026-10-29: they
ship the //c firmware DIX needs (`apple2c-32Kv0.rom`, `apple2e_char.rom`) and
fetch everything else from RetroBIOS. The download reads the **pinned**
commit, where all 49 catalogue entries resolve and match their SHA-256
(`rom_fetch_live_probe`, 2026-10-28). The pin is only as durable as that
commit: if RetroBIOS rewrites its history, or GitHub removes the repository,
the pinned URLs die with it, and `main` cannot replace them.

## Method

1. Every catalogue URL with the commit replaced by `main`: HTTP status.
2. The git blob SHA-1 of each dump in POM2's `roms/` looked up in the full
   `main` tree (`git/trees/main?recursive=1`, 25 678 entries, not
   truncated) — finds a loose copy under any name and any folder.
3. Every Apple II zip on `main` (84 archives, `.variants/` included)
   downloaded, each member SHA-256 compared with the catalogue.

## Results

**URLs:** 28 of the 43 distinct catalogue URLs answer 404 on `main`.

**Still on `main`, under some path** — 27 of 49:

- loose (20): `apple2o`, `apple2` (`apple2-asoft-auto.rom`), `apple2e`,
  `apple2e_unenh` (`bios/Other/mariani/Apple2e.rom`), `apple2_char`,
  `apple2e_char` = `apple2e_char_us`, `apple2e_char_us_unenh` =
  `apple2e_char_2k`, the four Disk II PROMs, `grappler_eps-1`, `341-0057`,
  the four Videx Videoterm dumps, the SSC EPROM (`bios/Other/mariani/SSC.rom`);
- inside a zip only (7 more): ThunderClock+ (`a2thunpl.zip`), TransWarp
  (`a2twarp.zip`), both CFFA 2.0 builds (`a2cffa02.zip`, `a2cffa2.zip`),
  both mouse ROMs (`a2mouse.zip`), Slinky (`a2memexp.zip`), SuperDrive
  `341-0438-a` (`a2superdrive.zip`).

**Gone from `main` entirely — 22 dumps:**

| Dump | What depends on it |
|---|---|
| `apple2c-32Kv0.rom`, `apple2c-16K.rom`, `3420033a.256` | every //c profile (incl. //c PAL — shipped in packages) |
| `apple2cp.rom` | //c Plus |
| `apple2p.rom` (20 KB) | ][+ — `apple2p.zip` still has the six 2 KB chips, i.e. the 12 KB image POM2 also accepts (`altPresentSize`) |
| `liron.rom` | Liron card; SmartPort card falls back to synthetic firmware |
| `341-0358-A.bin` | Apple II Workstation Card |
| `grappler_plus.bin` | Grappler+ (synthetic stub without it) |
| `apple2e_char_fr`, `_frca`, `_frca_unenh`, `_uk`, `_uk_unenh`, `_de`, `_de_improved`, `_us_mt_iigs`, `_reactive`, `_ft_blockascii` | View ▸ Character set locales |
| `342-0274-a.e9` | international //e video ROM |
| `Videx Lower Case Chip ROM.bin` | ][ / ][+ lowercase |
| `342-0326-a.f12` | nothing (MAME oracle only) |

## Decision (2026-10-29)

POM2's own archive, <https://archive.org/details/pom2_roms_202610> (`pom2_roms.zip`,
all 49 catalogue entries as `roms/<probed name>`), is the **primary** source;
RetroBIOS at the pin is the per-entry **fallback**. `rom_fetch_live_probe`
downloads the whole catalogue from each source alone: 49/49 from both.

## Options considered

1. **Keep the pin** (today). Works as long as `f71b006` is reachable.
2. **Re-point the 27 that `main` still has** to `main`-era paths (several
   only inside zips: the fetcher already unpacks members), and keep the pin
   for the 22 — halves the exposure.
3. **Propose the 22 back to RetroBIOS**, as PR #75 did — their 2026-10-20
   change dropped "copies only an install provides", so ask first which form
   they accept (MAME romset zips rather than loose renamed files?).
4. **A mirror POM2 controls** for the 22, with the same SHA-256 gate — a
   redistribution decision, which is exactly what the package change avoids.
