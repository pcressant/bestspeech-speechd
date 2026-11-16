# sd_bestspeech : Keynote Gold pour Speech Dispatcher

Module de sortie Speech Dispatcher pour la synthèse BeSTspeech de Berkeley
Speech Technologies, celle du Keynote GOLD de HumanWare. Le moteur est
[openbst](https://github.com/Mudb0y/openbst), une réimplémentation en C
natif mesurée au sample près contre les binaires d'origine. Il n'y a ni
Wine ni émulateur, et aucune DLL Windows.

Ce projet reprend l'idée de
[BstSpeech-sapi](https://github.com/gozaltech/BstSpeech-sapi), un wrapper
SAPI5 Windows autour de `b32_tts.dll`, et la transpose à Linux. Il en
reprend les 14 préréglages de voix, le nettoyage du texte, les
correspondances débit, hauteur et volume, et l'usage de Sonic pour les
vitesses que le moteur n'atteint pas.

## Voix

Seuls le français et l'anglais sont proposés : cinq versions du moteur,
chacune avec les 14 préréglages, soit 70 voix.

- `fr-2006-*` : 2006 français, voix française par défaut, 10 000 Hz.
- `fr-1998-*` : 1998 français, 11 025 Hz.
- `en-1995-*` : 1995 anglais, le son classique du Keynote Gold, voix
  anglaise par défaut, 11 025 Hz.
- `en-1998-*` : 1998 anglais, 11 025 Hz.
- `en-2006-*` : 2006 anglais, 10 000 Hz.

Les préréglages sont Fred, Sara, Hary, Wendy, Dexter, Alien, Kit, Bruno,
Ghost, Peeper, Dracula, Granny, Martha et Tim. Les types de voix de Speech
Dispatcher y correspondent ainsi :

- male1 : Fred, male2 : Hary, male3 : Dexter ;
- female1 : Sara, female2 : Wendy, female3 : Martha ;
- child_male : Tim, child_female : Kit.

Fred est la voix propre du moteur. Le changement de voix (`v`) n'agit que
sur la 1995. La hauteur, l'excitation et le gain agissent sur toutes les
versions.

Exemples :

    spd-say -o bestspeech -l fr "Bonjour"
    spd-say -o bestspeech -y en-1995-Granny "Hello"
    spd-say -o bestspeech -l fr -t female1 -r 50 "Voix rapide"

## Réglages

- **Débit** : de -100 à 100 donne une vitesse de 0,25 à `MaxSpeed`
  (4 par défaut). Le débit natif d'openbst plafonne vers 2,5 fois la
  vitesse normale ; au-delà, Sonic accélère le son. Les correspondances
  viennent d'une mesure faite avec `tools/calibrate.c`.
- **Hauteur** : décale le plancher et le plafond de la voix d'une octave
  au plus dans chaque sens.
- **Étendue** : élargit ou resserre l'écart entre plancher et plafond.
- **Volume** : gain appliqué aux échantillons, avec écrêtage.
- **Ponctuation, épellation, majuscules, caractères, touches, icônes
  sonores, marques d'index, arrêt, pause** : gérés.
  - La ponctuation suit le traitement des symboles du serveur.
  - L'annonce des majuscules ne s'applique qu'aux caractères et à
    l'épellation, pas à l'intérieur des mots.

Le fichier `bestspeech.conf` accepte :

- `DefaultBuild fr 2006FRE` (ou `1998FRN`)
- `DefaultBuild en 1995` (ou `1998ENG`, `2006ENG`)
- `MaxSpeed 4`
- `Debug 0`

## Réactivité

- **Une synthèse par phrase.** Orca met une marque d'index avant chaque
  mot. Le module dit la phrase entière d'un seul tenant et place les
  marques dans son audio, en proportion de leur position dans le texte.
  Il ne dit plus chaque mot comme une phrase séparée.
- **Silences coupés.** Le moteur laisse jusqu'à 150 ms de silence avant
  la parole et 370 à 480 ms après. Le module supprime le silence de
  début, ramène la fin de message à 20 ms et l'écart entre deux phrases à
  220 ms au plus. Ces pauses raccourcissent quand le débit augmente.
- **Premier son en moins d'une milliseconde.** Toutes les versions sont
  ouvertes au démarrage du module. Une phrase se synthétise bien en
  dessous de la milliseconde, et un texte long commence à sonner dès sa
  première phrase. L'arrêt est pris en compte entre deux blocs de 25 ms.

## Volume

Les versions n'ont pas le même niveau : la 2006 sort environ 5 fois
plus bas que la 1995, et les 1998 saturent. Le module ramène chacune à
un niveau commun, un peu plus fort qu'espeak-ng. Le volume 100 de Speech
Dispatcher (sa valeur par défaut) vaut ce niveau, 0 vaut la moitié et
-100 le silence, comme avec espeak-ng. Au-delà de 24000, les crêtes sont
arrondies au lieu d'être écrêtées.

## Particularités du moteur traitées par le module

- **Codage du texte.** Les versions 2006 lisent la page Windows (cp1252).
  Les versions 1995 et 1998 lisent la page DOS (cp850) : un octet
  Windows au-dessus de 0x7f leur fait rendre un silence total. Le module
  convertit l'UTF-8 vers la bonne page. Un caractère absent de la page
  devient une espace plutôt que « point d'interrogation ».
- **La virgule dans les versions 2006.** Elles lisent la virgule comme
  la fin du texte : « Bonjour, au revoir. » donne « Bonjour ». Le
  deux-points leur donne la même coupure de proposition sans rien
  arrêter, donc le module le leur passe à la place de la virgule.
- **Les nombres.** Dans toutes les versions, deux nombres séparés par une
  espace (« 10 30 », « 1 2 3 ») ou une virgule entre deux chiffres
  (« 3,5 ») rendent toute la phrase muette. Le module corrige ainsi :
  - entre deux nombres, il met deux espaces, ce qui fonctionne ;
  - un groupe de trois chiffres après une espace (« 1 000 ») est un
    séparateur de milliers : il le colle au nombre précédent ;
  - « 3,5 » devient « 3 virgule 5 » en français ;
  - « 10:30 » devient « 10 30 ».
- **Filet de sécurité.** Une phrase qui contient des mots ne revient
  jamais muette. Elle est d'abord redite sans ses caractères accentués,
  puis mot à mot en dernier recours.
- **La lettre c dans la 2006 français.** Cette version lit tout c, C
  et ç comme un s avant ses règles de prononciation : « comme » donne
  « somme », « aucun » donne « ausun ». La DLL d'origine fait de même :
  le logiciel hôte du Keynote devait corriger le texte avant de le lui
  passer. Le module fait ce travail dans `speechd/french.c` : c dur → k,
  c doux → s (ss entre voyelles), « accès » → ks, « chrome » et
  « psychologie » → k, « direct » prononcé jusqu'au t, « blanc » et
  « tabac » sans leur c. Seule, la lettre se dit « cé ».
- **« après »** se dit « aprèz » dans les deux versions françaises, quoi
  qu'il suive ; le module ne garde le z que devant une voyelle.
- **Vérification au dictionnaire.** `make frcheck WORDS=fr.dic` compare
  les phonèmes de chaque mot d'une liste (le dictionnaire Hunspell
  français, par exemple) à ceux d'espeak-ng. Sur 63 570 mots : 2006
  réécrite 12,1 % d'écarts, 2006 brute 35,5 %, 1998 12,6 %. Le reste
  tient surtout aux o ouverts ou fermés, au [ɥ] de « nuage » et aux mots
  empruntés.
- **Les commandes intégrées** `~v3]`, `~f80]` de la DLL ne sont pas
  comprises par openbst, qui les lit à voix haute. Le module applique
  donc les préréglages par des réglages. Le petit correctif
  `patches/openbst-voice-settings.patch` expose pour cela l'excitation
  et le gain. Un tilde présent dans le texte est retiré.
- **La 1998 français** s'écarte, selon openbst, du moteur d'origine sur
  les phrases de deux mots.

## Construire et tester

Avec Nix :

    nix build            # construit et lance tous les tests
    nix develop          # shell avec $OPENBST
    make OPENBST=$OPENBST test

Sans Nix, il faut un compilateur C, make, pkg-config, les fichiers de
développement de Speech Dispatcher (`spd_module_main.h`,
`libspeechd_module`), glib, Sonic, python3 et `patch`, ainsi qu'une copie
d'openbst :

    make OPENBST=/chemin/vers/openbst test
    make PREFIX=/usr/local install

`make test` fait quatre choses, sans serveur ni son :

- il lance le self-test d'openbst sur la bibliothèque réduite et
  corrigée : 1299 énoncés identiques à ce qu'ont dit les binaires
  d'origine ;
- il vérifie la réécriture française sur une liste de mots
  (`test/french.py`) ;
- il pilote le module en sortie brute (`RawOutput 1`, réservé aux
  tests) et compare l'audio, sample par sample, avec `bstspeak` pour le
  texte que le module doit lui passer ;
- il pilote le module en sortie normale et contrôle ce qu'entend
  l'utilisateur : niveau, silences, absence de coupure entre les mots
  marqués, nombres audibles, arrêt et pause.

## Installer

Speech Dispatcher découvre seul les modules de son répertoire. **N'ajoutez
pas de ligne `AddModule`** dans `speechd.conf` : la première déclaration
explicite coupe la découverte de tous les autres modules.

Sur NixOS, le module est lié dans le préfixe du paquet speechd, comme
OpenEVV. Ailleurs, `make install` installe `sd_bestspeech` et
`bestspeech.conf` dans les répertoires de Speech Dispatcher.

Pour que le module soit pris en compte, Speech Dispatcher doit redémarrer,
ce qui coupe la parole. Faites-le depuis une session de secours.

## Licences

- Le code de ce dépôt est sous licence MIT.
- Le code d'openbst est sous licence MIT.
- Les tables d'openbst (`src/data`, et quelques-unes dans `src/text.c`)
  sont l'œuvre de Berkeley Speech Technologies puis de HumanWare.
  Personne ici n'est en mesure de les licencier, d'où le statut
  « unfree » du paquet. Ce dépôt ne contient aucune de ces tables : il
  les prend dans openbst au moment de la construction.
