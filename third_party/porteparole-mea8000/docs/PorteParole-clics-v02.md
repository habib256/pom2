# Correction des clics : prototype 0.2

Date : 31 octobre 2026. Retour utilisateur : clics absents de la carte réelle,
audibles dans EDILOGO-VOIX et dans Porte-Parole. Son écoute de référence identifie
la banque 1, messages 1 et 2 (second message incomplet).

## Référence préservée

`origines/EDI LOGO/edilogo_demo_clean.mp3` : 6,096 s, MP3 stéréo 48 kHz.
SHA-256 : `5e155dc00c4d6615a6660016500c917fa1ddd643096ad9c34b4f376cc9494b3d`.
Le MP3 original est intact ; copie conservée dans la référence avant correction,
WAV mono d'analyse dans la nouvelle session. Les 14 fichiers de l'audit initial
sont également inchangés. Aucun accès en écriture à DS:3 Revival.

## Défaut 1 : énergie de résonance figée pendant STOP

La v0.1 arrêtait le calcul des quatre résonateurs pendant l'arrêt et l'attente.
Leur énergie interne pouvait réapparaître au message suivant avec de nouveaux paramètres.
Reproduction : son résonant, STOP, une seconde de silence, trame d'amplitude zéro.
Pic avant : 32767/32767 ; pic corrigé : 0/32767.
Le système interne évolue désormais à entrée nulle pendant que la sortie est coupée.
Il ne s'agit pas d'un lissage ajouté au son ni d'une normalisation de volume.
Ce choix supprime le défaut de temps figé ; la fidélité des détails silicium reste à mesurer.

## Défaut 2 : réserve DirectSound perdue au démarrage

Le backend peut retourner les deux curseurs à zéro pendant les premières mises à jour.
L'appel au helper commun traitait cette égalité comme un intervalle interdit circulaire
et ramenait systématiquement la position d'écriture à zéro, détruisant le préremplissage.
Sur le premier message de la banque 1, la trace avant correction recense
15 repositionnements en cours de synthèse, sur environ 0,75 s.

La carte ignore maintenant ce faux intervalle quand les curseurs sont égaux.
La réserve initiale est portée à 100 ms ; une vraie récupération de sous-alimentation
rétablit une réserve au lieu de rester collée au curseur matériel.
L'accélération disque automatique est bloquée pendant 300 ms après une activité de synthèse
afin de laisser passer le buffer et la fin du son. Le turbo demandé explicitement reste disponible.
Le helper partagé d'AppleWin n'a pas été modifié.

## Mesure réelle après correction

Une disquette de diagnostic distincte lit automatiquement les messages 1 et 2.
Les binaires PORTEPAROLE et LOGO1.R sont extraits sans modification d'un disque de test.
La trace après correction compte 5935
soumissions pendant la synthèse et **zéro repositionnement** pendant ces deux messages.
La réserve observée pendant la synthèse reste environ entre 83 et 140 ms.
Des repositionnements restent possibles pendant une attente silencieuse prolongée.

Cette mesure établit la disparition des ruptures observées sur ce parcours, pas une
preuve d'absence de clic sur tout périphérique ou toute charge système.
Le contrôle à l'oreille par l'utilisateur reste nécessaire.

## Autres vérifications

* Compilation Release Win32, VS2026 v145, avec le test CPU6502 du projet : réussie.
* 187 vérifications du cœur passent, dont le scénario de clic et la reprise d'état pendant sa décroissance.
* Les six essais des lecteurs 6502 de test passent encore.
* Leurs rendus isolés sont identiques bit à bit aux références v0.1 : pas de modification
  des phrases isolées pour masquer le problème de transport audio.
* Sept banques EdiLogo comparées en enchaînement, avec rendus v01/v02 conservés.
* Alignement de l'enveloppe RMS avec votre référence : corrélation environ 0,965,
  décalage environ 0,28 s et facteur temporel environ 1,002.
  Cela confirme le passage et le rythme global ; ce n'est pas un test de timbre ou de clics.

La télémétrie est désactivée par défaut. Pour un diagnostic explicite, `PP_AUDIO_TRACE`
doit désigner un nouveau fichier ; maximum 20000 lignes, sorties tamponnées.
Les lanceurs normaux ne définissent pas cette variable.

## Conservation et utilisation

La v0.1 et ses archives restent dans leur session d'origine. La sauvegarde avant
correction contient ses sources, binaire, réglages et paramètres de lancement.
Les lanceurs principaux pointent maintenant vers la v0.2 ; leurs versions précédentes
sont conservées dans cette sauvegarde. Aucun original de disquette n'est monté.

Les limites de fidélité détaillées dans PorteParole.md restent applicables :
bruit, excitation, arrondis, mémoire optionnelle, F11/F12 et validation matérielle générale.
