# Brief permanent — audit de provenance des affirmations sur le matériel

Second chantier, de nature différente de `BRIEF-parite-amont.md`.

Celui-là vérifie des **citations** : le pointeur `fichier.cpp:ligne` désigne-t-il
encore le bon code, et POM2 se comporte-t-il comme ce code ?

Celui-ci vérifie des **affirmations sans pointeur** : le code dit quelque chose
sur le matériel — « le 558 n'est pas redéclenchable », « mesuré à 1,17 s »,
« MAME fait ceci », « approximativement » — et rien ne permet de le recouper.

Pour lancer une tranche :

> Lis `docs/audits/BRIEF-provenance.md` et traite la tranche **N**.

---

## Le but

L'objectif du projet est un émulateur qui se comporte **le plus exactement
possible comme le silicium**. Une affirmation sur le matériel que personne ne
peut recouper est une dette : elle peut être juste, elle peut être fausse depuis
le premier jour, et rien dans le dépôt ne permet de le savoir.

Le livrable est double :

1. **Un registre de provenance** — pour chaque affirmation, le grade de preuve
   qui la soutient réellement.
2. **La liste de ce que POM2 ne sait pas** — les constantes ajustées, les
   conventions sans source, les endroits où un contrat a été modélisé à la
   place du silicium. Cette liste **est** un livrable, pas un échec.

Et, chemin faisant, les bugs : une affirmation contredite par le document
qu'elle invoque est un bug jusqu'à preuve du contraire.

---

## Les grades de preuve

Ils sont désormais **une convention du dépôt** (`CLAUDE.md`, en tête des
conventions), pas une invention de ce brief : toute affirmation sur le matériel
porte son grade et sa source dans un crochet en tête de commentaire —
`// [B] MC6845 datasheet § 4.2 — …`, `// [D] MAME mc6845.cpp:237-252 @ mame0287 — …`.

**Ton registre propose, pour chaque site, la ligne corrigée sous cette forme.**
C'est le livrable qui se transforme en commit.


| Grade | Ce qui soutient l'affirmation |
|---|---|
| **A** | Mesure sur **machine réelle** — oscilloscope, capture, exécution sur le fer. |
| **B** | **Document primaire** — fiche technique de la puce, Sather, référence technique Apple, spécification (IWM rev 19, WOZ/MOOF/2IMG), brevet, note technique Apple (TIL). |
| **C** | **Désassemblage de ROM** — firmware //c/+, RWTS, ROM de carte. Preuve de ce que le logiciel *attend*, donc du contrat ; pas directement du silicium. |
| **D** | **Un autre émulateur** — MAME, AppleWin, izapple2, OpenEmulator, Virtual ][. Lecture du matériel faite par un tiers, utile et faillible. |
| **E** | **Affirmé sans source.** Le commentaire énonce un fait matériel, rien ne l'étaye. |
| **F** | **Inconnu assumé** — le code dit lui-même « guess », « arbitrary », « approximately », « unverified », « magic number ». |

Un grade D qui contredit un grade A, B ou C n'a pas raison. Un grade E n'est pas
forcément faux — il est invérifiable, ce qui est le problème.

**Fais monter en grade quand tu peux.** Le gain principal de cet audit n'est pas
de classer, c'est de **trouver le document** qui transforme un E en B. Beaucoup
de fiches techniques sont en ligne ; voir « Atteindre les documents » plus bas.

---

## L'inventaire, mesuré le 2026-11-01

```
grep -rn "<motif>" src/ --include='*.cpp' --include='*.h'
```

| Catégorie | Occurrences | Grade probable |
|---|---:|---|
| `guess` | 78 | F |
| `arbitrar*` | 31 | F |
| `approximat*` | 29 | F |
| `unverified` / `unchecked` / `not verified` | 28 | F |
| `tuned` / `tuning` | 19 | F |
| `empiric*` | 3 | F |
| `magic number` | 3 | F |
| `FIXME` / `TODO` / `XXX` | 111 | — (à trier) |
| `measured` | 138 | A **ou D** — c'est tout l'enjeu |
| `datasheet` / `data sheet` | 135 | B |
| `Technical Reference` | 28 | B |
| `Sather` / `Understanding the Apple` | 27 / 9 | B |
| `patent` | 19 | B |
| `specification` / `rev 19` | 11 | B |
| `TIL[0-9]` | 1 | B |
| `disassembl*` | 47 | C |
| MAME **sans** `fichier:ligne` | 1304 | D invérifiable |
| AppleWin **sans** `fichier:ligne` | 392 | D invérifiable |
| OpenEmulator | 48 | D |
| Virtual ][ | 37 | D |
| izapple2 | 29 | D |
| TomCh / Tom Harte | 15 | A ou B (tests matériels) |
| Yellowstone / BMOW | 19 | B ou C |

Les totaux se recoupent : une même ligne peut compter dans deux catégories.

---

## La file des tranches

Classées par **valeur décroissante par heure d'agent**, pas par volume.

| # | Tranche | Sites | Pourquoi d'abord | État |
|---|---|---:|---|---|
| P1 | **Les inconnus assumés** — `guess`, `arbitrary`, `approximately`, `empirical`, `magic number`, `tuned` | ~163 | Le code avoue déjà ne pas savoir. Rien à débusquer : il reste à chercher la source et à faire monter le grade. Rendement maximal. | ⬜ |
| P2 | **Les non-vérifiés déclarés** — `unverified`, `unchecked`, `not verified` | 28 | Mêmes raisons, et plusieurs portent sur des points de comportement ouverts (reset //c, IOUDIS). | ⬜ |
| P3 | **Les fiches techniques** — `datasheet`, `data sheet` | 135 | Le document existe et se trouve. Une affirmation contredite par la fiche est un bug. | ⬜ |
| P4 | **Sather + références techniques Apple** | ~64 | Mêmes raisons ; Sather est la référence du //e et il est en ligne. | ⬜ |
| P5 | **`measured`** | 138 | Mesuré **contre quoi** ? Un oscilloscope est un grade A, un autre émulateur un grade D. Rien ne les distingue aujourd'hui. | ⬜ |
| P6 | **Les désassemblages** — `disassembl` | 47 | Grade C vérifiable : la ROM est là, l'affirmation se recoupe. | ⬜ |
| P7 | **Les brevets et spécifications** | 30 | Peu nombreux, documents trouvables. | ⬜ |
| P8 | **AppleWin sans citation** | 392 | Convertir en citations `fichier:ligne`, ou déclasser. Gros volume, valeur unitaire faible. | ⬜ |
| P9 | **MAME sans citation** | 1304 | Idem, le plus gros morceau. À découper par sous-système. | ⬜ |
| P10 | **`FIXME` / `TODO` / `XXX`** | 111 | Tri : lesquels portent sur la fidélité matérielle, lesquels sont de la dette ordinaire. | ⬜ |

**Ne modifie pas ce fichier** — plusieurs tranches tournent en parallèle. Ton
rapport porte son propre bilan.

---

## Marche à suivre

1. **Liste les sites** de ta tranche avec le grep correspondant.
2. Pour chacun, lis **l'affirmation** et **le code qu'elle justifie**. Une
   affirmation qui ne justifie rien d'observable n'a pas besoin d'être
   recoupée — classe-la `DÉCORATIVE` et passe.
3. **Cherche la source.** C'est le cœur du travail, pas une formalité :
   - la fiche technique de la puce nommée ;
   - Sather, les références techniques Apple, les notes TIL ;
   - le code amont quand l'affirmation porte sur un émulateur — et **cite-le
     en `fichier:ligne` + révision**, ce qui fait rentrer le site dans le
     premier chantier ;
   - la ROM elle-même quand elle est dans `roms/`.
4. **Attribue le grade réel**, pas celui que le commentaire suggère.
5. Quand le document **contredit** le code : constat (format plus bas).
6. Quand tu ne trouves pas : dis-le. `E` assumé vaut mieux que `B` inventé.

### Atteindre les documents

Beaucoup de fiches techniques sont derrière un mur (403, certificat TLS
invalide, protection anti-robot). Le réflexe du dépôt est **scrapling**
(`/Users/gistair/src/scrapling/venv`) dès que `WebFetch` échoue, y compris pour
récupérer des octets bruts de `.pdf` ou `.zip`. Il a débloqué des documents que
plusieurs agents avaient déclarés inaccessibles.

Pour bitsavers : les fiches ne sont presque jamais dans un dossier au nom du
composant. Elles sont **dans des recueils** — Dallas est sous
`components/dallasSemiconductor/` sans sous-dossier par référence, NEC classe
ses LSI d'horloge parmi les circuits grand public. **Parcours l'index du
répertoire** (`Fetcher.get` + `css('a')`) au lieu de deviner les URL.

---

## Le rapport

Un fichier : **`docs/audits/provenance-P<N>-<sujet>-2026-11-01.md`**. C'est le
seul fichier du dépôt que tu as le droit de créer. **Tu ne modifies aucun
fichier de code.**

### Section « Registre »

| `fichier:ligne` | Affirmation (abrégée) | Grade | Source trouvée | Verdict |
|---|---|---|---|---|

Verdicts : `CONFIRMÉE` · `CONTREDITE` (→ constat) · `IMPRÉCISE` (juste mais le
commentaire dit plus que la source) · `INTROUVABLE` (aucune source trouvée,
reste au grade E/F) · `DÉCORATIVE` (ne justifie aucun comportement).

### Section « Constats »

Même format que le premier chantier — y compris **`AMONT FAUX`** quand POM2 a
raison contre un autre émulateur :

```
### <titre court et factuel>
Fichier      : src/<fichier>:<ligne>
Source       : <document, édition, page> — ou <fichier amont:lignes @ révision>
Gravité      : CRITIQUE | MAJEUR | MINEUR | — (pour AMONT FAUX)
Confiance    : CONFIRMÉ | PLAUSIBLE (dis le doute)
Divergence   : ce que dit la source ; ce que fait POM2.
Scénario     : entrées ou état concrets → comportement faux observable.
               Pas de scénario → pas de constat.
Observation  : comment le voir.
```

### Section « Ce que POM2 ne sait pas »

**La section la plus importante de ce chantier.** Les sites restés en E ou F,
groupés par sous-système, avec pour chacun : ce qui est affirmé, ce qui a été
cherché, et **ce qui trancherait** (quel document, quelle mesure, quel
désassemblage). C'est une feuille de route, pas un aveu.

### Section « Bilan »

Sites examinés · répartition des grades **avant / après** ta tranche (combien
de E sont devenus B ou C : c'est la mesure du travail) · verdicts · constats ·
et ce que tu n'as pas pu vérifier.

---

## Ce que tu ne signales pas

- Le style, le nommage, la mise en forme.
- Une affirmation juste dont le commentaire est seulement un peu vague — sauf
  si elle justifie une constante : là, la précision compte.
- Les divergences voulues listées dans `BRIEF-parite-amont.md`.
- Tout constat dont tu ne peux pas écrire le scénario d'échec.

## Honnêteté

**Ne fabrique jamais une source.** Un numéro de page inventé dans une fiche
technique est le pire résultat possible de cet audit : il transforme une
incertitude connue en fausse certitude, et personne ne le rouvrira. Si tu n'as
pas ouvert le document, tu ne l'as pas lu — dis-le, et laisse le site en
`INTROUVABLE`.

Ta mémoire d'une fiche technique n'est pas une source. Ta mémoire du source
MAME non plus.
