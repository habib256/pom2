# Audit de parité — citations de MAME `apple2.cpp` (2026-11-01)

Tranche 13 du brief `docs/audits/BRIEF-parite-amont.md` : le driver Apple ][ /
][+ de MAME, `src/mame/apple/apple2.cpp` depuis le 2022-06-27 (`cf11b333`),
`src/mame/drivers/apple2.cpp` avant. À ne pas confondre avec `apple2e.cpp`
(tranche 2) ni avec `src/devices/bus/a2bus/`.

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep -E '(^|/|:)apple2\.cpp:'
```

Le grep trouve **14 citations dans 7 fichiers**, dont 8 dans `Memory.cpp`. Aucun
fichier de code n'a été modifié. Les sondes et les téléchargements sont sous
`/tmp/audit_apple2/`. Code POM2 lu à `693177dd`.

Cette tranche a ses limites, puisque le driver ][+ est petit. Elle a pourtant
un intérêt propre : chaque règle du ][+ a une jumelle //e dans le même
`Memory.cpp`, et c'est là, plus que dans les numéros de ligne, que se trouvent
les deux constats.

## Révision amont

**Aucun pin.** Ni les fichiers POM2 ni `DEV.md` (lignes 670, 806, 939, 974,
3354 et 9415, qui reprennent les mêmes numéros) ne nomment une révision
d'`apple2.cpp`.

Les verdicts sont rendus contre **`mame0287`**, dont le `apple2.cpp` est
identique octet pour octet à `13184ea2` (2026-03-08). `master` est `bc3e85d0`
(2026-09-08), dernier commit sur le fichier.

**Datation sur tout l'historique : 189 révisions**, soit 52 sous
`src/mame/apple/` (de `cf11b333`, 2022-06-27, à `bc3e85d0`) et 137 sous
`src/mame/drivers/` (de `7c19aac6`, 2015-11-08, à `ab6237da`). Avant 2015-11,
le fichier s'appelait `apple2.c`. Chaque citation a une ancre regex, et une
révision « tombe juste » quand la plage citée contient l'ancre. Le script est
`/tmp/audit_apple2/date_cites.py` ; l'ordre suit la liste de l'API GitHub, pas
les dates de commit, qui se chevauchent le 2026-09-06.

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|---|
| `13184ea2` = **`mame0287`** | 2026-03-08 | `c800_r` rend `$FF` sur `$CFFF` quand personne ne possède la fenêtre ; boutons ][+ sans flottement ; `gameio.cpp` lit les SW à **0** quand rien n'est branché | **0** |
| `6da5e1f1` → `64fb6ec0` | 2026-06-30 → 2026-09-06 | Arbitrage des slots (`CNXX_UNCLAIMED`, `$CFFF` servi par le propriétaire) ; PB0/PB1 ][+ flottants (`dcc9f33c`) ; câblage SW0/SW1 = Shift/Ctrl (`5bdf5863`) ; `FIXME` de l'horloge (`6676d572`). `machine_reset` y occupe 325-331 et la boucle `00 FF` 294-298, toutes deux à la ligne près | **5** : `Memory.cpp:642`, `Memory.cpp:753`, `M6502.cpp:2049`, `EmulationController.cpp:971`, `Memory.h:919` |
| `2eee17b8` → `bc3e85d0` = **`master`** | 2026-09-08 | Environ 20 lignes de notes ajoutées en tête, qui décalent tout le fichier | **5** : `Memory.cpp:918`, `Memory.cpp:1291`, `CassetteDevice.cpp:721`, `SlotBus.cpp:209`, `MemoryLanguageCard.cpp:111` |
| **aucune** (189 révisions, 2015 → 2026) | — | — | **4** : `Memory.cpp:877`, `:911`, `:1306`, `:1345` (la carte d'adresses) |

Les citations relèvent de **deux époques distantes de deux jours** (le 2026-09-06
et `master` du 2026-09-08), auxquelles s'ajoutent quatre numéros qui n'ont
jamais existé. Ces quatre-là sont décalés de −103 lignes par rapport à `master`
pour `$C000`, `$C060` et `$C070` (548/554/555 contre 651/657/658). La ligne de
`$C010` reprend le 548 de `$C000` alors qu'elle aurait dû valoir 549 : elle a été
copiée. Aucune révision ne place la carte en 548. Ce n'est pas non plus une
numérotation sans lignes vides ni sans commentaires (hypothèse testée, sans
résultat).

**Un pin qui couvre aussi les fichiers voisins.** La règle de POM2 pour PB2
vient de `src/devices/bus/a2gameio/gameio.cpp`, pas d'`apple2.cpp`, et c'est là
qu'une correction amont n'a été prise qu'à moitié (constat 1). Tableau proposé
pour `DEV.md` § Joystick / § Reset, modèle § Z8530 SCC :

| Commit amont | Fichier | Contenu | Pris par POM2 ? |
|---|---|---|---|
| `4890c043` (2023-05-19) | `apple2.cpp` | Touche RESET tenue sur la ligne, `keyb_reset_w` | Oui pour la carte langage, l'affichage et `$C800` ; **non pour le clavier** (constat 2) |
| `14c441d5` (2026-05-27) | `gameio.cpp` | SW0-SW3 lisent **1** quand rien n'est branché (« observed behavior on actual Apple II family hardware ») | **Non** (constat 1) |
| `6da5e1f1` (2026-06-30) | `apple2.cpp` | `$CFFF` servi par le propriétaire, bus flottant sinon ; bataille de bus en « last-one-wins » sur ][+ | Oui pour la première partie ; la seconde est écartée (comportement indéfini, voir les pistes) |
| `dcc9f33c` (2026-07-31) | `apple2.cpp` | PB0/PB1 du ][+ flottent haut | Oui (2026-10-31) |
| `ed697c30` (2026-08-12) | `apple2.cpp` | `c080_r` lu même sans effets de bord | Sans objet (débogueur) |
| `6676d572` (2026-09-02) | `apple2.cpp` | `FIXME` : l'horloge devrait valoir 1,020484 MHz (Sather) | Divergence documentée (`CpuClock.h:102-114, 168-172`) |
| `5bdf5863` (2026-09-05) | `apple2.cpp` | Câblage SW0 = Shift, SW1 = Ctrl | Oui |

---

## Rapport 1 — Citations

Les lignes sont triées dans cet ordre : INEXACTE, DÉCALÉE, OK (rev X). « 0287:n » donne la
position à la révision de référence, « m:n » celle sur `master`.

| Fichier POM2:ligne | Citation | Verdict | Commentaire |
|---|---|---|---|
| `src/Memory.cpp:642` | `apple2.cpp:325-331` | **INEXACTE** (positions justes sur `6da5e1f1`→`64fb6ec0`) | `machine_reset` (0287:318-324, m:344-350) est la remise à zéro *machine* de MAME, pas la touche RESET. La touche passe par `keyb_reset_w` (0287:802-813, m:710-721), qui ne fait que `reset_bus()` et la ligne RESET du 6502. `machine_reset` efface d'ailleurs `m_cnxx_slot`, que le commentaire dit « SURVIVE ». Que la carte langage survive ne doit rien à cette fonction : chez MAME, c'est `ramcard16k.cpp:59` (« power-on clear circuit is used in place of /RES »). Une remise à zéro machine MAME *réinitialise* la carte langage. |
| `src/EmulationController.cpp:971` | `apple2.cpp:325-331` | **INEXACTE** (idem) | « II/II+ machine_reset preserves LC + display » : même confusion entre la remise à zéro machine et la touche RESET. |
| `src/Memory.h:919` | `apple2.cpp:325-331` | **INEXACTE** (idem) | Même phrase. Le commentaire voisin `Memory.h:926-927` (sans numéro), « machine_reset (minimal — only kbd strobe + cnxx tracker; LC/display/MMU SURVIVE) », tout comme `DEV.md:974`, propagent la confusion. C'est de là que vient l'effacement du strobe au Ctrl-Reset du ][+ (constat 2). |
| `src/Memory.cpp:877` | `apple2.cpp:548` | DÉCALÉE (aucune révision) | `map(0xc000, 0xc000).mirror(0xf)` : 0287:756, m:651. |
| `src/Memory.cpp:911` | `apple2.cpp:548` | DÉCALÉE (aucune révision) | `map(0xc010, 0xc010).mirror(0xf)` : 0287:757, m:652. Même numéro que la ligne `$C000`. |
| `src/Memory.cpp:1306` | `apple2.cpp:554` | DÉCALÉE (aucune révision) | `$C060-$C067 .mirror(0x8)` : 0287:762, m:657. |
| `src/Memory.cpp:1345` | `apple2.cpp:555` | DÉCALÉE (aucune révision) | `$C070 .mirror(0xf)` : 0287:763, m:658. |
| `src/Memory.cpp:918` | `apple2.cpp:362-372` | OK (`master`) | `keyb_clr_strobe_r`, « reads floating bus, clears strobe », `return read_floatingbus()` ; 0287:336-346. |
| `src/Memory.cpp:1291` | `apple2.cpp:657` | OK (`master`) | `.nopw()` sur `$C060-$C067` ; 0287:762. |
| `src/CassetteDevice.cpp:721` | `apple2.cpp:439` | OK (`master`) | `input() > 0.0 ? 0 : 0x80`, garde `m_cassette.found()` en plus sur `master` ; 0287:412. |
| `src/SlotBus.cpp:209` | `apple2.cpp:563-578` | OK (`master`) | `c800_r` : `$CFFF` est servi par le propriétaire, le bus est flottant sinon. **Fausse à 0287** (542-567), où `$CFFF` sans propriétaire rend `$FF` : la règle date de `6da5e1f1`. Le pin doit être `master`. |
| `src/MemoryLanguageCard.cpp:111` | `apple2.cpp:236-281` | OK (`master`) | `a2bus_inh_w` (m:236-282, 0287:212-258). La parenthèse est exacte. Nuance : sur ][+, la carte langage de MAME est elle-même une carte du slot 0, et la boucle retient la première carte de plage `$D000` quel que soit son état (AMONT FAUX 2). Le « motherboard language card never sees it » vient donc d'`apple2e.cpp`, pas d'ici. |
| `src/Memory.cpp:753` | `apple2.cpp:294-298` | OK (rev `6da5e1f1`→`64fb6ec0`) | Boucle `00 FF` de `machine_start` ; 0287:288-292, m:313-317. La carte langage de MAME (`ramcard16k.cpp`, `device_start`) est remplie de **zéros**, POM2 la remplit en `00 FF` (voir les pistes). |
| `src/M6502.cpp:2049` | `apple2.cpp:325-331` | OK (rev `6da5e1f1`→`64fb6ec0`) | Juste (aucune RAM touchée), mais le chemin RESET est `keyb_reset_w`. Peut monter en grade B (ligne ci-dessous). |

### Lignes corrigées, au format du dépôt

```
// Memory.cpp:642, EmulationController.cpp:971, Memory.h:919
// [B] Sather, Understanding the Apple II (1983) p. 5-29/5-30 — « The Apple II
//     system RESET at pin 31 of the RAM card has no effect on the RAM card
//     configuration. The RESET handling program must configure the RAM card. »
// [C] ROM Autostart ][+ $FA6F-$FA7E — le gestionnaire RESET remet lui-même
//     AN0-AN3 ($C058/$C05A/$C05D/$C05F), $CFFF et le strobe (BIT $C010).
// [D] MAME apple2.cpp:802-813 @ mame0287 — keyb_reset_w : reset_bus() et la
//     ligne RESET du 6502, rien d'autre (machine_reset est la remise à zéro machine).

// Memory.cpp:877
// [D] MAME apple2.cpp:756 @ mame0287 — $C000 .mirror(0xf).r(keyb_data_r).nopw().
// Memory.cpp:911
// [B] Sather, Understanding the Apple II p. 7-5 — « The strobe flip-flop is
//     reset on power up and by any access to $C01X. »
// [D] MAME apple2.cpp:757 @ mame0287 — $C010 .mirror(0xf), lecture et écriture.
// Memory.cpp:918
// [D] MAME apple2.cpp:336-346 @ mame0287 — keyb_clr_strobe_r : efface le
//     strobe, rend read_floatingbus().
// Memory.cpp:1291
// [B] Sather, Understanding the Apple II p. 6-4 — un STA $C06X met le pilote
//     du MPU et le 74LS251 en conflit sur D7 : aucun registre derrière.
// [D] MAME apple2.cpp:762 @ mame0287 — $C060-$C067 .nopw().
// Memory.cpp:1306
// [B] Sather, Understanding the Apple II Table 7.1 — READ PUSHBUTTON 2
//     $C063/$C06B : $C068-$C06F doublent $C060-$C067.
// [D] MAME apple2.cpp:762 @ mame0287 — $C060-$C067 .mirror(0x8).
// Memory.cpp:1345
// [D] MAME apple2.cpp:763 @ mame0287 — $C070 .mirror(0xf), controller_strobe.
// CassetteDevice.cpp:721
// [A] Mesuré : $C060 lit 1 sans magnétophone sur un ][+ et un //e enhanced
//     (AppleWin GH#1234, mesures rapportées par rikkles et tomcw).
// [D] MAME apple2.cpp:412 @ mame0287 — input() > 0.0 ? 0 : 0x80.
// SlotBus.cpp:209
// [B] Sather, Understanding the Apple II p. 6-5 — $CFFF en PHASE 0 : toutes
//     les cartes cessent de répondre à I/O STROBE'.
// [D] MAME apple2.cpp:563-578 @ bc3e85d0 — c800_r sert le propriétaire sur
//     $CFFF et rend le bus flottant sinon (depuis 6da5e1f1 ; $FF à mame0287).
// MemoryLanguageCard.cpp:111
// [B] Sather, Understanding the Apple II p. 6-3/6-4 — INHIBIT' est un
//     wire-OR : n'importe quelle carte isole la ROM de la carte mère.
// [D] MAME apple2.cpp:212-258 @ mame0287 — a2bus_inh_w : INH_READ → upperbank[1] ;
//     les écritures vont à inh_w dans les deux vues.
// Memory.cpp:753
// [D] MAME apple2.cpp:288-292 @ mame0287 — machine_start remplit la RAM
//     principale en 00 FF (ramcard16k.cpp device_start met la carte langage à 0).
// M6502.cpp:2049
// [B] Sather, Understanding the Apple II p. 4-15 — RESET' : trois accès à la
//     pile avec R/W forcé haut (SP décrémenté, rien d'écrit), vecteur
//     $FFFC/$FFFD, bit I mis.
```

Sather est cité d'après la transcription OCR d'archive.org
(`Understanding_the_Apple_II_1983_Quality_Software_djvu.txt`, sous
`/tmp/audit_apple2/utaii.txt`). Les pages sont déduites des pieds de page de la
transcription, à une page près.

---

## Rapport 2 — Constats

### $C063 (PB2) lit 0 quand rien ne le tire ; le silicium lit 1

```
Fichier      : src/PaddleInputs.h:109-114 (appelé par src/Memory.cpp:1319)
Amont        : gameio.cpp:181-187 @ bc3e85d0 (sw2_r → 1 si rien n'est branché,
               depuis 14c441d5, 2026-05-27) ; apple2.cpp:448 @ bc3e85d0
               (flags_r case 3) ; apple2e.cpp:2210 @ master.
               À mame0287 : gameio.cpp:174-180, `m_sw_pullups ? 1 : 0`, avec
               l'en-tête « Apple main boards have no pullups on these inputs,
               which thus read 0 if disconnected ».
Gravité      : MINEUR
Confiance    : CONFIRMÉ pour le port vide (sonde, mesure [A], Sather [B], MAME
               master et AppleWin d'accord). PLAUSIBLE pour la configuration par
               défaut (« port branché ») : tout dépend du contrôleur que POM2
               prétend modéliser.
```

**Divergence.** POM2 rend `connected() && button_[2]`, donc bit 7 à 0 dès que
rien n'appuie sur PB2. Il le fait sur ][+ comme sur //e, et que le port de jeu
soit vide ou branché.

La machine réelle lit 1, preuves à l'appui :

- **[A]** AppleWin GH#1234, mesures sur machine réelle : un ][+ sans manette
  affiche « 1 1 1 1 » pour `PEEK(49248..49251)/128`, un //e enhanced
  « 1 0 0 1 ». Un //e avec une manette Kraft à deux boutons garde aussi SW2
  haut, sur les deux connecteurs.
- **[B]** Sather, p. 7-4 : « the three pushbutton inputs (PB0, PB1, and PB2)
  are actually TTL inputs ». Une entrée LS-TTL en l'air lit haut. Sather,
  p. 7-9/7-10 : « The two pull-down resistors in a paddle set are installed in
  the plug ». Une paire de paddles standard ne tire donc que PB0 et PB1 ; PB2
  reste en l'air même manette branchée.
- **[D]** MAME (`14c441d5`) et AppleWin (1.32.1.0) lisent 1.

**Datation.** POM2 a porté le 2026-10-31 la moitié `apple2.cpp` de la
correction amont : PB0/PB1 flottants (`dcc9f33c`, `DEV.md` § Joystick). Il n'a
pas pris la moitié `gameio.cpp` (`14c441d5`). Sa règle PB2 est donc restée
celle de `mame0287`, que MAME a corrigée depuis comme contraire au matériel.
`tests/input_io_smoke_test.cpp:219` épingle l'erreur
(`!up(plus, kBtn2) && "PB2 is not pulled up"`), et le test de la ligne 111 la
suppose aussi.

**Scénario.** Profil //e Enhanced PAL (le défaut), Machine ▸ décocher le port
de jeu, puis :

```
10 FOR A = 49248 TO 49251 : PRINT INT(PEEK(A)/128);" "; : NEXT
```

POM2 affiche `1 0 0 0`, un //e enhanced réel `1 0 0 1`. Sur le profil ][+ et
port vide, POM2 affiche `1 1 1 0` contre `1 1 1 1` sur la machine. La
conséquence pour le logiciel tient à ceci : un programme qui lit le mod Shift
sur SW2 (actif bas) voit Shift **enfoncé en permanence** sous POM2, alors que la
machine nue, PB2 en l'air, le lui montre relâché.

**Observation.** Sonde `/tmp/audit_apple2/probe_apple2.cpp`, liée sur
`build/libpom2_core_test.a` (plus récente que les sources lues) :

```
PEEK(49248..49251)/128  [silicium GH#1234 : ][+ vide = 1 1 1 1, //e enh vide = 1 0 0 1]
  ][+  port vide                    1 1 1 0
  ][+  port branche (defaut POM2)   1 0 0 0
  //e  port vide                    1 0 0 0
  //e  port branche (defaut POM2)   1 0 0 0
```

Sur le cas par défaut : la seule manette qui ferait lire 0 à PB2 est une
manette dont le troisième bouton porte un pull-down. Le périphérique `joy` de
MAME en modélise une (le bouton de la seconde manette, actif haut). Les
manettes Apple et Kraft n'en ont pas.

### Ctrl-Reset sur ][ / ][+ : POM2 efface le strobe et l'octet clavier

```
Fichier      : src/Memory.cpp:647 (`keyboard_.reset()` dans resetSoftSwitchesWarm),
               src/Keyboard.h:190-199
Amont        : apple2.cpp:802-813 @ mame0287 (keyb_reset_w ne touche ni m_strobe
               ni m_transchar) ; m:710-721.
Gravité      : MINEUR
Confiance    : PLAUSIBLE
```

**Le doute.** Sather écrit que l'impulsion de mise sous tension du 555
« resets the keyboard strobe flip-flop and also forces the RESET' line low »
(ch. 7, Fig. 7.7, p. 7-14/7-15). Il écrit aussi, p. 7-5, que la bascule est
remise à zéro « on power up and by any access to $C01X ». Il ne dit nulle part
explicitement que la touche RESET la laisse ; je n'ai pas consulté le schéma.
AppleWin (`KeybReset`) l'efface, MAME la garde.

**Divergence.** Au Ctrl-Reset et à F12 sur ][ / ][+, POM2 appelle
`Keyboard::reset()`, qui remet à zéro le strobe *et* les 7 bits de la touche.
Sa propre branche //e garde les deux (`cancelPaste` seul, `Memory.cpp:745`),
sur l'autorité de Sather IIe p. 7-3 (« except KEYSTROBE »). Le dépôt se
contredit donc d'une branche à l'autre. Pour la branche ][+, la seule source
citée est `machine_reset`, la remise à zéro machine (voir les citations
INEXACTES ci-dessus).

Sur ][+, la ROM Autostart fait elle-même `BIT $C010` (`$FA7E`) : seul l'octet
clavier reste visible. Sur le ][ de 1977, le Monitor d'origine (RESET → `$FF59`,
sans `BIT $C010`, vérifié dans `roms/apple2o.rom`) laisse aussi voir le strobe.

**Scénario.** Profil Apple ][ (1977). Au Monitor, `300:4C 00 03`, puis `300G`.
Taper `A` (jamais lu), puis RESET. D'après Sather, la machine réelle revient au
Monitor avec la touche toujours en attente, et `GETLN` la lit aussitôt : `*A`.
POM2 affiche `*`. Sur ][+, un gestionnaire de reset (`$3F2`) qui lit `$C000`
obtient `$00` sous POM2 et la dernière touche, bit 7 bas, sur la machine.

**Observation.** Même sonde, section 2 : avec `Q` en attente,
`resetSoftSwitchesWarm()` laisse `$C000 = $00`. Ce qui trancherait : le schéma
de l'Apple II Reference Manual (broches de remise à zéro de la bascule du
strobe), ou un essai sur un ][ / ][+.

### AMONT FAUX — MAME démarre la carte langage ][+ en banque 1 ; Sather dit banque 2

```
Fichier      : src/Memory.cpp:684 (`lcBank2Active = true`, vrai aussi sur ][+)
Amont        : bus/a2bus/ramcard16k.cpp:57-64 @ mame0287 (identique sur master) —
               device_reset : `m_dxxx_bank = 0`. `$C088-$C08F` donnent 0 et
               `$C080-$C087` 0x1000, donc 0 = banque 1.
Gravité      : —
Confiance    : CONFIRMÉ (les deux sources lues, plus [B])
```

**Divergence.** Sather, p. 5-29/5-30 : « At power up, the RAM card is disabled
for reading and enabled for writing. The PRE-WRITE flip-flop is reset, and
bank 2 is selected. The power-up circuit of R5 and C1 forces this
configuration by resetting the LS175 flip-flops. » POM2 fait exactement cela
(lecture ROM, écriture ouverte, pas de pré-écriture, banque 2), AppleWin aussi
(`kMemModeInitialState = MF_BANK2 | MF_WRITERAM`). MAME choisit la banque 1.

**Scénario, côté MAME.** Une écriture en `$D000-$DFFF` avant tout accès
`$C08x` atterrit en banque 1 chez MAME et en banque 2 sur la machine. Ce
résultat protège POM2 contre un futur « alignement » de la mise sous tension
sur `ramcard16k`.

### AMONT FAUX — MAME ][/][+ : la première carte INH du bus masque toutes les autres

```
Fichier      : src/MemoryLanguageCard.cpp:71 (`inhRead_` → `LcInhibitor`),
               slot 0 interne
Amont        : apple2.cpp:212-258 @ mame0287 (m:236-282)
Gravité      : —
Confiance    : CONFIRMÉ côté MAME (lecture du code). Côté POM2, épinglé par
               `saturn_card`, Saturn sur ][+.
```

**Divergence.** `a2bus_inh_w` parcourt les slots de 0 à 7 et retient la
**première** carte dont la plage INH est `$D000-$FFFF`, sans regarder
`inh_type()`. La plage est constante sur `ramcard16k` et `ramcard128k`
(`ramcard16k.h:41-42`, `ramcard128k.h:41-42`). Le `lang` par défaut du slot 0
gagne donc toujours : une Saturn en slot 5 ne voit jamais sa lecture servie
(ROM lue) et ses écritures partent vers la carte du slot 0.

Sather, p. 6-3/6-4 : INHIBIT' est un wire-OR, et « Any RAM card, firmware
card, or other peripheral card can steal access to F8 addressing by pulling the
INHIBIT' line low ». POM2 sert la Saturn à côté de sa carte langage interne.
`tests/saturn_card_test.cpp` le fait sur un ][+, avec le `RAMTEST128K` de
Saturn. MAME admet la simplification (« This code makes a ton of assumptions »,
0287:211).

---

## Pistes examinées et écartées

- **Le Ctrl-Reset ][+ garde la carte langage** : correct.
  - [B] Sather p. 5-29/5-30, cité plus haut.
  - [D] MAME `ramcard16k.cpp:59` et AppleWin `LanguageCard.cpp` (« II/II+
    unaffected »).
  - Sonde : `$D000` en lecture RAM reste `$42` après `resetSoftSwitchesWarm()`.
- **Le Ctrl-Reset ][+ garde l'affichage et les annonciateurs** : correct.
  - MAME `keyb_reset_w` ne touche pas le 74LS259 (`F9334`, sans ligne CLR).
  - AppleWin `CtrlReset` le réserve au //e ; il cite UTAII 4-4, page que je
    n'ai pas retrouvée telle quelle.
  - [C] La ROM Autostart remet elle-même AN0-AN3 (`$FA6F-$FA78`).
- **Le Ctrl-Reset ][+ garde la fenêtre `$C800`** (`SlotBus::reset(false)`) : identique à
  `keyb_reset_w`. La ROM Autostart fait `LDA $CFFF` (`$FA7B`). Sather (p. 6-5)
  ne prévoit que `$CFFF` comme libération.
- **Logique des bascules de la carte langage** (`languageCardSwitchAccess`) :
  - [B] Sather p. 5-29/5-30 : pré-écriture par un accès impair en lecture,
    remise à zéro par un accès pair ou une écriture ; WRITE ENABLE' maintenu
    sinon.
  - Identique à `ramcard16k.cpp do_io` et à `lc_update` (tranche 2). Une
    seule fonction pour ][+ et //e, sans contradiction.
- **Bataille de bus sur `$C800`** : POM2 applique « first-one-wins » partout ;
  le ][+ de MAME est en « last-one-wins » depuis `6da5e1f1`. Le PR amont
  (#15619) le dit lui-même : le comportement est indéfini sur machine, et l'un
  ou l'autre convient.
- **`$CFFF` sans propriétaire** : le bus flottant (`master`) est préféré au
  `$FF` de 0287 ; [B] Sather p. 6-5.
- **Bus flottant ][+** : le fantôme `$1000` en HBL texte, `h_state` à deux
  états 0 et le repli vertical sont conformes à `scanner_address` de `master`
  (`Memory.cpp:1648-1702`) ; 312 lignes en PAL.
- **`$C010-$C01F` sur ][+** : remise à zéro du strobe et bus flottant, en
  lecture comme en écriture ; `$C019` ne décode rien. Conforme à 0287:757 et
  336-354.
- **`$C060` sans magnétophone** : lit `$80`. Conforme à MAME et à la mesure
  [A] de GH#1234.
- **PB0/PB1 du ][+ en l'air** : lisent 1.
  - [A] Mesure GH#1234 sur ][+ : « 1 1 1 1 ».
  - [B] Sather p. 7-9/7-10 : les pull-down sont dans la fiche des paddles.
  - POM2 conforme.
- **Paddles, port vide** : lisent `$80`. Le 558 n'est pas redéclenchable
  (POM2 cite déjà Sather ch. 7, `PaddleInputs.h:186-195`).
- **Câblages Shift du ][+** (SW2, PADDL2/3, SW0/1) : conformes à m:442-464,
  tous actifs bas (`a2kbd.h:112`).
- **`$C050-$C05F`** :
  - lecture et écriture actionnent le latch, comme `switches_r` et
    `write_a0` ;
  - AN3 sans DHGR sur ][+ ;
  - le relais Chat Mauve est voulu (`docs/chatmauve_plan.md`).
- **Écritures `$C000-$C00F` sur ][+** : sans effet (`.nopw()`), à part le
  relais Chat Mauve de `$C00C/$C00D`, voulu (`Memory.cpp:885-898`).
  `display.eightyCol` n'est lu qu'avec `isIIE()` dans le rendu.
- **Horloge NTSC** : 1 022 727 Hz chez POM2, contre 1 020 484 chez Sather et
  1 021 800 chez MAME (avec `FIXME`, m:742-744). Divergence déjà documentée et
  assumée (`CpuClock.h:102-114, 168-172`).
- **Contenu de la RAM à la mise sous tension** : POM2 met la carte langage en
  `00 FF`, MAME la met à zéro (`ramcard16k.cpp device_start`). Une DRAM n'a
  pas de contenu défini à l'allumage [F] ; rien d'observable à trancher.
- **Écritures ][+ sous INH, carte langage du slot 0 ouverte en écriture** :
  - POM2 ne les envoie qu'à la carte qui tient INH.
  - D'après Sather (p. 5-29/5-30 et p. 6-3), la carte langage Apple *sort*
    INH, mais rien ne dit qu'elle le lit ; la machine écrirait peut-être dans
    les deux cartes. AppleWin choisit une seule carte (« don't support
    writing to multiple RAM cards »), MAME aussi.
  - Je n'ai pas de scénario où un logiciel dépend de la double écriture.
  - Non tranché ; le schéma de la carte langage trancherait.
- **F8 ROM de la carte langage Apple** :
  - Sather (p. 5-28 et suivantes) : la carte Apple tire INH pour
    `$F800-$FFFF` en permanence et sert sa propre ROM F8 (Autostart).
  - POM2 et MAME (`ramcard16k`) modélisent une carte sans ROM F8. Sur le
    profil ][ de 1977, une vraie carte Apple rendrait donc le RESET
    « autostart ».
  - Les cartes tierces sans ROM F8 existent : ce n'est pas un écart du
    modèle, juste un choix de carte non documenté.
- **No-Slot Clock sur ][+, garde en écriture** :
  - `Memory.cpp:2123` exclut `lcReadRam` mais pas `inhRead_`, alors que la
    lecture (`:1793`) exclut les deux. Le commentaire dit « Same gating as
    the read site ».
  - Incohérence latente : il faudrait une écriture `$F8xx` sous Saturn en
    lecture RAM pendant un déverrouillage NSC. Pas de scénario.
- **Code mort** : `Memory.cpp:933-934` (`readStrobe(iieMode)` pour ][+) est
  inatteignable, puisque la branche `:920` rend la main avant. Sans effet.
- **Citations sans numéro de ligne** (hors décompte) :
  - `c060_r` (`PaddleInputs.h:76`, `:97`, `DEV.md:9415`, `input_io_smoke_test.cpp:164`,
    `:208`) n'a existé dans **aucune** des 189 révisions : la fonction
    s'appelle `flags_r`.
  - `apple2.cpp do_io` (`Memory.cpp:976`, `:1160`, `DEV.md:806`) n'existe que
    de 2015-11 à 2017-11 (`7c19aac6` → `07de0d57`, ancien driver). Il faut
    aujourd'hui citer `switches_r` (0287:396-402) et `keyb_clr_strobe_r`.

---

## Bilan

- **Citations examinées** : 14 sur 14, confrontées à `apple2.cpp@mame0287`
  téléchargé et datées sur 189 révisions (2015-11 → 2026-09).
- **Verdicts** :
  - INEXACTE : 3 (les trois « machine_reset préserve la carte langage et
    l'affichage ») ;
  - DÉCALÉE : 4 (la carte d'adresses ; aucune révision) ;
  - OK (rev X) : 7, dont 5 sur `master` et 2 sur `6da5e1f1`→`64fb6ec0` ;
  - OK sur `mame0287` : 0 ;
  - ABSENTE : 0 ;
  - NON-PINNÉE : 0.
- **Constats** : CRITIQUE 0 · MAJEUR 0 · MINEUR 2. Le premier (PB2 lit 0) est
  CONFIRMÉ, le second (Ctrl-Reset ][+ qui efface le clavier) PLAUSIBLE.
- **AMONT FAUX** : 2. La banque 2 à la mise sous tension de la carte langage
  (`ramcard16k.cpp`) ; la boucle INH qui laisse le slot 0 masquer toute autre
  carte (`apple2.cpp`).
- **Révision de référence** : tag `mame0287` (contenu = `13184ea2`). **Aucun
  pin.** Les citations relèvent de deux époques
  (`6da5e1f1`→`64fb6ec0` pour la remise à zéro et la RAM, `master` `bc3e85d0`
  pour la carte, le strobe, `$C800`, INH et la cassette), plus 4 numéros
  inexistants. Le pin proposé est un tableau qui couvre aussi `gameio.cpp` et
  `ramcard16k.cpp` (voir § Révision amont).
- **Sources primaires** : Sather, *Understanding the Apple II* (1983), lu dans
  la transcription archive.org ; ROM `apple2p.rom` / `apple2.rom` / `apple2o.rom`
  désassemblées au vecteur RESET ; mesures sur machine réelle de AppleWin
  GH#1234. AppleWin consulté en seconde lecture (`Utilities.cpp CtrlReset`,
  `LanguageCard.cpp`, `Joystick.cpp`, `Keyboard.cpp`).
- **Non vérifié** :
  - le schéma de l'Apple II Reference Manual : remise à zéro de la bascule du
    strobe (constat 2) ; entrée INH éventuelle de la carte langage (piste des
    écritures sous INH) ;
  - les mesures GH#1234, que j'ai lues sans les refaire ; la page Sather
    « 4-4 » qu'AppleWin cite pour l'affichage au reset n'a pas été retrouvée
    telle quelle ;
  - la numérotation des pages de Sather, déduite des pieds de page de l'OCR
    (± 1 page) ;
  - aucun boot complet n'a été sondé pour le constat 2 (profil ][ 1977 avec
    le Monitor d'origine) : la sonde s'arrête à `Memory` ;
  - les quatre numéros 548/554/555 ont été cherchés sur 189 révisions
    `.cpp` ; l'historique `apple2.c` d'avant 2015-11 n'a pas été récupéré ;
  - une ancre trop stricte peut avoir manqué une révision où le code est mis
    en forme autrement. Les positions à `mame0287` et `master` ont été lues à
    la main pour les 14 citations.

---

## Suite donnée (2026-11-01)

Les deux constats sont **corrigés**. PB2 flotte haut (`PaddleInputs.h`,
`input_io_smoke` retourné sur trois assertions). Le Ctrl-Reset ][ / ][+ garde
strobe et octet — et le doute du rapport est levé **au grade B** : un agent a
lu Sather Fig. 7.7 (B10 LS74, CLR ← A12 LS02 ← C01X et la sortie du 555 A13 ;
la touche RESET ne touche que le collecteur de Q5) et la Fig. S-10 du
Reference Manual 1978, relus ensuite sur l'image. MAME avait raison, AppleWin
non. Toujours pas de mesure sur machine. Pinned : `paste_smoke` (bloc ][+).
