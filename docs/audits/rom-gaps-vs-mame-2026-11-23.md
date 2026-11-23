# The dumps MAME has and POM2 does not — 2026-11-23

The [2026-11-07 audit](rom-dumps-vs-mame-2026-11-07.md) asked whether every
file in `roms/` is the chip it claims to be. This one asks the reverse: which
dumps does MAME carry for the Apple II, its slot cards and its devices, that
no file in `roms/` contains?

**Method.** `mame -listxml` 0.288 for `apple2ee`, `apple2e`, `apple2p`,
`apple2c`, `apple2cp` and `apple2gs` gives 238 slot devices. 107 carry ROMs.
Each ROM's SHA-1 was compared with every aligned 256 B to 64 KB slice of every
file in `roms/`. Grade of a match: [D].

**Where to get them.** Every dump named below is in RetroBIOS at the commit
`RomFetch.h` already pins (`441d4129`). The Apple folder has `apple2e.zip`,
which holds 342-0133-A and 342-0132-C. The Arcade folders have `votrax.zip`
(`sc01a.bin`), `a2vidtrm.zip`, `a2parprn.zip`, `mockingboardd.zip`,
`a2ramfac.zip` and `a2tmstho.zip`. A new catalogue entry still has to go into
`pom2_firm.zip` too, which means a new archive.org item (CLAUDE.md, Version
string locations).

## 1. For hardware POM2 already emulates

| Dump | What it is | Used for, in POM2 | Priority |
|---|---|---|---|
| `sc01a.bin` (512 B) | The internal phoneme ROM of the **Votrax SC-01A**: the speech chip on the Sweet Micro *Mockingboard Sound/Speech I*. It sits on VIA 1 port B, and its A/R line drives CB1. | **POM2 has no SC-01 at all.** MAME's default //e Mockingboard has one (`a2mockingboard.cpp` @ mame0288, `VOTRAX_SC01A`), and so does AppleWin. A program that speaks through it waits for the A/R handshake on CB1. On POM2 it never comes: the speech is silent, and the program can hang. MAME's `votrax.cpp` (BSD-3) simulates the chip from this ROM. | **High**: the one gap in the Mockingboard family POM2 claims to cover. |
| Videoterm character sets, 9 × 2 KB: ASCII uppercase, APL, Epson block graphics, **French**, German, Katakana, Spanish, super/subscript, symbol (MAME marks the symbol set `BAD_DUMP`) | The card has two sockets, a primary and an alternate set. MAME lets the user fill each with any of 11 sets (`a2videoterm.cpp:279-305` @ mame0288). | `VidexVideotermCard::loadCharRoms(normal, alternate)` already takes any pair. Only the files and a choice in the settings are missing. | **Medium**: small and self-contained. The French set suits a French //e or ][+ user. |
| `341-0132-d.e12` (//e), `342-0132-c.e12` (//c), 2 KB each | The keyboard encoder ROM: the AY-5-3600 key matrix to ASCII, for every Shift/Ctrl/Caps combination. | POM2 maps host characters to ASCII by hand (`Keyboard.cpp`, `KeyChord.h`). The ROM is the [B]-grade table to check that mapping against, and the source for international keyboards. POM2 already has the French one, `342-0326-a.f12`. | Medium: a test oracle, not something the screen needs. |
| `prom.b4` (256 B) | The **Apple II Parallel Printer Interface Card** (1978), as opposed to the Parallel Interface Card `341-0057` POM2 has. | POM2's `printer` card is synthetic. This PROM would make it the real card. Note that **AppleWin's `Parallel.rom` is a different file** (SHA-1 `c75c7515…` against MAME's `c6788835…`): a second reading to settle before choosing one. | Medium-low. |
| `342-0133-a.chr` (4 KB) | The real **unenhanced //e character ROM**. | POM2's `apple2e_char_us_unenh.rom` / `apple2e_char_2k.rom` is **not this dump**. It matches `u13.9433c-0202.rcl-zh-32.bin`, the character ROM of the CEC / Zijin clones (MAME `cec2000`, `zijini`). It differs from 342-0133-A in 32 glyphs: codes `$60-$7F` of the first bank hold lowercase (as on the enhanced ROM) where Apple's chip holds the punctuation `` ` `` to `?`. **Nothing on screen changes today.** POM2 masks codes `$40-$7F` to `$00-$3F` outside ALTCHAR and moves `$60-$7F` to `$E0-$FF` with it (`Apple2Display.cpp`, `lookupCsbitsGlyph`), as MAME does, so it never reads those cells. [F] Whether the real //e reads them is the open question. Settled by: a photo of an unenhanced //e showing codes `$60-$7F` in both character sets. | Medium: the file should be the chip its name claims, and its download already exists (`apple2e.zip`, which the catalogue fetches for the unenhanced firmware). |
| Grappler+ `3.0.u9`, `3.2.u9` | The two other firmware revisions. | POM2 has 3.1 (identified, 2026-11-07 audit). | Low. |
| Mouse card PAL16R4 (JEDEC) | The card's address-decode PAL. | POM2 decodes the card in code. | None. |
| UltraTerm `ult_2a313.jed`, `ult_251c.jed` | The card's PAL. | Same. | None. |
| Integer BASIC ROM card `watson.d0`, `inspector.d8` | Third-party monitor ROMs fitted to that card. | POM2 has no ROM card. | Low. |

## 2. For hardware POM2 does not emulate

| Card | Dump(s) | Why it might matter |
|---|---|---|
| Sweet Micro **Mockingboard D** | `mockingboard d rom.bin` (2 KB; same SHA-1 as AppleWin's `Mockingboard-D.rom`) | The //c Mockingboard that plugs into the serial port. POM2 models the Mockingboard 4c instead. |
| Applied Engineering **RamFactor** | v1.0, 1.1, 1.3, 1.4 (8 KB each) | A common slot RAM card. POM2's Slinky covers the same ProDOS RAM disk; the AE firmware adds its own RAM-disk menus. |
| Applied Engineering **TimeMaster H.O.** | rev. 5 (4 KB) | A clock card; POM2 has the ThunderClock and the No-Slot Clock. |
| **Apple II SD Card**, **Booti** | `a2sd`, `a2booti` | Modern storage cards. |
| Apple **Serial Interface Card** (1978) | `341-0017`, `341-0048` | The SSC's predecessor. |
| Others (SCSI and SASI cards, Vulcan IDE, Corvus, 80-column clones, terminals, CD drives) | — | No sign that the software POM2's corpus covers needs them. |

## Order proposed

1. **Votrax SC-01A** on the Mockingboard: a new chip (MAME's simulation, ported as with the SSI263 and the MEA8000), with the ROM above.
2. **Videoterm character sets**: catalogue entries plus a setting for each socket.
3. **342-0133-A**: replace the clone ROM under the unenhanced names, keeping the old file accepted. Then settle the `$60-$7F` question.
4. The keyboard ROMs, as a test oracle for `Keyboard.cpp`.
5. The Apple parallel printer PROM, once the AppleWin/MAME difference is understood.
