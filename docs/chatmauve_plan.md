# Le Chat Mauve, at the silicon — what the cards do, what POM2 does, and the plan

*2026-09-01. Research pass over every primary source that could be found on
the public Internet (manuals, the Video-7 patent, the Eve's PLA dump, the
measurements AppleWin's contributors made on real cards), read against
POM2's `LeChatMauveCard` and the Chat Mauve render paths in
`Apple2Display`. The point of this document is precision: each rule below
carries where it comes from, and each gap says what would close it.*

> **Status (checked against the code 2026-11-28).** P0, P1, P2 shipped;
> P3 closed as bounded; P4 done 2026-10-20 (the RVB Graph from its
> manual); P6 first rung (the mode latch beam-races) plus
> the live switch/CPREG panel; P7 done. Arlequin's seven screens and
> Extasie's mixed-DHGR title page are pinned by `chatmauve_feline_screens`
> (2026-10-24). Open: P5, the rest of P6 (dot-level `$C0Bx`), the DIX / PoP
> goldens, and the questions of § 6. Changed in the code on 2026-11-28 from
> this verification: the latch's power-on per variant and no re-arm on
> Ctrl-Reset; the Eve takes the latch at HR 000 while ENHRCPREG is off; the
> Video-7's DIP-switch text colour and black-and-white inverse text. § 0 and § 2.2 describe the model as it
> was *before* the plan.
>
> **Verified against the sources 2026-11-28**, each re-read for this pass:
> the Video-7 patent, the Féline / Eve / RVB Graph manuals, the *Manuel
> Arlequin* as Extasie Reloaded quotes it, the Apple IIe and IIc Technical
> Reference Manuals, Purplesoft's own code, AppleWin (v1.29.15.0 `98b72658`
> and master `7fd7fd7c`) with fenarinarsa's issues and articles, and MAME
> `apple2video.cpp` @ e1479b7a. Corrected here: "80COL must have been
> written" (an AppleWin heuristic, withdrawn in 1.29.16.0 — never
> measured); the latch's power-on state (the patent's is 560 mono); the
> quadruplet rule (the Reloaded author's deduction, not the Arlequin
> manual's); the family (three cards and an adapter, slot 3 + aux, the
> signals each one sees); the TTL header (a 4-bit colour number, not
> RGBI); table IX-1's BW560 and CP280 rows; the PLA's true/complement
> order (not pinned). Every hardware claim now carries its grade
> (CLAUDE.md § Conventions). The living description is
> [`DEV.md` § Le Chat Mauve](../DEV.md#le-chat-mauve-lechatmauvecard) and
> the header of `src/LeChatMauveCard.h`.

## 0. Initial model, before the plan (2026-09-01)

At the initial assessment, POM2 modelled **one** Chat Mauve: a Féline-class 2-bit mode latch with
the four Video-7 DHGR interpretations (560 mono / 140 colour / mixed /
160 chunky), a Video-7-style fg/bg colour TEXT, two "Eve" toggles at
`$C0B8-$C0BB`, and the palette AppleWin captured off a Féline. That is a
good Féline / //c-adapter at the **byte** level, and a wrong Eve: the Eve
has **eight switches at the sixteen addresses `$C0B0-$C0BF`, a data register (CPREG)
that the card writes into auxiliary memory behind the CPU's back, ten
graphics modes selected by AN3 + three switches, colour text whose nibbles
are the other way round from Video-7's, and a colour decoder that is a
PLS100 PLA whose fuse map is public** — and none of that is modelled.
The plan below takes the Féline to dot-level exactness first (it is what
Extasie and DIX need), then builds the Eve from its manual and its PLA,
then the RVB Graph and the //c adapter's quirks, on a dot-clock video tap
that all of them share.

## 1. The family, and what each card can do

Three cards carried the name — the RVB Graph (][ / ][+), the Eve and the
Féline (//e) — plus the //c adapter ([E] fenarinarsa, *Le Chat Mauve sur
Apple II*, fenarinarsa.com/?p=1370: «trois cartes Le Chat Mauve et un
adaptateur»). The adapter was sold under an Apple part number — its label
carries the Le Chat Mauve cat, "//c RVB PERITEL", "A2M4020F", "Made in
France" ([A] photo of a unit, same post) — and is a Féline without its
64 K / 80-column half, which the //c has built in ([E] fenarinarsa, AppleWin
#764: «a Feline without the memory expansion»; no adapter manual found).
Sources: the manuals (§ 7), fenarinarsa's survey and his measurements on a
//c adapter and, through a friend, an Eve (AppleWin #764/#850), the Video-7
patent US 4,631,692.

| Mode | RVB Graph (II/II+) | **Eve** (//e, slot 3 + aux, 64 K) | **Féline** (//e, slot 3 + aux, 64 K) | **Adaptateur //c** |
|---|---|---|---|---|
| TEXT 40/80 mono, GR, HGR (LCM colour rules) | 40 only; its own C0-C3 / B0-B3 HGR rules (§ 3.6) | ✓ | ✓ | ✓ |
| HGR colour variants SPEC1 / SPEC2 / DASH / HRBW | — | ✓ (HR1-3) | HRBW only (AN3 off) | HRBW only (AN3 off) |
| HGR COL280A / COL280B (280×192, 4 colours) | — | ✓ | — | — |
| HGR CP280 (280×192, fg/bg per 7 dots, 16 colours) | — | ✓ | — | — |
| DHGR COL140 | — ([B] a ][ / ][+ card) | ✓ | ✓ | ✓ |
| DHGR BW560 | — | ✓ | ✓ | ✓ |
| DHGR **mixed** COL140 + BW560 | — | **—** | ✓ | ✓ |
| DHGR 160×192 chunky (Video-7 / Apple RGB only) | — | — | — | — |
| Double lo-res | — | ? ([F] not in the Eve manual) | ✓ ([B] Féline manual p. 39) | ✓ |
| TEXT colour (fg/bg per character, aux byte) | whole-screen register | ✓ (TXT16) | — | — |
| TEXT green monochrome | CT = 12 | ✓ (TXTGREEN) | — | — |
| HGR colours programmable (C0-C3 isolated, B0-B3 runs), dotted, 4-colour | ✓ | — | — | — |
| Registers | `$C0F0-$C0FF` + `$C700-$C73F` | `$C0B0-$C0BF` | none | none |
| Mode latch (AN3 clocks /80COL) | — (no 80COL) | ? ([A] second-hand yes; POM2 ignores it — § 6) | ✓ | ✓ |

Two hardware facts that matter for a model:

- **The Eve and the Féline plug into slot 3 and the auxiliary slot at
  once** ([B] Féline manual p. 8 and the Eve manual's installation chapter:
  «se brancher à la fois sur le connecteur 3 et le connecteur auxiliaire» —
  the European //e's in-line pair), which is why the Eve's registers are
  slot 3's `$C0B0-$C0BF`. From the aux slot they see the motherboard's
  `80VID'`, `AN3`, `GR`, `SEGB`, `VID7M`, the video data bus `VID0-VID7`,
  `SEROUT`, `14M`, `LDPS'` ([B] Apple IIe Technical Reference Manual, Table
  7-21, pp. 198-200 — there is no TEXT line: text is GR low).
- **The //c adapter hangs off the DB-15**: TEXT, GR, SEGB, 14M, LDPS\*,
  SEROUT\* (the dots), VIDD7 (the latched byte's bit 7 — not VID7M), SYNC\*,
  WNDW\*, PRAS\*, CREF — and **no AN3, no 80COL** ([B] Apple IIc Technical
  Reference Manual, Table 11-17, p. 272; DHGR shows as TEXT ≠ ¬GR). It has
  to *infer* 80COL from the LDPS\* cadence. That this inference causes its
  erratic mode switches is fenarinarsa's hypothesis, one unit tested ([F]
  AppleWin #850: «my guess is that it's a behavior of the IIc adapters only
  … it may even be a defect of *my* adapter»); the symptoms are measured
  ([A] the same: Prince of Persia's title screen dropping to mono after the
  first attract loop, traced to the `$C057` access after `STA ADCOLon`;
  non-reproducible results over 14 random switch sequences). An Eve on an
  unenhanced //e shows PoP correctly ([A] second-hand, a friend's Eve,
  #850). Settled by: a second adapter, or its schematic.
- **The Eve rev A applies the wrong DHGR colours to the 4-bit patterns**
  (it shipped before Apple's DHGR); rev B fixed it — the manual says the
  board *and its software* changed («modifier très légèrement la carte ève
  et ses logiciels»). Its addendum (p. 127, § 8) gives a BASIC program for
  showing a rev A picture on a rev B: `DATA 0,8,1,9,2,10,3,11,4,12,5,13,6,
  14,7,15` poked into a software colour table (`POKE −26368+16*I`), then
  `& BACK=0` / `& CLEAR` [B]. Read as "code *i* → table[*i*]" that is the
  4-bit code rotated by one bit — but that reading rests on what Purplesoft's
  `& CLEAR` does with the table, not disassembled [F]. (AppleWin's DHGR path
  @ 7fd7fd7c rotates LEFT in code while its comment says right [D].)
  Settled by: disassembling `& CLEAR`, or a rev A owner.

## 2. What POM2 models today, and where it departs

*Status 2026-09-01 evening: P0, P1 and P2 landed (see § 5 for what each
delivered and § 2.1 for the table as it stands now). The table below is the
one the plan was written against — kept as the record of where the model
started.*

### 2.1 After P0-P2 (2026-09-01)

`LeChatMauveCard` carries a **variant** — Féline, Adaptateur //c, Eve,
Video-7, and the RVB Graph (§ 3.6, P4 — complete since 2026-10-20) — and answers three questions for the renderers: `dhgrMode()`,
`hgrMode(an3On)`, `textMode(eightyCol, an3On)`. The card owns the latch, the
Eve's switch byte and CPREG; the pixel rules live in `Apple2Display`.

| Area | POM2 now | Pinned by |
|---|---|---|
| Mode latch | patent shift register, 80COL sampled on every `$C05E→$C05F` edge (MAME numbering); power-on per variant — Video-7 **560 mono** ([B] patent), Féline / //c adapter COL140 ([B] Féline manual), Eve and RVB Graph COL140 ([F]); **Ctrl-Reset leaves it alone** ([B] no reset input; [A] nicolho, #633), the ROM's reset `$C05F` clocking it once with 80COL off when AN3 was low (since 2026-11-28; until then POM2 re-armed COL140 as [D] AppleWin does); Féline / //c fold 160 → COL140, the Video-7 keeps all four; **the Eve takes it at HR 000 while ENHRCPREG is off** (§ 6) | `le_chat_mauve_smoke` §§ 1, 16, `chatmauve_dot_rules` § 4, `chatmauve_latch_split` |
| DHGR mixed | per-byte 560/140 mux over a free-running 4-dot cell latch — colour cell **cut** into a BW byte, last BW dot **repeated** into a colour byte ([A] //c adapter, fenarinarsa, PR #837); == [D] AppleWin `UpdateDHiResCellRGB` @ 98b72658 over 3×32×192 random rows | `chatmauve_dot_rules` § 2, golden `cm/feline/dhgr-mixed-boundary`, `chatmauve_feline_screens` (Extasie's title) |
| HGR with the card | LCM rule (2-bit cell, 3-bit window, bank = the dot's own byte) == [D] AppleWin `UpdateHiResRGBCell` @ 7fd7fd7c over 64×192 rows; the latch does NOT touch single HGR ([D] AppleWin never consults it there); AN3 off → Féline / //c **mono** ([B] Féline manual p. 38), Video-7 **F/B** ([B] AAL 1986-08), Eve keeps decoding ([F]) | `chatmauve_dot_rules` § 1, smoke § 3 / § 11 |
| Eve switches | the eight switches at the sixteen addresses `$C0B0-$C0BF`; a write sets one and loads CPREG ([B] manual p. 117); POM2 also decodes reads ([F]); all off at power-on ([B] p. 117); LOCKRES vs Ctrl-Reset ([B] p. 119); slot-3 collision guard over the whole window | smoke § 6 |
| CPREG auto-write | `Memory::setAuxShadow` — text page under TXT16, HGR page under ENHRCPREG, frozen by LOCKCPREG, only when the CPU's write landed in MAIN; zero cost on the hot path (writable[] diversion, like the write watches, and pinned to coexist with them) | smoke § 7 |
| Table IX-1 | `& GR 1..10` decoded from Purplesoft's own switch tables: 000 HRAPPLE / 100 SPEC1 / 110 SPEC2 / 001 DASH / 011 HRBW with AN3 on; 000 COL140 / 100 COL280A / 010 COL280B / 110 blank / 111 CP280 / **011 BW560** with AN3 off. The patent latch plays no part on the Eve in POM2 ([C] `& GR 6` leaves it at 00 in POM2 and expects COL140 — contested, § 6). **CP280 needs no 80COL** ([B] p. 118: Note 2 only; [C] PURPLESOFT* `$E317-$E32A` forces 80COL on for `& GR` 6/7/8/10 and leaves it at the `& TEXT` setting for 9 — off after `& TEXT 1`; the Eve is the aux memory and has the attribute byte regardless) | smoke § 10, goldens `cm/eve/*`, `purplesoft_eve_probe` |
| TXT16 / TXTGREEN | colour text with **hi = background**, 80COL off, no AN3 condition; TXTGREEN as a white → green pass over the text rows (40 and 80 col, mixed-mode band too) | smoke § 8-9, goldens `cm/eve/text*` |
| COL280A/B | the 560 stream in **2-dot cells** (code = dot + 2·next), palettes in the manual's order — read off Purplesoft's `& PLOT` bytes (§ 6) | smoke § 10, goldens `cm/eve/dhgr-hr*-col280*` |
| SPEC1 / SPEC2 | 5-dot window: `11011` → black (SPEC1), plus `00100` → white (SPEC2); alternating runs stay coloured | goldens `cm/eve/hgr-spec*` ([F] rule from the manual's prose; the PLA cannot settle it — P3 closed; a schematic, a board trace or an owner's capture) |
| DASH | rendered as HRAPPLE — P3 | — |
| Variant selection | one catalog key; `chatmauve_variant` setting (`feline` \| `iic` \| `eve` \| `video7` \| `rvb` — `LeChatMauveCard.cpp:28-34`). Chosen in **Slot Configuration** (a "model" combo under the card's row, staged + applied with the slots) or live in the Chat Mauve panel; //c-class connectors are hardware-fixed to the Adaptateur IIc at plug time | — |
| Snapshot | blob v3 = latch + switch byte + CPREG; v2's two toggles map onto TXT16 / TXTGREEN. **v5** carries the RVB Graph's control byte, CT and eight colour registers (a v4 blob's old mode strobe restores power-on registers) | smoke § 12, § 13 |

The mode latch now beam-races (P6 first rung); the RVB Graph is complete
from its manual (P4); P3 is closed as bounded, rather than waiting
for an implementation inferred from insufficient PLA evidence.
Purplesoft's screens are frozen by `purplesoft_eve_screens`.
Still open: dot-level `$C0Bx`, P5, the DIX / PoP
goldens, and the questions of § 6.

### 2.2 As found, 2026-09-01 morning

`LeChatMauveCard` (161 + 107 lines) + `Apple2Display::renderDhgr /
renderHiResChatMauve80 / renderHgrDuochrome / renderTextChatMauveFgBg`.

| Area | POM2 today | Hardware | Verdict |
|---|---|---|---|
| Mode latch | 2-bit FIFO, shifts 80COL on the `$C05E→$C05F` edge, resets to `11` (COL140), `an3Prev` starts high | Patent: 2-bit shift register, clock AN3, data **/80COL**, power-on **set** (F1=F2=1). No precondition on MIXED/HIRES (measured). | Correct up to the data polarity convention (POM2 clocks 80COL and numbers its enum accordingly; AppleWin clocks /80COL). Keep, document. *2026-11-28: not quite — the patent's F1=F2=1 is **560 mono**, not COL140 (§ 3.1).* |
| DHGR interpretations | MAME `dhgr_update` port: COL140, Mixed (per **source byte** MSB), Chunky160, BW560 | Féline/IIc: no 160 mode — falls back to COL140. Mixed: 4-dot grid, byte-boundary rules (§ 3.3). | 160 must fall back on Féline; mixed boundary rules are the byte-level approximation, not the measured dot rules. |
| HGR with the card | 280-wide LCM path (`renderHiResChatMauve80`) — check against § 3.4 | 2-bit cell colour + 3-bit window: only `010` / `101` are coloured | Verify pixel-for-pixel; AN3 off → HGR **mono** on Féline/IIc (`POKE -16290,0`). |
| TEXT colour | Video-7 F/B: aux byte **hi = fg, lo = bg**, enabled when AN3 **off** + 80COL off; "Eve" toggle `$C0B8/9` | Eve TXT16: aux byte **hi = background, lo = foreground** (manual IV-2.2, III-2: `POKE -16199,16*F+C`), needs 80COL **off**, `$C0B9` on / `$C0B8` off, and CPREG auto-writes the aux byte | Nibble order is inverted for the Eve; CPREG mechanism absent. |
| `$C0BA/$C0BB` | "HGR Duochrome" toggle | **TXTGREEN**: green monochrome text 40/80 (`POKE -16197,0` on, `-16198` off) | Mislabelled; the fg/bg HGR the code renders is the Eve's **CP280** (AN3 off, HR1+HR2+HR3 on, 80COL on), selected elsewhere. |
| `$C0B0-$C0B7`, `$C0BC-$C0BF` | not decoded | ENHRCPREG, HR1, HR2, HR3, LOCKCPREG, LOCKRES; every write also loads **CPREG** | Missing entirely. |
| Reset | `onReset` re-arms the latch | Eve: Ctrl-Reset clears all sixteen switches unless LOCKRES; latch set at power-on | Add. |
| Palette | AppleWin `PaletteRGB_Feline` (two distinct greys) | Féline capture, white-balanced; Eve rev B "same as NTSC but grey1/grey2 differ"; rev A wrong DHGR colours | Fine for Féline; Eve untested; TTL vs analog output not modelled. |
| Beam racing | per-frame mode | the card is combinational on the dot stream; switches take effect at the dot | Mid-line switches (DIX) need the dot-level tap (§ 4). |

## 3. The hardware, rule by rule

### 3.1 The mode latch (all cards with DHGR)

[B] US 4,631,692, FIG. 1: *"a two bit shift register which uses AN3 as its
clock and 80COL as its data input"*, and the next sentence: *"only the
inverse 80COL … is available … the input polarity … is inverted"* —
`~80COL` is the only polarity the hardware sees.

**Power-on.** [B] The patent: *"set upon power-on such that F1 and F2 get
initialized to their 'on' state"* — *"This initialization procedure
powers-on the hardware in the 560×192 mode. Optionally, the power-on state
could very well have been any of the other three."* The Video-7 comes up in
**560 mono**. [B] Féline manual (ch. 5, the DHGR page): «Les programmes qui
ne prévoient pas ces commutations seront toujours en couleur» — the Féline
comes up in colour. [D] AppleWin and MAME (`m_rgbmode = 3`) both default to
colour, and AppleWin re-arms it on Ctrl-Reset (`CtrlReset` →
`VideoResetState` → `RGB_ResetState`, Utilities.cpp @ 7fd7fd7c). [A]
nicolho, //c adapter (AppleWin #633, 2019-04-28): Renegade's title «in
color after a cold start … after one reboot or more, it always shows up in
B&W» — the adapter's latch survives a reboot. POM2 re-arms COL140 at power-
on and on every Ctrl-Reset, on every variant [F] — § 6.

**What clocks it.** [A] fenarinarsa, //c adapter (AppleWin #850,
2020-10-27): `$C05E → $C05F` *mostly* shifts the current `~80COL` into F1
and F1 into F2 — some runs of the same 14 random switch sequences differed
(«it may even be a defect of my adapter»). [A] The same (#764,
2020-02-25): MIXED on still clocks. [A] jvernet, //e + a Le Chat Mauve
card (#631): Apple II DeskTop clocks with TEXT on and gets BW560. HIRES off
and 80STORE off were not tested. The «80STORE must be set» (Video-7
manual) and «TEXT off, MIXED off, HIRES on» (AppleColor manual)
preconditions are [D] AppleWin's report of those manuals, read as
programming advice.

**"80COL must have been written since the last edge" was never measured.**
[D] It was AppleWin's heuristic for Renegade (tomcw, commit 2463aae545,
2019-04-06, GH#633), restated in PR #837 (2020-09-24) with only the PoP
intro as evidence, and withdrawn after fenarinarsa's random-pattern test
(#850; commit fb0871672c, AppleWin 1.29.16.0 — still in the v1.29.15.0 tag
`98b72658`, gone from master `7fd7fd7c`). AppleWin @ 7fd7fd7c, MAME @
e1479b7a and POM2 all clock on every edge. What the //c adapter really
samples, having no 80COL line, is P5's question [F].

Resulting modes, F2 F1 as numbered by the patent's table (= AppleWin's
`g_rgbMode`); POM2 and MAME store the complement:

| clocked (80COL then 80COL) | F2 F1 | Video-7 / Apple RGB | Féline / IIc | Eve ([A] second-hand, #764; POM2 ignores the latch — § 6) |
|---|---|---|---|---|
| `$C00C`, `$C00C` | 1 1 | 560 mono | 560 mono | BW560 |
| `$C00D`, `$C00D` | 0 0 | 140 colour | 140 colour | COL140 |
| `$C00C`, `$C00D` | 1 0 | **mixed** | **mixed** | (unsupported → 140 colour) |
| `$C00D`, `$C00C` | 0 1 | 160 chunky | (unsupported → 140 colour) | (→ 140 colour) |

Reference sequences ([C] Arlequin / Extasie, `Extasie Reloaded` annexe 6):

```
STA $C050 ; STA $C054 ; STA $C052 ; STA $C057 ; STA $C05D
STA $C00C ; STA $C05E ; STA $C05F        ; bit 1 (80COL off)
STA $C00D ; STA $C05E ; STA $C05F        ; bit 2 (80COL on)  → mixed
STA $C05E                                ; DHGR on
```

On the //c the `$C05E/$C05F` pair is reached through IOUDIS (`$C07E`).
[A] fenarinarsa (#850) traced PoP's B&W to the `$C057` access right after
`STA ADCOLon`; swapping the two fixed it on his adapter. Neither the patent
nor the withdrawn "80COL written" rule explains that; "the adapter's
inferred 80COL" is his guess [F], and "not of the Féline" is [F] too —
only an Eve was tested against it.

### 3.1.1 The Video-7 SL7 (and Apple's AppleColor card)

POM2's `video7` variant (power-on: 560 mono, [B] the patent). [B] Video-7 SL7 manual, as quoted by fenarinarsa
in AppleWin PR #837: «Inverse text always show in black and white». [D]
AppleWin `UpdateText40/80ColorCell` @ 7fd7fd7c: characters below `$80`
(inverse **and** flashing) are drawn white on black in 40 columns when F/B
is off, and always in 80 columns. [D] `RGB_SetVideocard` @ 7fd7fd7c: the
default text colour comes from DIP switches — blue, amber, green or white =
lo-res 6 / 9 / 12 / 15 on black. [D] F/B text = AN3 off + TEXT + 80COL off,
aux high nibble = foreground (AppleWin NTSC.cpp; MAME `text_update` @
e1479b7a). [B] Bob S-C, *Apple Assembly Line* 1986-08: «F/B mode is active
any time the 80 column switch is off and annunciator 3 is off». [C]
Apple's AppleColor demo disk shows F/B HGR (#851); [D] AppleWin has treated
the AppleColor card as an alias of the SL7 since 1e43b331f3. POM2 models
F/B text and F/B HGR, and since 2026-11-28 the DIP-switch text colour
(setting `chatmauve_video7_text`: white / green / amber / blue, the Chat
Mauve panel's combo) with inverse and flashing characters kept black and
white, in 40 and 80 columns outside F/B (`le_chat_mauve_smoke` § 15). Compatibility cases worth a golden: King's Quest 1 at
`$5FD7` (press TAB twice) re-arms mixed mode with only TEXT and HIRES set;
Apple II DeskTop's black-and-white DHGR with HIRES off (GH#631); DIGIDREAM
falling into F/B HGR by accident on an SL7 (AppleWin help,
`Troubleshooting.html`).

### 3.2 The Féline's HGR (also the //c adapter; presumably the Eve's HRAPPLE)

[A] fenarinarsa, //c adapter, «all possible combinations» (AppleWin #764,
2020-02-24); [A] Féline, «checked against real hardware» (PR #837); [D]
AppleWin `UpdateHiResRGBCell` @ 7fd7fd7c (= v1.29.15.0); GrafX2
`Update_color_hgr_pixel` (not re-read 2026-11-28). The Eve's HRAPPLE is
presumed the same [F] (fenarinarsa: «I can only guess it's the same for
EVE»).

1. Slice the 280 dots of a line into 140 **cells of 2 bits**. `01` → colour
   1, `10` → colour 2 (`00`/`11` no colour). The byte's bit 7 picks the
   pair: 0 → violet / green, 1 → blue / orange (AppleWin: `offset ?
   palette[1+c] : palette[6-c]`). Which byte's bit 7: AppleWin takes the
   byte holding the **dot**, so a cell straddling two bytes can show two
   palettes [D] — an implementation detail, not a measurement.
2. For each dot look at it with its two neighbours (3-bit window; the dot
   before the first and after the last are 0). **`010` or `101` → the middle
   dot takes its cell's colour. Anything else → white if 1, black if 0.**
   No half-dot shift, no fringing, no NTSC.

AN3 **off** in HGR makes the Féline/IIc render HGR in **monochrome**: [B]
Féline manual p. 38, `POKE -16290,0`, «si votre écran texte est en 40
colonnes, sinon cette instruction aura des effets surprenants» (in 80
columns `$C05E` arms DHGR); `POKE -16289,0` or Ctrl-Reset restores colour;
[A] the //c adapter does the same (#764). On the Apple RGB card that state
is the F/B (duochrome) mode (§ 3.1.1); on the Eve it is not [F]. [D]
AppleWin never consults the latch in single HGR; one ambiguous [A] hint
(jvernet, #631: MousePaint launched after A2D's BW560 shows B&W HGR — either
the latch reaching HGR, or AN3 left off).

### 3.3 The Féline's mixed DHGR — the mode Extasie was written for

[B] *Manuel Arlequin*, as quoted by *Extasie Chat Mauve Reloaded*
(boutillon.free.fr): *"Mode mixte : uniquement Féline IIe et //c avec
interface Péritel/RVB. La carte Eve n'est pas compatible avec ce mode. Le
bit 7 de chaque octet est utilisé dans l'affichage mixé. Si ce bit est à 0,
l'octet est destiné à être affiché en monochrome, sinon l'octet est destiné
à être affiché en couleur"*; a line is «20 cellules de 4 octets» (28 dots,
7 colour points per cell). [C] The Reloaded page's author, from Extasie's
behaviour («quelques tests avec Extasie») — **not the manual**: «le bit 7
de l'octet dans lequel se trouve le bit 0 du quadruplet détermine la
couleur de celui-ci», a 4-bit colour cell taking the bit 7 of the byte that
holds its **first** bit, on a grid fixed to the line, 7 cells per 4 bytes:

```
Aux      Princ    Aux      Princ
76543210 76543210 76543210 76543210     bits
X2103210 X1032103 X0321032 X3210321     which quadruplet each bit feeds
```

and its own boundary table (`1CCCCCCC 0NNNNNNC`) contradicts the measured
rule below. The manual itself advises keeping «les 4 bits 7 des octets
d'une cellule dans un même état», and says why: «des bavures peuvent
apparaître et … certaines routines du système Arlequin … risquent de ne pas
fonctionner» [B]. [A] fenarinarsa, //c adapter (PR #837, on Extasie's
*Elephant*), as [D] AppleWin implements it (`UpdateDHiResCellRGB` @
98b72658):

- bit 7 of each byte defines the mode of the following 7 bits; BW pixels
  are 1 dot, colour pixels 4 dots;
- a colour cell that crosses into a BW byte is **cut** there (less than 4
  dots wide);
- a BW cell that crosses into a colour byte has its **last BW dot
  repeated** until the next cell boundary.

fenarinarsa's first note (2020-02) said the switch only takes effect "from
the next 4-bit sequence"; the later PR refined that into the two rules
above. [A] beats [C] on the //c adapter; the Féline is untested [F]. The
patent's per-byte mux [B] — the byte's VID7 selects the 560 path or the 140
path *for the next seven 14 MHz periods* while the 140 path's 4-bit latch
keeps its own grid — explains the **cut**; its FIG. 2 text does not produce
the **repeat** [F]. [D] MAME @ e1479b7a shows the straddling cell's colour
instead, quoting the Video-7 User's Manual: «color aberrations will occur
at the leading and trailing edges» [B via D]. POM2 models mux per byte +
latch per 4 dots and pins the cases with the AppleWin oracle and the golden
`cm/feline/dhgr-mixed-boundary`; Extasie's mixed-DHGR title page is pinned
by `chatmauve_feline_screens`. Dragon Wars encodes bit 7 the other way
round — and on real //c adapters its title shows in **B&W** ([A] nicolho,
#633; [A] fenarinarsa, #764): POM2's `invertBit7` is AppleWin's convenience
(`-rgb-card-invert-bit7`) [D], not the hardware's behaviour.

### 3.4 The Eve: switches, CPREG, and table IX-1

[B] Manual, chapter IX (section IX-1 from p. 117; table IX-1 p. 118; TXT16 to
LOCKRES p. 119). Eight switches at the sixteen slot-3 addresses
`$C0B0-$C0BF` (−16208 … −16193), **all off at power-on**. «Il n'est pas
possible de relire l'état de ces commutateurs … Il faudra écrire à ces
adresses car, à chaque fois la donnée sera mémorisée dans … CPREG» (p. 117):
the switches are set by writes, and **every write latches the data byte
into CPREG** (dot colour in the low nibble, background in the high nibble).
POM2 also decodes reads [F] — settled by a `LDA $C0B9` on a real Eve.
Whether LOCKCPREG also stops CPREG from latching («bloque totalement le
fonctionnement») is [F].

| off | on | Switch | Effect |
|---|---|---|---|
| `$C0B0` | `$C0B1` | **ENHRCPREG** | off: CPREG stops acting on `$2000-$3FFF`; on: CPREG also mirrors HGR writes. *Must be off while AN3 is on.* |
| `$C0B2` | `$C0B3` | **HR1** | see table |
| `$C0B4` | `$C0B5` | **HR2** | see table |
| `$C0B6` | `$C0B7` | **HR3** | see table |
| `$C0B8` | `$C0B9` | **TXT16** | on + 80COL off: 40-col text is colour text, each char's aux byte = its colours (**hi nibble = background, lo = foreground**). With LOCKCPREG off, CPREG is written to aux on every character sent to the text page. |
| `$C0BA` | `$C0BB` | **TXTGREEN** | white → green monochrome text, 40 and 80 col. |
| `$C0BC` | `$C0BD` | **LOCKCPREG** | on: CPREG frozen (no auto-write, text or CP280). |
| `$C0BE` | `$C0BF` | **LOCKRES** | off: Ctrl-Reset clears the eight switches; on: the card survives Ctrl-Reset. |

**CPREG's auto-write is the LLE-relevant mechanism**: when it is "en
fonction", a CPU write to main `$0400-$07FF` (text page) or `$2000-$3FFF`
(HGR page, only with ENHRCPREG) makes the card write CPREG's value into
**auxiliary** memory at the same address. That is how `PRINT` in colour
text and HPLOT in CP280 get their colours without the program touching aux
— and it is a `Memory` write hook, not a video rule. Purplesoft's
`&COLOR=` / `&BACK=` and the Pascal unit drive it; the manual's BASIC-only
recipe is `POKE -16199,16*F+C` (TXT16 on with CPREG = F<<4 | C) then
`HOME`. [C] Note that every Purplesoft `& GR` ends with `STA $C0BD`
(LOCKCPREG on, `$E04D`), so the HPLOT-in-CP280 path through CPREG is
unverified past that point.

**Table IX-1 — choosing a graphics mode** ([B] the scanned page 118; the
manual prints "COL280A" on both 4-colour rows, the second is COL280B by
chapter VI). **Cross-checked 2026-09-01 against Purplesoft's own code** [C]
(the five tables re-confirmed byte for byte 2026-11-28 at runtime
`$E06F-$E0A0`, file offset `0x1F973` of
`disks_5.4/chatmauve/purplesoft-revb-oct83-demos.dsk`): the
`& GR n` handler in `PURPLESOFT*` (rev B, octobre 83) selects each mode
through five ten-entry tables at runtime `$E06F / $E079 / $E083 / $E08D /
$E097` — the low byte of a `$C0xx` address per mode, for AN3, ENHRCPREG,
HR1, HR2, HR3:

```
mode        1  2  3  4  5  6  7  8  9  10
AN3        5F 5F 5F 5F 5F 5E 5E 5E 5E 5E     on for 1-5, off for 6-10
ENHRCPREG  B0 B0 B0 B0 B0 B1 B1 B1 B1 B1     on for the AN3-off modes
HR1        B2 B3 B3 B2 B2 B2 B3 B2 B3 B2
HR2        B4 B4 B5 B4 B5 B4 B4 B5 B5 B5
HR3        B6 B6 B6 B7 B7 B6 B6 B6 B7 B7
```

so 1 HRAPPLEII = 000, 2 HRSPEC1 = 100, 3 **HRSPEC2 = 110**, 4 HRDASH = 001,
5 HRBW = 011, 6 COL140 = 000, 7 COL280A = 100, 8 COL280B = 010, 9 CP280 =
111, 10 **BW560 = 011** — HR2+HR3 is "black and white" in both AN3 states.
[B] The scan prints the same (`off off on on`, p. 118): the "HR3 alone"
of the first reading was our misread of it, not the scan's. [C] The
`& TEXT n` code at `$E1F6-$E215` (`LDX $E222,Y`→`$C0B8,X` TXT16;
`LDX $E228,Y`→`$C000,X` TXTGREEN; `LDX $E22E,Y`→`$C00C,X` 80COL; Y = n
counted from 1): TXT16 on for 3 and 4, TXTGREEN for 6, 80COL on for 2, 4,
5 and 6 — TXT16 and TXTGREEN as read.

| AN3 | HR1 | HR2 | HR3 | Mode | Note |
|---|---|---|---|---|---|
| on | off | off | off | HRAPPLEII — standard HGR | |
| on | on | · | · | HRSPEC1 — [B] «HR std + pts noirs/fond blanc» | the name is IV-3.1's |
| on | · | on | · | HRSPEC2 — [B] «HR std + pts blancs/fond noir» | [C] Purplesoft's HRSPEC2 sets HR1+HR2 (110), `$E083` |
| on | · | · | on | HRDASH — coloured horizontal lines drawn dotted | |
| on | off | on | on | HRBW — HGR in black and white | |
| off | off | off | off | **COL140** — 140/line, 16 colours | needs 80COL on |
| off | on | off | off | **COL280A** — 280/line, 4 colours (black, orange, green, white) | needs 80COL on |
| off | off | on | off | **COL280B** — 280/line, 4 colours (black, light blue, pink, yellow) | needs 80COL on |
| off | on | on | off | screen blanked to black | CPREG keeps working (LOCKCPREG off, ENHRCPREG on); [C] Purplesoft selects it at `$E009-$E011`, then clears `$2000-$3FFF` with CPREG = 0 at `$E016-$E03A` — the card clears aux |
| off | off | on | on | **BW560** — 560/line, black and white | needs 80COL on |
| off | on | on | on | **CP280** — 280/line, fg/bg per 7 dots, 16 colours | Note 2 only [B] p. 118 — no 80COL requirement; CPREG active iff LOCKCPREG off and ENHRCPREG on |
| off | · | · | — | 001 (HR3 alone), 101 (HR1+HR3) | not in table IX-1 [B], unused by Purplesoft [C]; POM2: BW560, COL140 [F] |

Mode semantics (manual IV-2, IV-3, VI-3):

- **HRSPEC1/2, HRDASH, HRBW** are decoder variants of the LCM HGR rule of
  § 3.2: SPEC1 turns an unlit dot between lit ones (the `101` window,
  coloured by the LCM rule) black — POM2 requires the wider `11011` [F];
  SPEC2 also turns `00100` white [F]; DASH renders the horizontal runs of
  the four HGR colours as dotted lines [B] IV-3.1 (POM2 renders HRAPPLE);
  HRBW is 280×192 black and white [B] IV-3.1.
- **CP280** ("coloriage foreground-background par série de sept points"):
  main byte = 7 dots (LSB leftmost), aux byte at the same address = colours,
  **high nibble background, low nibble foreground** — POM2's `CP280`
  since 2026-09-01 (the old "HGR Duochrome" label and its swapped nibbles
  are gone).
- **COL280A/B**: 280 dots, 2 bits per dot ("point par point"), 16 K, two
  fixed palettes — the 560-dot stream in 2-dot cells, aux byte then main
  byte ([C] Purplesoft's `& PLOT` bytes, § 6).
- **COL140 / BW560**: as on the Féline. No mixed mode ([B] Manuel
  Arlequin); double lo-res is not in the Eve manual [F].
- Colour numbering is Apple's lo-res order (0 black, 1 magenta, 2 dark
  blue, 3 purple, 4 dark green, 5 grey 1, 6 medium blue, 7 light blue,
  8 brown, 9 orange, 10 grey 2, 11 pink, 12 green, 13 yellow, 14 aqua,
  15 white).
- Text: TXT16 as above; TXTGREEN; 80-col text is mono only.
- **Charsets** are software (Purplesoft `&CHRS n`, eight fonts drawn on
  the graphics page) — nothing in the card.

### 3.5 The Eve's colour decoder is a PLA, and its fuse map is public

`Chat_Mauve_eve_PLA.jed` (Apple II Documentation Project, ROM Images):
Signetics **PLS100 / 82S100**, 16 inputs, 8 outputs, 48 product terms,
1928 fuses. "0 = intact" is pinned: the other reading makes many terms
contradictory. Which fuse of each input pair is the TRUE literal is **not**
pinned: inverting every literal also gives 48 clean terms. [D] MAME
disagrees with itself — `jedutil.cpp` @ mame0287 reads the even fuse as
true (as POM2's decoder does), `pla.cpp:149-153` @ mame0287 as the
complement — and [B] DataIO's «UniSite 82S100 Dumping V1.00» (bitsavers)
documents the pairs swapped relative to jedutil: the order depends on the
programmer, and this `.jed` is a Max Loader dump of unknown convention
[F]. Output polarity fuses `[0,0,0,0,0,0,1,1]` (F6, F7 active-low; `L01920
00000011`). The families below survive either reading; the polarity
deductions (I0 = the dot, F1·F5 = green) do not. Which board revision the
dump comes from is [F]. The structure is legible without the schematic:

- **I4** splits nearly the whole map (T0 and T28 carry no I4 literal): `/I4` terms use I1-I3, I5-I11 as a pixel
  window and I12-I15 as mode bits; `I4` terms are short (`I0 & I4`,
  `/I0 & I4 & /I15`, `I4 & I9`) — I4 is almost certainly the **TEXT/GR
  side** of the decode (or AN3), the `/I4` side the graphics decoder.
- **I12, I13, I14, I15** behave as mode selects (`I12 & /I14`, `I12 &
  I14`, `/I12 & /I13`, `/I12 & I13`) — the HR1/HR2/HR3/AN3/80COL family.
- **F1, F2, F4, F5** are set in the combinations a 4-bit colour code would
  produce (`F1,F2,F4,F5` = white; `F2,F4`; `F1,F5`; …): the **R, G, B,
  I** outputs. F0/F3 and the inverted F6/F7 look like blanking / sync /
  path-select lines.

The plan's Eve decoder is that PLA evaluated per dot (a 64 K-entry truth
table, trivially fast), with the pin roles pinned by consistency against
table IX-1 and the demo disks. That is as LLE as an Eve can get without
the board schematic, which is not public.

### 3.5.1 The fuse map, decoded (2026-09-01 evening)

`docs/Chat_Mauve_eve_PLA.jed` is in the tree now, and `tools/decode_eve_pla.py`
regenerates this table. Layout: QF1928 = **48 rows of 40 fuses** (16 input
pairs — fuse 0 = intact = connected, even fuse = true, odd = complement —
then 8 OR fuses, 0 = term feeds the output), plus 8 polarity fuses
`[0,0,0,0,0,0,1,1]` (F6, F7 active-low). The 0-intact convention is pinned
by structure (48 clean terms, each driving a small output set); the
true/complement order is not (§ 3.5). The 48 terms below equal
`tools/decode_eve_pla.py`'s output (re-checked 2026-11-28; the repo `.jed`
is byte-identical to apple2.org.za's). T0 lacks the /I4 of its seven
siblings, so the "eight symmetric terms" are seven plus one.

```
T 0: I6·I8·/I9·/I12·/I13                       -> F2      ┐ eight symmetric terms:
T 1: /I4·I7·I8·/I9·/I12·/I13                   -> F4      │ {I2,I3,I6,I7} × I9 phase,
T 2: I3·/I4·I8·/I9·/I12·/I13                   -> F5      │ gated I8, mode /I12·/I13
T 3: I2·/I4·I8·/I9·/I12·/I13                   -> F1      │ — one output each of
T 4: I3·/I4·I8·I9·/I12·/I13                    -> F2      │ F1/F2/F4/F5. The shape
T 5: I2·/I4·I8·I9·/I12·/I13                    -> F4      │ of a 2-dot-cell decoder
T 6: /I4·I6·I8·I9·/I12·/I13                    -> F5      │ (COL280A/B, two palettes
T 7: /I4·I7·I8·I9·/I12·/I13                    -> F1      ┘ by I9 = cell phase?)
T 8: /I4·/I12·I13                              -> F3      ─ F3 = the mono/560 line?
T 9: /I4·/I8·/I12·/I13                         -> F3
T10: /I1·/I2·/I4·I6·I7·/I9·I12·/I14            -> F2,F4   ┐
T11: /I1·/I2·/I4·I6·I7·I9·I12·/I14             -> F1,F5   │
T12: /I2·/I4·/I6·I7·/I9·I12·/I14               -> F4,F5   │ HGR family (I12·/I14):
T13: /I2·/I4·/I6·I7·I9·I12·/I14                -> F1,F2   │ pixel windows over
T14: I2·/I4·I7·I12·/I14                        -> F1,F2,F4,F5   I1,I2,I5,I6,I7 with
T15: I1·/I4·I7·I12·/I14                        -> F1,F2,F4,F5   I9 = colour phase and
T16: I1·I2·/I4·I5·/I6·/I7·I9·/I11·I12·/I14     -> F2,F4   │ /I10, /I11 variants —
T17: I1·I2·/I4·I5·/I6·/I7·I9·/I10·I12·/I14     -> F2,F4   │ the widened window the
T18: I1·I2·/I4·I5·/I6·/I7·/I9·/I11·I12·/I14    -> F1,F5   │ SPEC modes need (two
T19: I1·I2·/I4·I5·/I6·/I7·/I9·/I10·I12·/I14    -> F1,F5   │ don't-care neighbours
T20: I2·/I4·I5·I6·/I7·I9·/I11·I12·/I14         -> F4,F5   │ per case)
T21: I2·/I4·I5·I6·/I7·I9·/I10·I12·/I14         -> F4,F5   │
T22: I2·/I4·I5·I6·/I7·/I9·/I11·I12·/I14        -> F1,F2   │
T23: I2·/I4·I5·I6·/I7·/I9·/I10·I12·/I14        -> F1,F2   │
T24: I1·I2·/I4·I5·/I6·/I7·I9·I12·/I14·/I15     -> F2,F4   │
T25: I1·I2·/I4·I5·/I6·/I7·/I9·I12·/I14·/I15    -> F1,F5   │
T26: I2·/I4·I5·I6·/I7·I9·I12·/I14·/I15         -> F4,F5   │
T27: I2·/I4·I5·I6·/I7·/I9·I12·/I14·/I15        -> F1,F2   ┘
T28: I0·/I1·/I2·I7·/I10·/I11·I12·/I14          -> F1,F2,F4,F5   (white case)
T29: I0·/I4·/I5·I12·/I14·/I15                  -> F0,F1,F2,F4,F5
T30: /I0·I1·/I4·I5·I12·I14·/I15                -> F4,F6   ┐
T31: /I0·/I4·I5·I6·I12·I14·/I15                -> F5,F6   │ I12·I14 family —
T32: /I0·/I4·I5·I7·I12·I14·/I15                -> F1,F6   │ /I15: four bits
T33: /I0·I3·/I4·I5·I12·I14·/I15                -> F2,F6   │ {I1,I6,I7,I3} each to
T34: /I0·/I4·I5·/I9·I12·I14·/I15               -> F3,F6   │ one of F4/F5/F1/F2 —
T35: /I0·I1·/I4·I5·/I6·I12·I14·I15             -> F1,F2,F6      the shape of a 4-bit
T36: /I0·/I1·/I4·I5·I6·I12·I14·I15             -> F1,F5,F6      COL140 code fanned to
T37: /I0·I1·/I4·I5·I6·I12·I14·I15              -> F1,F2,F4,F5,F6  RGBI; I15 flips to a
T38: I0·I1·/I4·I5·/I6·I12·I14·/I15             -> F2,F4,F5,F6   second interpretation
T39: I0·/I1·/I4·I5·I6·I12·I14·/I15             -> F1,F2,F4,F6   (CP280? fg/bg from
T40: I0·I1·/I4·I5·I6·I12·I14·/I15              -> F1,F2,F5,F6   nibble halves)
T41: /I4·I5·I12·I14                            -> F6      ─ F6 active-low, always on
T42: I0·/I4·/I5·I12·I14·/I15                   -> F0,F1,F2,F4,F5,F6   in this family
T43: I0·/I4·/I5·I12·I14·I15                    -> F0,F7   ┘
T44: I0·I4                                     -> F0,F7   ┐ TEXT side (I4):
T45: /I0·I4·/I15                               -> F0,F1,F2,F4,F5  I0 = the dot,
T46: /I0·I4·I15                                -> F0,F1,F5      I15 = a text mode bit
T47: I4·I9                                     -> F0,F3   ┘ (TXTGREEN? F1·F5 = green?)
```

First reading (to be pinned by simulation against known modes, the actual
P3 work): **F1/F2/F4/F5 are the colour outputs** (plausibly R/G plus two of
B/I — T28's "all four" = white, T46's F1·F5 pair = a green for `/I0·I4·I15`
≈ "text dot off, TEXT side, mode bit 15" fits TXTGREEN's green background…
or its inverse); **F0/F7** (F7 active-low) travel together on the
bright/text cases — intensity and/or a path select; **F3** stands alone on
the mono-ish rows (T8/T9/T34/T47); **F6** (active-low) is asserted by T30-T42
of the I12·I14 family (T43, in the same family, drives F0·F7 instead) and
by nothing else — a family-wide strobe
(latch-load or burst gate). Mode bits: **I4** (TEXT side), **I12/I13/I14/
I15** (graphics families: /I12·/I13 → the 2-dot-cell block, I12·/I14 → the
HGR block, I12·I14 → the 4-bit block), **I5/I8** gate within families;
window dots live among I0-I3, I6-I11 with **I9** as the cell phase. One
sobering observation before the fit is even attempted: T0-T7 pair two
dot-inputs per I9 phase with their outputs SWAPPED between phases (I2→F1,
I7→F4 at /I9; I2→F4, I7→F1 at I9 — same for I3/I6 on F5/F2), and a 2-dot
COL280 code that asserts one or two of those lines cannot be finished RGBI
(code 3 = white needs three colour lines). So the PLA's outputs look like
**intermediate code lines feeding the LS parts downstream** (which the
board photo shows in quantity: LS273/LS374 latches, LS399 selectors after
the N82S100N), with F6 as the family strobe — not the final RGB. That
caps what P3 can extract from the fuse map alone: pin the input roles by
simulating against the modes whose behaviour is measured (COL140 / BW560 /
COL280, the latter now known from Purplesoft's `& PLOT` bytes), use the
result to cross-check SPEC1/2/DASH hypotheses, and accept that the exact
output encoding needs the schematic or a board trace. The measured-
behaviour models stay the reference.

**The simulation ran (2026-09-02, by hand from the decoder's terms — no
script or output was kept in the tree) and bounded it.** Evaluating the whole
truth table (48 terms, convention as § 3.5.1) family by family:

- `/I12·/I13` (gated by I8): the window inputs route to the outputs as a
  **phase demultiplexer** — at I9=0, I2→F1, I7→F4, I3→F5, I6→F2; at I9=1
  the pairs SWAP (I2→F4, I7→F1, I6→F5, I3→F2). Two interleaved dot streams
  distributed onto four latch lines by the cell phase: the PLA is
  *assembling the 2-dot cells* (COL280's, as measured off `& PLOT`) for the
  LS273/LS374 latches downstream, not decoding colours. With the window
  empty and I8=0, F3 asserts alone — the mono/blank line.
- `I12·I14` (gated by I5, F6 strobed low across the family): the same
  four-way routing off {I1, I3, I6, I7} in the /I15·/I0 subfamily
  (I1→F4, I3→F2, I6→F5, I7→F1, F3 high), with I15 / I0 switching to the
  paired-output combinations of T35-T43 — a second assembly mode over the
  same latch lines (the 4-bit cell family: COL140 / CP280 side).
- `I12·/I14`: the HGR-shaped family — wider windows, the /I10 vs /I11
  duplicated pairs — same output lines, less separable in isolation
  (several terms overlap unless the real mode bits exclude each other).
- `I4` (TEXT side): near-constant fills keyed by I0 (the dot) with I9/I15
  trims — consistent with text fg/bg selection ahead of the palette.

So the PLS100's job is **dot-stream steering and cell assembly** — phase
demux, byte-boundary handling, path select (F0/F3/F7), a family strobe
(F6) — and the palette/colour semantics live in the downstream TTL and the
resistor DAC the board photo shows. P3-by-fuse-map is hereby **bounded and
closed**: the map *corroborates* the measured structure (2-dot cells for
COL280, 4-dot assembly for the 140 family, a distinct HGR window family)
and can arbitrate future structural questions, but SPEC1/SPEC2/DASH's exact
pixel rules and the COL280A/B palettes are downstream of it. They stay
modelled from the manual's prose and Purplesoft's bytes (`cm/eve/*`
goldens) until a schematic or board trace surfaces.

### 3.6 The RVB Graph (II / II+)

[B] Source: its *Manuel de Référence* (§ 7). The OCR is unreliable ("81" for
B1, "8" for B, "Cî" for CT); every fact below was read off the scan, page
numbers as printed — all twenty re-checked against the scan 2026-11-28.

**Hardware (ch. I, V).** Slot 7 only (SYNC pin 19, COLOR REF pin 35, 14 MHz
pin 24, TEXT pin 23). The user pulls the motherboard's 9334 / 74LS259
display soft-switch latch and plugs the card's 16-pin ribbon into its
socket; a wire brings composite video from the auxiliary video connector.
Outputs: Péritel on an 8-pin header (three trim pots for R, G, B, p. 7;
0-1 V) and an Apple /// style TTL header (XRGB1/2/4/8, p. 18). "Brevet
déposé". SYNC and COLOR REF reach slot 7 on Apple II rev 2+ boards; 14M and
TEXT only on European rev 7+ boards (p. 17).

**Control register (pp. 12-14):** `POKE −16144 + S + U + K, 16·(15 − CT)`.
−16144 = `$C0F0` (p. 16), so the low address nibble carries the options:

| | Values | Meaning |
|---|---|---|
| CT | data high nibble = 15 − CT | colour of every text line (the 16 lo-res colours, table p. 8) |
| S | 0-3 (bits 0-1) | 0 colour lines continuous; 1 dotted ("tout point représenté en mémoire par un 0 apparaît noir"); 2 continuous, but runs take C0-C3 by position — "la couleur B n'apparaît plus"; 3 = 2, dotted |
| U | 0 or 4 (bit 2) | 0 usual colours, and "usually white" HGR dots take CT; 4 the colour registers |
| K | 0 or 8 (bit 3) | 0 registers open, RESET = `POKE −16144,0`; 8 registers closed, RESET has no effect on the card |

**Colour registers (pp. 9, 15-16):** `POKE −14577 − Bn, 15 − Cn`, then
−14561, −14545, −14529 for n = 1..3 — `$C70F`, `$C71F`, `$C72F`, `$C73F`
minus Bn. Write-only, undefined at power-on, written while K = 0. n is the
dot's position: 0 even column bit 7 = 0 (usually violet), 1 odd/0 (green),
2 even/1 (blue), 3 odd/1 (orange). **Cn** colours an isolated dot; **Bn**
a run of 2+ lit dots whose leftmost dot is at position n. B0 = B1 = B2 = B3
is the five-colour system of ch. III; 8 colours + black otherwise. HCOLOR
1 → C1, 2 → C0, 5 → C3, 6 → C2 (p. 11). The usual colours are C = 3, 12,
6, 9 and B = 15 (p. 10, Exemple 1). Monochrome is a choice of colours:
everything 15 = a black-and-white monitor, everything 12 = a green one
(p. 11).

**The silicon: its patent, FR 2 509 560** *(found 2026-11-28)*. «Dispositif
de décodage couleur», filed 10 July 1981 by Philippe Chaillat and Didier
Chaligne (Paris 19e, as Le Chat Mauve), published 14 January 1983, granted
23 December 1983 (WO 83/00250, EP 0 084 029 withdrawn) —
https://patents.google.com/patent/FR2509560A1/fr. It is the card's whole
schematic: its parts list matches the board photos (Apple II Documentation
Project, *Interface Cards / Video / Le chat mauve RVB Graph / Photos*) chip
for chip — TTL only (74LS164 / 170 / 174 / 175 / 244 / 258A / 273 / 378 /
379 / 398 / 51 / 158), no PROM, no PAL, nothing to dump. Read twice,
independently (the second reading with a cycle model of Fig. 2); where the
two agree it is [B]:

- **It decodes the VIDEO stream**, not memory: VID resynchronised at 14.318
  MHz into a 74LS164 (taps B1 oldest … B8 newest); the hi-res decoder works
  on B1, B2, B4, B6, B8 resampled at 7 MHz (Fig. 1-2). The byte's bit 7 is
  recovered from the stream — a dot edge off the 7 MHz grid is the half-dot
  delay (ST = B1⊕B2, latched on edges only) — and the column parity is the
  COLOR REF phase (CL from H3). **No half-dot shift on the output**: the
  shift only chooses the colour.
- **Isolated vs run**: two lit adjacent dots (B4 and B6) are a run →
  register B; a lit dot with a dark right neighbour → register C; dark →
  black. **A run keeps its first dot's colour** (chip 6 freezes for the run)
  and the freeze lasts one sample past it, so the run's last dot shows B.
- **The continuous-line fill** (S1 = A0 off): a single 0 just before a lit
  dot repeats the LEFT neighbour's sample, phase included — the C colour at
  its position, even after a run (11011 → B B C B B). Two 0s or more stay
  black; the 0 after the last lit dot is never filled.
- **S2 = A1** ("four colours"): no run is ever detected, B is never read —
  every dot takes its own Cn (1111 → C0 C1 C0 C1).
- **Hard-wired colours** with U = 0 (74LS158, Fig. 4): isolated dots 3
  violet / 12 green / 6 blue / 9 orange by position, runs CT — the patent's
  «couleurs prédéfinies à l'allumage».
- **Lo-res**: one 4-bit sample per 14-dot column, aligned to the colour
  phase (74LS398 at H'4), so each block is one clean colour. **Text**: the
  14 MHz tap selects CT or black. The decoder follows TEXT and HR (the
  card's own 74LS259); mixed mode works only through the motherboard's
  mixed-resolved TEXT line [F].
- **Control register** (Fig. 4): a 74LS273 clocked by DEVICE SELECT,
  **no R/W gate — a READ of `$C0Fx` latches it too** (S/U/K from the
  address, CT from whatever is on the bus). A0 = S1, A1 = S2, A2 = U, A3 =
  K, D4-D7 = 15 − CT. RESET clears it (the usual display, CT white) unless
  K, which holds the clear through a diode; K does not protect the control
  register itself.
- **Colour registers** (Fig. 3): two 74LS170 4×4 files, written by I/O
  SELECT — **the whole `$C700-$C7FF`**, A6/A7 not decoded (mirrored every
  `$40`), **a read writes them too** — entry from A4/A5, C from D0-D3 and B
  from A0-A3 in the same access, blocked by K through a diode;
  open-collector outputs + the inverting 74LS258 undo the manual's 15 − x,
  and with neither file enabled the bus floats to black.
- **Output** (Fig. 5): the 4-bit code through a 74LS244 into one resistor
  network per channel (M1: R 470 Ω, B 1 kΩ; M2: B 620 Ω; M3: G 560 Ω,
  B 1k5; M4: R 620 Ω, G 820 Ω) — POM2's `kRvbGraphPalette`; Péritel fast
  blanking tied on, slow switching +12 V. The production board adds three
  1 kΩ trimmers.

**Still [F]**: a dot stretched to 3 ticks, or a 1-tick gap, at a bit-7 byte
boundary (where the 7 MHz sample falls); the 74LS258A's B-input order (the
drawing's letters vs its pin numbers — the pin numbers fit the manual);
"620 Ω" vs 680 Ω; the absence of R/W (missing from the drawing and the
signal list, both readers agree; a continuity check on finger 18 settles
it); S1-S3 re-latched only on video edges (POM2 applies them per frame);
the colour registers' power-on contents («indéterminé», p. 9 — POM2 starts
with the usual colours); the slot (manual: 7 only; POM2 follows the slot);
and DHGR on a //e (the manual is a ][ / ][+ one — POM2 shows COL140 /
BW560 from the latch). Implementation: `LeChatMauveCard::deviceSelectRead
/Write`, `slotRomRead/Write`, `Apple2Display::renderHiResRvbGraph`,
`kRvbGraphPalette`; pinned by `le_chat_mauve_smoke` §§ 13, 17. Implementation:
`LeChatMauveCard::deviceSelectWrite` / `slotRomWrite`,
`Apple2Display::renderHiResRvbGraph`.

### 3.7 Palette, outputs, timing

- Féline: [A] one unit, fenarinarsa's white-balanced OSSC capture
  (AppleWin PR #837: «I will try to make more captures of other Féline and
  IIc adapters»); [D] `RGBMonitor.cpp` @ 7fd7fd7c `PaletteRGB_Feline`
  (greys 5 and 10 differ). POM2 uses it.
- Eve (revision not stated): «a bit different from NTSC, grey1 and grey2
  are different» [A, second-hand] fenarinarsa #764 — no capture. Rev A:
  § 1 [F].
- The cards have **two connectors**: analog RGB (Péritel, with three trim
  pots for R, G, B gain — [B] Féline manual p. 13, RVB Graph manual p. 7,
  the Eve manual; 0-0.7 V on the Féline p. 47 and the Eve, 0-1 V on the RVB
  Graph) and a **TTL header carrying the lo-res colour NUMBER** on
  XRGB1/2/4/8, XRGB1 the low bit, plus TTL sync — the Apple /// convention
  ([B] Féline manual p. 48, Eve manual IX-2 p. 122, RVB Graph manual p. 18;
  Apple TA30822: a «linear-weighted sum»). It is not RGBI: what a TTL
  monitor shows for each code is the monitor's, not the card's, and no TTL
  palette is known [F] — the "TTL-RGBI palette" this plan used to list is
  withdrawn. *Since 2026-09-04 the two connectors do differ on screen*: `NtscParams::rgbBandwidthMHz` (CRT Settings → Analog
  link) low-passes the RGB on its own sample grid — 0 = the TTL header,
  ~5-6 MHz = the Péritel cable, which softens a true 560-dot DHGR/COL280
  picture and leaves 280-dot HGR nearly alone. The three trim pots are not
  modelled yet; per-channel gain would sit in the same pre-pass.
  → [DEV § CRT effect stack](../DEV.md#universal-crt-effect-stack-crteffectstack)
- PAL 50 Hz: the //c PAL and //e PAL profiles already run 312 lines; a
  Chat Mauve plugged on an NTSC profile should keep the machine's timing
  (the card re-drives the machine's SYNC; its timing is the motherboard's)
  — offer auto-PAL as a profile convenience only.

## 4. Architecture: a dot-clock video tap, and cards as logic on it

Today's renderers work per frame, per byte, per mode. The cards are
combinational logic on the motherboard's **video stream** — `VID7M`
(serialised dots), `VID7` (the latched byte's bit 7), `LDPS` (byte latch),
`TEXT`, `GR`, `SEGB`, `~80COL`, `AN3`, `14M`, blanking — and sequential
logic no deeper than a 4-bit latch and the 2-bit mode register. The
faithful architecture is therefore:

```
Memory (main/aux, soft-switch edge log with cycles)
   │
   ▼
VideoTap        per scanline: 560 dots + per-byte VID7 + line-state
                (TEXT/GR/SEGB/80COL/AN3) sampled at the byte, from the
                existing beam-raced event log (mid-line switches for free)
   │
   ├─▶ FelineDecoder   mode latch · 4-dot cell latch · per-byte 560/140 mux
   │                   · HGR 2-bit cell + 3-bit window · AN3-off mono
   ├─▶ EveDecoder      $C0Bx switches · CPREG · PLA truth table on the
   │                   window/mode inputs · TXT16/TXTGREEN · LOCKRES
   ├─▶ RvbGraphDecoder $C0Fx · text colour reg · HGR colour regs
   └─▶ IIcAdapter      = Feline + inferred-80COL quirk model (optional)
   │
   ▼
560 × 192 RGB frame (existing GPU/CRT stages unchanged)
```

Cost: 560 × 192 × 50 = 5.4 M dots/s, a few ns each — negligible against the
NTSC shader path. The tap replaces the per-mode branches in
`Apple2Display` for the Chat Mauve case only; the NTSC/MAME paths stay.

Two things live outside the video path: **CPREG's auto-write** is a
`Memory::memWrite` hook (main `$0400-$07FF` / `$2000-$3FFF` → aux mirror,
gated by TXT16 / ENHRCPREG / LOCKCPREG), and **LOCKRES** hooks the reset
path (`onReset` clears the switches unless locked).

## 5. The plan

Estimates are for one person; each phase ends green with its own pinned
tests and a CHANGELOG entry, in the repo's usual form.

**P0 — Corpus and harness (½ d). ✅ 2026-09-01** — `docs/test_corpus.md` § 5;
`display_golden_hash` grew a per-variant `cm/<variant>/…` block (24 entries
today) and `POM2_GOLDEN_DUMP=<entry>:<line>` prints a scanline as hex RGB.
Arlequin's screens and Extasie's title are pinned since 2026-10-24
(`chatmauve_feline_screens`); the Chat Mauve demo sides and Eve Leonard are
still to fetch.

*As planned:* Register the material in
`docs/test_corpus.md`: the two Chat Mauve demo sides (Pascal
`EDITEUR`, ProDOS `ARLEQUIN` with `GLI16.2` and the `*.CHAR` fonts),
Purplesoft DOS 3.3 (Juillet 83, Rev. B Octobre 83 — `DEMO GR16K`, `TEXT
DEMO`), Purple Pascal 1.1, Arlequin boot + editor, **Extasie** (program,
slideshow, `DSP.IMG` viewer with the `$F2` images), `Eve Leonard`. Extend
the golden-hash harness with a "card variant" axis and a per-dot RGB dump
(`--dump-rgb-line`), so every rule below is pinned by an image, not a
sentence.

**P1 — The Féline and the //c adapter, dot-exact (1½ d). ✅ 2026-09-01** —
`chatmauve_dot_rules` (AppleWin oracle ports, boundary cases spelled out),
160 → 140 fallback, AN3-off mono, the MAME byte-level mixed rule retired.
The "80COL must have been written" detail was NOT added — rightly: it was
an AppleWin heuristic, withdrawn in 1.29.16.0, never a measurement (§ 3.1).
DIX / PoP goldens remain open (Extasie's title: `chatmauve_feline_screens`). This is a missing reference/golden comparison, not a
claim that POM2 cannot boot those disks headlessly. Purplesoft's Eve demo
screens already have their own headless boot/hash test.

*As planned:* The mode latch
as the patent draws it (already there; add the "80COL must have been
written" detail and the 160 → 140 fallback). The mixed mode as a per-byte
560/140 mux over a free-running 4-dot cell latch, pinned on Extasie's
slideshow and on three synthetic boundary cases (colour→BW cut, BW→colour
repeat, aligned). The LCM HGR rule (2-bit cell, 3-bit window) checked
pixel-for-pixel against `UpdateHiResRGBCell`; AN3-off HGR mono
(`POKE -16290,0`). Retire the byte-level MAME mixed rule for this card.
Golden hashes: Extasie slides, DIX's Chat Mauve screens, PoP title (both
adapter behaviours).

**P2 — The Eve's switches and CPREG (1½ d). ✅ 2026-09-01** — everything
listed, the variant as a card setting rather than four catalog keys (see
`LeChatMauveCard.h`), table IX-1 corrected from Purplesoft's tables.
Purplesoft `DEMO GR16K` / `DEMO TEXTE` run through `purplesoft_eve_probe`,
and their stable screens are frozen by `purplesoft_eve_screens` (6 + 7
ordered switch-state + frame-hash pairs).

*As planned:* Full `$C0B0-$C0BF` decode,
CPREG latch on every write, the auto-write hook in `Memory`, TXT16 with
the Eve nibble order, TXTGREEN (drop the "HGR Duochrome" label — the mode
it renders is CP280 and is selected by table IX-1), LOCKCPREG, ENHRCPREG,
LOCKRES on Ctrl-Reset, all switches off at power-on. Table IX-1 wired to
the existing renderers where they already exist (COL140, BW560, CP280 =
today's fg/bg HGR with nibbles swapped) and stubs that blank correctly
for the rest. Card **variant** in the catalog / Slot Config: *Féline*,
*Eve rev B*, *Adaptateur //c*, *RVB Graph* — the variant decides which
registers exist and which modes fall back. Snapshot v3. Pinned by
`Purplesoft DEMO GR16K` and `TEXT DEMO` goldens and a register test
driven from BASIC POKEs.

**P3 — The Eve's pixel rules, from the PLA (2-3 d, research). ▣ BOUNDED
AND CLOSED 2026-09-02** — the fuse map is decoded and simulated in-tree
(§ 3.5.1): the PLS100 is a dot-stream router/cell assembler, not the colour
decoder, so SPEC1/2, DASH and the COL280 palettes are downstream of it and
stay modelled from the manual's prose + Purplesoft's bytes. Reopen only if
a schematic or board trace surfaces.

*As planned:* Pin the
PLS100's pin roles: assume the 7-dot window and mode inputs, evaluate the
decoded terms against COL140 / BW560 / HRAPPLE (whose outputs are known)
until the assignment is unique, then read COL280A/B, CP280, SPEC1/2, DASH
straight out of the fuse map — the Eve decoder becomes the PLA itself.
Cross-check with Purplesoft's `&PLOT` for the COL280 bit order. Where the
PLA leaves a choice (rev A palette, the "blanked" row), document it.

**P4 — RVB Graph. ✓ Done 2026-10-20**, from its manual (§ 3.6). The
2026-09-02 version modelled four `$C0F0-$C0F3` "mode strobes" taken from a
forum thread that turned out to describe another card; the manual shows
those four addresses are the S values of one control register. Now
modelled: the control register (S, U, K, text colour CT), the eight colour
registers, dotted lines, the four-colour system, the K lock and its RESET
rule. Pinned by `le_chat_mauve_smoke` § 13, which replays the manual's
own POKEs.

**P5 — //c adapter quirk (½-1 d, optional). Not started** (the
`IIcAdapter` variant still behaves as the Féline, `LeChatMauveCard.h:32-34`).
Model 80COL as inferred from
the VID7M/LDPS cadence rather than read from the switch, behind a toggle,
so the PoP regression reproduces as on the real box. Off by default.

*What the adapter can see, measured (2026-11-08).* [A] Logic-analyser traces
of a real PAL //e rev B (S. Champailler, Accurapple logic_ana and
video_scanner captures @ 69f45f3) give the cadence of the two signals the
DB-15 carries, mode by mode — a //e, not a //c, so the //c IOU is assumed
to behave alike ([F] until a //c capture):

| Display | LDPS per byte | VID7M |
|---|---|---|
| 80-column text, or double hi-res (80COL on, AN3 off) | two: an aux load in PHASE 0 (+1.73 us after a mid-line `STA $C00D`), the main one in PHASE 1 | 14 MHz dots |
| HGR 40 columns, AN3 on | one; a byte with bit 7 set is loaded one 14M period late (LDPS periods 105 then 91 samples at 100 MHz instead of 98) | 7 MHz, one pulse stretched at that load |
| HGR 40 columns, AN3 off (80COL off) | one, never late, whatever bit 7 says (`test_p_line88`) | 7 MHz, regular |
| any mode, horizontal or vertical blanking | none | — |

*The hypothesis this supports* ([F], not modelled): the adapter cannot see
AN3 at all with 80COL off unless the picture holds bytes with bit 7 set,
and sees nothing during blanking. Its latch input would then depend on the
picture's content and on WHEN the program switches — which is what the
documented symptoms look like: PoP dropping to mono after the first attract
loop, fenarinarsa's non-reproducible pattern sheets (AppleWin's withdrawn
"80COL must have been written" heuristic, § 3.1, was one guess at the same
thing — a level the adapter only learns from a doubled LDPS). Coding it now would be a guess at the adapter's
circuit with nothing to check it against. **Settled by:** the adapter's
schematic (forum.silicium.org clone thread, § 6), or PoP's attract loop on a
real //c adapter compared frame by frame with a candidate model.

**P6 — Video tap (1-2 d). ◔ First rung landed 2026-09-02**: the mode LATCH
now beam-races — `forEachBeamSegment` walks it in parallel with
DisplayState from the same event log (a Dhgr ON→OFF event = the AN3 rising
edge clocking the logged 80COL level), seeded by the card's timestamped
edge ring (`latchBefore`), and each band paints with the latch of its own
moment. Pinned by `chatmauve_latch_split` (COL140/BW560 split both ways in
one frame). Still per-frame: the Eve's `$C0Bx` switches (no video events),
the exact in-cell dot position, and the DIX / PoP goldens. Since then: the Chat Mauve panel shows the eight Eve switch
pairs and CPREG live (`LeChatMauve_ImGui.cpp:71-73`, `:285`), and the
analog Péritel connector got its bandwidth stage (`NtscParams::
rgbBandwidthMHz`, CHANGELOG 2026-09-04). (The TTL-RGBI palette once
listed here is withdrawn: the TTL header carries a colour number, § 3.7.)

*As planned:* Move the Chat Mauve renderers onto the
per-dot tap driven by the existing soft-switch event log, so mid-scanline
mode changes (DIX) and per-line register changes land at the dot; PAL
convenience; TTL-RGBI palette option next to the capture; UI panel shows
the sixteen Eve switches and CPREG live.

**P7 — Docs. ✅** — `DEV.md` § Le Chat Mauve, the `docs/lle_vs_hle.md`
row at L1, the `CLAUDE.md` subsystem row and the CHANGELOG entries all
exist.

*As planned:* `DEV.md` § Le Chat Mauve rewritten from this document,
`docs/lle_vs_hle.md` row moved from "H1 + machine-level lie" to the level
each variant actually reaches, `CLAUDE.md` subsystem row, CHANGELOG.

Total: ~8-10 days, P1 + P2 (≈3 d) delivering what Extasie, Arlequin and
Purplesoft need.

## 6. Open questions, and what closes each

| Question | Closes with |
|---|---|
| ~~COL280A/B bit order~~ **Closed 2026-09-01, from Purplesoft's `& PLOT` run in POM2** (`tests/purplesoft_eve_probe.cpp COL280`): `& COLOR= 9` writes (main `$2A`, aux `$55`), `12` writes (`$55`, `$2A`), `15` writes (`$7F`, `$7F`) — i.e. COL280 is the **560-dot stream in 2-dot cells** (aux byte then main byte, bit 0 first; code = dot + 2 × next dot), NOT one bit from each bank per dot. Codes 1/2/3 = orange/green/white (A), light blue/pink/yellow (B) — the manual's lists in code order. | — |
| ~~The latch at power-on and Ctrl-Reset~~ **Done 2026-11-28**: Video-7 560 mono [B], Féline / adapter COL140 [B], Eve / RVB Graph COL140 [F]; Ctrl-Reset keeps it [A], the ROM's `$C05F` clocking it once | a Féline owner, for the Féline's own reset |
| **Does the Eve take the patent latch?** [A] second-hand #764 (the AppleColor demo, every Eve switch off): latch 00 → BW560. [C] Purplesoft clocks the latch to 00 at start-up (`STA $C000,Y` at `$E053`, twice, 80COL off) and expects COL140 at `& GR 6`, with ENHRCPREG on. POM2 since 2026-11-28 takes both at face value: **the latch rules HR 000 while ENHRCPREG is off; table IX-1 when it is on** [F] | an owner: Purplesoft `& GR 6` after `& GR 1` twice, then with `POKE -16208,0` (ENHRCPREG off) |
| HR3 alone (001) and HR1+HR3 (101) with AN3 off: not in table IX-1 [B] p. 118, unused by Purplesoft [C]; modelled BW560 / COL140 [F] | an owner |
| Does an Eve switch decode on a READ? (the manual has them set by writes) [F] | a `LDA $C0B9` on a real Eve |
| Double lo-res on the Eve: not in its manual; POM2 renders it [F] | an owner |
| SPEC1 / SPEC2 / DASH exact rules — SPEC1/2 modelled from the manual's prose with a 5-dot window, DASH as HRAPPLE [F] | a schematic, a trace of the public board photos (apple2.org.za, Photos/, front and back), or an owner's capture |
| PLA true/complement order, pin assignment, and which board revision the dump comes from [F] | the Signetics 82S100 fuse diagram + the programmer's convention; the public board photos |
| Eve palette (any revision); rev A's rotation direction (§ 1) [F] | a capture from an owner (system-cfg, WDA forums); disassembling Purplesoft's `& CLEAR` |
| ~~RVB Graph HGR colour registers~~ **Closed 2026-10-20**: its manual (§ 3.6) | — |
| ~~RVB Graph: `$C740-$C7FF`, a read of `$C0Fx`, the fill, the video stream~~ **Closed 2026-11-28 by its patent FR 2 509 560** (§ 3.6) | — |
| RVB Graph: a 3-tick dot / 1-tick gap at a bit-7 byte boundary; the 74LS258A input order; 620 vs 680 Ω; R/W truly absent [F] | a real card's RGB output; a continuity check on finger 18 |
| RVB Graph in DHGR on a //e: POM2 shows COL140 / BW560 from the latch [F] — the manual is a ][ / ][+ one | an RVB Graph in a //e |
| //c adapter internals — how it turns the LDPS\* cadence (P5 table, measured on a //e) into AN3 and 80COL, having neither line ([B] IIc TRM Table 11-17) | a schematic said to exist in a clone thread on forum.silicium.org (not found 2026-11-28); or PoP's attract loop on a real adapter |
| Which byte a Féline/Eve applies a new latch value to, after the AN3 edge (the edge itself is measured: 0.8 us into the access cycle, `test_p_line88`) | a capture of the card's RGB output around a mid-line `$C05E→$C05F`; POM2 keeps the column checked by eye on DIX |
| Mixed-mode boundary: [A] "cut / repeat" (//c adapter) vs [C] the Reloaded author's "quadruplet" deduction. POM2 implements cut / repeat only (the AppleWin oracle); the Féline is untested [F] | a Féline capture of Extasie's *Elephant* |
| ~~The Video-7's DIP-switch text colour and inverse-text rule~~ **Done 2026-11-28** (§ 3.1.1) | — |

## 7. Sources

- [B] Le Chat Mauve, *Eve — Manuel de référence* (chapters III-2, IV, V,
  VI, IX; table IX-1 p. 118; addendum p. 127) — archive.org item
  `Le_Chat_Mauve_Manuel_de_re_25cc_2581fe_25cc_2581rence`
  (https://archive.org/details/Le_Chat_Mauve_Manuel_de_re_25cc_2581fe_25cc_2581rence,
  OCR text; the same PDF is apple-iigs.info's `chatmauve.pdf`) and
  apple2.org.za *Apple II Documentation Project / Interface Cards / Apple
  IIe / Le chat mauve Eve* (PDF + erratum (scanned, no text layer), PLA
  `.jed`, demo disks, front/back photos).
- [B] Le Chat Mauve, *Féline* manual (~1985, 27-page scan of 48 printed
  pages; chapter 5 pp. 37-41, installation p. 8, trim pots p. 13, outputs
  pp. 47-48) —
  http://www.apple-iigs.info/doc/fichiers/Le%20chat%20mauve%20Feline%20-%20Manuel.pdf
  (plain http: the https certificate fails), with its addendum.
- [B] *Manuel Arlequin* — https://archive.org/details/LeChatMauveArlequin;
  its mixed-mode statement as quoted by "Extasie Chat Mauve Reloaded"
  (boutillon.free.fr), whose own analysis and annexes (the `$F2` image
  format, the switch sequences, annexe 6) are [C].
- [B] Apple IIe Technical Reference Manual, Table 7-21 (the aux slot,
  pp. 198-200); Apple IIc Technical Reference Manual, Table 11-17 (the
  DB-15 video connector, p. 272) — mirrors.apple2.org.za.
- fenarinarsa (Cyril Lambin), *Le Chat Mauve sur Apple II*
  (fenarinarsa.com/?p=1370, 2020, revised 2023) and *Modes vidéo chelous*
  (?p=1440); AppleWin issues #631, #633, #764, #819, #850, #851, #1152, PR
  #826, #837; `RGBMonitor.cpp` (`RGB_SetVideoMode`, `UpdateHiResRGBCell`,
  `UpdateDHiResCellRGB`, `UpdateText40/80ColorCell`, `RGB_SetVideocard`,
  `PaletteRGB_Feline`) at v1.29.15.0 `98b72658` and master `7fd7fd7c`
  (2026-11-15); commits 2463aae545 (the "80COL written" heuristic) and
  fb0871672c (its removal, 1.29.16.0). AppleWin models no Eve:
  `-rgb-card-type eve` falls back to `apple` (`help/CommandLine.html`).
- [B] Video-7, US patent 4,631,692 *RGB interface*
  (patents.google.com/patent/US4631692A — FIG. 1 shift register and its
  power-on, the four modes, the MIX mux per seven 14 MHz periods).
- [B] Bob Sander-Cederlof, *Apple Assembly Line*, 1986-08 (F/B mode).
- [D] MAME `pla.cpp` and `jedutil.cpp` @ mame0287 (PLS100 fuse order);
  [B] DataIO «UniSite 82S100 Dumping V1.00» (bitsavers).
- [B] FR 2 509 560 A1, *Dispositif de décodage couleur*, Chaillat &
  Chaligne, filed 1981-07-10 — the RVB Graph's schematic:
  https://patents.google.com/patent/FR2509560A1/fr (PDF:
  https://patentimages.storage.googleapis.com/02/5c/22/3c9c4a140dc41e/FR2509560A1.pdf);
  board photos: mirrors.apple2.org.za, *Apple II Documentation Project /
  Interface Cards / Video / Le chat mauve RVB Graph / Photos*.
- *RVB Graph — Manuel de Référence pour Apple II plus*, Le Chat Mauve,
  22 pp. scan (retronik.silicium.org, uploaded 2024): https://retronik.silicium.org/DOCUMENTS/Info/Apple_II/Le%20Chat%20Mauve%20RVB%20Graph%20Manuel%20de%20R%C3%A9f%C3%A9rence%20pour%20Apple%20II%20plus%20%28Le%20Chat%20Mauve%29.pdf
- forum.system-cfg.com t=9395 — a three-TTL-chip RGB card (Sonotec?),
  schematic reverse-engineered from its PCB artwork (2018); **not** a Le
  Chat Mauve card, despite what § 3.6 used to say.
- [D] MAME `src/mame/apple/apple2video.cpp` @ e1479b7a (= mame0287 on the
  rgbmode lines): `dhgr_update`, `an3_w`, `text_update` — the byte-level
  Video-7 rules POM2 ported before P1.
- **[A] Logic-analyser traces of a real PAL //e rev B's video generator**,
  S. Champailler (Accurapple, gitlab.com/wiz21/accurapple @ 69f45f3,
  `composite/tests/logic_ana/*.csv` and `composite/video_scanner/*.lac`,
  AGPL — read, cite the numbers, do not copy the files). The PAL/HAL's
  VID7M, LDPS, SEGB, GR+2, H0, PHASE0, 7M and 14M, the LS166's dot stream
  and parallel inputs, the video ROM's inputs, the IOU, with 6502 A15 as
  the access marker; the driving program (`la_gr_txt.s`) flips TEXT/GR,
  LORES/HIRES and 80COL mid-line. These are the //c adapter's own input
  signals (§ 1), so they are the oracle for the dot-level tap of § 4.
  Already read off (2026-11-08): GR+2 moves 2.41 us after the access cycle;
  TEXT reaches the 4th LDPS, LORES/HIRES and 80COL the 3rd (pinned by
  `raster_switch_kind_offset`); in 80 columns the aux byte loads on an extra
  LDPS in PHASE 0. Its author's notes (`TODOS.org`) also record VID7M
  following a GR+2 fall two 14M periods late when SEGB holds, and at once
  when SEGB falls with it. AN3 (test_p_line88, 80COL off): the IOU pin moves 0.8 us into
  the access and the half-dot mask reaches the byte fetched in that cycle.
  Not yet read off: SEGA lagging H0 by one cycle (its notebook says Sather's
  table 8.1 is wrong there), the ROM-input timing of the GR40/TXT40 switch
  (test_j), and a capture with 80COL ON and AN3 toggled, which would settle
  the double hi-res column POM2 still holds by eye. `test_p_first_line` is
  triggered on the AN3 access, not on VBL: it cannot date the first line.
