# winx86

**Moteur générique d'exécution x86 → ARMv7 pour jeux Windows (PE32) sur PS Vita.**

winx86 charge un exécutable ou une DLL Windows x86 (32 bits), traduit son code
au vol vers ARMv7 (dynarec dérivé de [Box86](https://github.com/ptitSeb/box86)),
et fournit le point d'accroche pour qu'un portage y branche ses propres shims
Win32 et ses propres optimisations natives — **sans que winx86 ait besoin de
connaître le jeu**.

Ce dépôt est **privé** pour l'instant.

## À qui ça s'adresse

À quiconque veut porter un jeu Windows x86 (PE32) sur PS Vita sans repartir
de zéro sur la partie « faire tourner du code x86 sur ARM ». winx86 ne fait
que ça : charger, traduire, ordonnancer, et offrir un point d'extension. Tout
le reste — quels shims Win32 le jeu a besoin, quelles fonctions mériteraient
un portage natif pour la performance, comment on installe/configure le jeu —
relève du portage lui-même.

## Preuve par l'exemple : d2vita

[d2vita](https://gitlab.com/claude5564407/d2-vita) est le portage qui a fait
naître winx86 : un portage privé et personnel de *Diablo II: Lord of
Destruction*. winx86 en a été extrait le 2026-09-09 en isolant précisément ce
qui n'avait **aucune** connaissance du jeu — le reste (les ~600 shims Win32
du jeu, ses hooks de performance sur des adresses précises du binaire, son
rendu Glide) est resté dans d2vita. d2vita consomme aujourd'hui winx86 comme
un sous-module git et sa propre documentation détaille comment ce lien
fonctionne concrètement.

## L'architecture en un coup d'œil

```
Exécutable Windows (.exe/.dll)
        │
   Chargeur PE32 (src/runtime/pe_image.*)
        │
   Interface Cpu (src/runtime/cpu.h) ──── backend CpuBox86 (ARMv7, dynarec)
        │
   Bridge (src/runtime/bridge.*) ─── point d'extension : shims Win32 + hooks natifs
        │
   Ordonnanceur de fils invités (coopératif ou natif + GIL)
```

## Architecture « hybride »

winx86 est une **bibliothèque statique autonome et compilable seule**
(`libwinx86.a`), pas un fork à copier-coller. Un portage l'ajoute comme
sous-module git et lie son propre exécutable de boot contre cette
bibliothèque. Ça veut dire concrètement :

- winx86 évolue dans son propre historique git, sa propre CI, sa propre
  suite de vérifications (`tools/shim_seq.py`, l'oracle qemu-arm).
- Un portage peut épingler une version précise de winx86 (le commit du
  sous-module) et la faire évoluer à son rythme.
- Un correctif générique découvert par un portage (ex: un bug de dynarec) se
  remonte dans winx86 et profite à tous les portages qui le consomment — la
  méthode a déjà servi une fois entre d2vita et son projet frère privé
  *carn-vita* (portage de Carnivores 2, même moteur sous-jacent, forké de
  d2vita avant l'existence de ce dépôt) : trois correctifs génériques
  trouvés côté carn-vita ont été remontés dans d2vita début septembre 2026,
  avant même que winx86 existe comme dépôt séparé.

## Pour démarrer

Voir [Démarrage rapide](demarrage.md) pour compiler winx86 seul, et
[Point d'extension](extension.md) pour brancher votre propre jeu dessus.
