# Audit de parité — citations de MAME `iwm.cpp` (2026-11-01)

Tranche : toutes les citations `iwm.cpp:<lignes>` sous `src/` (`*.cpp`, `*.h`),
obtenues par

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep 'iwm\.cpp:'
```

55 citations, dans `IWMDevice.{h,cpp}`, `DiskIICard.{h,cpp}`,
`Sony35Drive.{h,cpp}`, `SmartPortHub.h` et `EmulationController.cpp`. Aucun
fichier de code n'a été modifié. La seule sonde écrite vit hors du dépôt
(`/tmp`) ; son source et sa sortie figurent plus bas.

Arbitre : MAME, AppleWin et les fiches techniques font foi, mais c'est **le
silicium** qui décide. Une divergence avec MAME qui colle au matériel réel
n'est pas un bug.

## Révision amont

**Aucune révision n'est épinglée.** Ni l'en-tête de `IWMDevice.{h,cpp}` ni
DEV.md ne donnent de commit, de tag ou de date pour `iwm.cpp`.
`IWMDevice.cpp:19-20` renvoie à « `/tmp/iwm.txt` in this checkout (a snapshot
fetched via scrapling) », un fichier qui n'est pas dans le dépôt. Le défaut se
voit en pratique : les numéros de ligne cités mélangent au moins trois
révisions amont, et certains ne collent à aucune.

| Révision | Date | Ce qu'elle contient | Citations POM2 qui tombent juste |
|---|---|---|---|
| `1393404c` = **tag `mame0287`** | 2023-03 | `flush_write` en lot, `read()` = `control()` | aucune |
| `2b2dd63f` | 2026-05-12 | `read()` gardé par `side_effects_disabled()` | `59-81`, `83-97`, `99-115`, `78-79`, `129`, `311-318`, `363-366` |
| `c690a9ed` = `master` | 2026-08-16 | `flush_write` remplacé par `write_start`/`write_flux_change` | `303-329`, `342`, `303-313`, à une ligne près `335-343` |
| aucune | — | — | `sync 335-480`, `half_window_size 290-301`, `control 144-254`, `mode_w 256-269`, … |

Le code POM2 est lui aussi un assemblage : la mise en tampon de
`flush_write` vient de `mame0287`/`2b2dd63`, l'appel `write_start` de
`c690a9ed`.

Conformément à la consigne, **le verdict est rendu contre `mame0287`**
(`https://raw.githubusercontent.com/mamedev/mame/mame0287/src/devices/machine/iwm.cpp`,
536 lignes, téléchargé). `2b2dd63f`, `c690a9ed` et quatre révisions de 2021-2022
ont été récupérées pour situer les numéros, ainsi que `iwm.h` et
`applefdintf.cpp` à `mame0287`.

Repères dans `iwm.cpp@mame0287` utilisés ci-dessous : `device_reset` 61-83 ·
`update_timer_tick` 85-99 · `set_floppy` 101-117 · `read` 124-127 · `write`
129-132 · `flush_write` 134-163 · `control` 165-286 (phases 169-174, moteur
allumé 182-188, entrée en lecture 190-202, entrée en écriture 204-214, moteur
coupé 215-233, devsel 235-239, valeurs lues et dispatch 279-283) · `mode_w`
289-302 · `data_w` 304-311 · `half_window_size` 328-340 · `window_size`
342-354 · `read_register_update_delay` 356-359 · `write_clock_start` 361-369 ·
`write_clock_stop` 371-378 · `sync` 380-536. Les énumérations et les
registres sont dans **`iwm.h`** (44-48, 51-59, 64-73), pas dans `iwm.cpp`.

---

## Rapport 1 — Citations

Trié par gravité. « (= 2b2dd63) » ou « (= master) » signale une citation
exacte dans cette révision-là.

| Fichier POM2:ligne | Citation | Verdict | Commentaire |
|---|---|---|---|
| `src/DiskIICard.h:525` | `iwm.cpp:248-253` | INEXACTE | Le texte dit que le mode s'écrit « par `STA $C0EF` quand Q6 est déjà haut » ; l'amont (`mode_w` 289-302, dispatch 283) exige aussi `m_active == MODE_IDLE`, sans quoi l'octet va à `data_w`. C'est l'origine du constat 1. |
| `src/DiskIICard.cpp:2035` | `iwm.cpp:248-253` | INEXACTE | Même affirmation au-dessus du crochet qui l'implémente : condition `!m_active` omise (amont 283). Constat 1. |
| `src/IWMDevice.cpp:548` | `iwm.cpp:311-318` | INEXACTE | `data_w` est en 304-311 (= 2b2dd63). Le commentaire affirme aussi que l'octet est « visible via une lecture $C0nF une fois revenu en MODE_IDLE », or l'amont renvoie `$FF` pour Q6=Q7=1 et pour la donnée au repos (280, 283). Le code POM2 fait comme l'amont, seul le commentaire est faux. |
| `src/IWMDevice.cpp:240` | `iwm.cpp:103-114` | ABSENTE | La garde `side_effects_disabled()` décrite n'existe pas dans `mame0287`, où `read()` (124-127) appelle `control()` sans condition ; elle arrive avec `2b2dd63` (122-135). Ne pas l'appliquer est une divergence documentée (`IWMDevice.cpp:46-49`). |
| `src/IWMDevice.cpp:641` | `iwm.cpp:342` | ABSENTE | `m_floppy->write_start(...)` n'existe pas dans `mame0287` (`write_clock_start` 361-369 ne fait que noter `m_flux_write_start`) ; c'est exact sur `master` (342-343). |
| `src/Sony35Drive.cpp:451` | `iwm.cpp:335-343` | ABSENTE | Même appel `write_start` depuis `write_clock_start` : absent de `mame0287`, présent sur `master` en 336-344. |
| `src/Sony35Drive.h:430` | `iwm.cpp:335-343` | ABSENTE | Idem. |
| `src/DiskIICard.cpp:1898` | `iwm.cpp:107-109` | DÉCALÉE | Lecture du statut (cas 0x40) en 281. |
| `src/DiskIICard.cpp:1901` | `iwm.cpp:267` | DÉCALÉE | Statut bits 0-4 = mode : 292. |
| `src/DiskIICard.cpp:1902` | `iwm.cpp:157-159` | DÉCALÉE | Bit 5 du statut mis à 1 en 185, remis à 0 en 223 et 96. |
| `src/DiskIICard.cpp:1915` | `iwm.cpp:107-110` | DÉCALÉE | Lecture de `m_whd` (cas 0x80) en 282. |
| `src/DiskIICard.h:521` | `iwm.cpp:36-46` | DÉCALÉE | Les registres sont déclarés dans `iwm.h:70` ; 36-46 sont des `save_item`. |
| `src/DiskIICard.h:529` | `iwm.cpp:110` | DÉCALÉE | `m_whd` lu en 282. |
| `src/DiskIICard.h:531` | `iwm.cpp:183` | DÉCALÉE | `m_whd \|= 0x40` en 208 ; remis à 0 en 224 et 97. |
| `src/DiskIICard.h:532` | `iwm.cpp:57` | DÉCALÉE | `m_whd = 0xbf` en 71. |
| `src/DiskIICard.h:560` | `iwm.cpp:111` | DÉCALÉE | `update_phases()` en fin de `set_floppy` : 116. |
| `src/DiskIICard.h:819` | `iwm.cpp:126` | DÉCALÉE | `(m_status & 0x7f) \| wpt` en 281. |
| `src/EmulationController.cpp:1907` | `iwm.cpp:70-84` | DÉCALÉE | `update_timer_tick` en 85-99. |
| `src/IWMDevice.cpp:26` | `iwm.cpp:83-97` | DÉCALÉE | `update_timer_tick` en 85-99 (= 2b2dd63). |
| `src/IWMDevice.cpp:79` | `iwm.cpp:204-206` | DÉCALÉE | `m_timer->adjust(cycles_to_time(8388608))` en 230. |
| `src/IWMDevice.cpp:122` | `iwm.cpp:59-81` | DÉCALÉE | `device_reset` en 61-83 (= 2b2dd63). |
| `src/IWMDevice.cpp:143` | `iwm.cpp:78-79` | DÉCALÉE | `m_devsel = 0; m_devsel_cb(0);` en 80-81 (= 2b2dd63). |
| `src/IWMDevice.cpp:177` | `iwm.cpp:99-115` | DÉCALÉE | `set_floppy` en 101-117 (= 2b2dd63). |
| `src/IWMDevice.cpp:231` | `iwm.cpp:90-105` | DÉCALÉE | `update_phases()` de `set_floppy` en 116. |
| `src/IWMDevice.cpp:247` | `iwm.cpp:107` | DÉCALÉE | `wpt_r()` en 281. |
| `src/IWMDevice.cpp:252` | `iwm.cpp:129` | DÉCALÉE | Expression du statut en 281 (= 2b2dd63). |
| `src/IWMDevice.cpp:291` | `iwm.cpp:115-118` | DÉCALÉE | `write` en 129-132. |
| `src/IWMDevice.cpp:297` | `iwm.cpp:119-143` | DÉCALÉE | `flush_write` en 134-163. |
| `src/IWMDevice.cpp:338` | `iwm.cpp:478-496` | DÉCALÉE | Aller-retour SW_WINDOW_MIDDLE→END en 501-526 ; l'affirmation « une période de bit = 2 × `half_window_size` » est juste, chargement asynchrone compris (7 + (half − 7)). |
| `src/IWMDevice.cpp:362` | `iwm.cpp:144-254` | DÉCALÉE | `control` en 165-286. |
| `src/IWMDevice.cpp:381` | `iwm.cpp:147-152` | DÉCALÉE | Mise à jour des phases en 169-174. |
| `src/IWMDevice.cpp:424` | `iwm.cpp:194-195` | DÉCALÉE | `mon_w(false)` en 186-187. |
| `src/IWMDevice.cpp:454` | `iwm.cpp:218-221` | DÉCALÉE | `set_write_splice(...)` en 211-212. |
| `src/IWMDevice.cpp:490` | `iwm.cpp:235-239` | DÉCALÉE | Armement du délai + `m_devsel_cb` en 227-231 ; 235-239 est le bloc devsel en régime établi. |
| `src/IWMDevice.cpp:541` | `iwm.cpp:256-269` | DÉCALÉE | `mode_w` en 289-302. |
| `src/IWMDevice.cpp:580` | `iwm.cpp:302-313` | DÉCALÉE | `window_size` en 342-354. |
| `src/IWMDevice.cpp:584` | `iwm.cpp:290-301` | DÉCALÉE | `half_window_size` en 328-340 ; table identique. |
| `src/IWMDevice.cpp:604` | `iwm.cpp:302-313` | DÉCALÉE | `window_size` en 342-354 ; table identique. |
| `src/IWMDevice.cpp:612` | `iwm.cpp:326` | DÉCALÉE | La ligne 0x18 = 16 est en 351 ; valeur juste. |
| `src/IWMDevice.cpp:624` | `iwm.cpp:363-366` | DÉCALÉE | `read_register_update_delay` en 356-359 (= 2b2dd63). |
| `src/IWMDevice.cpp:633` | `iwm.cpp:318-326` | DÉCALÉE | `write_clock_start` en 361-369. |
| `src/IWMDevice.cpp:651` | `iwm.cpp:327-334` | DÉCALÉE | `write_clock_stop` en 371-378. |
| `src/IWMDevice.cpp:716` | `iwm.cpp:335-480` | DÉCALÉE | `sync` en 380-536. |
| `src/IWMDevice.cpp:722` | `iwm.cpp:83-97` | DÉCALÉE | `update_timer_tick` en 85-99 (= 2b2dd63). |
| `src/IWMDevice.h:87` | `iwm.cpp:48` | DÉCALÉE | `device_reset` en 61-83. |
| `src/IWMDevice.h:100` | `iwm.cpp:85` | DÉCALÉE | `set_floppy` en 101-117 (85 est `update_timer_tick`). |
| `src/IWMDevice.h:125` | `iwm.cpp:335` | DÉCALÉE | `sync` en 380. |
| `src/IWMDevice.h:227` | `iwm.cpp:303-329` | DÉCALÉE | `half_window_size` + `window_size` en 328-354 (= master). |
| `src/IWMDevice.h:236` | `iwm.cpp:31-35` | DÉCALÉE | Énumération MODE_* dans `iwm.h:44-48`, pas dans `iwm.cpp`. |
| `src/IWMDevice.h:244` | `iwm.cpp:38-46` | DÉCALÉE | Énumération S_*/SR_*/SW_* dans `iwm.h:51-59`. |
| `src/IWMDevice.h:292` | `iwm.cpp:202-206` | DÉCALÉE | Armement du délai en 227-231 ; la description de `update_timer_tick` est juste. |
| `src/IWMDevice.h:316` | `iwm.cpp:55-65` | DÉCALÉE | Registres déclarés dans `iwm.h:70-72` ; valeurs de reset en 70-80. |
| `src/SmartPortHub.h:87` | `iwm.cpp:99-115` | DÉCALÉE | `set_floppy` en 101-117 (= 2b2dd63) ; le comportement décrit (le moteur suit la sélection) est juste. |
| `src/Sony35Drive.cpp:509` | `iwm.cpp:303-313` | DÉCALÉE | `half_window_size` en 328-340 (≈ master 303-315). |
| `src/Sony35Drive.h:422` | `iwm.cpp:303-313` | DÉCALÉE | Idem. |

Hors du grep, des renvois sans nom de fichier (« MAME line 57 », « line
286 », « line 303 », « line 317 », « line 331 », « (line 259) », « :157-159 »)
souffrent du même mélange de révisions. Ils ne sont pas comptés ici.

---

## Rapport 2 — Constats

### 1. Sur un //c, une écriture 5.25" DOS 3.3 ou ProDOS écrase le registre de mode de l'ombre IWM et supprime le délai d'arrêt moteur

```
Fichier      : src/DiskIICard.cpp:2035-2041 (chemin LSS bit à bit) et
               src/DiskIICard.cpp:2054-2058 (chemin à porte héritée) ;
               conséquences dans src/DiskIICard.h:510 (iwmTimerOff),
               src/DiskIICard.cpp:1634 et :1797 (stopSpindle), src/DiskIICard.h:827 (iwmStatus)
Amont        : iwm.cpp:283 (dispatch), 289-292 (mode_w), 218-226 (mode bit 2) @ mame0287 (1393404c)
Gravité      : MAJEUR
Confiance    : CONFIRMÉ (lu dans les deux sources + sonde exécutée)
```

**Divergence.** Dans l'amont, un accès à une adresse impaire avec Q6=Q7=1 va
au registre de mode **seulement si l'IWM est au repos** :
`if(offset & 1) { if(m_active) data_w(data); else mode_w(data); }` (283).
`m_active` est non nul pendant MODE_ACTIVE **et** pendant les 2²³ ticks de
MODE_DELAY. Moteur allumé, l'octet est une **donnée à écrire**. Le
`IWMDevice` de POM2 respecte cette règle (`IWMDevice.cpp:533-535`). L'ombre IWM
de `DiskIICard`, qui répond à `$C0nE` et `$C0nF` sur toute machine IWM
(`setIwmHost(true)` : //c et //c+), ne la respecte pas :

```
if (iwmHost_ && low4 == 0xF && wasQ6) {
    iwmMode = v;
}
```

Elle vérifie Q6 mais pas l'état du moteur. Depuis le 2026-10-31, `iwmMode`
pilote aussi l'arrêt du moteur : `iwmTimerOff()` = `iwmMode & 0x04` fait
appeler `stopSpindle()` dès `$C0n8`, comme la branche « mode bit 2 » de
l'amont (218-226). Les bits 0-4 du mode sont également renvoyés dans le statut
(`iwmStatus()`).

**Scénario.** Profil `Apple //c` (ou `//c PAL`, ou le chemin 5.25" du
`//c+`), disquette 5.25" DOS 3.3 ou ProDOS, en écriture :

1. Le pilote ouvre l'écriture selon l'idiome Disk II : `LDA $C08D,X`
   (Q6 haut), `LDA $C08E,X` (Q7 bas, test de la protection), puis
   `STA $C08F,X` avec le premier octet de synchronisation `$FF`, moteur
   allumé. Le dépôt nomme lui-même cet idiome (`DiskIICard.h:850`,
   `DEV.md:6674`, `LironCard.cpp:243-246`). Les adresses exactes de RWTS n'ont
   pas été désassemblées pendant cet audit ; la sonde rejoue la séquence
   d'accès.
2. Sur un vrai IWM, et dans `IWMDevice`, ce `$FF` va à `data_w`. Dans
   `DiskIICard`, il devient `iwmMode = $FF`.
3. Le statut lu ensuite à `$C0nE` (Q6 haut) passe de `$A0` à `$BF`. Le mode
   « relu » vaut `$1F` alors que personne n'a écrit de mode.
4. Au `$C0E8` de fin d'appel RWTS, le moteur s'arrête **immédiatement** au
   lieu de tourner encore 1,17 s (8 388 608 ticks à 7,16 MHz). Chaque appel
   RWTS suivant trouve donc le disque arrêté et paie l'attente complète de
   démarrage, ce que `DiskIICard.cpp:1785-1793` chiffre déjà : « ~115 s de
   temps machine pour booter DOS 3.3 » quand l'arrêt était instantané. Le son
   d'arrêt et de redémarrage moteur accompagne chaque appel.
5. Rien ne remet `iwmMode` à zéro, sauf une nouvelle écriture Q6-haut sur
   `$C0nF` ou un chargement d'instantané : aucune affectation dans un chemin
   de reset (vérifié par `rg 'iwmMode'`). L'état survit donc à tout ce qui
   suit la première écriture disque. Je n'ai pas vérifié si la broche /RESET de
   l'IWM du //c est câblée au RESET système ; le seul point sûr est que
   `device_reset` de MAME remet `m_mode` à 0 (72).

Sur le //c+, les deux contrôleurs que POM2 fait tourner en parallèle se
contredisent alors : `IWMDevice::mode()` reste à `$00`, `DiskIICard::iwmMode`
passe à `$FF`.

**Silicium.** La règle « le mode ne s'écrit qu'IWM au repos » est celle de
MAME. Je ne l'ai pas recoupée avec la *IWM Specification* rev 19 citée par
`DEV.md:3061`, que je n'ai pas consultée, ni avec AppleWin. Indice cohérent
avec elle : le firmware de type Liron réécrit le mode en boucle jusqu'à le
relire dans le statut, ce qui ne se justifie que si une écriture peut être
refusée. Même sans la spec, le comportement POM2 est faux au sens matériel :
un octet de données ne peut pas être à la fois écrit sur la piste et chargé
dans le registre de mode.

**Observation.** Sonde jetable compilée contre le harnais de
`tests/diskii_motor_coast_test.cpp` (`/tmp/iwmaudit/probe_mode_clobber.cpp`,
mêmes sources que la cible `test_diskii_motor_coast`, lancée avec
`POM2_MEDIA_WRITE_DEFAULT=protected`) :

```
no write:      status before=A0 after=A0  motor right after $C0E8=1  after 0.5 s=1
after write:   status before=A0 after=BF  motor right after $C0E8=0  after 0.5 s=0
```

La séquence rejouée est, moteur allumé : `read $D`, `read $E`,
`write $F,$FF`, `read $C`, `read $E`, `read $C`, puis `read $8`. La section 7 de
`diskii_motor_coast_test.cpp` teste déjà le mode bit 2 « voulu » (mode écrit
moteur coupé). Il lui manque le cas inverse : `$C0nF` écrit **moteur allumé**
ne doit changer ni `iwmTimerOff()` ni les bits 0-4 du statut.

---

## Pistes examinées et écartées

Elles sont listées pour qu'on ne les rouvre pas sans élément nouveau. Aucune ne
donne de scénario d'échec observable, ou bien la divergence est documentée.

- **Fenêtres de bit, `read_register_update_delay`, `mode_w`, `data_w`, la
  machine à états de lecture et d'écriture de `sync()`, `update_timer_tick`,
  les transitions moteur/lecture/écriture de `control()`** : identiques à
  `mame0287`, ordre des effets de bord compris.
- **Q3 non modélisé** (`IWMDevice.h:50-56`, `IWMDevice.cpp:57-58`) :
  4 ticks Q3 et 14 ticks 7M donnent la même durée, à 0,09 % près. Divergence
  documentée. Côté amont, quand le moteur est coupé pendant une écriture
  synchrone, la branche MODE_DELAY (227-231) n'appelle pas
  `write_clock_stop()`, donc `cycles_to_time(8388608)` se compte en ticks Q3,
  soit environ 4,1 s au lieu de 1,17 s. Sur ce point **MAME s'écarte de la
  temporisation sur FCLK, et POM2 colle au silicium.**
- **Bruit injecté quand aucun flux n'arrive** (`IWMDevice::noiseTransition`)
  et **borne de rattrapage** (`kMaxSyncCatchUpTicks`) : divergences voulues et
  justifiées dans le code. Le commentaire de `noiseTransition` prétend que MAME
  « modélise cela implicitement », ce qui est inexact : sans transition,
  l'amont décale des zéros et `m_data` reste nul. Le choix de POM2 est celui
  d'un vrai lecteur, dont l'amplificateur produit du bruit sans disque.
- **Statut lu sans lecteur sélectionné** (`IWMDevice.cpp:255-268`, sens 5.25"
  conditionné par `devsel_ != 0`) : équivalent à `!m_floppy` amont, où
  `devsel_w` désélectionne le lecteur ; justifié dans le code.
- **`phasesCb_` émis seulement sur changement**, et **`m_devsel_cb`
  dédoublonné à l'entrée en MODE_DELAY** : l'amont rappelle sans condition,
  mais les récepteurs (`seek_phase_w` déclenché sur front, recalcul du lecteur
  actif) sont idempotents. Pas de scénario.
- **`whd` constant à `$BF` dans l'ombre `DiskIICard`** : simplification
  documentée sur place (`DiskIICard.cpp:1925-1931`).
- **Écriture de flux 5.25" coupée dans `IWMDevice::flushWrite`** : voulue
  (risque de double contrôleur, `IWMDevice.cpp:309-320`).

---

## Bilan

- **Citations examinées** : 55 sur 55, toutes confrontées à `iwm.cpp@mame0287`
  téléchargé. Le code POM2 a été lu autour de chacune.
- **Verdicts** : INEXACTE 3 · ABSENTE 4 · DÉCALÉE 48 · NON-PINNÉE 0 (verdicts
  rendus contre la révision de repli `mame0287`) · OK 0.
- **Constats** : CRITIQUE 0 · MAJEUR 1 · MINEUR 0.
- **Révision amont utilisée** : tag `mame0287` (commit `1393404c`, contenu de
  `iwm.cpp` identique depuis 2023-03). Comparaisons d'appoint : `2b2dd63f`
  (2026-05-12) et `c690a9ed` = `master` (2026-08-16). **Défaut : POM2
  n'épingle aucune révision, et ses citations en mélangent au moins trois.**
- **Non vérifié** :
  - la *IWM Specification* rev 19 (règle d'écriture du mode, câblage /RESET
    sur le //c), que je n'ai pas consultée ;
  - le code RWTS de DOS 3.3 et ProDOS, non désassemblé (l'idiome
    `STA $C08F,X` repose sur le dépôt lui-même et sur la sonde) ;
  - AppleWin, non consulté pour l'IWM ;
  - les renvois sans nom de fichier (« MAME line N »), hors du périmètre du
    grep.

---

## Suite donnée (2026-11-01, après l'audit)

**Constat 1 — confirmé de bout en bout, puis corrigé.**

Le rapport laissait ouverte la déclenchabilité réelle : la sonde rejouait une
séquence d'accès plausible, RWTS n'ayant pas été désassemblé. L'expérience a été
refaite avec un **vrai DOS 3.3** (`disks_5.4/dsk/dos33_master.dsk`) bootant et
exécutant un `SAVE` à travers son propre RWTS, sans la garde d'inactivité :

```
FAIL: DOS 3.3's RWTS write reached the IWM mode register
      (iwmMode = $FF, expected $00).
```

`$FF` : l'octet de synchronisation prédit par le rapport. Le scénario n'est donc
pas hypothétique — un `SAVE` DOS 3.3 standard arme le bit 2 du mode et supprime
le délai d'arrêt moteur pour le reste de la session.

Correction : garde `!spinning()` sur les **deux** chemins de décodage
(`DiskIICard.cpp:2051` bit-LSS, `:2069` legacy), `spinning()` étant le miroir
documenté du `m_active` amont. Épinglée par deux tests, chacun vérifié par
mutation :

- `diskii_motor_coast` § 7 — le `$C0nF` écrit moteur allumé, sur les deux
  chemins de décodage ;
- `dos33_save_smoke` — la preuve de bout en bout, avec le vrai RWTS.

Les trois citations `INEXACTE` ont été corrigées en commentaire, sans changement
de comportement. Le défaut de pin de révision a été porté dans la convention
`CLAUDE.md` (« MAME = source of truth, **at a named revision** »).
