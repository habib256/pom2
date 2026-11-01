# Audit de parité — file d'attente : stockage, bus de slot, cartes Z80, reliquats CPU (2026-11-01)

Tranche « 22+ » du brief `docs/audits/BRIEF-parite-amont.md` : les fichiers
amont que les tranches 1-21 n'avaient pas atteints dans ce domaine. **42
citations**, listées par :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' \
 | grep -E ':(a2bus\.h|atastorage\.cpp|atahle\.h|applefdintf\.cpp|ap_dsk35\.cpp|a2softcard\.cpp|z80\.cpp|z80\.lst|transwarp\.cpp|a2eramworks3\.cpp|a2ultraterm\.cpp|w65c02\.cpp|m6502\.cpp|disk\.cpp|disk\.h|DiskImageHelper\.h):'
```

| Fichier amont | Dépôt | Cit. |
|---|---|---:|
| `src/devices/bus/a2bus/a2bus.h` | MAME | 9 |
| `src/devices/machine/atastorage.cpp` | MAME | 6 |
| `src/devices/machine/atahle.h` | MAME | 1 |
| `src/devices/machine/applefdintf.cpp` | MAME | 1 |
| `src/lib/formats/ap_dsk35.cpp` | MAME | 1 |
| `src/devices/bus/a2bus/a2softcard.cpp` | MAME | 9 |
| `src/devices/cpu/z80/z80.cpp` | MAME | 2 |
| `src/devices/cpu/z80/z80.lst` | MAME | 1 |
| `src/devices/bus/a2bus/transwarp.cpp` | MAME | 2 |
| `src/devices/bus/a2bus/a2eramworks3.cpp` | MAME | 3 |
| `src/devices/bus/a2bus/a2ultraterm.cpp` | MAME | 2 |
| `src/devices/cpu/m6502/w65c02.cpp` | MAME | 1 |
| `src/devices/cpu/m6502/m6502.cpp` | MAME | 1 |
| `lib/device/iwm/disk.h` + `disk.cpp` | **fujinet-firmware**, pas AppleWin | 2 |
| `source/DiskImageHelper.h` | AppleWin | 1 |

**Correction d'attribution.** La commande de tranche classait `disk.cpp` et
`disk.h` dans AppleWin. Les deux citations (`SpOverSlipLink.cpp:736-738`)
désignent le firmware **FujiNet** (`FujiNetWIFI/fujinet-firmware`,
`lib/device/iwm/`). Elles ont été vérifiées là.

Les numéros de ligne POM2 portent sur **`4456fcad`**. Les fichiers `src/`
audités n'ont aucune modification locale. Aucun fichier du dépôt n'a été
modifié, sauf ce rapport. Il n'y a eu ni build ni sonde (règle de la tranche :
le build appartient à un autre processus). Les téléchargements et les scripts
sont sous `/private/tmp/claude-501/audit_q/` : `up/` pour les historiques
amont, `scripts/date_cites.py` pour la datation, `tw/` pour le manuel
TransWarp.

Sources consultées, par rang :

- **[A]** *Visual Z80*, une simulation au transistor du die réel. Elle est
  relayée par rofl0r ([hoglet67/Z80Decoder#2](https://github.com/hoglet67/Z80Decoder/issues/2),
  2022), puis par l'en-tête de `redcode/Z80` `sources/Z80.c` (Sainz de Baranda,
  2023 ; il cite aussi les captures matérielles de D. Banks en 2018 et le
  test ZX Spectrum de P. Helcmanovsky). Je n'ai pas exécuté Visual Z80
  moi-même.
- **[B]** *TransWarp Manual* v1.1, Applied Engineering. Récupéré sur
  archive.org (`Transwarp_Manual_v1.1`) le 2026-11-01. La Figure 1 a été lue
  sur le rendu de la page PDF 9 (page imprimée 5), parce que l'OCR l'a perdue.
- **[C]** ROM TransWarp v1.4 (`roms/ae_transwarp_1.4.bin`), désassemblée avec
  `da65 --cpu 65c02` : seulement le point d'entrée `$F000`. Firmware FujiNet
  `HEAD` = `f703133d00` (2026-10-30) : c'est le firmware du périphérique
  lui-même.
- **[D]** MAME : historique complet des 13 fichiers (**473 révisions**
  récupérées), `mame0287` = `e776c98438` (2026-03-29), et `master` du
  2026-11-01. AppleWin `master` = `4a43bcdb8e` (2026-10-31), ticket
  [GH#1414](https://github.com/AppleWin/AppleWin/issues/1414). L'historique
  de `atastorage.cpp` ne remonte qu'à 2023-06-28 sur ce chemin (GitHub ne
  suit pas le renommage), ce qui ne gêne aucune datation.

---

## Révision amont

**Pins existants.** Un seul : `DEV.md:141` épingle `w65c02.cpp` sur
`370d8dffb7` (= `mame0287`), et il est juste. Aucun autre fichier de la
tranche n'a de pin, ni dans les en-têtes POM2 ni dans `DEV.md`.

| Fichier amont | Rév. | `mame0287` | `master` |
|---|---:|---|---|
| `a2bus.h` | 39 | `237d736a31` (2026-03-21) | `6676d57264` (2026-09-02) |
| `atastorage.cpp` | 7 | `9c9425ec7d` (2025-02-07) | `4f44364b25` (2026-08-27) |
| `atahle.h` | 23 | `48933ef893` (2024-09-26) | `4f44364b25` (2026-08-27) |
| `applefdintf.cpp` | 21 | `63a4016822` (2026-01-07) | `1e6c9aa50a` (2026-08-20) |
| `ap_dsk35.cpp` | 41 | `8364a943ea` (2026-01-07) | `3d686710ab` (2026-08-21) |
| `a2softcard.cpp` | 30 | `ad7707c17b` (2025-11-10) | `6676d57264` (2026-09-02) |
| `z80.cpp` | 111 | `eed7848611` (2026-03-15) | `2b4bd12bbc` (2026-08-17) |
| `z80.lst` | 44 | `1ef074b1a9` | `47d7645d81` (2026-07-03) |
| `transwarp.cpp` | 27 | `f135a4e417` (2025-11-10) | `6676d57264` (2026-09-02) |
| `a2eramworks3.cpp` | 19 | `ebbc5a81ab` (2022-10-07) | **identique** |
| `a2ultraterm.cpp` | 34 | `bcf72d6394` (2025-11-16) | `774a180df2` (2026-08-02) |
| `w65c02.cpp` | 5 | `370d8dffb7` (2026-03-19) | `e59c49eb85` (2026-09-10) |
| `m6502.cpp` | 72 | `1afd61fbdb` (2026-03-20) | `e59c49eb85` (2026-09-10) |

**Méthode de datation.** Pour chaque citation, une ancre regex décrit le code
que POM2 désigne, pas son numéro de ligne. Exemples :
`::lba_address\(\)` + `m_sector_number - 1`, `\(offset & 0x9\) == 1` +
`data & 0x7f`, `macro\s+inir` + `macro\s+otdr`. Une révision « tombe juste »
quand la plage citée contient **toutes** les ancres. Le script est
`scripts/date_cites.py`.

| Révision | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|
| `a2bus.h` `cc6ee26353` (2026-08-16) → `master` | Les lecteurs par défaut des cartes rendent `get_open_bus()` (prise de bus DMA IIgs). `take_c800()` passe de 136 à **145** | Les **9** `a2bus.h:145`. Sur `mame0287`, la ligne est **136** |
| `atastorage.cpp` `0e459c17f5` (2024-06) → `9c9425ec7d` = **`mame0287`** | `lba_address` en 44-53, `IDE_COMMAND_SET_CONFIG` → `set_geometry` en 267-269 | Les **6** (master : 42-54 et 264-266) |
| `atahle.h` `f197f35c25` (2023-06) → `48933ef893` = **`mame0287`** | `IDE_COMMAND_SET_CONFIG = 0x91` en 143 | `AtaBlockDevice.h:80` (master : 155) |
| `applefdintf.cpp` `1e6c9aa50a` = `master` (2026-08-20) | `update_phases()` en 97-100 (0287 : 96-99), corps inchangé | `DiskIICard.h:590` |
| `ap_dsk35.cpp` `63a4016822` → `master` (et 2023-09/10) | `apple_gcr_format::load`, boucle piste/face/secteur en 366-386 | `Sony35Gcr.h:51` |
| `a2softcard.cpp` `115bb9936c` → `93eaa6a494` (2018-03 → 06) | `MCFG_CPU_ADD(Z80_TAG, Z80, 1021800*2)` en 41 | Le **numéro** de `SoftCardZ80.h:45/:94`. Le texte cité, `Z80(config, m_z80, 1021800*2)`, n'existe qu'à partir de `687c87abc0` (2018-07), et toujours en 42 |
| `a2softcard.cpp` **aucune** | — | `:82-86`, `:88-109`, `:111-176` (7 citations). `reset_from_bus` n'existe que depuis `f135a4e417` (2025-11-10), toujours en 77 ou 80, jamais en 82. Décalage de +2 constant contre `mame0287` : numéros qui n'ont jamais existé |
| `z80.cpp` `0bdb4f0669` (2025-06) → `master` | `block_io_interrupted_flags()` en 580-603 | `Z80.cpp:979`, `Z80.h:181` |
| `z80.lst` `a0cb1a1606` (2026-06-09) → `master` | `macro inir` … `otdr` en 769-880 (0287 : 767-877) | `Z80.cpp:981` |
| `transwarp.cpp` `03583be0fe` (2019-12) → `master` | Commentaire « Write to $C074… » en 16-19 | `TranswarpCard.h:54` |
| `transwarp.cpp` `f135a4e417` (2025-11) → `master` | `data == 3` : HALT + `lower_slot_dma()` en 317-323 | `TranswarpCard.cpp:157` |
| `a2eramworks3.cpp` `92606525b3` (2021-12) → `master` | `device_reset` en 65-68, `write_c07x` en 108-115 | Les **3** |
| `a2ultraterm.cpp` `bcf72d6394` = **`mame0287`** → `master` | Bits `CT1_*` en 65-69, `read_c0nx` … `read_c800` en 258-374 | Les **2** |
| `w65c02.cpp` `370d8dffb7` = **`mame0287`** seul | `do_sbc_cd` en 28-46 (master : 41-59, `prefetch_end` inséré avant) | `M6502.cpp:565` |
| `m6502.cpp` **aucune** | — | `M6502.cpp:926`, déjà retirée par la tranche 16 et laissée en commentaire historique |
| fujinet `disk.h` `dead2d321f` (2026-07-12) → avant `1a9b9eb05b` (2026-09-09) | `char disk_num;` en 33 | `SpOverSlipLink.cpp:736` (HEAD : 31) |
| fujinet `disk.cpp` `1502c31032` (2026-06-27) → avant `0ee67c27ce` (2026-10-22) | `"FUJINET_DISK_" + std::to_string(disk_num)` en 106 | `SpOverSlipLink.cpp:738` (HEAD : 104) |
| AppleWin `DiskImageHelper.h` `01ad5d1882` (2020-11) → `master` | `PhaseToTrack` en 85 | `DiskImage.h:273` |

**Pins à poser** (sous forme de tableau, modèle `DEV.md` § Zilog Z8530 SCC) :

- **`a2softcard.cpp`** : `mame0287`. Le portage est identique à `master` ;
  seule la constante `1021800` y est devenue `A2BUS_1M_CLOCK` (`6676d57264`).
  Les numéros sont à refaire.
- **`transwarp.cpp`** : `mame0287`. `master` ne change que la même constante.
  Deux comportements **délibérément non pris**, et un à revoir (constat 1).
- **`a2eramworks3.cpp`** : `ebbc5a81ab` (= `mame0287` = `master`).
- **`atastorage.cpp` / `atahle.h`** : `mame0287`. `master` (`4f44364b25`,
  « Removed mem_mask from ata bus handlers ») ne touche ni `lba_address` ni
  `SET_CONFIG`.
- **`z80.cpp` / `z80.lst`** : `master` pour `block_io_interrupted_flags`,
  **sauf** la ligne `WZ = PC + 1` que MAME a perdue (AMONT FAUX A). POM2 la
  garde, et `DEV.md:283-288` doit cesser de l'attribuer à MAME.
- **`a2bus.h`** : citer `master` (≥ `cc6ee26353`) ou ramener à `:136 @
  mame0287`. Le contrat de `take_c800()` est le même.
- **fujinet `disk.{h,cpp}`** : citer `f703133d00` (HEAD), lignes 31 et 104. Le
  bogue y est toujours, donc la réparation de POM2 reste nécessaire.

---

## Rapport 1 — Citations

Ordre : INEXACTE, ABSENTE, DÉCALÉE, OK (rev X), OK. « 0287:n » donne la
position à `mame0287`.

| Fichier POM2:ligne | Citation | Verdict | Commentaire |
|---|---|---|---|
| `src/M6502.cpp:926` | `m6502.cpp:408` | ABSENTE | Déjà requalifiée par la tranche 16. La ligne ne vit plus que dans la parenthèse « This used to cite… », et la citation active est `om6502.lst`. Aucune révision n'a de `F_B\|F_E` en 408 (0287 : 452, `state_import`). Rien à faire. |
| `src/SoftCardZ80.cpp:41` | `a2softcard.cpp:88-109` | DÉCALÉE | `write_cnxx` : 0287:86-107. Aucune révision n'a jamais placé la fonction en 88. |
| `src/SoftCardZ80.h:25` | `a2softcard.cpp:88-109` | DÉCALÉE | Idem. |
| `src/SoftCardZ80.cpp:65` | `a2softcard.cpp:82-86` | DÉCALÉE | `reset_from_bus` : 0287:80-84. N'existe que depuis 2025-11-10, jamais en 82. |
| `src/SoftCardZ80.h:88` | `a2softcard.cpp:82-86` | DÉCALÉE | Idem. |
| `src/SoftCardZ80.cpp:75` | `a2softcard.cpp:111-176` | DÉCALÉE | `dma_r`/`dma_w` : 0287:109-176. Table identique (six fenêtres de 4 Ko). |
| `src/SoftCardZ80.cpp:147` | `a2softcard.cpp:111-176` | DÉCALÉE | Idem. |
| `src/SoftCardZ80.h:34` | `a2softcard.cpp:111-176` | DÉCALÉE | Idem. L'en-tête dit aussi « reads of $CnXX float ($FF) ». POM2 rend en fait le bus flottant (`SlotPeripheral.h:98`), comme `master` (`a2bus.cpp:378-381`, `get_open_bus()`) : le « ($FF) » est à retirer. |
| `src/SoftCardZ80.h:45` | `a2softcard.cpp:41` | DÉCALÉE | 0287:42. Le **texte** cité vaut à partir de 2018-07 (ligne 42) ; le **numéro** 41 ne valait qu'avec la syntaxe `MCFG_CPU_ADD` de 2018-03 → 06. `master` : `A2BUS_1M_CLOCK*2`. |
| `src/SoftCardZ80.h:94` | `a2softcard.cpp:41` | DÉCALÉE | Idem. |
| `src/SpOverSlipLink.cpp:736` | fujinet `disk.h:33` | OK (rev `dead2d321f`–`1a9b9eb05b`^) | `char disk_num;` ; HEAD `f703133d00` : ligne 31. |
| `src/SpOverSlipLink.cpp:738` | fujinet `disk.cpp:106` | OK (rev `1502c31032`–`0ee67c27ce`^) | HEAD : 104, inchangé. L'appel `set_disk_number('0' + idx)` (`iwmFuji.cpp:149` @ HEAD) confirme le chiffre ASCII ; la disposition du DIB (`iwm.h:116-123` @ HEAD) colle aux décalages de `repairDibName`. |
| `src/SlotPeripheral.h:123` | `a2bus.h:145` | OK (rev `cc6ee26353`–master) | 0287:136. Le contrat (« return true if your card can take over the /IOSTB space », défaut false) est inchangé. |
| `src/GrapplerCard.h:100` | `a2bus.h:145` | OK (rev `cc6ee26353`–master) | Idem. Le `romLoaded_` qui conditionne le retour est une divergence documentée sur place. |
| `src/LironCard.h:124` | `a2bus.h:145` | OK (rev `cc6ee26353`–master) | Idem. |
| `src/SmartPortCard.h:213` | `a2bus.h:145` | OK (rev `cc6ee26353`–master) | Idem. |
| `src/SlotBus.cpp:169` | `a2bus.h:145` | OK (rev `cc6ee26353`–master) | Idem. |
| `src/SlotBus.cpp:194` | `a2bus.h:145` | OK (rev `cc6ee26353`–master) | Idem. |
| `src/SlotBus.h:330` | `a2bus.h:145` | OK (rev `cc6ee26353`–master) | Idem. |
| `src/WorkstationCard.h:230` | `a2bus.h:145` | OK (rev `cc6ee26353`–master) | Idem. |
| `src/CffaCard.h:115` | `a2bus.h:145` | OK (rev `cc6ee26353`–master) | Idem ; `a2cffa.cpp:61` @ master renvoie aussi true. |
| `src/DiskIICard.h:590` | `applefdintf.cpp:97-101` | OK (rev `1e6c9aa50a` = master) | 0287:96-100, corps identique. |
| `src/Z80.cpp:981` | `z80.lst:769-880` | OK (rev `a0cb1a1606`–master) | 0287:767-877. Les macros `inir/otir/indr/otdr` sont exactes, **mais elles ne posent plus `WZ = PC + 1`**, alors que POM2 le fait (`Z80.cpp:1235`, `:1256`). Voir AMONT FAUX A. |
| `src/AtaBlockDevice.h:37` | `atastorage.cpp:44-53` | OK | `lba_address()` ; master : 42-54. |
| `src/AtaBlockDevice.h:163` | `atastorage.cpp:44-53` | OK | Idem. |
| `src/AtaBlockDevice.cpp:60` | `atastorage.cpp:44-53` | OK | Formule CHS identique, sauf le secteur 0, où POM2 évite la sous-capacité de MAME (`m_sector_number - 1`). Garde documentée sur place (ATA-2 § 8). |
| `src/AtaBlockDevice.h:80` | `atastorage.cpp:267-269` | OK | `set_geometry(m_sector_count, (m_device_head & HS) + 1)`. |
| `src/AtaBlockDevice.h:166` | `atastorage.cpp:267-269` | OK | Idem. |
| `src/AtaBlockDevice.cpp:296` | `atastorage.cpp:267-269` | OK | Le commentaire dit « exactly », mais le code ajoute `if (sectorCount_ != 0)`, que MAME n'a pas. Sans scénario (voir Pistes). Retirer « exactly » ou nommer la garde. |
| `src/AtaBlockDevice.h:80` | `atahle.h:143` | OK | `IDE_COMMAND_SET_CONFIG = 0x91` ; master : 155. |
| `src/Sony35Gcr.h:51` | `ap_dsk35.cpp:366-386` | OK | Piste, puis face, puis secteur, `12 - track/16`. |
| `src/Z80.cpp:979` | `z80.cpp:580-604` | OK | Fonction en 580-603. Branches H et P/V transcrites, P/V inversé à cause du champ paresseux de MAME (bien vu, `DEV.md:290-294`). |
| `src/Z80.h:181` | `z80.cpp:580-604` | OK | Idem. |
| `src/TranswarpCard.h:54` | `transwarp.cpp:16-19` | OK | Commentaire d'en-tête « Write to $C074: 0 / 1 / 3 ». |
| `src/TranswarpCard.cpp:157` | `transwarp.cpp:317-323` | OK | HALT + `lower_slot_dma()`. La phrase sur `reset_from_bus:217` est juste pour MAME, mais c'est le point du constat 1. |
| `src/Memory.cpp:736` | `a2eramworks3.cpp:65-68` | OK | `device_reset` : `m_bank = 0`. AppleWin GH#1414 dit du Ctrl-Reset qui garde la banque : « this is spec ». |
| `src/Memory.cpp:823` | `a2eramworks3.cpp:108-115` | OK | `(offset & 0x9) == 1`, `data & 0x7f`. |
| `src/Memory.cpp:1383` | `a2eramworks3.cpp:108-115` | OK | Idem ; `data & 0x7F` est en ligne 113 comme dit. |
| `src/VidexUltratermCard.h:92` | `a2ultraterm.cpp:65-69` | OK | `CT1_MEMSEL/VIDSEL/CLKSEL/VTEMU/PAGEMASK`. POM2 appelle `kPages256` ce que MAME nomme `CT1_VTEMU`, avec le même sens. |
| `src/VidexUltratermCard.h:27` | `a2ultraterm.cpp:250-375` | OK | `reset_from_bus` … `write_c800` en 247-381. Le décodage A0-A1 répété est une divergence déclarée sur place (izapple2). |
| `src/M6502.cpp:565` | `w65c02.cpp:28-46` | OK | `do_sbc_cd`, ligne pour ligne ; pin `DEV.md:141` juste. |
| `src/DiskImage.h:273` | AppleWin `DiskImageHelper.h:85` | OK | `PhaseToTrack = ceil(phase) >> 1`. Sur des phases en demi-pistes, cela donne exactement `(qt + 1) >> 2`, vérifié pour 4k … 4k+3. |

Hors `grep` : `src/Sony35Gcr.h:47` cite le **symbole**
`ap_dsk35.cpp:apple35_sectors_per_track[]`. Il n'existe plus depuis
`dc57db5ceb` (2022-09-25), remplacé par `12 - (track/16)` en ligne : ABSENTE.

### Lignes corrigées

```
// SoftCardZ80.cpp:41 / SoftCardZ80.h:25
// [D] MAME a2softcard.cpp:86-107 @ mame0287 — write_cnxx toggles bus ownership.
// SoftCardZ80.cpp:65 / SoftCardZ80.h:88
// [D] MAME a2softcard.cpp:80-84 @ mame0287 — reset_from_bus: z80 reset + device_reset.
// SoftCardZ80.cpp:75, :147 / SoftCardZ80.h:34
// [D] MAME a2softcard.cpp:109-176 @ mame0287 — dma_r/dma_w, six 4 KB windows.
//     $CnXX reads: the floating bus (a2bus.cpp:378-381 @ master), not $FF.
// SoftCardZ80.h:45, :94
// [D] MAME a2softcard.cpp:42 @ mame0287 — Z80(config, m_z80, 1021800*2)
//     (master: A2BUS_1M_CLOCK*2).

// SpOverSlipLink.cpp:736-738
// [C] fujinet-firmware lib/device/iwm/disk.h:31 + disk.cpp:104 @ f703133d00
//     (2026-10-30) — still to_string(char); iwmFuji.cpp:149 passes '0' + idx.

// SlotPeripheral.h:123, GrapplerCard.h:100, LironCard.h:124, SmartPortCard.h:213,
// SlotBus.cpp:169, :194, SlotBus.h:330, WorkstationCard.h:230, CffaCard.h:115
// [D] MAME a2bus.h:136 @ mame0287 (= :145 @ master) — take_c800(), default false.

// DiskIICard.h:590
// [D] MAME applefdintf.cpp:96-100 @ mame0287 — update_phases().

// Z80.cpp:979 / Z80.h:181
// [D] MAME z80.cpp:580-603 @ mame0287 — block_io_interrupted_flags().
// Z80.cpp:981 (+ the repeat branches at :1233-1237, :1254-1258)
// [A] Visual Z80 (transistor-level die simulation), via hoglet67/Z80Decoder#2
//     (rofl0r, 2022) and redcode Z80.c (Sainz de Baranda, 2023): on a repeating
//     INIR/INDR/OTIR/OTDR, MEMPTR = PCi + 1 in the extra 5-T M-cycle.
// [D] MAME z80.lst:767-877 @ mame0287 — the macros; they LOST WZ = PC + 1 in
//     0c42667cf0 (2024-06-03) after 580447173e (2023-12-03) had added it.

// AtaBlockDevice.h:37, :163, AtaBlockDevice.cpp:60
// [D] MAME atastorage.cpp:44-53 @ mame0287 — lba_address(); POM2 maps CHS
//     sector 0 to 0 instead of underflowing (ATA-2 § 8: sectors are 1-based).
// AtaBlockDevice.h:80, :166, AtaBlockDevice.cpp:296
// [D] MAME atastorage.cpp:267-269 + atahle.h:143 @ mame0287 — SET_CONFIG ($91)
//     → set_geometry; POM2 ignores a zero sector count (MAME stores 0).

// Sony35Gcr.h:47
// [D] MAME ap_dsk35.cpp:368 @ mame0287 — `int ns = 12 - (track/16);`
//     (apple35_sectors_per_track[] was removed in dc57db5ceb, 2022-09-25).
// Sony35Gcr.h:51
// [D] MAME ap_dsk35.cpp:366-386 @ mame0287 — apple_gcr_format::load order.

// TranswarpCard.h:54
// [B] TransWarp Manual v1.1 p. 7 "Software Control" — $C074: 0 = "the maximum
//     hardware default speed", 1 = 1 MHz, 3 = off "until the TransWarp is again
//     activated by cold-booting the system". [D] MAME transwarp.cpp:14-18.
// TranswarpCard.cpp:157
// [D] MAME transwarp.cpp:317-323 @ mame0287 — HALT + lower_slot_dma.
//     (MAME never clears that HALT, and its reset_from_bus re-raises DMA.)

// Memory.cpp:736
// [D] MAME a2eramworks3.cpp:65-68 @ ebbc5a81ab (= mame0287 = master) and
//     AppleWin GH#1414 ("reset (CTRL+F2) doesn't affect the bank selector — this
//     is spec").
// Memory.cpp:823, :1383
// [D] MAME a2eramworks3.cpp:108-115 @ ebbc5a81ab — write_c07x.

// VidexUltratermCard.h:27
// [D] MAME a2ultraterm.cpp:247-381 @ mame0287 — reset_from_bus … write_c800.
// VidexUltratermCard.h:92
// [D] MAME a2ultraterm.cpp:65-69 @ mame0287 — CT1_* (kPages256 = CT1_VTEMU).

// M6502.cpp:565
// [D] MAME w65c02.cpp:28-46 @ mame0287 (= 370d8dffb7; master :41-59).

// DiskImage.h:273
// [D] AppleWin DiskImageHelper.h:85 @ 4a43bcdb8e — PhaseToTrack.
```

---

## Rapport 2 — Constats

### 1. Un Ctrl-Reset ranime une TransWarp coupée par `$C074 = 3`

```
Fichier      : src/TranswarpCard.cpp:309-325 (onReset : `halted_ = false;`
               puis engageShadow()) ; le commentaire :157-164 (« readA2Rom_
               stays false so onReset … re-covers it ») ; src/TranswarpCard.h:57-58
               (« the machine stays at 1 MHz until the next reset ») ; test qui
               épingle la règle : tests/transwarp_card_test.cpp:112-123
               (« only a reset revives a halted card »).
Amont        : [B] TransWarp Manual v1.1, p. 6 (« The only way to activate
               TransWarp is by powering-up, or "cold-booting", the computer ») et
               p. 7 (« Writing a three will disable the TransWarp card completely
               and pass processing control to the Apple's processor until the
               TransWarp is again activated by cold-booting the system »).
               [D] MAME transwarp.cpp:214-234 et 317-323 @ mame0287 (= master).
Gravité      : MINEUR
Confiance    : PLAUSIBLE. Le comportement de POM2 est certain : il est lu dans le
               code et épinglé par le test. Le doute porte sur ce que le manuel
               entend par « cold-booting ». S'il inclut Ctrl-Pomme-Reset, que la
               carte ne peut pas distinguer d'un Ctrl-Reset sur la ligne /RES, la
               règle de POM2 pourrait être celle du matériel.
```

**Divergence.** Selon le manuel, `$C074 = 3` coupe la carte et rend la main
au processeur de l'Apple **jusqu'à la remise sous tension**. POM2 a suivi
MAME : `onReset` remet `halted_ = false`. Le Ctrl-Reset suivant relance donc
l'accélération à 3,58 MHz et recouvre de nouveau `$F000-$FFFF` avec la ROM de
la carte, puisque `readA2Rom_` est faux. MAME n'est pas une meilleure
référence ici : son `reset_from_bus` rappelle `raise_slot_dma()` mais ne
relâche jamais le `INPUT_LINE_HALT` posé en 320. Les deux processeurs restent
alors arrêtés, et aucune lecture ne donne le comportement du manuel. Le crochet
dont un correctif aurait besoin existe déjà : `SlotPeripheral::onPowerCycle`
(`SlotPeripheral.h:139`, appelé par `SlotBus.cpp:337`). Il suffirait de
garder `halted_` dans `onReset` et de le remettre à zéro dans `onPowerCycle`.

**Scénario.** ][+ ou //e avec TransWarp et la ROM v1.4. Un programme exécute
`POKE 49268,3`, par exemple un chargeur de jeu qui exige 1 MHz, puis
l'utilisateur fait Ctrl-Reset. Matériel, selon le manuel : la machine reste
à 1 MHz sur la ROM de l'Apple. POM2 : retour à 3,5×, ROM TransWarp remise en
`$F000`, et le vecteur RESET entre dans le firmware de la carte (test de RAM
en `$1000`, recopie, cf. `DEV.md` § TransWarp) au lieu du moniteur.

**Observation.** `tests/transwarp_card_test.cpp:112-123` montre déjà le
comportement de POM2 (`mem.slotBus().reset()` → `!cpuHalted()`). Une sonde
utile tournerait le même harnais avec `SlotBus::reset(powerCycle=false)` puis
`true`, et lirait `cpuSpeedMultiplier()` et `shadowActive()` après chacun. Seul
un essai sur carte réelle trancherait le doute sur « cold-booting ».

---

## AMONT FAUX

Gravité `—`. Trois endroits où POM2 a raison contre l'amont. Ils protègent le
code contre un futur « alignement ».

### A. MAME a perdu `WZ = PC + 1` sur les répétitions INIR / INDR / OTIR / OTDR

```
Fichier      : src/Z80.cpp:1233-1237 et :1254-1258 (`st.wz = st.pc + 1` avant
               blockIoInterruptedFlags) ; épinglé par tests/z80_block_io_flags_test.cpp
               (vecteurs Tom Harte, WZ comparé en :113).
Amont        : z80.lst:767-877 @ mame0287 et 769-880 @ master (`PC -= 2;
               block_io_interrupted_flags();`, sans WZ) ; z80.cpp:580-603
               (la fonction ne touche pas WZ).
Source       : [A] Visual Z80 via hoglet67/Z80Decoder#2 (rofl0r, 2022) : « according
               to visual z80 [memptr = bc+1] is only the case when b == 0; otherwise
               memptr should contain the value PC+1 » ; redcode Z80.c (2023),
               INXR_OTXR_COMMON : `MEMPTR = PC + 1; return 21;`.
               [D] MAME 580447173e (2023-12-03, #11812, « Update WZ during in block
               I/O instructions ») avait ajouté exactement `WZ = PC + 1;` aux quatre ;
               0c42667cf0 (2024-06-03, « new generatable core », #12423) l'a perdu en
               passant au format .lst ; aucune révision depuis ne l'a rétabli.
```

C'est une **régression de MAME**, datée. Elle n'est visible que par les bits
X/Y d'un `BIT n,(HL)` exécuté pendant une interruption qui coupe la boucle,
mais le corpus SingleStepTests la teste. Deux textes de POM2 attribuent la
règle à MAME et sont à corriger : `DEV.md:283-288` (« — plus `WZ = PC+1`.
MAME calls this `block_io_interrupted_flags()` ») et l'en-tête de
`tests/z80_block_io_flags_test.cpp:21-28`.

### B. MAME accélère sur `$C074 = 0` même quand le bloc 2, interrupteur 8, impose 1 MHz

```
Fichier      : src/TranswarpCard.cpp:86-95 (cpuSpeedMultiplier : `!accelerationEnabled()`
               → 1.0 quel que soit $C074) et :150-152 (case 0 : in1MHz_ = false).
Amont        : transwarp.cpp:300-311 @ mame0287 (= master) — `data == 0` pose
               clock()/2 ou /4 d'après DSW1 seul, sans regarder DSW2 bit 7
               (« Acceleration disabled », :139-141), que reset_from_bus (:219)
               et hit_slot (:341) respectent pourtant.
Source       : [B] TransWarp Manual v1.1, p. 5, Figure 1 (lue sur l'image) :
               bloc 2 interrupteur 8 CLOSED → « Normal Apple (1 MHz) », quel que
               soit le bloc 1 (« the setting of switch 8 of block 1 is ignored ») ;
               p. 7 : « A zero enables the maximum hardware default speed. »
```

Avec la vitesse par défaut réglée sur 1 MHz par les interrupteurs, `POKE
49268,0` revient à cette vitesse par défaut, donc 1 MHz. MAME passe à 3,58 ou
1,79 MHz. POM2 reste à 1 MHz. Le doute est de lecture : « maximum hardware
default speed » se comprend comme « la vitesse par défaut fixée par les
interrupteurs ». C'est aussi la seule lecture cohérente avec le
`reset_from_bus` de MAME lui-même.

### C. Le firmware FujiNet annonce toujours `FUJINET_DISK_48` … `_55`

```
Fichier      : src/SpOverSlipLink.cpp:732-775 (repairDibName)
Amont        : fujinet-firmware lib/device/iwm/disk.cpp:104 + disk.h:31 @ f703133d00
               (2026-10-30) ; iwmFuji.cpp:149 `set_disk_number('0' + idx)`.
Source       : [C] le firmware du périphérique lui-même ; std::to_string n'a pas de
               surcharge char (promotion en int).
```

Le bogue est toujours dans HEAD. La réparation de POM2, qui ne s'applique
qu'aux valeurs 48 … 55, reste donc nécessaire. Elle s'éteindra d'elle-même
quand l'amont sera corrigé, comme le dit le commentaire.

---

## Pistes examinées et écartées

- **`take_c800`, liste des cartes** : les 11 classes POM2 qui surchargent
  `takesC800()` correspondent aux cartes MAME qui renvoient true @ master
  (`a2ssc:50`, `a2cffa:61`, `a2videoterm:122`, `a2ultraterm:135`,
  `a2thunderclock:73`, `grappler:64`, `a2memexp:63`). Le true de
  `a2diskiing:133` est la variante *Applesurance* (ROM `$C800` à banques),
  pas le Disk II. Chaque carte POM2 qui surcharge `expansionRomRead`
  surcharge aussi `takesC800`. Les variantes conditionnées par la ROM
  (Grappler, ThunderClock, Slinky) sont documentées sur place.
- **ATA, `lba_address`** : décalages LBA28 et formule CHS identiques. Le
  secteur 0 replié sur 0 est une garde voulue et documentée.
- **ATA `$91` avec un nombre de secteurs nul** : MAME stocke 0, POM2 garde
  l'ancienne géométrie. Aucun logiciel connu ne l'envoie, et la CFFA travaille
  toujours en LBA. Pas de scénario. Le texte d'ATA-2 § 9.16 n'a pas été relu
  (voir Bilan).
- **ATA, fin de commande sans IRQ** : les interruptions ne sont pas modélisées,
  ce qui est déclaré (`AtaBlockDevice.h:40`).
- **`applefdintf::update_phases`** : formule `(m_phases & mask) |
  (m_phases_input & ~mask)` inchangée de 0287 à master. Seul un format 2MG
  140K s'est ajouté.
- **`ap_dsk35`, ordre des blocs** : piste, puis face, puis secteur. La fonction
  libre `sony35::blockIndexFor` suppose deux faces, mais seul un test l'appelle
  (`tests/sony35_write_head_test.cpp:192`). Tout le code de production passe
  par `Disk35Image::blockIndexFor`, qui suit `headCount()`.
- **SoftCard** : table de traduction, bascule sur toute écriture `$CnXX`,
  remise à zéro du Z80 au premier octroi, gel sous WAIT, `$FF` en DMA coupé,
  remise à zéro sur `reset_from_bus`, IRQ de l'Apple non câblée : tout est
  conforme à MAME. La bascule écrite par le Z80 lui-même (`$EnXX`) traverse
  le vrai bus et rend la main, comme dans MAME.
- **Z80, répétitions** : H et P/V (inversion due au champ paresseux) et 21/16
  T-états sont conformes à MAME et à redcode. Le non-répétitif fixe WZ à BC±1
  (INI avant `B--`, OUTI après), comme MAME `ini`/`outi` et redcode.
- **TransWarp, fenêtres de ralentissement** : `$C090-$C0FF` et
  `$C100-$C7FF` en lecture comme en écriture, `$C070` exact, slot 0
  (`$C080-$C08F`) exclu ; `$C072` en écriture seulement, et transmis au bus ;
  `$C074` consommé en écriture seulement ; plus rien n'est observé une fois la
  carte arrêtée. Tout est conforme.
- **TransWarp, ombre de `$F000`** : MAME intercepte les **lectures** `$F000+`
  quel que soit l'état de la carte langage, alors que POM2 échange l'image
  dans le miroir ROM. La différence n'apparaîtrait que si le firmware de la
  carte lisait la RAM LC avant `$C072`, ce qu'il ne fait pas. Pas de scénario.
- **TransWarp, `in1MHz_` remis à zéro par le reset** : choix explicité sur
  place (l'état observable de MAME après reset est « rapide »).
- **RamWorks** : seules les **écritures** en `$C071/3/5/7` sont décodées. La
  vidéo lit la banque 0 aux deux endroits qui implémentent la règle
  (`Apple2Display.cpp:501`, `MemoryVideoLog.cpp:61-76`), comme
  `get_vram_ptr()` de MAME (`a2eramworks3.cpp:80-83`). La banque survit au
  Ctrl-Reset (AppleWin GH#1414 : « this is spec »). Le repli modulo sur les
  banques garnies laisse intacte la détection par signature.
- **UltraTerm** : le décodage A0-A1 répété est une divergence déclarée. La
  lecture de `$C0n0` rend le bus flottant contre `$FF` chez MAME : registre
  d'adresse du 6845 en écriture seule, aucun consommateur. Le reset remet
  `block512_` à 0 alors que MAME garde `m_rambank`, mais le firmware
  re-sélectionne la banque avant tout accès. Lectures et écritures de
  `$CC00-$CFFF` (masque `0xFF`/`0x1FF` selon `VTEMU`) identiques.
- **W65C02, SBC décimal** : `do_sbc_cd` transcrit à l'identique, V binaire.
  Le `prefetch_end` propre au W65C02 ajouté par master (`e59c49eb85`,
  échantillonnage de l'IRQ) sort du périmètre : POM2 est granulaire à
  l'instruction, et c'est déjà « WANTED » (`DEV.md:141` et § CPU).
- **AppleWin `PhaseToTrack`** : arithmétique vérifiée pour les quatre quarts
  de piste.

---

## Bilan

- **Citations examinées** : 42, plus une hors `grep` (`Sony35Gcr.h:47`).
- **Répartition des verdicts** :
  - OK : 19 ;
  - OK (rev X) : 13 (9 `a2bus.h` sur master, `applefdintf`, `z80.lst`, 2
    FujiNet) ;
  - DÉCALÉE : 9 (tout `a2softcard.cpp`) ;
  - ABSENTE : 1 (`m6502.cpp:408`, déjà traité), plus `apple35_sectors_per_track`
    hors `grep` ;
  - INEXACTE : 0 dans `src/`. Hors `src/`, deux textes attribuent à tort
    `WZ = PC + 1` à MAME : `DEV.md:283-288` et l'en-tête de
    `tests/z80_block_io_flags_test.cpp`.
- **Constats** : 1 MINEUR, PLAUSIBLE (TransWarp ranimée par Ctrl-Reset).
- **AMONT FAUX** : 3 (WZ des répétitions d'E/S du Z80, régression MAME datée ;
  `$C074 = 0` contre l'interrupteur 1 MHz ; DIB FujiNet).
- **Révision de référence** : `mame0287`, avec `master` daté pour chaque
  écart ; fujinet HEAD `f703133d00` ; AppleWin `4a43bcdb8e`.
- **Numéros qui n'ont jamais existé** : les 7 citations `a2softcard.cpp`
  82-86 / 88-109 / 111-176, décalées de +2 contre toutes les révisions.

**Ce que je n'ai pas pu vérifier :**

- **Aucune sonde, aucun build** (règle de la tranche). En particulier, je n'ai
  pas relancé les vecteurs Tom Harte `ed b2/b3/ba/bb` sur POM2 : la
  conformité de WZ repose sur le test existant, pas sur une exécution.
- Je n'ai pas exécuté **Visual Z80** moi-même. L'affirmation [A] est relayée
  par le ticket rofl0r et par l'en-tête de redcode.
- **TransWarp** : sur une vraie carte, Ctrl-Pomme-Reset compte-t-il comme le
  « cold-booting » du manuel ? Il faudrait une carte réelle. Je n'ai pas non
  plus vérifié si le firmware v1.4 a un chemin de reset à chaud. Son entrée
  `$F000` écrit sans condition en `$1000-$10FF`, et dans le modèle à un seul
  CPU de POM2 cela vise la RAM principale à chaque Ctrl-Reset. Sur le
  matériel, ce serait la RAM propre de la carte (256 Ko, « emulates most of
  the Apple's main and auxiliary memory »). Je ne l'ai pas tranché, et c'est
  peut-être un second constat. La sonde : Ctrl-Reset sous TransWarp, puis
  lecture de `$1000-$1FFF` avant et après.
- **SoftCard** : je n'ai pas consulté le manuel Microsoft. La vitesse
  effective du Z80 (2 T-états par cycle 6502) reste un choix MAME non recoupé.
- Je n'ai pas récupéré la **spécification ATA** (ATA-2 § 9.16, `$91` avec un
  nombre de secteurs nul ; état du registre de tête au reset).
- Je n'ai consulté ni le **manuel RamWorks III** ni le **manuel UltraTerm** :
  la règle du Ctrl-Reset RamWorks repose sur AppleWin GH#1414.
