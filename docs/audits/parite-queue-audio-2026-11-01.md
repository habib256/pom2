# Audit de parité — file audio et parole : `a2mockingboard`, `tms5220`, `a2sam`, `dac`, `spkrdev`, `filter` (MAME) ; `Mockingboard`, `SSI263`, `6522`, `MockingboardCardManager` (AppleWin) — 2026-11-01

Tranche 22+ du brief `docs/audits/BRIEF-parite-amont.md` : les fichiers amont
audio et parole que les tranches précédentes n'avaient pas couverts. Liste
des citations :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ --include='*.cpp' --include='*.h' \
  | grep -E ':(a2mockingboard|mockingboard|tms5220|a2sam|dac|spkrdev|filter|Mockingboard|SSI263|6522|MockingboardCardManager)\.(cpp|h):'
```

**40 citations**, conformes aux effectifs annoncés par la file : MAME
`a2mockingboard.cpp` 9, `mockingboard.cpp` 1, `tms5220.cpp` 5, `a2sam.cpp` 4,
`dac.cpp` + `dac.h` 2, `spkrdev.cpp` 2, `filter.cpp` 1 ; AppleWin
`Mockingboard.cpp` 9, `SSI263.cpp` 5, `6522.cpp` 1,
`MockingboardCardManager.cpp` 1. Les mêmes commentaires portent en plus
**17 numéros sans préfixe de fichier**, vérifiés eux aussi et listés à part.

**`mockingboard.cpp` n'existe pas, ni chez MAME ni chez AppleWin.** L'API
GitHub ne connaît aucun commit sur `src/devices/bus/a2bus/mockingboard.cpp`
ni sur `src/emu/bus/a2bus/a2mockingboard.cpp`. La seule citation de ce nom
(`Ay3_8910.h:173`) désigne `via_psg_ctrl`, qui se trouve dans le
`a2mockingboard.cpp` de MAME. Le préfixe `a2` a simplement été oublié. Le
fichier AppleWin, lui, s'appelle `Mockingboard.cpp` avec une majuscule, et
il est cité sous ce nom ailleurs.

Comme le brief l'impose, je n'ai modifié aucun fichier de `src/` ou de
`tests/`, je n'ai rien compilé et je n'ai lancé aucune sonde. Les sources
amont, les historiques et les scripts de datation sont sous
`/private/tmp/claude-501/audit_audio/` (`up/`, `revs/`, `awrevs/`,
`scripts/date_cites.py`, `scripts/date_aw.py`).

Code POM2 lu : `Ay3_8910.h` (décodeur BDIR/BC1), `Via6522.h` (événements de
port), `Mockingboard.cpp` (`onViaPortBChange`, reset, mixage),
`PhasorCard.cpp/.h` (décodage, sélection des puces, reset, parole),
`EchoPlusTMS5220Card.cpp`, `Tms5220.cpp` (`write`, `status`),
`Ssi263.cpp` (`reset`, `read`, durée), `SamCard.cpp/.h`, `Dac8Audio.h`,
`SpeakerDevice.cpp/.h` (noyau, convolution, filtre DC), `AyPsgSynth.h`
(`DcBlocker`), ainsi que `tests/mockingboard_bus_edges_test.cpp`.

---

## Révision amont

**Pins trouvés.**
- MAME, Mockingboard et AY : `a2b6ba2d4be7` (`master` du 2026-10-03), dans
  `docs/mockingboard_mame_audit.md:3`, `Ay3_8910.h:114` et `Via6522.h:393`.
- AppleWin : `6c579da5bd94` (2026-10-04), dans `PhasorCard.cpp/.h`,
  `Ssi263.cpp`, `Mockingboard.cpp:870` et `DEV.md` § SSI263.
- `Tms5220*.{h,cpp}` : une date, « (2026-10-18) ».
- S.A.M., DAC, haut-parleur, filtre : aucun pin.

**Les pins décrivent encore le code, et l'amont n'a pas bougé depuis.**
- MAME : pour les six fichiers concernés, `a2b6ba2d` a le même contenu que
  `master` (md5).
- AppleWin : les quatre fichiers sont identiques octet pour octet entre
  `6c579da5` et `master` `4a43bcdb` (2026-10-31).
- Aucun correctif postérieur n'est donc à prendre ou à refuser.

Datation sur **tout l'historique** : 50 révisions de `a2mockingboard.cpp`,
62 de `tms5220.cpp`, 30 de `a2sam.cpp`, 18 de `dac.cpp`, 41 de `dac.h`, 15 de
`spkrdev.cpp` et 10 de `filter.cpp` côté MAME ; 177 de `Mockingboard.cpp`,
44 de `SSI263.cpp`, 21 de `6522.cpp` et 19 de `MockingboardCardManager.cpp`
côté AppleWin. Chaque citation reçoit une ou plusieurs ancres regex.

### MAME

| Fichier | `mame0287` | pin `a2b6ba2d` = `master` | Citations qui tombent juste |
|---|---|---|---|
| `bus/a2bus/a2mockingboard.cpp` | `f135a4e417` (2025-11-10, « a2bus: Interface refinements ») | `ed697c3087` (2026-08-12, `side_effects_disabled` dans `read_c0nx`) | **Toutes** sur `f135a4e4` et sur le pin, **sauf** `585-607` (Echo+ `read_c0nx`/`write_c0nx`), juste **sur le pin seul** (582-604 sur `mame0287`). `128-132` ne tombe juste sur **aucune** des 50 révisions. |
| `sound/tms5220.cpp` | `d0f1c15a0f` (2025-04-27, « New sound infrastructure ») | idem | Toutes, de `d96222a3f0` (2025-02-27) à `master`. Exception : `:1430-1448` (SPEAK), qui ne contient `case 0x50` que dans `34365e7beb` (2017-03-07), et encore sans la remise à zéro de trame qu'il est censé désigner. |
| `bus/a2bus/a2sam.cpp` | `f135a4e417` | idem | `44` depuis `c2c3d685f0` (2021-10-04) ; `55-56` et `56` sur `f135a4e4` seul (= `mame0287` = `master`) |
| `sound/dac.cpp` / `dac.h` | `d0f1c15a0f` / `fe4cd123b9` | idem | `28-33` depuis 2020-10-17 ; `77-78` depuis 2022-07-12 ; `113` sur `d0f1c15a` seul ; `h:237` depuis `6d18df7a3c` (2025-07-13) |
| `sound/spkrdev.cpp` | `bd99a5d7f8` (2025-05-04) | `bdf56c6ae8` (2026-05-29) | **Aucune** : `74` est un `#include` ou une ligne vide selon la révision ; `319-322` est le début de `update_interm_samples_get_filtered_volume`, jamais la convolution |
| `emu/audio_effects/filter.cpp` | `f1b88c41c6` (2025-06-25) | `bdf56c6ae8` | `39-44` sur `bdf56c6a`, `c5aa2a22`, `d0f1c15a` ; **pas** sur `mame0287`, où c'est 37-42 |

### AppleWin

| Fichier | pin `6c579da5` = `master` `4a43bcdb` | Citations qui tombent juste |
|---|---|---|
| `Mockingboard.cpp` | dernier changement `3e8054b462` (2026-07-26) | Toutes **depuis `d5db8e84a6`** (2026-07-18, « AY: support empty socket »), sauf `193`, qui ne tombe juste sur **aucune** des 177 révisions |
| `SSI263.cpp` | `8193cc1fd1` (2026-07-05) | `613-617` et `852-869` depuis `508f667ea1` (2026-07-04) ; `200-209` depuis `552d830045` (2025-06-08) ; `806-844` depuis `287a0f892e` (2025-02-09) ; `930-956` depuis `fa6d2f7c12` (2025-02-22) ; `40-44` ne tombe juste sur **aucune** des 44 révisions |
| `6522.cpp` | `3e8054b462` | `39-61` sur 19 des 21 révisions depuis `f7c6ef397c` (2022-02-05) |
| `MockingboardCardManager.cpp` | `8193cc1fd1` | `388-407` depuis `508f667ea1` (2026-07-04) |

**Conclusion : une seule époque par dépôt.** Toutes les citations justes le
sont sur le pin et le restent jusqu'à `master`. Les cinq numéros faux ne
collent à aucune révision : ce sont des erreurs de saisie, pas des
citations vieillies. Seules deux citations MAME dépendent du choix entre pin
et `mame0287` : `a2mockingboard.cpp:585-607` et `filter.cpp:39-44`. Elles
sont justes sur le pin et pas sur `mame0287`. C'est donc le pin qu'il faut
écrire, pas le tag.

**Pin proposé sous forme de tableau** (modèle `DEV.md` § Zilog Z8530 SCC) :

| Amont | Révision | Pris | Pas pris (et pourquoi) |
|---|---|---|---|
| MAME `a2mockingboard.cpp` | `ed697c3087` (= pin `a2b6ba2d` = `master`) | Câblage VIA → AY (PB0 BC1, PB1 BDIR, PB2 /RESET), sélection Echo+ PB3/PB4 actives à l'état haut, routage stéréo, `$C0nX` en bus flottant, carte TMS5220C à `$C0n0` | Répartition PB3/PB4 et reset de la Phasor (POM2 suit AppleWin et le matériel, voir AMONT FAUX). Garde de plage des lectures et écritures Phasor (POM2 suit AppleWin). SC-01A (POM2 n'a pas de Votrax) |
| MAME `tms5220.cpp` | `d0f1c15a0f` | `process`, `lattice_filter`, `parse_frame`, `data_write`, `process_command`, RDBY retenu pendant la parole | VSM (l'Echo+ n'en a pas) |
| MAME `a2sam.cpp`, `dac.cpp/.h` | `f135a4e417`, `d0f1c15a0f`, `fe4cd123b9` | Loquet sur tout `$C0nX`, échelle R-2R 8 bits, route 0,5 | — |
| MAME `spkrdev.cpp` | `bdf56c6ae8` | ×4, 64 prises, convolution normalisée | Fenêtre de Blackman et pôle DC indépendant de la fréquence d'échantillonnage (choix de rendu, documentés en commentaire) |
| MAME `filter.cpp` | `bdf56c6ae8` | Passe-haut Butterworth à 20 Hz, Q = √2/2, un filtre par canal | — |
| AppleWin `Mockingboard.cpp`, `SSI263.cpp`, `6522.cpp`, `MockingboardCardManager.cpp` | `6c579da5bd94` (= `master` `4a43bcdb`) | Décodage Phasor sur toute la page, SSI263 sur A6/A5, reset des deux AY, sélection PB4/PB3, IRQ de parole, `SetCardMode`, reset du MB-C sans SSI263 | La règle « une fonction AY ne s'exécute que depuis INACTIVE » (GH#320) : voir le constat 1 et la piste « fonction sans INACTIVE » |

---

## Rapport 1 — Citations

Révision de référence : le pin `a2b6ba2d` pour MAME (égal à `master` ; écart
avec `mame0287` signalé) et `6c579da5` (= `master`) pour AppleWin.

| Fichier POM2:ligne | Citation | Verdict | Commentaire |
|---|---|---|---|
| `src/Ay3_8910.h:173` | `mockingboard.cpp:391-410` | **INEXACTE** | Le fichier est `a2mockingboard.cpp`. Les lignes 391-410 sont bien `via_psg_ctrl`, appelé à chaque écriture d'ORB. En revanche, la phrase suivante (« a driver holding BDIR through multiple PA changes legitimately re-strobes the same AY register with each new data byte ») n'est pas ce que fait MAME : une écriture de PA n'appelle que `via1_out_a` (`:436-439`), qui range l'octet dans `m_porta1` sans rien envoyer à l'AY. Le redéclenchement sur changement de PA n'existe que chez POM2 (`Mockingboard.cpp:1158-1161`). |
| `src/AyPsgSynth.h:393` | `a2mockingboard.cpp:128-132` | DÉCALÉE (aucune révision) | Ces lignes déclarent la classe `a2bus_echoplus_device`. AY1 sur le canal 0 et AY2 sur le canal 1 d'un haut-parleur stéréo : `:159-165` (pin et `mame0287`) ; `:157-163` de `28fafc0e` à `d0f1c15a`. |
| `src/SpeakerDevice.h:132` | `spkrdev.cpp:74` | DÉCALÉE (aucune révision) | `RATE_MULTIPLIER = 4` : `:85` sur le pin, `:81` sur `mame0287`, `:82` avant 2021. |
| `src/SpeakerDevice.cpp:310` | `spkrdev.cpp:319-322` | DÉCALÉE (aucune révision) | La convolution qui part de `m_composed_sample_index + 1`, la plus ancienne en tête, est `get_filtered_volume` : `:382-398`, boucle en `:389-394` sur le pin (`:388-393` sur `mame0287`). Les lignes 319-322 sont le début de `update_interm_samples_get_filtered_volume`. |
| `src/Mockingboard.h:33` | AppleWin `Mockingboard.cpp:193` | DÉCALÉE (aucune révision) | La ligne 193 est `isIRQ = true;` dans `Get6522IrqDescription`. Le câblage BDIR = b1, BC1 = b0, BC2 câblée à +5 V se trouve en `:350-353` (`AY8913_Write`), /RESET = b2 en `:253` (notes) et en `:293`/`:311`. `DEV.md:2552` reprend la même ligne fausse. |
| `src/Mockingboard.cpp:870` | AppleWin `SSI263.cpp:40-44` | DÉCALÉE (aucune révision) | « Mockingboard-C: CTRL+RESET is not connected to !PD/!RST pin 18 » est en **`:38`**. Les lignes 40-44 traitent du reset de la **Phasor** et de la mise sous tension. |
| `src/PhasorCard.cpp:131` | `filter.cpp:39-44` | OK (rev `bdf56c6ae8` = pin) | `reset_highpass_active`, actif par défaut. Sur `mame0287` : 37-42. Les 20 Hz sont en `:67`, Q = √2/2 depuis `9d16151558`. |
| `src/PhasorCard.cpp:139` | `a2mockingboard.cpp:192-208` | OK | Configuration Phasor : `ay2` passe sur `speaker2` canal 0, `ay3` sur `speaker` canal 1, `ay4` sur `speaker2` canal 1. Gauche = VIA1 (ay1 + ay2), droite = VIA2. |
| `src/AudioDevice.h:24` | `a2mockingboard.cpp:159-165` | OK | Plus `:192-208` sans préfixe : OK. |
| `src/Mockingboard.cpp:241` | `a2mockingboard.cpp:159-165` | OK | Plus `:186-189` sans préfixe : OK, avec une nuance. La puce de parole que MAME centre à 1,0 est un **Votrax SC-01A** (Mockingboard « Speech »), pas un SSI263. Que la sortie du SSI263 d'une Sound II soit mono et centrée relève de POM2, pas de MAME. |
| `src/Mockingboard.cpp:610` | `a2mockingboard.cpp:161-165` | OK | `AY8913` → `speaker` canal 0 et canal 1. |
| `src/Mockingboard.cpp:693` | `a2mockingboard.cpp:186-189` | OK | Même nuance (SC-01A). |
| `src/Mockingboard.cpp:1066` | `a2mockingboard.cpp:346-349` | OK | « Reads only select the VIA. » |
| `src/EchoPlusTMS5220Card.h:2` | `a2mockingboard.cpp:210-219` | OK | Plus `307-310`, `360-363`, `453-474` : OK ; `585-607` : OK sur le pin seul (582-604 sur `mame0287`). |
| `src/EchoPlusTMS5220Card.cpp:1` | `a2mockingboard.cpp:453-474` | OK | `via1_out_b` de l'Echo+ : PB2 remet les deux AY à zéro, PB3 et PB4 sélectionnent à l'état haut. Plus `585-607` : OK (pin). |
| `src/Tms5220.h:4`, `src/Tms5220Tables.h:4`, `src/Tms5220.cpp:4` | `tms5220.cpp:960-1370` | OK ×3 | De `process` à `lattice_filter` (le `return m_u[0]` est en 1372). `,1561-1662` sans préfixe : `parse_frame`, OK. |
| `src/Tms5220.cpp:78` | `tms5220.cpp:672-730` | OK | `data_write`. `,1393-1546` sans préfixe : `process_command`, OK. |
| `src/Tms5220.cpp:100` | `tms5220.cpp:1395-1415` | OK | RDBY retenu pendant TALK. `:794-807` sans préfixe : reprise de la commande retenue, OK. |
| `src/SamCard.cpp:25` | `a2sam.cpp:44` | OK | `write_c0nx` → `m_dac->write(data)`. |
| `src/SamCard.h:21` | `a2sam.cpp:44` | OK | Idem. |
| `src/SamCard.h:22` | `a2sam.cpp:55-56` | OK | `SPEAKER` + `DAC_8BIT_R2R … 0.5 // unknown DAC`. |
| `src/Dac8Audio.h:26` | `a2sam.cpp:56` | OK | Route 0,5. |
| `src/Dac8Audio.h:20` | `dac.h:237` | OK | `DAC_8BIT_R2R`, `dac_mapper_unsigned`, 8, `dac_gain_r2r`. |
| `src/Dac8Audio.h:22` | `dac.cpp:28-33` | OK | `code / 2^bits`. Plus `:77-78` (plage [-1, +1]) et `:113` (`min + v·(max-min)`) sans préfixe : OK. Le niveau POM2 `0,5·(-1 + 2c/256)` en découle exactement. |
| `src/PhasorCard.cpp:442` | AppleWin `Mockingboard.cpp:576-584` | OK | En mode Echo+, les tampons 2 et 3 sont copiés vers 0 et 1 (le haut-parleur droit est recopié à gauche). |
| `src/PhasorCard.cpp:763` | AppleWin `Mockingboard.cpp:683-692` | OK | CS de lecture par mode. `,780-790` : CS d'écriture, OK. |
| `src/PhasorCard.h:30` | AppleWin `Mockingboard.cpp:683-692` | OK | Plus `,780-790` : OK. |
| `src/PhasorCard.h:42` | AppleWin `Mockingboard.cpp:683-692` | OK | Décodage A7/A4 sur toute la page. |
| `src/PhasorCard.cpp:783` | AppleWin `Mockingboard.cpp:698-711` | OK | `CS_SSI263` en 703 ; principal en 708-709, secondaire en 710-711 (le secondaire gagne). |
| `src/PhasorCard.cpp:807` | AppleWin `Mockingboard.cpp:800-819` | OK | SSI263 en écriture sur A6/A5, en `:806-818`. |
| `src/PhasorCard.cpp:895` | AppleWin `Mockingboard.cpp:253` | OK | « ORB.b2 = AY /RESET … both AYs' /RESET pins ». Plus `,331-335` (GH#1197) : OK. |
| `src/PhasorCard.cpp:934` | AppleWin `Mockingboard.cpp:237-262` | OK | Notes GAL (GH#1192). Plus `,285-286` (`kAY1 = 2` sur b4, `kAY2 = 1` sur b3, « confirmed on real Phasor h/w ») : OK. |
| `src/PhasorCard.cpp:687` | AppleWin `SSI263.cpp:806-844` | OK | `SetSpeechIRQ` : CA1 en mode MB, IRQ CPU directe en natif. |
| `src/PhasorCard.cpp:863` | AppleWin `SSI263.cpp:852-869` | OK | `SetCardMode`. |
| `src/Ssi263.cpp:59` | AppleWin `SSI263.cpp:613-617` | OK | `DUR = 3` en mode trame, sinon `durPhon >> 6`. |
| `src/Ssi263.cpp:208` | AppleWin `SSI263.cpp:200-209` | OK | CTL H→L → `Play(m_durationPhoneme & PHONEME_MASK)`. |
| `src/Mockingboard.cpp:870` (2ᵉ plage) | AppleWin `SSI263.cpp:930-956` | OK | `Reset()` : une SSI263P sur MB-C/Phasor continue de parler. |
| `src/Via6522.h:139` | AppleWin `6522.cpp:39-61` | OK | `SY6522::Reset` : latch `$FFFF` à la mise sous tension seulement. Point déjà traité en tranche 11. |
| `src/Mockingboard.cpp:245` | AppleWin `MockingboardCardManager.cpp:388-407` | OK | « L = Address.b7=0, R = Address.b7=1 ». |
| *sans préfixe* `src/Tms5220.cpp:68` | `:1430-1448` | DÉCALÉE (aucune révision exacte) | Le SPEAK et sa remise à zéro de trame sont en **`:1474-1506`** (remise à zéro en 1484-1503). `:688-712`, sur la même ligne : OK. |

### Lignes corrigées (format `CLAUDE.md`)

```
// Ay3_8910.h:173
// [D] MAME a2mockingboard.cpp:391-410 @ a2b6ba2d — via_psg_ctrl, appelé à chaque
//     écriture d'ORB (6522via.cpp:847-853 @ 9edf3ab17d) ; une écriture de PA ne fait
//     que ranger m_porta1 (a2mockingboard.cpp:436-439).
// [D] AppleWin Mockingboard.cpp:366-437 @ 6c579da5 — une fonction ne s'exécute que
//     depuis l'état INACTIVE (GH#320, observation sur MB réelle).
// [F] Inconnu : ce que fait l'AY quand PA change sous LATCH ou WRITE maintenu. Le
//     redéclenchement sur changement de PA est propre à POM2.
// [B] R6522/W65C22 : une écriture d'ORB avec la même valeur ne change aucune broche
//     PB → l'AY ne peut pas la voir (constat 1).

// AyPsgSynth.h:393
// [D] MAME a2mockingboard.cpp:159-165 @ a2b6ba2d — AY1 → canal 0, AY2 → canal 1
//     d'un haut-parleur stéréo ; filter.cpp:173-182 — un filtre par canal.

// SpeakerDevice.h:132
// [D] MAME spkrdev.cpp:85 @ a2b6ba2d (:81 @ mame0287) — RATE_MULTIPLIER = 4 ;
//     spkrdev.h:39 — FILTER_LENGTH = 64.
// SpeakerDevice.cpp:310
// [D] MAME spkrdev.cpp:382-398 @ a2b6ba2d — get_filtered_volume, départ à
//     m_composed_sample_index + 1, normalisation par ampsum.

// Mockingboard.h:33 (et DEV.md:2552)
// [D] AppleWin Mockingboard.cpp:350-353 @ 6c579da5 — BDIR = b1, BC1 = b0, BC2 = +5 V ;
//     :253 — b2 = /RESET. MAME a2mockingboard.cpp:391-410, :441-451 @ a2b6ba2d.

// Mockingboard.cpp:870
// [D] AppleWin SSI263.cpp:38, :930-956 @ 6c579da5 — sur MB-C, CTRL+RESET n'est pas
//     relié à la broche 18 (!PD/!RST).

// Tms5220.cpp:68
// [D] MAME tms5220.cpp:1474-1506 @ a2b6ba2d (SPEAK), :688-712 (front SPEN de la FIFO).

// PhasorCard.cpp:131 (et DEV.md « filter.cpp:39-44 »)
// [D] MAME filter.cpp:39-44, :67 @ a2b6ba2d (:37-42, :65 @ mame0287) — passe-haut
//     actif par défaut, 20 Hz ; Q = √2/2 depuis 9d16151558.

// EchoPlusTMS5220Card.h:2 / .cpp:1
// [D] MAME a2mockingboard.cpp:210-219, :307-310, :360-363, :453-474, :585-607
//     @ a2b6ba2d (les deux dernières plages valent :582-604 @ mame0287).

// PhasorCard.cpp:895 — monte en grade
// [A] Phasor réelle sous mb-audit : /RESET (PB2) remet les DEUX AY d'un 6522 à zéro,
//     quels que soient le mode et les sélections (AppleWin GH#1197, case cochée ;
//     mesure brute non publiée dans l'issue).
// [D] AppleWin Mockingboard.cpp:253, :331-335 @ 6c579da5 ; MAME a2mockingboard.cpp:
//     474-483, :519-532 @ a2b6ba2d est faux (AMONT FAUX 1).
```

Les autres lignes OK restent au grade D telles quelles, en ajoutant
` @ a2b6ba2d` ou ` @ 6c579da5`. Exemple :
`// [D] AppleWin SSI263.cpp:852-869 @ 6c579da5 — SetCardMode republie D7.`

---

## Rapport 2 — Constats

### 1. Une écriture d'ORB de même valeur redéclenche l'AY, alors qu'aucune broche ne bouge

```
Fichier      : src/Via6522.h (case VIA_ORB : events |= 0x01 dès que DDRB ≠ 0, même
               valeur comprise) → src/Mockingboard.cpp:1155-1200 (onViaPortBChange →
               Ay3_8910::applyControl → Wrote → ayEnvWriteCount_++) ; même chemin dans
               PhasorCard.cpp:804 et EchoPlusTMS5220Card.cpp:171. Épinglé dans ce sens
               par tests/mockingboard_bus_edges_test.cpp:173-207
               (testHeldWriteRetriggersEnvelope) ; origine : docs/mockingboard_mame_audit.md,
               « Confirmed defects corrected », ligne 2 (2026-10-03)
Amont        : MAME 6522via.cpp:847-853 @ 9edf3ab17d + a2mockingboard.cpp:391-410
               @ a2b6ba2d (redéclenche) ; AppleWin Mockingboard.cpp:366-437 @ 6c579da5
               (« GH#320: functions only work from inactive state », ne redéclenche pas)
Gravité      : MINEUR
Confiance    : CONFIRMÉ par la lecture du code et par le test existant, qui exige
               précisément ce comportement. Aucune sonde lancée (interdit dans cette
               tranche). Aucun titre connu qui s'en serve.
```

**Le silicium (grade B, raisonnement sur les broches).** L'AY-3-8913 ne voit
de la carte que ses broches : BDIR, BC1 et /RESET sur PB0-PB2, DA0-DA7 sur
PA. Une écriture dans ORB d'un octet égal à son contenu ne fait changer
aucune broche PB. Le 6522 ne produit d'impulsion sur une écriture d'ORB
qu'en mode « handshake » de CB2, et CB2 n'est pas relié à l'AY (MAME
`a2mockingboard.cpp:184` le route vers le SC-01A seul). L'AY ne peut donc
pas distinguer deux suites dont les broches sont identiques. AppleWin le
modélise ainsi : une fonction AY ne s'exécute qu'en sortant de l'état
INACTIVE, d'après un rapport sur Mockingboard réelle (GH#320, Grouik /
French Touch, 2016, corrigé par `74ac2a4` avec des tests dans
AppleWin-Test).

**MAME.** `via6522_device::write` appelle `output_pb()` à chaque écriture
d'ORB quand DDRB ≠ 0. Le gestionnaire de port B de la carte relance alors
`via_psg_ctrl`. C'est un artefact logiciel : l'émulateur réagit à un
événement qui n'existe pas sur le bus.

**POM2.** Le 2026-10-03, le comportement de MAME a été adopté comme
« confirmed defect corrected ». Depuis, un second `STA ORB` de `$06`, avec
WRITE maintenu, réécrit R13 et **redémarre l'enveloppe**.

**Scénario.** Deux suites dont les broches de la carte sont identiques en
tout point :

```
A : PA=$0D, PB=$07, PB=$04, PA=$08, PB=$06 (t=0) ...................... PB=$04 (t=2000)
B : PA=$0D, PB=$07, PB=$04, PA=$08, PB=$06 (t=0), PB=$06 (t=1000), PB=$04 (t=2000)
```

Sur la carte réelle, A et B donnent la même sortie, puisque les broches ne
diffèrent à aucun instant. Sous POM2, B redémarre l'enveloppe (forme 8, dent
de scie descendante) à t=1000. Son niveau diffère donc de celui de A dès
t=1000. Le test `testHeldWriteRetriggersEnvelope` affirme exactement
l'inverse du silicium : il exige que B (`held`) sonne comme une écriture
pulsée `$04/$06`.

**Observation.** Inverser l'assertion finale de
`testHeldWriteRetriggersEnvelope` : la sortie de `held` doit égaler celle
d'une carte sur laquelle on n'a **pas** refait l'écriture d'ORB, et
`getAyWriteCount` ne doit pas avancer. Le même test vaut pour la Phasor et
l'Echo+, qui passent par le même événement `Via6522`. La correction
consiste à ne déclencher la fonction AY que sur un changement réel de
`readPortB()`, ce qui reste compatible avec les autres règles de
`DEV.md` § Mockingboard (lecture sur les broches, front de DDR).

**Doute résiduel.** Il ne porte pas sur la règle, mais sur ce que fait l'AY
quand **PA** change pendant que WRITE est maintenu. Voir la piste
« changement de PA sous fonction maintenue ».

### AMONT FAUX 1 — Sur la Phasor, MAME ne remet pas à zéro le bon AY (ni les deux)

```
Fichier      : src/PhasorCard.cpp:895-905 (PB2 bas → les deux AY du 6522, quel que soit
               le mode)
Amont        : MAME a2mockingboard.cpp:474-483 et :519-532 @ a2b6ba2d ; AppleWin
               Mockingboard.cpp:253, :331-335 @ 6c579da5 ; GH#1197
Gravité      : —
Confiance    : CONFIRMÉ dans les deux codes ; la règle matérielle est de grade A par
               procuration (mb-audit sur Phasor réelle, rapporté par AppleWin ; je n'ai
               pas vu la mesure brute)
```

En mode compatible Mockingboard, chez MAME :
- `via1_out_b` avec PB2 bas ne remet à zéro que `m_ay1` ;
- `via2_out_b` avec PB2 bas remet à zéro **`m_ay2`**. Or en compatible,
  `m_ay2` est piloté par **VIA1** (`:497-498`), alors que VIA2 pilote
  `m_ay3` (`:544-545`, `chip_sel = 1`).

Le pilote Mockingboard qui remet à zéro le canal droit par VIA2 laisse donc
**`m_ay3` intact** : sous MAME, le canal droit continue de jouer après le
reset. Il manque une remise à zéro du second AY du canal gauche, et il en
manque une autre à droite. AppleWin (GH#1197, « 6522's AY RESET always
resets both attached AYs, regardless of Phasor mode or chip-select bits »)
et POM2 remettent à zéro les deux AY du 6522 concerné.

**Scénario sous MAME.** Phasor en slot 4, mode MB, un morceau en cours sur
les deux canaux, puis la routine d'arrêt usuelle `PB=$00` / `PB=$04` sur
les deux VIA. MAME : le canal droit garde sa note. Matériel et POM2 :
silence.

Il ne faut pas « aligner » `PhasorCard::onViaPortBChange` sur MAME.

---

## Pistes examinées et écartées

**Saines** (conformes à l'amont, sans contradiction interne) :

- **Câblage VIA → AY** (PB0 BC1, PB1 BDIR, PB2 /RESET, PA bus) : MAME et
  AppleWin concordent ; POM2 lit les broches (`readPortA`/`readPortB`).
- **Echo+ (carte Street)** :
  - PB3 et PB4 sélectionnent à l'état haut, PB2 remet les deux AY à zéro
    quelle que soit la sélection. Avec les deux AY sélectionnés, le dernier
    gagne la lecture, comme chez MAME.
  - `$C0n0` vaut `0x1F | status`, les autres `$C0nX` valent 0, et le VIA
    est miroité sur toute la page `$Cn`.
  - Le TMS5220C n'est pas remis à zéro par le bus.
  Le tout est conforme à MAME `:307-310`, `:360-363`, `:453-474` et
  `:585-604`.
- **TMS5220C, chemin de commande** :
  - `0x60` vide la FIFO ; POM2 met `SPEN` à 0 explicitement, MAME y arrive
    par `update_fifo_status_and_ints` (FIFO vide et DDIS, l. 780-781). C'est
    équivalent.
  - Le front SPEN sur remplissage remet la trame à zéro.
  - En mode externe, `$70` est une donnée.
  - RDBY est retenu pendant TALK, puis repris.
  - `0x30` et `0x40` ne sont pas modélisés faute de VSM. C'est sans effet,
    sauf un écart sur `RDB_flag` après un `$10` suivi d'un `$50`. Le
    scénario suppose une VSM absente sur l'Echo+, et aucun logiciel connu
    ne le joue.
  - Le TMS tourne à 640 kHz/80 = 8 kHz, compté en temps émulé.
- **S.A.M.** : n'importe quelle écriture dans `$C0nX` charge le DAC ; une
  lecture rend le bus ouvert. Le niveau `0,5·(-1 + 2c/256)` est exactement
  MAME (`dac.cpp:28-33`, `:77-78`, `:113`, `a2sam.cpp:56`).
- **Haut-parleur** : ×4, 64 prises, départ de la convolution à l'index + 1,
  normalisation par la somme. La fenêtre de Blackman (MAME : rectangle,
  somme des valeurs absolues) et le pôle DC indépendant de la fréquence
  sont des choix de rendu documentés en commentaire, sans rapport avec le
  silicium.
- **`DcBlocker`** : coefficients de Zölzer, table 2.2, identiques à
  `filter.cpp` (passe-haut à 20 Hz, Q = 0,7071067), un filtre par canal
  (`apply`, l. 173-182).
- **Routage stéréo** : Mockingboard, AY1 à gauche et AY2 à droite ;
  Phasor, paire VIA1 à gauche et paire VIA2 à droite ; Echo+ sur Phasor,
  paire VIA2 recopiée des deux côtés. MAME, AppleWin et POM2 concordent.
- **SSI263 sur la Phasor** :
  - écritures sur A6 (principal) et A5 (secondaire) sur toute la page ;
  - lectures natives seulement si A7 = A4 = 0, D7 seul piloté, le
    secondaire l'emporte en `$Cn6x` ;
  - IRQ par CA1 en mode MB (front négatif seulement si `PCR.0 = 0`), IRQ
    directe en natif ;
  - republication de la demande lors du passage en mode MB.
  Tout est conforme à AppleWin `:698-713`, `:800-819`, `SSI263.cpp:806-869`.
- **Durée d'un phonème** : `DUR = 3` en mode trame, conforme ;
  `playbackDuration` reçoit le mode verrouillé (`aMode_`), comme AppleWin
  reçoit `m_currentMode`.
- **Reset à chaud du SSI263 sur Sound II** : non relié, donc la parole
  continue. **VOULU** (`DEV.md` § SSI263, « Sound II warm reset »). Sur la
  Phasor (SSI263AP), le reset à chaud remet la puce à zéro, conformément à
  AppleWin `:937-954` pour un type autre que P. La bizarrerie de la SSI263P
  n'est pas modélisée, ce qui est documenté.
- **6522, RES** : traité par la tranche 11 (`parite-6522via`), VOULU. Au
  passage, AppleWin conserve ORA et ORB lors d'un Ctrl-RESET (son `memset`
  n'a lieu qu'à la mise sous tension), contre la fiche Rockwell, qui les
  efface. Cela n'a pas de conséquence observable chez AppleWin, qui ne
  déclenche l'AY que sur une écriture d'ORB, pas sur une écriture de DDRB.
  Ce n'est donc pas un AMONT FAUX.

**Divergences entre lectures, non tranchées faute de silicium** (POM2 est
de grade F sur chacune) :

- **Changement de PA sous fonction maintenue.** POM2 redéclenche l'AY quand
  PA change pendant que LATCH ou WRITE est tenu (`Mockingboard.cpp:1158`,
  « so the order doesn't matter »). MAME ne le fait pas (`via1_out_a` range
  l'octet sans rien envoyer), AppleWin non plus (les fonctions ne partent
  que d'une écriture d'ORB, et seulement depuis INACTIVE). La seule
  observation sur silicium est GH#320 : la suite `PA=reg, PB=$07, PA=val,
  PB=$06`, sans INACTIVE, donne « no sound at all » sur une vraie MB. Les
  trois modèles prédisent :
  - AppleWin : silence ;
  - POM2 : réaccroche l'adresse sur `val`, puis écrit `val` dans le
    registre `val` si `val ≤ $0F`. Le plus souvent silence, avec quelques
    registres bas pollués ;
  - MAME : **joue la musique**, ce qui contredit l'observation.
  
  On ne peut donc pas dire que POM2 a tort, mais MAME est faux sur ce
  point. Une sonde le trancherait en partie : jouer la suite de GH#320
  sous POM2 et mesurer le RMS de sortie. Seule une capture sur MB réelle
  trancherait le modèle de l'AY lui-même.
- **Garde de plage des accès VIA de la Phasor.** MAME limite les lectures
  et écritures VIA à `offset < $20` ou `$80-$9F` (`:326`, `:367`). AppleWin
  et POM2 décodent A7/A4 sur toute la page. Aucune des deux lectures ne
  cite de mesure ; POM2 suit AppleWin, ce qui est documenté
  (`PhasorCard.h:42`).
- **Affectation PB3/PB4 de la Phasor en natif.** MAME : `~PB3` → `ay1`.
  AppleWin : `~PB4` → AY1 (« confirmed on real Phasor h/w » pour le mode
  MB), plus l'asymétrie du GAL (GH#1192, « AFAICT »). Sans effet audible
  chez MAME, puisque les deux AY d'une paire sont du même côté. POM2 suit
  AppleWin.
- **Niveau et position de la parole** : SC-01A centré à 1,0 chez MAME
  (Mockingboard Speech) ; le SSI263 d'une Sound II est centré par POM2 par
  analogie, et ceux de la Phasor sommés à 0,5. Aucune mesure de la sortie
  analogique d'une MB-C.

**Hors périmètre ou documentaire** :

- `DEV.md:2552` reprend la citation fausse `Mockingboard.cpp:193`, et
  `DEV.md` § Mockingboard « filter.cpp:39-44 » n'est juste que sur le pin.
- `SpeakerDevice.h:133` cite « `spkrdev_h.txt:28` » : il n'existe pas de
  fichier de ce nom. `FILTER_LENGTH = 64` est en `spkrdev.h:39 @ a2b6ba2d`.
- La « bizarrerie » de la Phasor en mode MB/E+ (« both E+/MB play tones at
  half frequency », GH#1197) touche l'horloge des AY de la Phasor, qui
  n'est citée nulle part dans cette tranche. Je ne l'ai pas examinée.

---

## Bilan

- **Citations examinées** : 40 sur 40, plus 17 numéros sans préfixe.
  Chacune a été datée sur l'historique complet du fichier amont : 226
  révisions MAME et 261 révisions AppleWin.
- **Verdicts (40)** :
  - OK : 33 ;
  - OK (rev pin, pas `mame0287`) : 1 (`filter.cpp:39-44`) ;
  - DÉCALÉE : 5, toutes sans aucune révision qui tombe juste
    (`AyPsgSynth.h:393`, `SpeakerDevice.h:132`, `SpeakerDevice.cpp:310`,
    `Mockingboard.h:33`, `Mockingboard.cpp:870`) ;
  - INEXACTE : 1 (`Ay3_8910.h:173` : mauvais nom de fichier, et un
    redéclenchement sur changement de PA que MAME ne fait pas) ;
  - ABSENTE : 0 ; NON-PINNÉE : 0.

  Sans préfixe : 16 OK (dont `585-607` sur le pin seul) et 1 DÉCALÉE
  (`:1430-1448` → `1474-1506`).
- **Constats** : CRITIQUE 0 · MAJEUR 0 · **MINEUR 1**. Une écriture d'ORB
  de même valeur redéclenche l'AY (R13), alors que les broches sont
  inchangées. C'est un alignement sur MAME (grade D) du 2026-10-03,
  contredit par la logique des broches (B) et par le modèle d'AppleWin
  (GH#320). Le test `mockingboard_bus_edges` l'épingle à l'envers.
- **AMONT FAUX** : 1. MAME remet à zéro le mauvais AY de la Phasor en mode
  compatible : `via2` touche `m_ay2` au lieu de `m_ay3`. Il ignore aussi la
  règle « les deux AY », que POM2 suit, d'après AppleWin GH#1197.
- **Révisions** :
  - MAME : pin `a2b6ba2d` (= `master` 2026-11-01). Contenu
    `ed697c3087` pour `a2mockingboard.cpp` et `bdf56c6ae8` pour
    `spkrdev`/`filter`. Pour `tms5220`, `a2sam` et `dac`, le contenu est le
    même sur `mame0287` et sur le pin.
  - AppleWin : `6c579da5` (= `master` `4a43bcdb`).
  - Les deux pins sont à jour. Une seule époque de citations par dépôt.
- **Non vérifié** :
  - le constat 1 n'a **pas de sonde** ; il repose sur la lecture du code
    et sur l'assertion du test existant. La règle est déduite des broches
    et du 6522, pas mesurée : aucune capture d'une écriture d'ORB de même
    valeur sur une vraie Mockingboard n'a été trouvée ;
  - la mesure brute derrière GH#1197 (reset des deux AY) et derrière
    « confirmed on real Phasor h/w » (PB4 → AY1) : seul le résumé
    d'AppleWin a été lu ;
  - le comportement réel de l'AY quand PA change sous LATCH ou WRITE
    maintenu (piste ci-dessus) ;
  - les fiches techniques SSI263A, TMS5220C et de l'AY sur ce point : je
    ne les ai pas téléchargées. Les verdicts sur le SSI263 et le TMS5220
    restent au grade D (AppleWin, MAME) ;
  - le type de DAC du S.A.M. (MAME : « unknown DAC ») et le couplage
    alternatif de sa sortie, que `Dac8Audio.h` affirme sans source
    (grade F) ;
  - le niveau et la position de la parole sur une MB-C ou une Phasor
    réelle.
