# QCM — Connaître la famille Apple II et les profils POM2

Version : 1.1 — vérification des informations POM2 le 20 octobre 2026\
Public : amateur éclairé, médiateur ou utilisateur de POM2  
Objectif observable : distinguer les principaux modèles Apple II, associer leurs processeurs et particularités, puis reconnaître les éléments matériels essentiels émulés par POM2.  
Sources de référence : `README.md` (profils, matériel, stockage), `CLAUDE.md` (profils, chronologie, mémoire et cadences), `src/SystemProfile.h` et `src/SystemProfile.cpp` (définition canonique des modèles).  
Périmètre : les machines 8 bits prises en charge par POM2. L’Apple IIgs, machine 16 bits, sert uniquement de contre-exemple et relève du projet frère POM2GS.

## Consigne

Pour chaque question, choisir une seule réponse. Compter un point par bonne réponse, sans pénalité. Ne consulter le corrigé qu’après avoir répondu aux vingt questions.

## Niveau 1 — Repères essentiels

### 1. En quelle année l’Apple II original a-t-il été lancé ?

A. 1976  
B. 1977  
C. 1979  
D. 1983

### 2. Quel modèle succède à l’Apple II original dans la chronologie proposée par POM2 ?

A. Apple II Plus  
B. Apple IIe  
C. Apple IIc  
D. Apple IIgs

### 3. Quelle association modèle–année est correcte ?

A. Apple II Plus — 1984  
B. Apple IIe — 1979  
C. Apple IIc — 1984  
D. Apple IIc Plus — 1985

### 4. Quelle évolution caractérise le passage de l’Apple II au II Plus dans les profils POM2 ?

A. Le passage au processeur 65C02 à 4 MHz  
B. L’intégration d’Applesoft BASIC et de l’Autostart ROM  
C. La suppression de tous les connecteurs d’extension  
D. L’apparition du lecteur 3,5 pouces intégré

### 5. Quel modèle est conçu comme une machine compacte sans connecteurs d’extension physiques ?

A. Apple II original  
B. Apple II Plus  
C. Apple IIe  
D. Apple IIc

### 6. Lequel n’est pas un profil de machine POM2 ?

A. Apple II original  
B. Apple IIe Enhanced  
C. Apple IIc Plus  
D. Apple IIgs

## Niveau 2 — Modèles et architecture

### 7. Quel processeur équipe le profil Apple IIe non amélioré de 1983 ?

A. NMOS 6502  
B. 65C02  
C. 65816  
D. Z80

### 8. Quel processeur équipe l’Apple IIe Enhanced de 1985 ?

A. NMOS 6502  
B. 65C02  
C. 65816  
D. 68000

### 9. Quelle fréquence distingue surtout l’Apple IIc Plus des autres modèles 8 bits de la gamme ?

A. Environ 500 kHz  
B. Environ 1 MHz uniquement  
C. Environ 2 MHz  
D. Environ 4 MHz

### 10. Pourquoi POM2 verrouille-t-il les périphériques intégrés des profils Apple IIc et IIc Plus ?

A. Ces machines réelles n’ont pas de connecteurs d’extension physiques  
B. Leur processeur ne sait adresser aucun périphérique  
C. Leur système interdit les lecteurs de disquettes  
D. Elles ne possèdent aucune ROM

### 11. Quelle description des ports de l’Apple IIc est correcte ?

A. Un port parallèle et aucun port série  
B. Deux ports série, désignés imprimante et modem  
C. Deux ports parallèles identiques  
D. Aucun port de communication

### 12. Dans la convention Apple II, à quel emplacement logique trouve-t-on habituellement le contrôleur Disk II ?

A. Slot 1  
B. Slot 4  
C. Slot 6  
D. Slot 7

### 13. Quelle combinaison de modes vidéo est prise en charge par POM2 ?

A. Texte, basse résolution, haute résolution, double haute résolution et 80 colonnes  
B. Texte uniquement  
C. VGA 640 × 480 uniquement  
D. Haute résolution monochrome uniquement

### 14. Quelle affirmation distingue correctement NTSC et PAL dans POM2 ?

A. NTSC et PAL fonctionnent tous deux à 50 Hz et 312 lignes  
B. NTSC utilise 262 lignes à environ 60 Hz ; PAL, 312 lignes à environ 50 Hz  
C. PAL est plus rapide que NTSC et fonctionne à 120 Hz  
D. La différence concerne seulement la forme du clavier

## Niveau 3 — Connaissance approfondie de la machine et de POM2

### 15. Quelle particularité possède le profil Apple IIc PAL « Le Chat Mauve » ?

A. Un adaptateur RGB Péritel Le Chat Mauve associé au connecteur vidéo DB-15  
B. Un processeur 65816 16 bits  
C. Huit connecteurs d’extension physiques  
D. Un écran LCD intégré

### 16. Quelle est la fréquence nominale utilisée par POM2 pour les profils NTSC à environ 1 MHz ?

A. 1 000 000 Hz exactement  
B. 1 015 600 Hz  
C. 1 022 727 Hz  
D. 4 090 908 Hz

### 17. Quelle zone mémoire correspond à la page haute résolution 1 ?

A. `$0400–$07FF`  
B. `$0800–$0BFF`  
C. `$2000–$3FFF`  
D. `$D000–$F7FF`

### 18. Quel accès mémoire commande le haut-parleur intégré ?

A. `$C010`  
B. `$C030–$C03F`  
C. `$C050–$C057`  
D. `$C0E0–$C0EF`

### 19. Que contiennent les paquets POM2 pour un démarrage standard ?

A. Aucun micrologiciel : toutes les ROM doivent être ajoutées avant le premier lancement\
B. Des ROM générées automatiquement par le processeur\
C. Les micrologiciels, polices et illustrations prévus par le manifeste de distribution\
D. Uniquement les ROM de l’Apple IIgs

### 20. Quelle affirmation décrit le mieux la place de l’Apple IIgs ?

A. C’est le nom commercial de l’Apple II Plus  
B. C’est une machine 16 bits de la famille, hors du périmètre de POM2 et couverte par POM2GS  
C. C’est une variante PAL de l’Apple IIc  
D. C’est une carte d’extension sonore

## Corrigé commenté

1. B. Le profil canonique nomme explicitement l’Apple II original comme modèle de 1977.  
2. A. L’Apple II Plus apparaît en 1979, après l’Apple II original et avant l’Apple IIe.  
3. C. L’Apple IIc date de 1984 ; le II Plus de 1979, le IIe de 1983 et le IIc Plus de 1988.  
4. B. Le II Plus conserve le NMOS 6502 mais adopte notamment Applesoft BASIC et l’Autostart ROM.  
5. D. Le « c » renvoie à la conception compacte ; contrairement aux II, II Plus et IIe, le IIc n’expose pas de connecteurs d’extension physiques.  
6. D. POM2 propose neuf profils 8 bits ; l’Apple IIgs 16 bits appartient au projet frère POM2GS.\
7. A. Le IIe de 1983 utilise encore le NMOS 6502 ; choisir 65C02 confond ce modèle avec le IIe Enhanced.  
8. B. L’amélioration de 1985 apporte le 65C02 et une ROM de caractères incluant MouseText.  
9. D. Le IIc Plus exécute son 65C02 à environ 4 MHz, contre environ 1 MHz pour les autres profils principaux.  
10. A. Les « slots » du IIc sont des emplacements logiques correspondant à du matériel intégré, pas des connecteurs que l’utilisateur pourrait retirer.  
11. B. Les ports imprimante et modem du IIc sont tous deux série ; le mot « imprimante » ne signifie pas qu’il s’agit d’un port parallèle.  
12. C. Le contrôleur Disk II est classiquement associé au slot 6, dont la ROM d’amorçage occupe la zone `$C600–$C6FF`.  
13. A. POM2 reproduit les principaux modes de la famille : texte, lo-res, hi-res, double hi-res et affichage 80 colonnes.  
14. B. Les profils NTSC utilisent 262 lignes et environ 60 Hz ; les profils PAL utilisent 312 lignes et environ 50 Hz.  
15. A. Ce profil représente la configuration européenne avec l’adaptateur RGB Péritel Le Chat Mauve relié au port vidéo DB-15.  
16. C. La constante nominale est de 1 022 727 Hz, obtenue à partir de 14,31818 MHz divisés par 14.  
17. C. La page HGR 1 occupe `$2000–$3FFF`; `$0400–$07FF` correspond à la page texte/lo-res 1.  
18. B. Tout accès dans `$C030–$C03F` bascule l’état du haut-parleur ; `$C050–$C057` sélectionne les modes d’affichage.  
19. C. Le manifeste de distribution inclut le dossier roms/, les polices et les illustrations nécessaires ; un paquet standard n’exige pas d’ajouter les ROM avant de démarrer. Les conditions de provenance et de distribution sont décrites dans THIRD-PARTY.md.\
20. B. L’Apple IIgs repose sur une architecture 16 bits distincte ; il n’est donc pas l’un des neuf profils de POM2.

## Interprétation du score

De 0 à 7 : repères à consolider ; reprendre la chronologie et les différences entre II, II Plus, IIe et IIc.  
De 8 à 14 : connaissances fonctionnelles ; revoir surtout les processeurs, les ports et les standards vidéo.  
De 15 à 17 : bonne maîtrise de la famille Apple II et de POM2.  
De 18 à 20 : maîtrise solide, y compris des spécificités d’architecture.
