# Audit de parité — citations d'AppleWin `NTSC.cpp` (2026-11-01)

Tranche 10 du brief `docs/audits/BRIEF-parite-amont.md`. Dépôt amont :
**AppleWin** (`source/NTSC.cpp`), pas MAME. Toutes les citations
`NTSC.cpp:<lignes>` sous `src/` :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep 'NTSC\.cpp:'
```

**23 citations** : 17 dans `AppleWinNtsc.cpp` (le pipeline couleur CPU
`ColorAppleWin`), 5 dans `VidHDCard.h`, 1 dans `VidHDCard.cpp` (le balayage
Super Hi-Res de la VidHD). Le grep en rate **7** autres, écrites sans nom de
fichier collé ou hors de `src/` ; elles sont datées avec les autres mais hors
du compte : `VidHDCard.h:21` (« `NTSC.cpp` `updateScreenSHR`, :1866-1895 »),
`VidHDCard.h:45` (« + :1872-1887 »), `DEV.md:1702` (`:905-912` +
`:1866-1895`), `DEV.md:2083` (`NTSC.cpp:115-132`), `DEV.md:2101`
(`NTSC.cpp:331`), `docs/graphics_modes_comparison.md:343` (`NTSC.cpp:115-132`).

Arbitres de rang supérieur consultés : le **brevet de la vidéo Apple II**,
US 4,136,359 (Wozniak, déposé le 1977-04-11, texte Google Patents). Je n'ai
**pas** pu lire Sather (*Understanding the Apple IIe*, ch. 8).

Aucun fichier de code n'a été modifié. Sondes, révisions amont et scripts :
`/tmp/audit_ntsc/` (sorties collées plus bas).

---

## Révision amont

**Pas de pin.** Ni `AppleWinNtsc.h/.cpp`, ni `VidHDCard.h/.cpp`, ni `DEV.md`
§ AppleWin NTSC / § VidHD ne nomment une révision. Verdicts rendus contre
`master` d'AppleWin = **`3e8054b462`** (2026-07-26), dernier commit du fichier
au 2026-11-01.

Historique complet récupéré : **168 révisions** (API GitHub
`commits?path=source/NTSC.cpp`, deux pages), de `e681c97c3b` (2014-12-31) à
`3e8054b462`. Datation scriptée (`date.py`) : une ou plusieurs ancres regex par
citation (le code décrit, pas son numéro), une révision « tombe juste » quand la
plage citée les contient toutes.

### Le résultat principal : les citations d'`AppleWinNtsc.cpp` sont numérotées SANS LES LIGNES VIDES

En numérotation réelle, **aucune** des 17 citations d'`AppleWinNtsc.cpp` ne
tombe juste sur aucune des 168 révisions (trois touchent fortuitement des
révisions de janvier 2015, voir le tableau). Mais les écarts avec `master`
croissent régulièrement (+8 à la ligne 30, +36 à la ligne 115, +138 à la ligne
781, +179 à la ligne 974) : c'est la signature d'une numérotation qui saute des
lignes. Renumérotée **en retirant les lignes strictement vides** (les lignes
faites seulement d'espaces ou de tabulations comptent), `master` contient
exactement ce que POM2 décrit : `#define NTSC_NUM_PHASES` à 104,
`#define CHROMA_ZEROS` … `#define SIGNAL_1` à 115-132,
`static void initChromaPhaseTables` à 781, `phi = (phase * RAD_90) + CYCLESTART`
à 791, `#define I_TO_R` à 833, `static real initFilterChroma` à 941, son
`-x[0] + x[2]` à 946, etc. (`date_nb.py`). **16 sur 17** tombent juste ainsi ;
la dix-septième (`:65`, `45-52`) s'arrête une ligne avant le
`#define CYCLESTART`, qui est en 53.

C'est **le même défaut que la tranche 5** (`mos6551`, « numérotation sans
lignes vides ») : l'outil qui a produit ces numéros comptait les lignes non
vides. Les trois citations de documentation qui décrivent ce même pipeline
(`DEV.md:2083`, `DEV.md:2101`, `graphics_modes_comparison.md:343`) ont le même
défaut. Les citations de la VidHD, écrites cinq mois plus tard, sont en
numérotation réelle.

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste (/23) |
|---|---|---|---:|
| `e681c97c3b` → `816e22524f` (≈ 110 rév.) | 2014-12 → 2021-01 | Création, nettoyages de 2015, mode TV (#650), RGB (#819), PAL (#763) | 0 vraies. Coïncidences en numérotation réelle : `:130` (833-841) sur `ce94b59663` seul, `:187` (839-881) sur 13 rév. de 2015-01 à 2016-03, `:198` (882-907) sur 9 rév. de 2015-01 ; en numérotation sans vides, `:65` (45-52) sur `75d56cad97`→`816e22524f`. Plages larges, voisines incohérentes : écartées |
| `443545b0f6` → `3e8054b462` | 2021-11 → 2026-07 | SHR VidHD ajouté (« 320x200 again doubled », l. 109) | `VidHDCard.h:56` (`:109`) |
| `9d312230e5` → `bc7c6b2150` → `8193cc1fd1` → **`3e8054b462` = `master`** | 2025-03-02 → 2026-07-26 | +15 lignes de débogueur (#1384) qui fixent la numérotation actuelle ; puis PAL Colour TV (#1443, l. 2075+), `(void)`→`()`, `BOOL`→`bool` (substitutions sur place) | **4** en réelle (`VidHDCard.cpp:97`, `VidHDCard.h:45`, `:97`, et `:56`) + `VidHDCard.h:21`, `:45` (+:1872-1887), `DEV.md:1702` ; **16** d'`AppleWinNtsc.cpp` en numérotation sans vides (+ `DEV.md:2083`, `:2101`, `graphics_modes:343`) |
| **aucune**, dans aucune numérotation | — | — | `VidHDCard.h:99` (`:1877`) et `:100` (`:1885`) : `0x9D00`/`0x9E00` sont en 1879/1887 sur `master` ; aucune révision ne les a eues en 1877/1885 (1761, 1765, 1831, 1833, 1838, 1844, 1855, 1859, 1861, 1864, 1879) |

**Une seule époque amont**, celle de `master`. Le port `ColorAppleWin` date du
2026-05-28 (`59171d4f`), quand `master` était `bc7c6b2150` ; la VidHD du
2026-10-16 (`ac088370`), sur `3e8054b462`. Ces deux révisions et `9d312230e5`
ont la même numérotation pour toutes les lignes citées (le correctif
`bc7c6b2150` ajoute ses lignes après la ligne 2070). C'est l'inverse des
tranches 1-3 : pas d'époques mêlées, mais une **règle de comptage** fausse.

**Correctifs amont postérieurs au port** : `8193cc1fd1` (listes `(void)`) et
`3e8054b462` (`BOOL` → `bool`), sans effet. Le seul correctif de comportement
récent, `bc7c6b2150` (2025-10-20, GH#1443 : en 50 Hz, ne pas tuer la salve de
couleur instantanément en Colour TV), est **antérieur** au port et **non pris**
— POM2 ne modélise pas la salve de couleur dans ce chemin (voir les pistes,
« bande texte du mode mixte »).

Révision à épingler : `@ 3e8054b462`, en **numérotation réelle**.

---

## Rapport 1 — Citations

Verdicts en numérotation réelle sur `3e8054b462`. « Sans vides » veut dire : la
plage tombe juste sur `3e8054b462` si l'on retire les lignes vides. Le contenu
de chaque plage a été relu ; aucune ne décrit autre chose que ce que POM2
prétend, d'où aucun `INEXACTE`. La colonne « Ligne corrigée » donne la forme du
dépôt ; là où le brevet porte la même règle, la citation monte en grade B.

| Fichier POM2:ligne | Citation | Verdict | Commentaire | Ligne corrigée |
|---|---|---|---|---|
| `VidHDCard.h:99` | `NTSC.cpp:1877` | DÉCALÉE | `0x9D00 + g_nVideoClockVert` (SCB) est en **1879** ; 1877 est `uint32_t a = pAux[0]`. Aucune révision ne l'a jamais eu en 1877. | `// [D] AppleWin NTSC.cpp:1879 @ 3e8054b462 — SCB en $9D00 + ligne (banque aux).` |
| `VidHDCard.h:100` | `NTSC.cpp:1885` | DÉCALÉE | `0x9E00 + paletteSelectCode * 16 * 2` est en **1887**. Même remarque. | `// [D] AppleWin NTSC.cpp:1887 @ 3e8054b462 — palettes en $9E00 + 32·p.` |
| `AppleWinNtsc.cpp:65` | `45-52` | DÉCALÉE (sans vides, à 1 ligne près) | Angles : réels 58-68 ; sans vides 45-53, le `#define CYCLESTART` actif est en 53, hors plage (52 est sa version commentée). Note : AppleWin les écrit en **`float`** (`PI 3.1415926535898f`, `RAD_45 PI*0.25f`) ; POM2 en `double` (voir sonde P1). | `// [D] AppleWin NTSC.cpp:58-68 @ 3e8054b462 — PI, RAD_45/90, CYCLESTART = 45°, en float.` |
| `AppleWinNtsc.cpp:35` | `781-907` | DÉCALÉE (sans vides) | `initChromaPhaseTables` : réel **919-1080** (la table TV s'achève en 1068). | `// [D] AppleWin NTSC.cpp:919-1080 @ 3e8054b462 — initChromaPhaseTables().` |
| `AppleWinNtsc.cpp:152` | `781-907` | DÉCALÉE (sans vides) | Idem. | idem |
| `AppleWinNtsc.cpp:39` | `974-981` | DÉCALÉE (sans vides) | `initFilterSignal` : réel **1153-1162**. | `// [D] AppleWin NTSC.cpp:1153-1162 @ 3e8054b462 — initFilterSignal(), passe-bas 2 pôles.` |
| `AppleWinNtsc.cpp:40` | `941-948` | DÉCALÉE (sans vides) | `initFilterChroma` : réel **1111-1120**. | `// [D] AppleWin NTSC.cpp:1111-1120 @ 3e8054b462 — initFilterChroma(), passe-bande.` |
| `AppleWinNtsc.cpp:41` | `952-970` | DÉCALÉE (sans vides) | `initFilterLuma0/1` : réel **1125-1148**. | `// [D] AppleWin NTSC.cpp:1125-1148 @ 3e8054b462 — initFilterLuma0/1().` |
| `AppleWinNtsc.cpp:106` | `941-981` | DÉCALÉE (sans vides) | Les quatre filtres et leurs `static` (état continu sur toute la construction) : réel **1111-1162**. « Fresh … per rebuild — the canonical first call state » : AppleWin ne remet pas l'état à zéro à un second appel (`NTSC_VideoInitChroma`, 2457), mais la sonde P3 montre que la table obtenue est identique. | `// [D] AppleWin NTSC.cpp:1111-1162 @ 3e8054b462 — filtres à état static, jamais remis à zéro.` |
| `AppleWinNtsc.cpp:119` | `946` | DÉCALÉE (sans vides) | `-x[0] + x[2]` : réel **1117**. L'analyse « passe-bande centré sur fs/4 » est juste : pôles r = 0,855, θ ≈ 44° au double échantillonnage = 3,58 MHz. | `// [D] AppleWin NTSC.cpp:1117 @ 3e8054b462 — zéro en x[0] inversé : passe-bande.` |
| `AppleWinNtsc.cpp:61` | `104` | DÉCALÉE (sans vides) | `NTSC_NUM_PHASES 4` : réel **137**. Le « 4 » est un fait du silicium : monte en B. | `// [B] US 4,136,359 (Wozniak) — horloge 14,31818 MHz, sous-porteuse 3,579545 MHz : 4 échantillons par cycle.` + `// [D] AppleWin NTSC.cpp:137 @ 3e8054b462 — NTSC_NUM_PHASES.` |
| `AppleWinNtsc.cpp:63` | `105` | DÉCALÉE (sans vides) | `NTSC_NUM_SEQUENCES 4096` : réel **138**. | `// [D] AppleWin NTSC.cpp:138 @ 3e8054b462 — 4096 historiques de 12 bits.` |
| `AppleWinNtsc.cpp:71` | `115-132` | DÉCALÉE (sans vides) | Coefficients : réel **151-170**. Chiffres identiques ; « verbatim » omet que ce sont des littéraux **`float`** (`7.438011255f`…). Le gain unité en continu annoncé se vérifie : 4/(7,6145 × 0,5253) = 1,000 et 4/(13,713 × 0,2917) = 1,000. | `// [D] AppleWin NTSC.cpp:151-170 @ 3e8054b462 — gains et pôles (littéraux float).` |
| `AppleWinNtsc.cpp:130` | `833-841` | DÉCALÉE (sans vides) | Matrice YIQ→RGB : réel **969-990** (commentaire, `#define`, application). | `// [D] AppleWin NTSC.cpp:969-990 @ 3e8054b462 — YI'Q'→RGB, 0.956/0.621, -0.272/-0.647, -1.105/1.702.` |
| `AppleWinNtsc.cpp:160` | `791` | DÉCALÉE (sans vides) | `phi = (phase * RAD_90) + CYCLESTART` : réel **930**. Le raisonnement (24 × 45° = 1080° ≡ 0) est juste. | `// [D] AppleWin NTSC.cpp:930 @ 3e8054b462 — phi semé une fois par phase.` |
| `AppleWinNtsc.cpp:187` | `839-881` | DÉCALÉE (sans vides) | Table Monitor (y0) : réel **988-1037**. | `// [D] AppleWin NTSC.cpp:988-1037 @ 3e8054b462 — table Color Monitor (luma y0).` |
| `AppleWinNtsc.cpp:190` | `30-32` | DÉCALÉE (sans vides) | `NTSC_REMOVE_*` = 1 : réel **38-40**. | `// [D] AppleWin NTSC.cpp:38-40 @ 3e8054b462 — blanc, noir et gris sans chroma.` |
| `AppleWinNtsc.cpp:191` | `862-876` | DÉCALÉE (sans vides) | Gris `0x83`/`0x78` : réel **1016-1032**. | `// [D] AppleWin NTSC.cpp:1016-1032 @ 3e8054b462 — gris 5 → $83, gris 10 → $78.` |
| `AppleWinNtsc.cpp:198` | `882-907` | DÉCALÉE (sans vides) | Table Colour TV (y1) : réel **1039-1068**. Contenu juste (blanc/noir seulement, pas de gris). Le mot « comb » du commentaire POM2 est impropre : y1 est la luma de `signal − chroma passe-bande` (un réjecteur), pas un filtre en peigne à retard de ligne ; AppleWin ne l'appelle pas ainsi. | `// [D] AppleWin NTSC.cpp:1039-1068 @ 3e8054b462 — table Color TV (luma de signal − chroma).` |
| `VidHDCard.cpp:97` | `NTSC.cpp:1872-1887` | OK | `getVideoScannerAddressSHR()`, SCB, palette. | `// [D] AppleWin NTSC.cpp:1872-1887 @ 3e8054b462 — adresse, SCB puis palette de la ligne.` |
| `VidHDCard.h:45` | `NTSC.cpp:905-912` | OK | `getVideoScannerAddressSHR` : `$2000 + 160·v + 4·(h − 25)`. La continuation `+ :1872-1887` est juste aussi. | `// [D] AppleWin NTSC.cpp:905-912, :1872-1887 @ 3e8054b462` |
| `VidHDCard.h:56` | `NTSC.cpp:109` | OK | « the 320x200 is again doubled (to 640x400) ». Juste depuis `443545b0f6`. | `// [D] AppleWin NTSC.cpp:109 @ 3e8054b462 — SHR doublé en 640×400.` |
| `VidHDCard.h:97` | `NTSC.cpp:911` | OK | `return 0x2000 + kBytesPerScanline * …`. | `// [D] AppleWin NTSC.cpp:911 @ 3e8054b462 — pixels SHR en $2000 + 160·y.` |

Hors grep, même révision :

| Où | Citation | Verdict | Ligne corrigée |
|---|---|---|---|
| `VidHDCard.h:21`, `DEV.md:1702` | `:1866-1895` | OK | `[D] AppleWin NTSC.cpp:1866-1895 @ 3e8054b462 — updateScreenSHR().` |
| `DEV.md:2083`, `graphics_modes_comparison.md:343` | `115-132` | DÉCALÉE (sans vides) | `NTSC.cpp:151-170 @ 3e8054b462` |
| `DEV.md:2101` | `NTSC.cpp:331` | DÉCALÉE (sans vides) | `NTSC.cpp:394-398 @ 3e8054b462` (`getScanlineColor`, registre 12 bits en 397) |
| `AppleWinNtsc.cpp:260`, `tests/applewin_ntsc_smoke_test.cpp:131` | `updateFramebufferTVDoubleScanline` (nom seul) | OK | `NTSC.cpp:534-551 @ 3e8054b462` |

La sémantique a été relue citation par citation (contenu de la plage réelle
contre le commentaire POM2) ; aucune n'attribue à AppleWin un code qu'il n'a
pas. Deux formulations sur-classent leur source : « verbatim » pour des
constantes que POM2 passe de `float` à `double` (sans effet au-delà de ±1 LSB,
sonde P1), et l'en-tête des deux fichiers, qui invoque « CLAUDE.md: MAME =
source of truth » — la convention actuelle dit l'inverse (le silicium est la
référence, MAME et AppleWin sont grade D).

---

## Rapport 2 — Constats

**Aucun constat.** Le port est fidèle : les tables couleur sont celles
d'AppleWin à 1 LSB près (sonde P1), et pour HGR, DHGR et GR la couleur que
reçoit chaque échantillon 14M est **identique** à celle qu'AppleWin calcule,
au décalage de position près (sonde P5, 0 écart sur 83 840 échantillons par
mode). Les deux écarts réels relevés — la frontière aux/main du DLGR et le
centrage de 2 échantillons — sont des désaccords entre émulateurs ou des choix
de position documentés, sans arbitre de rang supérieur à ma disposition : ils
sont dans les pistes.

**Aucun `AMONT FAUX`.** Le seul endroit où POM2 semble plus cohérent
qu'AppleWin (le DLGR, aligné sur son DHGR) n'est appuyé que par MAME (grade D) :
je ne l'élève pas en `AMONT FAUX` sans Sather.

---

## Sondes

`/tmp/audit_ntsc/aw_ntsc_verbatim.h` est **extrait mécaniquement** de
`NTSC.cpp@3e8054b462` (lignes 38-40, 58-68, 137-138, 151-170, 374-380, 919-1080,
1111-1162, sans retouche). `probe.cpp` compare ses tables à celles de POM2 lues
par `AppleWinNtsc::renderLine` (12 échantillons, sortie 11 : `phaseOffset =
(p + 1) & 3` donne la phase `p`) :

```
clang++ -std=c++17 -O1 -Isrc -I/tmp/audit_ntsc /tmp/audit_ntsc/probe.cpp src/AppleWinNtsc.cpp
```

```
== P1  LUT AppleWin (verbatim, constantes float) vs POM2 (double)
Monitor :  1182/16384 entrées diffèrent, écart max 1 LSB ; histogramme |d|=0..7+ : 15202 1182 0 0 0 0 0 0
ColorTV :  1591/16384 entrées diffèrent, écart max 1 LSB ; histogramme |d|=0..7+ : 14793 1591 0 0 0 0 0 0
== P2  quelques couleurs pleines (motif 4 bits répété, phase 0)
  nibble 1 : POM2 5B0033  AppleWin 5B0033 (RRGGBB)
  nibble 2 : POM2 8D5A00  AppleWin 8D5A00
  nibble 5 : POM2 838383  AppleWin 838383
  nibble 9 : POM2 5A00FF  AppleWin 5900FF
  nibble C : POM2 2AB9FF  AppleWin 2ABAFF
  (les 11 autres identiques)
== P3  AppleWin 2e appel d'initChromaPhaseTables (filtres statiques continués) : 0 entrées Monitor changent
== P4  RAD_45 float = 0.78539818525314331  double = 0.78539816339744828  dérive sur 4096x24 pas = 0.00215 rad
```

`probe2.cpp` fait tourner le **vrai** `Apple2Display` en
`HiResMode::ColorAppleWin` / Monitor sur une mémoire aléatoire (graine fixe) et
le compare au **runtime** d'AppleWin simulé ligne par ligne d'après
`updateScreenSingleHires40` (1607-1645), `updateScreenDoubleHires80`
(1378-1412), `updateScreenSingleLores40` (1677-1706),
`updateScreenDoubleLores80` (1478-1517), `updatePixels` (603-655) et
`getScanlineColor` (394-398), phase `k & 3` et historique remis à 0 à chaque
ligne (`updateVideoScannerAddress`, 801-803). Lignes 0-159, colonnes 16-539.
Compilée avec les sources de `add_executable(test_dhgr_phase_signal …)` :

```
clang++ -std=c++17 -O1 -Isrc -Itests -Ibuild/generated -I/tmp/audit_ntsc /tmp/audit_ntsc/probe2.cpp \
  src/Apple2Display.cpp src/Apple2Display_ChatMauve.cpp src/Apple2Display_Beam.cpp \
  src/AppleWinNtsc.cpp src/LeChatMauveCard.cpp build/libpom2_core_test.a \
  -framework CoreAudio -framework AudioToolbox -framework AudioUnit
```

```
HGR  :  AW[k-1]:  62201/83840 écarts>1LSB  AW[k+0]:      0/83840 écarts>1LSB  AW[k+1]:  62213/83840  AW[k+2]:  67764/83840
       flux brut POM2[x] vs AW[x+0] : 0 écarts ; par x mod 14 : 0 0 0 0 0 0 0 0 0 0 0 0 0 0
DHGR :  AW[k-1]:  78475/83840 écarts>1LSB  AW[k+0]:  78674/83840  AW[k+1]:      0/83840 écarts>1LSB  AW[k+2]:  78684/83840
       flux brut POM2[x] vs AW[x+1] : 0 écarts ; par x mod 14 : 0 0 0 0 0 0 0 0 0 0 0 0 0 0
GR   :  AW[k-1]:  74996/83840 écarts>1LSB  AW[k+0]:      0/83840 écarts>1LSB  AW[k+1]:  74980/83840  AW[k+2]:  67548/83840
       flux brut POM2[x] vs AW[x+0] : 0 écarts ; par x mod 14 : 0 0 0 0 0 0 0 0 0 0 0 0 0 0
DLGR :  AW[k-1]:  70732/83840 écarts>1LSB  AW[k+0]:  76776/83840  AW[k+1]:  48584/83840  AW[k+2]:  78240/83840
       flux brut POM2[x] vs AW[x+1] : 6320 écarts ; par x mod 14 : 0 0 0 0 0 0 3220 0 0 0 0 0 0 3100
```

Lecture : HGR et GR identiques à position égale ; DHGR identique avec le
retard d'un échantillon qu'AppleWin applique volontairement (GH#555) ; DLGR
identique **sauf** les échantillons 6 et 13 de chaque cellule de 14, c'est-à-dire
la frontière aux/main (voir pistes). Ces deux échantillons entrent dans la
fenêtre de 12 bits de la plupart des sorties, d'où 58 % de couleurs
différentes pour un flux qui ne diffère que de 14 %.

---

## Pistes examinées et écartées

- **Règle de phase, une seule fois dans le dépôt.** `lut[(x + phase) & 3]`
  (`AppleWinNtsc::renderLine`), `k = (xi + rowPhase) & 3` (démod OE sur CPU) et
  `(floor(fx) + uPhaseOffset) & 3` (shader) lisent tous `signalPhaseRow_`, posé
  par le seul `setSignalPhase` de `fillCompositeSignal` (1 pour DHGR, DLGR et
  80 colonnes, 0 sinon). Une trame à phases mêlées part sur le CPU. Rien ne se
  contredit. La remise à zéro de la phase à chaque ligne a une source primaire :
  le brevet US 4,136,359 retarde le compteur de deux périodes 14M (180° de la
  sous-porteuse) une fois par ligne pour que la phase couleur soit la même à
  chaque ligne (grade B).
- **Demi-point HGR (bit 7).** `buildHgrWordRow` reporte le bit 13 du mot
  précédent dans un octet décalé ; AppleWin reporte aussi le bit 13
  (`updatePixels`, 628/653), d'où l'allongement d'un demi-point du dernier point
  avant un octet décalé et la troncature après. Sonde P5 : 0 écart. La copie de
  `hgrpaint/HgrConvert.cpp:81-86` applique la même règle.
- **DHGR : +1 échantillon chez AppleWin, 0 chez POM2**, même couleur par bit
  (P5). AppleWin dit lui-même que c'est un contournement (« the 14M video modes
  actually start 7x 14M pixels to the left on *real h/w* », 771-775) ; −7 et +1
  donnent la même phase. Différence de position seulement, sans arbitre lu.
- **DLGR : la frontière aux/main est à un échantillon près différente** chez
  AppleWin (aux sur les échantillons 0-6 de sa cellule, sans le report d'un bit
  qu'il applique en DHGR) et chez POM2 = MAME (aux sur 0-6 avec la phase
  avancée d'un cran, comme son DHGR). La relation bit/phase de chaque moitié est
  la même (P5 : le flux ne diffère qu'aux positions 6 et 13 mod 14). MAME et
  AppleWin se contredisent donc ; POM2 suit MAME, et c'est AppleWin qui est
  incohérent avec son propre DHGR. **Non tranché** : il faudrait Sather ch. 8
  (chargement aux/main en modes doubles). Effet : la limite entre deux blocs
  DLGR de couleurs différentes glisse de 1/14 de cellule (≈ 70 ns).
- **GR 40 colonnes** : `(nibble >> (absX & 3))` chez POM2, `lo >> ((1 −
  (hpos & 1)) × 2)` chez AppleWin : même bit à chaque échantillon (P5, 0 écart).
  Le brevet décrit ce mécanisme : un registre à décalage qui recircule quatre
  bits (grade B).
- **Tables : `float` contre `double`.** P1 : au plus 1 LSB sur 7 à 10 % des
  entrées. P4 : la phase dérive de 0,002 rad sur une construction. Esthétique,
  aucun pixel ne change de couleur nommée.
- **Second appel d'`initChromaPhaseTables` chez AppleWin** (état de filtre non
  remis à zéro) : P3, table identique. POM2 reconstruit à zéro, sans conséquence.
- **Centrage de 2 échantillons.** AppleWin dessine les modes Colour TV et
  Colour Monitor NTSC deux pixels plus à gauche (`g_pVideoAddress -= 2`, 769),
  ce qui compense le retard de la fenêtre causale de 12 bits. POM2 ne le fait pas
  (« no window-centring », `DEV.md:2101`) : l'image `ColorAppleWin` est deux
  échantillons plus à droite que chez AppleWin, et que les modes LUT et OE de
  POM2, dont les fenêtres sont centrées. Choix de position documenté, pas de
  couleur. La phrase de `DEV.md` laisse croire qu'AppleWin n'en fait pas non
  plus : à préciser.
- **Bande texte du mode mixte rendue en monochrome net.** AppleWin la colore en
  NTSC (salve présente, `updateScreenText40/80`) et la décolore en 50 Hz
  (GH#763/#1131, et `bc7c6b2150`). POM2 la remplace toujours par du texte net
  (`patchMixedTextBand`), choix documenté (`DEV.md` § Beam-racing, « crisp mono
  text is composited after demod »). Sur un Apple II NTSC à moniteur couleur,
  la bande texte porte des franges ; je n'ai pas de source primaire lue pour le
  dire avec un grade. Écart de rendu voulu.
- **Trame qui finit en TEXT après des lignes graphiques, en `ColorAppleWin`.**
  `render()` ne démodule en AppleWin que si l'état de fin d'image est graphique
  (`Apple2Display.cpp:657`, `:720`) ; sinon la bande HGR passe par
  `renderBeamRacing` → `renderHiRes`, qui rabat `ColorAppleWin` sur la LUT MAME
  (`:1671`). La même mémoire est donc peinte avec deux palettes selon l'endroit
  où la trame se termine. C'est une incohérence entre deux approximations du
  composite, pas un écart au silicium : hors périmètre des constats. Lu, **non
  sondé**.
- **Mode TV** : le mélange 3/4 + 1/4 est bien la moyenne de la paire d'AppleWin
  (`updateFramebufferTVDoubleScanline`, 534-551 : ligne Apple non mélangée,
  ligne intermédiaire 50/50 avec la ligne précédente non mélangée), et la ligne 0
  se mélange au noir comme chez AppleWin. La ligne intermédiaire finale (50 % du
  courant, 547-548) n'a pas d'équivalent : bord seulement.
- **Salve de couleur en NTSC** : AppleWin ne la coupe jamais d'un coup à 60 Hz ;
  une ligne de texte la décrémente de 4 (`updateScreenText40/80`, l. 1717-1718
  et 1782-1783), donc le texte qui suit des lignes graphiques reste coloré.
  POM2 colore aussi ces lignes quand la trame finit en graphique (le signal les
  contient). Cohérent.
- **TEXT plein écran** : AppleWin passe aux tables N&B quand la salve manque
  (`GetColorBurst`), dont la luminance est le bit brut ; POM2 rend le texte net.
  Équivalent.
- **« Idealized »** de POM2 (luma Monitor, chroma × 1,6) n'a rien à voir avec
  `VT_COLOR_IDEALIZED` d'AppleWin (`RGBMonitor.cpp`) ; c'est dit dans
  `AppleWinNtsc.cpp:84-87`. Homonymie seulement.
- **DHIRES sans 80COL sur //e** : AppleWin (`updateScreenDoubleHires40`, `m &
  0x7F`, sans décalage) et MAME masquent le bit 7 ; POM2 aussi
  (`Apple2Display.cpp:2302`). `Memory.cpp:1228-1230` ne pose `dhgr` qu'en mode
  //e : un ][+ qui touche AN3 garde son demi-point. Sain.
- **Ordre 80 colonnes** : aux puis main (`paintText80`), comme AppleWin
  (`(main << 7) | aux`, 1807). Sain.
- **VidHD** : adresses `$2000 + 160·y`, SCB `$9D00 + y`, palettes `$9E00 +
  32·p`, 200 lignes doublées : conformes à 905-912 et 1866-1895. La peinture
  d'un bloc par trame (pas de course de faisceau) est documentée dans
  `VidHDCard.h`. Le remplissage et le mode 640 viennent de `VidHD.cpp` et de
  MAME, hors tranche.
- **Hors tranche, noté en passant** : l'en-tête d'`AppleWinNtsc.{h,cpp}` et
  `DEV.md` § AppleWin NTSC citent une convention périmée (« MAME = source of
  truth », « AppleWin = source of truth »), contraire à la règle actuelle de
  `CLAUDE.md`.

---

## Bilan

- **Citations examinées** : 23 (+ 7 hors grep : 3 continuations de la VidHD,
  3 dans `DEV.md`, 1 dans `graphics_modes_comparison.md`).
- **Verdicts** (les 23) : 4 OK ; 0 OK (rev X) ; **19 DÉCALÉE**, dont **17**
  qui tombent juste sur `3e8054b462` en **numérotation sans lignes vides**
  (16 exactement, 1 à une ligne près) et 2 (`VidHDCard.h:99`, `:100`) décalées
  de 2 lignes et qu'aucune révision n'a jamais portées ; 0 ABSENTE ;
  0 INEXACTE ; 0 NON-PINNÉE. Hors grep : 3 OK, 3 DÉCALÉE (sans vides).
- **Révision amont** : pas de pin. Une seule époque, `9d312230e5` →
  `3e8054b462` (`master`, 2026-07-26), numérotation identique sur toutes les
  lignes citées ; à épingler `@ 3e8054b462`, en numérotation réelle. Aucun
  correctif amont de comportement postérieur au port ; `bc7c6b2150` (salve PAL en
  Colour TV) antérieur et hors du modèle de POM2.
- **Défaut structurel** : deuxième occurrence (après `mos6551`, tranche 5)
  d'un outil de numérotation qui saute les lignes vides. Toute citation produite
  par le même outil est suspecte ; un contrôle mécanique (ancre +
  renumérotation) la date en une passe.
- **Constats** : CRITIQUE 0 · MAJEUR 0 · MINEUR 0. **AMONT FAUX** : 0.
- **Montée en grade possible** : le « 4 phases par point » et la remise de phase
  à chaque ligne sont dans le brevet US 4,136,359 (grade B), à placer au-dessus
  des lignes D.

**Ce que je n'ai pas pu vérifier :**

- **Sather, ch. 8** (*Understanding the Apple IIe*), non lu : il reste le seul
  arbitre possible pour la frontière aux/main du DLGR (MAME/POM2 contre
  AppleWin), le décalage de −7 échantillons des modes 14M qu'AppleWin attribue
  au matériel, et la couleur de la bande texte du mode mixte.
- **La matrice YIQ→RGB** n'a pas été confrontée à la norme FCC/SMPTE : je ne
  sais pas si `-1.105/1.702` est la valeur normative ou un arrondi d'AppleWin.
  POM2 recopie AppleWin, donc pas d'écart entre eux.
- **Aucune mesure sur machine réelle** (grade A) : les teintes absolues des
  tables sont un modèle d'AppleWin calibré sur MAME (`DEV.md`), pas une capture.
- **Le runtime d'AppleWin n'a pas été exécuté** : la sonde P5 le simule d'après
  le code lu. Le cas mi-ligne (changement de mode au milieu d'une ligne) et la
  salve de couleur en 50 Hz n'ont pas été simulés.
- La trame qui finit en TEXT (palette MAME dans le mode AppleWin) est lue dans
  le code, pas sondée.
- `VidHD.cpp` (remplissage, mode 640) et `RGBMonitor.cpp` sont hors tranche.
