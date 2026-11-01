# Audit de parité — citations de MAME `apple2e.cpp` (2026-11-01)

Tranche 2 du brief `docs/audits/BRIEF-parite-amont.md`. Toutes les citations
`apple2e.cpp:<lignes>` sous `src/` :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep -E '(^|/|:)apple2e\.cpp:'
```

82 citations dans 20 fichiers (33 dans `Memory.cpp`, 9 dans
`MemoryProfile_IIcClass.cpp`). Aucun fichier de code n'a été modifié. Le fichier
amont est `src/mame/apple/apple2e.cpp`, et non `src/devices/`.

## Révision amont

**Un seul pin dans la tranche** : `ZipChip.h:20-21` dit « Line numbers below are
mamedev/mame master as fetched 2026-10-16 », ce qui correspond à `fc6d4bf5`
(2026-09-04, dernier commit sur ce fichier avant cette date). Aucun autre
fichier POM2 ni DEV.md n'épingle de révision d'`apple2e.cpp`.

Verdicts rendus contre le tag **`mame0287`**. Son `apple2e.cpp` est identique
octet pour octet au commit `13184ea2` (2026-03-08).

Pour dater chaque citation, j'ai récupéré **91 révisions** du fichier
(`cf11b333`, 2022-06-27, déplacement dans `src/mame/apple/`, jusqu'à `b2a182f0`
= `master`, 2026-10-29). À chaque citation correspond une ancre textuelle, le
code que POM2 décrit, et une révision « tombe juste » quand la plage citée
contient cette ancre. Les ancres et le script sont décrits dans le bilan.

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|---|
| `13184ea2` = **`mame0287`** | 2026-03-08 | Sonde //c+ par l'octet ROM `$3BBF`, Zip simple (sans masque `$E4` ni délai de 52 ms), `reset_w` sans `accel_reset` | **7** : `Memory.cpp:295, 716, 747, 1089`, `EmulationController.cpp:973`, `MemoryProfile_IIcClass.{h:38, cpp:35}` |
| `6676d572` → `b2a182f0` = **`master`** | 2026-09-02 → 2026-10-29 | Zip complète (prise d'écriture sur tout l'espace, `$C05E`, délais), `c080_r`/`read_slot_rom`/`c800_r` à leurs positions actuelles, //c+ identifié par sa config machine | **26** : `SlotBus.{cpp,h}` (6), `ZipChip.h` (3, = pin `fc6d4bf5`), `PaddleInputs.h` (2), `MemoryLanguageCard.cpp:114, 145`, `Memory.h:1368, 1464`, `Memory.cpp:1136, 1280, 1285, 1290, 1343, 1411`, `GrapplerCard.h:100`, `CassetteDevice.cpp:721`, `MemoryProfile_IIcClass.cpp:450`, `SuperSerialCard.cpp:840` |
| `2e65fff8` → `774a180d` | 2026-07-11 → 08-02 | Nettoyage de juillet | `Memory.cpp:989, 998, 1101` |
| `3924a69f` → `6da5e1f1` | 2026-06-08 → 06-30 | Zip améliorée, arbitrage de slots | `Memory.cpp:1045` |
| `bcc561af` (seule) | 2026-05-08 | « debugger improvements » | `Memory.cpp:968, 977, 1818, 1820, 1833` |
| `bcc561af` → `b7010dac` / `b76d72b2` / `d0e8fc89` | 2026-05 → 2026-08 | idem, positions stables quelques semaines | `Memory.cpp:701, 1260, 1881, 2214`, `MemoryProfile_IIcClass.cpp:52, 57, 240, 302`, `SystemProfile.cpp:119`, `MainWindow_Input.cpp:201` |
| `13a2cd29` → `ec1c79fb` | 2026-01-19 | tk3000 | `SystemProfile.cpp:74` |
| `7da65f10` → `b76d72b2` | 2022-09 → 2026-05 | — | `Memory.cpp:680`, `Memory.h:1491` |
| `f4293f9d` → `d0e8fc89` | 2022-11 → 2026-05 | — | `SystemProfile.cpp:186`, `MemoryProfile_IIcClass.cpp:149` |
| `597ee869` → `6a4854bb` | 2023-01 → 2023-05 | — | `MemoryLanguageCard.cpp:36` (`lc_update`) |
| `ab6237da` → `714fb0bc` | 2022-06 → 2026-01 | — | `mig_r`/`mig_w` 532-624 (×3) |
| **aucune** (2022-06 → 2026-10) | — | — | **19**, dont tout le bloc SmartPort (`phases_w 625-631`, `sel35_w 633-635`, `recalc_active_device 638-679`, `mig_r 532-569`) et la plupart des numéros de `c000_r`/`do_io` dans `Memory.cpp` |

Le constat de la tranche 1 se confirme : **au moins neuf époques amont se
côtoient**, de 2022 à octobre 2026, souvent à l'intérieur d'un même fichier.
`Memory.cpp` cite à lui seul `mame0287`, `master`, `bcc561af`, la série de
juillet et des numéros introuvables. Le bloc SmartPort (`SmartPortHub`,
`IWMDevice.h`, `Memory.h:152`) cite des positions qu'aucune révision depuis
2022-06 ne contient : en 2022, `phases_w` était en 645 et `recalc_active_device`
en 663, et les écarts entre fonctions ne correspondent pas non plus.

---

## Rapport 1 — Citations

Ordre : INEXACTE, DÉCALÉE (aucune révision), OK (rev X), OK. « 0287:n » donne
la position à la révision de référence.

| Fichier POM2:ligne | Citation | Verdict | Commentaire |
|---|---|---|---|
| `src/Memory.cpp:709` | `apple2e.cpp:1224` | INEXACTE | Le commentaire affirme « IOUDIS resets to true on every reset ». L'amont ne fait `m_ioudis = true` que dans `machine_reset` (0287:1215) ; `reset_w`, le Ctrl-Reset (0287:1440-1511, inchangé sur `master`), ne touche pas IOUDIS. L'écart de comportement est le point ouvert `CHANGELOG.md:2389` (voir « Pistes »). |
| `src/DiskIICard.h:761` | `apple2e.cpp:1180-1185` | INEXACTE | Attribue à la « chaîne `phases_w` » un appel `m_iwm->set_floppy(...)` « à chaque réinsertion ». L'amont `phases_w` (0287:701-707) ne fait que `seek_phase_w` ; `set_floppy` n'est appelé que dans `recalc_active_device` (0287:754), sur changement de lecteur, jamais sur pas de tête. 1180-1185 sont des `save_item`. |
| `src/DiskIICard.cpp:805` | `apple2e.cpp:1180-1185` | INEXACTE | « Mirror 5.25-inch image/head changes into the on-board IWM » : la notion de recopie sur pas de tête n'existe pas en amont (même explication que ci-dessus). C'est un choix POM2 né de la séparation DiskIICard/IWM ; la citation ne le justifie pas. |
| `src/IWMDevice.h:46` | `apple2e.cpp:2430-2432` | DÉCALÉE | `m_isiicplus && slot == 6` → `m_iwm` : 0287:2789 (dans `c080_r`), `master`:2900. |
| `src/IWMDevice.h:109` | `apple2e.cpp:638-679` | DÉCALÉE | `recalc_active_device` : 0287:719-760. Aucune révision depuis 2022-06. |
| `src/Memory.cpp:700` | `apple2e.cpp:1273` | DÉCALÉE | INTCXROM forcé sur //c : 0287:1268 (`machine_reset`). |
| `src/Memory.cpp:872` | `apple2e.cpp:1825-1828` | DÉCALÉE | Latch clavier en miroir sur `$C000-$C00F` : 0287:2057-2060. |
| `src/Memory.cpp:941` | `apple2e.cpp:1842-1871` | DÉCALÉE | Lectures d'état `$C011-$C01F` OR `m_transchar` : 0287:2076-2119. |
| `src/Memory.cpp:1040` | `apple2e.cpp:1750-1773` | DÉCALÉE | Annonciateurs AN0-AN2 : 0287:1968-1996. |
| `src/Memory.cpp:1127` | `apple2e.cpp:1711-1716` | DÉCALÉE | Strobe `$C040` : 0287:1919-1925. Il est gardé par `!m_isiic` en amont, ce que le commentaire « on every access » omet (sans effet : le //c passe par la souris IOU avant). |
| `src/Memory.cpp:1301` | `apple2e.cpp:1889` | DÉCALÉE | Cas `$C061-$C06F` de `c000_r` : 0287:2148-2186. Les six autres numéros de la même phrase (1903…1927) ne collent pas davantage. |
| `src/Memory.cpp:1381` | `apple2e.cpp:2569-2587` | DÉCALÉE | SET/CLR IOUDIS sur `$C078-$C07F` : 0287:2558-2574 (`c000_w`). |
| `src/Memory.cpp:1515` | `apple2e.cpp:1842-1871` | DÉCALÉE | Comme `Memory.cpp:941`. |
| `src/Memory.h:152` | `apple2e.cpp:638-679` | DÉCALÉE | `recalc_active_device` : 0287:719-760. |
| `src/Memory.h:903` | `apple2e.cpp:1908-1913` | DÉCALÉE | Shift → PB2 (`m_shift_key_mod`) : 0287:2162-2167. |
| `src/EmulationController.cpp:404` | `apple2e.cpp:4521-4524` | DÉCALÉE | `add_35(m_floppy[2..3])` : 0287:5336-5337 (`apple2cp`). |
| `src/MemoryProfile_IIcClass.cpp:216` | `apple2e.cpp:3148-3151` | DÉCALÉE | `c800_b2_int_r` et sa garde MIG : 0287:3135-3140. |
| `src/MemoryProfile_IIcClass.cpp:270` | `apple2e.cpp:532-569` | DÉCALÉE | `mig_r` : 0287:593-636. Aucune révision ne lui donne 38 lignes. |
| `src/SmartPortHub.h:19` | `apple2e.cpp:638-679` | DÉCALÉE | `recalc_active_device` : 0287:719-760. |
| `src/SmartPortHub.cpp:17` | `apple2e.cpp:638-679` | DÉCALÉE | Idem ; « lines 625-637 » pour les callbacks : 0287:701-717. |
| `src/SmartPortHub.cpp:36` | `apple2e.cpp:633-635` | DÉCALÉE | `sel35_w` vide : 0287:715-717. Contenu exact. |
| `src/SmartPortHub.cpp:155` | `apple2e.cpp:625-631` | DÉCALÉE | `phases_w` : 0287:701-707. |
| `src/CassetteDevice.cpp:721` | `apple2e.cpp:2180` | OK (master) | `input() > 0.0 ? 0 : 0x80` ; 0287:2142. |
| `src/GrapplerCard.h:100` | `apple2e.cpp:2977` | OK (master) | `take_c800()` dans `read_slot_rom` ; 0287:2865. |
| `src/MainWindow_Input.cpp:201` | `apple2e.cpp:2157-2169` | OK (rev `b76d72b2`–`d0e8fc89`) | `$C061`/`$C062` Open/Solid-Apple ; 0287:2148-2160. |
| `src/Memory.cpp:680` | `apple2e.cpp:1227-1232` | OK (rev `7da65f10`–`b76d72b2`) | `m_lcwriteenable = true` au reset ; 0287:1223. |
| `src/Memory.cpp:701` | `apple2e.cpp:1467-1475` | OK (rev `bcc561af`–`6da5e1f1`) | INTCXROM rétabli par `reset_w` pour le //c ; 0287:1457-1466. |
| `src/Memory.cpp:968` | `apple2e.cpp:2107` | OK (rev `bcc561af`) | VBLBAR `$C019` ; 0287:2100-2101. |
| `src/Memory.cpp:977` | `apple2e.cpp:2244` | OK (rev `bcc561af`) | « does not reset » ; 0287:2235. |
| `src/Memory.cpp:989` | `apple2e.cpp:2256-2257` | OK (rev `2e65fff8`–`774a180d`) | VBLINT //c ; 0287:2235-2236. |
| `src/Memory.cpp:998` | `apple2e.cpp:2092-2093` | OK (rev `2e65fff8`–`774a180d`) | VBLBAR //e ; 0287:2100-2101. |
| `src/Memory.cpp:1045` | `apple2e.cpp:2057-2065` | OK (rev `3924a69f`–`6da5e1f1`) | `lower_irq(IRQ_VBL)` réservé au //c ; 0287:2014-2017. |
| `src/Memory.cpp:1101` | `apple2e.cpp:1234` | OK (rev `6da5e1f1`–`774a180d`) | `m_ioudis = true`, dans `machine_reset` seulement ; 0287:1215. |
| `src/Memory.cpp:1136` | `apple2e.cpp:2312-2313` | OK (master) | `$C041` RDVBLMSK ; 0287:2241-2242. |
| `src/Memory.cpp:1260` | `apple2e.cpp:1907-1923` | OK (rev `bcc561af`–`b7010dac`) | ROMBANK `$C02x` ; 0287:1898-1914. |
| `src/Memory.cpp:1280` | `apple2e.cpp:2177-2185` | OK (master) | `$C060`/`$C068` cassette + bus flottant ; 0287:2139-2146. |
| `src/Memory.cpp:1285` | `apple2e.cpp:2039` | OK (master) | `case 0x68` inerte en écriture ; preuve mince de « read-only », mais exacte (le `c000_w` amont n'a d'effet sur aucun `$C06x`). |
| `src/Memory.cpp:1290` | `apple2e.cpp:2321` | OK (master) | Inverseur 40/80 du //c ; 0287:2250-2251. |
| `src/Memory.cpp:1343` | `apple2e.cpp:2053-2056` | OK (master) | 558 non redéclenchable ; 0287:2027-2043. |
| `src/Memory.cpp:1411` | `apple2e.cpp:2341-2343` | OK (master, ±1) | RDDHIRES inversé ; le `case` est en 2340 sur `master` ; 0287:2269-2271. |
| `src/Memory.cpp:1818` | `apple2e.cpp:5249-5254` | OK (rev `bcc561af`) | `apple2c_iwm` → `A2BUS_IWM` ; 0287:5342-5347. |
| `src/Memory.cpp:1820` | `apple2e.cpp:5212` | OK (rev `bcc561af`) | `A2BUS_DISKIING` en sl6 du //c 16 Ko ; 0287:5305. |
| `src/Memory.cpp:1833` | `apple2e.cpp:6281-6302` | OK (rev `bcc561af`) | Lignes `COMP` des //c 32 Ko ; 0287 vers 6374-6395. |
| `src/Memory.cpp:1881` | `apple2e.cpp:1619-1631` | OK (rev `bcc561af`–`b7010dac`) | `update_slotrom_banks` OR `m_isiic` ; 0287:1604-1658. |
| `src/Memory.cpp:2214` | `apple2e.cpp:3186-3190` | OK (rev `bcc561af`–`b76d72b2`) | `c800_w` → `mig_w` ; 0287:3175-3177. |
| `src/Memory.h:1368` | `apple2e.cpp:2266-2275` | OK (master) | Octet `$C05E` de la Zip ; absent de 0287 (Zip simple). |
| `src/Memory.h:1464` | `apple2e.cpp:1292` | OK (master) | `m_mockingboard4c = false` dans `machine_reset` ; 0287:1247. |
| `src/Memory.h:1491` | `apple2e.cpp:1224` | OK (rev `7da65f10`–`b76d72b2`) | « initialises to true » : juste (machine_reset) ; 0287:1215. |
| `src/MemoryLanguageCard.cpp:36` | `apple2e.cpp:1506-1564` | OK (rev `597ee869`–`6a4854bb`, 2023) | `lc_update` ; 0287:1686-1752. Les citations les plus anciennes de la tranche. |
| `src/MemoryLanguageCard.cpp:114` | `apple2e.cpp:826-870` | OK (master) | `a2bus_inh_w`, avec `inh_w :3321` également juste sur `master` ; 0287:780-826. |
| `src/MemoryLanguageCard.cpp:145` | `apple2e.cpp:2665` | OK (master) | `write_c07x` vers la carte aux ; 0287:2551. |
| `src/MemoryProfile_IIcClass.cpp:52` | `apple2e.cpp:1907-1923` | OK (rev `bcc561af`–`b7010dac`) | `m_romswitch = !m_romswitch` ; 0287:1903. |
| `src/MemoryProfile_IIcClass.cpp:57` | `apple2e.cpp:1917-1922` | OK (rev `bcc561af`–`b7010dac`) | Reset MIG sur retour en banque 0 ; 0287:1907-1913. |
| `src/MemoryProfile_IIcClass.cpp:149` | `apple2e.cpp:5249-5272` | OK (rev `f4293f9d`–`b76d72b2`) | `apple2c_iwm`/`apple2c_mem` ; 0287:5342-5365. |
| `src/MemoryProfile_IIcClass.cpp:240` | `apple2e.cpp:3186-3190` | OK (rev `bcc561af`–`b76d72b2`) | `c800_w` MIG ; 0287:3175-3177. |
| `src/MemoryProfile_IIcClass.cpp:302` | `apple2e.cpp:647-650` | OK (rev `bcc561af`–`d0e8fc89`) | `m_iwm->reset()` ; 0287:642-646. |
| `src/MemoryProfile_IIcClass.cpp:450` | `apple2e.cpp:804` | OK (master) | `ss_w(m_hdsel)` en fin de `recalc_active_device` ; 0287:758. |
| `src/MemoryProfile_IIcClass.h:22` | `apple2e.cpp:532-624` | OK (rev `ab6237da`–`714fb0bc`) | `mig_r`+`mig_w` ; 0287:593-699. |
| `src/MemoryProfile_IIcClass.h:90` | `apple2e.cpp:532-624` | OK (rev `ab6237da`–`714fb0bc`) | Idem. |
| `src/PaddleInputs.h:136` | `apple2e.cpp:1007` | OK (master) | 10 800 ns par unité ; 0287:957. |
| `src/PaddleInputs.h:184` | `apple2e.cpp:2053-2056` | OK (master) | Monostables non redéclenchables ; 0287:2027-2043. |
| `src/SlotBus.cpp:141` | `apple2e.cpp:2883-2918` | OK (master) | `c080_r` → bus flottant ; 0287:2769-2810. |
| `src/SlotBus.cpp:166` | `apple2e.cpp:2970-2987` | OK (master) | `read_slot_rom` ; 0287:2859-2876. |
| `src/SlotBus.cpp:193` | `apple2e.cpp:2989-3025` | OK (master) | `write_slot_rom` ; 0287:2877-2912. |
| `src/SlotBus.cpp:209` | `apple2e.cpp:3137-3152` | OK (master) | `c800_r` libère en `$7FF` en servant le propriétaire ; 0287:3028-3051. |
| `src/SlotBus.h:29` | `apple2e.cpp:2970-2987` | OK (master) | Idem, et `:2989-3025`, `:3137-3155` justes sur `master`. |
| `src/SlotBus.h:331` | `apple2e.cpp:216` | OK (master) | `CNXX_UNCLAIMED` ; 0287:200. |
| `src/SuperSerialCard.cpp:840` | `apple2e.cpp:5531-5551` | OK (master) | Câblage DCD/DSR/CTS des deux ports RS-232 du //c ; 0287:5293-5307. |
| `src/SystemProfile.cpp:74` | `apple2e.cpp:5520-5544` | OK (rev `13a2cd29`–`ec1c79fb`) | `ROM_START(apple2e)` ; 0287:5582-5594. |
| `src/SystemProfile.cpp:119` | `apple2e.cpp:5168-5188` | OK (rev `b76d72b2`) | `apple2c()` : retrait sl1-sl7, Mockingboard 4C en sl4, DISKIING en sl6 (vérifié) ; 0287:5259-5310. |
| `src/SystemProfile.cpp:186` | `apple2e.cpp:5229-5249` | OK (rev `f4293f9d`–`d0e8fc89`) | `apple2cp()` retire sl4+sl6 pour l'IWM ; 0287:5320-5340. |
| `src/SystemProfile.cpp:199` | `apple2e.cpp:532-624` | OK (rev `ab6237da`–`714fb0bc`) | `mig_r`/`mig_w` ; 0287:593-699. |
| `src/ZipChip.h:25` | `apple2e.cpp:2562-2632` | OK (rev `fc6d4bf5`, pin du fichier) | Écritures Zip ; le cas `$C05F` est en 2642, juste après la plage (le fichier le cite d'ailleurs en `:2642-2648`). |
| `src/ZipChip.h:39` | `apple2e.cpp:2237-2281` | OK (rev `fc6d4bf5`, pin du fichier) | Lectures Zip déverrouillée. |
| `src/ZipChip.h:92` | `apple2e.cpp:1315` | OK (rev `fc6d4bf5`, pin du fichier) | `m_accel_speed = 4000000` ; 0287:1296. |
| `src/EmulationController.cpp:973` | `apple2e.cpp:1453-1508` | OK | Corps de `reset_w` (MMU/IOU/LC). |
| `src/Memory.cpp:295` | `apple2e.cpp:1275-1299` | OK | Sonde //c+ `m_rom_ptr[0x3bbf] == 0x05`. **Disparue sur `master`**, où le //c+ est identifié par sa config machine (`apple2cp` pose `m_isiicplus = true`, l. 5574). |
| `src/Memory.cpp:716` | `apple2e.cpp:1430-1508` | OK | `reset_w` ne touche pas le slot aux (RamWorks). |
| `src/Memory.cpp:747` | `apple2e.cpp:1014-1035` | OK | Motif RAM `00 FF` à la mise sous tension. |
| `src/Memory.cpp:1089` | `apple2e.cpp:1808-1876` | OK | Branche IOUDIS de `do_io`. |
| `src/MemoryProfile_IIcClass.cpp:35` | `apple2e.cpp:1275-1299` | OK | Sonde `$3BBF` (même remarque que `Memory.cpp:295`). |
| `src/MemoryProfile_IIcClass.h:38` | `apple2e.cpp:1275-1299` | OK | Idem. |

---

## Rapport 2 — Constats

**Aucun constat.** Je n'ai trouvé dans cette tranche aucune divergence de
comportement non documentée dont je puisse écrire le scénario d'échec. Les trois
citations INEXACTES sont de la dette documentaire. La seule divergence
observable relevée (IOUDIS et masque VBL au Ctrl-Reset) est déjà consignée comme
point ouvert, et je n'ai pas pu trancher côté silicium : voir ci-dessous.

---

## Pistes examinées et écartées

- **Port MIG** (`migRead`/`migWrite`) : identique à `mig_r`/`mig_w`, y compris
  les fenêtres `$CC00`/`$CE00` → `mig_r(offset - 0x400)` et le masquage de
  l'index de page. La polarité de `$CE40`/`$CE60` est inversée par rapport à
  MAME, ce qui est **VOULU** : `MemoryProfile_IIcClass.cpp:327-331`, preuve tirée
  de la ROM //c+ (`$E603`, `$E503`/`$E4F2`). C'est **le silicium contre MAME**.
- **`recalcActiveDevice`** : même table de routage que 0287:719-760 ; le second
  3,5" externe, que MAME laisse à `nullptr`, est **VOULU** (`SmartPortHub.cpp:120-121`,
  Apple TIL03435).
- **IWM du //c+ partagé avec DiskIICard** : MAME fait de l'IWM le seul
  contrôleur du slot 6 (`c080_r`), POM2 répartit 5,25"/3,5". C'est une couture
  HLE documentée (`MemoryProfile_IIcClass.cpp:155-166`, constat de la tranche 1
  pour ses effets de bord).
- **Carte langage** (`languageCardSwitchAccess`) : identique à `lc_update`, avec
  écriture « collante », pré-écriture et sélection de banque.
- **Zip Chip** : fidèle à `fc6d4bf5`, qu'il s'agisse du déverrouillage par
  quatre `$5A`, du verrouillage par `$A5`, des registres `$C05B`/`$C05C`/`$C05E`/`$C05F`,
  des délais (52 ms slot, 5 ms haut-parleur, 5 ms PTRIG sur `$C070` seul, comme
  l'amont), de `accel_reset` ou du bit 7 de `$C05B` sur PH0/1024. Divergences
  **VOULUES** listées dans `ZipChip.h:57-76` : écritures en RAM non vues, boot
  rapide par défaut, `$C05D` absent de l'amont.
- **Ordre Zip / IWM sur `$C0Ex`** : le crochet accélérateur (`Memory.cpp:1807`)
  passe avant le routage IWM (`:1834`), comme `accel_slot(slot)` avant
  `m_iwm->read` dans `c080_r`. Le //c+ ralentit donc bien sur accès disque.
- **Fenêtre `$C800`** : prise gardée par `takesC800()`, libération en `$CFFF`
  en lecture (le propriétaire est servi) et en écriture (le propriétaire voit
  l'écriture), libération sous INTCXROM et INTC8ROM : conforme à `read_slot_rom`,
  `write_slot_rom`, `c800_r`, `c800_int_r`, `c800_w`.
- **IOU du //c** : `$C058-$C05F` sous IOUDIS, `$C041`, VBLINT `$C019` (armé
  seulement masque levé, comme `raise_irq`), acquittement par tout
  `$C070-$C07F`, SET/CLR IOUDIS par parité sur `$C078-$C07F`, RDDHIRES inversé,
  `$C015`/`$C017` en drapeaux souris : conformes à `do_io`/`c000_iic_r`/`c000_w`.
- **Surcouche `$C05A`/`$C05B` → masque VBL sur //e** : **VOULUE**
  (`Memory.cpp:1043-1050`, `vbl_smoke_test`).
- **Phase du front VBL de `$C019` sur //e** : **VOULUE**, mesurée sur MAD EFFECT
  (`Memory.cpp:1012-1032`, `vbl_edge_phase`).
- **Ctrl-Reset sur //c : IOUDIS remis à 1 et masque VBL désarmé** : `reset_w`
  de MAME garde les deux (0287:1440-1511, `master`:1430-1513). Le point est déjà
  ouvert dans `CHANGELOG.md:2389-2391`, avec la mention « Unchecked against the
  IIc Technical Reference ». **Non tranché ici** pour la même raison, je n'ai
  pas cette référence. Le volet ROMBANK et sélections MIG a été corrigé depuis
  (`IIcClassProfile::onResetSoftSwitches`, l. 81-91). Il reste une
  contradiction interne : `SmartPortHub::warmReset` (`SmartPortHub.cpp:87-94`)
  efface encore `sel35_`/`intDrive_`/`hdSel_` au reset à chaud, alors que le
  profil les garde. Pour `sel35`/`intDrive`, ce n'est pas observable : le stub
  de reset en banque 1 bascule en banque 0, ce qui les efface dans les deux
  émulateurs. Pour `hdSel`, la bascule ne l'efface pas en amont ; je n'ai pas
  vérifié si le firmware le repositionne avant chaque accès 3,5".
- **Sonde //c+ par l'octet ROM `$3BBF`** : exacte à 0287, remplacée sur
  `master` par l'identification par configuration machine. POM2 doit sonder la
  ROM (ses profils choisissent un fichier ROM) ; c'est équivalent pour les dumps
  authentiques.
- **Inverseur 40/80 du //c** : même sens que MAME (`0x40` = 40 colonnes → bit 7
  à 1), mais valeur par défaut différente : 40 colonnes chez MAME
  (`PORT_CONFNAME(0x40, 0x40, …)`, `master`:3879), 80 chez POM2
  (`iic40ColSwitch_ = false`, `Memory.h:1460`). C'est un réglage physique de
  l'utilisateur, pas une erreur de fidélité.
- **Motif RAM `00 FF`, polarité cassette, 10,8 µs par unité de manette, 558 non
  redéclenchable, `$C060-$C06F` inertes en écriture, mirroirs `$C068-$C06F`** :
  conformes.

---

## Bilan

- **Citations examinées** : 82 sur 82, toutes confrontées à
  `apple2e.cpp@mame0287` téléchargé et datées sur 91 révisions.
- **Verdicts** :
  - INEXACTE 3 ;
  - DÉCALÉE 19 (aucune révision ne tombe juste) ;
  - OK (rev X) 53, dont 26 sur `master` ;
  - OK 7 (sur `mame0287`) ;
  - ABSENTE 0 ;
  - NON-PINNÉE 0.
- **Constats** : CRITIQUE 0 · MAJEUR 0 · MINEUR 0.
- **Révision de référence** : tag `mame0287` (contenu = `13184ea2`). Datation sur
  91 révisions, de `cf11b333` (2022-06-27) à `b2a182f0` (`master`, 2026-10-29).
  Un seul pin dans la tranche (`ZipChip.h`, `fc6d4bf5`).
- **Méthode de datation** : une ancre regex par citation (le code décrit, par
  exemple `0x3bbf\] == 0x05`, `::recalc_active_device\(` + `ss_w\(m_hdsel`).
  « OK (rev X) » veut dire que la plage citée contient l'ancre dans la
  révision X. Le script est dans `/tmp/a2e/date_cites2.py`, hors dépôt. Les
  sémantiques ont été vérifiées à la main pour les zones lues (MIG, SmartPort,
  reset, `do_io`, `c000_r`, `c000_iic_r`, `c000_w`, Zip, slots, `$C800`, LC,
  configs machine), pas citation par citation pour les 53 « OK (rev X) ».
- **Non vérifié** :
  - la *Apple IIc Technical Reference* : effet du RESET sur IOUDIS et le masque
    VBL, et repositionnement de HDSEL par le firmware //c+ ;
  - l'historique antérieur au 2022-06-27, quand le fichier s'appelait
    `src/mame/drivers/apple2e.cpp`. Les 19 citations sans révision pourraient
    en venir, mais l'écart entre fonctions du bloc SmartPort rend la chose peu
    probable ;
  - une ancre trop stricte peut avoir manqué une révision dont le code est mis
    en forme autrement. Pour les 19 DÉCALÉES, la position à 0287 a toutefois été
    lue à la main ;
  - AppleWin, non consulté. Les points où MAME et AppleWin pourraient diverger
    (RDDHIRES/RDIOUDIS sur //e) ont été laissés à la règle MAME, qui cite des
    tests matériels de TomCh.
