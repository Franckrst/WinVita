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
[d2vita](https://gitlab.com/claude5564407/d2-vita), un portage privé de
Diablo II: Lord of Destruction sur PS Vita, en isolant le sous-ensemble du
moteur qui n'avait aucune connaissance du jeu (chargeur PE32, dynarec, `Cpu`/
`Bridge`, ordonnanceurs, outillage générique). Le reste — les ~600 shims
Win32 du jeu, ses hooks de performance sur des adresses précises de son
binaire, son rendu Glide — est resté dans d2vita.

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

## Évolution

Voir `git log` pour le détail commit par commit. Les grandes étapes
initiales : extraction du moteur générique (2026-09-09), câblage de d2vita
comme premier consommateur via sous-module git, mécanisme de « saveur »
LIBTAG/EXTRA pour builds A/B, correctif d'un bug de contamination de build
(un en-tête spécifique à la vraie console Vita polluait la cible de test
qemu-arm/Linux).
