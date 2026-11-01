# Audit de parité — reliquats horloge, réseau, imprimante, série (2026-11-01)

Tranche « 22+ » du brief `docs/audits/BRIEF-parite-amont.md` : les citations que
les tranches précédentes n'ont pas touchées pour dix fichiers amont, dans quatre
dépôts. Liste obtenue par :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' \
 | grep -E ':(a2thunderclock\.cpp|upd1990a\.h|uthernet\.cpp|printer\.cpp|ctronics\.cpp|imagewriter\.h|iw_charmaps\.h|W5100\.h|IPRaw\.cpp|SerialComms\.cpp):'
```

**24 citations** : `a2thunderclock.cpp` 6, `upd1990a.h` 1, `uthernet.cpp` 3,
`printer.cpp` 2, `ctronics.cpp` 1, `imagewriter.h` 3, `iw_charmaps.h` 3,
`W5100.h` 2, `IPRaw.cpp` 2, `SerialComms.cpp` 1.

Aucun fichier de code n'a été modifié, aucune compilation ni sonde n'a été
lancée (l'arbre de build appartient à un autre processus). Les fichiers amont,
leur historique et le script de datation sont dans le répertoire de travail de
la session (`…/scratchpad/up/`, `…/scratchpad/tc/`, `…/scratchpad/doc/`).

**Le `printer.cpp` n'est pas MAME.** Les deux citations
(`FujiNetLink.h:88`, `FujiNetCard.h:189`) écrivent le chemin en entier :
`lib/device/iwm/printer.cpp:32`, c'est-à-dire le firmware FujiNet
(`FujiNetWIFI/fujinet-firmware`). MAME n'a pas de `printer.cpp` pertinent ici.

**Arbitres trouvés au-dessus des émulateurs.**

- **ThunderClock Plus** : le schéma relevé sur une carte réelle par Mark Aikens
  (`markadev/AppleII-RevEng`, `Thunderware-Thunderclock-Plus/Schematic.pdf`,
  rév. 880-02, daté 2024-11-03), avec le dump U9 v1.3 du même dépôt. C'est une
  lecture du silicium par un tiers, pas une mesure : je l'ai lue au niveau des
  portes (U3 74LS174, U6 74LS74, U7 74LS109, U8 74LS126, U10 74LS27). **Je n'ai
  pas pu lire le manuel ThunderClock** (« ch. V », « 5-1 », « 5-2 », cité par
  `ClockCard.h:62-67`) : introuvable en ligne pendant cette tranche.
- **ImageWriter II** : *ImageWriter II Technical Reference Manual* (Apple 1985),
  le PDF que la tranche 8 a récupéré (archive.org renvoie aujourd'hui une
  erreur 500 ; copie locale `/private/tmp/audit_imagewriter/iw2_techref.pdf`).
  Table 2-1 p. 22 et Figure 2-2 p. 24 (PDF pp. 46 et 48), lues sur l'image de la
  page et non sur l'OCR, qui est illisible pour cette figure.
- **6551** : les lignes de grade B de `parite-mos6551-2026-11-01.md` (R6551,
  bits d'état 5-6).
- **W5100 / Uthernet II** : les sources de `parite-uthernet2-2026-11-01.md`.
- **IPv4** : RFC 791 (IHL minimal = 5).

---

## Révision amont

Aucun des fichiers POM2 concernés n'épingle de révision, et `DEV.md` non plus
(§ ProDOS clock card, § Uthernet I, § ImageWriter, § SSC). L'historique complet
de chaque fichier amont a été récupéré (API GitHub `commits?path=…`) et chaque
citation datée par ancre regex (`date.py` : une révision « tombe juste » quand la
plage citée contient toutes les ancres).

| Fichier amont | Révisions | Référence (= HEAD au 2026-11-01) | Ce que les citations datent | Citations justes sur la référence |
|---|---:|---|---|---:|
| MAME `bus/a2bus/a2thunderclock.cpp` | 24 | `f135a4e417` (2025-11-10) = `mame0287` | `:73` (`take_c800`) n'existe que depuis `f135a4e417`. `:112-115` ne contient la fonction entière sur **aucune** révision : le corps tombe en 112 sur `24066545bd` → `d469cc04bc` (2017-06 → 2018-01), la signature jamais. `:73` pour le quartz ne tombe juste nulle part | 4/6 |
| MAME `machine/upd1990a.h` | 17 | `5cc9a8d408` (2026-10-26) ; `mame0287` identique sur la plage | `:76-91` juste de `1bc5484698` (2022-06-04) à aujourd'hui | 1/1 |
| MAME `bus/a2bus/uthernet.cpp` | 4 | `f135a4e417` (2025-11-10) = `mame0287` | **aucune** révision ne contient `:47-52` ni `:54` comme annoncé ; `reset_from_bus` n'existe que depuis `f135a4e417`, en `:55`. Le port POM2 (`f7af7577`, 2026-07-28) est postérieur : décalage de 1-2 lignes dès l'écriture | 0/3 |
| FujiNet `lib/device/iwm/printer.cpp` | 35 | `f0ac9dd959` (2026-10-24) | `:32` = `create_dib_reply_packet` sur `1502c31032` et `ed2d256bfe` (2026-06-27/28) seulement ; depuis `675b73fb87` (2026-07-10) la fonction est en `:34-46`, le type en `:41` | 0/2 (OK sur rév. ancienne) |
| MAME `bus/centronics/ctronics.cpp` | 30 | `4861a5b148` (2026-11-01) ; `mame0287` identique sur la plage | `:56-73` juste de `e9c1f4a42a` (2023-06-16) à aujourd'hui | 1/1 |
| greg-kennedy `imagewriter.h` | 1 (+6 GSport) | `68ea1d08e6` (2026-01-21) = GSport `199e2f9a76` (2015-02-08) à la fin de ligne près | justes sur les 7 révisions GSport/greg-kennedy | 3/3 |
| greg-kennedy `iw_charmaps.h` | 1 (+3 GSport) | `68ea1d08e6` = GSport `abae7bcdfd` (2024-11-03) | `:62-77` juste depuis `abae7bcdfd` (ajout de `#define N_PAPER_SIZES`) ; `:50-60` juste partout | 3/3 |
| AppleWin `source/W5100.h` | 6 | `b3e617f1e5` (2022-06-17) | le masque `0x03` n'existe que depuis `b3e617f1e5` | 2/2 |
| AppleWin `source/Tfe/IPRaw.cpp` | 3 | `cb1366f485` (2024-12-24) | `:104-132` juste sur les 3 ; `:134-162` depuis `60cbbb40db` (2022-05-19) | 2/2 |
| AppleWin `source/SerialComms.cpp` | 67 | `56c95d3126` (2026-09-19) | `:864` juste de `4ab60cebf8` (2024-01-06) à `3e8054b462` (2026-07-26) ; `56c95d3126` a remonté le bloc de 2 lignes (`:862`) | 0/1 (OK sur rév. ancienne) |

Révisions à épingler : `@ mame0287` pour MAME, `@ f0ac9dd959` pour FujiNet,
`@ 68ea1d08e6` pour greg-kennedy (contenu = GSport `199e2f9a76`/`abae7bcdfd`),
`@ b3e617f1e5` / `@ cb1366f485` / `@ 56c95d3126` pour AppleWin.

**Correctifs amont postérieurs aux citations** : aucun ne change un
comportement que POM2 porte. `upd1990a` `5cc9a8d408` (2026-10-26) est déjà dans
POM2 (`parite-upd1990a-2026-11-01.md`, constats appliqués). `SerialComms.cpp`
`56c95d3126` n'ajoute qu'une option de ligne de commande. FujiNet
`f0ac9dd959` touche OPEN/CLOSE de l'imprimante, pas le DIB, dont le type est
toujours faux (AMONT FAUX 1).

---

## Rapport 1 — Citations

Triées par gravité du verdict. Colonne « Ligne corrigée » au format du dépôt.

| Fichier POM2:ligne | Citation | Verdict | Commentaire | Ligne corrigée |
|---|---|---|---|---|
| `ClockCard.cpp:68` | `a2thunderclock.cpp:73` (quartz 32,768 kHz) | DÉCALÉE | La ligne 73 est `take_c800()` ; le quartz est en `:94` (`UPD1990A(config, m_upd1990ac, 32.768_kHz_XTAL)`). Sur aucune révision la ligne 73 ne porte le quartz. | `// [A-] Schéma ThunderClock Plus (markadev, rév. 880-02) — Y1 32,768 kHz sur XTAL/XTAL' de U1.` + `// [D] MAME a2thunderclock.cpp:94 @ mame0287` |
| `ClockCard.cpp:327` | `a2thunderclock.cpp:112-115` | DÉCALÉE | `read_c0nx` est en `:144-147`. Le contenu (offset ignoré, DATA_OUT en bit 7) est juste pour MAME, mais voir constat 2 : la carte ne fait pas que ça. | `// [A-] Schéma ThunderClock (rév. 880-02) — lecture : U8 met DATA_OUT sur D7, TP sur D6, le drapeau IRQ sur D5 ; D0-D4 non pilotés.` + `// [D] MAME a2thunderclock.cpp:144-147 @ mame0287 — D7 seul.` |
| `UthernetCard.cpp:46`, `UthernetCard.h:25` | `uthernet.cpp:47-52` | DÉCALÉE | `read_c0nx` en `:50`, `write_c0nx` en `:53` ; la plage coupe l'écriture. | `// [D] MAME uthernet.cpp:49-53 @ mame0287 — read/write_c0nx passent l'offset tel quel.` |
| `UthernetCard.cpp:59` | `uthernet.cpp:54` | DÉCALÉE | `reset_from_bus` est en `:55` (ligne 54 vide), et n'existe que depuis `f135a4e417`. | `// [D] MAME uthernet.cpp:55 @ mame0287 — reset_from_bus → cs8900a reset.` |
| `FujiNetLink.h:88`, `FujiNetCard.h:189` | `lib/device/iwm/printer.cpp:32` | OK (rév. `ed2d256bfe`) | Juste sur `1502c31032`/`ed2d256bfe` (2026-06-27/28) ; sur HEAD la fonction est en `:34-46`, `dib.type = SP_TYPE_BYTE_FUJINET_MODEM` en `:41`. Le diagnostic (type MODEM, nom juste) est exact : AMONT FAUX 1. | `// [D] FujiNet lib/device/iwm/printer.cpp:34-46 @ f0ac9dd959 — dib.type = …_MODEM (:41), régression de 6b28e707d3 ; SP_TYPE_BYTE_FUJINET_PRINTER = 0x14 (lib/bus/iwm/iwm.h:85).` |
| `SuperSerialCard.cpp:1092` | `SerialComms.cpp:864` | OK (rév. `3e8054b462`) | « DSR is active low » est en `:862` sur HEAD ; le retour `ST_DSR \| ST_DCD \| ST_TX_EMPTY` « nothing attached » est en `:819`. | `// [B] R6551 Status bits 5-6 — « 1 indicates a high level (false) ».` + `// [D] AppleWin SerialComms.cpp:819, :862 @ 56c95d3126` |
| `ClockCard.h:76`, `ClockCard.h:301`, `ClockCard.cpp:46` | `a2thunderclock.cpp:94` | OK | `UPD1990A(...)`, la variante 1990A et non 4990A. | `// [A-] Schéma ThunderClock (rév. 880-02) — U1 = PD1990AC, C0-C2 parallèles.` + `// [D] MAME a2thunderclock.cpp:94 @ mame0287` |
| `ClockCard.h:188` | `a2thunderclock.cpp:73` (`take_c800`) | OK | POM2 ne réclame `$C800` qu'avec l'EPROM 2 Ko chargée : VOULUE (doc dans le même commentaire). | `// [D] MAME a2thunderclock.cpp:73 @ mame0287 — take_c800() inconditionnel.` |
| `ClockCard.cpp:35` | `upd1990a.h:76-91` | OK | La table des 16 modes. | `// [D] MAME upd1990a.h:76-91 @ mame0287` (+ la fiche NEC p. 796 déjà citée par DEV pour les codes 0-7) |
| `GrapplerCard.cpp:113` | `ctronics.cpp:56-73` | OK | Le bloc `// pull up` de `device_start`. | `// [D] MAME ctronics.cpp:56-73 @ mame0287 — port vide : toutes les lignes tirées haut.` |
| `ImageWriter.h:253` | `imagewriter.h:74-83` | OK | `STYLE_*`. | `// [D] greg-kennedy/ImageWriter imagewriter.h:74-83 @ 68ea1d08e6 (= GSport 199e2f9a76)` |
| `ImageWriter.h:635` | `imagewriter.h:214-294` | OK | De `color` à `ASCII85CurCol`. | idem, `:214-294` |
| `ImageWriter.h:848` | `imagewriter.h:254-262` | OK | `bitGraphicParams`. POM2 élargit `remBytes` à 32 bits : sans effet (ESC G prend au plus 4 chiffres). | idem, `:254-262` |
| `ImageWriter.cpp:90` | `iw_charmaps.h:50-60` | OK | Contrôlée contre le TR, case par case, les 8 langues : la table amont est juste, l'ordre des index aussi. Transcription CP437 de POM2 juste (déjà vue en tranche 8). | `// [B] ImageWriter II TR, Table 2-1 p. 22 (SW1-1..1-3 : 0 US, 1 Italie, 2 Danemark, 3 GB, 4 Allemagne, 5 Suède, 6 France, 7 Espagne) et Figure 2-2 p. 24 (les dix symboles).` + `// [D] greg-kennedy iw_charmaps.h:50-60 @ 68ea1d08e6` |
| `ImageWriter.cpp:36`, `ImageWriter.h:268` | `iw_charmaps.h:62-77` | OK | Tailles de papier du pilote GS/OS. | `// [D] greg-kennedy iw_charmaps.h:62-77 @ 68ea1d08e6 (= GSport abae7bcdfd)` |
| `UthernetIICard.h:26` | `W5100.h:5-12` | OK | Masque et quatre registres. | `// [B] Uthernet II manual (2018-11-17) p. 10 + schéma — A0/A1 seuls.` + `// [D] AppleWin W5100.h:5-12 @ b3e617f1e5` |
| `UthernetIICard.h:73` | `W5100.h:6` | OK | `U2_C0X_MASK 0x03`. | idem, `:6` |
| `W5100Device.cpp:263` | `IPRaw.cpp:104-132` | OK | `createETH2Frame` ; port fidèle (en-tête IPv4 sans options, id 0, somme RFC 1071). | `// [B] RFC 791 §3.1 (en-tête), RFC 1071 (somme).` + `// [D] AppleWin IPRaw.cpp:104-132 @ cb1366f485` |
| `W5100Device.cpp:304` | `IPRaw.cpp:134-162` | OK | `getIPPayload`. POM2 ajoute la garde IHL ≥ 5 : AMONT FAUX 3. | `// [B] RFC 791 §3.1 — IHL minimal 5.` + `// [D] AppleWin IPRaw.cpp:134-162 @ cb1366f485` |

Le grade « A- » des lignes ThunderClock signale un relevé de carte réelle fait
par un tiers, sans mesure de ma part ; à abaisser en B si le dépôt préfère
réserver A aux mesures.

---

## Rapport 2 — Constats

Deux constats, tous deux sur la ThunderClock Plus, tous deux MINEUR et
PLAUSIBLE : ils viennent du schéma relevé, que je n'ai pu confronter ni au
manuel ni à une sonde. MAME a les mêmes écarts (il ne modélise même pas
l'interruption) : ce ne sont pas des AMONT FAUX, l'amont et POM2 ont tort
ensemble.

### 1. L'interruption ThunderClock s'acquitte par une lecture avec A3 = 1, pas par n'importe quel accès

```
Fichier      : src/ClockCard.cpp:337 (deviceSelectRead → clearIrqRequest)
               src/ClockCard.cpp:345 (deviceSelectWrite → clearIrqRequest)
               src/ClockCard.h:66 (« any device-select read/write clears »)
Amont        : MAME a2thunderclock.cpp @ mame0287 — ne câble pas TP sur /IRQ ;
               arbitre : schéma ThunderClock Plus rév. 880-02 (markadev)
Gravité      : MINEUR
Confiance    : PLAUSIBLE — schéma relevé par un tiers, lu porte à porte ; pas
               de manuel (le « 5-2 point 3 » que cite POM2 n'a pas pu être lu),
               pas de sonde.
Divergence   : Sur la carte, STATUS_RD = NOR(Q3, /DEVSEL, CTRL_WR) — toute
               lecture de $C0nX — horloge U7B (74LS109) avec J = K' = A3 :
               U7B mémorise A3 de la DERNIÈRE LECTURE. Son /Q tient la bascule
               de requête U6B (74LS74, horlogée par le front montant de TP,
               D = latch d'autorisation U6A) en reset, et force à 0 la bascule
               de drapeau U7A (J = U6B.Q, K' = U7B./Q, horloge Φ0), qui
               attaque D5 en lecture et Q1 → /IRQ. Donc :
                 - une lecture à $C0n8-$C0nF (A3 = 1) efface le drapeau et
                   BLOQUE toute nouvelle requête ;
                 - une lecture à $C0n0-$C0n7 (A3 = 0) n'efface rien, elle
                   relâche le blocage ;
                 - une écriture ne touche ni U7B ni U7A : elle n'efface pas le
                   drapeau (bit 6 = 0 ne fait qu'empêcher les fronts TP
                   suivants de poser une requête) ;
                 - /RES positionne U7B (S) : après RESET, aucune requête ne peut
                   naître avant une lecture à $C0n0-$C0n7.
               C'est exactement la séquence « LDA $C088,Y / LDA $C080,Y » que
               POM2 attribue lui-même au manuel (ClockCard.cpp:335-336).
               POM2 efface sur TOUTE lecture et TOUTE écriture, à tout offset,
               et n'a pas d'état de blocage.
Scénario     : (a) Un gestionnaire d'IRQ qui n'acquitte que par LDA $C0C0
               (slot 4, A3 = 0) : sur la carte /IRQ reste bas, le 6502 rentre
               en boucle dans le gestionnaire ; sur POM2 tout fonctionne. Un
               logiciel mis au point sous POM2 plante sur la machine.
               (b) Après Ctrl-Reset, STA $40 → $C0C0 puis CLI, sans aucune
               lecture de la carte : la carte ne lève jamais d'IRQ ; POM2 en
               lève une à chaque front TP.
               (c) Un programme en scrutation (SEI) qui teste deux fois le bit 5
               de $C0C0 avant d'acquitter par $C0C8 : la carte rend 1 les deux
               fois, POM2 rend 1 puis 0.
Observation  : test dans tests/clock_card_smoke_test.cpp : armer TP 64 Hz,
               attendre un front, lire $C0n0 deux fois (bit 5 = 1, 1 attendu),
               lire $C0n8 (bit 5 encore 1, puis 0 et plus aucune IRQ), lire
               $C0n0 (les IRQ reprennent). Le firmware U9 v1.3 n'accède qu'à
               $C080,Y (12 accès, tous en STA/LDA abs,Y à l'offset 0) : le
               chemin ProDOS de lecture de l'heure n'est pas concerné.
```

### 2. Le port $C0nX n'est pas décodé : toute écriture charge le latch, et une lecture rend TP sur D6

```
Fichier      : src/ClockCard.cpp:347 (`if (low4 != 0) return;`)
               src/ClockCard.cpp:339-341 (lecture : bits 7 et 5 seuls)
Amont        : MAME a2thunderclock.cpp:144-147, :154-175 @ mame0287
               (écriture à l'offset 0 seulement, lecture = D7 seul)
Gravité      : MINEUR
Confiance    : PLAUSIBLE — même source que le constat 1.
Divergence   : CTRL_WR = NOR(Q3, /DEVSEL, R/W') ne reçoit aucune ligne
               d'adresse : une écriture à N'IMPORTE QUEL $C0nX horloge U3
               (74LS174 : D0 DATA_IN, D1 CLK, D2 STB, D3 C0, D4 C1, D5 C2 —
               le même brochage que MAME) et U6A (D6 = autorisation IRQ).
               En lecture, U8 (74LS126) ne pilote que D7 = DATA_OUT, D6 = TP
               (U1 broche 10, tirée par R5) et D5 = drapeau IRQ ; D0-D4 restent
               en l'air (bus flottant). POM2, comme MAME, ignore les écritures
               hors $C0n0, rend 0 sur D6 et 0 sur D0-D4.
Scénario     : (a) Un programme qui scrute TP sans interruption — BIT $C0C0 /
               BVC — pour se caler sur des ticks de 64, 256 ou 2048 Hz : la
               carte fait basculer V au rythme de TP, POM2 rend toujours V = 0
               et le programme attend indéfiniment.
               (b) STA $18 → $C0C4 puis STA $1C → $C0C4 : la carte verrouille
               TIME_READ ; POM2 ne fait rien, le décalage qui suit rend
               l'ancien registre.
               Aucun logiciel connu ne fait (a) ou (b) : le firmware n'utilise
               que $C080,Y.
Observation  : même test que le constat 1 ; `tpLevel_` existe déjà dans
               ClockCard (ClockCard.cpp:570) et suffit pour D6.
```

### AMONT FAUX — POM2 a raison contre l'amont

```
### AMONT FAUX 1. Le DIB de l'imprimante FujiNet se déclare MODEM
Fichier      : src/FujiNetLink.h:88-95, src/FujiNetCard.h:189-195
Amont        : FujiNet lib/device/iwm/printer.cpp:41 @ f0ac9dd959
Gravité      : —
Confiance    : CONFIRMÉ (lu sur les 35 révisions)
Divergence   : `dib.type = SP_TYPE_BYTE_FUJINET_MODEM` (0x15). Avant
               6b28e707d3 (2026-06-27, « simplify DIB packet creation »), le
               même fichier envoyait SP_TYPE_BYTE_FUJINET_PRINTER (0x14) : c'est
               une régression de ce commit, toujours sur HEAD. POM2 reconnaît
               l'imprimante à son nom « PRINTER » ou au type 0x14, donc marche
               avec les deux époques du firmware.
Protège      : un « alignement » qui reconnaîtrait l'imprimante par son seul
               type perdrait l'imprimante de tout FujiNet postérieur au
               2026-06-27.

### AMONT FAUX 2. MAME ne câble pas TP sur /IRQ
Fichier      : src/ClockCard.cpp:570-579
Amont        : MAME a2thunderclock.cpp @ mame0287 (aucun tp_callback)
Gravité      : —
Confiance    : CONFIRMÉ côté MAME (lu) ; côté carte, schéma : U1.TP → horloge
               U6B → U7A → Q1 → /IRQ.
Divergence   : POM2 modélise l'interruption périodique (déjà dit « POM2-original »
               par DEV § ProDOS clock card). Le schéma lui donne raison sur le
               principe — voir le constat 1 pour l'acquittement.

### AMONT FAUX 3. getIPPayload accepte un IHL < 5
Fichier      : src/W5100Device.cpp:331
Amont        : AppleWin IPRaw.cpp:144-150 @ cb1366f485
Gravité      : —
Confiance    : CONFIRMÉ (lu ; RFC 791 §3.1 : « the minimum value for a correct
               header is 5 »)
Divergence   : AppleWin calcule `payload = frame + 14 + ihl*4` sans borne
               basse : une trame IPv4 avec IHL = 1 livre au socket IPRAW une
               « charge utile » qui commence dans l'en-tête IP. POM2 rejette la
               trame (`ipv4HeaderSize < sizeof(Ip4Header)`).
```

---

## Pistes examinées et écartées

- **Brochage du latch ThunderClock** (bit 0 DATA_IN … bit 5 C2) : identique
  dans MAME, POM2 et le schéma (U3 : D0→DATA_IN, D1→CLK, D2→STB, D3→C0, D4→C1,
  D5→C2).
- **Reset du latch** : `/RES` attaque `/Mr` de U3, qui remet STB/CLK/C à 0 sans
  front STB ; POM2 remet `prevWrite = 0` (`ClockCard.cpp:137`), même effet.
  `/RES` remet aussi U6A (autorisation) et U7A (drapeau) à 0, comme `onReset`.
- **Requête sur front montant de TP, gardée par l'autorisation** : U6B horlogé
  par TP, D = U6A.Q ; POM2 pareil (`ClockCard.cpp:575-579`).
- **Fenêtre `$C800` conditionnée à l'EPROM 2 Ko** : VOULUE, documentée en
  `ClockCard.h:187-193`. Avec l'EPROM, même résultat que MAME.
- **`CnXX` = première page de l'EPROM** : le schéma alimente A8-A10 de U9 par
  U4A/U4B (A8·A9 / A9·A11), cohérent avec MAME (`read_cnxx` = `m_rom[offset]`) ;
  pas d'écart observable trouvé.
- **Code de mode 7 = TEST** : relevé de `upd1990a.cpp`, déjà tranché par la
  tranche 15 ; non rouvert.
- **Table des jeux nationaux ImageWriter** : justes contre la Figure 2-2 du TR
  pour les huit langues. La figure montre douze colonnes, mais `$24` (`$`) et
  `$5E` (`^`) sont les mêmes dans toutes les langues : la table amont à dix
  points est complète.
- **« Wide fanfold (14 x 11 in) » = 1071 pt** : 1071 pt = 14,875 po, le format
  standard 14⅞ × 11 ; seule l'étiquette (amont et POM2) arrondit. Pas de
  comportement.
- **Pull-ups d'un port Centronics vide** (`GrapplerCard::lineBits`) : la
  citation est juste ; la polarité des bits de la Grappler+ relève de
  `parite-grappler-2026-11-01.md`, non rouverte.
- **Masque A0/A1 de l'Uthernet II** : juste, déjà B dans la tranche 7.
- **`createEth2Frame`** : en-tête identique à AppleWin (version 4, IHL 5, id 0,
  fragment 0, somme RFC 1071, octet impair compté comme octet bas sur hôte
  petit-boutiste = bon résultat). La W5100 v1.1.6 n'a pas de registre de
  fragmentation (« Not support IP Fragmentation ») : pas de DF à poser.
- **DCD/DSR actifs bas sur la SSC** : juste ; le sujet a été traité par les
  tranches 5 et 14, non rouvert.
- **IA conservée au reset du CS8900A** : VOULUE (tranche 4).

**Incohérence de documentation (pas un bug de code)** : `DEV.md` § ProDOS clock
card dit encore « dividers 512/128/16/8 … → 64/256/2048/4096 Hz (modes 4-7) ».
Le code traite le mode 7 comme TEST depuis la tranche 15 (`ClockCard.cpp:40-51`)
et refuse 4096 Hz au chargement (`ClockCard.cpp:840-843`). La phrase de DEV est
à corriger.

---

## Bilan

- **Citations examinées** : 24 (10 fichiers amont, 4 dépôts).
- **Verdicts** (par site de citation) : 16 OK, 3 OK sur révision ancienne
  (FujiNet `printer.cpp` ×2 → `ed2d256bfe`, `SerialComms.cpp` → `3e8054b462`),
  5 DÉCALÉE (`ClockCard.cpp:68`, `ClockCard.cpp:327`, `UthernetCard.cpp:46`,
  `UthernetCard.h:25`, `UthernetCard.cpp:59`). Aucune ABSENTE, aucune
  INEXACTE, aucune NON-PINNÉE.
- **Citations sans révision juste** : `a2thunderclock.cpp:112-115` (aucune
  révision ne contient la fonction entière), `a2thunderclock.cpp:73` pour le
  quartz, `uthernet.cpp:47-52` et `:54` — numéros qui n'ont jamais existé.
- **Constats** : 0 CRITIQUE, 0 MAJEUR, 2 MINEUR (PLAUSIBLE), tous deux
  ThunderClock et tous deux partagés avec MAME.
- **AMONT FAUX** : 3 (DIB imprimante FujiNet, TP non câblé chez MAME, IHL < 5
  chez AppleWin).
- **Révisions** : `mame0287` (= HEAD pour les fichiers MAME de la tranche, sauf
  `upd1990a.h` et `ctronics.cpp`, identiques sur les plages citées) ; FujiNet
  `f0ac9dd959` ; greg-kennedy `68ea1d08e6` ; AppleWin `b3e617f1e5`,
  `cb1366f485`, `56c95d3126`.

**Ce que je n'ai pas pu vérifier.**

- **Le manuel ThunderClock Plus.** Les constats 1 et 2 reposent sur le schéma
  relevé ; POM2 cite le manuel (« 5-2 point 3 : tout accès efface ») pour la
  règle que le schéma contredit. Si le manuel dit vraiment « any access », l'un
  des deux se trompe, et seule une carte réelle tranche : lire `$C0n0` deux
  fois après un front TP et regarder D5, puis `$C0n8`. C'est la sonde ou la
  mesure qui ferait passer les deux constats en CONFIRMÉ.
- **Le schéma est un relevé de tiers.** Je l'ai lu sur l'image du PDF
  (les liaisons U10A/U10B, U7B J/K', l'entrée D6 = TP de U8A), pas dans le
  netlist KiCad (`.kicad_sch` téléchargé, non analysé).
- **Uthernet I et Ctrl-Reset** : `UthernetCard.cpp:59-61` affirme que la carte
  relie RESET du bus au CS8900A. MAME le fait depuis `f135a4e417` ; je n'ai
  trouvé ni schéma ni manuel de l'Uthernet I pour le confirmer.
- **Aucune sonde** : interdit pour cette tranche. Les scénarios ThunderClock
  sont écrits mais non exécutés.
