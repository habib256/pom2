# Audit de parité : citations `imagewriter.cpp` (2026-11-01)

Tranche 8 du brief `docs/audits/BRIEF-parite-amont.md`. Toutes les citations
`imagewriter.cpp:<lignes>` sous `src/`, obtenues par :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep 'imagewriter\.cpp:'
```

24 citations : 20 dans `src/ImageWriter.cpp`, 4 dans `src/ImageWriter.h`.
`ImageWriterRom.h`, `ImageWriterPdf.*`, `PrinterScreenDump.*` et
`PrinterRender.*` n'en contiennent aucune. Aucun fichier de code n'a été
modifié. La sonde et les fichiers amont sont dans `/tmp/audit_imagewriter/`.

**Arbitre.** La source primaire est le manuel d'Apple, *ImageWriter II
Technical Reference Manual* (1985). Je l'ai récupéré sur archive.org (item
`AppleImageWriterIITechnicalReferenceManual`, PDF et texte OCR). Les numéros de
page cités viennent de sa table des matières.

## L'amont n'est pas MAME

La file du brief attribue cette tranche à MAME. C'est faux :

- **MAME** ne contient aucun `imagewriter.cpp`, ni au tag `mame0287` (arbre
  récursif de 33 602 entrées) ni sur `master` (34 641 entrées). La recherche
  `imagewriter` ne ramène que `3rdparty/bgfx/.../aviwriter.h`.
- **AppleWin** non plus (arbre `master`, 702 entrées). Deux phrases de
  `docs/printer_plan.md` (l. 24 et l. 52) disent pourtant « ported
  line-for-line from AppleWin's `imagewriter.cpp` ». Cette attribution est
  fausse.
- **L'amont réel**, nommé correctement par `ImageWriter.h:25-33` et DEV.md
  § ImageWriter, est **greg-kennedy/ImageWriter**. Ce dépôt importe lui-même le
  code de **GSport** (`david-schmidt/gsport`, `src/imagewriter.cpp`), qui vient
  de DOSBox.

## Révision amont

**Aucun commit n'est épinglé.** `ImageWriter.h` et DEV.md nomment le dépôt sans
révision. L'absence de pin ne gêne pas ici, parce que l'amont n'a qu'un seul
contenu possible :

- greg-kennedy/ImageWriter compte 5 commits (2026-01-21 → 2026-02-02).
  `imagewriter.cpp` y est **identique octet pour octet** dans les cinq (même
  MD5), de `68ea1d08` à `d0d70958` (HEAD).
- Ce fichier est **GSport `efcba1b4`** (2025-07-16) à la fin de ligne près
  (`diff --strip-trailing-cr` vide). `efcba1b4` est aussi le HEAD de GSport
  pour ce fichier.

J'ai récupéré les **16 révisions GSport** du fichier (2014-04 → 2025-07) et les
5 de greg-kennedy, puis daté chaque citation par une ancre regex sur la plage
citée (script `/tmp/audit_imagewriter/date.py`).

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|---|
| GSport `9fe038c8` … `3d256ecb` (14 révisions) | 2014-04 → 2015-07 | Port DOSBox. `style = 0` au reset, pas de `#define SWITCHA_*`, bogues `if (switcha &= …)` | **0** |
| GSport `abae7bcd` | 2024-11-03 | « Assorted fixes » : `SWITCHA_*`, saut de perforation (sur le bit `0x10`, 0,25"), `bottomMargin - lineSpacing`, `ESC H` → `updateSwitch()`. Au reset, `style = STYLE_BASE`, une constante qui n'existe pas : le fichier ne compile pas | **22** (toutes sauf `h:51` et `h:763`) |
| GSport `efcba1b4` | 2025-07-16 | « fix(regression): BOLD/BASE typo » : `STYLE_BASE` devient `STYLE_BOLD` pour rétablir la compilation | **23** (ajoute `h:51`) |
| greg-kennedy `68ea1d08` → `d0d70958` = **référence** | 2026-01-21 → 02-02 | Import de `efcba1b4`, fichier inchangé ensuite | **23** |
| aucune | — | — | `h:763` (`1160-1200`) |

Toutes les citations sauf une tombent donc juste sur l'unique contenu de
greg-kennedy, et 22 d'entre elles déjà sur GSport `abae7bcd`. Le port est
postérieur au 2024-11-03 ; la citation `h:51` le situe même après le
2025-07-16. **Il n'existe aucun correctif amont postérieur à la référence** :
greg-kennedy n'a plus touché le fichier, et la référence est le HEAD de GSport.

Repères dans la référence : `FillPalette` 101-114 · constructeur 117-260 ·
`resetPrinterHard` 262-268 · `resetPrinter` 270-325 · `selectCodepage` 345-362 ·
`updateFont` 366-445 · `updateSwitch` 447-479 · `processCommandChar` 497-1217
(ESC 1-6 665-678, ESC V 718-736, ESC U 738-758, hauteurs de pas 765-836,
ESC K 935-949, HT 1132-1142, VT 1144-1168, CR/LF 1172-1182) · `newPage`
1222-1252 · `printChar` 1254-1360 · `setupBitImage` 1432-1540 ·
`printBitGraph` 1541-1603 · `formFeed` 1606-1613.

---

## Rapport 1 — Citations

Les lignes corrigées utilisent la référence notée
`@ d0d70958 (= GSport efcba1b4)`. Abréviation : **TR** = *ImageWriter II
Technical Reference Manual* (Apple, 1985).

| Fichier POM2:ligne | Citation | Verdict | Commentaire |
|---|---|---|---|
| `src/ImageWriter.h:51` | `imagewriter.cpp:289` | **INEXACTE** | La ligne contient bien `style = STYLE_BOLD`, mais le motif avancé (« to thicken a thin TrueType face ») est démenti par l'historique : GSport avait `style = 0` de 2014 à 2024. Le gras n'est arrivé qu'en 2025, comme correctif de compilation d'une faute de frappe (`STYLE_BASE`). POM2 a raison sur le fond (voir AMONT FAUX A). |
| `src/ImageWriter.h:763` | `imagewriter.cpp:1160-1200` | DÉCALÉE | Le rétablissement de `msb` après ESC V / ESC U est en **735** et **757** (`msb = 0;`). Les lignes 1160-1200 contiennent VT, FF, CR, LF, SO, SI et DC2. Aucune révision ne place ce code dans cette plage. |
| `src/ImageWriter.cpp:1502` | `imagewriter.cpp:497-1217` | OK | `processCommandChar` exact. Réserve : le titre dit « ported verbatim », alors que ce bloc porte au moins six écarts voulus et documentés sur place (HT, ESC 1-6, ESC L, ESC H, US n, ESC V/U). Retirer « verbatim ». |
| `src/ImageWriter.cpp:820` | `imagewriter.cpp:437-442` | OK | Le cisaillement 0,20 de FreeType est bien là. Réserve : POM2 écrit « matching », mais son cisaillement d'un point par cellule donne une pente d'environ 0,06, pas 0,20. Seul le chemin Epson pose `kStyleItalics`. |
| `src/ImageWriter.cpp:1637` | `imagewriter.cpp:1131-1141` | OK | Cas HT en 1132-1142 (±1). Le défaut « dernier taquet » de la référence est confirmé à la lecture (voir AMONT FAUX C). |
| `src/ImageWriter.cpp:51` | `imagewriter.cpp:87-98` | OK | `#define SWITCHA_*`. POM2 corrige le sens du bit `0x10` (voir AMONT FAUX B). |
| `src/ImageWriter.cpp:75` | `imagewriter.cpp:450-460` | OK | Les dix points de code nationaux. |
| `src/ImageWriter.cpp:262` | `imagewriter.cpp:935-949` | OK | ESC K. Table identique au TR, Table 8-6 p. 123 : à faire monter en grade B. |
| `src/ImageWriter.cpp:420` | `imagewriter.cpp:262-325` | OK | `resetPrinterHard` + `resetPrinter`. La plage sans préfixe `1222-1252` de la même ligne est `newPage`, OK aussi. |
| `src/ImageWriter.cpp:430` | `imagewriter.cpp:281` | OK | `leftMargin = 0.25`. Le TR ne donne aucune distance au bord du papier (p. 59) ; `ImageWriter.h:306-309` le dit déjà. |
| `src/ImageWriter.cpp:488` | `imagewriter.cpp:315` | OK | `newPage(false,true)` : la référence jette la feuille. L'éjection par POM2 est VOULUE. |
| `src/ImageWriter.cpp:537` | `imagewriter.cpp:345-362` | OK | `selectCodepage`. |
| `src/ImageWriter.cpp:543` | `imagewriter.cpp:447-479` | OK | `updateSwitch`. POM2 s'en écarte volontairement pour la perforation (B-3, 0,5"). |
| `src/ImageWriter.cpp:575` | `imagewriter.cpp:394-425` | OK | Calcul de `actcpi`. Écart sur proportionnel + condensé : latent, voir les pistes. |
| `src/ImageWriter.cpp:618` | `imagewriter.cpp:1606-1613` | OK | `formFeed`, sans éjecter une page blanche. |
| `src/ImageWriter.cpp:916` | `imagewriter.cpp:1260-1263` | OK | Masque du bit 7 hors graphisme. |
| `src/ImageWriter.cpp:935` | `imagewriter.cpp:1325` | OK | Zéro barré sur B-1. |
| `src/ImageWriter.cpp:1404` | `imagewriter.cpp:1432-1603` | OK | `setupBitImage` + `printBitGraph`. |
| `src/ImageWriter.cpp:1480` | `imagewriter.cpp:1574-1577` | OK | ESC t. |
| `src/ImageWriter.cpp:1837` | `imagewriter.cpp:665-678` | OK | ESC 1-6 en position absolue dans la référence : c'est le défaut que POM2 corrige (voir AMONT FAUX D). |
| `src/ImageWriter.cpp:1895` | `imagewriter.cpp:765-836` | OK | Les huit pas. Densités conformes au TR, Table 8-2 p. 106. Pour les `cpi` des pas proportionnels, voir le constat 3. |
| `src/ImageWriter.cpp:2453` | `imagewriter.cpp:101-114` | OK | `FillPalette` : forme close vérifiée bande par bande contre les appels 169-184. |
| `src/ImageWriter.h:47` | `imagewriter.cpp:1556-1573` | OK | Le « Primative scaling function ». |
| `src/ImageWriter.h:57` | `imagewriter.cpp:203-208` | OK | Commentaire `yyyxxxxx`. |

Hors du motif du brief mais vérifiées au passage : `iw_charmaps.h:50-60`
(`intCharSets`, avec la transcription CP437 de POM2 recontrôlée case par case)
et `iw_charmaps.h:62-77` (tailles de papier). Les deux sont **OK**.

### Lignes corrigées (format du dépôt)

Grade D par défaut. Je ne donne une ligne B que là où j'ai lu la page du TR.

```
// [B] TR ch. 3 pp. 34-35 — A-1..A-3 langue, A-5 réponse select, A-6 LF ligne
//     pleine, A-7 commandes d'impression, A-8 LF après CR ; B-1 zéro barré,
//     B-3 saut de perforation (ouvert = saut), B-6 8e bit.
// [D] greg-kennedy/ImageWriter imagewriter.cpp:87-98 @ d0d70958 (= GSport
//     efcba1b4) — SWITCHA_* ; nomme à tort 0x10 « perforation skip ».
// [D] … imagewriter.cpp:450-460 @ d0d70958 — remap des dix points nationaux.
// [B] TR Table 8-6 p. 123 — ESC K 0..6 = noir, jaune, magenta, cyan,
//     orange, vert, violet.
// [D] … imagewriter.cpp:262-325, 1222-1252 @ d0d70958 — reset / newPage.
// [F] Distance du point 0 au bord du papier : 0,25" vient de la référence
//     (imagewriter.cpp:281 @ d0d70958) ; le TR p. 59 n'en donne aucune.
//     Réglé par : une mesure sur une ImageWriter II.
// [D] … imagewriter.cpp:315 @ d0d70958 — la référence jette la feuille ;
//     POM2 l'éjecte (voulu).
// [D] … imagewriter.cpp:345-362 @ d0d70958 — selectCodepage.
// [D] … imagewriter.cpp:447-479 @ d0d70958 — updateSwitch (perforation sur
//     A-5 et 0,25" : faux, voir [B] TR p. 35 et Table 5-12 p. 73).
// [D] … imagewriter.cpp:394-425 @ d0d70958 — calcul de actcpi.
// [D] … imagewriter.cpp:1606-1613 @ d0d70958 — formFeed sans page blanche.
// [D] … imagewriter.cpp:437-442 @ d0d70958 — cisaillement italique 0,20.
// [B] TR Table 4-2 p. 41 et Table A-1 p. 134 — 8e bit ignoré par défaut,
//     ESC Z NUL SP l'inclut.
// [D] … imagewriter.cpp:1260-1263 @ d0d70958 — sauf en graphisme.
// [B] TR p. 35 — B-1 fermé = zéros barrés.
// [D] … imagewriter.cpp:1325 @ d0d70958.
// [D] … imagewriter.cpp:1432-1603 @ d0d70958 — setupBitImage / printBitGraph.
// [D] … imagewriter.cpp:1574-1577 @ d0d70958 — ESC t (manuel LQ non lu).
// [D] … imagewriter.cpp:497-1217 @ d0d70958 — interpréteur (porté, avec écarts).
// [B] TR « Using Tabs » p. 67 — HT va au taquet SUIVANT ; plusieurs HT
//     avancent d'autant de taquets.
// [D] … imagewriter.cpp:1132-1142 @ d0d70958 — la référence va au dernier.
// [B] TR Table 4-6 p. 49 — ESC 1..6 insère m points une fois.
// [D] … imagewriter.cpp:665-678 @ d0d70958 — la référence en fait une position absolue.
// [B] TR Table 8-2 p. 106 — densités 72/80/96/107/120/136/144/160.
// [D] … imagewriter.cpp:765-836 @ d0d70958 — pas, printRes, definedUnit.
// [D] … imagewriter.cpp:101-114 @ d0d70958 — FillPalette.
// [D] … imagewriter.cpp:1556-1573 @ d0d70958 — mise à l'échelle des pixels.
// [B] TR Tables A-1 / A-2 pp. 134-135 — boldface Off au reset matériel et
//     logiciel.
// [D] … imagewriter.cpp:289 @ d0d70958 (= GSport efcba1b4) — STYLE_BOLD au
//     reset, artefact d'un correctif de compilation de 2025 (GSport avait
//     style = 0 de 2014 à 2024).
// [D] … imagewriter.cpp:203-208 @ d0d70958 — codage yyyxxxxx.
// [D] … imagewriter.cpp:735, 757 @ d0d70958 — ESC V / ESC U remettent msb = 0
//     (constante) ; POM2 rétablit selon B-6 (voulu).
```

---

## Rapport 2 — Constats

Les cinq constats portent sur des écarts que POM2 **hérite de la référence**
(GSport/greg-kennedy fait pareil) et que le manuel d'Apple contredit. Les
quatre AMONT FAUX qui suivent sont, à l'inverse, les points où POM2 a déjà
corrigé la référence.

### 1. Une ligne pleine provoque un saut de ligne ; le manuel dit surimpression par défaut, et A-6 n'est pas modélisé

```
Fichier      : src/ImageWriter.cpp:970-975 (printCharInternal : wrap → lineFeed())
               src/ImageWriter.cpp:1456-1460 (printBitGraph : colonnes jetées)
Amont        : imagewriter.cpp:1352-1357 @ d0d70958 (même saut de ligne)
Gravité      : MINEUR
Confiance    : CONFIRMÉ pour le texte (TR + sonde) ; PLAUSIBLE pour le
               graphisme (le TR ne traite le dépassement qu'en une phrase
               générale)
```

**Divergence.** TR p. 79 : « When you turn the printer on, it is not set to
insert a line feed character after an automatic carriage return. In this case,
if you send a long string of characters to the printer without any line feeds,
each line of text overprints the previous line. » Le TR p. 34 dit la même chose
pour le commutateur logiciel A-6 (« The default is for no line feed to be
inserted »), et p. 83 pour ESC R (« the excess characters print over other
characters on the same line unless the "automatic line feed when line is full"
function is enabled »). `ESC D SP NUL` / `ESC Z SP NUL` (Table 5-16) commutent
ce comportement.

POM2, comme la référence, fait **toujours** CR **et** LF. A-6 (bit `0x20` de
`switcha_`) n'est lu nulle part : `ESC D SP NUL` est accepté sans effet.
web-a2e (grade D, `citoh-printer.js:796-800`) fait aussi CR + LF ; les trois
émulateurs s'accordent contre le manuel.

Pour le graphisme, le TR p. 34 parle de « text **or graphics** » : la ligne
part et le chariot revient. POM2 **jette** les colonnes en trop. DEV.md
§ ImageWriter justifie ce choix par « an over-long graphics line on the real
printer loses its excess columns », sans source (grade E) et contre un grade B.

**Scénario.** Mise sous tension, 12 cpi, 100 × `X` sans CR. Matériel : 96
caractères, puis les 4 suivants frappés sur les 4 premiers de la **même**
ligne. POM2 : 96 caractères, puis 4 sur la ligne suivante. Même écart avec
`ESC R 100 _`, ou avec une ligne Applesoft de plus de 96 caractères listée sans
retour inséré par la carte.

**Observation.** Sonde, cas 1 (`headY` attendu : 0) :

```
1a. 100 x 'X' sans CR, 12 cpi : head Y = 0.1667 in, encre y 0..37 (38 px de haut)
1b. ESC R 100 '_'           : head Y = 0.1667 in, encre y 16..41
1c. ESC D SP NUL puis 100 'X': head Y = 0.1667 in
```

### 2. Un LF ne ramène pas la tête à la marge : l'insertion de CR (ESC l, active par défaut) n'est pas modélisée

```
Fichier      : src/ImageWriter.cpp:1686-1703 (cas LF), :629-637 (lineFeed),
               :1960 (« case 0x6c: break; // CR before LF/FF »)
Amont        : imagewriter.cpp:1177-1182 @ d0d70958 (« //curX = leftMargin; »
               commenté)
Gravité      : MINEUR
Confiance    : CONFIRMÉ (TR + sonde)
```

**Divergence.** TR Table 5-14 p. 75 : `ESC l 0` = « Insert CR before LF and FF
**(default)** », `ESC l 1` = pas d'insertion. Le TR (pp. 75-76) décrit la
séquence : après un LF, « The printer is set to start the next line at the left
margin ». Tables A-1 et A-2 : « Carriage return insertion : On », au reset
matériel comme logiciel. POM2 avance le papier sans toucher `curX_` et ignore
`ESC l`. web-a2e (grade D, `citoh-printer.js:261-266, 322-324`) modélise
l'insertion conformément au manuel ; la référence non.

**Scénario.** `"ABC" LF "DEF"`. Matériel : `DEF` en marge gauche de la ligne 2.
POM2 : `DEF` commence sous la colonne qui suit `C`, ce qui produit un escalier.
Toute source qui termine ses lignes par LF seul (texte LF, pilote graphique
`ESC G … LF`) en souffre. Je n'ai pas inventorié les logiciels Apple II qui le
font.

**Observation.** Sonde, cas 2 (tête attendue à 0,50" après `DEF`, à 0,25" après
un LF seul) :

```
2a. "ABC" LF "DEF" (AutoFeed Off): head X = 0.7500 in (marge gauche 0.25)
2b. "ABC" LF "DEF" (AutoFeed Auto): head X = 0.7500 in (marge gauche 0.25)
2c. ESC l 0 "ABC" LF        : head X = 0.5000 in
```

### 3. En pas proportionnel, marges et taquets sont comptés à 10 et 12 colonnes au pouce au lieu de 9 et 10

```
Fichier      : src/ImageWriter.cpp:1908-1911 (ESC p → cpi_ = 10, ESC P → cpi_ = 12),
               utilisés par :1986 (ESC L) et :1751 (tabStop, pour ESC ( ESC ) ESC u)
Amont        : imagewriter.cpp:819-836 @ d0d70958 (mêmes valeurs)
Gravité      : MINEUR
Confiance    : CONFIRMÉ (TR + sonde)
```

**Divergence.** TR p. 60 (Table 5-2 et texte) : « If elite proportional pitch
is selected, the margin setting is measured at 10 character positions to the
inch; with pica proportional pitch, it is measured at 9 to the inch. » Les
plages le confirment : `ESC L` 000-071 en pica proportionnel et 000-079 en
élite proportionnel ; taquets 001-072 et 001-080 (Table 5-8 p. 66). Les deux
densités graphiques (144 et 160 dpi) divisées par 16 points donnent bien 9 et
10. POM2 et la référence utilisent `cpi_` = 10 et 12. web-a2e aussi
(`CPI.propPica: 10, propElite: 12`) : encore trois émulateurs contre le manuel.

**Scénario.** `ESC p ESC L 009` : marge à 0,25 + 9/9 = 1,25" sur le matériel,
1,15" dans POM2. `ESC P ESC L 010` : 1,25" contre 1,083" (soit 1/6" d'écart).
Même décalage pour les taquets, donc pour toute mise en page proportionnelle
qui positionne ses colonnes par `ESC L` ou par HT.

**Observation.** Sonde, cas 3 (le témoin 3d en élite fixe tombe juste) :

```
3a. ESC p ESC L 009 : marge = 1.1500 in (manuel : 0.25 + 9/9 = 1.2500)
3b. ESC P ESC L 010 : marge = 1.0833 in (manuel : 0.25 + 10/10 = 1.2500)
3c. ESC p ESC ( 010. HT : tête = 1.1500 in (manuel : 0.25 + 9/9 = 1.2500)
3d. témoin ESC E ESC L 012 : marge = 1.2500 in (manuel : 0.25 + 12/12 = 1.2500)
```

Pour corriger, il faut une « largeur de colonne » propre aux deux pas
proportionnels, distincte de `cpi_`. `cpi_` sert aussi d'avance de repli et de
pas de BS, deux usages que je n'ai pas arbitrés.

### 4. La police à la mise sous tension et après ESC c est Correspondance ; le manuel dit Brouillon (draft)

```
Fichier      : src/ImageWriter.cpp:449 (« quality_ = Quality::Correspondence;
               // ESC a default (Table 4-1) »), src/ImageWriter.h:700
Amont        : — (la référence n'a pas de faces ; ESC a est ignoré en 917-918)
Gravité      : MINEUR
Confiance    : CONFIRMÉ (TR + sonde)
```

**Divergence.** TR p. 39 : « When you turn on the printer it is set to use the
draft font. » Table A-1 p. 134 (reset matériel) et Table A-2 p. 135 (après
`ESC c`) : « Font : Draft ». Le commentaire de POM2 cite justement la
Table 4-1, dont la ligne « Default » renvoie au bouton Print Quality, que le
texte d'à côté place sur draft à la mise sous tension. web-a2e
(`citoh-printer.js:167`) part bien de draft.

Conséquence : toute page qui ne commence pas par `ESC a` (un `PR#1 : LIST`,
par exemple) sort avec la face correspondance (9 fils, autre dessin) au lieu
de la face brouillon 12 colonnes. Ce sont pourtant ces deux faces que la phase
A du plan d'impression a importées.

**Règle tenue deux fois.** La face vient de `quality_` (posée par `ESC a`,
côté invité). La cadence vient de `speed_`, un réglage hôte qui vaut 250 cps
(draft) par défaut. Aujourd'hui POM2 imprime donc la face correspondance à la
vitesse draft ; le matériel imprime la face draft à 250 cps. Un `ESC a 2` (NLQ,
45 cps d'après le TR p. 39) reste rythmé à 250 cps tant que l'hôte n'a pas
changé de cadence, ce qui raccourcit d'autant le BUSY que voit l'invité.

**À reprendre avec la correction.** TR p. 39 : « The boldface, double-width,
half-height, subscript, superscript, and proportional printing options are not
available for the draft font. If you select any of these options while printing
in the draft font, the printer changes to the correspondence font. » POM2 ne
l'applique qu'au proportionnel (`currentBank`, `ImageWriter.cpp:717-721`).
Passer le défaut à draft sans cette règle donnerait du gras en face brouillon.

**Observation.** Sonde, cas 4 :

```
4a. police à la construction : Correspondence (manuel : Draft)
4b. ESC a 2 puis ESC c        : Correspondence (manuel Table A-2 : Draft)
```

### 5. ESC M et ESC m sont ignorés ; ce sont ESC a 2 (NLQ) et ESC a 0 (correspondance)

```
Fichier      : src/ImageWriter.cpp:1952-1954 (« case 0x6d: case 0x4d: break;
               // typeface select — one font »)
Amont        : imagewriter.cpp:919-920 @ d0d70958 (commentés « Same as ESC a
               n commands », sans effet ; ESC a n'y fait rien non plus)
Gravité      : MINEUR
Confiance    : CONFIRMÉ (TR + sonde)
```

**Divergence.** TR Table 4-1 p. 39 : `ESC m` = « Same as ESC a 0 », `ESC M` =
« Same as ESC a 2 », ajoutés « to maintain compatibility with the Apple
Scribe™ printer ». POM2 a implémenté `ESC a` mais a laissé ces deux alias à
« typeface variant », une lecture que le manuel ne soutient pas. web-a2e
(`citoh-printer.js:486-487`) les implémente.

À noter aussi : `kIw1Ignored` (`ImageWriter.cpp:135-140`) décrit `m M` comme
« alternate character map (MouseText) ». Sur l'ImageWriter II, MouseText
s'active par `ESC &` / `ESC $` (Table 4-2 p. 41). Je n'ai pas vérifié ce que
fait l'ImageWriter I de ces deux codes.

**Scénario.** Un programme écrit pour la Scribe envoie `ESC M` avant son
texte : le matériel imprime en NLQ (cellule de 18 rangs, 45 cps), POM2 reste
dans la face courante.

**Observation.** Sonde, cas 5 (encre différente pour le même mot) :

```
5a. ESC M : Correspondence (manuel : NLQ)
5b. ESC a 2 puis ESC m : NLQ (manuel : Correspondence)
5c. encre "HELLO" : ESC a 2 = 186 px, ESC M = 220 px
```

### AMONT FAUX A. La référence met le gras au reset ; POM2 a raison de ne pas le faire

```
Fichier      : src/ImageWriter.cpp:453-455, src/ImageWriter.h:50-52
Amont        : imagewriter.cpp:289 @ d0d70958 (= GSport efcba1b4)
Gravité      : —
Confiance    : CONFIRMÉ (TR + historique GSport)
```

TR Tables A-1 et A-2 : « Boldface text : Off » au reset matériel comme
logiciel. La référence met `style = STYLE_BOLD`. L'historique explique pourquoi :
GSport avait `style = 0` jusqu'en 2024 ; `abae7bcd` (PR #20) l'a remplacé par
`STYLE_BASE`, une constante inexistante ; `efcba1b4` (PR #21, « Fix a type in
the imagewriter code that caused a compile failure ») a mis `STYLE_BOLD` pour
que le fichier compile. Le gras au reset est donc **accidentel**, et récent.
Sonde, cas 4c : `style après ESC c : normal (manuel : boldface Off)`. Il faut
garder le comportement de POM2 et corriger son motif (`ImageWriter.h:50-52`,
DEV.md § ImageWriter « Deliberate deviations »).

### AMONT FAUX B. La référence lit le saut de perforation sur A-5 avec une marge de 0,25" ; c'est B-3 et 0,5"

```
Fichier      : src/ImageWriter.cpp:54-72, :548-566
Amont        : imagewriter.cpp:97, 462-471 @ d0d70958 (arrivé avec abae7bcd)
Gravité      : —
Confiance    : CONFIRMÉ (TR)
```

TR p. 34 : A-5 = « Response to Software Select-Deselect ». TR p. 35 : « When
software switch B-3 is open (off), the printer automatically skips over the
last half inch … and the top half inch ». Table 5-12 p. 73. POM2 suit le manuel
et le documente sur place. Un futur « alignement sur la référence » serait une
régression.

### AMONT FAUX C. HT : la référence va au dernier taquet, POM2 au suivant

`imagewriter.cpp:1132-1142` écrase `moveTo` à chaque taquet situé à droite de la
tête. Avec des taquets dans l'ordre croissant, la tête finit donc sur le plus
éloigné. TR p. 67 : « start at the next tab position … Several CONTROL-I
characters can be sent in a row to cause the next character to print several
tab positions from the last character printed ». POM2
(`ImageWriter.cpp:1635-1650`) suit le manuel, et DEV.md le documente. Même
chose pour VT.

### AMONT FAUX D. ESC 1..6 : la référence en fait une position absolue

`imagewriter.cpp:673-675` fait `curX = m/unit`. TR Table 4-6 p. 49 et le texte
que POM2 cite (`ImageWriter.cpp:1824-1849`) décrivent une insertion de m
points, faite une seule fois. POM2 suit le manuel ; le test `imagewriter_smoke`
fixe ce comportement.

---

## Pistes examinées et écartées

- **MAME et AppleWin** : aucun modèle d'ImageWriter (arbres vérifiés). La
  seule seconde lecture disponible est web-a2e (MIT, `345455a2`), consultée
  pour les constats 1 à 5.
- **Correctifs amont postérieurs** : il n'y en a pas. Ceux de `abae7bcd`
  (2024-11), antérieurs à la référence, sont dans POM2 : test de fin de page
  `bottomMargin - lineSpacing` (`lineFeed`, `ImageWriter.cpp:636`), plus de
  `switcha &=` destructif, `ESC H` → `updateSwitch()` (`:1938`).
- **Palette** (`indexToRgb` contre `FillPalette` + appels 169-184) : identique,
  y compris la bande 0 qui reste blanche.
- **ESC K** : identique à la référence et à la Table 8-6.
- **Densités des huit pas, ESC F, ESC h** : conformes à la Table 8-2 et à la
  Table 8-5 (« nnnn dot columns from the left margin »).
- **Origine des taquets** : TR p. 66, « starting from the left margin. The left
  margin has the column position number 1 … the tabs remain in the same
  absolute locations ». `tabStop` (`:1751`) calcule la position au moment de la
  pose, comme le manuel.
- **Défauts DIP** : 12 cpi (SW1-6 fermé, p. 25), pas de saut de perforation
  (SW1-5 ouvert), pas de LF après CR (SW1-8 ouvert). POM2 est conforme.
- **Ordre des langues A-1..A-3** : conforme à la Table A-3.
- **Bit 7 des paramètres de ESC D / ESC Z** : le TR (pp. 32 et 78) dit qu'un
  `ESC D` envoyé depuis Applesoft ferme aussi A-8, parce que le bit 7 arrive.
  POM2 le masque avant l'analyseur. C'est **VOULU** et documenté (DEV.md « What
  the guest cannot reach »), mais la justification « neither bit 7 is wired »
  est périmée, puisque le cas CR consulte A-8 (`ImageWriter.cpp:1681`). Pas de
  constat, faute de scénario net : le détecteur `AutoFeed` absorbe la
  différence dans les cas courants.
- **Bornes de ESC H [1", feuille]** : **VOULUES** (DEV.md). Le TR accepte
  0001-9999 ; les étiquettes d'un pouce (son propre exemple, p. 61) passent.
- **ESC c éjecte la feuille, `spacesToZeros` limité aux chiffres, `msb`
  rétabli selon B-6 après ESC V / ESC U** : **VOULUS** et documentés sur place
  (`:486-494`, `:1724-1732`, `:886-895`).
- **Proportionnel + condensé** (POM2 double `actcpi_`, la référence divise
  `horizPoints`) : **latent**, seule la lignée Epson pose `kStyleCondensed`.
- **Cisaillement italique** : seul le chemin Epson pose `kStyleItalics` ; aucun
  scénario sur le chemin C. Itoh.
- **Règles de géométrie dupliquées** : `PrinterScreenDump` choisit `ESC n/E/p/P`
  d'après la Table 8-2 et descend d'une bande par `ESC T 16` (8 points à
  72 dpi), avec CR + LF en fin de bande. Ce flux reste juste avec le détecteur
  `AutoFeed` comme avec l'insertion de CR du constat 2. `ImageWriterPdf` ne
  dérive sa géométrie que du `dpi` de la page, et `PrinterRender` repasse par
  `ImageWriter`. Aucune ne réimplémente pas, marges, demi-pas ou densités. La
  seule règle tenue deux fois est la paire face / cadence du constat 4.
- **Chemins Epson (`ImageWriter_EscP.cpp`) et Diablo** : hors tranche (autre
  lignée, autres manuels). Non audités.

---

## Bilan

- **Citations examinées** : 24 sur 24, confrontées à greg-kennedy
  `d0d70958` (= GSport `efcba1b4`) et datées sur 21 révisions (16 GSport,
  5 greg-kennedy). S'y ajoutent, hors motif du brief, 4 citations
  `iw_charmaps.h` et une plage sans préfixe (`1222-1252`), toutes OK.
- **Verdicts** : OK 22 (dont deux avec réserve de formulation : « ported
  verbatim » en `:1502`, « matching » en `:820`) · INEXACTE 1 (`h:51`, le
  motif) · DÉCALÉE 1 (`h:763` → 735 et 757) · ABSENTE 0 · OK (rev X) 0 ·
  NON-PINNÉE 0.
- **Constats** : CRITIQUE 0 · MAJEUR 0 · MINEUR 5 (ligne pleine sans A-6 ;
  LF sans insertion de CR ; colonnes proportionnelles à 10/12 au lieu de 9/10 ;
  police par défaut correspondance au lieu de draft ; ESC M / ESC m ignorés).
  AMONT FAUX 4 (gras au reset, perforation A-5 à 0,25", HT au dernier taquet,
  ESC 1..6 absolu). Les cinq MINEUR sont hérités de la référence et contredits
  par le manuel d'Apple.
- **Révision de référence** : pas de pin dans le dépôt. Elle n'est pas
  ambiguë pour autant : greg-kennedy/ImageWriter n'a qu'un seul contenu de
  `imagewriter.cpp`, identique au HEAD GSport `efcba1b4`. À épingler quand même
  dans `ImageWriter.h` et DEV.md, avec la mention GSport.
- **Erreurs de provenance trouvées** : la file du brief dit « MAME », et
  `docs/printer_plan.md` l. 24 et 52 disent « AppleWin ». L'amont est
  greg-kennedy/GSport.
- **Ce que je n'ai pas pu vérifier** :
  - le *ImageWriter LQ Reference Manual* (ESC C / U / h / t, 216 dpi), non
    récupéré : la citation `:1480` reste en grade D ;
  - le contenu de la Table 4-3 et de l'annexe B (glyphes nationaux et
    MouseText) ;
  - le comportement réel d'une ligne graphique trop longue : le TR n'en dit
    qu'une phrase générale (p. 34), d'où le PLAUSIBLE du constat 1 sur ce
    volet. Je n'ai aucune capture matérielle ;
  - quels logiciels Apple II réels déclenchent les constats 1, 2 et 5 (lignes
    LF seules, lignes trop longues, `ESC M`) : rien d'inventorié ;
  - le comportement de `ESC m` / `ESC M` sur l'ImageWriter I et le DMP ;
  - le texte du TR est l'OCR d'archive.org : les citations ont été relues dans
    le texte, mais pas recoupées avec le scan page par page ;
  - je n'ai pas lancé la suite `ctest`. La sonde compile contre
    `ImageWriter.cpp`, `ImageWriter_CustomChars.cpp`, `ImageWriter_EscP.cpp`
    et `hgrpaint/HgrFont.cpp`, comme `test_imagewriter`.

### Sonde

`/tmp/audit_imagewriter/probe.cpp`, compilée par
`clang++ -std=c++17 -O1 -Isrc -Itests -Ibuild/generated probe.cpp
src/ImageWriter.cpp src/ImageWriter_CustomChars.cpp src/ImageWriter_EscP.cpp
src/hgrpaint/HgrFont.cpp build/libpom2_core_test.a -framework CoreAudio
-framework AudioToolbox -framework AudioUnit`. Elle envoie les flux décrits par
l'API publique (`printBytes`, `status()`, `quality()`, `currentPage()`).
Sortie complète :

```
1a. 100 x 'X' sans CR, 12 cpi : head Y = 0.1667 in, encre y 0..37 (38 px de haut)
1b. ESC R 100 '_'           : head Y = 0.1667 in, encre y 16..41
1c. ESC D SP NUL puis 100 'X': head Y = 0.1667 in
2a. "ABC" LF "DEF" (AutoFeed Off): head X = 0.7500 in (marge gauche 0.25)
2b. "ABC" LF "DEF" (AutoFeed Auto): head X = 0.7500 in (marge gauche 0.25)
2c. ESC l 0 "ABC" LF        : head X = 0.5000 in
3a. ESC p ESC L 009 : marge = 1.1500 in (manuel : 0.25 + 9/9 = 1.2500)
3b. ESC P ESC L 010 : marge = 1.0833 in (manuel : 0.25 + 10/10 = 1.2500)
3c. ESC p ESC ( 010. HT : tête = 1.1500 in (manuel : 0.25 + 9/9 = 1.2500)
3d. témoin ESC E ESC L 012 : marge = 1.2500 in (manuel : 0.25 + 12/12 = 1.2500)
4a. police à la construction : Correspondence (manuel : Draft)
4b. ESC a 2 puis ESC c        : Correspondence (manuel Table A-2 : Draft)
4c. style après ESC c        : normal (manuel : boldface Off)
5a. ESC M : Correspondence (manuel : NLQ)
5b. ESC a 2 puis ESC m : NLQ (manuel : Correspondence)
5c. encre "HELLO" : ESC a 2 = 186 px, ESC M = 220 px
```
