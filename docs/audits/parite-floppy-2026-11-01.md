# Audit de parité — citations de MAME `floppy.cpp` (2026-11-01)

Tranche du brief `docs/audits/BRIEF-parite-amont.md`, menée **sans agent
payant** (abonnement). 24 citations `floppy.cpp:<lignes>` sous `src/`, dans
`DiskImage.cpp`, `DiskIICard.{h,cpp}`, `IWMDevice.cpp` et `Sony35Drive.{h,cpp}`.
Le fichier amont est `src/devices/imagedev/floppy.cpp`.

Aucun fichier de code n'a été modifié.

## Révision amont

**Aucun pin**, ni dans les en-têtes POM2 ni dans `DEV.md`.

Datation sur **190 révisions** (toute l'histoire du fichier, téléchargée), une
ancre regex par citation — la méthode de la tranche 2.

| Révision la plus récente où la citation tombe juste | Citations |
|---|---|
| `6272d014` (2026-11-01, `master`) | `Sony35Drive.cpp:697` (`:290`), `IWMDevice.cpp:264` (`:2711-2719`) |
| `5dd18911` (2026-10-22) | `IWMDevice.cpp:643` (`:1261-1279`) |
| `b523fac5` (2026-10-08) | `Sony35Drive.cpp:456` (`:1345`) |
| `bc9aec5a` (2026-10-03) | `DiskIICard.cpp:1543` (`:809-839`) |
| `7870bf4b` (2026-08-28) | `Sony35Drive.cpp:453` (`:1263-1264`), `:455` (`:1273-1274`) |
| `c2d379e4` / `ac2ac0e7` (2026-08-10) | `Sony35Drive.cpp:698` (`:560`), `:721` (`:672-673`), `DiskIICard.cpp:1215` (`:1176-1215`) |
| `7e46505c` (2026-07-18) | `Sony35Drive.cpp:722` (`:723`) |
| **`63a40168` (2023-07-24)** | `DiskIICard.cpp:1466` (`:817-820`), `DiskImage.cpp:1873` (`:293`) |

**Au moins huit époques, de juillet 2023 à aujourd'hui.** Deux citations sont
ancrées en **2023**, ce qu'aucune tranche précédente n'avait vu. Et aucune ne
tombe juste sur le tag `mame0287` : à cette révision `:723` est une ligne
blanche, `:1345` un `//`, et `:2711-2719` le constructeur de `teac_fd_55f`.
Le verdict est donc rendu contre **`master` = `6272d014`**, faute de repère
commun.

Les positions réelles à `mame0287`, pour mémoire : `m_dir(0)` 268,
`m_dskchg = exists` 539, `m_dskchg = 1` 647, `m_dskchg = 0` 697,
`if(!m_image || m_mon)` 1129/1163, `time_next_index` 817. `m_write_cyl` et
`write_do_flush` **n'y existent pas** : ils arrivent avec `c690a9ed`
(2026-08-16, « batch sequential flux writes »), déjà porté.

## Rapport 2 — Constats

### 1. Le disque se téléporte à la position angulaire zéro à chaque démarrage moteur

```
Fichier      : src/DiskIICard.cpp:1186-1191 (lssStart : « anchor its
               revolution_start_time to "now" so the disk's angular position
               reads as zero immediately after spin-up »)
               src/DiskIICard.h:672-690 (le contrat, per-drive)
Amont        : floppy.cpp @ bc9aec5a (2026-10-03), « Resume the disk at its
               stopped angular position when the motor restarts » — ajoute
               m_stopped_position, sauvegardé, remis à 0 au device_reset et au
               chargement d'image.
Gravité      : MINEUR
Confiance    : divergence CONFIRMÉE (lue dans les deux sources) ; portée
               PLAUSIBLE — aucun logiciel identifié qui en dépende.
```

**Divergence.** Jusqu'au 2026-10-03 l'amont re-ancrait la révolution sur
l'instant du démarrage moteur, et POM2 l'a porté fidèlement — le commentaire
de `lssStart` le dit mot pour mot. Depuis `bc9aec5a`, l'amont mémorise la
position angulaire à l'arrêt (`m_stopped_position`) et y reprend.

**Silicium.** Un plateau qui s'arrête ne revient pas à zéro : il repart d'où il
s'est arrêté. C'est le sens même du commit amont, et il n'a pas besoin d'être
arbitré — POM2 synchronise le disque sur le moteur, ce qu'aucune mécanique ne
fait.

**Scénario.** Un programme coupe le moteur (`$C0E8`), attend, le relance
(`$C0E9`) et lit le premier champ d'adresse qui passe. Sur le matériel, le
secteur rencontré dépend du temps écoulé ; dans POM2 c'est **toujours** celui
de la position zéro. Deux conséquences : POM2 est déterministe là où le
matériel ne l'est pas, et une protection qui mesure la rotation à travers un
arrêt moteur voit une valeur impossible. Je n'ai pas identifié de titre du
corpus qui le fasse — d'où la gravité MINEURE et la portée PLAUSIBLE.

**Correction suggérée (non appliquée).** Mémoriser
`(lssCycle - revolutionStartLssCycle[d]) mod period` à l'arrêt du moteur, et
re-ancrer `revolutionStartLssCycle[d] = lssCycle - stoppedPosition` au
démarrage. **Pas appliqué délibérément** : c'est la partie la plus sensible du
chronométrage de POM2 (le corpus DIX, `diskii_lss_smoke`, le boot DOS 3.3 et
l'autosave en dépendent), la portée réelle est inconnue, et le gain est de la
fidélité théorique. À faire avec un titre qui le démontre, ou pas du tout.

## Piste ouverte, non instruite

`b523fac5` (2026-10-08) — « Hold the write protect sensor covered and suppress
flux for a configurable time during disk changes » : pendant un changement de
disquette, le capteur de protection d'écriture reste couvert et le flux est
supprimé pendant un temps configurable. POM2 bascule le médium d'un coup
(`MediaMount.h`, `notifyMediaChange`). Non instruit faute de temps ; c'est le
premier endroit à regarder si quelqu'un reprend cette tranche.

## Bilan

- **Citations examinées** : 24 sur 24, datées sur les 190 révisions du fichier.
- **Verdicts** : toutes **DÉCALÉE** contre `mame0287` (aucune n'y tombe juste),
  et **OK (rev X)** contre leur propre époque — voir le tableau. Aucune
  INEXACTE : ce que les commentaires affirment est exact, c'est leur adresse
  qui ne l'est pas.
- **Constats** : CRITIQUE 0 · MAJEUR 0 · MINEUR 1 (position angulaire) ·
  AMONT FAUX 0.
- **Révision de référence** : `master` = `6272d014` (2026-11-01), faute de pin
  et faute de repère commun à `mame0287`.
- **Non vérifié** :
  - `b523fac5` (capteur WPT pendant un changement de disquette), ci-dessus ;
  - les écritures de flux par lots (`c690a9ed`) au-delà des citations, déjà
    portées mais non re-vérifiées ligne à ligne ;
  - aucune sonde exécutée dans cette tranche : les deux constats sont lus,
    pas mesurés. C'est en-dessous du standard des tranches 1 à 8.
