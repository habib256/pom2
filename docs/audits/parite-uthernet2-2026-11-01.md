# Audit de parité — citations d'AppleWin `Uthernet2.cpp` (2026-11-01)

Tranche 7 du brief `docs/audits/BRIEF-parite-amont.md`. Dépôt amont : **AppleWin**
(`source/Uthernet2.cpp`), pas MAME — MAME n'a pas de W5100. Toutes les citations
`Uthernet2.cpp:<lignes>` sous `src/` :

```
grep -rnoE '[A-Za-z0-9_]+\.(cpp|h|lst):[0-9]+(-[0-9]+)?' src/ \
  --include='*.cpp' --include='*.h' | grep 'Uthernet2\.cpp:'
```

**46 citations** : 32 dans `W5100Device.cpp`, 9 dans `W5100Device.h`, 3 dans
`UthernetIICard.cpp`, 1 dans `UthernetIICard.h`, 1 dans `W5100NameResolver.h`.
Le grep du brief en rate **2** de plus, écrites sans nom de fichier à la suite
d'une première (`W5100Device.cpp:1774` « `+ :1527-1534` », `W5100Device.h:467`
« `, :1487-1525` ») ; elles sont datées avec les autres mais hors du compte.

Arbitre : la fiche technique WIZnet W5100 et le manuel de la carte. J'ai pu
récupérer la **datasheet W5100 v1.1.6** (WIZnet 2008, copie SparkFun) et le
**manuel Uthernet II du 17 nov. 2018** (a2RetroSystems, D. Finnigan — le même
document que POM2 cite), schéma compris (pp. 50-51 du PDF). La v1.2.8 que cite
POM2 n'est plus à l'URL de WIZnet (404) ; je n'ai pas pu la lire.

Aucun fichier de code n'a été modifié. Sonde et fichiers amont :
`/tmp/audit_uthernet2/` (sortie collée plus bas).

---

## Révision amont

**Pas de pin.** Ni l'en-tête de `W5100Device.h` (« Line citations below are
against AppleWin's `Uthernet2.cpp` »), ni `W5100Device.cpp`, ni
`UthernetIICard.*`, ni `DEV.md` § Uthernet II (« Citations in
`W5100Device.cpp` are AppleWin line numbers ») ne nomment une révision. Verdicts
rendus contre `master` d'AppleWin = `3e8054b462` (2026-07-26), dernier commit du
fichier au 2026-11-01.

Historique complet récupéré : **23 révisions** (API GitHub
`commits?path=source/Uthernet2.cpp`), datation scriptée (`date.py` : une ou
plusieurs ancres regex par citation, une révision « tombe juste » quand la plage
citée les contient toutes ; relu à la main pour les plages qui couvrent
plusieurs fonctions).

| Révision | Date | Ce qu'elle contient | Citations qui tombent juste (/46) |
|---|---|---|---:|
| `9f8c4d99af` → `500c2e50b6` (6 rév.) | 2022-02 → 2022-03 | Création, état sauvegardable, IPRAW, échec de connexion TCP | 0 |
| `6a5ea92a4e` | 2022-05-08 | DNS virtuel (`PTIMER = 0`, commentaire 32-37) | 5 (les trois `32-37`, les deux `119-181`) |
| `60cbbb40db` → `b3e617f1e5` (3 rév.) | 2022-05 → 2022-06 | Taille d'en-tête par protocole, source IPRAW, masque A0/A1 | 21 |
| `a85c9bfc07` → `6e8d79ddde`, `1127447a10` → `35f176e4d8` | 2024-03 → 2024-12 | `fcntl`, bus flottant `$Csxx`, DEVSEL actif sans NIC | 4 |
| `2e65426a11` | 2024-08-15 | Avertissement d'interface invalide (+5 lignes) | 6 |
| `cb1366f485` → `1a68914a4a` (3 rév.) | 2024-12 → 2025-09 | `_WIN32`, synchro Linux, avertissements x64 | 9 |
| **`80ec6e5545`** | 2026-03-07 | `ThrowErrorInvalidSlot` (+3 lignes au constructeur) | **46** |
| `926c592f75` | 2026-06-06 | `BOOL` → `bool` (substitutions ligne à ligne) | **46** |
| **`3e8054b462` = `master`** | 2026-07-26 | idem | **46** |

**Une seule époque, et c'est la bonne.** Les 46 citations (et les 2
continuations) tombent juste sur `80ec6e5545`, `926c592f75` et `3e8054b462`, qui
ont une numérotation identique et ne diffèrent que par des `!!`/`TRUE`. Aucune
ne tombe juste *seulement* sur une révision ancienne. Le port POM2 date du
2026-07-28 (`f7af7577`), deux jours après `3e8054b462` : il a été écrit contre
le `master` de l'époque, qui est encore le `master` d'aujourd'hui. C'est
l'inverse des tranches 1-3 (trois époques mêlées pour `iwm.cpp`, neuf pour
`apple2e.cpp`, pin périmé pour `z80scc.cpp`).

**Correctifs amont postérieurs** (leçon 2) : **aucun** — AppleWin n'a pas touché
`Uthernet2.cpp` depuis `3e8054b462`. Les correctifs *antérieurs* qui changent un
comportement sont tous dans POM2 :

| Correctif amont | Dans POM2 ? |
|---|---|
| `500c2e50b6` échec de connexion TCP → socket fermé | oui — `poll()`, CLOSED + TIMEOUT |
| `60cbbb40db` taille d'en-tête dans le calcul de place libre ; IPRAW écrit l'IP **source** | oui — `headerSize`, `receiveOnePacketIpRaw` |
| `b3e617f1e5` seuls A0/A1 décodés (`& 0x03`) | oui — `kRegisterMask` |
| `6e8d79ddde` `$Csxx` lit le bus flottant | oui — pas de ROM, défaut `SlotPeripheral` |
| `fd93396162` DEVSEL actif même sans interface réseau valide | oui — aucun registre n'est conditionné au backend |

Révision à épingler : `@ 3e8054b462`.

---

## Rapport 1 — Citations

**46 OK.** Aucune DÉCALÉE, ABSENTE, INEXACTE. Chaque plage commence sur la
première ligne de la fonction ou du bloc cité (vérifié par l'ancre de début) et
la contient en entier. La colonne « Ligne corrigée » donne la forme du dépôt ;
là où un document primaire porte la même règle, la citation monte en grade B et
la ligne AppleWin reste en second.

| Fichier POM2:ligne | Citation | Verdict | Commentaire | Ligne corrigée |
|---|---|---|---|---|
| `W5100Device.cpp:381` | `1362-1409` | OK | `Uthernet2::Reset`. | `// [B] W5100 datasheet v1.1.6 §4 — RTR 0x07D0, RCR 0x08, RMSR/TMSR 0x55, PTIMER 0x28, Sn_DHAR FF, Sn_TTL 0x80.` + `// [D] AppleWin Uthernet2.cpp:1362-1409 @ 3e8054b462 — Reset().` |
| `W5100Device.cpp:419`, `:1667`, `W5100Device.h:58` | `32-37` | OK | Commentaire « Virtual DNS … PTIMER = 0 ». Extension AppleWin : D est ici la source de définition, rien à monter. | `// [D] AppleWin Uthernet2.cpp:32-37 @ 3e8054b462 — DNS virtuel, PTIMER = 0 le signale.` |
| `W5100Device.cpp:450`, `W5100Device.h:365` | `212-234` | OK | `Socket::setStatus`, taille d'en-tête par état. | `// [B] W5100 datasheet v1.1.6 §5.2.2-5.2.4 — en-tête RX : UDP 8, IPRAW 6, MACRAW 2 octets.` + `// [D] … :212-234 @ 3e8054b462` |
| `W5100Device.cpp:498` | `910-945` | OK | `openSystemSocket`. | `// [D] AppleWin Uthernet2.cpp:910-945 @ 3e8054b462 — openSystemSocket().` |
| `W5100Device.cpp:566` | `947-1001` | OK | `openSocket`. POM2 refuse MACRAW hors socket 0 (B, voir AMONT FAUX). | `// [D] … :947-1001 @ 3e8054b462 — openSocket().` |
| `W5100Device.cpp:616` | `1039-1075` | OK | `connectSocket`. POM2 ajoute la garde SOCK_INIT (B). | `// [D] … :1039-1075 @ 3e8054b462 — connectSocket().` |
| `W5100Device.cpp:711` | `1077-1102` | OK | `setCommandRegister`. | `// [D] … :1077-1102 @ 3e8054b462 — setCommandRegister().` |
| `W5100Device.cpp:755` | `441-485` | OK | `setTXSizes`/`setRXSizes`. Le `floorPow2` est propre à POM2. | `// [B] W5100 datasheet v1.1.6 §4.1 RMSR/TMSR — attribution depuis le socket 0 dans 8 Ko.` + `// [D] … :441-485 @ 3e8054b462` |
| `W5100Device.cpp:820` | `487-502` | OK | `getTXDataSize`. POM2 diverge (`stagedTxBytes`) et a raison : AMONT FAUX A2. | `// [D] … :487-502 @ 3e8054b462 — masque les deux pointeurs ; anneau plein lu vide.` |
| `W5100Device.cpp:865` | `897-908` | OK | `resetRXTXBuffers`. | `// [D] … :897-908 @ 3e8054b462 — resetRXTXBuffers().` |
| `W5100Device.cpp:1981` | `897-908` | OK | Lignes justes, mais « exactly as a **real** OPEN does » sur-classe la source : c'est l'OPEN d'AppleWin, dont l'appel porte lui-même `// needed?` (ligne 997). | `// [D] … :897-908 @ 3e8054b462 — l'OPEN d'AppleWin remet les pointeurs à 0 (lui-même « needed? », :997).` |
| `W5100Device.cpp:876` | `520-547` | OK | `updateRSR`. | `// [D] … :520-547 @ 3e8054b462 — updateRSR().` |
| `W5100Device.cpp:894`, `W5100Device.h:429` | `119-181` | OK | `write8` … `writeDataForProtocol`. | `// [D] … :119-181 @ 3e8054b462 — écrivains de l'anneau RX.` |
| `W5100Device.cpp:936` | `747-770` | OK | `receiveOnePacket`. | `// [D] … :747-770 @ 3e8054b462` |
| `W5100Device.cpp:953` | `704-745` | OK | `receiveOnePacketFromSocket`. | `// [D] … :704-745 @ 3e8054b462` |
| `W5100Device.cpp:1066` | `595-659` | OK | `receiveOnePacketRaw`. | `// [B] W5100 datasheet v1.1.6 §5.2.4 — MACRAW sur le socket 0 seulement.` + `// [D] … :595-659 @ 3e8054b462` |
| `W5100Device.cpp:1170` | `661-680` | OK | `receiveOnePacketMacRaw`. | `// [D] … :661-680 @ 3e8054b462` |
| `W5100Device.cpp:1185` | `682-701` | OK | `receiveOnePacketIPRaw`. | `// [D] … :682-701 @ 3e8054b462` |
| `W5100Device.cpp:1202` | `844-895` | OK | `sendData`. | `// [B] W5100 datasheet v1.1.6 §4.2 Sn_TX_RR — après SEND, Sn_TX_RR == Sn_TX_WR.` + `// [D] … :844-895 @ 3e8054b462` |
| `W5100Device.cpp:1272` | `812-842` | OK | `sendDataToSocket`. | `// [D] … :812-842 @ 3e8054b462` |
| `W5100Device.cpp:1402` | `772-793` | OK | `sendDataIPRaw`. | `// [D] … :772-793 @ 3e8054b462` |
| `W5100Device.cpp:1431` | `1487-1525` | OK | `getMACAddress`. | `// [D] … :1487-1525 @ 3e8054b462` |
| `W5100Device.cpp:1477`, `W5100Device.h:467`, `W5100NameResolver.h:21` | `1012-1037` | OK | `resolveDNS`. | `// [D] … :1012-1037 @ 3e8054b462 — resolveDNS() (bloquant chez AppleWin).` |
| `W5100Device.cpp:1539` | `1104-1152` | OK | `readSocketRegister`. POM2 ne tire un paquet que sur l'octet fort de RSR (VOULUE, et B : la datasheet impose « upper byte first »). | `// [B] W5100 datasheet v1.1.6 §4.2 Sn_RX_RSR — lire l'octet fort puis le faible.` + `// [D] … :1104-1152 @ 3e8054b462` |
| `W5100Device.cpp:1571` | `1233-1291` | OK | `writeSocketRegister`. | `// [D] … :1233-1291 @ 3e8054b462` |
| `W5100Device.cpp:1618` | `1293-1332` | OK | `setModeRegister` + `writeCommonRegister`. | `// [B] W5100 datasheet v1.1.6 §4.1 MR — RST s'efface seul.` + `// [D] … :1293-1332 @ 3e8054b462` |
| `W5100Device.cpp:1675` | `1154-1183` | OK | `readValueAt`. POM2 replie `≥ $8000` d'abord (VOULUE, manuel p.13). | `// [B] Uthernet II manual (2018-11-17) p.13 — $8000+ adresse $0+.` + `// [D] … :1154-1183 @ 3e8054b462` |
| `W5100Device.cpp:1704` | `1334-1354` | OK | `writeValueAt`. Même repli. | idem, `:1334-1354` |
| `W5100Device.cpp:1745` | `1185-1207` | OK | `autoIncrement` + `readValue`. | `// [B] Uthernet II manual (2018-11-17) pp.12-13 — $5FFF→$4000, $7FFF→$6000, $FFFF→$E000.` + `// [D] … :1185-1207 @ 3e8054b462` |
| `W5100Device.cpp:1774` | `269-306` | OK | `Socket::process` (SYN_SENT). La continuation `:1527-1534` (`Update`) est juste aussi. | `// [D] … :269-306, :1527-1534 @ 3e8054b462` |
| `W5100Device.cpp:1896` | `1563-1587` | OK | Régions sauvées, trous réservés. | `// [B] Uthernet II manual (2018-11-17) p.9 table 4.1 — $30-$3FF et $800-$3FFF réservés.` + `// [D] … :1563-1587 @ 3e8054b462` |
| `W5100Device.h:402` | `910-1102` | OK | Cycle de vie des sockets. | `// [D] … :910-1102 @ 3e8054b462` |
| `W5100Device.h:415` | `441-547` | OK | Géométrie des tampons. | `// [D] … :441-547 @ 3e8054b462` |
| `W5100Device.h:436` | `549-770` | OK | Chemins de réception. | `// [D] … :549-770 @ 3e8054b462` |
| `W5100Device.h:447` | `772-895` | OK | Chemins d'émission. | `// [D] … :772-895 @ 3e8054b462` |
| `W5100Device.h:458` | `1104-1354` | OK | Décodage des registres. | `// [D] … :1104-1354 @ 3e8054b462` |
| `UthernetIICard.cpp:18`, `UthernetIICard.h:26` | `1411-1472` | OK | `IO_C0` + `u2_C0`. | `// [B] Uthernet II manual (2018-11-17) p.10 + schéma (PDF p.50) — $C0x4-7, A0/A1 seuls.` + `// [D] … :1411-1472 @ 3e8054b462` |
| `UthernetIICard.cpp:54` | `1439-1456` | OK | Chemin de lecture. | `// [D] … :1439-1456 @ 3e8054b462` |
| `UthernetIICard.cpp:66` | `1421-1437` | OK | Chemin d'écriture. | `// [D] … :1421-1437 @ 3e8054b462` |

Hors grep, vérifiées en passant et justes sur `3e8054b462` :
`tests/uthernet2_w5100_smoke_test.cpp` cite `:1398-1408` (valeurs initiales) et
`:1293-1302` (MR RST) ; `W5100.h:5-12` et `W5100.h:6` (masque A0/A1) tombent
juste sur `master` de `W5100.h`.

---

## Rapport 2 — Constats

Quatre constats, tous MINEUR : aucun logiciel connu n'en dépend, mais chacun a
un scénario écrit d'après un document primaire. Puis sept `AMONT FAUX`, où POM2
suit la datasheet contre AppleWin.

### 1. Ctrl-Reset garde le pointeur d'adresse indirect, alors que la carte câble /RESET du bus sur /RESET du W5100

```
Fichier      : src/UthernetIICard.cpp:78-88 (onReset → resetChip(false))
               tests/uthernet2_w5100_smoke_test.cpp:310-315 (épingle ce comportement)
Amont        : Uthernet2.cpp:1362-1376 @ 3e8054b462 (Reset(false) garde myDataAddress)
               Utilities.cpp:583-606 @ 21ea317902 (CtrlReset → GetCardMgr().Reset(false))
Gravité      : MINEUR
Confiance    : CONFIRMÉ côté POM2 (sonde P3) ; côté matériel, grade B
               (schéma + manuel), pas de mesure.
Divergence   : AppleWin traite Ctrl-Reset comme le reset LOGICIEL (bit 7 de MR),
               qui laisse le pointeur en place. POM2 fait de même depuis le
               2026-10-27 (CHANGELOG.md:2586-2589) en citant « manual p.10 ».
               Or la p.10 du manuel dit les deux choses : « After a hardware
               reset the address pointer is initialized to 0 » et « a software
               reset does not reset the address pointer ». Le schéma de la carte
               (manuel, PDF pp.50-51) relie la broche RST du connecteur au net RESET,
               qui va à la broche 59 /RESET du W5100. Ctrl-Reset est donc un
               reset MATÉRIEL de la puce : le pointeur doit revenir à $0000.
               Le correctif du 27 octobre a aligné POM2 sur AppleWin (D) contre
               le manuel (B) qu'il croyait citer — la règle « a lower grade
               never overrules a higher one » à l'envers.
Scénario     : un programme laisse le pointeur à $0426 (lecture de S0_RX_RSR),
               l'utilisateur fait Ctrl-Reset. Sur la carte, $C0n5/$C0n6 lisent
               $00/$00. Sur POM2 : $04/$26 (sonde P3). Un pilote qui, après le
               reset, n'écrit que l'octet faible de l'adresse — ce que le manuel
               p.12 autorise explicitement — écrit dans les registres du
               socket 0 au lieu des registres communs.
Observation  : sonde P3 ; le test uthernet2_w5100_smoke affirme le contraire et
               devrait être retourné avec le code.
```

Le reste de `resetChip(false)` est juste : registres, sockets et tampons sont
remis à zéro comme par le /RESET matériel. Le reseed de SHAR à
`02:55:32:00:00:01` (la datasheet dit `00`) est une commodité documentée dans
le même commentaire : VOULUE. Les caches ARP/DNS n'ont pas d'équivalent
observable.

### 2. Une commande RECV qui laisse des données dans l'anneau ne relève pas Sn_IR.RECV

```
Fichier      : src/W5100Device.cpp:748 (case kW5100SnCrRecv: updateRsr(i))
Amont        : — (AppleWin ne modélise pas Sn_IR du tout ; voir AMONT FAUX A1)
Gravité      : MINEUR
Confiance    : CONFIRMÉ (datasheet lue, sonde P1)
Divergence   : datasheet v1.1.6 §4.2 Sn_IR bit 2 : RECV « is set as '1' whenever
               W5100 receives data. And it is also set as '1' if received data
               remains after execute CMD_RECV command ». POM2 lève RECV à
               l'arrivée des données, jamais après une commande RECV.
Scénario     : pilote qui suit la « First method » de la datasheet (§5.2.1.1 :
               « If (Sn_IR(RECV bit) == '1') goto Receiving Process »). Le pair
               envoie 300 octets ; le pilote en consomme 100, acquitte RECV,
               avance Sn_RX_RD et émet RECV. La puce relève RECV (200 octets
               restent) ; POM2 laisse Sn_IR = 00. Le pilote attend des données
               déjà dans l'anneau jusqu'à ce que le pair envoie autre chose.
Observation  : sonde P1 — « après RECV : RSR=200 Sn_IR=00 », et toujours 00
               après 80 000 cycles de poll.
```

### 3. Un DISCON émis par le guest ne lève jamais Sn_IR.DISCON

```
Fichier      : src/W5100Device.cpp:719-730 (DISCON → closeSocket → clearSocket)
               src/W5100Device.cpp:485-496 (clearSocket met interrupt à 0)
Amont        : Uthernet2.cpp:1087-1089 @ 3e8054b462 (DISCON == CLOSE, sans Sn_IR)
Gravité      : MINEUR
Confiance    : CONFIRMÉ (datasheet lue, sonde P2)
Divergence   : datasheet §4.2 Sn_IR bit 1 : DISCON « is set as '1' if connection
               termination is requested or finished », et §5.2.1.1
               « ESTABLISHED : CLOSED ? First method : If (Sn_IR(DISCON bit) ==
               '1') goto CLOSED stage ». POM2 passe à CLOSED (ou FIN_WAIT puis
               CLOSED) et remet Sn_IR à 0 ; le bit DISCON n'est levé que sur
               un FIN reçu du pair.
Scénario     : pilote qui émet DISCON puis attend Sn_IR.DISCON (méthode 1).
               Sur la puce, le bit monte quand la fermeture aboutit. Sur POM2,
               Sn_SR = 00 et Sn_IR = 00 pour toujours : le pilote boucle. Un
               pilote qui teste Sn_SR == SOCK_CLOSED (méthode 2) n'est pas
               touché.
Observation  : sonde P2 — « Sn_SR=00 Sn_IR=00 » après DISCON et quatre polls.
```

### 4. La ligne /IRQ de la carte n'est pas modélisée : le commentaire dit que la carte laisse /INT en l'air, le schéma dit le contraire

```
Fichier      : src/W5100Device.cpp:465-467 et :1650-1653 (« the card leaves INTn
               unconnected ») ; UthernetIICard n'appelle jamais assertIrq()
Amont        : — (aucune ligne IRQ/IMR/Sn_IR dans Uthernet2.cpp @ 3e8054b462)
Gravité      : MINEUR
Confiance    : PLAUSIBLE — doute résiduel : schéma lu sur un scan à 150 dpi, et
               aucun logiciel Apple II connu n'arme IMR.
Divergence   : schéma du manuel (PDF pp.50-51) : broche 56 /INT du W5100 → net INT_W
               → IC3 (1G07, tampon à drain ouvert, broche 2 → 4) → net INT →
               broche IRQ du connecteur. Le manuel p.9 liste « Interrupts (IRQ) »
               parmi les fonctions. Datasheet §4.1 IR/IMR : « The /INT signal
               retain low as long as any masked signal is set ». POM2 calcule
               IR et stocke IMR mais ne tire jamais /IRQ (sonde P4 :
               IR=01 IMR=01 slotIrqAsserted=0). L'affirmation « INTn
               unconnected » n'a pas de source : c'est un grade E contredit
               par un B.
Scénario     : programme qui écrit IMR=$01, installe un vecteur IRQ et attend
               CON ou RECV par interruption. Sur la carte, l'interruption
               arrive ; sur POM2, jamais. Inversement, un programme qui arme IMR
               sans gestionnaire plante sur la carte (IRQ non réclamée) et
               tourne sur POM2.
Observation  : sonde P4.
```

### AMONT FAUX — POM2 suit la datasheet, AppleWin non

Gravité `—`. Ces blocs protègent le code contre un « alignement sur
AppleWin » qui serait une régression.

**A1. AppleWin n'a pas de Sn_IR.** `Uthernet2.cpp:1104-1152` et `:1233-1291`
@ `3e8054b462` : l'offset `$02` tombe dans `default` (lecture de la mémoire
brute, écriture ignorée), donc Sn_IR lit toujours `00`. Datasheet §4.2 : SEND_OK,
TIMEOUT, RECV, DISCON, CON. Un pilote qui attend SEND_OK après SEND, ou CON
après CONNECT (les « First method » de §5.2.1), boucle sur AppleWin. POM2 a un
vrai Sn_IR à écriture 1-pour-effacer et un IR dérivé (`W5100Device.cpp:1592-1601`,
`:1695-1702`).

**A2. Pointeurs TX.** `Uthernet2.cpp:487-502` masque Sn_TX_RD et Sn_TX_WR
avant de les soustraire, et `:870-871` écrit dans Sn_TX_RD la valeur
**masquée** de WR. Datasheet §4.2 Sn_TX_WR/Sn_TX_RR : WR est un compteur libre
dont seul `WR & mask` est une adresse physique, et « after transmission is
finished, Sn_TX_RR and Sn_TX_WR will have same value ». Conséquences chez
AppleWin, sonde P5 : un anneau de 2 Ko plein lit TX_FSR = 2048 (vide) au lieu
de 0 ; après SEND, TX_RD = `0000` alors que TX_WR = `0800` ; et un SEND avec
zéro octet en attente prend la branche `else` de `sendData` (`:862-867`) et
émet les 2048 octets périmés de l'anneau, là où la datasheet dit « transmits
the data as much as the increased size of Sn_TX_WR ». POM2 (`stagedTxBytes`,
`W5100Device.cpp:88-96`, `:1250-1254`) donne 0, `0800`/`0800`, et rien.

**A3. Un FIN du pair ferme le socket chez AppleWin.** `Uthernet2.cpp:727-731` :
`recvfrom` = 0 → `clearFD()` → SOCK_CLOSED. Datasheet §4.2 Sn_SR :
SOCK_CLOSE_WAIT (`$1C`), « connection termination request is received from
peer host … The connection can be closed by receiving the DISCON or CLOSE
commands ». `W5100.h` d'AppleWin ne définit même pas CLOSE_WAIT. Un client
qui doit encore envoyer après le FIN du serveur perd son envoi. POM2 :
`W5100Device.cpp:1042-1049`.

**A4. MACRAW accepté sur les sockets 1-3.** `Uthernet2.cpp:971-973` ; datasheet
§4.2 Sn_MR (« In case of socket 0, MACRAW and PPPoE mode exist ») et §5.2.4
(« only supported in socket 0 »). Un OPEN MACRAW sur S1 passe en `$42` chez
AppleWin alors que la réception n'alimente que S0. POM2 :
`W5100Device.cpp:588-592`.

**A5. CONNECT hors SOCK_INIT.** `Uthernet2.cpp:1039-1075` ne teste pas l'état :
un CONNECT sur un socket UDP fait `connect()` sur le datagramme et le passe en
ESTABLISHED, en-tête RX de 8 octets perdu. Datasheet §4.2 Sn_CR CONNECT
« only used in TCP mode » et Sn_SR SOCK_SYNSENT « CONNECT … at the SOCK_INIT
status ». POM2 : `W5100Device.cpp:628`.

**A6. Registres R/W dont AppleWin jette l'écriture.** Sn_DHAR, Sn_MSSR
(`:1278-1288`, branche `default`), IMR, RTR, RCR, PMAGIC (`:1305-1332`, rien
pour eux). Datasheet §4.1/§4.2 : tous `[R/W]`. Un pilote qui écrit RTR puis le
relit pour vérifier son initialisation voit `07D0` au lieu de sa valeur. POM2 :
`W5100Device.cpp:1581-1585`, `:1649-1671`.

**A7. Miroir `$8000-$FFFF`.** `Uthernet2.cpp:1154-1183` ne lit au-delà de
`$7FFF` que la mémoire brute (`$8403` rend `myMemory[$0403]`, pas Sn_SR ; le
commentaire amont l'avoue : « this might not be 100% correct »), `:1334-1354`
jette les écritures, et `:1185-1200` laisse `$FFFF` passer à `$0000`. Manuel
p.13 : « $801A corresponds to $001A … When the address pointer surpasses $FFFF
it will wrap back to $E000 ». POM2 replie l'adresse avant tout décodage
(`W5100Device.cpp:1683`, `:1710`) et passe `$FFFF` à `$E000` (`:1754-1756`).
Le manuel déconseille d'aller au-delà de `$7FFF` : enjeu pratique faible.

---

## Sonde

`/tmp/audit_uthernet2/probe.cpp`, compilée avec les sources de la ligne
`add_executable(test_uthernet2_w5100 …)` :

```
clang++ -std=c++17 -O1 -Isrc -Itests -Ibuild/generated /tmp/audit_uthernet2/probe.cpp \
  src/UthernetIICard.cpp src/W5100Device.cpp src/W5100HostSockets.cpp \
  src/W5100NameResolver.cpp src/SlotBus.cpp
```

Aucune connexion réseau : une `W5100SocketFactory` factice rend un socket TCP
qui se connecte aussitôt et livre 300 octets une fois. Le guest est joué par
`deviceSelectRead/Write` sur `$C0n4-7`, comme un pilote 6502. P5 recopie
`getTXDataSize` d'AppleWin (`:487-502`) tel quel pour la comparaison.

```
== P1  Sn_IR.RECV après une commande RECV qui laisse des données
  Sn_SR=17 Sn_IR=01 (après CONNECT)
  RSR=300  Sn_IR=04 (RECV levé à l'arrivée)
  après RECV : RSR=200  Sn_IR=00  (datasheet : RECV=1 « if received data remains »)
  après 80k cycles de poll : Sn_IR=00  IR=00
== P2  Sn_IR.DISCON après une commande DISCON émise par le guest
  Sn_SR=00  Sn_IR=00  (datasheet « CLOSED? first method » : DISCON=1)
== P3  Ctrl-Reset (slot RST -> /RESET du W5100 sur le schéma)
  $C0n5/$C0n6 après onReset : 04 26  (manuel p.10 : « After a hardware reset the address pointer is initialized to 0 »)
  SHAR après onReset : 02:55:32:00:00:01 (datasheet SHAR reset = 00)
  MR RST logiciel puis $C0n5/6 : 00 0E (manuel p.10 : inchangé — attendu)
== P4  IMR + Sn_IR en attente -> ligne /IRQ du slot
  IR=01 IMR=01 slotIrqAsserted=0  (schéma : /INT W5100 -> 1G07 -> /IRQ du slot)
== P5  AMONT FAUX — anneau TX plein (2 Ko) et Sn_TX_RD après SEND
  POM2    : TX_FSR=0 avant SEND
  AppleWin: TX_FSR=2048 avant SEND (getTXDataSize masque -> 0 présents)
  POM2    : après SEND TX_RD=0800 TX_WR=0800 Sn_IR=10
  AppleWin: après SEND TX_RD=0000 (Uthernet2.cpp:870-871 écrit sn_tx_wr MASQUÉ) TX_WR=0800
  AppleWin: SEND avec 0 octet en attente -> rr==wr -> branche else -> 2048 octets envoyés
```

---

## Pistes examinées et écartées

- **Règles implémentées deux fois.** Registre de mode par `$C0n4` et par le port
  de données à l'adresse 0 : même `setModeRegister`, même `modeRegister_` en
  lecture. `readValueAt` et `peekValueAt` : même repli `& $7FFF`, mêmes valeurs
  pour Sn_SR, Sn_IR, FSR, RSR, IR. Comptage TX : `txDataSize`, `sendData` et
  `socketInfo` passent tous par `stagedTxBytes`. Anneau RX : `updateRsr` masque,
  `ringWrite8` fait le modulo, `clampRingState` remet tout dans la géométrie
  après RMSR ou snapshot. IR dérivé dans les deux chemins de lecture. Rien ne se
  contredit.
- **Ordre des octets** de bout en bout (DIPR/DPORT vers `connect`, Sn_PORT vers
  `bind`/`listen`, `accept` vers DIPR/DPORT, en-tête UDP RX) : tout reste en
  ordre réseau, `readNetworkWord` → `hostToNet16` là où il faut. Sain.
- **RSR tiré sur l'octet fort seul** (VOULUE, `W5100Device.cpp:1553-1565`) : la
  datasheet impose « upper byte first » (B). Le déchirement que corrige POM2
  chez AppleWin donne toujours une valeur ≤ au vrai compte (fort de N, faible de
  N+M) : le pilote lit moins que disponible, sans dommage. Pas un AMONT FAUX
  observable.
- **Anneau RX jamais plein** (`ringHasRoomFor` strict, lecture TCP à
  `freeRoom - 1`) : identique chez AppleWin. La datasheet montre un RSR de
  `$0800` dans un exemple de format, ce qui ne prouve pas que la puce remplit
  l'anneau. Un datagramme de taille exacte `taille - 8` serait perdu ; aucun
  logiciel connu, grade F. Écarté.
- **RMSR/TMSR qui débordent 8 Ko** (`floorPow2` chez POM2, taille non puissance
  de 2 chez AppleWin) : la datasheet dit seulement « the socket should not be
  used ». Comportement indéfini, aucun des deux n'est contredit.
- **Auto-incrément `$DFFF → $C000`** dans le miroir : extrapolation de POM2, le
  manuel ne parle que de `$FFFF → $E000` et déconseille la zone. Latent.
- **OPEN avec le bit DNS sur MACRAW ou CLOSED** (`$0C`, `$08`) : POM2 lance la
  résolution et efface DIPR, AppleWin non. Aucun pilote n'écrit ces modes.
  Latent.
- **Sn_IR effacé par CLOSE et OPEN** (`clearSocket`) : la datasheet ne dit rien ;
  les pilotes l'effacent eux-mêmes. Pas de scénario.
- **Valeurs initiales** (RTR, RCR, RMSR, TMSR, PTIMER `$28` sans DNS virtuel,
  Sn_DHAR `FF`, Sn_TTL `$80`, Sn_MSSR `0000`, Sn_CR relu `00`) : conformes à la
  datasheet v1.1.6.
- **MR RST** garde le pointeur : conforme au manuel p.10 (c'est le reset
  logiciel). L'algorithme de détection du manuel (MR `$80` → relu `$00`, MR `$03`
  → relu `$03`) passe.
- **Divergences VOULUES**, toutes documentées dans le code ou DEV.md § Uthernet
  II : barrière loopback/multicast/link-local (`checkDestination`, décision de
  sécurité, CLAUDE.md « The guest is inside the loopback perimeter ») — y compris
  l'effet qu'un client DHCP W5100 ne peut pas tourner ; ports locaux < 1024 et
  ports de POM2 refusés ; LISTEN opt-in sur `127.0.0.1` ; DNS asynchrone, validé
  et limité ; DNS virtuel non restauré par un snapshot ; PTIMER verrouillé tant
  que le DNS virtuel est actif ; datagramme UDP lu entier ou jeté ; arriéré
  `pendingTx` compté dans TX_FSR ; `poll()` qui reçoit sans attendre la lecture
  de RSR ; Sn_PORT non lié pour un client TCP ; SEND ignoré hors état émetteur ;
  réponse ARP en IPRAW ; cache ARP plafonné ; pointeurs remis à zéro pour un
  socket restauré CLOSED.
- **Commentaire périmé**, sans effet : `UthernetIICard.cpp:108-111` dit que les
  données ne sont tirées qu'à la lecture de RSR ; `W5100Device::poll()`
  (`:1790-1796`) les tire désormais à chaque passage.
- **Hors tranche, noté en passant** : les renvois internes de DEV.md § Uthernet
  II ont dérivé (`W5100Device.cpp:167-182` → 170-184 pour `checkDestination`,
  `:201-212` → 205-216, `:110-130` → 119-136, `:82-91` → 88-96,
  `W5100Device.h:299` → 301). La datasheet est citée « §5.2.3 Sn_IR » ; dans la
  v1.1.6, Sn_IR est au §4.2 et le §5.2.3 est IP raw (la v1.2.8 n'a pas pu être
  lue pour trancher).

---

## Bilan

- **Citations examinées** : 46 (+ 2 continuations sans nom de fichier, + 4 hors
  grep dans `tests/` et pour `W5100.h`).
- **Verdicts** : 46 OK, 0 OK (rev X), 0 DÉCALÉE, 0 ABSENTE, 0 INEXACTE. Une
  citation (`W5100Device.cpp:1981`) sur-classe sa source (« a real OPEN » pour
  du code AppleWin).
- **Révision amont** : pas de pin. Une seule époque, `80ec6e5545` =
  `926c592f75` = `3e8054b462` (`master`, 2026-07-26), ports et citations
  cohérents ; à épingler `@ 3e8054b462`. Aucun correctif amont postérieur.
- **Constats** : 0 CRITIQUE, 0 MAJEUR, 4 MINEUR (3 CONFIRMÉ, 1 PLAUSIBLE) :
  1. Ctrl-Reset garde le pointeur d'adresse indirect (régression du 2026-10-27,
     manuel mal lu, test qui l'épingle).
  2. RECV qui laisse des données ne relève pas Sn_IR.RECV.
  3. DISCON émis par le guest ne lève jamais Sn_IR.DISCON.
  4. Ligne /IRQ de la carte absente, et commentaire « INTn unconnected »
     contredit par le schéma.
- **AMONT FAUX** : 7 (Sn_IR absent, pointeurs TX, FIN → CLOSED, MACRAW hors
  socket 0, CONNECT hors INIT, registres R/W jetés, miroir `$8000+`).

**Ce que je n'ai pas pu vérifier :**

- La datasheet **v1.2.8** que cite POM2 (404 chez WIZnet) ; tout est arbitré sur
  la **v1.1.6**. Un changement de texte entre les deux sur Sn_IR (RECV après
  RECV, DISCON) n'est pas exclu.
- Aucune mesure sur une vraie carte (grade A) : les constats 1 et 4 reposent sur
  le schéma du manuel, lu sur un scan à 150 dpi, d'une seule révision de carte.
  Je n'ai pas vérifié qu'aucune révision ultérieure ne coupe /INT ou /RESET.
- Que le RESET d'un //e ou d'un IIgs tire bien la broche RST du connecteur : je
  m'appuie sur le modèle de POM2 lui-même (`SlotBus::reset`, « Apple II RESET
  line ») et sur le schéma, pas sur la Technical Reference.
- Le comportement de la puce sur un datagramme qui ne tient pas dans l'anneau,
  sur Sn_IR après OPEN/CLOSE, et sur un anneau rempli exactement : la datasheet
  ne le dit pas.
- Je n'ai lancé aucun logiciel Apple II réel (IP65, Contiki, A2Stream, ADTPro)
  sur POM2 : je ne sais pas si l'un d'eux suit la « First method » (Sn_IR) ou
  arme IMR. Les constats 2, 3 et 4 restent sans logiciel témoin connu.
- `W5100HostSockets.cpp` (la couche socket hôte) n'a été lue que pour l'ordre des
  octets et `accept`/`bind`/`listen` ; elle ne porte pas de citation amont.
