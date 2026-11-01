# Audit de parité — carte vidéo, souris, port de jeu et 68705 (2026-11-01)

Tranche de la file « 22+ » du brief `docs/audits/BRIEF-parite-amont.md` : sept
fichiers amont que Cursor n'avait pas atteints.

| Fichier amont | Dépôt / chemin | Cit. | Fichiers POM2 |
|---|---|---:|---|
| `VidHD.cpp` | AppleWin `source/VidHD.cpp` | 8 | `VidHDCard.h/.cpp` |
| `mouse.cpp` | MAME `src/devices/bus/a2bus/mouse.cpp` | 5 | `MouseCard.h/.cpp` |
| `m68705.h` | MAME `src/devices/cpu/m6805/m68705.h` | 5 | `M68705P3.h/.cpp` |
| `m68705.cpp` | MAME `src/devices/cpu/m6805/m68705.cpp` | 5 | `M68705P3.h/.cpp` |
| `4play.cpp` | MAME `src/devices/bus/a2bus/4play.cpp` | 5 | `FourPlayCard.h/.cpp` |
| `joypad.cpp` | MAME `src/devices/bus/snes_ctrl/joypad.cpp` (pas `a2gameio`) | 2 | `SnesMaxCard.h/.cpp` |
| `6821pia.cpp` | MAME `src/devices/machine/6821pia.cpp` | 4 | `MC6821.cpp` |

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ --include='*.cpp' --include='*.h' \
  | grep -E ':(VidHD\.cpp|mouse\.cpp|m68705\.h|m68705\.cpp|4play\.cpp|joypad\.cpp|6821pia\.cpp):'
```

**34 citations.** Code POM2 lu à `1495e283`. Aucun fichier de code modifié,
aucune compilation, aucune sonde. Les téléchargements et le script de
datation sont sous `/private/tmp/claude-501/audit_vmg/` et
`/private/tmp/claude-501/audit_vmg_scripts/date.py`.

## Révision amont

**Aucun pin**, ni dans les fichiers POM2 ni dans `DEV.md` (§ Mouse Card
l. 9279-9330, § 4play l. 7256, § VidHD l. 1735, qui reprennent les mêmes
numéros). Verdicts rendus contre **`mame0287`** pour MAME et contre le
**`master`** d'AppleWin pour `VidHD.cpp` (dernier commit sur le fichier :
`3e8054b4`, 2026-07-26).

**Historique complet récupéré** (API GitHub, puis le blob de chaque
révision) : `mouse.cpp` 44 révisions (2015-11 → 2026-09), `m68705.cpp` 59,
`m68705.h` 35, `6821pia.cpp` 45, `joypad.cpp` 9, `4play.cpp` 4, `VidHD.cpp`
12. Une ancre regex par citation ; une révision tombe juste quand la plage
contient toutes les ancres.

| Fichier | `mame0287` = | `master` | Écart 0287 → master |
|---|---|---|---|
| `mouse.cpp` | `f135a4e4` (2025-11-10) | `6676d572` (2026-09-02) | La seule ligne changée est l'horloge du PIA, qui passe de `1021800` à `A2BUS_1M_CLOCK` (l. 195), sans effet. |
| `m68705.cpp` / `.h` | `f4326e74` (2026-01-23) | `c780dc8d` (2026-10-02) | `8089ec90` ne touche que `state_add`, `c780dc8d` ajoute le MC146805F2. **Rien sur le P3 ni sur le timer** ; le fichier `.cpp` est décalé de +1 à +10 lignes, le `.h` de +1. |
| `6821pia.cpp` | `e9c1f4a4` (2023-06-16) | idem | Aucun. |
| `4play.cpp` | `b6c88f3e` (2024-12-15) | idem | Aucun. |
| `joypad.cpp` | `0e664087` (2021-12-22) | idem | Aucun. |

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|---|
| **`mame0287`** (les cinq fichiers MAME ci-dessus) | 2026-01-23 pour m68705 | Timer MOR/PGM, `tcr_r` avec `TCR_PSC`, PIA à jour | **15** : `m68705.cpp` 5/5, `m68705.h` 4/5, `6821pia.cpp` 3/4, `4play.cpp` 3/5, `joypad.cpp` 1/2 (`84-119`) |
| `fe2f2bb4` → `48933ef8` (m68705.h) | 2020-03-27 → 2024-09-26 | `m6805_timer::reset()` en 73-78 | **1** : `M68705P3.cpp:168` (`m68705.h:73-79`) |
| `4071b43e` → `60bd3086` (6821pia.cpp) | 2020-07-02 → 2020-07-26 | `control_b_w` avec `set_out_cb2(temp)` en 801 | **1** : `MC6821.cpp:240` (`6821pia.cpp:788-802`) |
| `486a4e5c` (mouse.cpp) | 2021-01-06 → 2021-10-02 | `pia_irqa_w`/`pia_irqb_w` en 236-241, sous `WRITE_LINE_MEMBER` | **1** : `MouseCard.cpp:484` (`mouse.cpp:235-240`) |
| AppleWin **`master`** (`3e8054b4`) | 2026-07-26 | `IORead`, `VideoIOWrite`, `Reset` stables depuis `577ffccf` (2021-12-19) | **6** des 8 citations `VidHD.cpp` |
| AppleWin `443545b0` → `47f72188` | 2021-11-30 → 2021-12-18 | `SaveSnapshot` en 168-177, **quatre registres, sans le mode mémoire** | **1** : `VidHDCard.cpp:23` (`VidHD.cpp:170-180`), voir Rapport 1 |
| **aucune** | — | — | **4 mouse** (`:148` ×2, `:46`, `:275-284`), `VidHD.cpp:143-157`, `4play.cpp:41-48` ×2, `joypad.cpp:84-95` |

**Les numéros de `mouse.cpp` n'ont jamais existé**, sauf un. Sur les 44
révisions de 2015 à 2026, `2043600` occupe les lignes 89, 104, 131-132 ou
184-188 ; `PB6 is IRQ for the slot` est en 55 **dans toutes**. La seule
plage qui tombe juste, 235-240, date de `486a4e5c` (2021). L'hypothèse
d'une numérotation sans lignes vides a été testée sur `mame0287` : `:46`
tombe alors juste, mais `:148` donne 144 et `:235-240` donne 231-234. C'est
une coïncidence, pas une explication.

Tableau proposé pour `DEV.md` § Mouse Card, sur le modèle du § Z8530 SCC :

| Fichier amont | Révision qui décrit le code POM2 | Correctifs postérieurs | Pris ? |
|---|---|---|---|
| `mouse.cpp` | `mame0287` (= `f135a4e4`) | `6676d572` : constante d'horloge renommée | Sans objet |
| `m68705.cpp` / `.h` | `mame0287` (= `f4326e74`) | `8089ec90`, `c780dc8d` : rien sur le P3 | Sans objet |
| `6821pia.cpp` | `mame0287` (= `e9c1f4a4` = master) | — | — |
| `4play.cpp` | `mame0287` (= master) ; POM2 s'en écarte **volontairement** sur `$C0n4-$C0nF` (AppleWin, écrit par le concepteur de la carte) | — | — |
| AppleWin `VidHD.cpp` | `master` `3e8054b4` | — | — |

---

## Rapport 1 — Citations

Les lignes sont triées par verdict, du plus grave au moins grave. « 0287:n »
donne la position à `mame0287`, « m:n » celle sur `master`.

| Fichier POM2:ligne | Citation | Verdict | Commentaire |
|---|---|---|---|
| `src/MouseCard.h:156` | `mouse.cpp:148` | **INEXACTE** (numéro jamais existé) | « The 68705 has its own crystal (…2043600 Hz) ». La ligne MAME est en 0287:188 (`M68705P3(config, m_mcu, 2043600)`), et MAME écrit seulement « 68705 is clocked at 2M » (0287:58). **Le schéma Apple 050-0101-A ne montre aucun quartz** : X1 et X2 sont des pastilles de cavalier (0287:39). L'entrée EXT (broche 4) du 6805 reçoit le « 2 MHZ CLOCK » sorti du PAL16R4 U2A (broche 18), qui reçoit Q3 (P1-37). La conclusion du commentaire reste juste : l'horloge suit le bus, jamais l'accélérateur. Le raisonnement ne l'est pas (voir les pistes). |
| `src/MouseCard.cpp:232` | `mouse.cpp:148` | DÉCALÉE (aucune révision) | `M68705P3(config, m_mcu, 2043600);` : 0287:188, m:188. |
| `src/MouseCard.cpp:481` | `mouse.cpp:46` | DÉCALÉE (aucune révision) | « 68705 PB6 is IRQ for the slot » : 0287:55, ligne 55 dans les 44 révisions. Ce que dit la citation est juste : **[B]** sur le schéma, PB6 (broche 18) va sur IRQ* (P1-30) avec R2, 10 kΩ, vers +5 V. |
| `src/MouseCard.cpp:482` | `mouse.cpp:275-284` | DÉCALÉE (aucune révision) | `mcu_port_b_w` : 0287:342-352. Sur `486a4e5c`, la fonction est en 281-291, et 284 coupe le `else`. |
| `src/VidHDCard.cpp:131` | `VidHD.cpp:143-157` | DÉCALÉE (aucune révision) | Branche 640 de `UpdateSHRCell` : m:152-169 (2021-11 : 129-147). Le contenu est exact (palettes 8/12/0/4, table 4-21). |
| `src/VidHDCard.cpp:23` | `VidHD.cpp:170-180` | DÉCALÉE (deux époques) | Les numéros tombent juste sur `443545b0`→`47f72188` (2021-11/12), où `SaveSnapshot` n'écrit que les quatre registres. Le « plus son mode mémoire ][+ » n'existe que depuis `577ffccf` (2021-12-19). Sur master, `SaveSnapshot` occupe 197-217 ; depuis `16b2cf32` (2024-12), il sauve aussi `$400-$9FFF` pour un ][+ ou un //e sans mémoire auxiliaire. |
| `src/FourPlayCard.cpp:31` | `4play.cpp:41-48` | DÉCALÉE | Le bloc `p1` occupe 0287:41-50. La plage citée s'arrête au bit 5 et laisse de côté les bits 6-7 (BUTTON2, BUTTON1) qu'elle décrit. |
| `src/FourPlayCard.h:60` | `4play.cpp:41-48` | DÉCALÉE | Idem. Même numéro dans `DEV.md:7273`. |
| `src/SnesMaxCard.cpp:10` | `joypad.cpp:84-95` | DÉCALÉE (aucune révision) | « suppress contradictory directions » : `port_poll` 0287:91-103. La plage citée s'arrête avant le `if up, no down` (98-100). |
| `src/MouseCard.cpp:484` | `mouse.cpp:235-240` | OK (rev `486a4e5c`, 2021-01-06 → 2021-10-02) | `pia_irqa_w` / `pia_irqb_w` vides ; 0287:296-302. **[B]** Sur le schéma, les broches IRQA (38) et IRQB (37) du PIA sont **NC** : elles ne sont pas « câblées mais mortes », elles ne sont pas câblées du tout. |
| `src/M68705P3.cpp:168` | `m68705.h:73-79` | OK (rev `fe2f2bb4`→`48933ef8`, 2020-03 → 2024-09) | `m6805_timer::reset()` : 0287:75-81, m:76-82. |
| `src/MC6821.cpp:240` | `6821pia.cpp:788-802` | OK (rev `4071b43e`→`60bd3086`, 2020-07) | `control_b_w` : 0287:778-807, `set_out_cb2(temp)` en 803. |
| `src/M68705P3.h:199` | `m68705.cpp:54` | OK | `{ 0x0784, "MOR" }`, inchangé depuis `87d31219` (2023-10). **[B]** fiche MC68705P5 : MOR en `$784`. |
| `src/M68705P3.cpp:32` | `m68705.cpp:465-476` | OK | `device_start` : `get_mask_options()`, branche `MOR_TOPT` en 0287:468 ; m:466-477. |
| `src/M68705P3.cpp:128` | `m68705.cpp:465-476` | OK | Idem. **[C]** Le MOR du dump `341-0269` (SHA-1 `3a2baa66…`, celui de MAME) vaut `$40` : TOPT, ÷1, source CLOCK. |
| `src/M68705P3.cpp:349` | `m68705.cpp:994-1015` | OK | `m6805_timer::tcr_w`, recopié ligne pour ligne ; m:1004-1025. |
| `src/M68705P3.cpp:386` | `m68705.cpp:1017-1046` | OK | `m6805_timer::update` ; m:1027-1056. POM2 traite TIMER (entrée externe) comme arrêté : MAME compte `m_timer_edges`, toujours 0 ici, donc c'est équivalent. |
| `src/M68705P3.h:202` | `m68705.h:249-253` | OK | `MOR_PS`…`MOR_CLK` ; m:250-254. |
| `src/M68705P3.h:207` | `m68705.h:93-101` | OK | `enum tcr_mask` ; m:94-102. |
| `src/M68705P3.h:214` | `m68705.h:40-45` | OK | `enum timer_source`, en place depuis `1d8c814c` (2019). |
| `src/M68705P3.cpp:203` | `m68705.h:86` | OK | `tcr_r` ; m:87. **[B]** fiche : en mode MOR, b3 « reads as a 1 ». |
| `src/MC6821.cpp:120` | `6821pia.cpp:403-412` | OK | `port_a_r`, strobe de CA2 sous `if (m_out_ca2)`. |
| `src/MC6821.cpp:149` | `6821pia.cpp:444-451` | OK | `port_b_r`, rétablissement de CB2 (commentaire 444-449, code 450-451). |
| `src/MC6821.cpp:218` | `6821pia.cpp:689-706` | OK | `port_b_w`, strobe de CB2 sous `c2_strobe_mode` seul. |
| `src/FourPlayCard.cpp:62` | `4play.cpp:123-140` | OK | `read_c0nx` (123-141). |
| `src/FourPlayCard.h:32` | `4play.cpp:95-140` | OK | Constructeurs, `device_start` vide, `read_c0nx`. |
| `src/FourPlayCard.h:76` | `4play.cpp:136` | OK | `default:` (le `return 0xff` est en 137). Cette constante `kUnmapped` n'est plus lue nulle part, voir les pistes. |
| `src/SnesMaxCard.h:11` | `joypad.cpp:84-119` | OK | `port_poll` et `read_pin4` (91-114). `write_strobe` (120-127), qui fait partie du protocole, est juste à côté. |
| `src/VidHDCard.h:25` | `VidHD.cpp:68-79` | OK (master) | `IORead`, `$24 $EA $4C` (69-79). |
| `src/VidHDCard.h:31` | `VidHD.cpp:82-102` | OK (master) | `VideoIOWrite` (82-100). |
| `src/VidHDCard.h:36` | `VidHD.cpp:81` | OK (master) | « VidHD has no support for reading the IIgs video registers ». |
| `src/VidHDCard.h:38` | `VidHD.cpp:57-61` | OK (master) | `Reset` (58-62) : `m_NEWVIDEO = 0`. |
| `src/VidHDCard.h:135` | `VidHD.cpp:57-61` | OK (master) | Idem. |
| `src/VidHDCard.cpp:73` | `VidHD.cpp:93-99` | OK (master) | Les quatre registres IIgs (94-98). |

### Lignes corrigées, au format du dépôt

```
// MouseCard.h:156
// [B] Apple, schéma 050-0101-A (A// Mouse Board) — pas de quartz : EXT (broche 4)
//     du 6805 reçoit le « 2 MHZ CLOCK » du PAL16R4 U2A (broche 18), qui reçoit
//     Q3 (P1-37). Le MCU suit donc l'horloge du bus (PAL compris), jamais
//     l'accélérateur.
// [D] MAME mouse.cpp:188 @ mame0287 — M68705P3(config, m_mcu, 2043600).
// MouseCard.cpp:232
// [B] MC68705P5 Advance Information (Motorola, nov. 1983) — horloge
//     interne = fOSC ÷ 4 (≈ 510,9 kHz).
// [D] MAME mouse.cpp:188 @ mame0287 — M68705P3(config, m_mcu, 2043600).
// MouseCard.cpp:481-484
// [B] Schéma 050-0101-A — 6805 PB6 (broche 18) → IRQ* (P1-30), R2 10 kΩ vers
//     +5 V ; IRQA/IRQB du PIA (37/38) NC.
// [D] MAME mouse.cpp:55 @ mame0287 (« PB6 is IRQ for the slot ») ;
//     mouse.cpp:342-352 @ mame0287 — mcu_port_b_w ; 296-302 — pia_irqa_w/irqb_w vides.

// M68705P3.h:199
// [B] MC68705P5 (nov. 1983) — Mask Option Register en $784.
// [D] MAME m68705.cpp:54 @ mame0287.
// M68705P3.cpp:32, :128
// [C] MOR du dump 341-0269 = $40 (TOPT=1, PS=0, CLS=0).
// [B] MC68705P5 (nov. 1983), § Timer — TOPT=1 : prescaler et source fixés
//     par le MOR, « TCR bits b7 and b6 are read/write, the other six have no
//     effect on a write and read as logic 1s ».
// [D] MAME m68705.cpp:465-476 @ mame0287 — device_start, branche MOR_TOPT.
// M68705P3.cpp:349
// [D] MAME m68705.cpp:994-1015 @ mame0287 — m6805_timer::tcr_w.
// M68705P3.cpp:386
// [D] MAME m68705.cpp:1017-1046 @ mame0287 — m6805_timer::update.
// M68705P3.h:202
// [D] MAME m68705.h:249-253 @ mame0287 — enum mor_mask.
// M68705P3.h:207
// [D] MAME m68705.h:93-101 @ mame0287 — enum tcr_mask.
// M68705P3.h:214
// [D] MAME m68705.h:40-45 @ mame0287 — enum timer_source.
// M68705P3.cpp:168
// [B] MC68705P5 (nov. 1983) — « At Reset, the prescaler and counter are
//     initialized to an all 1s condition; TIR is cleared and TIM is set ».
// [D] MAME m68705.h:75-81 @ mame0287 — m6805_timer::reset().
// M68705P3.cpp:203
// [B] MC68705P5 (nov. 1983) — TCR b3 « reads as a logic 0 when TOPT = 0 and
//     as a 1 when TOPT = 1 ».
// [D] MAME m68705.h:86 @ mame0287 — tcr_r.

// MC6821.cpp:120 / :149 / :218
// [D] MAME 6821pia.cpp:403-412 / 444-451 / 689-706 @ mame0287.
// MC6821.cpp:240
// [D] MAME 6821pia.cpp:789-803 @ mame0287 — control_b_w, set_out_cb2(temp)
//     inconditionnel.
// (Sur la carte souris, CB2 et CA1/CA2 sont NC et CB1 est tiré à +5 V par R3 :
//  [B] schéma 050-0101-A. Aucune de ces règles n'y est observable.)

// FourPlayCard.cpp:31, FourPlayCard.h:60
// [B] Lukazi, « Apple II 4play joystick card RevB » (blog, 2016-05) —
//     « Trigger1 : Trigger2 : Not Used (always high) : Trigger3 : Right :
//     Left : Down : Up » ; Trigger3 à 0 sans le câble d'extension trois boutons.
// [D] MAME 4play.cpp:41-50 @ mame0287 — bloc "p1".
// FourPlayCard.cpp:62, FourPlayCard.h:32
// [D] MAME 4play.cpp:123-141 @ mame0287 — read_c0nx.
// FourPlayCard.h:76
// [D] MAME 4play.cpp:136-137 @ mame0287 — default: return 0xff (POM2 rend le
//     bus flottant, comme AppleWin FourPlay.cpp, écrit par le concepteur).

// SnesMaxCard.h:11
// [D] MAME joypad.cpp:91-127 @ mame0287 — port_poll, read_pin4, write_strobe.
// SnesMaxCard.cpp:10
// [D] MAME joypad.cpp:91-103 @ mame0287 — port_poll : gauche efface droite,
//     haut efface bas.

// VidHDCard.cpp:23
// [D] AppleWin VidHD.cpp:197-217 @ 3e8054b4 — SaveSnapshot : mode mémoire et
//     les quatre registres, plus $400-$9FFF sur ][/][+ ou //e sans aux.
// VidHDCard.cpp:131
// [D] AppleWin VidHD.cpp:152-169 @ 3e8054b4 — UpdateSHRCell, mode 640 ;
//     palettes 8/12/0/4 (IIgs HW Ref table 4-21).
// VidHDCard.h:25 / :31 / :36 / :38 / :135, VidHDCard.cpp:73
// [D] AppleWin VidHD.cpp:69-79 / 82-100 / 81 / 58-62 / 58-62 / 94-98 @ 3e8054b4.
```

Le passage du MC6821 au grade B n'est pas proposé ligne par ligne : sur ces
quatre points, la fiche et MAME ne disent pas la même chose (voir les
pistes), et la carte souris ne permet pas de trancher.

---

## Rapport 2 — Constats

**Aucun constat de comportement.** Les 34 citations ont été confrontées au
code POM2, qui suit fidèlement l'amont, et, pour le 68705, le PIA et la
souris, à la fiche ou au schéma. Les points où l'amont s'écarte des
documents (CB2 du PIA, écriture de PSC en mode MOR) ne sont pas
observables sur la seule carte qui utilise ces modèles : ils sont dans les
pistes, avec la raison.

### AMONT FAUX — AppleWin VidHD : composante ×16, le blanc `$FFF` sort en 240

```
Fichier      : src/VidHDCard.h (`rgb12`, ×17) et src/VidHDCard.cpp:58-61 (`iigsColour`, ×17)
Amont        : AppleWin VidHD.cpp:120-128 @ 3e8054b4 — ConvertIIgs2RGB :
               rgb.r = color.red * 16 (de même pour g et b)
Gravité      : —
Confiance    : PLAUSIBLE. Le code AppleWin est lu. Que le CNA 4 bits du VGC
               (ou la sortie HDMI de la VidHD) monte jusqu'à la pleine échelle
               est la lecture naturelle d'un CNA linéaire, mais je n'ai ni
               mesure ni page de la IIgs Hardware Reference qui le dise.
```

**Divergence.** AppleWin multiplie chaque composante 4 bits par 16 : `$F`
donne 240, et le blanc SHR `$0FFF` sort en (240, 240, 240). POM2 multiplie
par 17 (`rgb12`, `iigsColour`) : `$F` donne 255, `$0` donne 0, et les
seize niveaux sont équidistants entre les deux bornes.

**Scénario, côté AppleWin.** Une image SHR qui utilise le blanc `$FFF` est
affichée en gris clair à 94 %, et le dégradé `$000`→`$FFF` n'atteint pas le
blanc. Ce constat protège POM2 contre un futur « alignement sur AppleWin »
du facteur, qui assombrirait toutes les palettes SHR de 6 %.

---

## Pistes examinées et écartées

- **Horloge du 68705** : rythmée sur l'horloge du bus, la moitié des ticks du
  bus.
  - [B] Schéma 050-0101-A : EXT ← « 2 MHZ CLOCK » ← PAL16R4 ← Q3. Le MCU
    suit le bus, y compris en PAL ; un accélérateur ne change pas Q3.
  - [B] Fiche MC68705P5 : horloge interne fOSC ÷ 4.
  - POM2 est conforme (`busClock_.busTicks`). Seul le mot « crystal » du
    commentaire est faux (Rapport 1).
- **MOR `$40`, ÷1, source CLOCK** :
  - [C] lu dans `roms/mouse_341-0269.bin` (SHA-1 identique à celui de
    MAME) ;
  - [B] la fiche confirme que, avec TOPT=1, une écriture dans TCR ne
    reprogramme ni le diviseur ni la source.
- **Lecture de TCR en mode MOR** : écart sans effet.
  - [B] La fiche veut que les bits 0-5 se lisent toujours à 1. MAME et POM2
    rendent `m_tcr | PSC`, donc les bits 0-2 et 5 de la dernière écriture.
  - Le firmware `341-0269` n'accède à `$09` que par `BSET/BCLR 6` et
    `BCLR 7` (`$548`, `$565`, `$5B0`, `$627`, `$63E`, `$67D` ; recherche
    d'octets dans la ROM).
  - Ces lecture-modification-écriture réécrivent ce qu'elles ont lu.
    `m_tcr` part de `$7F` au reset, et ses bits 0-5 restent donc à 1 pour
    toujours, ce qui rend la même valeur que la fiche.
- **Écriture de PSC en mode MOR** : sans effet observable.
  - [B] Selon la fiche, elle n'a aucun effet en mode MOR. MAME et POM2
    remettent pourtant le prescaler à zéro.
  - Avec ÷1, `prescale & ((1<<0)-1)` vaut toujours 0 : le prescaler
    n'intervient pas.
  - Latent seulement pour un autre 68705 en mode MOR avec un diviseur ≠ 1.
- **Reset du timer** : `tdr=$FF`, `prescale=$7F`, `tcr=$7F` (TIR=0, TIM=1).
  [B] Fiche : « initialized to an all 1s condition ».
- **Les trois règles CB2/CA2 du MC6821** (`port_b_r` qui rétablit CB2,
  strobe de `port_b_w`, `control_b_w` inconditionnel) : copiées fidèlement
  de MAME.
  - [B] La fiche Motorola MC6820, *MC6800 Microcomputer System Design Data*
    (1976), p. 47, Table 5, dit autre chose sur deux points :
    - CB2, en mode strobe de type CB1, remonte « when the interrupt flag
      bit CRB-7 is set by an active transition of the CB1 signal », pas à
      la lecture du port B ;
    - CB2 n'est une sortie que si CRB-5 vaut 1, alors que MAME pilote
      `set_out_cb2` dans `port_b_w` et `control_b_w` quel que soit CRB-5.
  - Ce n'est pas un `AMONT FAUX`, puisque POM2 suit MAME, et ce n'est pas
    un constat : sur la carte souris, CB2, CA1 et CA2 sont **NC** et CB1 est
    tiré à +5 V par R3 (schéma), donc rien de tout cela n'est observable.
  - Fiche du 6820, pas du 6821, mais les deux puces sont réputées
    fonctionnellement identiques ; la carte porte un 6520/6521 d'après la
    sérigraphie du schéma.
  - **À rouvrir si une autre carte POM2 adopte `MC6821`.**
- **Banque de l'EPROM au reset** : POM2 met `romBank = 0`. [B] Schéma : PB1-PB3
  sont tirés à la masse par R5-R7, et le reset du PIA (DDR=0) les laisse en
  l'air. Conforme.
- **Reset de la carte souris** : POM2 réinitialise le MCU et le PIA à chaque
  `onReset`. [B] Schéma : RES* (P1-31) va au PIA (broche 34) et au 6805
  (broche 28). Conforme.
- **PB5 et PC1 tirés à la masse** (R4 sur le schéma) : non modélisé, comme
  chez MAME. Pas de scénario : le firmware pilote toujours un côté avant de
  lire.
- **IRQ de la carte souris dérivée de PB6 seul** : [B] schéma (IRQA/IRQB NC).
  Conforme, y compris la republication après restauration d'un snapshot.
- **4play, bit 4 = Trigger 3** : POM2 et MAME le lisent comme un bouton.
  - AppleWin (`FourPlay.cpp`, en-tête de Lukazi, 2021) écrit « Bit 4 =
    Not Used, Always Low ».
  - [B] Le blog rev B du même auteur : « Trigger3 will be zero unless
    joystick supports three buttons and the three button extension cable
    is used ».
  - AppleWin ne modélise simplement pas le câble. POM2 est conforme ; ce
    n'est pas un `AMONT FAUX`, AppleWin n'est pas faux, il simplifie.
- **4play, bit 5 toujours à 1** : [B] blog (« Not Used (always high) »).
  MAME, AppleWin et POM2 sont d'accord.
- **4play, `$C0n4-$C0nF`** : écart non tranché.
  - Le code rend le bus flottant (AppleWin, écrit par le concepteur), MAME
    rend `$FF`. Le blog n'en dit rien.
  - Si la carte ne décodait que A0-A1, ces adresses reproduiraient
    `$C0n0-$C0n3`. Il faudrait le schéma de la carte rev B.
  - Hygiène : l'en-tête `FourPlayCard.h:32-33`, `kUnmapped`, que rien ne lit,
    et `DEV.md` § 4play (« everything else is `$FF` », « offsets 4-15 reading
    `$FF` ») décrivent encore MAME. Le test passe parce qu'un `SlotBus` isolé
    rend `$FF`.
- **SNES MAX** :
  - Polarité : appuyé = 0. MAME `snesmax.cpp` (`read_pin4() ? 0x00 : 0x80`),
    AppleWin (`~buttons`) et POM2 sont d'accord.
  - Bits au-delà du 16ᵉ : la ligne lit « appuyé », ce qui vaut « manette
    branchée » chez AppleWin. Même convention que `0xffff0000` dans MAME.
  - Latch de 7 ticks de bus, chargé au front descendant du strobe : conforme
    à `write_strobe` et `reset_latch`.
  - L'ordre des bits (B, Y, Select, Start, U, D, L, R, A, X, L, R) est
    conforme à `joypad.cpp:21-32`.
- **VidHD, signature `$Cn00-02` et `$C0nX` flottant** : conforme à
  `IORead` et à `InitializeIO` d'AppleWin.
- **VidHD, les quatre registres en écriture seule** : conforme ; SHR quand
  NEWVIDEO vaut `$C0` (`VidHD.h:37`, `IsSHR`).
- **VidHD, mode 640** : palettes 8/12/0/4, une paire de bits par point, MSB
  d'abord. Conforme à AppleWin et à la table 4-21.
- **VidHD, couleur de départ du remplissage 320** : AppleWin lit le point
  précédent du tampon de sortie, au-delà du début de ligne. POM2 prend
  `fillmode_init` du VGC (MAME, mesure John Brooks), et l'en-tête dit que le
  comportement de la VidHD est supposé. Divergence documentée (`DEV.md`
  § VidHD).
- **VidHD, NEWVIDEO bit 5 (DHGR noir et blanc)** : AppleWin le modélise
  (`IsDHGRBlackAndWhite`), POM2 non, et il le documente (« NOT modelled »).
- **VidHD, RESET efface NEWVIDEO** : conforme à AppleWin (`Reset`, appelé
  aussi à la mise sous tension). Je ne sais pas ce que fait la vraie carte :
  aucune source matérielle trouvée.

---

## Bilan

- **Citations examinées** : 34 sur 34, confrontées aux fichiers amont
  téléchargés et datées sur leur historique complet (196 blobs MAME, 12
  blobs AppleWin).
- **Verdicts** :

  | Fichier | OK | OK (rev X) | DÉCALÉE | INEXACTE |
  |---|---:|---:|---:|---:|
  | `VidHD.cpp` (8) | 6 | 0 | 2 | 0 |
  | `mouse.cpp` (5) | 0 | 1 | 3 | 1 |
  | `m68705.h` (5) | 4 | 1 | 0 | 0 |
  | `m68705.cpp` (5) | 5 | 0 | 0 | 0 |
  | `4play.cpp` (5) | 3 | 0 | 2 | 0 |
  | `joypad.cpp` (2) | 1 | 0 | 1 | 0 |
  | `6821pia.cpp` (4) | 3 | 1 | 0 | 0 |
  | **Total (34)** | **22** | **3** | **8** | **1** |

  ABSENTE : 0. NON-PINNÉE : 0.
- **Constats** : CRITIQUE 0 · MAJEUR 0 · MINEUR 0.
- **AMONT FAUX** : 1, le facteur ×16 d'AppleWin pour la VidHD (PLAUSIBLE).
- **Révision de référence** : `mame0287` pour MAME, `master` `3e8054b4`
  pour AppleWin. **Aucun pin.**
  - Le 68705 et le PIA suivent `mame0287`, à deux citations près, datées de
    2020-2024.
  - **4 des 5 numéros de `mouse.cpp` n'ont existé dans aucune des 44
    révisions** ; le cinquième date de 2021.
  - Rien de ce qui a été commis en amont après `mame0287` ne touche le P3,
    le PIA, la souris ou le 4play. Le pin proposé est donc simplement
    `mame0287`, avec le tableau de la section « Révision amont ».
- **Sources primaires** :
  - schéma Apple 050-0101-A de la carte souris (apple2.org.za), lu sur
    l'image ;
  - fiche MC68705P5 Advance Information (nov. 1983, bitsavers) ;
  - fiche MC6820 dans *MC6800 Microcomputer System Design Data* (1976,
    bitsavers) ;
  - blog de Lukazi sur le 4play rev B ;
  - octet MOR et accès au TCR lus dans le dump `341-0269`.
- **Non vérifié** :
  - **la fiche du MC68705P3 elle-même** : j'ai lu celle du P5, qui ne
    diffère que par le bit SNM du MOR. Que le timer du P3 soit identique
    est une hypothèse, que le `& 0xF7` de MAME (« no SNM bit ») partage ;
  - **la fiche du MC6821** : j'ai lu celle du 6820. La différence de
    rétablissement de CB2 avec MAME n'est pas tranchée sur silicium.
    MAME l'affirme dans un commentaire (`6821pia.cpp:444-448`,
    `cb1_w`) sans citer de source ;
  - le schéma du 4play rev B, qui dirait ce que lisent `$C0n4-$C0nF`
    (bus flottant, `$FF` ou miroir) ;
  - le comportement de la vraie VidHD au RESET et sa conversion de couleur
    (l'`AMONT FAUX` reste PLAUSIBLE) ; aucune capture de la carte ;
  - la mesure du 59,8 Hz de `mouse_card_smoke` n'a pas été relancée
    (interdiction de compiler) : je la tiens de `DEV.md` ;
  - les Apple II Mouse Technical Notes #1-#7 n'ont pas été relues pour
    cette tranche. Le schéma suffisait pour les points ouverts.
