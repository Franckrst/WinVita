# Historique et provenance

## Box86

winx86 doit son cœur de traduction x86 → ARMv7 à
[Box86](https://github.com/ptitSeb/box86) (Sébastien Chevalier, dit
« ptitSeb », licence MIT). `third_party/box86-dynarec/` en est une
extraction verbatim, plus un jeu de patches locaux mécaniquement régénérés
(fastmmu, memintrin, emitprof, signtag, un point d'accroche de préemption
d'ordonnanceur, un interrupteur de protection en écriture pour le code
auto-modifiant). Ces patches sont pensés pour être proposables en amont à
Box86 lui-même. Voir [Architecture](architecture.md) pour la mécanique
d'extraction/resynchronisation.

## Extraction depuis d2vita (2026-09-09)

winx86 a été extrait le 2026-09-09 du projet
[d2vita](https://github.com/Franckrst/D2Vita), un portage de
Diablo II: Lord of Destruction sur PS Vita, en isolant le sous-ensemble du
moteur qui n'avait aucune connaissance du jeu (chargeur PE32, dynarec, `Cpu`/
`Bridge`, ordonnanceurs, outillage générique). Le reste — les ~600 shims
Win32 du jeu, ses hooks de performance sur des adresses précises de son
binaire, son rendu Glide — est resté dans d2vita.

## La frontière a bougé (2026-09-10 / 09-11)

La coupure initiale était prudente : **aucun** shim Win32 n'avait suivi, un
audit ayant trouvé du contenu propre au jeu caché derrière des noms d'API
parfaitement génériques. Cette prudence était justifiée sur le constat, mais la
conclusion « aucun shim n'est générique par nature » s'est révélée trop forte.

En reprenant groupe de DLL par groupe de DLL, et en classant chaque fonction par
son **corps** plutôt que par son nom, plus de deux cents inscriptions ont rejoint
le moteur : SHELL32, ADVAPI32, USER32, DirectDraw/Bink/Smacker/ijl11, IMM32,
GDI32, fenêtre et curseur, VERSION et PSAPI, puis toute la couche socket.

Trois points d'extension neutres sont nés de ce travail, chacun d'une politique
qu'il fallait sortir d'un shim sans la perdre : l'allocateur de brouillon invité,
l'observateur passif de la couche socket, et la route de connexion. Le détail est
dans [Point d'extension](extension.md), la liste exacte dans [Shims
fournis](shims.md).

## Pourquoi un historique git neuf, pas hérité de d2vita

winx86 démarre avec un historique git **volontairement neuf**, pas extrait
par réécriture de l'historique de d2vita (`git filter-repo` ou équivalent).
d2vita est un travail dérivé de binaires Windows propriétaires
décompilés — un contexte légalement sensible détaillé dans la documentation
de d2vita elle-même. Réécrire son historique dans un nouveau dépôt risquerait
de faire resurgir, via `git log`/`git show` sur d'anciens commits, du
contenu que des commits plus récents ont retiré de l'état courant mais pas
de l'historique. Repartir d'un historique neuf pour winx86 évite la question
plutôt que de la gérer au cas par cas.

## La piscine JIT plafonnée par le noyau, pas par choix (2026-09-13)

Une tentative de réserver la piscine JIT *avant* l'arène invitée (pour
absorber le gâchis d'alignement, 16+13=29 Mio en un seul bloc) a révélé que
le noyau Vita refuse tout bloc `sceKernelAllocMemBlockForVM` au-delà de
16 Mio (`SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW`) — jamais testé avant sur ce
projet. La tentative a été abandonnée (code revenu à l'état d'avant), et la
piscine réécrite pour grandir par segments de 16 Mio plutôt qu'un bloc
unique. Détail technique dans [Architecture](architecture.md).

Au passage, un bug latent préexistant a été corrigé : l'ancien code posait
son drapeau « essai déjà tenté » **avant** de connaître le résultat de
l'allocation — un unique échec (pression mémoire, fragmentation) désarmait
la piscine pour le reste de la session, sans retentative ni second signal
que la ligne de log « REFUSÉE ». Le nouveau code pose ce drapeau seulement
**après** un refus réel du noyau, jamais par anticipation — et comme les
segments vivent dans un tableau séparé, un segment déjà ouvert avant l'échec
reste pleinement utilisable : seule la croissance future est bloquée, pas ce
qui a déjà été alloué.

## Évolution

Voir `git log` pour le détail commit par commit. Les grandes étapes
initiales : extraction du moteur générique (2026-09-09), câblage de d2vita
comme premier consommateur via sous-module git, mécanisme de « saveur »
LIBTAG/EXTRA pour builds A/B, correctif d'un bug de contamination de build
(un en-tête spécifique à la vraie console Vita polluait la cible de test
qemu-arm/Linux).
