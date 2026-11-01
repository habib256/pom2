# Audit de parité — citations de MAME `a2videoterm.cpp` et `a2pic.cpp` (2026-11-01)

Tranche groupée du brief `docs/audits/BRIEF-parite-amont.md` : deux cartes
voisines par leur taille et par leur façon d'être portées.

- **Videx Videoterm** (`VidexVideotermCard`, clé `videoterm`), MAME
  `src/devices/bus/a2bus/a2videoterm.cpp`. Ce rapport couvre la **carte autour
  du CRTC** : décodage `$C0nX`, VRAM en banques, ROM, `$C800`, jeux de
  caractères, peintre, bascule AN0. Le HD6845S (`Hd6845Crtc`, MAME
  `mc6845.cpp`) relève d'une autre tranche, menée en parallèle.
- **Apple II Parallel Interface Card** (`AppleParallelCard`, clé `pic`), MAME
  `src/devices/bus/a2bus/a2pic.cpp`. Le constat BUSY de la Grappler voisine
  (`parite-grappler-2026-11-01.md`) n'est pas recompté ici.

Les deux chemins amont n'ont jamais bougé : ils sont dans `bus/a2bus/` depuis
le renommage `*.c → *.cpp` (`7c19aac60e`, 2015-11-08).

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep -E 'a2videoterm\.cpp:|a2pic\.cpp:'
```

**21 citations sous `src/`** : 11 `a2videoterm.cpp` (3 dans
`VidexVideotermCard.h`, 6 dans `VidexVideotermCard.cpp`, une dans
`Hd6845Crtc.h`, une dans `RomCatalog.h`) et 10 `a2pic.cpp` (4 dans
`AppleParallelCard.h`, 6 dans `AppleParallelCard.cpp`).
Le motif du brief en laisse passer une vingtaine d'autres, datées elles aussi
(§ « Citations hors motif ») : les numéros **nus** (`(:122)`, `:200`…) de
`VidexVideotermCard.*`, 3 citations dans `tests/`, 4 dans `DEV.md` et 4 dans
`docs/printer-detection.md`.

**Commit POM2 de référence : `af7dd56f`.** Tous les numéros de ligne POM2 sont
ceux de ce commit. Aucun des fichiers audités n'est modifié dans l'arbre de
travail. Aucun fichier du dépôt n'a été modifié. Sondes, scripts et
téléchargements sont sous `/tmp/audit_videoterm_pic/`.

## Sources

**Grade B — documents primaires** (miroir `mirrors.apple2.org.za`, *Apple II
Documentation Project*) :

- *Videx Videoterm — Installation and Operation Manual*, troisième tirage
  (janvier 1982), PDF de 72 pages, scanné sans couche texte. Lu en images. **Le
  scan s'arrête à la page 4-22** : les chapitres 5 (« Device Select
  Operation » 5-9, « Memory Mapping » 5-12) et 6 (« Theory of Operation »,
  « The Soft Video Switch ») **n'y sont pas**. Pages utiles : 4-13 à 4-16, dont
  le Program Listing #3 (p. 4-15).
- **Schéma Videx « Videoterm Display Board » n° V8010**, 10 août 1980,
  © Paul Davis 1980. Une page, lue à 300 et 600 dpi, zone par zone.
- *Videx Soft Video Switch — Manual*, 4 pages, couche texte : § « Theory of
  Operation ».
- *Apple II Parallel Interface Card — Installation and Operating Manual*
  (Apple), 27 pages en images : table 3-1 (p. 16), chapitre 4 et figure 4-1
  (p. 17-18), tables A-5 et A-6 (p. 22-23).
- **Schéma Apple 050-0046-A**, « Apple II Parallel Interface Card », 1/82
  (Apple Service Level 2 Technical Reference #204-1011 vol. 2, document 531
  de D. T. Craig). Lu à 300 et 600 dpi.

**Grade C — ROM** :

- PROM **341-0057** (`roms/341-0057.bin`, CRC `0a6b084b`). Je l'ai désassemblée
  (`da65`) dans la vue CPU, après application du remappage X2, latch ACK à 1 et
  à 0, pour les deux firmwares. Les deux images 256 octets du miroir (« ROM
  Parallel Mode », « ROM Centronics Mode ») sont **exactement** la vue CPU
  latch = 1 des moitiés `$100` et `$000` (0 octet de différence).
- Firmware Videoterm v2.4 (`roms/videx_videoterm_v24_60hz.bin`, premier Ko),
  désassemblé à l'origine `$C800`. Il ne diffère de la version 50 Hz
  (`…_50hz.bin`, identique à la ressource d'izapple2) qu'aux quatre octets de
  la table 6845 que décrit `DEV.md`.
- ROM ][+ (`roms/apple2p.rom`) : `$FA6F` = `LDA $C058` dans le RESET.

**Grade D — émulateurs** :

- MAME : les **41** révisions de `a2videoterm.cpp` et les **38** de `a2pic.cpp`
  (API GitHub, une page de 100), plus `mame0287` et `master`.
- izapple2 `master` (`6abcb80dea54`, 2026-11-01) : `cardVidexVideotern.go`,
  `cardParallelPrinter.go`.
- AppleWin `master` (`4a43bcdb8e1c`, 2026-10-31). **AppleWin n'a pas de
  `source/Videx.cpp`** : aucun fichier Videx dans l'arbre, donc pas
  d'émulation Videoterm, et pas de seconde lecture de ce côté. Il a en revanche
  `source/ParallelPrinter.cpp`. Sa `resource/Parallel.rom` est la vue CPU
  latch = 1 du firmware « Parallel Printer » de la 341-0057, octet pour octet :
  AppleWin émule la PIC sans son latch.

---

## Révision amont

### Pins

- **PIC** : il n'y a pas de pin dans le code, mais `docs/printer-detection.md:9-10`
  en déclare un : « MAME `master` as of 2026-09-26 (commit `f663f942`) ». Ce
  document a été écrit le jour où `AppleParallelCard` est entré dans le dépôt
  (`9742fde4`, 2026-09-26). À cette date, `a2pic.cpp` vaut `ed697c3087`
  (2026-08-12), qui est toujours `master` aujourd'hui. **Le pin est à jour** et
  sert de référence pour la PIC.
- **Videoterm** : **aucun pin**, ni dans `VidexVideotermCard.*`, ni dans
  `DEV.md` § Videx Videoterm. Verdicts rendus contre `mame0287`, conformément
  au brief. Le code suit pourtant **`ed697c3087`**, le `master` du jour de son
  écriture (`45b3e474`, 2026-09-29). Trois citations ne tombent juste que là.

Identités vérifiées par MD5 :

- `a2videoterm.cpp` : `mame0287` = `0cfd9e0b95`, et `master` = `ed697c3087` ;
- `a2pic.cpp` : `mame0287` = `f135a4e417`, et `master` = `ed697c3087`.

### `a2videoterm.cpp` — 41 révisions, 16 cibles datées (11 du motif + 5 numéros nus distincts)

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|---|
| `7c19aac60e` → `a1d35e5abf` (31 révisions) | 2015-11-08 → 2020-09-27 | Ancienne structure ; fusion `a2vidtrm`/`a2vtc2` (2019-06), horloges et largeurs de caractère corrigées (`95830cc258`, 2019-06-09) | 0 |
| `c2c3d685f0`, `48933ef893` | 2021-10-04 → 2024-09-26 | `ROM_START(a2videoterm)` à 44-64 | 2 (`RomCatalog.h:208` 44-64, `:45`) |
| `f135a4e417` | 2025-11-10 | `take_c800` passe à l'opt-in (override `true` en 122), `reset_from_bus` (372-375) | 4 (+ `:122`, `:125`) |
| `c27c33cd83` | 2025-11-10 | Port `CONFIG` pour choisir les jeux primaire et alternatif (+~30 lignes) : la numérotation actuelle | 12 (+ `:200`, `:204`, 382-392, 399-411, 417-420, `:359-365`, `:367-370`, `:372-375`) |
| `04c025b999` | 2025-11-22 | Libellés `CONFIG`, srcclean | 12 |
| `0cfd9e0b95` = **`mame0287`** | 2025-11-23 | Correctif hors-bornes : VRAM indexée `& 0x1ff`, écriture bornée à `< 0x600` (avant, le `STA $CFFF` du firmware écrivait hors du tableau). **En lecture, `$CE00-$CFFF` reste un miroir de la VRAM** | 14 (+ 379-471, 464-471) |
| `774a180df2` | 2026-08-02 | `SCREEN(config, …)` sans type (ligne 199) | 14 |
| `ed697c3087` = `master` | 2026-08-12 | `get_open_bus()` : `read_c0nx` hors offset 1, et `read_c800` pour `offset >= 0x600` (+2 lignes en 453-458) | **16** — 444-459 (avec le bus flottant) et 473-500 **ne tombent juste que là** |

**Une seule époque, `ed697c3087`.** Toutes les citations du Videoterm tombent
juste sur `master`, et le code implémente les deux changements de
`ed697c3087` : bus flottant en `$C0nX` (`VidexVideotermCard.cpp:76`) et en
`$CE00-$CFFF` (`:99`). Aucun correctif amont postérieur n'existe : `master`
n'a pas bougé depuis le 2026-08-12. Le pin à écrire :

| Upstream | Pris | Pas pris |
|---|---|---|
| `f135a4e417` (take_c800 opt-in, reset_from_bus) | oui | — |
| `c27c33cd83` (port CONFIG des jeux de caractères) | le défaut (normal + inverse) | les dix autres jeux (`DEV.md` : « not offered yet ») |
| `0cfd9e0b95` (VRAM `& 0x1ff`, écriture < `$600`) | oui | — |
| `ed697c3087` (bus flottant) | oui | — |

### `a2pic.cpp` — 38 révisions, 8 cibles datées

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|---|
| `7c19aac60e` → `97b6717027` (23 révisions) | 2015-11-08 → 2019-03-26 | Ancienne implémentation (`m_autostrobe`, latch ACK effacé à la lecture de `$C0n4`, PROM sans remappage) | 0 |
| `3677f57a4a` → `49006281c1` (4 révisions) | 2020-10-24 → 2021-04-03 | Réécriture (V. Crabb) : interrupteurs, cavaliers X1-X6, latch ACK, remappage X2 ; classe déclarée dans `a2pic.h` | 0 |
| `299265cc52` → `5f4e3730d9` (8 révisions) | 2021-10-02 → 2024-10-17 | Classe déplacée dans le `.cpp` (+73 lignes), puis changements d'API sans décalage | 3 (286-293, 294-299, 240-275) |
| `f135a4e417` = **`mame0287`** | 2025-11-10 | `reset_from_bus` séparé de `device_reset` (+6 lignes en 364) | 6 (+ 210-283, 210-238, 305-311) |
| `ed697c3087` = `master` = **pin déclaré** | 2026-08-12 | `get_open_bus()` dans `read_c0nx` : bits 1-6 de `$C0n4` et valeur par défaut. Aucun décalage | 6 — le **contenu** décrit par `AppleParallelCard.h:27-29` et `.cpp:38-49` (bus flottant) n'existe qu'ici |
| — | — | — | **2 ne tombent juste sur aucune révision** : 358-371, 364-365 |

La PIC est elle aussi sur une seule époque (`ed697c3087`). Les deux citations
de reset sont fausses sur toutes les révisions (voir Rapport 1).

Méthode : historique complet récupéré, une ancre regex par citation (le code
décrit, par exemple `m_rom\[offset\+0x300\]` ou `m_firmware_base = BIT\(sw1, 5\)`),
une révision « tombe juste » quand la plage contient toutes les ancres, puis
relecture à la main (`/tmp/audit_videoterm_pic/date.py`). J'ai aussi testé la
numérotation sans lignes vides (le piège des tranches 5 et 10) : elle ne
sauve aucune des deux citations orphelines.

---

## Rapport 1 — Citations

Ordre : INEXACTE, DÉCALÉE, OK (rev X), OK. La « ligne corrigée » est au format
de `CLAUDE.md`. Je la fais monter au grade B ou C quand j'ai trouvé mieux que
MAME.

### `a2pic.cpp` (référence : `ed697c3087`, pin déclaré)

| Fichier POM2:ligne | Citation | Verdict | Commentaire | Ligne corrigée |
|---|---|---|---|---|
| `src/AppleParallelCard.h:72` | `a2pic.cpp:364-365` | **INEXACTE** | « SW1:6 […] Takes effect on reset, as on the card ». **Aucune révision ne place l'affectation du firmware en 364-365** : sur `ed697c3087`, ce sont l'accolade et l'appel `reset_from_bus()` de `device_reset`, et l'affectation est en 374. Surtout, MAME applique SW6 **immédiatement** (`sw1_firmware`, 170-173, via `PORT_CHANGED_MEMBER`) **et** le relit au reset. Sur le schéma, SW6 attaque directement la broche A8 de la PROM : l'effet est immédiat sur la carte aussi. Le code POM2 ne l'applique qu'au reset (`AppleParallelCard.cpp:106`), mais rien ne l'appelle (piste latente). | `/// [B] Schéma Apple 050-0046-A : SW6 attaque directement A8 de la PROM 7B (tirage RP2-5) — effet immédiat ; manuel PIC p. 23 : SW6 OFF = « Parallel » ($100), ON = « Centronics » ($000).` + `/// [D] MAME a2pic.cpp:170-173 (sw1_firmware, immédiat) et :374 (relu au reset) @ ed697c3087.` |
| `src/AppleParallelCard.cpp:105` | `a2pic.cpp:358-371` | DÉCALÉE | **Ne tombe juste sur aucune des 38 révisions.** Sur `ed697c3087`, 358-371 couvre la fin de `device_start`, `device_reset` (363-366) et l'en-tête de `reset_from_bus` (369-371), sans aucun de ses effets (374-385). Sur ≤ `5f4e3730d9`, `device_reset` occupait 362-379. | `// [D] MAME a2pic.cpp:363-386 @ ed697c3087 — device_reset → reset_from_bus : SW6 relu, autostrobe coupé, reset_mode (IRQ coupée, latch ACK à 1).` + `// [B] Manuel PIC ch. 4 et fig. 4-1 : le reset coupe l'autostrobe et l'IRQ et remet la bascule ACK.` |
| `src/AppleParallelCard.cpp:65` | `a2pic.cpp:240-275` | DÉCALÉE | `write_c0nx` occupe 241-281. La plage coupe le cas 7 (277-279), que le code POM2 implémente juste en dessous (`:79`). La troncature est la même sur les 10 révisions où les autres ancres tombent juste. | `// [D] MAME a2pic.cpp:241-281 @ ed697c3087 — write_c0nx.` + `// [B] Manuel PIC table A-6 : $C080 charge le port (+ strobe si autostrobe), $C082 strobe, $C086 autorise l'IRQ, $C087 la coupe, remet la bascule ACK et coupe l'autostrobe.` |
| `src/AppleParallelCard.h:27` | `a2pic.cpp:210-283` | OK | `read_c0nx` + `write_c0nx`, A3 ignorée. Le bus flottant de `$C0n4` (bits 1-6) et de la valeur par défaut n'existe que depuis `ed697c3087` : sur `mame0287`, `$C0n4` lit 0 sur les bits 1-6 et la valeur par défaut est `0x00`. | `// [B] Schéma 050-0046-A : 74LS138 (6B) décode A0-A2 seules sous DEVSEL (C080, C082-C087 ; $C0n1 non câblée) ; tirages RP1 sur DI0-DI7, d'où le $97.` + `// [D] MAME a2pic.cpp:210-281 @ ed697c3087.` |
| `src/AppleParallelCard.cpp:38` | `a2pic.cpp:210-238` | OK | `read_c0nx` (accolade fermante en 239). Le `openBus() & 0x7E` de POM2 est celui de `ed697c3087`. | `// [D] MAME a2pic.cpp:210-239 @ ed697c3087 — $C0n4 = latch<<7 | bus flottant & $7E | /ACK.` + `// [B] Schéma 050-0046-A : 74LS125 (5B) met la bascule ACK sur D7 et /ACK (via le XOR de SW5) sur la ligne de DI0 ; le reste flotte. Le manuel (table A-6) dit « bit 6 » : voir Pistes.` |
| `src/AppleParallelCard.h:36` | `a2pic.cpp:286-293` | OK | Remappage X2 ; inchangé depuis `299265cc52`. | `// [B] Manuel PIC table A-5 p. 22 : $Cs00-3F↔$Cs40-7F, $Cs80-BF↔$CsC0-FF.` + `// [C] 341-0057 : la boucle d'attente d'ACK est $CnC0 BCC $CnC0 / $CnC2 BCS $CnC2, visible seulement latch à 0 (sonde [P2]).` + `// [D] MAME a2pic.cpp:286-293 @ ed697c3087.` |
| `src/AppleParallelCard.cpp:88` | `a2pic.cpp:286-293` | OK | Idem. | idem |
| `src/AppleParallelCard.h:34` | `a2pic.cpp:294-299` | OK | `m_autostrobe_disable = 0U` en 298. | `// [B] Manuel PIC ch. 4 p. 17 : l'autostrobe est armé quand le code de la PROM est accédé (I/O SELECT), désarmé par un reset ou par $C087.` + `// [C] 341-0057 : le seul accès des deux firmwares est STA $C080,Y ($C184 / $C18E), qui ne strobe que grâce à lui.` + `// [D] MAME a2pic.cpp:294-299 @ ed697c3087.` |
| `src/AppleParallelCard.cpp:94` | `a2pic.cpp:294-299` | OK | Idem. | idem |
| `src/AppleParallelCard.cpp:100` | `a2pic.cpp:305-311` | OK | `write_cnxx` (305-312). Tombe juste depuis `f135a4e417` ; c'était 304-311 avant. | `// [D] MAME a2pic.cpp:305-312 @ ed697c3087 — une écriture $CnXX (conflit de bus) arme aussi l'autostrobe.` |

### `a2videoterm.cpp` (référence : `mame0287` ; le code suit `ed697c3087`)

| Fichier POM2:ligne | Citation | Verdict | Commentaire | Ligne corrigée |
|---|---|---|---|---|
| `src/VidexVideotermCard.h:26` | `a2videoterm.cpp:379-471` | OK (rev `ed697c3087`) | La plage contient `read_c0nx` … `write_c800` sur les deux révisions. Mais la carte qu'elle documente annonce « `$CE00-$CFFF` open bus », ce qui n'est vrai que sur `ed697c3087` : sur `mame0287`, ces adresses sont un miroir de la VRAM. | `// [B] Manuel Videoterm (3e tirage, 1982) p. 4-14 à 4-16, listing #3 : page = PEEK($C080+s·16+4·page), fenêtre $CC00-$CDFF de 512 octets.` + `// [C] Firmware v2.4 : $CA69 LDA $C0B0,X (X = page·4), $CA80/$CA85 STA $CC00,X/$CD00,X.` + `// [D] MAME a2videoterm.cpp:382-470 @ ed697c3087 (bus flottant en $CE00-$CFFF ; miroir de VRAM @ mame0287).` |
| `src/VidexVideotermCard.cpp:95` | `a2videoterm.cpp:444-459` | OK (rev `ed697c3087`) | `read_c800` avec le `get_open_bus()` de `offset >= 0x600`. Sur `mame0287`, la fonction tient en 444-457 et n'a pas de bus flottant. | `// [D] MAME a2videoterm.cpp:444-459 @ ed697c3087 — ROM $C800-$CBFF, VRAM $CC00-$CDFF, bus flottant $CE00-$CFFF (444-457, miroir de VRAM, @ mame0287).` |
| `src/VidexVideotermCard.cpp:134` | `a2videoterm.cpp:473-500` | OK (rev `ed697c3087`) | `crtc_update_row`, exactement. Sur `mame0287`, il est en 471-498. | `// [D] MAME a2videoterm.cpp:473-500 @ ed697c3087 (471-498 @ mame0287) — crtc_update_row.` |
| `src/VidexVideotermCard.h:72` | `a2videoterm.cpp:125` | OK | `uint8_t m_ram[512*4]`. | `/// [D] MAME a2videoterm.cpp:125 @ mame0287. [B] Schéma V8010 : quatre 2114 (U10-U13), 2 Ko ; manuel p. 4-16 : « ADDRESS MOD 2048 ».` |
| `src/VidexVideotermCard.h:110` | `a2videoterm.cpp:122` | OK | `take_c800() … { return true; }`, depuis `f135a4e417`. | `// [D] MAME a2videoterm.cpp:122 @ mame0287.` + `// [C] Firmware : $CB36 STA $CFFF exécuté depuis la page $C3, la carte reprend $C800 sur /IOSEL.` + `// [B] Schéma V8010 : I/O STB (broche 20) attaque le 74LS139 U9.` |
| `src/Hd6845Crtc.h:18` | `a2videoterm.cpp:204` | OK | `HD6845S(config, m_crtc, 17.43_MHz_XTAL / 9)`. | `// [D] MAME a2videoterm.cpp:204 @ mame0287.` + `// [B] Schéma V8010 : quartz Y1 17,430 MHz.` + `// [C] Table 6845 du firmware ($C8A1) : R0 = $7B, soit 124 × 9 / 17,43 MHz = 64,0 µs par ligne.` |
| `src/RomCatalog.h:208` | `a2videoterm.cpp:44-64` | OK | `ROM_START(a2videoterm)` … `ROM_END`, inchangé depuis `c2c3d685f0`. | `// [D] MAME a2videoterm.cpp:44-64 @ mame0287.` |
| `src/VidexVideotermCard.cpp:71` | `a2videoterm.cpp:382-392` | OK | `read_c0nx`. Le `openBus()` de POM2 est le `get_open_bus()` de `ed697c3087` (0xFF sur `mame0287`). | `// [D] MAME a2videoterm.cpp:382-392 @ ed697c3087 — toute lecture $C0nX choisit la banque ; seul $C0n1 répond.` + `// [B] Manuel p. 4-15 : SELECT=PEEK(LINE2+PAGE*4).` |
| `src/VidexVideotermCard.cpp:81` | `a2videoterm.cpp:399-411` | OK | `write_c0nx`, exact. Mais la phrase « RS is A0 for offsets 0/1 only: $C0n2/3, $C0n6… reach nothing on the 6845 » est la lecture de MAME, présentée comme un fait. **Le schéma la met en doute** : /CS du 6845 (broche 25) est à la masse, RS = A0 (piste 1). | `// [D] MAME a2videoterm.cpp:399-411 @ mame0287 — seuls $C0n0/$C0n1 atteignent le 6845.` + `// [F] Schéma V8010 : /CS (broche 25) à la masse, RS = A0 ; si E (broche 23) suit DEVSEL, toute adresse impaire est le registre et toute écriture paire le latch (lecture d'izapple2). Tranché par : le tracé de E sur une carte, ou les chapitres 5-6 du manuel.` |
| `src/VidexVideotermCard.cpp:90` | `a2videoterm.cpp:417-420` | OK | `m_rom[offset+0x300]`. | `// [D] MAME a2videoterm.cpp:417-420 @ mame0287.` + `// [C] Firmware : le code de la page $C3 est celui de $CB00-$CBFF ($CB36 STA $CFFF, LDX #$C3).` |
| `src/VidexVideotermCard.cpp:104` | `a2videoterm.cpp:464-471` | OK | Sur `ed697c3087`, `write_c800` occupe exactement 464-470. Sur `mame0287`, 462-469 : la plage contient le corps. | `// [D] MAME a2videoterm.cpp:464-470 @ ed697c3087 — seule la fenêtre VRAM s'écrit (borne < $600 depuis 0cfd9e0b95).` |

### Citations hors motif

| Où | Citation | Verdict | Note |
|---|---|---|---|
| `VidexVideotermCard.h:35` | `(:122)`, `(:372-375)` | OK, OK | `take_c800` ; `reset_from_bus` → `m_crtc->reset()`. Grade B : sur le schéma, RESET du 6845 (broche 2) relié au RESET du bus (broche 31) — lu, tracé incomplet. |
| `VidexVideotermCard.h:40` | `(crtc_update_row, :473-500)` | OK (rev `ed697c3087`) | Comme `.cpp:134`. |
| `VidexVideotermCard.h:76-77` | `:200` (×2) | OK | `set_raw(17.43_MHz_XTAL, 1116, 0, 720, 260, 0, 216)`. |
| `VidexVideotermCard.h:78` | `:204` | OK | |
| `VidexVideotermCard.h:87`, `.cpp:35` | `(:45)` (×2) | OK | `ROMREGION_ERASEFF`. Grade B : sur la vraie carte, l'EPROM du firmware est une **2708 de 1 Ko** (U3 sur le schéma). L'image de 2 Ko est celle du clone (`6.ic6.bin`), et l'en-tête dit à tort « a 2 KB firmware EPROM » (`.h:22`). |
| `VidexVideotermCard.cpp:34` | `(:359-365)` | OK | `device_start` : `memset(m_ram, 0, …)`. |
| `VidexVideotermCard.cpp:111-112` | `(:367-370)`, `(:372-375)` | OK, OK | La fusion des deux resets en un seul hook est la piste 2. |
| `tests/videx_videoterm_bus_test.cpp:18` | `379-471` | OK | |
| `tests/videx_videoterm_bus_test.cpp:108` | `122` | OK | |
| `tests/videx_videoterm_render_test.cpp:18` | `473-500` | OK (rev `ed697c3087`) | |
| `DEV.md:1595` | `379-471` | OK (rev `ed697c3087`) | Annonce « `$CE00-$CFFF` open bus » (`DEV.md:1600`). |
| `DEV.md:1600`, `:1609` | `:122`, `:200` | OK | |
| `DEV.md:1607` | `:473-500` | OK (rev `ed697c3087`) | |
| `docs/printer-detection.md:76`, `:319` | `286-293` | OK | |
| `docs/printer-detection.md:313` | `210-283` | OK | |
| `docs/printer-detection.md:321` | `294-299` | OK | |
| `VidexVideotermCard.h:52`, `DEV.md:1627` | izapple2 `cardVidexVideotern.go:131-139` | OK, non pinnée | Juste sur `6abcb80dea54`. À écrire `[D] izapple2 cardVidexVideotern.go:131-139 @ 6abcb80dea54`. |
| `VidexVideotermCard.h:48-51`, `DEV.md:1622-1626` | `vterm24.dis LC82A / LCA89 / LC987`, Autostart `$FA6F` | OK (grade C) | `STA $C059` en `$C82A`, et en `$CA8F` dans la routine `$CA89`. `STA $C058` en `$C995`, dans la routine `$C987` (Ctrl-Z 1). La ROM ][+ fait `LDA $C058` en `$FA6F`. |

---

## Rapport 2 — Constats

**Aucun constat atteignable.** Sur les deux cartes, le code fait ce que MAME
`ed697c3087` fait, et pour le comportement que le firmware exerce, il fait ce
que disent les schémas, les manuels et les ROM. Trois divergences avec le
silicium (ou avec MAME) ont été établies ou soupçonnées, mais aucune n'est
atteignable avec un logiciel connu. Elles sont dans les pistes 1 à 3.

Deux résultats protègent le code d'un futur alignement :

### 1. AMONT FAUX — izapple2 dessine la Videoterm en cellules de 8 points ; il en faut 9

```
Fichier      : src/VidexVideotermCard.h:73 (kCellWidth = 9), .cpp:161-166 — POM2 a raison
Amont        : izapple2 cardVidexVideotern.go (videxCharWidth = uint8(8)) @ 6abcb80dea54
Gravité      : —
Confiance    : CONFIRMÉ (schéma + table du firmware ; MAME et POM2 concordent)
```

**Divergence.** izapple2, que POM2 cite pour la règle AN0, décale 8 points par
caractère (`for range int(videxCharWidth)`, `bits <<= 1`) et produit une image
de 640 points. MAME (`set_char_width(9)`, :207 @ `mame0287`) et POM2 font 9
points, le neuvième répétant le bit 0.

**Pourquoi 9.**

- Le quartz du schéma V8010 fait 17,430 MHz, et le 6845 tourne à 1/9 de cette
  fréquence (74LS161 U23 en diviseur).
- La table 6845 du firmware v2.4 programme R0 = `$7B`, soit 124 caractères par
  ligne. Avec 9 points : 124 × 9 / 17,43 MHz = **64,0 µs**, une ligne NTSC
  (15,6 kHz). Avec 8 points : 56,9 µs, soit 17,6 kHz, hors de portée d'un
  moniteur de 1980. La table 50 Hz (R0 = `$7A`) donne 63,5 µs.
- La zone visible de MAME, 1116 = 124 × 9, dit la même chose.

**Risque protégé.** POM2 cite izapple2 dans ce même en-tête. Un
« alignement » sur sa largeur de cellule serait une régression.

### 2. AMONT FAUX (au tag) — `mame0287` sert la VRAM en miroir à `$CE00-$CFFF`

```
Fichier      : src/VidexVideotermCard.cpp:96-99 — POM2 a raison contre mame0287
Amont        : a2videoterm.cpp:444-457 @ mame0287 (= 0cfd9e0b95) ; corrigé par ed697c3087
Gravité      : —
Confiance    : PLAUSIBLE — MAME master concorde ; le schéma va dans ce sens sans
               que j'aie pu suivre chaque sortie du 74LS139
```

**Divergence.** Sur `mame0287`, `read_c800` renvoie
`m_ram[(offset & 0x1ff) + m_rambank]` pour **tout** `offset >= 0x400`. La
fenêtre `$CC00-$CDFF` réapparaît donc en `$CE00-$CFFF`, jusqu'à `$CFFF`
compris. Sur `ed697c3087`, ces adresses lisent le bus flottant, comme chez
POM2 (sonde [V3] : `$CE00 = FF` avec `$CC00 = 77`).

**Indices côté silicium.** Sur le schéma V8010, I/O STB valide un 74LS139 (U9)
qui décode A9-A10 en quatre quarts de 512 octets. Le manuel ne parle que de
`$CC00-$CDFF` pour la RAM (p. 4-16 : « the addresses $CC00 to $CDFF are used
in writing characters »), et le firmware n'adresse jamais rien au-delà de
`$CDFF`, sauf le `STA $CFFF` de libération. Il est donc peu probable que la
2114 soit sélectionnée au quatrième quart.

**Risque protégé.** Un alignement sur le tag `mame0287`, la référence par
défaut du brief, rétablirait le miroir. C'est l'inverse de ce qu'a fait
l'amont lui-même.

---

## Pistes examinées et écartées

**Les trois divergences latentes, par ordre d'importance :**

1. **Videoterm — le 6845 répond peut-être sur les 16 adresses `$C0nX`, RS =
   A0.** Sur le schéma V8010, /CS (broche 25) est **relié à la masse**, et RS
   (broche 24) = A0. Le 6845 n'est donc sélectionné que par E (broche 23). Si E
   suit DEVSEL, ce que le schéma suggère sans que j'aie pu le suivre jusqu'au
   bout :
   - toute écriture paire charge le latch d'adresse ;
   - toute écriture impaire écrit le registre ;
   - toute lecture impaire lit le registre.

   C'est exactement le modèle d'izapple2 (`rsPin := (i & 1) == 1` sur
   `i = 0..15`). MAME et POM2 ne décodent que `$C0n0`/`$C0n1` (sonde [V1] :
   `$C0n3` lit `FF` au lieu de R12 ; [V2] : `$C0n2`/`$C0n3` laissent R13 à 0).

   **Latent** : le firmware v2.4 n'écrit le 6845 qu'en `$C0B0`/`$C0B1` (dix
   accès absolus, de `$C81C` à `$C9E0`). Il ne lit jamais un registre. Il ne
   change de banque que par `LDA $C0B0,X` avec X ∈ {0, 4, 8, C}, des lectures
   paires sans effet sur le 6845. Le listing #3 du manuel fait de même
   (`PEEK(LINE2+PAGE*4)`). Il faudrait un programme qui change de banque **en
   écriture** (`STA $C0B4`) puis écrive `$C0B1` sans recharger le latch.

   Je ne l'ouvre pas en constat : la source de E n'est pas tracée, et une
   validation par A1-A3 rendrait MAME exact. Ce qui trancherait : les
   chapitres 5 (« Device Select Operation ») et 6 (« Theory of Operation ») du
   manuel, absents du scan, ou un multimètre sur une carte.
2. **Videoterm — le Ctrl-Reset remet la banque VRAM à 0 ; MAME et le schéma la
   gardent.** `VidexVideotermCard::onReset` (`.cpp:109-115`) fait
   `device_reset` (banque 0) **et** `reset_from_bus` (6845) sur chaque reset du
   bus. MAME ne fait que le second au RESET (`reset_from_bus`, 372-375). Sur le
   schéma, les entrées R des bascules 4013 (U7, U8) sont à la masse : la
   banque n'est pas câblée au RESET. Sonde [V4] : banque 512 avant, 0 après.

   **Le dépôt se contredit** : `GrapplerCard.cpp:257-266` garde sa banque
   `$C800` au reset par exactement ce raisonnement (« the U2D bank flip-flop is
   not wired to bus RESET […] an earlier POM2 revision cleared it here, a
   silent divergence »), et `SlotPeripheral` offre `onPowerCycle` pour séparer
   les deux.

   **Latent** : le firmware recharge la banque (`$CA69`) avant **chacun** de
   ses accès VRAM (`$CA80`, `$CA85`, `$CBA3`, `$CBA8`), et l'Autostart libère
   `$C800` au reset (`$FA7B LDA $CFFF`). Il faudrait un programme qui écrive
   `$CC00` après un Ctrl-Reset sans resélectionner de page.
3. **PIC — SW6 n'agit qu'au reset ; sur la carte et chez MAME, il agit tout de
   suite.** `setParallelPrinterFirmware` ne fait que mémoriser l'interrupteur,
   et `firmwareBase_` n'est recalculé que dans `onReset` (sonde [P4] : `$C116`
   reste `28` jusqu'au reset, puis passe à `AA`). Sur le schéma 050-0046-A,
   SW6 est la ligne A8 de la PROM. Chez MAME, `sw1_firmware` (170-173) applique
   le changement sur-le-champ. **Latent** : aucun appelant dans `src/` (ni
   interface, ni `PrinterPortControl`, ni session), donc le firmware
   « Centronics » est inatteignable aujourd'hui. Le commentaire est la
   citation INEXACTE du Rapport 1.

**Le reste, sain ou voulu :**

- **PIC — bit de l'état /ACK en `$C0n4` : bit 0 (schéma, MAME, POM2) ou bit 6
  (manuel).** La table A-6 du manuel (p. 23) dit « Read ACK status (bit 6) and
  ACK flip-flop status (bit 7) ». Le schéma 050-0046-A, lu à 600 dpi, met la
  sortie du 74LS125 (5B) broche 11, l'entrée /ACK après le XOR de SW5, sur la
  ligne de DI0 (sortie 18 du 74LS244 9A), et la broche 3, la bascule ACK, sur
  celle de DI7. Les 74LS244 et 74LS125 ne sont jamais validés ensemble (`$C083`
  contre `$C084`). Deux documents Apple de grade B se contredisent. Le câblage
  donne raison à MAME et à POM2, sous réserve que les lignes DIn aillent bien
  sur Dn (c'est ce que fait le statut `$97` de MAME, cohérent avec les
  tirages). Aucun des deux firmwares ne lit `$C0n4`. Je n'ouvre rien.
- **PIC — boucle d'attente du firmware** : `$CnC0 BCC $CnC0` / `$CnC2 BCS $CnC2`
  latch à 0, `BCC $C1E9` / `BCS $C1C4` latch à 1. Le firmware ne lit jamais le
  statut : **c'est le remappage d'A6 qui fait l'attente**. Sonde [P2], et
  `parallel_cards` (PR#1, hors ligne, reprise) passe.
- **PIC — autostrobe** : coupé au reset, armé par tout accès `$CnXX`, lecture
  comme écriture. Sonde [P1] : après un reset, `$C0n0` sans accès `$CnXX`
  n'imprime rien, puis `$C0n2` imprime. Conforme au manuel (ch. 4, table A-6)
  et à MAME.
- **PIC — statut `$C0n3`** : `$DF` prêt, `$97` hors ligne, `$F7` sans papier,
  `$FF` sans imprimante (sonde [P3]). Le `$97` est l'effet des tirages RP1 sur
  DI0-DI7 du schéma. Les trois lignes viennent d'un seul modèle,
  `CentronicsPrinter`, partagé avec la Grappler 1981 : pas de deuxième règle.
- **PIC — décodage** : A3 ignorée, et `$C0n1` ne fait rien (sortie Y1 du
  74LS138 non câblée). Les lectures de `$C0n6`/`$C0n7` ont les mêmes effets que
  les écritures (74LS138 non qualifié par R/W). `$C0n5` (strobe auxiliaire)
  n'est modélisé ni chez MAME ni chez POM2, ce qui est documenté.
- **PIC — durée et polarité du strobe, polarité de l'ACK, cavaliers X1-X6** :
  **VOULU** (`AppleParallelCard.h:40-42`, `docs/lle_vs_hle.md`). Les défauts
  de MAME sont la colonne « Centronics » du schéma pour SW4, SW5 et SW7, et
  POM2 les fige.
- **PIC — IRQ** : jamais levée. SW7 vaut « Disabled » par défaut chez MAME, et
  OFF dans la colonne Centronics du schéma. Le manuel précise : « Current
  Apple II system software does not process interrupts ». Pas d'interrupteur
  exposé : latent, équivalent au défaut.
- **PIC — défaut SW6 = « Parallel Printer » (LF après CR)** : c'est le défaut de
  MAME. La colonne « Centronics » du schéma met pourtant SW6 sur ON (firmware
  Centronics) pour une imprimante Centronics. C'est un choix de configuration,
  pas une divergence de comportement. À noter si l'ImageWriter hôte double un
  jour les interlignes.
- **PIC — octet tenu, abandonné au reset** : **VOULU** (`CentronicsPrinter.h:30-36`,
  `:83`), appliqué de la même façon par les trois cartes parallèles
  (`GrapplerCard.cpp:268`, `GrapplerClassicCard.cpp:86`).
- **PIC — ACK instantanée, latch jamais bloqué par un strobe en cours** (garde
  `m_strobe_timer->enabled()`, 530-533) : **VOULU**, c'est la couture « strobe »
  de `lle_vs_hle.md`.
- **PIC — `$C800`** : non réclamé (`takesC800` par défaut à `false`). Le manuel
  le confirme (table A-5) : « PIC does not use this space ».
- **PIC — AppleWin** : `ParallelPrinter.cpp` répond `$FF` à toute lecture,
  masque le bit 7 en sortie (`value & 0x7F`) et n'a pas de latch. Sa ROM est
  le firmware de la PIC sans remappage. Le masquage correspond au cavalier X5
  (7 bits), que MAME et POM2 laissent en X6 (8 bits). Je n'ai pas trouvé le
  réglage d'usine.
- **`PrinterPortControl` — `line_bits` de la PIC** : rapporte BUSY | PE |
  SELECT dans la disposition de la Grappler+ (`PrinterPortControl.cpp:138-140`,
  commentaire `PrinterPortControl.h:81` « as the status byte carries them »).
  La PIC ne lit pas BUSY, et son octet d'état a une autre disposition. C'est un
  rapport hôte (HTTP `/printer-port`), invisible du programme invité : pas de
  constat, mais le commentaire est faux pour `pic`.
- **Videoterm — banque choisie par lecture comme par écriture, fenêtre de 512
  octets, 2 Ko** : B (manuel p. 4-14 à 4-16) et C (`$CA69`). La lecture ou
  l'écriture **dans** la fenêtre `$CC00` ne change pas de banque, chez POM2
  comme partout.
- **Videoterm — `$CnXX`** : ROM[`$300` + octet bas], écritures ignorées. Le
  firmware écrit dans la page `$C3` « pour réclamer `$C800` » (commentaire MAME
  435) : sans effet, conforme.
- **Videoterm — `take_c800` et `$CFFF`** : réclamé par `/IOSEL`, libéré par
  `$CFFF`, par `SlotBus` (testé par `videx_videoterm_bus` sur une vraie
  `Memory`). Le `STA $CFFF` du firmware (`$CB36`) n'écrit rien dans la VRAM
  chez POM2 (borne `< $600`), contrairement à MAME avant `0cfd9e0b95`.
- **Videoterm — jeu alternatif dérivé (`~normal`)** : c'est le défaut de MAME
  (`CONFIG` = `0xa0`, `4.ic4.bin`), et le firmware met le drapeau inverse dans
  le bit 7 (`$CA71-$CA7E`, `ROR` depuis `$07FB`). Les dix autres jeux de
  `c27c33cd83` ne sont pas offerts, ce qui est documenté.
- **Videoterm — neuvième point = bit 0 répété** : identique à
  `crtc_update_row` (sonde du test `videx_videoterm_render`).
- **Videoterm — VRAM à zéro à la mise sous tension** : comme MAME
  (`device_start`). Des 2114 réelles partent au hasard. Latent : le firmware
  efface l'écran avant de l'utiliser.
- **Videoterm — accès factices du 6502** (`STA $C0B0,X` lit avant d'écrire) :
  même adresse, même banque, sans effet.
- **Videoterm — débogueur** : ni la PIC ni la Videoterm n'implémentent
  `peekSlotRom` / `peekExpansionRom`. Le débogueur montre donc le miroir plat,
  et surtout **ne déclenche aucun effet de bord**, ni autostrobe ni banque
  (`MemoryCpuView.cpp:77-87`). Cosmétique.
- **Bascule de moniteur TEXT && AN0** (`ownsScreen`) : grade B. Le manuel du
  Soft Video Switch : « controlled by two conditions: the state of annunciator
  zero and the color killer signal ». Couleur active (graphique, mixte compris),
  40 colonnes ; sinon AN0. Deux réserves, sans constat :
  - le même manuel inverse `$C058`/`$C059` (« To turn annunciator zero on, a
    memory reference to $C058 ») : c'est une coquille, car le firmware fait
    `STA $C059` en entrant (`$C82A`) et l'Autostart `LDA $C058` au reset ;
  - un Apple II rév. 0, sans color killer, suivrait AN0 seul : latent.
- **Videoterm abandonné sur les profils //e** : vérifié dans
  `SlotConfigurationCoordinator.cpp:243-260` (plan vidé, clé gardée). Les
  sessions n'ont pas été suivies.
- **6845 au Ctrl-Reset** (R14/R15 et le latch remis à 0 par
  `Hd6845Crtc::reset`) : laissé à la tranche `mc6845.cpp`. Côté carte, je ne
  retiens que ceci : la broche RESET du 6845 est bien reliée au RESET du bus
  sur le schéma, donc le `reset_from_bus` de MAME est fondé.
- **Dérive interne POM2** : `docs/lle_vs_hle.md:144` cite
  `AppleParallelCard.h:39-41` pour la liste « Not modelled », qui est en 40-42.

---

## Sonde

`/tmp/audit_videoterm_pic/probe.cpp`, greffée sur les sources de
`parallel_cards` et `videx_videoterm_bus` :

```
clang++ -std=c++17 -O1 -Isrc -Itests -Ibuild/generated probe.cpp \
  src/AppleParallelCard.cpp src/CentronicsPrinter.cpp \
  src/VidexVideotermCard.cpp src/Hd6845Crtc.cpp build/libpom2_core_test.a \
  -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework CoreFoundation
```

La Videoterm y tourne sur une ROM et une police synthétiques, la PIC sur la
vraie PROM 341-0057. Sortie, sur `af7dd56f` :

```
[V1] $C0n1 lu (R12)            = 15
[V1] $C0n3 lu (A0=1, R12 ?)    = FF  (schéma : RS=A0 -> R12 si E suit DEVSEL)
[V2] R13 après écritures $C0n2=13/$C0n3=$2A = 00 (POM2/MAME : 00 ; RS=A0 : 2A)
[V3] banque après $C0n4 = 512 ; $CC00=77 ; $CE00=FF (mame0287 : miroir 77 ; master : bus flottant)
[V4] banque après onReset (Ctrl-Reset) = 0 (MAME reset_from_bus : garde 512 ; schéma : 4013 R à la masse)
[V4] R12 après onReset = 15 (conservé)
[P1] après reset : écriture $C0n0='A' sans accès $CnXX -> imprimés = 0
[P1] puis $C0n2 (strobe)                         -> imprimés = 1
[P2] $C1C0/$C1C2 latch ACK=1 : 90 27 B0 00
[P2] hors ligne, après strobe : $C0n4=7F ; $C1C0/$C1C2 = 90 FE B0 FE (boucle sur place attendue : 90 FE B0 FE)
[P2] de retour en ligne : $C0n4=FF ; imprimés = 2 ; $C1C0 = 90 27
[P3] statut $C0n3 prêt=DF hors-ligne=97 papier=F7 absente=FF
[P4] SW6 -> Centronics, sans reset : $C116 = 28 ; après reset : AA (Centronics $000)
```

Les binaires de test existants passent eux aussi, avec
`POM2_MEDIA_WRITE_DEFAULT=protected` : `test_parallel_cards` (firmware PIC de
bout en bout sur //e), `test_videx_videoterm_bus` et
`test_videx_videoterm_boot` (PR#3 sur la ROM ][+ réelle).

---

## Bilan

- **Citations examinées** : 21 sous `src/` (11 `a2videoterm.cpp`, 10 `a2pic.cpp`),
  datées sur les 41 et 38 révisions amont et relues à la main. S'y ajoutent
  les citations hors motif : 11 numéros nus (8 cibles, dont 5 que le motif
  ne contient pas), 3 dans `tests/`, 4 dans
  `DEV.md`, 4 dans `docs/printer-detection.md`, une izapple2 et quatre
  adresses de ROM (grade C).
- **Verdicts (21 sous `src/`)** :
  - `a2videoterm.cpp` : OK 8, OK (rev `ed697c3087`) 3, DÉCALÉE 0, INEXACTE 0 ;
  - `a2pic.cpp` : OK 7, DÉCALÉE 2 (`AppleParallelCard.cpp:65`, plage tronquée ;
    `.cpp:105`, juste sur **aucune** révision), INEXACTE 1 (`.h:72`, SW6
    « au reset » sur des lignes qui ne le disent pas, alors que MAME et le
    silicium l'appliquent tout de suite) ;
  - ABSENTE 0 · NON-PINNÉE 0.

  Hors motif : tout est OK ou OK (rev `ed697c3087`). La citation izapple2 est
  juste, mais non pinnée.
- **Constats** : CRITIQUE 0 · MAJEUR 0 · MINEUR 0. Trois divergences latentes
  (pistes 1-3) :
  - le décodage RS = A0 du 6845 de la Videoterm, que le schéma met en doute ;
  - la banque VRAM remise à 0 au Ctrl-Reset, contradiction interne avec
    `GrapplerCard` ;
  - le SW6 de la PIC appliqué au seul reset.
- **AMONT FAUX** : 2.
  - izapple2 dessine des cellules de 8 points au lieu de 9 ;
  - le tag `mame0287` sert la VRAM en miroir à `$CE00-$CFFF` (PLAUSIBLE, déjà
    corrigé sur `master`).
- **Révisions de référence et pins** :
  - **PIC** : pin déclaré par `docs/printer-detection.md` (MAME `master`
    2026-09-26, soit `ed697c3087` pour `a2pic.cpp`), **à jour**, car `master`
    n'a pas bougé depuis le 2026-08-12 ;
  - **Videoterm** : aucun pin. Verdicts contre `mame0287` (= `0cfd9e0b95`),
    mais le code suit **`ed697c3087`**, sur une seule époque. Le tableau de pin
    à écrire est en § Révision amont.

  Les deux fichiers sont donc homogènes, contrairement à `iwm.cpp` ou à
  `GrapplerClassicCard`. Seules les deux citations de reset de la PIC ne
  collent à aucune révision.
- **Montées en grade** : 9 des 10 citations PIC et 6 des 11 citations Videoterm
  reçoivent une ligne B ou C, et une septième (`VidexVideotermCard.cpp:81`)
  une réserve F :
  - PIC : manuel tables 3-1, A-5 et A-6, schéma 050-0046-A, firmware
    341-0057 ;
  - Videoterm : manuel p. 4-14 à 4-16, schéma V8010, firmware v2.4, manuel du
    Soft Video Switch.
- **Ce que je n'ai pas pu vérifier** :
  - **la source de E (broche 23) du 6845 de la Videoterm** : la piste 1 en
    dépend tout entière. Il manque aussi les chapitres 5-6 du manuel Videoterm
    (« Device Select Operation », « Memory Mapping », « Theory of Operation »),
    absents du scan de 72 pages ;
  - la sortie exacte du 74LS139 (U9) qui valide la RAM (AMONT FAUX 2) et le
    câblage des entrées S/R des bascules 4013 au-delà de ce qui est lisible
    (piste 2) ;
  - le chemin de l'horloge du 74LS273 de la PIC : je n'ai pas établi par le
    schéma qu'une **lecture** de `$C0n0` ou `$C0n2` ne strobe pas. MAME et POM2
    l'excluent, et le manuel dit « on write » pour `$C080` seulement ;
  - l'étiquette du strobe auxiliaire (broche 13 : « 500 ns » chez MAME), mal
    lisible sur le scan ;
  - le réglage d'usine des cavaliers X5/X6 de la PIC (7 ou 8 bits) ;
  - « AppleWin issue #105 » et la phrase du manuel Videoterm qui imposerait le
    slot 3 (je n'ai vu que le listing #3 qui suppose `SLOT=3`) ; « Epson APL et
    Fourth Dimension byte-identical » (`AppleParallelCard.h:24-25`) ;
  - je n'ai pas relancé `ctest` en entier, seulement les trois binaires de test
    voisins déjà construits ;
  - les tags `mame0287` et `master` ont été identifiés par MD5 des fichiers ;
    l'API GitHub a refusé (limite de débit) la requête de date du tag.
