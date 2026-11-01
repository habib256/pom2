# Audit de parité — citations de MAME `grappler.cpp` (2026-11-01)

Tranche 6 du brief `docs/audits/BRIEF-parite-amont.md` : les cartes
imprimante Orange Micro Grappler (1981, `GrapplerClassicCard`, clé `grappler1`)
et Grappler+ (`GrapplerCard`, clé `grappler`). Toutes les citations
`grappler.cpp:<lignes>` sous `src/` :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep 'grappler.cpp:'
```

32 citations : 17 dans `GrapplerCard.cpp`, 5 dans `GrapplerCard.h`, 7 dans
`GrapplerClassicCard.cpp`, 3 dans `GrapplerClassicCard.h`. Aucun fichier de
code n'a été modifié. La sonde vit dans `/tmp/audit_grappler/probe.cpp`, le
script de datation dans `/tmp/audit_grappler/date.py`, les 13 révisions amont
dans `/tmp/audit_grappler/revs/`.

## Révision amont

**Pas de pin par commit.** Ce qui s'en approche :

- `DEV.md:8394` (§ Grappler+) : « Pinned against MAME `bus/a2bus/grappler.cpp`
  (**2026-07-28 audit**) ». Une date, pas une révision. Le fichier amont n'a
  pas bougé entre le 2025-11-10 et le 2026-08-12, donc cette date désigne
  `4373abe8ab`.
- `docs/printer-detection.md:9-10`, écrit le jour où `GrapplerClassicCard` est
  entré dans le dépôt (commit `9742fde4`, 2026-09-26) : « MAME `master` as of
  2026-09-26 (commit `f663f942`) ». À cette date, `grappler.cpp` vaut
  `ed697c3087`.
- Les en-têtes de `GrapplerCard.*` et `GrapplerClassicCard.*` ne portent aucun
  pin.

**`mame0287` = `4373abe8ab`, octet pour octet** (même MD5), et `ed697c3087`
n'en diffère que par deux lignes (294 et 302), sans décalage de numérotation.
Les deux « pins » implicites et le tag donnent donc **les mêmes numéros de
ligne** : les verdicts sont rendus contre `mame0287`.

Historique complet récupéré (13 révisions, `api.github.com/…/commits?path=…`)
et chaque citation confrontée à chaque révision par une ancre regex (nom de
fonction MAME + motif du code décrit), puis relue à la main.

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste (ancre dans la plage) |
|---|---|---|---|
| `958189731b` | 2021-04-06 | Création ; classes déclarées dans `grappler.h` | 0 |
| `3e28cafa55` | 2021-04-07 | `side_effects_disabled()` autour des effets de bord des lectures `$C0nX` de la Grappler 1981 | 0 |
| `299265cc52` → `5f4e3730d9` (8 révisions) | 2021-10-02 → 2024-10-17 | Classes déplacées dans le `.cpp` (+190 lignes) ; puis changements d'API sans décalage des zones citées (timers, `ATTR_COLD`, syntaxe `PORT_CHANGED_MEMBER`) | **6**, dont 4 qui ne tombent juste **que** là : `GrapplerClassicCard.cpp:53` (305-316, exact), `:73` (319-322), `:80` (121-124), `GrapplerClassicCard.h:25` (121-124) |
| `f135a4e417` | 2025-11-10 | `take_c800()` (+2 lignes à 64) et `reset_from_bus` dans la base | 9 |
| `4373abe8ab` = **`mame0287`** | 2025-11-10 | `reset_from_bus` déplacé : la Grappler 1981 relâche /STROBE, la Grappler+ remet son latch ACK à 1 (536-539) | **27** — les 22 de `GrapplerCard.*` sauf une (`:171`, décalée d'une ligne), et 6 de `GrapplerClassicCard.*` (dont `:53` au sens large) |
| `ed697c3087` = `master` | 2026-08-12 | Grappler 1981 : le bus flottant remplace les `0xf0U` / `0xffU` codés en dur dans `read_c0nx` | 27 (mêmes numéros que `mame0287`) |

En clair :

- **`GrapplerCard.*` est homogène** : 21 citations sur 22 tombent juste sur
  `mame0287`, la 22ᵉ est à une ligne près. L'« audit du 2026-07-28 » a bien été
  fait sur une seule révision.
- **`GrapplerClassicCard.*` mélange deux époques**, alors qu'il a été écrit le
  2026-09-26 contre un `master` déclaré : quatre citations ne tombent juste que
  sur l'état 2021-10 → 2024-10 (avant `f135a4e417`), une (`:24`, 424-435) que
  sur l'état ≥ 2025-11-10, et une (`:85`, 369-372) **sur aucune des 13
  révisions**.
- **Correctif postérieur au pin** : le seul, `ed697c3087` (bus flottant de la
  Grappler 1981), **est dans POM2** — `GrapplerClassicCard.cpp:42-48` renvoie
  `openBus() & 0xF0` | lignes et `openBus()` hors A0.

---

## Rapport 1 — Citations

Ordre : INEXACTE, DÉCALÉE, OK (rev X), OK. « Ligne corrigée » : la citation au
format du dépôt (`CLAUDE.md`), avec le grade C ajouté quand le firmware de la
carte confirme le contrat (désassemblé pour cet audit : Grappler+ 3.1 =
`roms/grappler_plus.bin`, Grappler 1981 = `roms/grappler_eps-1.bin`, adresses
CPU dans la fenêtre `$C800`).

| Fichier POM2:ligne | Citation | Verdict | Commentaire | Ligne corrigée |
|---|---|---|---|---|
| `src/GrapplerClassicCard.cpp:61` | `397-419` | **INEXACTE** | La plage contient la mise à 1 du latch sur front descendant de /STROBE quand /ACK est au repos (395-404). Mais la phrase suivante, « the printer takes the byte on the rising edge », n'est dans aucune ligne de `grappler.cpp`, et l'imprimante de MAME dit le contraire : `centronics_printer_device::input_strobe` prend l'octet sur le front **descendant** (`printer.cpp:119-129 @ mame0287`). C'est un choix de POM2 présenté comme un portage. Sans effet avec le firmware (voir Pistes). | `// [D] MAME grappler.cpp:395-404 @ mame0287 — front descendant de /STROBE : latch à 1 si /ACK au repos.` + `// [F] POM2 : l'imprimante prend l'octet au front MONTANT ; MAME printer.cpp:119-129 @ mame0287 le prend au front descendant. Tranché par : le chronogramme STROBE/BUSY d'une imprimante Centronics de l'époque.` |
| `src/GrapplerCard.cpp:171` | `559-570` | DÉCALÉE | La garde `if (m_ack_in)` est en 558 ; 559-570 n'en est que le corps. Aucune révision ne la place dans la plage. | `// [D] MAME grappler.cpp:558-570 @ mame0287 — une écriture ne remet le latch à 0 que si /ACK est au repos.` |
| `src/GrapplerClassicCard.cpp:85` | `369-372` | DÉCALÉE | **Ne tombe juste sur aucune des 13 révisions.** Sur `mame0287`, 369-372 est le bloc de commentaire « printer status inputs » ; le relâchement de /STROBE au reset est `reset_from_bus` 362-365 (appelé par `device_reset` 356-359). Sur ≤ `5f4e3730d9` c'était `device_reset` 354-357. | `// [D] MAME grappler.cpp:362-365 @ mame0287 — reset_from_bus relâche /STROBE (device_reset 356-359 l'appelle).` |
| `src/GrapplerClassicCard.cpp:53` | `305-316` | OK (rev `299265cc`–`5f4e3730`) | Exact sur ces révisions. Sur `mame0287`, `write_c0nx` occupe 307-318 : la ligne qui relâche /STROBE (317) sort de la plage. Le firmware confirme le contrat : données en A0, /STROBE bas par A1, relâché par A2. | `// [D] MAME grappler.cpp:307-318 @ mame0287 — A0 données, A1 /STROBE bas, sinon A2 /STROBE haut.` + `// [C] eps-1 $CBF5/$CBF8/$CC16 — STA $C081,Y / STA $C082,Y / STA $C084,Y.` |
| `src/GrapplerClassicCard.cpp:73` | `319-322` | OK (rev `299265cc`–`5f4e3730`) | `read_cnxx` (une page par slot) : `mame0287` 321-324. | `// [D] MAME grappler.cpp:321-324 @ mame0287 — rom[offset \| slot<<8].` |
| `src/GrapplerClassicCard.cpp:80` | `121-124` | OK (rev `299265cc`–`5f4e3730`) | `read_c800` : `mame0287` 123-126 (POM2 cite déjà 123-126 dans `GrapplerCard.cpp:237`). | `// [D] MAME grappler.cpp:123-126 @ mame0287 — la ROM entière en $C800 (banque 0).` |
| `src/GrapplerClassicCard.h:25` | `121-124` | OK (rev `299265cc`–`5f4e3730`) | Idem. | `// [D] MAME grappler.cpp:123-126 @ mame0287` |
| `src/GrapplerClassicCard.cpp:24` | `424-435` | OK | `set_ack_in` : /ACK bas met le latch à 0 (426). Tombe juste à partir de `f135a4e417`. | `// [D] MAME grappler.cpp:414-434 @ mame0287 — /ACK bas : latch à 0.` |
| `src/GrapplerClassicCard.cpp:38` | `278-302` | OK | `read_c0nx` 279-304 : effets de bord A1/A2 en lecture (283-289), statut sur A0. | `// [D] MAME grappler.cpp:279-304 @ mame0287 ; bus flottant d'ed697c3087 (2026-08-12).` + `// [C] eps-1 $CBCB-$CBDE — lit le statut en $C081,Y.` |
| `src/GrapplerClassicCard.h:19` | `223-446` | OK (plage large) | Contient toute la classe, mais déborde de 4 lignes avant (fin de `set_slct_in`) et de 12 après (en-tête de la base Grappler+). Plage exacte : 227-434. | `// [D] MAME grappler.cpp:227-434 @ mame0287 — a2bus_grappler_device.` |
| `src/GrapplerClassicCard.h:30` | `397-434` | OK | Le latch vaut 1 en attente, 0 une fois l'octet acquitté : `set_strobe` + `set_ack_in`. | `// [D] MAME grappler.cpp:395-434 @ mame0287` |
| `src/GrapplerCard.h:96` | `586-591` | OK | `write_cnxx` : conflit de bus, banque remise à 0. | `// [D] MAME grappler.cpp:586-591 @ mame0287` |
| `src/GrapplerCard.h:101` | `64` | OK | `take_c800() { return true; }` (existe depuis `f135a4e417`). | `// [D] MAME grappler.cpp:64 @ mame0287` |
| `src/GrapplerCard.h:128` | `498-511` | OK | `INPUT_PORTS_START(grapplerplus)` ; le `INPUT_PORTS_END` est en 512. | `// [D] MAME grappler.cpp:498-512 @ mame0287` |
| `src/GrapplerCard.h:178` | `811-819` | OK | `ack_latch_set` lève l'IRQ si elle est autorisée. | `// [D] MAME grappler.cpp:811-819 @ mame0287` |
| `src/GrapplerCard.h:192` | `699-707` | OK | Statut Grappler+ : niveaux bruts des broches. | `// [D] MAME grappler.cpp:699-708 @ mame0287` + `// [C] Grappler+ 3.1 $CDE1-$CDED — statut lu en $C080,Y.` |
| `src/GrapplerCard.cpp:77` | `699-709` | OK | Idem. Le désassemblage `$CD89-$CD95` déjà cité sur place est un grade C : à marquer comme tel. | `// [D] MAME grappler.cpp:699-708 @ mame0287` + `// [C] Grappler+ 3.1 $CD89-$CD95 — teste SELECT, puis attend le bit ACK.` |
| `src/GrapplerCard.cpp:162` | `547-575` | OK | `write_c0nx` de la base : données si `!(offset & 3)`, banque haute sur A0. | `// [D] MAME grappler.cpp:547-575 @ mame0287` + `// [C] Grappler+ 3.1 $CDAC STA $C080,Y (données) ; $CFB8/$CFC7/$CFDB STA $C081 (banque haute).` |
| `src/GrapplerCard.cpp:163` | `711-745` | OK | Surcouche A1/A2 (désactive / autorise l'IRQ). | `// [D] MAME grappler.cpp:711-745 @ mame0287` |
| `src/GrapplerCard.cpp:168` | `795-808` | OK | `data_latched` arme le timer de /STROBE (806). Le `839-849` qui suit (sans préfixe, donc non compté) est bien `update_strobe`. | `// [D] MAME grappler.cpp:795-808, 839-849 @ mame0287` |
| `src/GrapplerCard.cpp:179` | `795-808` | OK | Masque du bit 7 par S1:1 (799). | `// [D] MAME grappler.cpp:795-799 @ mame0287` |
| `src/GrapplerCard.cpp:186` | `559-570` | OK | `m_ack_latch = 0U` (564). | `// [D] MAME grappler.cpp:558-570 @ mame0287` |
| `src/GrapplerCard.cpp:216` | `578-583` | OK | `read_cnxx` : banque à 0, A6 forcé à 0 en `$Cn80-$CnFF` quand le latch est à 0. | `// [D] MAME grappler.cpp:578-583 @ mame0287` |
| `src/GrapplerCard.cpp:237` | `123-126` | OK | `read_c800`. | `// [D] MAME grappler.cpp:123-126 @ mame0287` |
| `src/GrapplerCard.cpp:237` | `160-165` | OK | `set_rom_bank`. | `// [D] MAME grappler.cpp:160-165 @ mame0287` |
| `src/GrapplerCard.cpp:247` | `536-539` | OK | `reset_from_bus` de la Grappler+ : latch ACK seul (voir Pistes, l'IRQ au Ctrl-Reset). | `// [D] MAME grappler.cpp:536-539 @ mame0287` |
| `src/GrapplerCard.cpp:249` | `777-787` | OK | `device_reset` : IRQ désactivée et ligne relâchée. | `// [D] MAME grappler.cpp:777-787 @ mame0287` |
| `src/GrapplerCard.cpp:263` | `586-591` | OK | `write_cnxx`. | `// [D] MAME grappler.cpp:586-591 @ mame0287` |
| `src/GrapplerCard.cpp:272` | `811-819` | OK | `ack_latch_set`. | `// [D] MAME grappler.cpp:811-819 @ mame0287` |
| `src/GrapplerCard.cpp:273` | `822-831` | OK | `ack_latch_cleared`. | `// [D] MAME grappler.cpp:822-831 @ mame0287` |
| `src/GrapplerCard.cpp:274` | `715-745` | OK | A1 abaisse, A2 lève. | `// [D] MAME grappler.cpp:715-745 @ mame0287` |
| `src/GrapplerCard.cpp:275` | `777-787` | OK | Le reset abaisse l'IRQ. L'invariant `m_irq == ack && !disable` annoncé à `:276` tient pour toutes les transitions listées ; `reset_from_bus` (536-539), non listé, le casse côté MAME (voir Pistes). | `// [D] MAME grappler.cpp:777-787 @ mame0287` |

---

## Rapport 2 — Constats

### 1. Hors ligne, la Grappler+ lit BUSY bas — la Grappler 1981 branchée sur la même imprimante le lit haut

```
Fichier      : src/GrapplerCard.cpp:110-119 (lineBits) ; doc à GrapplerCard.h:194-199
Amont        : pas MAME (grappler.cpp:699-707 @ mame0287 ne fait que lire la
               broche). Référence : manuel Epson FX, Table K-1 (BUSY « goes HIGH
               … 3) When Off-Line ») et Table K-2 (OFF-LINE → ERROR LOW, BUSY
               HIGH, ACKNLG not generated) ; même table dans le FX-80 Operation
               Manual (« Relations among ON-LINE, SLCT IN, DC1/DC3 and Interface
               Signal »). Et le dépôt lui-même : CentronicsPrinter.h:24-25,
               busyLine().
Gravité      : MINEUR
Confiance    : CONFIRMÉ (la contradiction est lue dans les deux modèles POM2,
               figée en sens opposés par deux tests, et visible à la sonde).
               Doute résiduel : la source est le manuel Epson FX ; je n'ai pas
               trouvé la spécification parallèle de l'Apple Dot Matrix / C. Itoh
               8510 que POM2 met par défaut derrière la carte.
Divergence   : GrapplerCard calcule BUSY = busy || paperOut et laisse BUSY bas
               quand l'imprimante est seulement hors ligne. CentronicsPrinter
               (Grappler 1981, PIC) calcule BUSY = !connected || busy ||
               paperOut || !online. Une même option, `--printer-port
               N:online=off`, donne donc deux niveaux de BUSY selon la carte, et
               c'est le second qui suit le manuel de l'imprimante.
Scénario     : configuration par défaut (Grappler+ en slot 1). L'utilisateur
               décoche « on line » dans le panneau ImageWriter, ou lance POM2
               avec `--printer-port 1:online=off`. Un programme lit $C090 :
               $51 (BUSY 0, PE 0, SELECT 0, ACK 1). Un pilote qui suit la règle
               d'Epson (« transfer only … when the level of the BUSY signal is
               LOW », note 4 de la Table K-1), ou qui attend BUSY comme le fait
               le firmware de la Grappler 1981 ($CBD9), conclut que l'imprimante
               est prête et envoie l'octet. POM2 le met en attente, l'ACK ne
               vient pas, et le pilote reste bloqué là où un vrai bureau l'aurait
               fait attendre sur BUSY. Le firmware 3.1 de la Grappler+ n'est pas
               touché : il teste SELECT avant tout ($CD8C).
Observation  : sonde ci-dessous, cas [1]. Côté tests :
               grappler_printer_state_test.cpp:83 fige « offline » = 0x01 pour
               la Grappler+ ; parallel_cards_test.cpp:116 fige « Grappler
               offline: BUSY, no SELECT » = 0x08 pour la Grappler 1981.
```

Correction suggérée (non appliquée) : dans `lineBits`, BUSY =
`printerBusy() || out || !online()`, comme `CentronicsPrinter::busyLine`, et
corriger la ligne « offline » de `grappler_printer_state` (0x01 → 0x09 ; « paper
out + offline » vaut déjà 0x0D). Côté firmware 3.1, rien ne change : il ne lit
pas BUSY.

### Sonde

`/tmp/audit_grappler/probe.cpp`, compilée contre `build/libpom2_core_test.a` avec
les deux vraies ROM. [1] met les deux cartes hors ligne et lit le statut ; [2]
autorise l'IRQ de la Grappler+ ($C095 = A0|A2, sans écriture de donnée) puis
appelle `onReset()` (Ctrl-Reset) ; [3] lit `$C1C0` avec le latch ACK à 0 puis à
1 ; [4] fait un reset de la Grappler 1981 pendant que /STROBE est bas.

```
[INFO] Grappler: Loaded Grappler+ ROM: roms/grappler_plus.bin
Grappler+ ROM chargee : 1
Grappler 1981 ROM chargee : 1
[1] hors ligne : Grappler+ $C090=51 -> BUSY=0 PE=0 SELECT=0
[1] hors ligne : Grappler 1981 $C091=F9 -> BUSY=1 PE=0 SELECT=0
[2] apres $C095 (enable IRQ) : status=D3 bit7=1
[2] apres onReset (Ctrl-Reset) : status=53 bit7=0 ; banque $C800 toujours haute : 1
[3] $C1C0 ACK=0 -> 29 (rom[$80]=29) ; ACK=1 -> D0 (rom[$C0]=D0)
[4] 1981 : octets imprimes par le front montant du RESET : 1
```

(Le quartet haut `F` de la Grappler 1981 est le bus flottant : la sonde n'a pas
de `Memory` derrière le `SlotBus`, donc `openBus()` vaut $FF.)

---

## Pistes examinées et écartées

- **Le Ctrl-Reset désactive l'IRQ ACK de la Grappler+** — divergence confirmée
  (sonde [2]), mais **latente**. `GrapplerCard::onReset` (Ctrl-Reset, selon
  `SlotPeripheral.h:132-139`) applique à la fois `reset_from_bus` (latch ACK, 536-539)
  et `device_reset` (IRQ désactivée, 777-787) ; MAME ne fait que le premier sur
  un RESET du bus. POM2 a pourtant de quoi séparer les deux (`onPowerCycle`), et
  son propre commentaire (`:250-253`) déduit de `reset_from_bus` que la bascule
  de banque n'est pas câblée au RESET, sans appliquer le même raisonnement à
  la bascule d'IRQ U2A. Pas de scénario aujourd'hui : un balayage du firmware
  3.1 (accès absolus indexés à `$C08x`) ne trouve que `$C080` et `$C081`,
  jamais `$C082` / `$C084` ; il faut un pilote tiers piloté par IRQ qui survive
  au Ctrl-Reset. Le silicium n'est pas tranché, et MAME n'est pas cohérent non
  plus : après `reset_from_bus` avec l'IRQ autorisée, latch = 1 mais `m_irq`
  n'est pas réévalué. À trancher sur le schéma (U2A est-il relié à /RST ?).
- **Grappler 1981 : l'octet part au front montant de /STROBE, pas au
  descendant** (voir la citation INEXACTE). Latent : le firmware eps-1 écrit la
  donnée, abaisse puis relâche /STROBE sans lire le statut entre les deux
  (`$CBF5`, `$CBF8`, `$CC16`), et chaque impulsion imprime un octet dans les deux
  lectures. Un reset pendant /STROBE bas imprime l'octet au reset (sonde [4])
  là où l'imprimante MAME l'a pris au front descendant : même total.
- **Grappler 1981 : front descendant de /STROBE sans la garde `m_ack_in`, et
  /ACK remontant pendant /STROBE bas qui remet le latch à 1 (428-432)** : non
  modélisés, sans effet. Chez POM2, /ACK est une impulsion instantanée, donnée
  au front montant de /STROBE ou par `releasePending` quand l'imprimante revient
  en ligne. Or le firmware n'émet pas d'impulsion vers une imprimante hors ligne
  (il teste SELECT en `$CBD2`, puis attend BUSY) et ne tient /STROBE bas que
  quelques cycles.
- **Grappler+ : garde `m_ack_in` sur l'effacement du latch (558-570)** :
  **VOULUE**, documentée à `GrapplerCard.cpp:170-172` (avec une ACK instantanée,
  /ACK n'est jamais tenu bas au moment d'une écriture).
- **Impulsion /STROBE de 7 horloges réduite à rien** : **VOULUE**
  (`docs/lle_vs_hle.md`, table des coutures ; `DEV.md:8405-8407`).
- **IRQ en niveau dérivé plutôt qu'en bascule** : **VOULUE** et équivalente
  pour toutes les transitions MAME, sauf `reset_from_bus` (piste 1).
- **ACK conditionné par le BUSY de l'hôte (`ackEffective`)** : **VOULU**, c'est le
  modèle de contre-pression de POM2 (`DEV.md:8409-8411`). Il est appliqué de la
  même façon au bit 0 du statut et au masque A6 de `read_cnxx` (sonde [3]).
- **S1 par défaut = Apple Dot Matrix (101), pas l'Epson de MAME** : **VOULU**
  (`DEV.md` § Grappler+, `GrapplerCard.h:135-141`).
- **`takesC800()` conditionné à la présence de la ROM** : **VOULU** (bug hunt
  n° 9, `GrapplerCard.h:99-107`). Identique à MAME quand la ROM est là ; la
  Grappler 1981, sans ROM, n'est pas branchée du tout.
- **Octet tenu quand l'imprimante ne peut pas le prendre** : **VOULU**, c'est la
  « lecture récupérable » (`GrapplerCard.h:211-215`, `CentronicsPrinter.h:30-35`),
  appliquée de la même façon par les deux modèles.
- **Banque `$C800` gardée au reset** : conforme à MAME (`reset_from_bus` et
  `device_reset` ne la touchent pas), sonde [2]. Gardée aussi au cycle
  d'alimentation (`onPowerCycle` → `onReset`), sans effet observable : pour
  reprendre la fenêtre `$C800`, il faut un accès `$CnXX`, et cet accès remet
  la banque à 0.
- **Effets de bord des lectures pour le débogueur** (`3e28cafa55`,
  `side_effects_disabled`) : couverts autrement. `Memory::peekCpuView` ne
  passe jamais par `deviceSelectRead` (`MemoryCpuView.cpp:58`), et
  `peekSlotRom` vaut `false` par défaut, si bien que le débogueur ne déclenche
  ni /STROBE ni changement de banque.
- **Bus flottant de la Grappler 1981** (`ed697c3087`, postérieur à `mame0287`) :
  présent dans POM2.
- **Masque du bit 7 par S1:1** : conforme à `data_latched` (799). La
  réémission par `sw_msb` quand on bascule l'interrupteur à chaud n'est pas
  modélisée : latent, car `dipMsb_` n'est réglé qu'au branchement.
- **Combinaisons d'adresses `$C0n4` / `$C0n5` / `$C0nC` de la Grappler+**
  (donnée + autorisation d'IRQ) : même ordre que MAME (donnée, banque, IRQ). Avec
  l'ACK instantanée, l'IRQ monte tout de suite au lieu de monter à l'ACK de
  l'imprimante : même état final, et la différence de timing relève de la
  couture /STROBE.
- **Polarités opposées du latch ACK** (1 = acquitté sur la Grappler+, 1 = en
  attente sur la 1981) : conformes à MAME, et documentées
  (`docs/printer-detection.md` § 8). `PrinterPortControl.cpp:118-140` rapporte
  pour chaque carte les bits que le programme lit : pas de troisième règle.
- **Papier épuisé avec SELECT laissé haut** : le manuel FX-80 dit que
  l'imprimante passe hors ligne d'elle-même quand le papier est épuisé, alors
  que `GrapplerCard.h:200-205` laisse SELECT à `setOnline`, avec la mention
  « Epsons reportedly do not » (grade E). Ce n'est pas un constat : la table
  du manuel ne donne pas le niveau de la sortie SLCT, et l'utilisateur règle
  les deux états séparément. La mention mérite d'être regradée.
- **Deuxième lecture amont** : AppleWin n'a pas de Grappler. Son
  `ParallelPrinter.cpp` (master) répond `0xFF` à toute lecture (« status -
  TODO? »). Pas d'arbitrage possible de ce côté. L'imprimante de MAME ne tranche
  pas le constat 1 non plus : son commentaire « sets us busy when the printer
  goes offline » (`printer.cpp:64-71 @ mame0287`) ne pilote en fait que PERROR,
  et SELECT y reste à 1.

---

## Bilan

- **Citations examinées** : 32 sur 32, datées sur les 13 révisions de
  `grappler.cpp` (2021-04-06 → 2026-08-12) et relues à la main.
- **Verdicts** :
  - OK 25 (dont `GrapplerClassicCard.h:19`, à la plage trop large) ;
  - OK (rev `299265cc`–`5f4e3730`) 4, toutes dans `GrapplerClassicCard.*` ;
  - DÉCALÉE 2 (`GrapplerCard.cpp:171`, à une ligne près ;
    `GrapplerClassicCard.cpp:85`, qui ne tombe juste sur aucune révision) ;
  - INEXACTE 1 (`GrapplerClassicCard.cpp:61`, front de prise de l'octet) ;
  - ABSENTE 0 · NON-PINNÉE 0.
- **Constats** : CRITIQUE 0 · MAJEUR 0 · MINEUR 1 (BUSY hors ligne sur la
  Grappler+, contradiction interne au dépôt, tranchée par le manuel Epson FX).
  Deux divergences confirmées à la sonde sont latentes : l'IRQ désactivée au
  Ctrl-Reset et la prise de l'octet au front montant sur la 1981.
- **Révision de référence** : `mame0287` (= `4373abe8ab`), sans pin par commit.
  La date de `DEV.md` (2026-07-28) et le commit de `printer-detection.md`
  (`f663f942`) retombent sur les mêmes numéros de ligne. `GrapplerCard.*` suit
  cette révision ; `GrapplerClassicCard.*` mêle l'état 2021-10 → 2024-10 et
  l'état ≥ 2025-11. Il faudrait écrire le pin dans les deux en-têtes.
- **Non vérifié** :
  - les schémas et le manuel d'Orange Micro : je n'ai pas pu établir si /RST
    est relié à la bascule d'IRQ U2A ou à la bascule de banque U2D. La piste 1
    en dépend, ainsi que la décision « reset garde la banque » (prise sur la
    foi de MAME) ;
  - la spécification parallèle de l'Apple Dot Matrix / C. Itoh 8510
    (comportement de BUSY hors ligne) : le constat 1 repose sur les deux
    manuels Epson FX ;
  - le chronogramme STROBE d'une imprimante Centronics (front de prise de
    l'octet) ;
  - l'usage de l'IRQ par le firmware 3.1 : seul un balayage statique des accès
    absolus indexés a été fait, pas de désassemblage complet (un accès indirect
    `(zp),Y` à `$C084` m'aurait échappé) ;
  - je n'ai pas relancé `ctest` ; la sonde appelle les cartes directement et
    n'exécute pas le firmware (c'est `parallel_cards` et
    `grappler_printer_state` qui le font).
