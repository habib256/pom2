# Provenance audit P2 — the declared "unverified" (2026-11-06)

Tranche P2 of `docs/audits/BRIEF-provenance.md`. Sites listed by:

```
grep -rniE "unverified|unchecked|not verified" src/ --include='*.cpp' --include='*.h'
```

**30 lines match; 3 are claims about hardware or a format.** The other 27 use
"unchecked" in the programming sense (`loadFileUnchecked`,
`writeRamUnchecked`, `EmulationController`'s unchecked state access, the
`byteio::Reader` note in `Scc8530Device.cpp`) or are history
(`Disk35Image.cpp:620`, "A WOZ went unchecked"). The brief counted 28 such
sites on 2026-11-01; most were settled or reworded between then and now.

## Registre

| `fichier:ligne` | Affirmation (abrégée) | Grade | Source trouvée | Verdict |
|---|---|---|---|---|
| `src/Memory.cpp:725-734` | IOUDIS is set on EVERY reset; "still unverified against the IIc Technical Reference" | E → **B + C** (outcome), **F** (mechanism) | [B] *Apple IIc Technical Reference Manual*, 2nd ed. (© 1984, 1986; first printing March 1987), ch. 2, "The reset routine", p. 50: the routine "enables DHiRes switch access (by turning on IOUDis)" and "The procedure described above is the same for any type of reset"; p. 46, soft-switch table note: "The firmware normally leaves IOUDis on." [C] 16 KB //c ROM (`roms/apple2c-16K.rom`): RESET vector `$FA62` → `JSR $C41C`; that routine writes `$C079` (CLRIOUDIS), the mouse mode switches `$C058+X`, then `STA $C078` (SETIOUDIS) at `$C467` → `RTS`. | **CONFIRMÉE** for the observable outcome; **IMPRÉCISE** on the mechanism (firmware, not the RESET line) |
| `src/AppleSingle.h:31-38` | AppleSingle v1 layout (16-byte "ProDOS" filler; entry 7 = create 4, modify 4, access 2, type 2, aux 4) taken from CiderPress II; "[F] Unverified against the File Type Note" | F → **B** | [B] Apple II File Type Note $E0/0001, "AppleSingle File" (Matt Deatherage, written March 1989, revised January 1991), HTML conversion by Aaron Heiss archived at `web.archive.org/web/2016/http://www.1000bit.it/support/manuali/apple/technotes/ftyp/ftn.e0.0001.html`: version `$00010000`; "Home File System: 16 Bytes … possibly padded with blanks", ProDOS = `$50726F444F5320202020202020202020`; "The File Info entry (Entry ID = 7) … For ProDOS files, the entry is 16 bytes long and consists of the creation date and time and the modification date and time in ProDOS 8 … form, the access word, a two-byte file type and four-byte auxiliary type"; all fields "Rev.", i.e. "reverse of normal 65816/6502 order" (big-endian). | **CONFIRMÉE** — `AppleSingle.cpp` reads entry 7 big-endian (`be16`/`be32`) at offsets 8 / 10 / 12, size 16 |
| `src/CpuClock.h:231-232` | PAL picture aspect 1.507 is "unverified against any measurement" | F (unchanged) | None. The derivation's inputs (52 µs active line, ~287.5 active lines per field) are standard PAL broadcast figures, but the comment cites no document for them (E). Not researched further in the time box. | **INTROUVABLE** (the result); the comment now states the grade explicitly |

## Constats

None. No source contradicts the code.

Note on the IOUDIS site, kept here because it is the open point the brief
named: POM2's mechanism (`Memory::resetSoftSwitches` presets `ioudis = true`
in "hardware") differs from the documented one (the reset *routine* sets it).
There is no failure scenario: the 65C02 always fetches RESET from `$FFFC` →
`$FA62` in ROM, and the routine clears then sets IOUDIS before any user code
can run, so the end state is identical. Without a failure scenario this is
not a finding.

## Ce que POM2 ne sait pas

**//c IOU at RESET.** Whether the IOU chip itself forces IOUDIS on when RESET
is asserted. Searched: the IIc Technical Reference (2nd ed.) chapters 2
(reset routine, soft switches) — it describes the firmware, not the IOU's
reset behaviour. Unobservable to software (see above). *Would settle it:* a
logic probe on a real //c's IOU while RESET is held low, or the IOU's
internal documentation (none known to be public).

**PAL picture aspect (`kPom2PalWindowAspect = 1.507`).** A derivation by the
same method as Sather's NTSC 1.225, with PAL raster inputs that carry no
citation. *Would settle it:* a ruler on a Péritel-era PAL monitor showing a
known-square test pattern (grade A); for the inputs, ITU-R BT.470 (or CCIR
Report 624) for the 52 µs active line and 575 active lines.

**Not checked, out of scope but noticed.** The same `CpuClock.h` block states
the PAL Apple's line still takes 912 dot periods ("the PAL crystal was chosen
so the line still takes 912 periods"). 912 × 15 625 Hz = 14.25 MHz, whereas
POM2's PAL CPU clock × 14 = 1 015 625 × 14 = 14.21875 MHz (910 periods per
line) and MAME's pixel clock is quoted as 14.2375 MHz. The three numbers do
not agree with each other; worth a look by whoever audits `CpuClock.h`.

## Bilan

- **Sites examined:** 30 grep hits → 3 hardware/format claims, 27 programming
  uses of "unchecked" or history (DÉCORATIVE, not graded).
- **Grades before → after:** E 1 → 0, F 2 → 1, B 0 → 2 (one with a C
  alongside). Two of the three declared unknowns now carry a primary source.
- **Verdicts:** CONFIRMÉE 2 (one also IMPRÉCISE on mechanism), INTROUVABLE 1.
- **Constats:** 0.
- **Comment edits proposed:** 5 (`Memory.cpp`, `AppleSingle.h`,
  `AppleSingle.cpp` ×2, `CpuClock.h`), comment text only.
- **Not verified:** the 32 KB //c and //c+ ROMs' reset paths (the
  `STA $C078` stores exist in both — 32 KB rev 0 at file offsets `$0465`,
  `$411A`, `$419E`, `$54AC`; //c+ at `$411A`, `$419E`, `$5497`, `$564B` —
  but only the 16 KB ROM's path from `$FA62` was traced); every branch of
  `$C41C` (the `BCS $C46C` at `$C44B` skips the store when the computed mouse
  mode is ≥ `$10`; not traced to its inputs); the manual was read through OCR
  of a scanned PDF (Apple II Documentation Project mirror), so quotations are
  OCR text checked by eye, not a text layer; the PAL broadcast standards.
