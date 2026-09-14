# Limites connues / dette technique

Cette page liste honnêtement ce qui n'est pas fait, plutôt que de le laisser
découvrir en silence.

!!! note "Elle se vide aussi"
    Une page de dette ne fait que grossir si personne n'en retire les entrées
    payées. Chaque chantier terminé doit en supprimer une — une dette déjà réglée
    qui traîne fait croire à un travail restant.

## Variables d'environnement : deux noms `D2_*` sans équivalent

Le gros du renommage est fait (`1e003e2`, complété par `bc41daf` pour le plan
mémoire invité) : les réglages génériques s'appellent `WX86_*`, les anciens
noms `D2_*` (dont `D2ARENA`, `D2HI`, `D2LAYOUT`, `D2MEMBASE`, désormais
`WX86_ARENA`/`WX86_HI`/`WX86_LAYOUT`/`WX86_MEMBASE`) restent acceptés en
repli pour ne pas casser les scripts existants.

Deux variables lues par le moteur n'ont **pas** de jumeau `WX86_` :
`D2_EIPTRAP_N`, `D2_XFERTRACE` — toutes deux dans
`third_party/box86-dynarec/dynarec/dynarec.c`, du code Box86 vendored et
patché mécaniquement (voir [Architecture](architecture.md)), volontairement
laissé intact. Deux autres n'ont aucun préfixe du tout (`THREADLOG`,
`TRAPTAG`), ce qui est un risque de collision avec l'environnement d'un
consommateur.

C'est un problème de **nom**, pas de couplage : leur comportement est
entièrement générique. Mais ça induit en erreur quelqu'un qui chercherait un
lien avec le jeu d'origine là où il n'y en a pas.

## `select` : coupure serveur pas encore vue sur console

`select` et `__WSAFDIsSet` sont servis par le moteur, comme le reste de la couche
socket ([liste générée](shims.md)). Leur contrat Winsock est couvert sur sockets
locales par l'auto-test du portage d2vita (`D2_SELECTTEST`), qui échoue si l'on
réintroduit un retour d'erreur immédiat ou un reset signalé dans `exceptfds`.

Sous charge réseau réelle, une session en ligne du portage sur un serveur public
tourne sans gel sur console. Reste le chemin d'erreur : une connexion coupée par
le serveur en pleine partie doit finir en déconnexion propre, pas en gel. Cette
entrée disparaît une fois ce cas vu sur console.

## La présentation écran — jamais auditée

`vita_present.cpp` (présentation à l'écran) n'a jamais été relu pour en extraire
une éventuelle partie générique. C'est le dernier gros morceau où personne n'a
regardé — l'inconnue n'est donc pas « combien est générique », c'est « on ne
sait pas ».

## Chemin personnel en dur dans deux scripts

`tools/regen_box86_patch.sh` et `tools/extract_box86.sh` ont
`/home/doudou/repos/box86` comme valeur par défaut du dépôt Box86 amont. C'est
surchargeable en premier argument, donc sans conséquence fonctionnelle, mais
c'est un chemin personnel dans un dépôt destiné à être lisible par d'autres.

## Une seule preuve d'usage

winx86 n'a qu'un consommateur réel à ce jour. Tout ce qui est déclaré
« générique » l'est par audit du code, pas par la preuve d'un deuxième portage
qui s'en sert — à une exception près, la technique de comparaison entre deux
portages décrite dans [Outils IA](outils-ia.md).

C'est la limite la plus honnête de ce dépôt : la frontière a été tracée avec
soin, mais elle n'a pas encore été tirée par un second usage.
