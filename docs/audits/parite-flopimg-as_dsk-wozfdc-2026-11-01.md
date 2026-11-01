# Audit de parité — `flopimg.cpp`, `as_dsk.cpp`, `wozfdc.cpp` (2026-11-01)

Tranche groupée du brief `docs/audits/BRIEF-parite-amont.md` : les formats
disque (`src/lib/formats/flopimg.cpp`, `src/lib/formats/as_dsk.cpp`, formats
Applesauce WOZ/MOOF) et le contrôleur Disk II à séquenceur
(`src/devices/machine/wozfdc.cpp`). 27 citations :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep -E '(^|/|:)(flopimg|as_dsk|wozfdc)\.cpp:'
```

- `flopimg.cpp` : 11 citations (`Sony35Drive.{h,cpp}`, `Sony35Gcr.{h,cpp}`) ;
- `as_dsk.cpp` : 10 citations (`DiskImage.{h,cpp}`) ;
- `wozfdc.cpp` : 6 citations (`DiskImage.cpp`, `DiskIICard.cpp`).

Les numéros de ligne POM2 portent sur le commit **`af7dd56f`** (« Audit reports
for slices 4-14 »). Les fichiers audités n'ont pas de modification locale.
Aucun fichier du dépôt n'a été modifié, sauf ce rapport. Les sondes, les
téléchargements et les copies mutées vivent sous `/tmp/audit_flopimg/`.

Sources consultées, par rang :

- **[B]** *WOZ Disk Image Reference 2.1* (16 juin 2021) et *1.0*, *MOOF
  Reference*, *A2R* (applesaucefdc.com, récupérés le 2026-11-01) ;
- **[C]** RWTS de DOS 3.3 (`disks_5.4/dsk/dos33_master.dsk`) et ROM //c+
  (`roms/apple2cp.rom`), pour la table GCR 6&2 ;
- **[D]** MAME : historique complet des trois fichiers, plus `ap2_dsk.cpp`
  2018-2022 et `flopimg.h` @ `mame0287` ; AppleWin `master` = `4a43bcdb`
  (2026-10-31) : `Disk.cpp`, `DiskImageHelper.cpp` et le ticket
  [GH#1022](https://github.com/AppleWin/AppleWin/issues/1022).

---

## Révision amont

**Aucun pin.** Ni les en-têtes POM2 ni `DEV.md` n'épinglent de révision pour
ces trois fichiers. `DEV.md` § SmartPort 3.5" cite des numéros de
`flopimg.cpp` (`2017-2106`, `:2019-2027`, `967`, `512`, `2107`, `1530`) sans
révision.

Historiques récupérés en entier :

| Fichier amont | Révisions | Équivalent de `mame0287` | `master` |
|---|---:|---|---|
| `src/lib/formats/flopimg.cpp` | 65 (depuis le renommage `.c`→`.cpp`, 2015-11) | `23f379f741` (2025-11-08) | `e807e58019` (2026-10-21) |
| `src/lib/formats/as_dsk.cpp` | 9 (créé le 2022-09-28 par `f9870d6f02`, « Move the applesauce formats in their own file ») | `0d9909dc01` (2025-11-09) | `869b1239ac` (2026-06-18), **même numérotation** que `mame0287` (seul un `#include` change) |
| `src/lib/formats/ap2_dsk.cpp` (ancien foyer du WOZ) | 43 (2018-04 → 2022-09) | — | — |
| `src/devices/machine/wozfdc.cpp` | 28 (depuis 2015-11) | `acb5d3fc93` (2025-10-24) | `59074c5008` (2026-10-29) |

Datation : une ancre regex par citation, qui décrit le code que POM2 cite et
non son numéro. Exemples : `gcr6bw_tb\[0x100\]` + la dernière rangée de la
table, `::load_flux_track\(` + `MG_F \| uint64_t\(cpos\)`, `case 0xa:` +
`floppy = floppy0->get_device\(\)`. Une révision « tombe juste » quand la plage
citée contient toutes les ancres. Script : `/tmp/audit_flopimg/date_cites.py`.

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|---|
| **`flopimg`** `23f379f741` = **`mame0287`** | 2025-11-08 | Tables GCR, `gcr6_encode/decode`, `build_mac_track_gcr`, nibbliseur, identiques à `master` | 4 : `Sony35Gcr.cpp:54`, `Sony35Drive.cpp:123`, `:146`, `Sony35Gcr.h:56` |
| **`flopimg`** `d6dd21e201` → `4f754dc2ea` | 2026-05-09 → 05-26 | Même contenu, numérotation du printemps 2026 | **9 sur 11** tombent juste sur `d6dd21e201`, l'époque du portage (premier commit POM2 `be6d8bed`, 2026-05-27). `Sony35Gcr.h:30` n'est juste **que** là |
| **`flopimg`** `edf51464f0` → `39a922eef1` | 2026-09-02 → 09-07 | Support des disques ED (lignes décalées de +73) | `Sony35Drive.cpp:89` (`:2094`), ajouté par POM2 `5de7ac80` le 2026-09-07 |
| **`flopimg`** aucune | — | — | `Sony35Drive.h:229` (`:2019` désigne la ligne de commentaire au-dessus du tableau sur `d6dd21e201`, à une ligne près) |
| **`as_dsk`** aucune (2022-09 → 2026-06), ni `ap2_dsk` (2018-04 → 2022-09) | — | — | **10 sur 10** |
| **`wozfdc`** `acb5d3fc93` = **`mame0287`** | 2025-10-24 | « improve timing of data register reads » : `lss_sync(1)` sur lecture paire | `DiskImage.cpp:1771` (`:331`) |
| **`wozfdc`** `c690a9ed43` | 2026-08-16 | Écritures de flux par lots | `DiskIICard.cpp:297` (`:348`, juste aussi sur `5b1422965d`, 2015) et `:1929` (`:206-212`) |
| **`wozfdc`** aucune (2015 → 2026) | — | — | `DiskIICard.cpp:1547`, `:1693`, `:1700` (sélection du lecteur) |

**Le bloc `as_dsk.cpp` ne tombe sur aucune révision.** Les écarts avec
`mame0287` croissent le long du fichier : −6 (`crc32r`), −20
(`load_flux_track`), −50 (CRC), −53 (`info_vers`), −54 (champs FLUX), −62
(carte FLUX). Ce n'est donc pas une révision décalée en bloc. Les citations
sont entrées dans POM2 dès `be6d8bed` (2026-05-27), quand `as_dsk.cpp` valait
déjà `0d9909dc01`. Ce sont des numéros qui n'ont jamais existé, comme le bloc
SmartPort de la tranche 2.

**Pin à poser** (sous forme de tableau, modèle `DEV.md` § Zilog Z8530 SCC) :

- **`flopimg.cpp`** : le pin peut être `mame0287`. Les cinq fonctions
  portées (`build_mac_track_gcr`, `extract_sectors_from_track_mac_gcr6`,
  `generate_nibbles_from_bitstream`, `gcr6_encode`, `gcr6_decode`) et les deux
  tables sont identiques de `mame0287` à `master`. Le correctif postérieur
  `1c0701d9dc` (2026-09-24, « endless loop when a GCR sync mark wraps ») ne
  touche que le GCR5 Commodore.
- **`as_dsk.cpp`** : chargeurs WOZ et MOOF identiques de `mame0287` à `master`.
- **`wozfdc.cpp`** : POM2 suit `mame0287`. Depuis, MAME a ajouté :
  - `ed697c3087`, garde de débogueur : sans objet ;
  - `c690a9ed43`, écritures par lots : non pris. POM2 garde le tampon de 32
    transitions et la vidange anticipée à 30, ce qui donne la même épissure ;
  - `5d5ef79d08`, `device_reset` coupe le moteur : POM2 le fait dans `onReset`,
    mais sur un **Ctrl-Reset**, ce que `device_reset` n'est pas (voir Pistes) ;
  - `59074c5008`, nettoyage et `data & 0xf` dans `set_phase` : équivalent.

---

## Rapport 1 — Citations

Ordre : INEXACTE, DÉCALÉE, OK (rev X), OK. « 0287:n » donne la position à
`mame0287`. La ligne corrigée suit le format de grade de `CLAUDE.md`. Quatre
d'entre elles montent en **grade B** (spécification WOZ) et deux en
**grade C** (RWTS, ROM //c+).

| Fichier POM2:ligne | Citation | Verdict | Commentaire |
|---|---|---|---|
| `src/DiskImage.cpp:1160` | `as_dsk.cpp:61-81` | **INEXACTE** | 0287:61-79 est la fin de `load_bitstream_track`. `load_flux_track` est en 81-102. Surtout, le commentaire lit `MG_F \| 0` comme un « time-zero marker » et en conclut que le dernier octet n'est pas une transition. Or `flopimg.h:471` @ 0287 définit `MG_F` comme « Flux orientation change », et MAME commente « There is always a pulse at index, and it's the last one in the stream ». **C'est l'origine du constat 1.** |
| `src/DiskImage.cpp:967` | `as_dsk.cpp:275-277` | **INEXACTE** | « MAME rejects on mismatch unless the stored CRC is zero ». Faux : MAME rejette tout écart, **zéro compris** (0287:324-327 pour le WOZ, 509-511 pour le MOOF). Aucune révision depuis 2018 n'a eu d'exemption. L'exemption vient de la spec (B) et d'AppleWin. Voir AMONT FAUX A. |
| `src/DiskImage.cpp:2912` | `as_dsk.cpp:275-277` | **INEXACTE** | « Zero the CRC32 sentinel (matches MAME) ». MAME **calcule** le CRC à l'écriture (0287:266) et refuserait le fichier que POM2 écrit. La spec autorise le zéro, donc POM2 est conforme, mais pas pour la raison citée. |
| `src/DiskImage.h:512` | `as_dsk.cpp:283` | **INEXACTE** | « MAME reads the same byte » (INFO+39, optimal bit timing). Aucune révision de MAME ne lit INFO+39 : MAME normalise chaque piste à une révolution. 0287:283 est dans `description()`. Constat déjà fait par AppleWin GH#1022 (« optimalBitTiming isn't accounted for »). |
| `src/DiskImage.cpp:965` | `as_dsk.cpp:10-23` | DÉCALÉE | `crc32r` : 0287:16-30. Contenu identique, et identique à la spec (Appendix A). |
| `src/DiskImage.cpp:1069` | `as_dsk.cpp:287-290` | DÉCALÉE | `off_flux`/`flux_size` : 0287:341-345, avec la même règle « largest = 0 ⇒ pas de FLUX ». |
| `src/DiskImage.cpp:1097` | `as_dsk.cpp:284-286` | DÉCALÉE | `info_vers < 1 \|\| info_vers > 3` : 0287:337-339. La spec demande `>=` (voir Pistes, écart latent). |
| `src/DiskImage.cpp:1138` | `as_dsk.cpp:320` | DÉCALÉE | `img[off_flux*512 + 8 + trkid]` sans vérifier l'identifiant du chunk : 0287:382 (et 549 pour le MOOF). Exact sur le fond. |
| `src/DiskImage.cpp:1258` | `as_dsk.cpp:316-326` | DÉCALÉE | Priorité FLUX sur TMAP : 0287:381-390. |
| `src/DiskImage.h:606` | `as_dsk.cpp:61-81` | DÉCALÉE | `total_ticks` : 0287:87-89. MAME s'en sert pour **ramener** la boucle à une révolution (`cpos*200000000/total_ticks`, 100), pas comme durée de rotation. La période « somme des ticks » est un choix POM2, cohérent avec la spec. |
| `src/DiskIICard.cpp:1547` | `wozfdc.cpp:219-241` | DÉCALÉE | Sélection du lecteur : 0287:264-291. 219-241 couvre `write()`, `set_phase()` et le début de `control()`. Aucune révision n'a jamais mis `case 0xa` en 219. |
| `src/DiskIICard.cpp:1693` | `wozfdc.cpp:219-227` | DÉCALÉE | `case 0xa` : 0287:264-273. |
| `src/DiskIICard.cpp:1700` | `wozfdc.cpp:229-241` | DÉCALÉE | `case 0xb` : 0287:274-291. La phrase « not from position 0 … the way MAME models it » est périmée : MAME reprend à l'angle d'arrêt depuis `bc9aec5a` (tranche `floppy.cpp`), et POM2 aussi depuis `ad4d8192`. |
| `src/Sony35Drive.h:229` | `flopimg.cpp:2019` | DÉCALÉE | `cells_per_speed_zone[]` : 0287:2017. Sur `d6dd21e201`, 2019 est le commentaire `// 30318342 = …` juste au-dessus (déclaration en 2020). |
| `src/Sony35Drive.cpp:80` | `flopimg.cpp:2019-2027` | OK (rev `102fa769b1`–`4f754dc2ea`) | Valeurs `30318342 / {394..590}` ; 0287:2016-2023. |
| `src/Sony35Drive.cpp:89` | `flopimg.cpp:2094` | OK (rev `edf51464f0`–`39a922eef1`) | `60.0 / 1.979e-6` ; 0287:2016. |
| `src/Sony35Drive.cpp:159` | `flopimg.cpp:326` | OK (rev `d6dd21e201`–`39a922eef1`) | `raw_w` ; 0287:327-331. |
| `src/Sony35Drive.cpp:172` | `flopimg.cpp:2017-2106` | OK (rev `102fa769b1`–`4f754dc2ea`) | `build_mac_track_gcr` entier ; 0287:2014-2102. Port vérifié champ par champ : 6 208 cellules par secteur, entrelacement 2:1, sommes de contrôle. |
| `src/Sony35Gcr.cpp:32` | `flopimg.cpp:979-997` | OK (rev `d6dd21e201`–`1e6c9aa50a`) | `gcr6bw_tb` ; 0287:980-999. Table identique octet pour octet (vérifié par script). |
| `src/Sony35Gcr.h:30` | `flopimg.cpp:2107` | OK (rev `d6dd21e201` seule) | `extract_sectors_from_track_mac_gcr6` ; 0287:2104. |
| `src/DiskIICard.cpp:297` | `wozfdc.cpp:348` | OK (rev `c690a9ed43`) | `write_line_active = false` dans `lss_start` ; 0287:353 (`lss_start` 346-356). « Only in lss_start » oublie `device_reset` (0287:113), sans effet : `onReset` le remet aussi. |
| `src/DiskIICard.cpp:1929` | `wozfdc.cpp:206-212` | OK (rev `c690a9ed43`) | `lss_sync(1); return data_reg` ; 0287:209-213. |
| `src/Sony35Gcr.cpp:54` | `flopimg.cpp:522-533` | OK | `gcr6_decode` ; 0287:523-533. |
| `src/Sony35Drive.cpp:123` | `flopimg.cpp:967-977` | OK | `gcr6fw_tb` ; 0287:968-978. Table identique, **et** identique à la table d'écriture du RWTS DOS 3.3 et de la ROM //c+. |
| `src/Sony35Drive.cpp:146` | `flopimg.cpp:512-520` | OK | `gcr6_encode` ; 0287:513-521. |
| `src/Sony35Gcr.h:56` | `flopimg.cpp:1535-1551` | OK | Alignement de `generate_nibbles_from_bitstream` (0287:1531-…). |
| `src/DiskImage.cpp:1771` | `wozfdc.cpp:331` | OK | `time_to_cycles` : ticks à `clock()*2` puis `(c+1)>>1` ; master:335. |

### Lignes corrigées

```
// DiskImage.cpp:1160 (et :1170-1173, à réécrire avec le constat 1)
// [B] WOZ 2.1 reference, TRKS (applesaucefdc.com/woz/reference2/, fetched
//     2026-11-01) — "Each byte represents a single flux transition";
//     the streams are "properly looped": the last byte is the transition
//     that closes the loop, at angular position 0.
// [D] MAME as_dsk.cpp:81-102 @ mame0287 — load_flux_track pushes MG_F|0
//     ("There is always a pulse at index, and it's the last one").

// DiskImage.cpp:967
// [B] WOZ 2.1 reference, file header — "If the CRC is 0x00000000, then no
//     CRC has been calculated for the file and should be ignored."
// [D] AppleWin DiskImageHelper.cpp:2123 @ 4a43bcdb — same. MAME
//     as_dsk.cpp:324-327 @ mame0287 rejects a zero CRC: upstream is wrong.

// DiskImage.cpp:2912
// [B] WOZ 2.1 reference, file header — a zero CRC means "not computed".
//     (MAME computes it on save, as_dsk.cpp:266 @ mame0287, and refuses a
//     zero CRC on load: a POM2-saved WOZ does not open in MAME.)

// DiskImage.h:512
// [B] WOZ 2.1 reference, INFO +39 Optimal Bit Timing (v2), 125 ns units,
//     32 = 4 us. MAME never reads it (as_dsk.cpp, every revision): it
//     scales each track onto one revolution instead.

// DiskImage.cpp:965
// [B] WOZ 2.1 reference, Appendix A — CRC-32 (Gary S. Brown), reflected
//     polynomial 0xEDB88320. [D] MAME as_dsk.cpp:16-30 @ mame0287 crc32r.

// DiskImage.cpp:1069
// [B] WOZ 2.1 reference, INFO +46 FLUX Block / +48 Largest Flux Track (v3):
//     "make sure that BOTH this value and the next one are non-zero".
//     [D] MAME as_dsk.cpp:341-345 @ mame0287.

// DiskImage.cpp:1097
// [D] MAME as_dsk.cpp:337-339 @ mame0287 — info_version outside 1..3 refused.
//     [B] WOZ 2.1 says "use >= when checking the INFO Version field".

// DiskImage.cpp:1138
// [D] MAME as_dsk.cpp:382 @ mame0287 — img[off_flux*512 + 8 + trkid],
//     chunk ID not checked.

// DiskImage.cpp:1258
// [B] WOZ 2.1 reference, FLUX chunk — a track valid in both maps: "the
//     'FLUX' one should be used". [D] MAME as_dsk.cpp:381-390 @ mame0287.

// DiskImage.h:606
// [B] WOZ 2.1 reference, TRKS — a flux stream is one loop; its length is
//     the sum of its deltas. [D] MAME as_dsk.cpp:87-100 @ mame0287 sums
//     total_ticks to scale the loop onto one revolution.

// DiskIICard.cpp:1547
// [D] MAME wozfdc.cpp:264-291 @ mame0287 — control() $A/$B: mon_w(true) on
//     the old drive, mon_w(false) on the new one.
// DiskIICard.cpp:1693
// [D] MAME wozfdc.cpp:264-273 @ mame0287 — case 0xa.
// DiskIICard.cpp:1700
// [D] MAME wozfdc.cpp:274-291 @ mame0287 — case 0xb.

// DiskIICard.cpp:297
// [D] MAME wozfdc.cpp:353 @ mame0287 — write_line_active cleared by
//     lss_start (346-356) and device_reset (113), nowhere else.

// DiskIICard.cpp:1929
// [D] MAME wozfdc.cpp:209-213 @ mame0287 — even read: lss_sync(1); return
//     data_reg. [B] WOZ 2.1 reference, "Abusing Disk Controller Soft
//     Switches": A0 drives the latch's Output Enable through a NOT gate.

// DiskImage.cpp:1771
// [D] MAME wozfdc.cpp:331-339 @ mame0287 — time_to_cycles: ticks at
//     clock()*2, then (c+1)>>1.

// Sony35Drive.cpp:80 / Sony35Drive.h:229
// [D] MAME flopimg.cpp:2016-2023 @ mame0287 — cells_per_speed_zone =
//     30318342 / {394, 429, 472, 525, 590}.
// Sony35Drive.cpp:89
// [D] MAME flopimg.cpp:2016 @ mame0287 — "30318342 = 60.0 / 1.979e-6".
// Sony35Drive.cpp:123
// [C] DOS 3.3 RWTS write-translate table $BA29 (dos33_master.dsk T0 S4
//     +$29) and //c+ ROM (apple2cp.rom offset $4D56): the 64 6&2 disk
//     bytes. [D] MAME flopimg.cpp:968-978 @ mame0287 gcr6fw_tb.
// Sony35Drive.cpp:146
// [D] MAME flopimg.cpp:513-521 @ mame0287 — gcr6_encode.
// Sony35Drive.cpp:159
// [D] MAME flopimg.cpp:327-331 @ mame0287 — raw_w.
// Sony35Drive.cpp:172
// [D] MAME flopimg.cpp:2014-2102 @ mame0287 — build_mac_track_gcr.
// Sony35Gcr.cpp:32
// [D] MAME flopimg.cpp:980-999 @ mame0287 — gcr6bw_tb, the inverse of the
//     [C] DOS 3.3 $BA29 table.
// Sony35Gcr.cpp:54
// [D] MAME flopimg.cpp:523-533 @ mame0287 — gcr6_decode.
// Sony35Gcr.h:30
// [D] MAME flopimg.cpp:2104 @ mame0287 — extract_sectors_from_track_mac_gcr6.
// Sony35Gcr.h:56
// [D] MAME flopimg.cpp:1531-1551 @ mame0287 — generate_nibbles_from_bitstream
//     alignment.
```

---

## Rapport 2 — Constats

### 1. Une piste FLUX perd, à chaque tour, la transition qui ferme sa boucle (5,25" et 3,5")

```
Fichier      : src/DiskImage.cpp:1225-1236 (loadFluxTrack : `i != trackSize - 1`
               ne pose jamais la transition finale) et :1166-1173 (le commentaire
               qui le justifie) ;
               src/Disk35Image.cpp:745-754 (même règle : « The final delta
               closes the circular track at its index » → `continue`) ;
               écrivains alignés sur la même lecture : src/DiskImageWozFlux.cpp:325
               (`appendGap(period); // terminal wrap, no pulse`) et
               src/Disk35Image_Bitstream.cpp:224 (`delta(period - previous)`) ;
               test qui épingle la mauvaise règle : tests/woz_bit_timing_smoke_test.cpp:251
               (« the final byte never emits », 4 transitions attendues au lieu de 5).
Amont        : [B] WOZ 2.1 reference (TRKS + FLUX), MOOF reference, A2R reference ;
               [D] MAME as_dsk.cpp:81-102 @ mame0287 (= master), flopimg.h:471.
Gravité      : MAJEUR
Confiance    : CONFIRMÉ pour la divergence (spec et MAME lus, deux sondes, deux
               mutations). Portée PLAUSIBLE : aucune vraie image FLUX sous la main,
               donc je ne sais pas où Applesauce place la couture dans ses captures.
```

**Divergence.** La spec est explicite et le même encodage sert au WOZ, au MOOF
et à l'A2R : « Each byte represents a single flux transition and its value is
the number of ticks since the previous flux transition », 255 servant de
continuation, et les flux sont « properly looped so there is no time warp when
wrapping ». Le dernier octet non-255 est donc une transition : celle qui ferme
la boucle, à la position angulaire 0. MAME la restitue avec
`buf.push_back(floppy_image::MG_F | 0)` et le dit : « There is always a pulse at
index, and it's the last one in the stream ». `MG_F` est un changement
d'orientation du flux (`flopimg.h:471`), pas un marqueur de temps.

POM2 a lu `MG_F|0` comme « the floppy_image time-zero marker » et saute le
dernier octet sans rien poser en 0. Ses **deux** chargeurs le font
(`DiskImage` 5,25", `Disk35Image` 3,5" WOZ/MOOF), et ses **deux** écrivains
réécrivent ce dernier écart comme un « wrap » sans impulsion. Résultat : une
transition de moins par tour. Deux intervalles fusionnent à la couture, et un
bit « 1 » devient « 0 », ou ouvre une zone faible si l'écart fusionné atteint
16 µs.

**Scénario.**

- *5,25"* : WOZ 2.1 dont une piste est en FLUX (`INFO` v3, `FLUX` non nul)
  et dont la couture tombe dans un champ. À chaque tour, le nibble à cheval
  sur la couture est faux, le secteur échoue à sa somme de contrôle, et toujours
  au même endroit. Les pistes FLUX existent précisément pour les protections
  qui exigent ce niveau de fidélité (`DiskImage.cpp:1016-1018` en nomme quatre).
- *3,5"* : `Disk35Image` décode les blocs **au montage**. Le secteur à cheval
  sur la couture ne se décode plus et le bloc se lit en zéros, avec un simple
  avertissement « decoded N of M blocks ».
- *Interopérabilité* : un fichier FLUX réécrit par POM2 gagne, pour tout
  lecteur conforme (MAME, matériel), une transition parasite en 0 qui n'était
  pas sur la surface.

**Observation.** Deux sondes synthétiques et deux mutations, toutes hors dépôt :

1. `/tmp/audit_flopimg/mkflux.py` fabrique un WOZ 2.1 dont la piste 0 est en
   FLUX : 22 enregistrements `D5 AA AD` + 256 nibbles connus, couture au
   milieu du nibble n° 100 d'un enregistrement.
   `/tmp/audit_flopimg/probe_flux_seam.cpp` lit 12 Ko de nibbles par une
   boucle 6502 réelle (`LDA $C08C,X / BPL`) sur `DiskIICard` avec la PROM P6
   de `roms/`. Lancée avec `POM2_MEDIA_WRITE_DEFAULT=protected`, sur une copie
   en lecture seule :

   ```
   cells=50336 transitions=35024 bytes=35024 last_byte=64 seam=record 10 data nibble 100 ($DB)
   flux events on qt0: 35023, period 402688 LSS cycles
     record at $27BE: 1 bad nibbles, first at #100: D9(D9) DA(DA) CB(DB) DC(DC) DD(DD) DE(DE) DF(DF) E5(E5)
   records read: 43, corrupted: 1
   ```

   Le fichier porte 35 024 transitions et POM2 en charge 35 023 ; `$DB` est lu
   `$CB`. Coutures en enregistrements 3 et 17 : même résultat.

   **Mutation** (`/tmp/audit_flopimg/DiskImage_mut.cpp`, une transition posée
   en 0 quand le dernier octet n'est pas 255) : `35024` événements,
   `records read: 43, corrupted: 0`, pour les trois coutures.

2. `/tmp/audit_flopimg/probe_moof_seam.cpp`, greffé sur
   `tests/Woz35Fixture.h` : piste 0 face 0 d'un 800K encodée en GCR Sony
   (`encodeTrack`), convertie en MOOF FLUX avec couture à 10 %, 37 % puis 71 %
   du tour :

   ```
   seam at 0.10 …  track 0 side 0: 11/12 blocks intact  block 1 LOST
   seam at 0.37 …  track 0 side 0: 11/12 blocks intact  block 4 LOST
   seam at 0.71 …  track 0 side 0: 11/12 blocks intact  block 8 LOST
   ```

   **Mutation** (`/tmp/audit_flopimg/Disk35Image_mut.cpp`, dernier octet posé
   en 0) : `12/12 blocks intact` pour les trois.

**Ce qu'un correctif touche.** Les deux chargeurs, les deux écrivains (qui
doivent finir le flux **sur** une transition, en faisant tourner la liste) et
deux tests :

- `woz_bit_timing_smoke` : les deltas `{100,100,FF,55,100,45}` donnent cinq
  transitions (100, 200, 510, 610, 655 ≡ 0), pas quatre ;
- l'aller-retour FLUX de `woz_writeback_smoke`.

Le commentaire de `DiskImage.cpp:1166-1173` est à réécrire avec la ligne
corrigée ci-dessus.

---

## AMONT FAUX

Gravité `—`. Trois endroits où POM2 a raison contre MAME. Ils protègent le
code contre un futur « alignement sur MAME ».

### A. MAME refuse un WOZ ou un MOOF dont le CRC vaut zéro

```
Fichier      : src/DiskImage.cpp:976 (`if (expected != 0)`), src/Disk35Image.cpp:609
Amont        : as_dsk.cpp:324-327 (WOZ) et 509-511 (MOOF) @ mame0287 = master ;
               toutes les révisions d'ap2_dsk.cpp depuis 2018
Source       : [B] WOZ 2.1 reference — "If the CRC is 0x00000000, then no CRC has
               been calculated for the file and should be ignored."
               [D] AppleWin DiskImageHelper.cpp:2123 @ 4a43bcdb — même règle.
```

MAME compare sans exception. Un WOZ dont l'outil n'a pas calculé le CRC, ou
tout WOZ **sauvé par POM2** (qui met le CRC à zéro, `DiskImage.cpp:2914`),
ne s'ouvre pas dans MAME. POM2 et AppleWin suivent la spec. Deux citations
POM2 attribuaient cette règle à MAME : elles sont classées INEXACTE plus haut.

### B. MAME ne charge que 141 quarts de piste en 5,25"

```
Fichier      : src/DiskImage.cpp:1265 (les 160 entrées de TMAP), DiskIICard.cpp:51
               (la tête va jusqu'au quart de piste 156 = piste 39)
Amont        : as_dsk.cpp:352 @ mame0287 — `unsigned int limit = is_35 ? 160 : 141;`
Source       : [B] WOZ 2.1 reference, TMAP — "The table only shows to track 35, but
               can also accommodate 40 track disks."
```

Un WOZ 40 pistes perd ses pistes 35.25 à 39.75 dans MAME. *Balance of Power*
lit justement la piste 35.00 = quart de piste 140, la dernière que MAME garde
(et KEGS s'y était cassé, GH#1022).

### C. MAME rend `$FF` sur une adresse impaire du Disk II

```
Fichier      : src/DiskIICard.cpp:1981-1984 (`return openBus();`)
Amont        : wozfdc.cpp:215 @ mame0287 (`return 0xff;`)
Source       : [B] WOZ 2.1 reference — "the low bit of the address line (A0) is
               connected through a NOT gate to the Output Enable line of the data
               latch": une adresse impaire ne pilote pas le bus.
               [D] AppleWin Disk.cpp:2190-2199 @ 4a43bcdb — « only even addresses
               return the latch (UTAIIe Table 9.1) », sinon MemReadFloatingBus.
```

MAME et AppleWin se contredisent. AppleWin colle au câblage décrit par la spec
et par Sather, cité par AppleWin mais non relu ici. POM2 rend le bus flottant,
comme AppleWin.

---

## Pistes examinées et écartées

**Position angulaire au changement de piste — ne pas « corriger » seul.** La
spec (« new_position = current_position * new_track_length /
current_track_length ») et AppleWin (`Disk.cpp:377`) conservent la **fraction**
de tour. MAME la conserve aussi, en ramenant chaque piste à une révolution de
200 ms (`flopimg.cpp:916-920`). POM2 garde le **temps** écoulé depuis l'ancre
(`DiskIICard.cpp:1328-1336`, `DiskImage::getNextTransition`), avec une période
propre à chaque piste. Sondé sur *Balance of Power* (WOZ du ticket GH#1022,
piste 0 = 51 416 bits, piste 35 = 50 007 ; contrôle « cross-track sync »
T$00 S$0A → T$23, puis recherche de `F2…FE` dans 256 nibbles) :

- **POM2 actuel** : passe, séquence en `$DC87`, Y=`$94` ;
- **mutation qui applique la formule de la spec** (`DiskIICard_mut.cpp`,
  `AUDIT_XTRACK=1`) : **échoue**, séquence repoussée en `$DCF8`, hors fenêtre ;
- pour mémoire, d'après le ticket : MAME 0.236 la trouve en `$DCC1`-`$DCC5`,
  AppleWin en `$DCDD` grâce à un arrondi de +7 bits ajouté exprès.

Le contrôle est sensible à 1 % de vitesse : selon le ticket, il échoue sur
l'un des deux lecteurs réels de l'auteur. La règle de la spec n'est sûre
qu'avec une vitesse de rotation fidèle. Or POM2 fait tourner chaque piste à
`bits × 8` cycles LSS : 201,1 ms pour T0, 195,6 ms pour T35, au lieu de 200 ms
fixes. Écart réel mais latent : je n'ai pas de titre qui échoue aujourd'hui.

**Autres pistes :**

- **Commentaire « 2,2 % trop vite »** (`DiskImage.cpp:1768-1789`, divergence
  documentée) : sa prémisse, une cellule nominale de 4 µs, ne vaut pas pour le
  WOZ. Une piste écrite par un Apple (51 416 cellules) dure 201,1 ms à
  8 cycles LSS. Et la cellule 5,25" de MAME est elle-même de 3 915 ns
  (`as_dsk.cpp:115` @ mame0287), soit 8,0 de ses cycles LSS. À relire avant
  tout « re-basage ».
- **Pistes courtes** : MAME complète de zéros jusqu'à 51 090 bits une piste de
  moins de 48 535 bits (`as_dsk.cpp:62-71`). POM2 boucle la piste courte telle
  quelle. Aucune image concernée trouvée. Latent.
- **`INFO` version > 3** : `DiskImage` la refuse, comme MAME, alors que la spec
  demande `>=`. `Disk35Image` ne la vérifie pas. Les deux chargeurs divergent,
  mais aucun WOZ v4 n'existe. Latent.
- **Optimal bit timing 3,5" WOZ** : `Disk35Image` l'accepte sans borne
  (`buf[infoOff+39] ? … : 16`) ; `DiskImage` et le MOOF bornent à 8..64. Latent.
- **Boot sector format** (`INFO+38`) : 2 → 13 secteurs, 3 → 16, conforme à la
  spec. MAME ne lit pas ce champ.
- **Vérification de l'ID du chunk FLUX** (POM2 seul) et règle « les deux
  champs non nuls » : conformes à la spec, sans effet de bord.
- **Structure TRK du WOZ 1** (6646 / 6648 / 6650 / 6652 / 6653) : conforme à
  la spec 1.0. Le point d'épissure n'est qu'une indication d'écriture vers une
  vraie disquette, ignoré sans conséquence.
- **Conversion bits → flux** : une transition au centre de chaque cellule à 1
  (`i*cyc + cyc/2`), comme MAME (`i*2+1`, `flopimg.cpp:918`).
- **TMAP `$FF`** : POM2 sert un bruit d'octets (`advanceNoise`), là où la spec
  recommande des bits aléatoires à ~30 % de 1. Aucun scénario identifié.
- **Bits faibles** : POM2 suit MAME (zones ≥ 16 µs, une impulsion sur deux à
  4 µs), la spec recommande une fenêtre de 4 bits à ~30 %. Les deux modèles
  changent bien d'une lecture à l'autre. Domaine de la tranche `floppy.cpp`.
- **LSS avec la PROM P6** : identique à `lss_sync` @ 0287, qu'il s'agisse de
  l'adressage, du répartiteur d'opcodes, du décalage avec WPT, du chargement
  de `last_6502_write`, de QA, de la fenêtre PULSE ou de l'ordre « détection
  de front d'écriture, puis état ». Le reset du séquenceur sur `$C08D` (E7)
  émerge de la vraie PROM, comme dans MAME. `write()` fait `lss_sync` →
  `control` → latch, sans `lss_sync(1)`, comme MAME.
- **`control()`** `$C`/`$D`/`$E`/`$F` et les phases (`if (active)
  seek_phase_w`) : identiques. Écarts **voulus** : délai moteur 1,136 s (556,
  schéma 050-0005, `DiskIICard.h:510-519`) ; `lssSync` sans compensation
  d'offset (`DiskIICard.cpp:1281-1312`) ; séquenceur et boot PROM propres ;
  crochets IWM sous `setIwmHost()` ; vidange de l'écriture avant un pas de
  tête (`DiskIICard.cpp:1620-1631`).
- **Changement de lecteur en pleine écriture** : MAME laisse `write_buffer`
  dans le contrôleur, qui part au prochain `write_flux`, donc sur le
  **nouveau** lecteur. POM2 le pose sur l'ancien. POM2 est plus physique
  (grade E : seul le lecteur sélectionné reçoit le courant d'écriture), et le
  chemin est exotique.
- **Ctrl-Reset** : AppleWin, citant *UTAIIe* table 9.1, remet tous les
  interrupteurs à zéro, y compris la **sélection du lecteur 1**, et laisse le
  moteur finir sa course (`Disk.cpp:1797-1830`). POM2 (`onReset`) garde
  `activeDrive` et coupe la broche net. Le `device_reset` de MAME (`5d5ef79d`)
  n'est pas un Ctrl-Reset. Pas de scénario : la boot PROM et RWTS
  resélectionnent leur lecteur. Non tranché, faute du schéma (broche RESET du
  556, `/CLR` du 9334) et de Sather.
- **Tables GCR 6&2** : identiques à MAME et au RWTS de DOS 3.3 (`$BA29`), et
  présentes dans la ROM //c+. **Encodeur Sony** (`buildTrackBits`) : fidèle à
  `build_mac_track_gcr`. 6 208 cellules par secteur, donc la pré-amorce tombe
  juste et le bourrage final ne sert jamais ; `kCellsPerRev` est exact.
- **CRC-32** : implémentations identiques dans `DiskImage` et `Disk35Image`,
  et conformes à l'annexe A de la spec.
- **Dette documentaire, sans effet sur le comportement** :
  - `DEV.md` § SmartPort 3.5" donne encore les anciennes valeurs
    `kCellsPerRev` (70 695, 64 234, 51 388), que le code a corrigées ;
  - `DiskIICard.cpp:1180-1186` affirme que POM2 « has not taken »
    `bc9aec5a`, alors que le code reprend à l'angle d'arrêt depuis
    `ad4d8192` ;
  - `DiskImage.h:500-501` parle d'un WOZ FLUX « forced » protégé en
    écriture, alors que l'encodeur existe et que le test vérifie l'inverse.

---

## Bilan

- **Citations examinées** : 27 sur 27 (11 `flopimg`, 10 `as_dsk`,
  6 `wozfdc`). Datation sur 65 + 9 (+ 43 `ap2_dsk`) + 28 révisions, et
  confrontation au contenu de `mame0287`.
- **Verdicts** :
  - INEXACTE **4** (toutes `as_dsk`) ;
  - DÉCALÉE **10** (6 `as_dsk`, 3 `wozfdc`, 1 `flopimg`) ;
  - OK (rev X) **8** (6 `flopimg`, 2 `wozfdc`) ;
  - OK **5** (4 `flopimg`, 1 `wozfdc`) ;
  - ABSENTE 0 ; NON-PINNÉE 0.
- **Constats** : CRITIQUE 0 · **MAJEUR 1** (transition de couture des pistes
  FLUX, 5,25" et 3,5") · MINEUR 0 · **AMONT FAUX 3** (CRC nul ; 141 quarts de
  piste ; `$FF` sur adresse impaire).
- **Révisions de référence** : `mame0287`, soit `flopimg` = `23f379f741`,
  `as_dsk` = `0d9909dc01` (numérotation identique sur `master`
  `869b1239ac`), `wozfdc` = `acb5d3fc93`. Les citations se répartissent sur
  au moins **cinq époques** :
  - `wozfdc` 2015 et 2026-08 ;
  - `flopimg` 2025-11, 2026-05 et 2026-09 ;
  - plus le bloc `as_dsk` qui ne colle à aucune révision.
- **État des pins** : aucun. Les fonctions portées de `flopimg.cpp` et les
  chargeurs d'`as_dsk.cpp` n'ont pas bougé de `mame0287` à `master` : un pin
  `mame0287` les décrit. Pour `wozfdc.cpp`, POM2 suit `mame0287` ;
  `c690a9ed` n'est pas pris (sans effet) et `5d5ef79d` l'est en esprit, sur
  le Ctrl-Reset.
- **Non vérifié** :
  - **aucune vraie image WOZ 2.1 ou MOOF à pistes FLUX** sous la main (les
    WOZ-a-day téléchargés sont en flux de bits). La position de la couture
    dans les captures Applesauce, donc la fréquence réelle du constat 1,
    reste à mesurer sur une image du corpus ;
  - Sather, *Understanding the Apple II(e)* ch. 9 (table 9.1, timing du LSS) :
    non relu, cité à travers AppleWin (grade D) ;
  - le schéma Disk II 050-0005 (RESET vers le 556 et le 9334) ;
  - le RWTS désassemblé au-delà de la localisation de la table `$BA29` ;
  - le côté lecteur de MAME (`floppy.cpp`) pour savoir si son lecteur 5,25"
    dépasse la piste 35 : l'AMONT FAUX B porte sur le chargeur seul ;
  - *Glutton* (AppleWin GH#1021), autre contrôle « cross-track », non sondé ;
  - le chemin de lecture 3,5" en flux (`Sony35Drive` à partir des
    transitions de `Disk35Image`) : non sondé séparément. Il lit la même liste
    de transitions, donc la même transition manquante.

---

## Suite donnée (2026-11-01)

Constat 1 **corrigé** dans les deux chargeurs (`DiskImage.cpp`, `Disk35Image.cpp`)
et les deux écrivains (`DiskImageWozFlux.cpp`, `Disk35Image_Bitstream.cpp`),
par un agent qui a relu les références WOZ 2.1 et MOOF. Le mutant confirme le
symptôme : « decoded 11 of 1600 blocks » au montage 3,5". Épinglé par
`moof35_load` (couture dans un champ de données), `woz_writeback` (réécriture
octet pour octet, rotation si la transition en 0 est effacée),
`woz_bit_timing` (cinq transitions). Les trois `AMONT FAUX` (CRC nul, 141
quarts de piste, `$FF` sur adresse impaire) restent tels quels. Non vérifié :
une capture Applesauce FLUX réelle ; que ni le Disk II ni le Sony ne lisent
un index (ce qui rend la rotation invisible).
