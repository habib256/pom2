# Brief permanent — audit de parité amont de POM2

Pour un agent Opus sous Cursor. **Une tranche par conversation.** Les tranches
sont indépendantes et peuvent tourner en parallèle.

Pour lancer une tranche, il suffit de dire :

> Lis `docs/audits/BRIEF-parite-amont.md` et traite la tranche **N**.

---

## Le but

L'objectif du projet est un émulateur qui se comporte **le plus exactement
possible comme le silicium**. Tout le reste en découle, y compris ce brief.

POM2 cite MAME en commentaire quand il en porte du code, et ces citations
n'avaient jamais été re-vérifiées. C'est le prétexte de l'audit, pas son but.
**Le but est de trouver des bugs** : des endroits où POM2 ne se comporte pas
comme le matériel.

### La hiérarchie des sources

Elle n'est pas négociable, et **MAME n'est pas en haut** :

1. **Le silicium** — mesures sur machine réelle, comportement observable.
2. **Les documents primaires** — fiches techniques des puces, Sather
   (*Understanding the Apple IIe*), les références techniques Apple, les
   spécifications (IWM rev 19, WOZ/MOOF/2IMG), les brevets.
3. **Le désassemblage et l'analyse des ROM** — firmware //c/+, RWTS, les ROM
   de cartes. POM2 en tire beaucoup : la polarité `$CE40`/`$CE60` du MIG vient
   de la ROM //c+ (`$E603`), pas de MAME, et **contredit MAME**.
4. **Les émulateurs** — MAME *et* AppleWin, à égalité. Ce sont des lectures du
   matériel faites par d'autres, utiles et faillibles. AppleWin a raison là où
   MAME a tort, et réciproquement.

Conséquences pratiques :

- Une divergence avec MAME justifiée par un niveau supérieur **n'est pas un
  bug**. Classe-la `VOULUE` et cite la justification.
- Quand MAME et AppleWin se contredisent, **dis-le** et indique lequel colle
  aux documents matériels. Ne tranche pas en silence.
- **Consulte AppleWin même pour une tranche MAME** quand le point est
  délicat : c'est une seconde lecture gratuite du même matériel.
- Quand POM2 a raison contre l'amont, c'est un résultat : ouvre un constat
  **`AMONT FAUX`** (même format, gravité `—`). Deux ont déjà été trouvés — les
  ticks Q3 de l'IWM, la polarité MIG. Ils protègent le code contre un futur
  « alignement sur MAME » qui serait une régression.

---

## La file des tranches

| # | Fichier amont | Cit. | Dépôt | État |
|---|---|---:|---|---|
| 1 | `iwm.cpp` | 55 | MAME | ✅ `parite-iwm-2026-11-01.md` — 1 MAJEUR confirmé |
| 2 | `apple2e.cpp` | 82 | MAME | ✅ `parite-apple2e-2026-11-01.md` — 0 constat, 3 INEXACTES, 9+ époques amont mêlées |
| 3 | `z80scc.cpp` | 107 | MAME | ✅ `parite-z80scc-2026-11-01.md` — 1 MAJEUR + 1 MINEUR confirmés, pin périmé |
| 4 | `cs8900a.cpp` | 38 | MAME | ⬜ |
| 5 | `mos6551.cpp` | 33 | MAME | ⬜ |
| 6 | `grappler.cpp` | 32 | MAME | ⬜ |
| 7 | `Uthernet2.cpp` | 46 | **AppleWin** | ⬜ `source/`, pas `src/devices/` |
| 8 | `imagewriter.cpp` | 24 | MAME | ⬜ |
| 9 | `floppy.cpp` | 24 | MAME | ⬜ |
| 10 | `NTSC.cpp` | 23 | **AppleWin** | ⬜ |
| 11 | `6522via.cpp` | 20 | MAME | ⬜ attention : biais T1 voulu, voir plus bas |
| 12 | `ay8910.cpp` | 17 | MAME | ⬜ |
| 13 | `apple2.cpp` | 14 | MAME | ⬜ |
| 14 | `a2ssc.cpp` | 14 | MAME | ⬜ |
| 15 | `upd1990a.cpp` | 13 | MAME | ⬜ |
| 16+ | 67 autres fichiers | ~260 | mixte | ⬜ par paquets de 3-4 |

**801 citations, 82 fichiers amont.** `ow65c02.lst` est la table d'opcodes
65C02 → `src/devices/cpu/m6502/`.

**Ne modifie PAS ce fichier.** Plusieurs tranches tournent en parallèle et
s'écraseraient mutuellement. Ton rapport porte son propre bilan ; la file est
tenue à jour en dehors de toi.

---

## Marche à suivre

### 1. Lister

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep '<FICHIER_AMONT>:'
```

### 2. Établir la révision amont — leçon de la tranche 1

**Ne suppose pas qu'il y en a une seule.** L'audit `iwm.cpp` a montré que les
citations de POM2 mélangent **au moins trois révisions amont**, et que
certaines ne collent à aucune. C'est le défaut structurel que cet audit a pour
mission de cartographier.

Donc :

1. Cherche un pin dans l'en-tête du fichier POM2 et dans `DEV.md` (commit, tag,
   date). Note-le.
2. Rends le verdict contre le tag **`mame0287`** (ou le `master` d'AppleWin)
   quand aucun pin n'existe.
3. **Quand une citation ne tombe pas juste sur la révision de référence,
   cherche sur quelle révision elle tombe juste** et dis-le. C'est l'information
   la plus utile du rapport : elle date la citation.
4. Ouvre ton rapport par un tableau « Révision → ce qu'elle contient → citations
   qui tombent juste », sur le modèle de `parite-iwm-2026-11-01.md`.

**Méthode de datation, mise au point par la tranche 2** (82 citations datées sur
91 révisions — lis `parite-apple2e-2026-11-01.md` § Bilan avant de commencer) :

1. Récupère **tout l'historique** du fichier amont, pas deux ou trois révisions :
   `https://api.github.com/repos/mamedev/mame/commits?path=<chemin>&per_page=100`
   puis le blob de chacune.
2. Pour chaque citation, écris **une ancre regex** : le code que POM2 décrit, pas
   son numéro de ligne. Par exemple `0x3bbf\] == 0x05`, ou
   `::recalc_active_device\(`.
3. Une révision « tombe juste » quand la plage citée **contient** l'ancre. Script
   la boucle ; à la main, 82 citations × 91 révisions n'est pas tenable.
4. Une citation qui ne tombe juste sur **aucune** révision est un signal fort :
   dis-le explicitement. La tranche 2 en a trouvé 19, tout le bloc SmartPort —
   des numéros qui n'ont peut-être jamais existé.

Attention au chemin amont : il a pu bouger. `apple2e.cpp` est dans
`src/mame/apple/` depuis 2022-06, `src/mame/drivers/` avant.

URLs :

- MAME — `https://raw.githubusercontent.com/mamedev/mame/<ref>/src/devices/<chemin>`
- AppleWin — `https://raw.githubusercontent.com/AppleWin/AppleWin/<ref>/source/<chemin>`

### 3. Lire le code POM2, pas seulement le commentaire

C'est l'objet réel de l'audit. Compare : ordre des effets de bord, conditions
de bord, largeurs et débordements, timing en cycles, état après reset,
registres en lecture seule / écriture seule, drapeaux, valeurs par défaut.

**Cherche en priorité les règles implémentées deux fois dans POM2.** Le bug de
la tranche 1 était exactement ça : `IWMDevice` appliquait la règle amont,
l'ombre IWM de `DiskIICard` ne l'appliquait pas. Quand le dépôt se contredit
lui-même, pas besoin d'arbitrer contre MAME.

### 4. Sonder quand c'est possible

Une sonde qui compile et qui tourne transforme un `PLAUSIBLE` en `CONFIRMÉ`.
Écris-la **hors du dépôt** (`/tmp/...`), en te greffant sur le harnais d'un
test existant de `tests/`, et colle sa sortie dans le rapport.

Pense à `POM2_MEDIA_WRITE_DEFAULT=protected` : ctest le pose pour tous les
tests, une sonde qui boote une image suivie doit faire pareil.

---

## Divergences VOULUES — ne pas signaler

POM2 s'écarte de l'amont délibérément, chaque fois documenté. Avant d'ouvrir un
constat, vérifie qu'il n'est pas ici, **et relis la justification à l'endroit
indiqué** : elle couvre souvent plus de cas que la ligne ci-dessous.

| Divergence voulue | Justification |
|---|---|
| Horloge CPU PAL 1 015 625 Hz, pas le `A2PAL_1M_CLOCK = 1016966` de MAME | `CpuClock.h` — MAME porte un `FIXME` dessus |
| Biais de relecture du T1 du 6522 à **-1**, pas celui de MAME | `DEV.md` § Mockingboard — l'oracle est TRIBU |
| Sync MMIO Mockingboard à `syncToCpuCycle()+1` | `DEV.md` — le cycle de donnée en vol |
| `lssSync` sans la compensation d'offset de MAME | `DEV.md` § Storage |
| Boot PROM et séquenceur Disk II propres à POM2 | `DiskIIBootRom.h`, `DiskIICard::ownSequencerStep` |
| Crochets IWM d'un Disk II conditionnés par `setIwmHost()` | `CLAUDE.md` § conventions |
| IWM sur sa propre horloge 7,16 MHz | `CpuClock.h`, `POM2_IWM_TICKS_PER_CPU_CYCLE` |
| **Q3 non modélisé dans l'IWM** | Établi tranche 1 : à 0,09 % près, et c'est **MAME qui s'écarte du silicium** sur le timer d'arrêt moteur |
| Bus étranger : aucune branche ajoutée dans `M6502` | `docs/PERFORMANCE.md` §§ 8.2/8.5 |
| SmartPort au niveau protocole, firmware UniDisk hors périmètre | `DEV.md` § SmartPort |
| Tout seam HLE assumé | **`docs/lle_vs_hle.md` — à lire en entier d'abord** |

Règle générale : **une divergence documentée est une décision, pas un bug.** Si
tu en trouves une justifiée ailleurs, classe-la `VOULUE` et cite la source.

---

## Le rapport

Un fichier : **`docs/audits/parite-<fichier-amont-sans-extension>-2026-11-01.md`**.
C'est le seul fichier du dépôt que tu as le droit de créer. **Tu ne modifies
aucun fichier de code.**

### Section « Révision amont »

Le tableau décrit en § 2.

### Section « Rapport 1 — Citations »

Une ligne par citation : `fichier POM2:ligne` · citation · verdict ·
commentaire d'une phrase. Trié par gravité.

| Verdict | Sens |
|---|---|
| `OK` | La plage citée contient bien le code décrit, sur la révision de référence. |
| `OK (rev X)` | Juste, mais sur une autre révision — nomme-la. |
| `DÉCALÉE` | Le code existe, pas à ces lignes. Donne les bonnes. |
| `ABSENTE` | Le code décrit n'est plus dans le fichier amont. |
| `INEXACTE` | Les lignes existent mais ne disent pas ce que POM2 prétend. **Le plus grave** : explique l'écart. |
| `NON-PINNÉE` | Verdict impossible faute de révision de référence. |

**Pour chaque citation, donne aussi la ligne corrigée au format du dépôt**
(`CLAUDE.md`, en tête des conventions) : un crochet de grade, la source, la
révision. Un portage d'émulateur est un **grade D** :

```
// [D] MAME iwm.cpp:289-302 @ mame0287 — mode_w ne prend le mode qu'à l'arrêt.
```

Si en cherchant tu trouves mieux qu'un émulateur — la fiche technique de la
puce, la ROM, une mesure — donne la ligne au grade correspondant (B, C ou A) :
**faire monter une citation en grade vaut mieux que la réparer.** C'est le
livrable qui se transforme en commit.

### Section « Rapport 2 — Constats »

Le livrable principal. Pour chaque divergence de comportement, et **seulement**
si tu peux écrire le scénario d'échec :

```
### <titre court et factuel>
Fichier      : src/<fichier>:<ligne>
Amont        : <fichier>:<lignes> @ <révision>
Gravité      : CRITIQUE | MAJEUR | MINEUR
Confiance    : CONFIRMÉ (lu dans les deux sources, sonde à l'appui)
               | PLAUSIBLE (doute résiduel — dis lequel)
Divergence   : ce que fait l'amont ; ce que fait POM2.
Scénario     : entrées ou état concrets → comportement faux observable.
               Pas de scénario → pas de constat. Tu le jettes.
Observation  : comment le voir — un test de tests/, une sonde, une image disque.
```

### Section « Pistes examinées et écartées »

**Obligatoire.** Ce que tu as regardé et qui s'est révélé sain, en une ligne
chacune. Ça évite qu'un futur audit rouvre les mêmes fausses pistes, et c'est
la moitié de la valeur du rapport.

### Section « Bilan »

Citations examinées · répartition des verdicts · constats par gravité ·
révision amont utilisée · **et ce que tu n'as pas pu vérifier**.

---

## Ce que tu ne signales pas

- Le style, le nommage, la mise en forme, les `const` manquants.
- « MAME fait autrement » sans conséquence observable.
- Les divergences voulues ci-dessus.
- Tout ce dont tu ne peux pas écrire le scénario d'échec.

Mieux vaut trois constats solides que vingt soupçons. Tu es jugé sur le taux de
vrais positifs, pas sur le volume.

**Zéro constat est un résultat valide et attendu.** La tranche 2 a examiné 82
citations et n'a trouvé aucun bug : le code était fidèle. Rends ce verdict-là
sans le farder. N'invente jamais un constat pour justifier la tranche — un faux
positif coûte plus cher que le silence, parce qu'il envoie quelqu'un vérifier.

## Honnêteté

Une citation que tu n'as pas pu vérifier n'est pas `OK` — dis que tu n'as pas
pu. Si tu n'as pas réussi à récupérer le fichier amont, écris-le au lieu de
raisonner de mémoire : **ta mémoire du source MAME n'est pas une source.**

La section « ce que je n'ai pas pu vérifier » du bilan vaut autant que les
constats. La tranche 1 y a gagné sa crédibilité en admettant n'avoir pas
désassemblé RWTS.
