# Porte-Parole pour AppleWin : prototype 0.2

Mise à jour : correction des clics et validation du tampon audio ; voir [PorteParole-clics-v02.md](PorteParole-clics-v02.md).
Les résultats ci-dessous décrivent la base 0.1 ; le compte rendu v0.2 fait foi pour les corrections et tests supplémentaires.

Date : 31 octobre 2026. Prototype de dÃ©veloppement, fidÃ©litÃ© matÃ©rielle et sonore encore Ã  qualifier.

## Origine et isolation

Base AppleWin officielle : commit `4a43bcdb8e1cb908662f4c270ca84f309a97802f`.
Arbre de développement : copie indépendante d'AppleWin.
Aucune source ni aucun exÃ©cutable DS:3 Revival utilisÃ© ou modifiÃ©.
Les logiciels et données d'origine sont conservés à part du code de l'émulateur.
Leurs copies horodatÃ©es sont dans `references`, avec manifestes SHA-256.
Avant modification d'AppleWin, ses sources concernÃ©es et son binaire compilÃ© sans Porte-Parole
ont Ã©tÃ© sauvegardÃ©s dans `references\AppleWin_avant_PorteParole_20261031_172141`.

## Architecture

* `source/Mea8000.h` : cÅ“ur autonome C++14, sans Windows, Python, fichiers ou audio hÃ´te.
* `source/PorteParole.cpp/.h` : carte et adaptation des cycles CPU Ã  l'horloge du quartz.
* `Card`, `CardManager`, `CmdLine` : type ajoutÃ© en fin d'Ã©numÃ©ration, sÃ©lection par interface ou `-s4 porteparole`.
* `Windows/AppleWin.cpp` : la synthÃ¨se empÃªche l'accÃ©lÃ©ration automatique liÃ©e au moteur disque.
* `test/PorteParole` : tests du cÅ“ur et pont DLL de validation des vrais lecteurs 6502.

La carte standard sans mÃ©moire optionnelle ne publie aucune ROM fictive.
La fente est configurable de 1 Ã  7. Pour la fente 4 : donnÃ©es/statut Ã  $C0C0,
commande Ã  $C0C1. Les autres offsets sont provisoirement miroitÃ©s selon A0.
Seul D7 est considÃ©rÃ© comme Ã©tabli ; les autres bits sont provisoirement lus Ã  zÃ©ro.

Le cÅ“ur garde un pitch initial, une trame en cours et un tampon de quatre octets.
Un tampon complet bloque D7 jusqu'au prochain changement de trame.
Une temporisation provisoire de douze pÃ©riodes du quartz suit chaque octet acceptÃ©.
Les durÃ©es sont 64, 128, 256 ou 512 Ã©chantillons natifs, Ã  4 MHz / 480.
La trame minimale dure donc 7,68 ms, indÃ©pendamment de la vitesse du processeur Apple II.
STOP interrompt la synthÃ¨se ; CONT permet la rÃ©pÃ©tition ; l'absence de trame suivante
produit un arrÃªt progressif. ROE est mÃ©morisÃ©, sans inventer de raccordement IRQ.

La synthÃ¨se utilise quatre rÃ©sonateurs en cascade, une source dent de scie ou du bruit,
des interpolations et un flux mono 48 kHz. Les historiques survivent aux Ã©critures et aux
frontiÃ¨res des blocs hÃ´tes. Le gain est fixe, sans normalisation par phrase.
Le profil analogique provisoire reprend une hypothÃ¨se de calibration provisoire : passe-bas Butterworth
d'ordre 4 Ã  3 kHz et passe-haut d'ordre 2 Ã  120 Hz.
Le buffer hÃ´te est continu, enregistrÃ© auprÃ¨s de SoundCore pour la gestion du son et des dialogues.
L'accÃ©lÃ©ration explicitement demandÃ©e par l'utilisateur avance le matÃ©riel mais coupe l'audio hÃ´te.

La sÃ©rialisation enregistre tous les paramÃ¨tres, le tampon partiel, les phases, le bruit,
les historiques des filtres et la fraction de cycle. Les champs sont numÃ©riques, sans
sauvegarde brute d'une structure dÃ©pendant de l'alignement 32/64 bits.
La restauration exacte du cÅ“ur est testÃ©e ; le parcours complet F11/F12 AppleWin reste Ã  vÃ©rifier.

## Sources de spÃ©cification

* Manuel local `origines/PORTE PAROLE/doc/Ediciel Porte Parole.pdf`, annexes 2, 5 et 6.
* Logiciel Ediciel original `PORTEPAROLE`, 174 octets Ã  $0300.
* Notes de développement et prototypes logiciels de référence non distribués avec ce dépôt.
* Documentation Philips MEA8000 et comparaison conceptuelle avec le cÅ“ur MAME.

Le cÅ“ur C++ a Ã©tÃ© Ã©crit pour ce prototype. Aucun composant MAME n'est liÃ© et aucune
dÃ©pendance Python n'est nÃ©cessaire pour utiliser l'exÃ©cutable AppleWin.

## Compiler et tester

Toolset utilisÃ© : Visual Studio 2026, v145, Release Win32, SDK Windows 10.
Le projet VS2026 du dÃ©pÃ´t amont choisit lui-mÃªme v145. La variante VS2022 est Ã©galement
mise Ã  jour dans ses listes de fichiers mais n'a pas fait l'objet d'une validation sÃ©parÃ©e.

```powershell
.\Build-PorteParole.ps1
.\test\PorteParole\BuildTests.cmd
```

Le projet AppleWin exÃ©cute aussi son test CPU6502 avant compilation.
La construction ne remplace pas le binaire figÃ© d'une session d'essai.

Pour les essais des lecteurs 6502 : Python 64 bits et py65 1.2.0.
Cette dÃ©pendance est installÃ©e localement dans `developpement/validation-deps`.

```powershell
python -B .\test\PorteParole\validate_drivers.py
```

Le banc py65 est synchronisÃ© Ã  la fin de chaque instruction. Il ne certifie pas la
position exacte de chaque accÃ¨s bus Ã  l'intÃ©rieur d'une instruction.
Il charge les binaires des lecteurs sans les modifier, contrÃ´le les octets acceptÃ©s,
et produit les WAV et statistiques dans la session indiquÃ©e par `prototype-session.json`.

## VÃ©rifications obtenues

* Compilation du binaire AppleWin de rÃ©fÃ©rence puis du prototype : rÃ©ussies.
* Test CPU6502 prÃ©vu par le projet : passÃ© lors des compilations.
* 89 vÃ©rifications du cÅ“ur : attente, dÃ©lais, tampon plein, refus des octets en trop,
  quatre durÃ©es, sous-alimentation, STOP/CONT, indÃ©pendance aux dÃ©coupages,
  restauration exacte des Ã©tats et gÃ©nÃ©ration sonore non nulle.
* Lecteur Ediciel : EDILOGO1-0 et EDILOGO2-1, en fente 4.
* Lecteur de test : BONJOURH en fente 4, CHATF en fente 1, EDILOGO1-0 en fente 7.
* ArrÃªt ESC du lecteur de test : confirmÃ© par le code de retour ABORT=1.
* Aucun octet refusÃ© et aucun Ã©chantillon Ã©crÃªtÃ© dans ces cinq lectures complÃ¨tes.
* Le pilote Ediciel fourni lit un octet au-delÃ  de la longueur du message avant ses
  quatre zÃ©ros de fin. Le banc l'exerce avec un octet sentinelle $A5, sans corriger le pilote.
* AppleWin rÃ©el : dÃ©marrage du logiciel Porte-Parole original, saisie/conversion et
  retour Ã  l'interface aprÃ¨s demande de lecture.

Les rendus hors ligne et le retour de l'interface ne constituent pas une validation
Ã  l'oreille de la sortie DirectSound ni une comparaison complÃ¨te avec une vraie carte.

## Limites et suite de la validation

1. Mesurer le quartz, les miroirs d'adresses, les bits bas de statut et le dÃ©lai REQ sur le matÃ©riel.
2. VÃ©rifier le reset, les commandes rarement utilisÃ©es et l'alimentation tardive en trames.
3. Arbitrer FM2 code 16 : manuel Ediciel/Python = 1110 Hz, MAME = 1100 Hz.
4. Qualifier l'amplitude absolue, l'excitation, les arrondis du circuit et le filtre analogique.
   Le bruit dÃ©terministe choisi n'est pas une reproduction du gÃ©nÃ©rateur de bruit silicium.
   Le pitch est bornÃ© Ã  0..510 dans ce prototype ; le comportement hors plage reste Ã  mesurer.
5. VÃ©rifier les longues lectures hÃ´tes, la pause, les pÃ©riphÃ©riques audio et les sous-alimentations
   du buffer DirectSound. L'absence de pÃ©riphÃ©rique audio doit laisser fonctionner le protocole.
6. Effectuer F11/F12 en lecture, puis les essais complets de PICOLO, clavier, musique et bruitages.
7. Valider le nouveau logiciel sur le IIe rÃ©el Ã©quipÃ© de la carte et de 128 Ko.
8. Ne dÃ©velopper la mÃ©moire optionnelle 2716/6116 qu'avec un cas d'usage et le dÃ©codage vÃ©rifiÃ©.

Les banques de voix, rÃ¨gles de franÃ§ais et fichiers .U restent des donnÃ©es des logiciels
Apple II ; ils ne sont pas codÃ©s dans la carte Ã©mulÃ©e.
