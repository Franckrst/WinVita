# Limites connues / dette technique

Cette page liste honnêtement ce qui n'est pas fait, plutôt que de le laisser
découvrir en silence.

!!! note "Elle se vide aussi"
    Une page de dette ne fait que grossir si personne n'en retire les entrées
    payées. Chaque chantier terminé doit en supprimer une — une dette déjà réglée
    qui traîne fait croire à un travail restant.

## Variables d'environnement : six noms `D2*` sans équivalent

Le gros du renommage est fait (`1e003e2`) : les réglages génériques s'appellent
`WX86_*`, les anciens noms `D2_*` restent acceptés en repli pour ne pas casser
les scripts existants.

Six variables lues par le moteur n'ont **pas** encore de jumeau `WX86_` :
`D2ARENA`, `D2HI`, `D2LAYOUT`, `D2MEMBASE`, `D2_EIPTRAP_N`, `D2_XFERTRACE`.
Deux autres n'ont aucun préfixe du tout (`THREADLOG`, `TRAPTAG`), ce qui est un
risque de collision avec l'environnement d'un consommateur.

C'est un problème de **nom**, pas de couplage : leur comportement est
entièrement générique. Mais ça induit en erreur quelqu'un qui chercherait un
lien avec le jeu d'origine là où il n'y en a pas.

## `select` reste inscrit par le portage

Toute la couche socket est passée côté moteur ([liste générée](shims.md)) — sauf
`select` (`WSOCK32.dll!#18`) et le `__WSAFDIsSet` qui l'accompagne.

Son cœur — la traduction `fd_set` ↔ `pollfd` — est pourtant générique. Ce qui le
retient est précis : c'est le site exact d'une famine réseau déjà corrigée, dont
la signature de régression ne se voit **que sous charge réseau réelle**. La
validation en ligne faite lors du découplage n'a pas couvert ce cas-là. C'est
donc un report assumé, avec un critère de levée écrit : un test de charge réel,
pas une relecture.

## `ds_emul` (DirectSound) — pas extrait

Émulation DirectSound par fabrication d'une vtable COM à partir d'une table de
shims. Le *mécanisme* — fabriquer une vtable COM générique depuis une table de
pointeurs de fonctions — est réutilisable tel quel par n'importe quel jeu
utilisant DirectSound ; le fichier actuel, côté portage, mélange ce mécanisme
avec des choix de mixage audio propres au jeu.

Une scission propre (mécanisme ici, choix de mixage là-bas) est identifiée mais
n'a jamais été auditée en détail.

## La présentation écran — jamais auditée

`vita_present.cpp` (présentation à l'écran) n'a jamais été relu pour en extraire
une éventuelle partie générique. C'est, avec `ds_emul`, le dernier gros morceau
où personne n'a regardé — l'inconnue n'est donc pas « combien est générique »,
c'est « on ne sait pas ».

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
