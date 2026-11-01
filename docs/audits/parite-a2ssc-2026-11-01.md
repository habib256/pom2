# Audit de parité — citations de MAME `a2ssc.cpp` (2026-11-01)

Tranche 14 du brief `docs/audits/BRIEF-parite-amont.md`. Toutes les citations
`a2ssc.cpp:<lignes>` sous `src/` :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep -E '(^|/|:)a2ssc\.(cpp|h):'
```

14 citations dans 5 fichiers : 9 dans `SuperSerialCard.{h,cpp}`, une dans
`MainWindow_DevicePanels.cpp`, une dans `LironCard.h`, une dans
`WorkstationCard.h`. Aucune citation de `a2ssc.h`.

Le périmètre est ce que `a2ssc.cpp` ajoute **autour** de l'ACIA : les deux
bancs de DIP et leur lecture, la ROM 341-0065-A et la fenêtre `$C800`, le
câblage de l'IRQ, le reset, les lignes modem au connecteur. Le 6551 lui-même
est la tranche 5 (`parite-mos6551-2026-11-01.md`). Son MAJEUR (IRQ DCD/DSR
malgré IRD) n'est ni redécouvert ni recompté. Il a d'ailleurs été corrigé
pendant cet audit, par le commit `0675fb74`.

**Le code a bougé pendant l'audit.** `0675fb74` a décalé les numéros de
`SuperSerialCard.cpp` d'environ 28 lignes. Tous les numéros POM2 de ce rapport
sont ceux de `0675fb74`. Aucun fichier de code n'a été modifié. Les sondes,
scripts et téléchargements sont sous `/tmp/audit_a2ssc/`.

## Sources

**Grade B, documents primaires** (archive.org) :

- *Apple II Super Serial Card — Installation and Operating Manual* (Apple,
  1981), identifiant `a2_Super_Serial_Card_installation_and_operating_Manual`,
  lu dans sa couche texte (`_djvu.txt`). Chapitre 4, « How the SSC works » :
  p. 45 (logique d'adressage, bascule `$C800`, table 4-4), p. 46 (table 4-5 des
  registres, tampons LS365, sélection du 6551), p. 47 (récepteurs MC1489,
  tirages 15 kΩ, bus de données tiré à +5). Annexe A : tableau des registres
  (« SSC-54 » chez AppleWin), dont le bit 0 de `$C0n2` = CTS.
- **Schéma Apple 050-0045-A**, « Super Serial Card », rév. A (scan David
  T. Craig, 9/1981), identifiant `super_serial_card_050-0045-a_schematic`. Lu
  à 300 dpi, zone par zone.
- *Apple Tech Info Library* n° 7895, « Super Serial Card: Description »
  (`Super_Serial_Card_Description`) : brochage, tableaux des interrupteurs.

**Grade C** : l'EPROM **341-0065-A** elle-même (`roms/ssc_341-0065-a.bin`,
SHA-1 `6dab633470c6bc4cb3e81d09fda46597caf8ee57`, identique au
`ROM_LOAD` de MAME). Je l'ai désassemblée avec un désassembleur écrit pour
l'occasion (`/tmp/audit_a2ssc/dis6502.py`, origine `$C800`).

**Grade D** : MAME, les 34 révisions de `src/devices/bus/a2bus/a2ssc.cpp`, plus
`bus/rs232/rs232.cpp` et `mame/apple/apple2e.cpp` au tag `mame0287`. AppleWin
`master` (`56c95d3126`, 2026-09-19), `source/SerialComms.cpp`.

## Révision amont

**Aucun pin.** Ni `SuperSerialCard.{h,cpp}`, ni DEV.md (§ Super Serial Card,
lignes 6404-6420 et 6504), ni `docs/printer-detection.md` ne nomment de
révision. Les verdicts sont rendus contre **`mame0287`**. Pour ce fichier, son
contenu est identique, octet pour octet, à celui de `f135a4e417` (AJR,
2025-11-10, « a2bus: Interface refinements »). Le `master` (`d066f16134`,
2026-06-02) ne change qu'une ligne, `MOS6551(config, m_acia, 0)` →
`MOS6551(config, m_acia)` (l. 274), sans décaler aucun numéro.

Datation sur **les 34 révisions** du fichier, de `7c19aac60e` (2015-11-08,
renommage `.c` → `.cpp`) à `d066f16134`. J'ai écrit une ancre regex par
citation : le code décrit, pas son numéro. Une révision tombe juste quand la
plage citée contient toutes les ancres. Script :
`/tmp/audit_a2ssc/date_cites.py` (numérotation réelle) et `date_stripped.py`
(numérotation sans lignes vides, l'artefact trouvé par la tranche 5).

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|---|
| `7c19aac60e` → `97b6717027` (13 révisions) | 2015-11 → 2019-03 | Ancien plan des DIP (mode en DSW1 `$0C`, format en DSW2 `$C0`, interrupteur d'IRQ dans DSW2 `$04`) ; ACIA en `$C0n8-$C0nB` seulement, par un `switch` | 0 |
| `810a143a40`, `0e0ec65ca2`, `5976fc789c` | 2020-12-30 | AJR : emplacement des DIP, plan actuel des bits, CTS en bit 0 de DSW2, port `DSWX` (SW2-6, SW1-7, SW2-7), décodage par ET, **ACIA en miroir sur `$C0nC-$C0nF`** ; sortie RTS | 0 |
| `8fa9e7d941`, `c296bd9729` | 2020-12-30 → 2021-01-01 | Ajout de l'Apricorn SSI ; porte d'IRQ en l. 372 | 0 (`:373` à ±1) |
| `c2c3d685f0` → `5f4e3730d9` (4 révisions) | 2021-10 → 2024-10 | Nettoyages ; `take_c800` encore implicite (opt-out dans `a2bus.h`) | 1 : `SuperSerialCard.h:95` (88-91, stable depuis 2021-10) |
| `f135a4e417` = **`mame0287`** | 2025-11-10 | `take_c800() { return true; }` explicite (opt-in), `reset_from_bus()` → `m_acia->reset()` au Ctrl-Reset | **9** : `MainWindow_DevicePanels.cpp:283`, `LironCard.h:125`, `WorkstationCard.h:231`, `SuperSerialCard.h:95, 98, 108, 110, 120, 406` |
| `d066f16134` = `master` | 2026-06-02 | Argument d'horloge du constructeur | les mêmes 9 |
| *0287 sans lignes vides, décalé de +2* | — | même fichier, vidage texte | 3 : `SuperSerialCard.cpp:958` (339-353), `SuperSerialCard.cpp:808` et `SuperSerialCard.h:531` (373) |
| **aucune** | — | — | 2 : `SuperSerialCard.cpp:823` (352-355) et `:829` (373-376) pointent l'en-tête de la bonne fonction en numérotation réelle mais s'arrêtent avant son corps (décalage -2 et -3) |

**Une seule époque amont pour le code, deux numérotations pour les
commentaires.** Le comportement porté est celui de `f135a4e417` :
`takesC800()` à `true` sous la règle opt-in, et la réinitialisation du 6551 au
Ctrl-Reset (`SuperSerialCard::onReset`). Tout ce qui est antérieur est
contenu dedans. Les numéros, eux, mêlent deux sources. Neuf viennent du
fichier réel. Trois viennent du même fichier **privé de ses lignes vides et
décalé de deux lignes** : `read_c0nx` est en 337-351 dans ce vidage, cité
339-353, et la porte SW2-6 en 371, citée 373. C'est l'artefact de la tranche 5,
dans le même fichier source POM2. Les deux derniers numéros ne collent à rien.
`373` désigne à la fois la porte d'IRQ (`.cpp:808`, `.h:531`) et `read_c800`
(`.cpp:829`), ce qu'aucune numérotation unique ne permet.

**Pin à écrire** (tableau, modèle DEV.md § Zilog Z8530 SCC) :

| Commit amont | Pris ? |
|---|---|
| `810a143a40` (plan des DIP, décodage par ET) | pris, **sauf** le bit 0 de DSW2 (CTS), figé à 0 ; le miroir ACIA `$C0nC-F` est pris et il est faux (constat 2) |
| `0e0ec65ca2` (libellés SW1-7 / SW2-7) | sans objet : SW1-7 et SW2-7 ne sont modélisés ni chez MAME ni chez POM2 |
| `5976fc789c` (sortie RTS) | non pris, **VOULU** (`SuperSerialCard.cpp:61-74`, tranche 5) |
| `8fa9e7d941` (Apricorn SSI) | non pris : carte non modélisée |
| `f135a4e417` (`take_c800` opt-in, `reset_from_bus`) | pris |
| `d066f16134` | sans effet |

---

## Rapport 1 — Citations

Ordre : DÉCALÉE, puis OK. « 0287:n » donne la position à la révision de
référence. La dernière colonne propose la ligne à écrire au format des
conventions. Quand le manuel, le schéma ou la ROM disent la même chose, la
citation monte en grade.

| Fichier POM2:ligne | Citation | Verdict | Commentaire | Ligne corrigée proposée |
|---|---|---|---|---|
| `src/SuperSerialCard.cpp:808` | `a2ssc.cpp:373` | DÉCALÉE | Porte SW2-6 : `acia_irq_w` en 0287:429-445, test `m_dswx->read() & 4` en 433. 0287:373 est l'en-tête de `read_c800`. Juste en numérotation sans lignes vides +2. | `// [B] Manuel SSC p. 46 : « Switch SW2-6, when ON, passes interrupt requests from the ACIA to the Apple II » ; schéma 050-0045-A : SW2-6 en série entre la broche 26 (IRQ) du 6551 et l'IRQ du bus. [D] MAME a2ssc.cpp:429-445 @ mame0287 (porte en 433, évaluée aux seuls fronts, voir AMONT FAUX 4).` |
| `src/SuperSerialCard.h:531` | `a2ssc.cpp:373` | DÉCALÉE | Même chose. La description (port `DSWX` à part, jamais visible en `$C0n2`) est juste : 0287:150-157. | idem `.cpp:808`, plus `[D] MAME a2ssc.cpp:150-157 @ mame0287 (DSWX).` |
| `src/SuperSerialCard.cpp:823` | `a2ssc.cpp:352-355` | DÉCALÉE | `read_cnxx` : 0287:354-357, `+0x700` en 356, hors plage. La plage s'arrête sur l'accolade ouvrante. | `// [B] Manuel SSC p. 45, table 4-4 : ROM $0700-$07FF → $Cs00-$CsFF. [C] 341-0065-A : la page $Cn (offset $700) fait STA $CFFF puis JSR $C800. [D] MAME a2ssc.cpp:354-357 @ mame0287.` |
| `src/SuperSerialCard.cpp:829` | `a2ssc.cpp:373-376` | DÉCALÉE | `read_c800` : 0287:376-379. La plage contient l'en-tête et la signature, pas `return m_rom[offset]` (378). | `// [B] Manuel SSC p. 45, table 4-4 : ROM $0000-$06FF → $C800-$CEFF ; un accès $CFxx désarme la fenêtre. [D] MAME a2ssc.cpp:376-379 @ mame0287 (sert les 2 Ko).` |
| `src/SuperSerialCard.cpp:958` | `a2ssc.cpp:339-353` | DÉCALÉE | `read_c0nx` : 0287:394-409. Juste en numérotation sans lignes vides +2. Décrit fidèlement MAME, mais : (1) **le miroir `$C0nC-$C0nF` est contredit par le schéma** (constat 2) ; (2) « 74LS259 » : il n'y en a pas sur la carte, les DIP passent par deux 74LS365. | `// [B] Manuel SSC p. 46, table 4-5, et schéma 050-0045-A : 6551 CS0 = A3, /CS1 = A2, donc $C0n8-$C0nB seulement ; deux 74LS365, SW1 en $C0n1, SW2 + CTS en $C0n2. [D] MAME a2ssc.cpp:394-409 @ mame0287 : le miroir $C0nC-F est contredit par le schéma.` |
| `src/SuperSerialCard.h:98` | `a2ssc.cpp:352-376` | OK (±2) | Contient le corps de `read_cnxx` (356) et la signature de `read_c800` (376). S'arrête 2 lignes avant le corps de cette dernière. | `/// [B] Manuel SSC p. 45, table 4-4. [C] 341-0065-A désassemblée (code d'origine $C800, page $Cn = offset $700). [D] MAME a2ssc.cpp:354-379 @ mame0287.` |
| `src/MainWindow_DevicePanels.cpp:283` | `a2ssc.cpp:118-122` | OK | `PORT_DIPNAME( 0x03, 0x00, "Mode" )` et ses quatre valeurs. | `// [B] TIL 7895 et manuel SSC, « Mode Switches » SW1-5/6 ; [C] 341-0065-A $C828 (bit 0 → SIC, bit 1 → imprimante) ; [D] MAME a2ssc.cpp:118-122 @ mame0287.` |
| `src/SuperSerialCard.h:108` | `a2ssc.cpp:118-122` | OK | Idem. | idem |
| `src/SuperSerialCard.h:110` | `a2ssc.cpp:128-144` | OK | SW2-2 et SW2-3/4 changent de sens en mode imprimante (`PORT_CONDITION`). | `/// [B] TIL 7895, « Printer and Printer PROM Emulation Mode » (SW2-2 délai après CR, SW2-3/4 largeur). [D] MAME a2ssc.cpp:128-144 @ mame0287.` |
| `src/SuperSerialCard.h:120` | `a2ssc.cpp:98-148` | OK | Les deux bancs, de `PORT_START("DSW1")` à « End of Line ». **La plage s'arrête une ligne avant le bit 0 de DSW2, CTS (0287:149)**, et la liste POM2 l'omet aussi. `$50` et `$0C` « read high » sont justes : bus tiré à +5 (manuel p. 47). | `/// [B] Manuel SSC annexe A (DIPSW1/DIPSW2, bit 0 de $C0n2 = CTS, vrai à 0) et p. 47 (bus de données tiré à +5 : bits non câblés lus à 1). [D] MAME a2ssc.cpp:98-149 @ mame0287.` |
| `src/SuperSerialCard.h:95` | `a2ssc.cpp:88-91` | OK | `ROM_START( ssc )`, CRC `b7539d4c`. Stable depuis `c2c3d685f0` (2021-10). | `/// [B] TIL 7895 : « On-Board ROM: 2K x 8 bit (2316 or equivalent) ». [D] MAME a2ssc.cpp:88-91 @ mame0287 (CRC b7539d4c, SHA-1 6dab6334…).` |
| `src/SuperSerialCard.h:406` | `a2ssc.cpp:50` | OK | `take_c800() const override { return true; }`, explicite depuis `f135a4e417`. | `/// [B] Manuel SSC p. 45 : bascule armée par un accès $Csxx, désarmée par $CFxx ou RESET. [D] MAME a2ssc.cpp:50 @ mame0287.` |
| `src/LironCard.h:125` | `a2ssc.cpp:50` | OK | Exemple de la règle opt-in, juste depuis `f135a4e417`. | `/// [D] MAME a2bus.h:145 + a2ssc.cpp:50 @ mame0287 (opt-in depuis f135a4e417, 2025-11-10).` |
| `src/WorkstationCard.h:231` | `a2ssc.cpp:50` | OK | Idem. | idem |

**Hors grep, non comptées**, mêmes verdicts :

- `docs/printer-detection.md:111` (`394-409`), `:117` (`99-122`), `:136`
  (`150-157`) et `:139` (`:50`) sont justes à 0287. `:125` (`124-148`) est
  juste, mais s'arrête avant le bit CTS (149).
- `DEV.md:6408` (`88-91`) et `:6418` (`118-122`) sont justes. `:6411`
  (`352-376`) est OK à ±2, et `:6420` (`128-144`) est juste.
- `tests/ssc_firmware_test.cpp:110` (`352-355`) et `:113` (`373-376`) sont
  DÉCALÉES, `:129` (`:121`, « Printer Mode ») est juste.
- `tests/ssc_transport_seam_test.cpp:342` (`:373`) est DÉCALÉE, et
  `tests/slot_bus_smoke_test.cpp:71` (`:50`) est juste.
- Hors tranche, relevé en passant : `docs/printer-detection.md:204` place
  `apple2c_map` en `apple2e.cpp` L3595-3604. À `mame0287`, il est en
  3572-3578.

---

## Rapport 2 — Constats

### 1. Sans rien au bout du câble, POM2 lit DCD, DSR et CTS inactifs ; la SSC les tire actifs

```
Fichier      : src/SuperSerialCard.cpp:893 (inputLinesLocked : Cable::Nothing → { false, false, false })
               src/SuperSerialCard.h:148 (« Nothing — no cable: every input inactive »)
Amont        : rs232.cpp:84-92 @ mame0287 (port sans périphérique : lignes à 1 = inactives) ;
               AppleWin SerialComms.cpp @ 56c95d3126 (ST_DSR|ST_DCD « when nothing is attached »)
Gravité      : MINEUR
Confiance    : CONFIRMÉ (manuel + schéma, sonde)
```

**Silicium.** Manuel SSC, p. 47, « Data Input and Output » :

> Three of the received handshake lines (Clear To Send, Data Carrier Detect,
> and Data Set Ready) have 15K ohm pullup resistors so the SSC will work with
> devices that do not assert those signals.

Le schéma 050-0045-A le confirme : R5, R2 et R6 (15 kΩ) relient à +12 V les
entrées 1, 4 et 10 du MC1489 (7A), du côté carte du bloc de cavaliers. Une
entrée RS-232 à +12 V est « ON », le 1489 inverse, et le 6551 voit /CTS, /DCD
et /DSR **bas, donc actifs**. Sans câble, la carte se croit reliée à un
appareil prêt : statut `& $70` = `$10`, et le bit 0 de `$C0n2` (CTS) vaut 0.

**Divergence.** MAME branche un `rs232_port_device` vide, dont les lignes
valent 1, c'est-à-dire inactives. AppleWin, en TCP sans client, rapporte
DCD/DSR inactifs. POM2 a repris ce modèle pour le câble `none` : les trois
lignes sont inactives, TDRE est masqué et le statut vaut `$60`. Le dépôt
appuie ce choix sur des affirmations sans source, que le manuel contredit :

- `docs/printer-detection.md:143-146` : « A `PR#n` on a communications-mode
  SSC with nothing on the line […] hangs there without a timeout » ;
- `docs/printer-detection.md:176` : ligne « nothing | inactive | inactive |
  inactive | `$60` » ;
- `docs/printer-detection.md:185-187` : « an open input is only *typically*
  inactive — unverified on hardware ». C'est un grade F, que le manuel
  (grade B) tranche dans l'autre sens pour la SSC ;
- `DEV.md:6431-6432` : « `PR#2` therefore waits, as a real card with no modem
  does » ;
- `tests/ssc_firmware_test.cpp:161`, qui épingle `{ Cable::Nothing, 0x60 }`.

**Scénario.** Profil //e Enhanced, SSC en slot 2 avec l'EPROM d'Apple, mode
communications, `--printer-port 2:cable=none` (ou le panneau, ou
`/printer-port`). `PR#2` puis `PRINT "HELLO"` : le firmware boucle à `$CC02`
sur `JSR $CAF5 / BNE` (`status & $70 == $10`, désassemblé ci-dessous) et la
machine est figée. Une vraie SSC sans câble envoie HELLO dans le vide et rend
la main. De même, une routine de détection (l'objet même de
`printer-detection.md`) lit « rien de branché » = `$60` dans POM2 et `$10`
sur la carte, soit la même chose qu'une imprimante prête.

```
CC02  20 F5 CA  JSR $CAF5        ; CAF5: LDY $26 / LDA $C089,Y / AND #$70 / CMP #$10 / RTS
CC05  D0 FB     BNE $CC02        ; attend TDRE=1, DCD et DSR actifs
CC07  98 09 89 A8 A5 27 99 FF BF ; puis STA $C088+s0 (TDR)
```

**Portée.** Le câble `auto` sans pair telnet donne aussi DCD et DSR
inactifs, avec la même attente sur `PR#2` : c'est celle qu'épingle
`ssc_firmware_test.cpp:340-347`. Ce cas se défend comme modèle d'un appareil
**câblé mais éteint**, puisque l'émetteur d'un appareil hors tension ramène la
ligne vers 0 V et l'emporte sur 15 kΩ. Je ne le compte donc pas comme
constat. Mais sa justification écrite (« as a real card with no modem
does ») est fausse et doit changer. La correction suggérée, non appliquée :
`Cable::Nothing` → `{ true, true, true }` sur une SSC, avec la ligne `nothing`
de `printer-detection.md` § 5, l'alinéa « What blocks » du § 4 et le test.
Les ports du //c sont une autre carte : voir « Non vérifié ».

**Observation.** Sonde, cas 1 et 2 :

```
1. cable none             dcd=0 dsr=0 cts=0  status&$70=$60  $C0n2=$52 (bit0=0)
2. //e + EPROM, comm mode, cable none       PC=$CAF7  sent=""
2. //e + EPROM, comm mode, cable null-modem PC=$C28B  sent="…HELLO…"
```

Le cas 1 montre aussi une contradiction interne. Avec `none`, le 6551 voit CTS
inactif (TDRE masqué), mais `$C0n2` bit 0, qui **est** la broche CTS
(manuel, table 4-5 : « LS365 (3B) $C082 store state of SW2 and state of
CTS »), dit CTS vrai. Le bit est une constante de réglage dans POM2 (voir
« Pistes »).

### 2. L'ACIA répond aussi en `$C0nC-$C0nF`, alors que le 6551 de la SSC n'est sélectionné qu'en `$C0n8-$C0nB`

```
Fichier      : src/SuperSerialCard.cpp:965-966 (lecture : low4 & 0x08, puis low4 & 0x03)
               src/SuperSerialCard.cpp:1077-1078 (écriture, même décodage)
Amont        : a2ssc.cpp:394-409 et 415-419 @ mame0287 (BIT(offset, 3) → m_acia, miroir),
               depuis 810a143a40 (2020-12-30, « mirror register addresses »)
Gravité      : MINEUR
Confiance    : divergence CONFIRMÉE (schéma + manuel, sonde) ; portée PLAUSIBLE
               (aucun logiciel identifié qui touche $C0nC-$C0nF d'une SSC)
```

**Silicium.** Manuel SSC, p. 46 : « The ACIA has two pins used to select one of
its four registers. **While address lines A2 and A3 select the chip**, A0 and
A1 select the actual register. » Table 4-5 : ACIA en `$C088`-`$C08B`
(+s0), rien en `$C08C-$C08F`. Le schéma 050-0045-A le précise. La broche 2
(CS0, active haute) du SY6551 (4A) est sur **A3**, au même nœud que l'entrée 1
du LS32 (3C). La broche 3 (/CS1, active basse) est sur **A2**. Φ2 (broche 27)
vient de l'inverseur LS00 (2C). Le 6551 n'est donc sélectionné que pour
A3 = 1 et A2 = 0. En `$C0nC-$C0nF`, aucun registre ne répond.

**Divergence.** MAME a introduit le miroir en 2020 (`BIT(offset, 3)` seul) ;
POM2 l'a repris et l'écrit en toutes lettres (« A0-A1 only decoded so
$C0nC-$C0nF mirror », `.cpp:958-960` ; `printer-detection.md:112`). AppleWin
renvoie `IO_Null` en `$C0nC-F` (`SerialComms.cpp`, `SSC_IORead` et
`SSC_IOWrite`), ce qui colle au schéma. Le miroir n'est pas inerte, parce que
ces registres ont des effets de bord. Dans POM2 :

- lire `$C0nC` dépile un octet reçu ;
- lire `$C0nD` acquitte l'interruption du 6551 ;
- écrire `$C0nC` émet un octet ;
- écrire `$C0nD` fait un reset programmé ;
- écrire `$C0nE` réécrit le registre de commande (DTR compris).

**Scénario.** SSC en slot 2, pilote à interruptions en commande `$09`, octets
en attente. Un autre programme sonde les slots en lisant `$C08C,X` pour tous
les X (motif classique de détection d'un Disk II, Q6L) ou écrit `$C08E,X`
pour remettre Q7 à zéro. Sur la vraie carte, rien ne se passe. Dans POM2, la
lecture de `$C0AC` vole un octet au pilote, la lecture de `$C0AD` lui vole son
interruption, et l'écriture de `$C0AE` coupe DTR.

**Doute résiduel.** Je n'ai pas tracé l'activation du LS245 (6C) en lecture
de `$C0nC` : la vraie carte y rend soit `$FF` (bus interne tiré à +5,
manuel p. 47), soit le bus flottant. Dans les deux cas, ce n'est pas l'ACIA.
Je n'ai pas non plus trouvé de logiciel Apple II qui touche ces adresses sur
le slot d'une SSC.

**Observation.** Sonde, cas 3 (câble null-modem, commande `$09`, « AB » reçu) :

```
3. after RX of "AB": irqState=$04 line=1 rxDepth=2
3. read $C0nD -> $98 ; irqState=$00 line=0
3. read $C0nC -> 'A' ; rxDepth=1 ; $C0nE -> $09
3. write 'Z' to $C0nC -> txDepth 0 -> 1, sent="Z"
3. write $00 to $C0nE -> command reg now $00 (DTR=0)
```

### 3. Un port série du //c répond aux DIP de la SSC en `$C090-$C097` / `$C0A0-$C0A7`

```
Fichier      : src/SuperSerialCard.cpp:1066-1070 (lecture des DIP, sans test de builtInPort_)
               src/Memory.cpp:1872 (le //c route $C090-$C0AF vers la carte)
Amont        : apple2e.cpp:3572-3578 @ mame0287 (apple2c_map : ACIA en $C098-$C09B
               et $C0A8-$C0AB seulement) ; apple2e.cpp:2769-2808 (c080_r : pas de
               carte en sl1/sl2, retirées en apple2c, → read_floatingbus())
Gravité      : MINEUR
Confiance    : PLAUSIBLE (divergence certaine avec MAME et avec le modèle du dépôt
               lui-même ; la valeur lue sur un vrai //c n'est ni mesurée ni tirée
               de la //c Technical Reference)
```

**Divergence.** Le //c n'a pas de DIP. Le dépôt le sait à deux endroits :
`PrinterPortControl.cpp:222-224` refuse `dsw1=`/`dsw2=` sur un port intégré
(« a //c serial port has no DIP switches »), et `printer-detection.md:204`
écrit « No DIP switches ». Mais `deviceSelectRead` ne teste pas
`builtInPort_`, et `Memory.cpp:1872` lui transmet `$C090-$C097` et
`$C0A0-$C0A7`. Le //c rend donc les valeurs par défaut du constructeur
(`$FC`/`$52`), ou celles d'une session restaurée
(`SessionMachine.cpp:128`). C'est la même règle écrite deux fois, et les deux
versions se contredisent. La tranche 5 avait signalé le point sans
l'instruire (« à confier à `apple2e.cpp` / `a2ssc.cpp` »).

**Scénario.** Profil //c. Un programme écrit pour la SSC, qui la reconnaît
par sa signature Pascal (le //c a la même, `$31`, `printer-detection.md`
§ 3), lit `$C0A1` pour connaître le mode et le débit choisis. Il le fait pour
régler son 6551 ou pour décider qu'il parle à une SSC et pas à un //c. POM2
répond `$FC` (19 200 bauds, communications) à chaque lecture. MAME, et
selon toute vraisemblance un vrai //c, rendent le bus flottant, c'est-à-dire
un octet vidéo. Un programme mis au point sur POM2 passera, puis échouera sur
la machine. Le pilote VDrive d'A2 File Cmd évite déjà ce piège
(« `$C0A1` is not a register there »).

**Observation.** Sonde, cas 4 (ROM `apple2c-32Kv0.rom`, port 1 intégré, lu par
`Memory::memRead`) :

```
4. //c (isIIcClass=1) port 1: 50 FC 52 FF 50 FC 52 FF 00 70 00 00 00 70 00 00   ($C090..$C09F)
```

Attendu selon MAME : bus flottant en `$C090-$C097` et en `$C09C-$C09F`.
Pour le miroir `$C09C-F` sur le //c, voir « Non vérifié ».

### 4. AMONT FAUX — Chez MAME, basculer SW2-6 pendant une interruption fige la ligne IRQ du slot

```
Fichier      : src/SuperSerialCard.cpp:799-819 (pushIrqLine, setIrqDipEnabled) — POM2 a raison
Amont        : a2ssc.cpp:429-445 @ mame0287 (acia_irq_w)
Gravité      : —
Confiance    : CONFIRMÉ côté POM2 (sonde) ; côté MAME par lecture seule, MAME non exécuté
```

**Silicium.** Le schéma 050-0045-A met SW2-6 en série entre la sortie IRQ
(broche 26, drain ouvert) du 6551 et la ligne IRQ du bus. Le manuel, p. 46,
dit : « Switch SW2-6, when ON, passes interrupt requests from the ACIA to the
Apple II. » Ouvrir l'interrupteur relâche donc la ligne aussitôt. Le fermer
pendant que le 6551 tient son IRQ la tire aussitôt.

**Amont.** MAME ne consulte `DSWX` que dans `acia_irq_w`, c'est-à-dire sur un
**front** de l'IRQ du 6551. Si l'on passe SW2-6 sur Off pendant que l'IRQ est
tenue, le front descendant suivant trouve l'interrupteur ouvert et
n'appelle pas `lower_slot_irq()` : la ligne du slot reste levée pour toujours.
Si l'on passe sur On pendant une IRQ tenue, rien ne remonte avant le front
suivant. MAME permet de changer un DIP en cours d'exécution.

**POM2** réévalue la ligne à chaque bascule (`setIrqDipEnabled` →
`pushIrqLine`). Un futur « alignement sur MAME » réintroduirait la ligne
figée. Sonde, cas 5 :
`5. IRQ pending: line=1 | SW2-6 off: line=0 | SW2-6 on again: line=1 (irqState=$04)`.

### 5. AMONT FAUX — AppleWin lit à 0 les bits non câblés des DIP ; ils valent 1

```
Fichier      : src/SuperSerialCard.h:520-524 (lastDip1 = $FC, kDip2* = $52 / $7A) — POM2 a raison
Amont        : AppleWin SerialComms.cpp:896-951 @ 56c95d3126 (CommDipSw : bits 3-2 de DIPSW1
               et 6, 4 de DIPSW2 à 0)
Gravité      : —
Confiance    : CONFIRMÉ (manuel ; MAME et POM2 concordent)
```

Le manuel, p. 47, dit : « The data bus has a 3.3K ohm pullup resistor on
each line so the data inputs on the LS245 are not floating when it turns on
in output mode. » Les bits que les LS365 ne pilotent pas (DSW1 `$0C`, DSW2
`$50`) se lisent donc à 1. MAME (`PORT_BIT(…, IP_ACTIVE_LOW, IPT_UNUSED)`,
0287:117 et 134) et POM2 (`$FC`, `$52`) lisent 1. AppleWin compose l'octet
bit à bit et laisse ces bits à 0 : DSW1 = `$E0` à 9600 bauds, contre `$EC` sur
la carte.

**Scénario.** Un programme compare `$C0n1` à une table sans masquer (par exemple
`CMP #$E0` pour « 9600, communications »). Il passe sous AppleWin et échoue
sur une vraie SSC. POM2 fait comme le matériel. Il ne faut pas « s'aligner »
sur AppleWin ici.

---

## Pistes examinées et écartées

- **Mappage de la ROM** : `$Cn00-$CnFF` = offset `$700-$7FF`, `$C800-$CFFF` =
  les 2 Ko entiers. Conforme au manuel (p. 45, table 4-4 : `$0700-$07FF` →
  `$Cs00-$CsFF`, `$0000-$06FF` → `$C800-$CEFF`), à MAME, à AppleWin
  (`InitializeIO`, `SSC_SLOT_FW_OFFSET = 7*256`) et à la ROM elle-même. Son
  code d'offset 0 appelle `$C99B`, `$CAFF`, `$CC02`, donc il est assemblé en
  `$C800`. La page `$Cn` fait `STA $CFFF` (`$Cn1B`, `$CnE4`) puis
  `JSR $C800` (`$Cn8E`).
- **La ROM n'est vue que de la SSC** : `SlotCardFactory` ne charge pas
  l'EPROM sur un port du //c (épinglé `ssc_firmware::testFactory`). Le //c
  ne lit jamais la page `$Cn` ni la fenêtre `$C800` d'un port série :
  INTCXROM est forcé (`Memory.cpp:1895-1925`) et `exposesIicOnboardRom()`
  vaut `false` pour la SSC. Pas de fuite SSC → //c côté ROM.
- **`takesC800()` à `true` sans dump** : **VOULU** (`SuperSerialCard.h:406-411`).
  Une vraie SSC a toujours sa ROM.
- **Bascule `$C800` désarmée par tout accès `$CFxx` et par RESET** (manuel
  p. 45 : « reset by access to the $CFxx space or by a reset ») : POM2 et
  MAME ne désarment qu'en `$CFFF`. POM2 garde même le propriétaire au
  Ctrl-Reset (`SlotBus.cpp:340`, « RESET retains the expansion-ROM enable »,
  affirmation de grade E héritée du premier commit), et MAME aussi
  (`reset_w` ne touche pas `m_cnxx_slot`). Sonde, cas 6 :
  `owner=2 | after RESET: owner=2 | after $CF80 read: owner=2`. **Latent**,
  sans scénario : le firmware SSC n'accède à `$CFxx` que par `STA $CFFF`, et
  les firmwares de cartes commencent tous par `$CFFF`. Pour la SSC, le
  commentaire de `SlotBus.cpp:340` est contredit par un grade B. Il faut le
  corriger ou le restreindre, même si la règle générale vaut peut-être pour
  d'autres cartes.
- **Bit 0 de `$C0n2` = CTS** (manuel, table 4-5 et annexe A, « true when 0 » ;
  MAME 0287:149 ; AppleWin `CommDipSw`, GH#311) : POM2 le fige à 0
  (`kDip2Communications = $52`, `kDip2Printer = $7A`). C'est sans effet
  observable dès que le constat 1 est corrigé, car aucun câble POM2 ne laisse
  alors tomber CTS. Mais le bit est persisté comme un réglage
  (`ssc_dsw2_slotN`), et `--printer-port N:dsw2=0x53` ferait dire « CTS
  faux » à `$C0n2` pendant que le 6551 voit CTS vrai. À dériver de
  `inputLines().cts`, et à documenter dans `SuperSerialCard.h:120-131` et
  `printer-detection.md` § 4, qui l'omettent.
- **Le firmware lit-il CTS en `$C0n2` ?** Non. DSW2 n'est lu qu'une fois, en
  `$C81A` (copie en `$2B`). Ensuite le firmware teste les bits 1 (`$C81F`),
  5 (`$C842`), 3-2 (`$C857`), 7 et 5 (`$C879`), 3-2 (`$C888`), jamais le
  bit 0. DSW1 est relu en `$CB1C`, `$CB6D`, `$CC56` (mode) et `$CD81`
  (débit, `CTL = DIP >> 4 | $10`).
- **Interrupteur SW2-6 hors du DSW2 mappé** : conforme au manuel (p. 46,
  « Three switches are not connected to the LS365s »). Défaut **On**
  contre Off chez MAME, et contre le conseil du manuel pour un ][ ou un ][+ :
  **VOULU**, documenté (`irqSwitchOn_`, DEV.md § SSC « Recorded, not
  changed »).
- **SW1-7 / SW2-7** (source du DCD : broche 8, ou SCTS broche 19 en position
  TERMINAL) et le **bloc de cavaliers** MODEM/TERMINAL ne sont pas modélisés.
  MAME déclare SW1-7 et SW2-7 dans `DSWX` sans jamais les lire. Les
  préréglages de câble de POM2 en tiennent lieu. Pas de scénario tant que
  l'utilisateur ne peut pas les régler.
- **Décodage des DIP** (`$C0n0` = DSW1 & DSW2, `$C0n3` = `$FF`, `$C0n4-7` en
  miroir) : c'est la lecture de MAME. Au schéma, les validations des deux
  LS365 passent par la porte LS32 (3C), soit A3 OU l'entrée qu'elle partage
  avec l'inverseur du Φ2 du 6551, puis par A1 (2B) ou A0 (3B). A2 n'apparaît
  pas dans ce chemin, ce qui donne bien un miroir en `$C0n4-7`. Tracé
  partiel, et AppleWin rend `IO_Null` en `$C0n0` et `$C0n3-7`. Non tranché,
  pas de scénario : le firmware ne lit que `$C0n1` et `$C0n2`.
- **Écritures en `$C0n0-$C0n7`** : ignorées par POM2, MAME et AppleWin,
  conformément au schéma (pas de verrou). En revanche les commentaires
  « 74LS259 » (`SuperSerialCard.cpp:961`, `:1076` ; `.h:507`) désignent une
  puce qui n'est pas sur la carte. Ce sont deux 74LS365 (2B, 3B), selon la
  table 4-5 et le schéma. C'est de la dette documentaire.
- **En-tête périmé de `SuperSerialCard.h:22-35`** : DIP « en `$C0nA-$C0nB` »,
  « `$C0nC-$C0nD` ACIA reset / status mirror », « `$C0nE` reads `$00` »,
  « bit 4 always 1 », « bit 7 we hold low ». Aucun de ces points ne décrit
  plus le code. Dette documentaire, sans effet.
- **Reset** : `onReset` fait le reset matériel du 6551 au Ctrl-Reset, comme
  `reset_from_bus()` de `f135a4e417`. Avant novembre 2025, MAME ne le
  faisait qu'au reset machine. Le manuel compte RESET parmi les signaux de la
  logique de contrôle (p. 45). Le détail du registre est l'affaire de la
  tranche 5.
- **Mode et DSW2 réécrit par `setMode`** : politique d'interface (un jeu de
  positions par mode), cohérente avec les `PORT_CONDITION` de MAME et avec
  le TIL 7895. Le décodage du mode concorde avec le firmware (`$C828` :
  bit 0 → SIC, bit 1 → imprimante) et le manuel (SW1-5 Off + SW1-6 On =
  imprimante = `$02`).
- **Débit par défaut** `$FC` (19 200) contre `$EC` (9600) chez MAME et AppleWin :
  réglage d'usine non documenté dans les sources lues, choix utilisateur,
  sans conséquence de fidélité.
- **Câble `auto` sans pair telnet** : DCD et DSR inactifs, ce qui fait
  attendre `PR#2`. Défendable comme « appareil câblé mais éteint », voir la
  portée du constat 1. Seule sa justification écrite est fausse.
- **Sortie RTS** (`5976fc789c`) : non modélisée, **VOULU**
  (`SuperSerialCard.cpp:61-74`).
- **Apricorn Super Serial Imager** (`8fa9e7d941`) : hors périmètre, carte non
  proposée par POM2.
- **Correctifs amont postérieurs à `mame0287`** : seulement `d066f16134`, sans
  effet.

---

## Sortie complète de la sonde

`/tmp/audit_a2ssc/probe.cpp`, greffée sur les harnais de
`tests/ssc_firmware_test.cpp` (//e + EPROM réelle, `pasteKeyStream`) et de
`tests/iic_printer_port_test.cpp` (//c par `loadAppleIIRom` sur
`apple2c-32Kv0.rom`). Compilée par `clang++ -std=c++17 -O1 -Isrc -Itests
-Ibuild/generated probe.cpp src/SuperSerialCard.cpp
src/SuperSerialTcpTransport.cpp build/libpom2_core_test.a -framework CoreAudio
-framework AudioToolbox -framework AudioUnit`, lancée depuis la racine du dépôt
avec `POM2_MEDIA_WRITE_DEFAULT=protected`, sur l'arbre `0675fb74`.

```
EPROM bytes: 2048
1. cable auto             dcd=0 dsr=0 cts=1  status&$70=$70  $C0n2=$52 (bit0=0)
1. cable none             dcd=0 dsr=0 cts=0  status&$70=$60  $C0n2=$52 (bit0=0)
1. cable printer          dcd=1 dsr=1 cts=1  status&$70=$10  $C0n2=$52 (bit0=0)
1. cable printer-offline  dcd=0 dsr=0 cts=1  status&$70=$70  $C0n2=$52 (bit0=0)
1. cable modem            dcd=0 dsr=1 cts=1  status&$70=$30  $C0n2=$52 (bit0=0)
1. cable null-modem       dcd=1 dsr=1 cts=1  status&$70=$10  $C0n2=$52 (bit0=0)
2. //e + EPROM, comm mode, cable none       PC=$CAF7  sent=""
2. //e + EPROM, comm mode, cable null-modem PC=$C28B  sent="
]PRINT "HELLO"
HELLO

]"
3. after RX of "AB": irqState=$04 line=1 rxDepth=2
3. read $C0nD -> $98 ; irqState=$00 line=0
3. read $C0nC -> 'A' ; rxDepth=1 ; $C0nE -> $09
3. write 'Z' to $C0nC -> txDepth 0 -> 1, sent="Z"
3. write $00 to $C0nE -> command reg now $00 (DTR=0)
4. //c (isIIcClass=1) port 1: 50 FC 52 FF 50 FC 52 FF 00 70 00 00 00 70 00 00   ($C090..$C09F)
5. IRQ pending: line=1 | SW2-6 off: line=0 | SW2-6 on again: line=1 (irqState=$04)
6. after $C200 read: $C800 owner=2 | after RESET: owner=2 | after $CF80 read: owner=2
```

Attendu selon le manuel et le schéma :

- cas 1, `none` : `dcd=1 dsr=1 cts=1`, `$10` ;
- cas 2, `none` : HELLO émis, pas de blocage à `$CAF7` ;
- cas 3 : aucun effet de bord en `$C0nC-$C0nF` ;
- cas 4 : pas de DIP en `$C090-$C097` (bus flottant selon MAME) ;
- cas 6 : propriétaire libéré au RESET et à `$CF80` (piste latente).

---

## Bilan

- **Citations examinées** : 14 sur 14, confrontées à `a2ssc.cpp@mame0287`
  (contenu identique à `f135a4e417`) téléchargé et datées sur les 34 révisions
  du fichier, en numérotation réelle et sans lignes vides.
- **Verdicts** :
  - OK 9 (dont 1 à ±2 : `SuperSerialCard.h:98`), tous exacts sur
    `f135a4e417` = `mame0287` = `master` ;
  - DÉCALÉE 5 (`SuperSerialCard.cpp:808, 823, 829, 958`, `SuperSerialCard.h:531`) ;
  - INEXACTE 0, OK (rev X) 0, ABSENTE 0, NON-PINNÉE 0.
- **Constats** :
  - CRITIQUE 0, MAJEUR 0 ;
  - MINEUR 3 :
    1. câble `none` : DCD/DSR/CTS lus inactifs alors que la SSC les tire
       actifs, d'où le blocage de `PR#2` ;
    2. ACIA en miroir sur `$C0nC-$C0nF`, avec ses effets de bord ;
    3. les ports du //c répondent aux DIP de la SSC ;
  - AMONT FAUX 2 : SW2-6 évalué aux seuls fronts chez MAME ; bits non câblés
    des DIP lus à 0 chez AppleWin.
- **Révision de référence** : tag `mame0287` (= `f135a4e417`, 2025-11-10),
  faute de pin. Une seule époque pour le code porté (`f135a4e417` : opt-in
  `take_c800`, `reset_from_bus`). Trois numéros viennent d'un vidage sans
  lignes vides décalé de +2, comme en tranche 5 dans le même fichier, et deux
  ne tombent juste sur aucune numérotation. Le pin à écrire est le tableau de
  la section « Révision amont ».
- **Sources** : le manuel SSC, le schéma 050-0045-A et le TIL 7895 font monter
  11 des 14 citations au grade B. Les trois constats et les deux AMONT FAUX
  reposent sur eux, ou sur la ROM désassemblée, et non sur MAME. Deux des
  trois constats sont des points où **MAME s'écarte du silicium** et où POM2
  l'a suivi (lignes d'un port vide, miroir `$C0nC-F`).
- **Non vérifié** :
  - aucune **mesure** sur une vraie SSC ;
  - le manuel a été lu dans sa couche OCR (`_djvu.txt`), pas sur les images
    de page. Les passages cités sont lisibles, mais les numéros de page
    viennent des en-têtes de l'OCR ;
  - au schéma, la sélection du 6551 (CS0 = A3, /CS1 = A2) a été tracée fil à
    fil. Ne l'ont été qu'en partie : les validations des LS365, donc le
    miroir des DIP en `$C0n4-7` et le `$C0n0` en ET ; l'activation du LS245
    en `$C0nC-F` (`$FF` ou bus flottant) ; le câblage de la broche /RES du
    6551 sur le RESET du bus ;
  - la ***Apple IIc Technical Reference*** n'a pas été consultée. Restent
    ouverts : ce que rend un vrai //c en `$C090-$C097` / `$C0A0-$C0A7`
    (constat 3 à confiance PLAUSIBLE) ; si son ACIA est en miroir sur
    `$C09C-$C09F` / `$C0AC-$C0AF` comme POM2 le suppose (`Memory.cpp:1866`,
    et la carte), ou non comme dans MAME ; si ses récepteurs RS-232 ont des
    tirages comme ceux de la SSC (le câble `none` d'un port //c reprend le
    même modèle tout-inactif) ;
  - aucun logiciel Apple II identifié pour les constats 2 et 3 ;
  - l'AMONT FAUX 4 côté MAME repose sur la lecture du source : MAME n'a pas
    été exécuté ;
  - la règle générale « RESET garde la fenêtre `$C800` » de `SlotBus` pour
    les autres cartes : seule la SSC a été confrontée à un document.

---

## Suite donnée (2026-11-01)

Constats 1, 2 et 3 **corrigés** (`0x10` sans câble ; décodage A2/A3 sur la
SSC, miroir conservé sur un port //c ; DIP du //c → bus ouvert). Vérification
sur pièces par un agent : manuel SSC téléchargé et OCRisé, schéma 050-0045-A
lu à 600 dpi — les pull-ups sont **p. 48** (pas 47) et sur les broches 1489
**1, 4 et 13** (pas 10) ; le décodage est **p. 47**. La //c TRM (Tables 7-3,
8-3, 11-10, p. 276) montre un GLU sans A2 : le miroir `$C09C-F` est plausible
sur le //c et y reste. Ce que rend un //c en `$C090-$C097` n'est toujours pas
mesuré. Les tests qui parquaient un octet derrière CTS via `Cable::Nothing`
utilisent désormais `setInputLinesForTest`. Pinned : `ssc_firmware`,
`ssc_acia_smoke` (`testSscDecodesA2AndA3`).
