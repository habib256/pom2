# Audit de parité — citations de MAME `a2memexp.cpp` (2026-11-01)

Tranche `a2memexp.cpp` du brief `docs/audits/BRIEF-parite-amont.md` : l'Apple
II Memory Expansion Card (670-0024, « Slinky »), `src/SlinkyCard.{h,cpp}`,
DEV.md § Memory Expansion Card. Toutes les citations `a2memexp.cpp:<lignes>` :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ tests/ \
  --include='*.cpp' --include='*.h' | grep 'a2memexp\.cpp:'
```

**15 citations** : 12 sous `src/` (5 dans `SlinkyCard.h`, 7 dans
`SlinkyCard.cpp`) et 3 dans `tests/slinky_card_test.cpp`. S'y ajoutent
**11 citations nues** (`:129`, `:63`…, sans nom de fichier, que le grep du
brief ne voit pas) : 6 dans `SlinkyCard.{h,cpp}` et 5 dans DEV.md. Elles sont
examinées aussi et comptées à part.

Numéros POM2 : commit **`af7dd56f`** (« Audit reports for slices 4-14… »).
`SlinkyCard.{h,cpp}` et `slinky_card_test.cpp` n'ont pas bougé depuis
`41965917` (2026-10-21). DEV.md avait des modifications locales non commitées
pendant l'audit ; je le cite par section, pas par ligne. Aucun fichier du dépôt
n'a été modifié. Les sondes, scripts et téléchargements sont sous
`/tmp/audit_a2memexp/`.

## Sources

**Grade B, documents primaires** :

- *Apple II Memory Expansion Card — User's Manual* (Apple, 030-1209-B, ©1985),
  Apple II Documentation Project (`Interface Cards/Memory/Apple II Memory
  Expansion Card/Manuals/`). PDF image, passé à l'OCR (tesseract). Utile :
  p. 1 (« comes with 256K RAM, but it can be expanded to 512K, 768K, or 1
  megabyte »), annexe A (autotest, `Cn0AG`), annexe C (trois rangées de huit
  puces, repères (1), (2), (3)), annexe D (l'autotest affiche « CARD OK » et
  la taille, 256K / 512K / 768K / 1 MEG). Le manuel renvoie à un *reference
  manual* de la carte //e que je n'ai pas trouvé en ligne.
- *Apple II Technical Notes — Memory Expansion Card #1: Questions and
  Answers* (C. Birse, 04/1986, révisée M. Askins & M. Deatherage, 11/1988),
  même dossier A2DP (`MemExp #001 Q&A.pdf`, couche texte). Les trous d'écran
  (`$478+n` = nombre de bancs de 64K, `$04` pour 256K, `$08` pour 512K),
  « RESET turn[s] off the Memory Expansion Card registers until an access to
  the $Cn00 space », le reformatage à tout accès firmware, le bug `CMP #4` au
  lieu de `CPY #4` en `$Cn5A`.
- *Apple IIc Memory Expansion Card Reference Manual* (Apple, 1988),
  archive.org `apple-2c-memory-expansion-card-reference-manual`, chap. 2
  « Hardware », pp. 5-7 : « a **20-bit address register** and a 1-byte data
  register » ; `$C0C2` = « high nibble (D3 to D0) of the address register » ;
  « The address pointer must be loaded in the following order: low byte,
  middle byte, high byte » ; quatre rangées de huit 256K×1, une matrice
  CMOS (« gate array »), une PAL 16L8, un 74HCT245 ; DRAM rafraîchie toutes
  les 4 ms par la matrice. C'est la carte sœur pour le //c ; elle n'est pas
  la 670-0024, d'où la prudence ci-dessous.
- *Applied Engineering RamFactor User's Manual* v1.5, chap. 6 « RamFactor
  Hardware », p. 45 (A2DP, `AE RamFactor/Manuals/`, couche texte). La
  RamFactor est le clone compatible registre de la carte Apple (même signature
  `$Cs00: C9 20 C9 00 C9 03 C9 00`, p. 45). Le texte : registres désactivés
  après mise sous tension **ou** Control-Reset jusqu'à « any address in the
  firmware page » ; poids fort lu « always F » jusqu'à 1 Mo ; « Whenever the
  lower or middle address byte changes from a value with bit 7 = 1 to one
  with bit 7 = 0, the next higher byte is automatically incremented. This
  means that you should always load the bytes in the order low-middle-high,
  and always load all three of them. (Unless, of course, you are sure of the
  previous contents […]) » ; base indexée dans une autre page pour éviter les
  doubles incréments.

**Grade C** : la ROM **341-0344A** elle-même (`roms/slinky_341-0344a.bin`,
SHA-1 `6e823a1f…`, CRC32 `1e994e17`, identique octet pour octet à l'image de
l'A2DP et au `ROM_LOAD` de MAME). Désassemblée avec `da65` : page `$C500`
(`rom[$500]`), fenêtre `$C800` (`rom[$800]`) et l'autotest relogé en `$2000`
(`rom[$CF0-$FEF]`, copié par `$C836`).

**Grade D** : MAME, **les 35 révisions** du fichier sur tout son historique :
`src/mess/machine/a2memexp.c` (2012-08 → 2013-10), `src/emu/bus/a2bus/a2memexp.c`
(2013-10 → 2015-09), `src/devices/bus/a2bus/a2memexp.c(pp)` depuis (24 commits
de `7c19aac60e` à `f135a4e417`), plus `mame/apple/apple2e.cpp@mame0287` pour la
mémoire d'extension intégrée du //c. izapple2 `master`,
`cardMemoryExpansion.go` (`9e89f0a23f`, 2024-03-24). **AppleWin ne modélise
pas la carte** : aucune entrée dans `source/Card.h` (`CT_*`), aucun fichier
`source/` qui en parle.

## Révision amont

**Pin dans POM2** : `SlinkyCard.cpp:18`, « (mamedev/mame master, 2026-10) ».
Pas de hash. Il est **encore juste** : le `master` du 2026-11-01 et le tag
`mame0287` sont identiques octet pour octet à `f135a4e417` (AJR, 2025-11-10,
« a2bus: Interface refinements »), dernier commit à toucher le fichier.

Datation : une ancre regex par citation (le code décrit, pas son numéro),
une révision « tombe juste » quand la plage contient toutes les ancres
(`/tmp/audit_a2memexp/date.py`).

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste |
|---|---|---|---|
| `7285b359d2` → `cd6f73b9c7` (5) | 2012-08 → 2013-10 | MESS, `src/mess/machine/a2memexp.c`, `m_slot` | 0 |
| `cd6f73b9c7` → `f88cefad27` (7) | 2013-10 → 2015-09 | `src/emu/bus/…`, en-tête de licence ajouté en 2015-05 | 0 |
| `7c19aac60e` → `97b6717027` (12) | 2015-11 → 2019-03 | renommage `.cpp`, `tiny_rom_entry`, mconfig en méthode | 0 |
| `c2c3d685f0`, `48933ef893` | 2021-10, 2024-09 | nettoyage, `ATTR_COLD` | 2 (`h:43`, `h:96` : la `ROM_START` est en 23-26 depuis 2021-10) |
| **`f135a4e417`** = `mame0287` = `master` | 2025-11-10 | `take_c800() { return true; }` explicite, `slotno()`, classes dans un namespace anonyme | **les 15** (et les 11 nues) |
| aucune | — | — | 0 |

**Une seule époque.** Rien n'est daté ailleurs que sur `f135a4e417` ; les
deux citations de la `ROM_START` tombent juste depuis 2021-10 parce que ces
lignes n'ont plus bougé. Le comportement porté (registres, reset, ROM) n'a pas
changé depuis 2012 : seules la plomberie et la règle `take_c800` (opt-in
explicite depuis `f135a4e417`) ont évolué.

**Pin à écrire** (tableau, modèle DEV.md § Zilog Z8530 SCC) :

| Commit amont | Pris ? |
|---|---|
| `f135a4e417` — `a2bus_memexpapple_device` (registres `$C0n0-$C0nF`, masque `$FFFFF`, `m_bankhior = $F0`, `read_cnxx`/`read_c800`, `take_c800` opt-in) | pris |
| `f135a4e417` — `a2bus_ramfactor_device` (8 Mo, ROM 8 Ko banquée par `$C0nF`) | non pris : aucune ROM RamFactor livrée |
| le verrou `m_wptr` distinct du pointeur `m_liveptr` (présent depuis 2012) | pris, **et faux** au regard des documents (constat 1) |
| `device_reset` qui remet `m_regs` à zéro, octet haut compris (depuis 2012) | pris, **et faux** sur le quartet haut (constat 2) |
| (absent de MAME) registres coupés au RESET jusqu'à un accès `$Cn00` | **ajouté par POM2**, TN MemExp #1 (AMONT FAUX 4) |

---

## Rapport 1 — Citations

Toutes **OK** sur `mame0287` (= `f135a4e417` = `master`). Pas de DÉCALÉE,
pas d'INEXACTE au sens strict : les lignes disent ce que POM2 leur fait
dire. Les remarques portent sur ce que POM2 **ajoute** autour de la
citation. La dernière colonne propose la ligne à écrire ; quand la ROM ou un
manuel disent la même chose, la citation monte en grade.

| Fichier POM2:ligne | Citation | Verdict | Commentaire | Ligne corrigée proposée |
|---|---|---|---|---|
| `src/SlinkyCard.h:25` | `a2memexp.cpp:166-232` | OK | `read_c0nx` (166) à la fin de `write_c0nx` (232). Le tableau qui suit est fidèle à MAME ; deux de ses affirmations sont fausses pour le silicium (constats 1 et 2). | `// [B] Apple IIc Memory Expansion Card Reference Manual (1988) ch. 2 pp. 5-7 : un registre d'adresse de 20 bits auto-incrémenté, $C0n2 = quartet D3-D0, données en $C0n3, chargement bas → milieu → haut. [B] RamFactor User's Manual v1.5 p. 45 (clone registre). [D] MAME a2memexp.cpp:166-232 @ mame0287 (verrou m_wptr séparé : voir constat 1).` |
| `src/SlinkyCard.h:43` | `a2memexp.cpp:25` | OK | `ROM_LOAD( "341-0344a.bin", 0x0000, 0x1000, CRC(1e994e17) …)`. Stable depuis 2021-10. | `// [C] 341-0344A (4 Ko, CRC32 1e994e17, SHA-1 6e823a1f…), identique à l'image de l'A2DP. [D] MAME a2memexp.cpp:23-26 @ mame0287.` |
| `src/SlinkyCard.h:94` | `a2memexp.cpp:130` | OK | `m_addrmask = 0xfffff`. | `// [B] IIc MemExp Ref. Manual ch. 2 p. 5 : registre de 20 bits, 1 Mo au plus. [D] MAME a2memexp.cpp:130 @ mame0287.` |
| `src/SlinkyCard.h:95` | `a2memexp.cpp:129` | OK | `m_bankhior = 0xf0`. | `// [B] RamFactor Manual v1.5 p. 45 : « always "F" when you read it » ; IIc MemExp Ref. Manual p. 5 : $C0n2 ne porte que D3-D0. [C] 341-0344A, autotest $2050 : ORA #$F0 / CMP $C0n2. [D] MAME a2memexp.cpp:129 @ mame0287.` |
| `src/SlinkyCard.h:96` | `a2memexp.cpp:24-25` | OK | `ROM_REGION(0x1000, …)` + `ROM_LOAD`. Stable depuis 2021-10. | `// [D] MAME a2memexp.cpp:24-25 @ mame0287 (région de 4 Ko).` |
| `src/SlinkyCard.cpp:46` | `a2memexp.cpp:155-159` | OK | `device_reset` : `memset(m_regs, 0…)`, `m_wptr = m_liveptr = 0`. POM2 y ajoute la coupure des registres (TN MemExp #1), juste. Mais remettre `m_regs[2]` à `$00` fait lire `$00` au lieu de `$Fx` (constat 2). | `// [B] TN Memory Expansion Card #1 (11/1988) : RESET coupe les registres jusqu'au prochain accès $Cn00 ; RamFactor Manual p. 45 : « power up or Control-Reset ». [D] MAME a2memexp.cpp:155-159 @ mame0287 (pas de coupure ; quartet haut remis à 0, faux).` |
| `src/SlinkyCard.cpp:55` | `a2memexp.cpp:175-177, 195-197, …` | OK | 175-177 : `m_regs[0..2]` depuis `m_liveptr` ; 195-197 : depuis `m_wptr`. L'affirmation « The three pointer bytes **always** mirror a pointer […] forced high nibble » est fausse après un reset (constat 2). | `// [B] RamFactor Manual p. 45 / IIc MemExp Ref. Manual p. 5 (voir h:95). [D] MAME a2memexp.cpp:175-177, 195-197 @ mame0287.` |
| `src/SlinkyCard.cpp:63` | `a2memexp.cpp:166-181` | OK | `read_c0nx` entier. | `// [B] IIc MemExp Ref. Manual ch. 2 pp. 5-7. [D] MAME a2memexp.cpp:166-181 @ mame0287.` |
| `src/SlinkyCard.cpp:77` | `a2memexp.cpp:188-232` | OK | `write_c0nx` entier. Le commentaire décrit exactement l'asymétrie de MAME (« the latch keeps the START of a transfer ») — c'est elle que les documents contredisent (constat 1). | `// [B] IIc MemExp Ref. Manual ch. 2 : UN registre de 20 bits ; RamFactor Manual p. 45 : écrire un octet ne touche que lui, le reste garde la valeur COURANTE, et un bit 7 qui tombe propage une retenue. [D] MAME a2memexp.cpp:188-232 @ mame0287 : le verrou m_wptr est une construction de MAME.` |
| `src/SlinkyCard.cpp:104` | `a2memexp.cpp:238-247` | OK | `read_cnxx`. POM2 cite `m_rom[offset + slotno() * 0x100]` ; le code est `int const slotimg = slotno() * 0x100; … m_rom[offset+slotimg]` (et `m_slot * 0x100` avant 2025-11) : paraphrase fidèle. | `// [C] 341-0344A : sept pages $Cn (rom[n·$100]) qui ne diffèrent que par les dix octets codant le slot ($Cn1A, $Cn1C, $Cn40, $Cn4A, $Cn50, $Cn53, $Cn6F, $Cn71, $CnE4, $CnEE). [D] MAME a2memexp.cpp:238-247 @ mame0287.` |
| `src/SlinkyCard.cpp:120` | `a2memexp.cpp:253-260` | OK | `read_c800` : `m_rom[offset+0x800]`. | `// [C] 341-0344A : l'entrée $Cn fait STY $07F8 ; BIT $CFFF ; JSR $C800 — la fenêtre est rom[$800-$FFF]. [D] MAME a2memexp.cpp:253-260 @ mame0287.` |
| `src/SlinkyCard.cpp:135` | `a2memexp.cpp:149-152` | OK | Les quatre `save_item`. | `// [D] MAME a2memexp.cpp:149-152 @ mame0287.` |
| `tests/slinky_card_test.cpp:19` | `a2memexp.cpp:166-232` | OK | Comme `h:25`. Le test épingle l'asymétrie de MAME (l. 79-84) : il faudra l'inverser si le constat 1 est corrigé. | comme `h:25` |
| `tests/slinky_card_test.cpp:60` | `a2memexp.cpp:147` | OK | `memset(m_ram, 0xff, 1024*1024…)`. La valeur réelle d'une DRAM à la mise sous tension n'est documentée nulle part : `$FF` est le choix de MAME. | `// [D] MAME a2memexp.cpp:147 @ mame0287. [F] Contenu d'une DRAM 41256 à froid inconnu ; le firmware n'en dépend pas (il formate).` |
| `tests/slinky_card_test.cpp:86` | `a2memexp.cpp:130` | OK | Comme `h:94`. | comme `h:94` |

**Citations nues** (hors grep du brief), toutes OK sur `mame0287` :

| Fichier POM2:ligne | Citation | Verdict | Commentaire |
|---|---|---|---|
| `src/SlinkyCard.h:31` | `:129` | OK | `m_bankhior`. Mais « — **what firmware sizes it by** » est faux : la ROM dimensionne par une écriture / relecture de `$A5` en `bank:0000`, du banc `$F` vers le bas (`$CAB1-$CAEE`), et masque `$C0n2` par `AND #$0F`. Ligne : `// [C] 341-0344A $CAB1 : dimensionnement par $A5 écrit/relu en bank:0000, du banc $F vers le bas ; le quartet haut est masqué (AND #$0F).` |
| `src/SlinkyCard.h:36` | `:130` | OK | « how a partly populated board aliases is not in any source POM2 has » n'est plus vrai : voir la piste « rangées absentes ». « 256 KB / 512 KB » oublie 768K, que les deux manuels listent. |
| `src/SlinkyCard.h:45` | `:238-247`, `:253-260` | OK ×2 | Comme `.cpp:104` et `.cpp:120`. Le plan de la ROM qui suit (page 0 = « easter egg », sept pages, `$800-$FFF` en `$C800`) est **exact** (vérifié sur la ROM : la page 0 est le texte « This wonderful, wonderful rom is brought to you by Richard Williams… »). |
| `src/SlinkyCard.h:51` | `:63` | OK | `take_c800() const override { return true; }`, explicite depuis `f135a4e417`. POM2 rend `romLoaded_` : sans ROM, la carte ne sert pas la fenêtre et ne la prend pas (règle `take_c800` de CLAUDE.md) ; via `SlotCardFactory` la carte n'existe pas sans ROM. VOULUE. Ligne : `// [C] 341-0344A : BIT $CFFF ; JSR $C800 depuis la page $Cn. [D] MAME a2memexp.cpp:63 @ mame0287.` |
| `src/SlinkyCard.cpp:34` | `:147` | OK | Comme `test:60`. |
| DEV.md § Memory Expansion Card | `:166-232`, `:130`, `:238-247`, `:253-260`, `:155-159` | OK ×5 | Mêmes remarques que les citations correspondantes ; « no size setting — how a partly populated card aliases is not sourced » est à reprendre (piste « rangées absentes »). |

---

## Rapport 2 — Constats

### 1. Écrire un seul octet d'adresse recharge les deux autres depuis un verrou fantôme, pas depuis le registre courant

```
Fichier      : src/SlinkyCard.cpp:81-91 (deviceSelectWrite, cases 0-2), :77-80 (commentaire)
Amont        : a2memexp.cpp:188-217 @ mame0287 (m_wptr) — présent depuis 7285b359d2 (2012)
Gravité      : MINEUR
Confiance    : PLAUSIBLE — POM2 confirmé par sonde ; le comportement réel s'appuie
               sur deux documents B qui décrivent la carte //c et le clone AE,
               pas la 670-0024 elle-même (aucune mesure sur la carte).
```

**Divergence.** MAME, et POM2 à sa suite, gardent deux pointeurs : `m_wptr`,
qui ne change qu'aux écritures de `$C0n0-$C0n2`, et `m_liveptr`, que le port
de données incrémente. Écrire **un** octet d'adresse prend les deux autres
dans `m_wptr`, c'est-à-dire dans l'adresse de **début** du dernier transfert,
et y recopie le pointeur vivant. POM2 le dit et l'épingle
(`SlinkyCard.cpp:77-80`, `slinky_card_test.cpp:79-84`).

Les documents décrivent un seul registre :

- [B] *Apple IIc Memory Expansion Card Reference Manual*, ch. 2 p. 5 : « a
  20-bit address register […] Because the address register automatically
  increments after each access of the data register » ; p. 7 : « an auto
  increment 20-bit address pointer ». Un registre, que les écritures chargent
  octet par octet et que l'accès aux données incrémente.
- [B] *RamFactor User's Manual* v1.5 p. 45 : « Whenever the lower or middle
  address byte changes from a value with bit 7 = 1 to one with bit 7 = 0, the
  next higher byte is automatically incremented. This means that you should
  always load the bytes in the order low-middle-high, and always load all
  three of them. (**Unless, of course, you are sure of the previous
  contents** and can be sure you will get predictably correct results.) »
  Ce conseil n'a de sens que si l'octet non réécrit garde sa valeur
  **courante** : c'est un compteur en ondulation, où la retenue vers l'octet
  supérieur est le front descendant du bit 7 de l'inférieur, qu'il vienne de
  l'incrément **ou d'une écriture**.

izapple2 (D) modélise un registre unique, sans verrou ; il n'a pas non plus
la retenue sur écriture.

**Scénario.** L'idiome « relire la même page » : charger `$000000` (trois
octets), lire 256 octets par `$C0n3`, puis `LDA #$00 / STA $C0n0` pour
recommencer la page. Le pointeur vivant vaut `$000100` ; réécrire l'octet bas à
`$00` ne fait pas tomber de bit 7.

- Silicium (un registre) : `$000100`, la page **suivante**.
- POM2 / MAME : `$000000`, la **même** page, rechargée depuis le verrou.

Le programme lit 256 octets d'une autre page que sur la machine réelle. Même
chose pour toute écriture partielle après qu'un transfert a franchi une
frontière de 256 octets ou de 64 Ko. À l'inverse, POM2 ne propage pas la
retenue d'un bit 7 qui tombe sur écriture (`$C0n0 : $80-$FF → $00-$7F`
incrémente l'octet du milieu sur le silicium d'après le manuel AE).

**Portée.** Le firmware Apple **n'est pas touché** : la sonde fait formater la
carte par la vraie ROM (ProDOS, Pascal, DOS 3.3, 6502 et 65C02) sous trois
modèles — POM2, registre 20 bits, registre 20 bits + retenue sur bit 7 — et la
RAM de la carte est **identique octet pour octet** dans les trois (empreintes
FNV `B66CCC48`, `CFBABE56`, `D04823FA`). La ROM fait pourtant des écritures
partielles à chaque formatage : l'octet bas seul 3 fois sur 65C02 (6 sur
6502, où `DEC` écrit deux fois), l'octet du milieu seul une fois (`DEC $C0n0`
du dimensionnement, `STA $C0n0 #$44` du VTOC DOS, `STA $C0n1` de la table du
répertoire). Apple a placé chacune là où les trois modèles coïncident. L'écart ne touche donc que du
code qui pilote la carte lui-même **et** enfreint la consigne « always load
all three ». Je n'en connais aucun ; d'où MINEUR.

**Observation.** `/tmp/audit_a2memexp/probe` section [2] :

```
STA $C0n0 #$00 : POM2 pointe $00000 et lit $A0 ; registre 20 bits pointe $00100 et lit $B0
```

Section [1] : formatage identique sous les trois modèles (sortie complète en
fin de rapport).

### 2. Après un RESET, `$C0n2` se relit `$00` au lieu de `$Fx`

```
Fichier      : src/SlinkyCard.cpp:47-52 (onReset : regs_.fill(0)) ; idem à la construction (regs_{})
Amont        : a2memexp.cpp:155-159 @ mame0287 (memset(m_regs, 0, …)), depuis 2012
Gravité      : MINEUR
Confiance    : CONFIRMÉ (POM2 par sonde ; quartet haut documenté B et exigé par la ROM, C)
```

**Divergence.** `publishPointer` force le quartet haut de `$C0n2` à `F`, mais
seulement quand le pointeur bouge. `onReset` (et le constructeur) mettent
`regs_[2]` à `$00` : jusqu'à la première écriture de pointeur ou le premier
accès aux données, `$C0n2` se lit `$00`. MAME a le même défaut, pour la même
raison (`m_regs[2]` remis à zéro sans `| m_bankhior`).

Sur la carte, ces quatre bits n'existent pas : [B] IIc MemExp Ref. Manual p. 5,
« high nibble (D3 to D0) of the address register », registre de 20 bits ;
[B] RamFactor Manual p. 45, « The top nybble can be any value when you write
it, but it will **always** be "F" when you read it » (cartes ≤ 1 Mo). [C] La
ROM 341-0344A compte dessus : l'autotest relogé (`$2050`) fait `ORA #$F0 /
CMP $C0n2` et déclare « ADDRESS ERROR » sinon.

**Scénario.** Ctrl-Reset (ou mise sous tension), le scan de boot touche
`$Cn00` et réactive les registres, puis un programme identifie la carte en
lisant `$C0n2` et en testant `AND #$F0 = #$F0` (signature naturelle d'une
carte ≤ 1 Mo, celle que le manuel AE décrit) : POM2 lit `$00` et conclut
qu'il n'y a pas de carte ; la vraie carte rend `$F0`-`$FF`. La valeur du
compteur lui-même après un RESET (remis à zéro ou conservé) n'est documentée
nulle part [F] ; seul le quartet haut fait l'objet du constat.

**Observation.** Sonde section [3] :

```
avant RESET : $C0n2=$F5
après RESET + LDA $C500 : $C0n0/1/2 = $00 $00 $00
```

### 3. Un cycle d'alimentation (coldBoot) laisse le mégaoctet de la carte intact

```
Fichier      : src/SlinkyCard.h:121 (onReset seul, pas d'onPowerCycle) ; SlinkyCard.cpp:47-52
Amont        : a2memexp.cpp:145-153 @ mame0287 (la RAM n'est remplie qu'à device_start)
Gravité      : MINEUR
Confiance    : CONFIRMÉ (sonde) ; le contenu exact d'une DRAM après coupure est [F]
```

**Divergence.** `coldBoot` est le cycle d'alimentation de POM2 :
`SlotBus::reset(/*powerCycle=*/true)` appelle `onPowerCycle()` de chaque carte
(`EmulationController::coldBoot`, `Pom2Core::coldBoot` : « A POWER cycle […]
every card's onPowerCycle() runs »). `SlinkyCard` ne surcharge pas
`onPowerCycle`, dont le défaut est `onReset()` : les latches tombent, **la RAM
reste**. Le dépôt applique ailleurs la règle inverse pour la même physique :
`EzColorCard::onPowerCycle` — « Slot power off and on: the DRAM forgets. » —
et `Memory::clearRam` repeint la RAM principale, auxiliaire et RamWorks. Or la
carte est de la DRAM pure, rafraîchie par la matrice toutes les 4 ms ([B] IIc
MemExp Ref. Manual p. 6 ; le manuel //e annonce une carte sans pile) : sans
alimentation, plus de rafraîchissement, plus de données.

**Scénario.** Copier un fichier sur `/RAM5`, bouton Power (ou
`/reset?kind=cold`), redémarrer ProDOS. Le firmware reformate bien la carte
(les trous d'écran `$0438+n` / `$04B8+n` sont repeints en `00 FF` par
`clearRam` : pour un même slot ils ont la même parité, donc la même valeur
`v`, et `v ⊕ $A5 = v` est impossible), mais seuls le répertoire et la bitmap
sont réécrits. Un lecteur de blocs (READ_BLOCK sur un bloc libre, utilitaire de
récupération) ou un programme qui lit la carte par ses registres retrouve
l'ancien fichier, intact, après une coupure de courant. Côté hôte, chaque
image du ring de rewind continue de porter ces pages (jusqu'à 1 Mo).

**Observation.** Sonde section [4] :

```
octet $034000 écrit $5A avant le cycle, relu après : $5A
```

### 4. AMONT FAUX — MAME n'éteint pas les registres au RESET

```
Fichier      : src/SlinkyCard.cpp:47-52, :66, :83, :107 ; SlinkyCard.h:114, :134-139
Amont        : a2memexp.cpp:155-159, 166-232 @ mame0287
Gravité      : —
Confiance    : CONFIRMÉ (deux documents B concordants)
```

MAME laisse `read_c0nx` / `write_c0nx` actifs après `device_reset`. [B] TN
Memory Expansion Card #1 : « Why does RESET turn off the Memory Expansion
Card registers until an access to the $Cn00 space? […] to optimize speed and
the number of pins and logic on the custom gate array. The boot scan hits
$Cn00 anyway » ; [B] RamFactor Manual p. 45 : « After power up or
Control-Reset, the registers on the card are all in a disabled state. They
will be enabled by addressing any address in the firmware page ». POM2
modélise la coupure (lecture = bus flottant, écriture ignorée) et le réveil
par une lecture **ou** une écriture de `$Cn00-$CnFF`. **À garder** : un
« alignement sur MAME » ferait accepter des écritures de pointeur qu'une vraie
carte ignore après un Ctrl-Reset.

### 5. AMONT FAUX — le commentaire de MAME sur le plan de la ROM

```
Fichier      : src/SlinkyCard.h:45-51
Amont        : a2memexp.cpp:242 @ mame0287
Gravité      : —
```

MAME écrit « first 0x400 of ROM contains a CnXX image for each of slots 1-7,
last 0x400 is c800 image ». La ROM fait 4 Ko : les pages `$Cn` occupent
`$000-$7FF` (`$100-$7FF` pour les slots 1-7, page 0 = texte de l'auteur) et la
fenêtre `$C800` `$800-$FFF`, ce que le code de MAME (`slotno()*0x100`,
`+0x800`) et l'en-tête de POM2 disent correctement. Commentaire seulement ; à
ne pas recopier si quelqu'un réécrit l'en-tête « d'après MAME ». (Même
fichier, ligne 255 : « bit 1 of cn0F » alors que le code teste `& 0x01` ; sans
objet chez POM2, qui ne modélise pas la RamFactor.)

---

## Pistes examinées et écartées

- **La vraie ROM tourne juste sur POM2.** L'autotest d'Apple (`$Cn0A`, manuel
  annexes A et D), 400 M cycles, 6502 et 65C02 : « CARD SIZE = 1 MEG »,
  « PASSES = 0003 », « CARD OK ». Il éprouve la relecture des trois octets
  (quartet haut à F), la retenue de `$FFFFFF` à `$000000` par `DEC $C0n0` +
  lecture + écriture, les lignes d'adresse une à une et toute la RAM. Les
  appels ProDOS STATUS / WRITE / READ passent déjà dans `slinky_card`.
- **Retenue et repli sur 20 bits** : `liveptr_` déborde au-delà de `$FFFFF`
  mais l'indice RAM est masqué et `$C0n2` se lit `((p>>16)&$FF)|$F0`, donc
  `$F0` à `$100000` comme sur un registre de 20 bits. Un `$FF` écrit en
  `$C0n2` vise le banc `$F`. Sain.
- **Fausses lectures du 6502** : la ROM n'adresse la carte que par
  `$BFF8-$BFFB,X` avec `X = $88 + n·$10`, donc toujours en franchissant une
  page. Sur NMOS la lecture parasite tombe en `$BFxx` (RAM), sur 65C02 il n'y
  en a pas ; `M6502::nmosIndexDummyRead` / `cmosIndexedWriteDummyRead`
  reproduisent les deux. `DEC $BFF8,X` (double écriture NMOS de l'ancienne
  valeur) ne change rien au résultat. Un `STA $C083,X` sans franchissement
  ferait un double incrément sur NMOS — POM2 le fait aussi, comme le silicium
  (le manuel AE p. 45 et le manuel //c p. 5 mettent en garde).
- **Rangées absentes : pas de miroir.** POM2 n'offre que 1 Mo (latent, pas un
  constat). Mais la question que l'en-tête dit « non sourcée » est tranchée :
  les manuels (B) vendent 256K / 512K / 768K / 1 Mo par rangées de huit puces,
  l'autotest affiche ces quatre tailles, et la ROM (C, `$CAB1`) dimensionne en
  écrivant `$A5` en `bank:0000` du banc `$F` vers le bas. Sonde [6] : rangées
  absentes → 512 / 1024 / 1536 blocs et `$047D` = `$04` / `$08` / `$0C`
  (les valeurs de la TN) ; rangées en **miroir** → 2048 blocs, « 1 MEG » sur
  une carte de 256K, ce que le manuel exclut. Une option de taille devrait donc
  laisser lire autre chose qu'un écho hors de la population (la valeur lue est
  inconnue [F] : bus non piloté). L'autotest passe à 256K et 768K sous ce
  modèle (sonde [5]).
- **`$C0n4-$C0nF`** : POM2 et MAME en font des latches lisibles ; izapple2 lit
  `$FF` et ignore l'écriture ; le manuel //c ne documente que `$C0C0-$C0C3`.
  Le décodage réel (A0-A1 seuls, donc miroir de `$C0n0-$C0n3` ?) n'est dans
  aucune source [F] ; la ROM n'y touche jamais. Pas de scénario sourcé. Se
  tranche sur une vraie carte.
- **`$C0nF` / banc de ROM** : propre à la RamFactor (`m_isramfactor`). La
  carte Apple n'a qu'une ROM de 4 Ko, non banquée ; POM2 garde `$C0nF` comme
  latch sans effet, comme MAME. Sain.
- **Pages `$Cn` et fenêtre `$C800`** : `rom[n·$100 + bas]` et
  `rom[$800 + (off & $7FF)]`, conformes à la ROM (C) et à MAME. La ROM arme la
  fenêtre par `BIT $CFFF ; JSR $C800`. `takesC800()` rend `romLoaded_` : sans
  dump la carte n'est jamais construite par `SlotCardFactory`. VOULUE, règle
  `take_c800` de CLAUDE.md.
- **Réveil des registres** : seul `$Cn00-$CnFF` les réveille (lecture ou
  écriture), pas la fenêtre `$C800`, conformément à la TN. Quand `INTCXROM`
  masque le slot, `Memory` n'appelle pas la carte, comme sur un //e où `/IOSEL`
  n'est pas émis. Sain.
- **Formatage selon l'OS** (TN : « any access to the firmware will cause it
  to format itself ») : POM2 exécute le vrai firmware, donc le choix
  ProDOS / Pascal / DOS 3.3 par `$BF00`, la garde des trous d'écran à `$A0`
  et le bug `CMP #4` de `$Cn5A` (TN) sont reproduits par construction.
- **//c et //c+ — une règle ou deux ?** POM2 n'a **aucune** mémoire
  d'extension intégrée : sur un //c-class, `$C0C0-$C0C3` tombe sur
  `IIcMouse::deviceSelectRead` → bus flottant, ce que fait aussi MAME quand le
  //c n'a pas de RAM d'extension (`memexp_r` → `read_floatingbus()`). Un //c+
  sans carte est une configuration réelle ; il n'y a donc pas deux règles dans
  POM2. **MAME, lui, en a deux** pour la même puce : la carte masque à
  `$FFFFF` (repli), le //c intégré (`apple2e.cpp:833-913 @ mame0287`) compare
  un pointeur de 24 bits à `m_exp_addrmask` et rend `$FF` au-delà, sans repli
  sur 20 bits (`$FF` écrit en `$C0C2` y vise `$FFxxxx`, hors RAM). Le manuel
  //c (B) donne raison au masque. À reporter si POM2 porte un jour la carte //c
  (`apple2c_mem`, ROM 3/4 et //c+), à ne pas copier tel quel.
- **Le //c ROM 3/4** mettrait la carte en slot 4 (`$C400`) et la souris en
  `$C700` (manuel //c, annexe A) ; les profils //c de POM2 ne sondent que les
  ROM 255 et 0, et le //c+ garde sa souris en `$C700`. Pas de conflit atteint.
- **Instantanés** : v1 → registres actifs, v2 porte le drapeau ; RAM en pages
  creuses. Le contrat est épinglé par `card_snapshot_contract` et le cas « carte
  pleine > 1 Mio » par `slinky_card`. Hors du périmètre MAME.
- **Contenu de la RAM à la mise sous tension** (`$FF`, MAME `device_start`) :
  [F], sans conséquence pour le firmware, qui formate à la première
  sollicitation. Pas de constat ; seul le **maintien** du contenu à travers un
  cycle d'alimentation en est un (constat 3).
- **AppleWin** ne modélise pas la carte : pas de seconde lecture de ce côté.

---

## Sortie complète de la sonde

`/tmp/audit_a2memexp/probe.cpp`, compilée contre `build/libpom2_core_test.a`
(construite à 20:50, après la dernière modification de `Memory.cpp`) :

```
clang++ -std=c++17 -O2 -Isrc -Itests -Ibuild/generated /tmp/audit_a2memexp/probe.cpp \
  build/libpom2_core_test.a -framework CoreAudio -framework AudioToolbox \
  -framework AudioUnit -framework CoreFoundation -o /tmp/audit_a2memexp/probe
POM2_MEDIA_WRITE_DEFAULT=protected /tmp/audit_a2memexp/probe          # [1]-[4]
POM2_MEDIA_WRITE_DEFAULT=protected /tmp/audit_a2memexp/probe size     # [6]
POM2_MEDIA_WRITE_DEFAULT=protected /tmp/audit_a2memexp/probe diag 400000000   # [5]
```

`POM2` = `pom2::SlinkyCard` tel quel ; `REG20` = un registre de 20 bits ;
`REG20B7` = `REG20` + retenue sur front descendant du bit 7 à l'écriture (manuel
AE). Machine : `Memory` + `M6502` en //e, `roms/apple2e.rom`, carte en slot 5.
Le compteur « lo/mi/hi » compte **toutes** les écritures de chaque octet
d'adresse ; l'excédent sur « hi » donne les écritures isolées.

```
[1] Formatage par le firmware (STATUS sur $CnFF), RAM de la carte comparée
  POM2    65C02 ProDOS STATUS C=0 A=$00 blocs=2048  taille($047D)=$10  RAM fnv=B66CCC48
  REG20   65C02 ProDOS STATUS C=0 A=$00 blocs=2048  taille($047D)=$10  RAM fnv=B66CCC48  écritures d octets d adresse lo/mi/hi=5/3/2
  REG20B7 65C02 ProDOS STATUS C=0 A=$00 blocs=2048  taille($047D)=$10  RAM fnv=B66CCC48  écritures d octets d adresse lo/mi/hi=5/3/2
  POM2    6502  ProDOS STATUS C=0 A=$00 blocs=2048  taille($047D)=$10  RAM fnv=B66CCC48
  REG20   6502  ProDOS … RAM fnv=B66CCC48  écritures d octets d adresse lo/mi/hi=8/3/2
  REG20B7 6502  ProDOS … RAM fnv=B66CCC48  écritures d octets d adresse lo/mi/hi=8/3/2
  POM2    65C02 Pascal … RAM fnv=CFBABE56   (REG20, REG20B7 : identiques, 5/3/2)
  POM2    6502  Pascal … RAM fnv=CFBABE56   (REG20, REG20B7 : identiques, 8/3/2)
  POM2    65C02 DOS3.3 … RAM fnv=D04823FA   (REG20, REG20B7 : identiques, 7/5/4)
  POM2    6502  DOS3.3 … RAM fnv=D04823FA   (REG20, REG20B7 : identiques, 10/5/4)

[2] Écriture d'UN octet d'adresse après un flux (idiome « relire la page »)
  après 3 octets à 0 puis 256 lectures : POM2 lit $F00100, REG20B7 $F00100
  STA $C0n0 #$00 : POM2 pointe $00000 et lit $A0 ; registre 20 bits pointe $00100 et lit $B0

[3] $C0n2 après RESET puis accès $Cn00 (aucune écriture de pointeur)
  avant RESET : $C0n2=$F5
  après RESET, avant $Cn00 : $C0n2=$00 (bus flottant attendu)
  après RESET + LDA $C500 : $C0n0/1/2 = $00 $00 $00

[4] Cycle d'alimentation (SlotBus::reset(powerCycle=true), = coldBoot)
  octet $034000 écrit $5A avant le cycle, relu après : $5A

[5] Autotest de la carte ($Cn0A, manuel annexes A et D)
  POM2    65C02 1024K : 400.0 M cycles, toujours en boucle
    |MEMORY CARD TEST
    |ESC TO EXIT
    |TEST WILL TAKE 180 SECONDS
    |CARD SIZE = 1 MEG
    |PASSES = 0003
    |… (points de progression)
    |CARD OK
  POM2    6502  1024K : idem, PASSES = 0003, CARD OK
  REG20B7 65C02 1024K : idem, PASSES = 0003, CARD OK
  REG20B7 65C02  768K : TEST WILL TAKE 135 SECONDS, CARD SIZE = 768K, PASSES = 0005, CARD OK
  REG20B7 65C02  256K : TEST WILL TAKE 45 SECONDS, CARD SIZE = 256K, PASSES = 0015, CARD OK

[6] Dimensionnement par la ROM ($CAB1) : rangées absentes vs rangées en miroir
   256K absentes : STATUS 512 blocs, $047D = $04
   256K miroir   : STATUS 2048 blocs, $047D = $10
   512K absentes : STATUS 1024 blocs, $047D = $08
   512K miroir   : STATUS 2048 blocs, $047D = $10
   768K absentes : STATUS 1536 blocs, $047D = $0C
```

(Sous [1], les lignes « identique à POM2 » ont été condensées ; la sortie brute
est dans `/tmp/audit_a2memexp/probe_out.txt`, celle de [5] dans
`diag_out.txt`. Le « bus flottant » de [3] vaut `$00` parce que la sonde n'a
pas de vidéo.)

---

## Bilan

- **Citations examinées** : 15 sur 15 (grep du brief), plus 11 citations nues
  (6 dans `SlinkyCard.{h,cpp}`, 5 dans DEV.md). Confrontées à
  `a2memexp.cpp@mame0287` et datées sur les **35 révisions** du fichier
  (2012-08 → 2025-11, trois chemins successifs).
- **Verdicts** : OK 15 / 15 (et 11 / 11 nues) ; DÉCALÉE 0, INEXACTE 0,
  OK (rev X) 0, ABSENTE 0, NON-PINNÉE 0. Les remarques portent sur les
  affirmations **autour** des citations : « what firmware sizes it by »
  (`h:31`, faux : la ROM dimensionne par écriture/relecture), « not in any
  source » pour les cartes partielles (`h:36-39`, DEV.md, désormais sourcé),
  « always » (`cpp:55`, faux après un reset), le verrou décrit en `cpp:77`.
- **Constats** :
  - CRITIQUE 0, MAJEUR 0 ;
  - MINEUR 3 :
    1. écrire un seul octet d'adresse recharge les autres depuis le verrou
       `m_wptr` de MAME au lieu du registre courant (PLAUSIBLE ; la ROM
       Apple n'est pas touchée) ;
    2. après un RESET, `$C0n2` se relit `$00` au lieu de `$Fx` (CONFIRMÉ) ;
    3. un cycle d'alimentation (coldBoot) laisse la RAM de la carte intacte,
       alors que POM2 efface ailleurs la DRAM au même événement (CONFIRMÉ) ;
  - AMONT FAUX 2 : MAME n'éteint pas les registres au RESET (TN MemExp #1) ;
    commentaire de MAME faux sur le plan de la ROM (`0x400` au lieu de
    `0x800`).
- **Révision de référence** : `mame0287` = `f135a4e417` (2025-11-10) =
  `master` au 2026-11-01. Le pin de `SlinkyCard.cpp:18` (« master, 2026-10 »)
  est **juste mais sans hash** ; une seule époque amont. Le pin à écrire est
  le tableau de « Révision amont ».
- **Montée en grade** : les registres passent de D à **B** (manuel //c chap. 2,
  manuel RamFactor p. 45), le reset à **B** (TN MemExp #1), le plan de la ROM
  et la fenêtre `$C800` à **C** (la ROM désassemblée), le quartet haut à
  **B + C** (autotest `$2050`).
- **Non vérifié** :
  - aucune mesure sur une vraie 670-0024. Les deux documents qui décrivent un
    registre unique et la retenue sur bit 7 portent sur la carte //c (même
    génération, matrice non identifiée) et sur le clone AE ; d'où PLAUSIBLE
    pour le constat 1 ;
  - le *reference manual* de la carte //e (cité par son manuel utilisateur)
    et son schéma : introuvables en ligne ; les photos de l'A2DP n'ont pas été
    exploitées pour tracer la matrice ;
  - ce que lit une rangée absente, ce que décodent `$C0n4-$C0nF`, et si le
    RESET remet le compteur à zéro : [F] ;
  - aucun logiciel réel connu n'a été trouvé qui écrive un pointeur partiel
    après un franchissement de page, ou qui lise `$C0n2` à froid ; AppleWorks
    et les pilotes GS/OS de la carte n'ont pas été désassemblés ;
  - le chemin DOS 3.3 n'a été exercé que par le formatage (`$BF00 = $33`),
    pas par un vrai `IN#n` sous DOS (le patch RWTS `$BD00 → JMP $CnDB`) ;
  - le comportement de MAME à son propre « hard reset » (reconstruction de la
    machine ou non) n'a pas été vérifié, et n'est pas invoqué au constat 3.
