# Audit de parité — citations de MAME `apple2video.cpp` (2026-11-01)

Tranche `apple2video.cpp` du brief `docs/audits/BRIEF-parite-amont.md`. Fichier
amont : `src/mame/apple/apple2video.cpp` (depuis `5b82686d`, 2022-07-01),
auparavant `src/mame/shared/apple2video.cpp` (`ab6237da`, 2022-06-27) puis
`src/mame/video/apple2.cpp` (jusqu'à `cf11b333`, 2022-06-27).

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep 'apple2video\.cpp:'
```

**12 citations** au grep, et **6 plages de plus** écrites dans les mêmes
commentaires sans le nom de fichier collé (`:486-493`, `764-825`, deux fois
`:788-791` et `:571-583`) : **18 plages numérotées** sous `src/`, toutes
datées. S'y ajoutent 9 plages hors `src/` (docs, tests) et une quinzaine de
citations par nom de symbole seulement, vérifiées sur le fond.

Numéros POM2 : arbre de travail au-dessus de **`af7dd56f`** (« Audit reports
for slices 4-14… »). `src/Memory.cpp` est modifié en parallèle par
l'utilisateur (+16 lignes au-dessus du bloc vidéo) : les citations de
`Memory.cpp` sont en 1620/1664/1673 dans l'arbre, en 1604/1648/1657 à
`af7dd56f`. Aucun fichier du dépôt n'a été modifié en dehors de ce rapport.
Sondes, révisions amont et scripts : `/tmp/audit_apple2video/`.

**Sources de rang supérieur consultées** (texte OCR d'archive.org, item
`Understanding_the_Apple_IIe`, et `understanding_the_apple_ii`) :
Jim Sather, *Understanding the Apple IIe* (1985), ch. 3, 5, 7, 8 et annexe I ;
*Understanding the Apple II* (1983), ch. 8. Deux jeux de **photos de matériel
réel publiés** : le tableau « VIDMODE LATENCY » d'arekkusu42 joint à MAME
PR #15247 (//e, IIgs, ][+, //c PAL) et les essais de tomcw sur un //e PAL
réel dans AppleWin GH#654/#656 ; une **mesure publiée** de Ryan Schmidt sur
un //e non amélioré (TomHarte/CLK#1204). Seconde lecture : AppleWin `master`
`4a43bcdb` (2026-10-31 ; `Video.cpp` @ `5b7360e1`, `Memory.cpp` et `NTSC.cpp`
@ `3e8054b4`).

---

## Révision amont

**Pas de pin.** Ni `Memory.cpp`, ni `Apple2Display*.{h,cpp}`,
`Apple2VideoDecode.h`, ni `DEV.md` § Display ne nomment une révision.
Verdicts rendus contre **`mame0287`**, dont le fichier est identique octet
pour octet à `13184ea2` (2026-03-08, PR #15073).

Historique complet récupéré : 49 révisions sous `src/mame/apple/`
(`5b82686d` 2022-07-01 → `e1479b7a` 2026-09-12, = `master` au 2026-11-01),
2 sous `shared/`, 55 sous `video/apple2.cpp` (2015-11-08 → 2022-06-27) ;
105 blobs (celui du commit de renommage `cf11b333` n'a pas pu être lu à son
nouveau chemin). Une ancre regex par citation (le code décrit) ; une révision
« tombe juste » quand la plage citée contient toutes les ancres. Comme les
tranches 5 et 10 ont trouvé des numéros comptés sans les lignes vides, chaque
citation a été datée **dans les deux numérotations** (réelle, et sans lignes
strictement vides). La date d'écriture de chaque citation vient de
`git log -S` sur POM2.

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste (/18) |
|---|---|---|---|
| `13184ea2` = **`mame0287`** | 2026-03-08 | PR #15073 (« improve video timing ») : HBL relatif, 80STORE en course de faisceau. **`scanner_address` n'existe pas encore dans ce fichier** (le bus flottant //e est inline dans `apple2e.cpp:3319-3433`, la version AppleWin `(i + 63) % 65`) | **0** |
| `55a64a57` → `b3bc8ac9` | 2026-04-24 → 04-30 | PR #15247 : délais propres à chaque commutateur (`delayed_update`, `m_delay_bias`), `scanner_address` partagé, déplacement de +140 lignes | **7** en numérotation réelle : `Memory.cpp:1620` (124-201), `:1664` (140), `:1673` (149), `Apple2Display.h:756-757` et `Apple2Display.cpp:1320-1322` (788-791, 571-583) — écrites les 2026-05-15 et 05-24, quand `master` était `b3bc8ac9` |
| `b3bc8ac9`, **sans lignes vides** | 2026-04-30 | idem | **3** : `Apple2Display.cpp:2014` (479-498), `Apple2VideoDecode.h:57` (376-419), et hors `src/` `emulation-parity.md` (460-471) ; `:1681` (479-485) et `:1684` (486-493) à 1 et 2 lignes près. Toutes écrites les 2026-05-12/13 |
| `7a73cf23` | 2026-05-25 | « Fixed up some stuff » (Vas Crabb) : mise en forme seule, −3 lignes dans `scanner_address` | `Apple2Display.cpp:2455` (738, écrite le 2026-09-09) ; sans lignes vides : `:1745` (487-494, écrite le 2026-05-30) |
| `e1479b7a` = **`master`** | 2026-09-12 | commentaires IIgs (Mega II) ; comportement //e identique à `55a64a57` | `Apple2Display.cpp:534` (1170-1178, écrite le 2026-10-28) ; sans lignes vides : `:1800` (471-479, écrite le 2026-10-12) ; et les 7 de la ligne `55a64a57` |
| **aucune**, dans aucune numérotation | — | — | **3** : `Apple2Display.cpp:1510` (709-712), `:1642` (747), `:1800` (764-825) ; hors `src/` : `emulation-parity.md` (751-758) |

**Lecture.** Chaque citation suit le `master` du jour où elle a été écrite —
aucune ne date de `mame0287`, qui précède de sept semaines l'arrivée de
`scanner_address` dans ce fichier. Le défaut n'est donc pas un mélange
d'époques (une seule époque de comportement, `55a64a57` → `master`), mais
**deux règles de comptage** : le lot du 2026-05-12/13 (le passage d'audit
« MAME divergence » de `TODO.md`, commit `15be348d`) compte **sans les lignes
vides** sur `b3bc8ac9` — `use_page_2` « :359 », le décalage mono « 460-471 »,
le `switch` « 479-498 » et la table « 376-419 » tombent exactement — tandis
que ses numéros de la zone `hgr_update` (« :739 », « :747 », « 751-758 »,
« :799 ») sont trop hauts de 14 à 16 lignes dans cette numérotation et
n'existent dans aucune autre. C'est la **troisième occurrence** de l'outil qui
saute les lignes vides (après `mos6551` et `NTSC.cpp`).

**Pin à poser : `@ e1479b7a`**, numérotation réelle. Pour le //e, `master` se
comporte comme `55a64a57`. Ce que POM2 prend et ne prend pas, commit amont par
commit amont :

| Commit amont | Date | Contenu | Dans POM2 |
|---|---|---|---|
| `e516ef85` (PR #14554) | 2025-11-22 | FLASH //e = `frame_number() & 0x10` | **pris** (`Apple2Display.cpp:536-538`) |
| `67b96bf9` (PR #14590) | 2025-12-01 | le bus flottant est lu **avant** les effets vidéo de l'accès (`apple2e.cpp`, `apple2.cpp`) ; bascules ALTCHAR/80COL en course de faisceau | **bus flottant non pris → constat 2** ; course de faisceau ALTCHAR/80COL prise |
| `13184ea2` (PR #15073) = `mame0287` | 2026-03-08 | HBL relatif, VBL décalé de ~25 cycles, 80STORE en course de faisceau | 80STORE pris (côté adresse) ; phase VBL **VOULUE** (mesurée, `vbl_edge_phase`) |
| `55a64a57` (PR #15247) | 2026-04-24 | délais par commutateur ; `scanner_address` partagé | `scanner_address` **pris** (porté le 2026-05-15) ; délais **pris en partie** : AN3 −1 colonne, HIRES et 80STORE = PAGE2 ; **TEXT/MIXED non pris → constat 1** ; 80COL/ALTCHAR/AN2 non pris (voir pistes) |
| `d3c426df`, `d3989b3e`, `b3bc8ac9` | 2026-04 | plantage aux std80, HGR en mémoire réduite, texte ivelultr/dodo | sans objet |
| `7a73cf23` | 2026-05-25 | mise en forme | sans effet |
| `9fc40a64`, `e1479b7a` | 2026-09 | IIgs (Mega II) | sans objet |

---

## Rapport 1 — Citations

Ordre : DÉCALÉE, OK sans lignes vides, OK. Aucune citation ne tombe juste sur
`mame0287` ; « 0287:n » donne la position à la référence, « m:n » celle de
`master` (`e1479b7a`). Le fond de chaque plage a été relu : aucune ne prête à
MAME un code qu'il n'a pas.

| Fichier POM2:ligne | Citation | Verdict | Commentaire | Ligne corrigée |
|---|---|---|---|---|
| `Apple2Display.cpp:1510` | `709-712` | DÉCALÉE (aucune révision) | Constructeur de mots de `lores_update<Double>` : m:722-723, 0287:582-583 ; sur `7a73cf23` (`master` le jour de l'écriture), 709-712 est le début de la branche `else` et le constructeur est en 719-720. | `// [B] Sather, Understanding the Apple IIe, p. 8-30 et table 8.11 — LORES80 : motif aux puis motif carte mère, 7 points chacun ; un bloc aux montre la couleur carte mère tournée d'un bit à droite.` + `// [D] MAME apple2video.cpp:722-723 @ e1479b7a — mots de lores_update<Double>.` |
| `Apple2Display.cpp:1642` | `747` | DÉCALÉE (aucune révision) | `bit7_mask = m_dhires ? 0 : 0x80` : m:830 (commentaire « verified on h/w » en 829), 0287:691 ; 733 sans lignes vides sur `b3bc8ac9`. | `// [D] MAME apple2video.cpp:829-830 @ e1479b7a — DHIRES sans 80COL : pas de retard du bit 7 (« verified on h/w »).` |
| `Apple2Display.cpp:1681` | `479-485` | DÉCALÉE (sans vides, à 1 ligne près) | Sur `b3bc8ac9` sans vides, 479 est le `switch` et l'appel `artifact_color_lut[color_mode]` est en 486. Réel : m:548-556, 0287:408-416. | `// [D] MAME apple2video.cpp:548-556 @ e1479b7a — composite_color_mode 0/1 → artifact_color_lut[0/1].` |
| `Apple2Display.cpp:1684` | `:486-493` | DÉCALÉE (sans vides, à 2 lignes près) | `rotl4(w & 0x0f, x + is_80_column - 1)` : 495 sans vides sur `b3bc8ac9` ; m:557-565, 0287:417-425. | `// [D] MAME apple2video.cpp:557-565 @ e1479b7a — mode 2, filtre carré 4 bits.` |
| `Apple2Display.cpp:1800` | `764-825` (2ᵉ plage) | DÉCALÉE (aucune révision) | `hgr_update` (14 points par octet, retard du bit 7) : m:817-883, retard en 857-859. Sans vides, la plage ne tombe que sur `9fc40a64`, révision qui a vécu un jour avant l'écriture : fortuit. | `// [D] MAME apple2video.cpp:523-545, 817-883 @ e1479b7a — render_line (monochrome) et hgr_update.` |
| `Apple2Display.cpp:1745` | `487-494` | OK (rev `7a73cf23`, sans lignes vides) | `case 2:` en 487, `rotl4(…, x + is_80_column - 1)` en 494 ; écrite le 2026-05-30, quand `7a73cf23` était `master`. Réel : m:557-565. | `// [D] MAME apple2video.cpp:557-565 @ e1479b7a — rotl4(w & 0x0f, x + is_80_column - 1).` |
| `Apple2Display.cpp:1800` | `471-479` | OK (rev `55a64a57` → `master`, sans lignes vides) | Branche monochrome de `render_line` (`render_line_monochrome(…, fg, BLACK)` en 476). Réel : m:523-545. | voir la ligne `764-825` ci-dessus |
| `Apple2Display.cpp:2014` | `479-498` | OK (rev `55a64a57` → `b3bc8ac9` et `master`, sans lignes vides) | Exactement le `switch (composite_color_mode())`, du `else` au `break` du mode 2. Réel : m:546-566. | `// [D] MAME apple2video.cpp:546-566 @ e1479b7a — les trois modes couleur composites.` |
| `Apple2VideoDecode.h:57` | `376-419` | OK (rev `55a64a57` → `b3bc8ac9` et `master`, sans lignes vides) | La table commence en 376 et se ferme en 416. Réel : m:436-476, 0287:296-336. Les 256 octets sont identiques à ceux de POM2 (sonde P3) et n'ont jamais changé depuis `17704fc8` (2023-01-06). | `// [D] MAME apple2video.cpp:436-476 @ e1479b7a — artifact_color_lut[2][128] (PR #10792, benrg).` |
| `Memory.cpp:1620` | `124-201` | OK (rev `55a64a57` → `master`) | `scanner_address`, commentaire d'en-tête compris (m:123-201). **Absent de `mame0287`.** Le commentaire POM2 dit « v_clock [0..261] » alors que le code (et l'amont) couvre 311 lignes en PAL. Monte en grade B : Sather donne les trois règles et la source du octet. | `// [B] Sather, Understanding the Apple IIe, pp. 5-10/5-11 — base HBL = base affichée − $18, repli sur 128 octets, première adresse HBL lue deux fois ; p. 7-26 — le MPU lit l'octet vidéo de la CARTE MÈRE fetché en PHASE 1.` + `// [D] MAME apple2video.cpp:123-201 @ e1479b7a — scanner_address().` |
| `Memory.cpp:1664` | `140` | OK (rev `55a64a57` → `b3bc8ac9`) | `h_state = h_clock - (h_clock > 0)` : m:139 (−1 depuis `7a73cf23`). | `// [B] Sather IIe p. 5-10 — « The first address of HBL is always addressed twice consecutively ».` + `// [D] MAME apple2video.cpp:139 @ e1479b7a.` |
| `Memory.cpp:1673` | `149` | OK (rev `55a64a57` → `b3bc8ac9`) | `v_state = 256 + v_clock` : m:148. La phrase POM2 qui suit (« POM2's frame is 262 lines ») est périmée : le code retranche `kLinesPerFrame`, 262 ou 312. | `// [B] Sather IIe p. 3-17 et p. 8-17 — le compteur vertical se recharge à 011111010 (60 Hz) ou 011001000 (IOU 50 Hz) : 262 ou 312 états.` + `// [D] MAME apple2video.cpp:148-152 @ e1479b7a.` |
| `Apple2Display.cpp:534` | `1170-1178` | OK (rev `b3bc8ac9`, `7a73cf23`, `master`) | `frame_number() & 0x10` pour le //e, `(machine().time() * 4).seconds() & 1` pour le ][. 0287:1022-1027. Monte en B. | `// [B] Sather IIe p. 3-18, table 3.3 p. 3-20, p. 8-14 — FLASH bascule toutes les 16 recharges du balayeur : fréquence verticale / 32, 1,87 Hz à 60 Hz, durées ×1,2 avec un IOU 50 Hz.` + `// [B] Sather, Understanding the Apple II, p. 8-16 — le « 2 Hz text flasher » du ][.` + `// [D] MAME apple2video.cpp:1170-1178 @ e1479b7a.` |
| `Apple2Display.cpp:2455` | `738` | OK (rev `7a73cf23`) | `render_line(…, monochrome, Double)` : m:741, 0287:601. | `// [D] MAME apple2video.cpp:741 @ e1479b7a — is_80_column = Double dans lores_update.` |
| `Apple2Display.h:756` | `:788-791` | OK (rev `55a64a57` → `master`) | Mode texte Video-7 premier plan / fond. 0287:648-651. | `// [D] MAME apple2video.cpp:788-791 @ e1479b7a` |
| `Apple2Display.h:757` | `:571-583` | OK (rev `55a64a57` → `master`) | `render_line_color_array` : point allumé = quartet haut. 0287:431-443. | `// [D] MAME apple2video.cpp:571-583 @ e1479b7a` |
| `Apple2Display.cpp:1320` | `:788-791` | OK (rev `55a64a57` → `master`) | idem | idem |
| `Apple2Display.cpp:1322` | `:571-583` | OK (rev `55a64a57` → `master`) | idem | idem |

Hors `src/`, mêmes règles :

| Où | Citation | Verdict |
|---|---|---|
| `docs/audits/emulation-parity.md:30`, `docs/test_corpus.md:50` | `124-201` | OK (rev `55a64a57` → `master`) |
| `docs/audits/emulation-parity.md:30` | `460-471` | OK (rev `55a64a57` → `b3bc8ac9`, sans lignes vides) — la branche `monochrome_dhr_shift`, exacte ; réel m:529-540 |
| `docs/audits/emulation-parity.md:30` | `:751-758` | DÉCALÉE (aucune révision) — `empty_words` : m:835-843 |
| `DEV.md:1048` | `376-419` | OK (sans lignes vides), comme `Apple2VideoDecode.h:57` |
| `docs/backlog/display.md:119` | `1176-1178` | OK (`master`) |
| `CHANGELOG.md:2391`, `tests/display_dirty_skip_test.cpp:423` | `1170-1178` | OK (`master`) |
| `tests/video7_parity_smoke_test.cpp:88` | `906-930` | OK (rev `d3989b3e` → `master`) |

Citations par nom de symbole, relues sur `master` :

| Où | Citation | Verdict | Commentaire |
|---|---|---|---|
| `Apple2Display.cpp:1375` | `apple2_palette[]`, « calibrated against real Apple II hardware » | **INEXACTE (provenance)** | Les 16 valeurs sont identiques (m:987-1005, stables depuis `017b53b2`, 2019-12-09), mais MAME les tire d'une feuille de calcul (« Latest Apple II NTSC palette from "Apple II Video Display Theory" », m:983-986) : un modèle calculé, pas une capture. Ligne : `// [D] MAME apple2video.cpp:983-1005 @ e1479b7a — palette tirée de la feuille « Apple II Video Display Theory ».` + `// [F] Aucune mesure colorimétrique ; à trancher avec un colorimètre sur un moniteur NTSC.` |
| `Apple2Display.cpp:1573-1574`, `Apple2VideoDecode.h:29` | « PR #10773 by benrg — the gold-standard algorithm calibrated against real Apple II hardware » | **INEXACTE (provenance)** | #10773 (`823422ef`, 2023-01-02) apporte le doubleur de bits et le retard du bit 7 ; la table d'artefacts vient de #10792 (`17704fc8`, 2023-01-06, benrg aussi), dont le commentaire dit que ses cas difficiles sont « resolved somewhat arbitrarily ». Rien n'y est calibré sur du matériel : grade D, et F pour la teinte. |
| `Memory.cpp:1659`, `Apple2Display.cpp:1277` | `use_page_2() = m_page2 && !m_80store` | OK | m:414. Monte en B : Sather IIe p. 5-11, « A13 is equivalent to (PAGE2 · 80STORE')' ». |
| `Apple2Display.cpp:1279` | `text_update` ne lit que `m_ram_ptr` en 40 colonnes | OK | m:784. |
| `Apple2Display.cpp:1486` | DLGR : `rotl4(NIBBLE(aux),1)` puis `NIBBLE(main)` | OK | m:697, 703. Monte en B : Sather p. 8-30. |
| `Apple2Display.cpp:827`, `:1931` | `is_80_column = 1` en DHGR | OK | m:520, 944. |
| `Apple2Display.cpp:1935` | Video-7 `rgbmode == 3`, quartet tourné de 1 | OK | m:975. |
| `Apple2Display.cpp:2299`, `Apple2VideoDecode.h:100` | `bit7_mask = m_dhires ? 0 : 0x80` | OK | m:830. |
| `Apple2Display.cpp:2452` | `is_80_column` = `Double` dans `lores_update` | OK | m:741. |
| `LeChatMauveCard.cpp:87` | `device_reset` : `m_dhires = false` | OK | m:112. |
| `LeChatMauveCard.h:56` | sémantique `rgb_monitor` | OK | m:418. |
| `VidHDCard.cpp:108` | `fillmode_init[scb & 0x1f]`, essais de John Brooks | OK | m:1072-1080, depuis `5656c0ea` (2020-04-17). |
| `hgrpaint/HgrPaintModel.h:169` | filtre carré DHGR | OK | m:557-565 ; la conséquence (`rotl4(v, 1)` par groupe aligné) est épinglée par `dhgr_paint_model`, non redérivée ici. |

---

## Rapport 2 — Constats

### Bascule TEXT/MIXED au milieu d'une ligne : une colonne trop tôt

```
Fichier      : src/Apple2Display_Beam.cpp:177-205 (beamColForEvent : TextMode et
               MixedMode restent sur le -24 calibré pour PAGE2) ; mapping commun
               src/Apple2Display.cpp:358-405, src/CpuClock.h:282.
Amont        : apple2video.cpp:235-323 @ e1479b7a (txt_w / mix_w → delayed_update(3),
               scr_w → delayed_update(2), res_w → 2, an3_w / a80col_w / altcharset_w → 1,
               m_delay_bias = 1 sur //e l. 104) ; AppleWin Video.cpp:218-221 @ 5b7360e1
               (« Only 1-cycle delay for VF_TEXT & VF_MIXED mode changes (GH#656) »).
Gravité      : MINEUR
Confiance    : CONFIRMÉ — lu dans les deux sources, sonde P1 ; et deux jeux de photos
               de matériel publiés, indépendants, qui disent la même chose.
```

**Divergence.** Sur le matériel, une bascule TEXT↔GRAPHICS ou MIXED atteint
l'écran **un cycle après** une bascule PAGE2 faite au même cycle :

- [A] tomcw, AppleWin GH#654/#656 (2019), sur un //e PAL réel : après la
  synchronisation VBL de Sather, `LDA $C055 / LDA $C054` en ligne 0 et
  `LDA $C050 / LDA $C051` en ligne 7, à la même phase ; le rectangle GR de la
  ligne 7 est décalé d'une colonne par rapport à celui de PAGE2. AppleWin en a
  tiré le retard d'un cycle sur TEXT et MIXED seulement. Le cas avait été
  ouvert par un courriel d'Arnaud portant sur le « Little Text Window » de
  Sather.
- [A] arekkusu42, tableau « VIDMODE LATENCY » joint à MAME PR #15247 : latences
  en cycles PH0, colonne //e = valeur − 1 : TEXT 3−1 = 2, MIXED 3−1 = 2,
  PAGE2 2−1 = 1, 80STORE 2−1 = 1. TEXT et MIXED portent l'astérisque
  « glitching, approximate ». MAME l'implémente sous la forme
  `delayed_update(3)` contre `delayed_update(2)`.
- [B] Sather IIe p. 8-12 et p. 8-38 donne le mécanisme : GR+1 et GR+2 sont le
  signal GR retardé d'une et deux horloges du balayeur. GR+2 décide de la
  ROM vidéo en GRAPHICS ou TEXT ; une bascule de page ne fait que changer
  l'adresse du prochain fetch. Le livre ne chiffre pas l'écart d'un
  commutateur à l'autre : la preuve chiffrée reste celle des photos.

POM2 place TEXT et MIXED sur la même colonne que PAGE2 (`hpos − 24`). Son
commentaire laisse la question ouverte faute de mesure : « TextMode /
MixedMode are left at -24 too: no measured title exercises a mid-line
text/graphics flip … revisit only with a measured case ». Les deux mesures
ci-dessus sont ce cas mesuré.

L'ancrage n'est pas en cause. PAGE2 tombe sur la même colonne chez POM2
(mesuré sur MAD EFFECT) et chez MAME, et AN3 une colonne avant, des deux
côtés (OLDSKOOL chez POM2, la photo `C05E` chez MAME). Seul l'écart relatif
de TEXT et MIXED manque.

**Scénario.** Le « Little Text Window » de Sather (*UTAIIe* p. 3-26, fig. 3.12)
ou les essais de tomcw : un accès `$C050` puis `$C051` au cycle horizontal 40
d'une ligne, le même cycle qu'un `$C055` sur une autre ligne. Sur le matériel,
la fenêtre GR commence une colonne à droite de la fenêtre PAGE2 (colonne 17
contre 16). Dans POM2, les deux commencent en colonne 16 (sonde P1). Le bord
gauche et le bord droit d'une fenêtre texte découpée mi-ligne glissent
ensemble de 7 points vers la gauche. Une démo French Touch qui bascule
TEXT↔HGR mi-ligne (`docs/test_corpus.md`, ligne « French Touch productions »)
voit sa coupure déplacée d'autant.

**Observation.** Sonde P1 (`/tmp/audit_apple2video/probe_latency.cpp`). Les
tests `horizontal_split_560` et `horizontal_split_composite` figent
aujourd'hui la coupure TEXT à `hpos 45 → col 21` (« mapping is `hpos - 24` »).
Un correctif les fera passer à 22, et c'est attendu.

### La lecture d'un commutateur `$C050-$C057` rend le bus flottant du mode d'APRÈS la bascule

```
Fichier      : src/Memory.cpp:1183-1223 (arbre ; 1167-1207 à af7dd56f) — l'état
               d'affichage est modifié, puis `return isWrite ? 0 : floatingBus();`
Amont        : apple2e.cpp:2080 et apple2.cpp:425 @ master du 2026-11-01
               (2054 et 398 @ mame0287) : `const u8 uFloatingBus = read_floatingbus(); //
               video side-effects latch after reading` — PR #14590, 67b96bf9, 2025-12-01.
Gravité      : MINEUR
Confiance    : CONFIRMÉ — lu dans les deux sources, sonde P2, et une mesure publiée.
```

**Divergence.** Le balayeur lit la RAM pendant la PHASE 1 et le MPU lit cet
octet de la carte mère ; la bascule, elle, n'a lieu qu'en PHASE 0 du même
cycle ([B] Sather IIe pp. 5-33 à 5-36 et 7-26). L'octet rendu par
l'accès qui bascule est donc celui du mode d'avant. [A] Ryan Schmidt l'a
mesuré sur un //e non amélioré réel (TomHarte/CLK#1204) : en TEXT + HIRES,
`LDX $C050` rend un octet de la page texte et `LDY $C050` un octet HGR. MAME
lit le bus avant les effets depuis `67b96bf9`. POM2 applique la bascule puis
lit le bus, et rend le mode d'après dans les deux cas — comme Virtual ][ et
OpenEmulator dans le même rapport. AppleWin fait comme POM2 : lu, non
exécuté. Dans `Video.cpp:240-242`, `MemReadFloatingBus` est appelé après
`NTSC_SetVideoMode`, et l'adresse du bus suit aussitôt `g_uNewVideoModeFlags`.

**Scénario.** Le programme `instant.s` de CLK#1204 : page texte remplie de
`$A0`, HGR de `$7F`, TEXT + HIRES actifs, puis `LDX $C050 / LDY $C050`. Le
matériel rend `X = $A0`, `Y = $7F` ; POM2 rend `$7F`, `$7F` (sonde P2, //e et
][+). Même effet pour `LDA $C055` depuis la page 1 (POM2 rend déjà la page 2).
Seul le premier octet d'une suite est faux. Un verrouillage par vapeur qui
échantillonne avec le même accès que celui qui bascule (une boucle qui
alterne `$C050`/`$C051` pour lire les deux zones, par exemple) se trompe à
chaque tour. La coupure DROL (`LDA $C050` répété, déjà en graphique) et le
test `vapor_lock` (`$C058`) ne sont pas touchés.

**Observation.** Sonde P2 (`/tmp/audit_apple2video/probe_c05x_order.cpp`).
Aucun test du dépôt ne fige l'ordre actuel (aucun `memRead(0xC05x)` vérifié
dans `tests/`). Le même ordre vaut pour `$C05E/$C05F`, sans effet puisque AN3
ne change pas l'adresse lue.

---

## `AMONT FAUX`

**Aucun.** Sur les deux constats, MAME est du côté du matériel. Deux points où
**AppleWin** s'écarte de Sather sont dans les pistes (lecture du bus par
l'accès qui bascule, et vue CPU de la mémoire dans un cas limite). Ils ne
justifient aucune entrée ici : POM2 suit déjà MAME sur le second et
partage l'erreur sur le premier.

---

## Sondes

Compilées contre l'arbre de travail, sur le modèle de
`add_executable(test_drol_pageflip_render …)` et `test_floatingbus_page2` :

```
clang++ -std=c++17 -O1 -Isrc -Itests -Ibuild/generated /tmp/audit_apple2video/<sonde>.cpp \
  [src/Apple2Display.cpp src/Apple2Display_ChatMauve.cpp src/Apple2Display_Beam.cpp \
   src/AppleWinNtsc.cpp src/LeChatMauveCard.cpp] build/libpom2_core_test.a \
  -framework CoreAudio -framework AudioToolbox -framework AudioUnit
```

**P1 — colonne d'effet d'un commutateur mi-ligne** (`probe_latency.cpp`,
`Apple2Display::forEachBeamSegment`, rendu RGBA et composite communs ; la
bascule PAGE2 est faite en aller-retour, sans quoi l'heuristique DROL
l'applique à toute la trame) :

```
Accès au cycle horizontal 40 de la ligne 10 (NTSC)
commutateur        POM2 col   MAME col   écart     photo //e (cycles)
TEXT  ($C050/51)   16         17         -1         3*-1 = 2
MIXED ($C052/53)   16         17         -1         3*-1 = 2
PAGE2 ($C054/55)   16         16         +0         2-1   = 1
HIRES ($C056/57)   16         16         +0         1*a-1 = 0 ("should be 2?")
80COL ($C00C/0D)   16         15         +1         1*b-1 = 0
ALTCH ($C00E/0F)   16         15         +1         1-1   = 0
AN3   ($C05E/5F)   15         15         +0         1-1   = 0
80STO ($C000/01)   16         16         +0         2-1   = 1
```

« MAME col » = `(hpos − 25) + (cycles − m_delay_bias)`, d'après
`delayed_update` ; l'ancre PAGE2 est la même que celle de POM2.

**P2 — octet rendu par l'accès qui bascule** (`probe_c05x_order.cpp`) :

```
//e  LDX $C050 -> 7F   LDY $C050 -> 7F   (matériel : texte puis HGR = A0|42 puis 7F)
//e  LDA $C055 -> 55   LDA $C055 -> 55   (ordre MAME : page 1 puis page 2 = 7F puis 55)
][+  LDX $C050 -> 7F   LDY $C050 -> 7F   (matériel : texte puis HGR = A0|42 puis 7F)
][+  LDA $C055 -> 55   LDA $C055 -> 55   (ordre MAME : page 1 puis page 2 = 7F puis 55)
```

**P3 — adresse du balayeur** (`probe_fbus.cpp`) : `Memory::floatingBus(cycle)`
contre `scanner_address()` de `master` recopié à l'identique (et la règle
`model::II` du `$1000` en HBL), sur chaque cycle d'une trame, NTSC et PAL,
//e et ][+, les 16 combinaisons HIRES/TEXT/MIXED/PAGE2 × 80STORE, deux
remplissages aléatoires de la RAM :

```
floatingBus vs scanner_address(MAME master): 3581760 échantillons, 0 écarts
```

Tables (scripts Python sur les blobs) : `artifact_color_lut` 256/256 octets
identiques et inchangés depuis `17704fc8` ; palette 16/16 identique, stable
depuis `017b53b2`.

---

## Pistes examinées et écartées

- **Formule du balayeur, NTSC et PAL** : P3, 0 écart. Les règles ont un grade B :
  HBL = base − $18 et repli sur 128 octets (Sather IIe p. 5-10 ; déjà cité
  dans `CpuClock.h`) ; première adresse HBL lue deux fois (p. 5-10) ; recharge
  verticale à `011001000` sur IOU 50 Hz (p. 3-17, p. 8-17).
- **Le bus flottant lit la RAM principale, jamais l'aux** : Sather IIe p. 7-26
  (« the MPU will read the motherboard video data from PHASE 1 video
  scanning ») et pp. 5-35/5-36 (la donnée aux n'est pas passée au bus de
  données en PHASE 1, EN80' haut). POM2 = MAME = Sather. AppleWin lit par
  `MemGetMainPtr(0)`, c'est-à-dire la vue CPU de la page 0. Si la page 0 est
  la principale et RAMRD actif, `mem + $0400` est la copie aux : AppleWin
  s'écarte alors de Sather. Lu, non exécuté.
- **Le `$1000` en HBL est propre au ][/][+** : Sather IIe annexe I p. I-4,
  point 5 (« HBL gating is not included in the Apple IIe RAM addressing »).
  POM2 conditionne sur `!iieMode`, MAME sur `model::II`.
- **Période du FLASH** : 16 trames par demi-période sur //e, en NTSC comme en
  PAL (Sather p. 3-20 : fréquence verticale / 32, soit 1,56 Hz en 50 Hz).
  Sur le ][, 2 Hz exacts, le « 2 Hz text flasher » de Sather (*UTA2* p. 8-16).
  La PR MAME #14554 avance ~2,182 Hz d'après les figures 8.6/8.7 d'*UTA2*,
  que l'OCR ne restitue pas. C'est un 555, d'une tolérance comparable, et sa
  phase n'est pas définie (`docs/backlog/display.md`, clos le 2026-10-24).
- **Instant de bascule du FLASH** : Sather le place juste après la recharge du
  balayeur (ligne 256, en VBL), POM2 à la frontière de trame (avant la ligne
  0). Dans les deux cas c'est hors de l'image : même trame visible.
- **Routage des caractères** : ALTCHAR, MouseText en `$40-$5F`, minuscules
  inversées en `$60-$7F` et clignotement de `$40-$7F` sont identiques à
  `get_text_character` (m:586-661). Le mécanisme est celui de Sather
  pp. 8-13/8-14. Le //e non amélioré lit ce que contient sa ROM de 4 Ko
  (`apple2e_char_2k.rom` fait 4096 octets).
- **Ordre aux puis principal en 80 colonnes, DHGR et DLGR** : Sather p. 8-38
  (LDPS' tombe en PHASE 0 pour le motif aux, puis en PHASE 1 pour celui de la
  carte mère) et p. 8-30. POM2 = MAME.
- **Frontière aux/principal du DLGR** (piste ouverte de la tranche NTSC) :
  Sather p. 8-30 décrit deux moitiés de 7 points qui commencent chacune au
  bit 0 du quartet (« 0010 … 0100 010 »). C'est le constructeur de mots de
  MAME et de POM2. L'arbitre que la tranche NTSC cherchait penche donc pour
  MAME/POM2 ; je n'ai pas relancé la simulation d'AppleWin pour l'écrire
  comme un `AMONT FAUX` contre lui.
- **Image 80 colonnes décalée de 7 points à gauche sur //e** : la note b du
  tableau d'arekkusu (« //e shifts 80VID to left ») rejoint le contournement
  d'AppleWin (« start 7x 14M pixels to the left on real h/w ») et la chronologie
  de Sather (le motif aux est chargé une demi-période avant celui de la carte
  mère). POM2 et MAME alignent 40 et 80 colonnes ; la phase couleur est la
  même (−7 ≡ +1 mod 4). C'est un écart de géométrie de l'image entière, sans
  programme du corpus où il se voie : **latent**.
- **80COL et ALTCHAR mi-ligne** : MAME (photo) les place une colonne avant
  PAGE2, AppleWin sur la même colonne, comme POM2 (P1). La note b lie la
  mesure de TXT80 au décalage ci-dessus, et une seule source chiffre ALTCHAR.
  MAME et AppleWin se contredisent, sans arbitre de rang B : non tranché.
- **HIRES mi-ligne** : POM2 = MAME = PAGE2 ; la photo hésite (« 1*a, //e, ][+
  should be 2? ») ; MAD EFFECT l'a confirmé côté POM2. Sain.
- **AN3 mi-ligne, une colonne avant PAGE2** : POM2 = MAME = photo `C05E`. Sain.
- **Masque du bit 7 (DHIRES sans 80COL)** : appliqué de la même façon dans les
  quatre chemins (`Apple2Display.cpp:1646`, `:1809`, `:2302`,
  `buildHgrWordRow`) ; `Memory.cpp:1244` ne pose `dhgr` qu'en mode //e. Sain.
- **Table d'artefacts et palette** : identiques à l'octet (voir Sondes).
- **Texte Video-7 premier plan/fond** : caractère lu en principale, couleurs
  en aux à la même adresse, quartet haut = premier plan, glyphe doublé sur
  14 points : conforme à m:788-791 et 571-583.
- **`use_page_2()` en texte, lo-res, HGR et bus flottant** : conforme, grade B
  (Sather p. 5-11).
- **Mode mixte** : bande texte à partir de la ligne 160 dans les deux, et le
  balayeur repasse en adresses texte sur `Mixed · V4 · V2`. Sain.
- **Phase du front VBL, horloge PAL, DHGR/AN3 effacé au reset** : **VOULUES**,
  non rouvertes. Note pour l'horloge PAL : Sather IIe p. 3-17 donne une
  ligne de « 64.000 [µs] from 14.25 » MHz, soit 65 × 15 625 = 1 015 625 Hz
  exactement, la valeur de POM2.
- **`empty_words` de MAME** (support de RAM vide qui flotte à 1) : POM2 a
  toujours au moins 48 Ko. Latent, sans objet.
- **Couleur de la bande texte du mode mixte** : choix documenté (tranche NTSC).

---

## Bilan

- **Citations examinées** : 18 plages numérotées sous `src/` (12 au grep et
  6 inline), 9 hors `src/`, une quinzaine par nom de symbole.
- **Verdicts (les 18)** :
  - OK (rev X), numérotation réelle : 9 ;
  - OK (rev X), sans lignes vides : 4 ;
  - DÉCALÉE : 5, dont 2 sans lignes vides à 1-2 lignes près et 3 qui ne
    tombent juste sur aucune révision ;
  - OK sur `mame0287` : 0 ; ABSENTE 0 ; INEXACTE 0 ; NON-PINNÉE 0.

  Hors `src/` : 6 OK, 2 OK sans vides, 1 DÉCALÉE. Par symbole : 2 INEXACTES de
  provenance (palette et table « calibrées sur le matériel »), le reste OK.
- **Constats** : CRITIQUE 0 · MAJEUR 0 · **MINEUR 2** (TEXT/MIXED mi-ligne une
  colonne trop tôt ; bus flottant lu après la bascule). Les deux sont
  confirmés par sonde et appuyés par des mesures publiées (grade A).
  **AMONT FAUX : 0.**
- **Révision amont** : pas de pin. Référence `mame0287` (= `13184ea2`), sur
  laquelle aucune citation ne tombe. Une seule époque de comportement,
  `55a64a57` → `e1479b7a` (`master`), citée en deux numérotations.
  **Pin recommandé : `@ e1479b7a`**, avec le tableau des commits pris / non
  pris ci-dessus (#14590 et la partie TEXT/MIXED de #15247 non pris).
- **Montées en grade** : le balayeur, sa source (carte mère), la recharge
  PAL, le FLASH, `use_page_2` et le DLGR passent de D à B (Sather IIe
  pp. 3-17, 3-20, 5-10, 5-11, 7-26, 8-14, 8-30 ; *UTA2* p. 8-16) ; les lignes
  sont dans le Rapport 1.

**Ce que je n'ai pas pu vérifier :**

- **Les figures de Sather** : je n'ai lu que le texte OCR d'archive.org. Les
  chronogrammes (fig. 8.5, 8.17) et les figures 8.6/8.7 d'*UTA2* (fréquence
  du 555) n'ont pas été vus en image.
- **Les photos des issues AppleWin GH#654/#656** : vues seulement à travers le
  texte des issues. Le tableau d'arekkusu (PR #15247) a été téléchargé et lu en
  image ; je n'en ai pas refait la mesure.
- **Le //c et le //c+** : la tranche porte sur le //e (Sather). Les latences des
  photos //c PAL n'ont pas été comparées ligne à ligne.
- **L'ordre du bus flottant sur ][+** : même mécanisme, mais la mesure publiée
  porte sur un //e non amélioré.
- **AppleWin** n'a pas été exécuté (bus flottant au basculement, vue CPU avec
  RAMRD) : lu dans le code seulement.
- **DIX lui-même** n'a pas été lancé. Je n'ai pas identifié le titre précis du
  corpus qui bascule TEXT mi-ligne ; seul `docs/test_corpus.md` affirme
  qu'il en existe.
- **L'origine des numéros de la zone `hgr_update`** du lot de mai (« 747 »,
  « 751-758 », « 739 », « 799 ») : aucune règle de comptage ne les reproduit.
- Le blob du commit de renommage `cf11b333` (illisible aux deux chemins), et
  l'historique antérieur à 2015-11 (`src/mess/`). Aucune citation n'en relève :
  les structures citées datent toutes de 2023 ou après.
