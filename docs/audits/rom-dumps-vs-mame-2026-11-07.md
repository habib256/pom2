# The ROM dumps against MAME's catalogue — 2026-11-07

`tools/audit_rom_dumps.py` compares every file in `roms/` with the CRC-32 of
every `ROM_LOAD` in MAME's Apple drivers and `a2bus` cards at `b2a182f0b7`
(the revision POM2's citations pin). It was written after the ][+ dump turned
out to carry a wrong IRQ vector that nobody had seen since 2005
(`src/RomErrata.h`). Grade of a match: [D].

Run on the 63 files of a full `roms/`: **41 MATCH, 3 TILED, 4 PARTIAL, 15 NONE**.
One defect in code POM2 executes, already corrected; one mislabel. Every
other difference is explained below.

## PARTIAL — explained

| File | What differs | Verdict |
|---|---|---|
| `apple2p.rom` (20 KB) | `$B000-$CFFF` (+0x0 to +0x1800) and the `$F800` page (+0x4800). `$D000-$F7FF` are 341-0011 to 341-0015. | `$B000-$CFFF` is filler: a ][+ has no ROM there. The loader skips the first 4 KB and the I/O page, and `$C100-$CFFF` is slot space, answered by the slot bus. **The F8 page is the defect**: `$FFFE/$FFFF` = `$59 $FF` where 341-0020-00 has `$40 $FA`. Corrected in memory by `RomErrata.h`; the source fix is [RELEASE-012](../backlog/release-1.0.md#release-012). |
| `apple2c-32Kv0.rom` | Byte-identical to MAME's `3420033a.256` except bytes `0x000-0x0FF`. | Bank 0's `$C000-$C0FF`: soft switches, never read as ROM, and the loader ignores them. No effect. |
| `apple2e.rom` (32 KB) | The upper 16 KB are `342-0304-a.e10` + `342-0303-a.e5`, the enhanced //e. The lower 16 KB: a second `342-0303-a.e5` and 8 KB MAME does not catalogue. | The loader takes the upper 16 KB of a 32 KB //e dump (`Memory::loadAppleIIRom`). No effect. |
| `apple2e_char_ft_blockascii.rom` (8 KB) | The high bank is the US //e set; the low bank is not in MAME. | French Touch's block-ASCII font, a custom set by design (CLAUDE.md, Display). |

## NONE — explained

| File | Why MAME has no match |
|---|---|
| `grappler_plus.bin` | It says **"Grappler+ Version 3.1"** in the clear. MAME has 3.0 (`17cf5e02`) and 3.2 (`6f88b70c`) only. A third revision, not a bad dump. `docs/printer-detection.md` already named it 3.1; **`RomFetch.cpp` called it "EPROM 3.2", corrected.** |
| `diskii_p6_13.rom` | The 13-sector P6 (341-0010). MAME's `wozfdc` loads the 16-sector 341-0028 only. |
| `liron.rom` | The Liron's ROM, dumped by BMOW; MAME still lists it as WANTED (CLAUDE.md). |
| `dan2_eprom.bin` | The DAN ][ controller; not in MAME (izapple2). |
| `341-0358-A.bin` | The Workstation Card firmware; not in MAME's `a2bus`. Catalogued with its own CRC in `RomCatalog.h`. |
| `342-0326-a.f12` | The //e international keyboard-decode ROM (FR); MAME's `apple2eefr` lists no separate keyboard ROM. Catalogued with its own CRC. |
| `apple2e_char_de`, `_de_improved`, `_fr`, `_frca`, `_frca_unenh`, `_uk`, `_uk_unenh`, `_us_mt_iigs`, `_reactive` | National, improved or third-party character sets. MAME carries the French one only as the 8 KB `342-0274-a.e9`, which `apple2e_char_fr_8k.rom` matches. `_fr` and `_frca_unenh` are the same file (`ab0be706`). |

## What would settle the rest

- Each NONE above is identified, not verified: a CRC from a second, independent
  dump of the same chip would raise it from "identified" to [D] or better.
- Re-run the tool whenever a dump is added to `RomCatalog.h`, or the MAME pin moves.
