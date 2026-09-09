# Outils IA / agents réutilisables

Cette page recense l'outillage de développement pensé pour être réutile par
n'importe quel portage basé sur winx86 — pas seulement d2vita — et propose
quelques pistes pour la suite.

## `tools/shim_seq.py` / `.sh` / `.allow`

Reconstruit la **séquence ordonnée** des shims Win32 (et hooks natifs)
inscrits dans un binaire de boot, avec l'empreinte du corps de chaque
inscription, et compare deux états (arbre courant vs une révision git).
Trois verdicts, du plus grave au moins grave :

- **ENSEMBLE** — une clé apparaît/disparaît, ou change de nombre
  d'inscriptions. Toujours fatal.
- **EFFECTIF** — même jeu de clés, mais un autre corps gagne pour au moins
  une clé (rappel : `register_shim` écrase, la dernière inscription gagne).
  Toujours fatal — c'est exactement le bug qu'un déplacement de bloc mal
  fait produit.
- **ORDRE** — même table effective, mais la séquence diffère (permutation de
  blocs). Fatal par défaut ; `ALLOW_REORDER=1` l'accepte pour un refactor
  qui déplace des blocs entiers sans changer la table effective.

**Quand s'en servir** : avant tout refactor qui déplace du code
d'enregistrement de shims — typiquement en sortant un morceau d'un fichier
monolithique vers son propre fichier, exactement le genre d'opération que ce
dépôt a effectuée sur lui-même plusieurs fois en se séparant de d2vita.
Lancer `tools/shim_seq.sh` avant et après ; `OK` = rien n'a changé côté
table effective.

`tools/shim_seq.allow` documente les transitions de corps *explicitement*
attendues (clé, empreinte avant, empreinte après, raison) quand un
changement de comportement est volontaire — jamais un blanc-seing, une
dérive ultérieure de la même clé redevient fatale.

## `tools/extract_box86.sh` / `tools/regen_box86_patch.sh`

Permettent de re-synchroniser `third_party/box86-dynarec/` contre une
version plus récente de Box86 en amont sans perdre les patches locaux :
`regen_box86_patch.sh` capture le delta local actuel dans un fichier de
patch mécaniquement validé (auto-vérifié en le réappliquant et en comparant
le résultat bit à bit) ; `extract_box86.sh` réextrait un Box86 propre,
réapplique le patch, et **refuse de toucher à l'arbre existant** si le
résultat diverge — plutôt que d'écraser silencieusement du travail non
capturé.

## `tools/pe_analyze.cpp`

Inspecte la table d'imports d'un binaire PE32 — utile pour vérifier
empiriquement quelles fonctions Win32 un binaire importe réellement avant
d'écrire un shim pour une fonction qui ne sera jamais appelée.

## La méthodologie d'extraction sûre (démontrée, pas juste documentée)

winx86 lui-même a été extrait de code entangled dans un fichier de boot
monolithique de 12 000+ lignes, en plusieurs passes. La méthode qui a
fonctionné, à chaque fois :

1. **Cartographier en lecture seule d'abord** — identifier les limites
   exactes (souvent non contiguës : du code apparenté peut être entrelacé
   avec du code totalement différent) et l'inventaire complet des symboles
   qui traversent la frontière, avant de toucher à un seul fichier.
2. **Ne jamais faire confiance à une estimation de limites** — revérifier
   avec l'outillage (`grep -n`, `sed -n`) au moment de l'extraction, pas
   seulement au moment de la cartographie ; le code bouge.
3. **Externaliser mécaniquement, sans changer la logique** — chaque symbole
   qui traverse la frontière devient un `extern`, rien d'autre ne change.
4. **Valider avec un diff de séquence/empreinte** (`shim_seq.sh` ici) **et
   un oracle déterministe rejoué deux fois** (voir la documentation de
   d2vita pour l'oracle qemu-arm) — ne commiter que si les deux sont
   authentiquement au vert, jamais sur la seule preuve de compilation.
5. **S'arrêter et rapporter honnêtement si ce n'est pas un mouvement propre**
   — plusieurs tentatives pendant l'extraction de winx86 ont été
   volontairement interrompues en cours de route (un mouvement de shims Win32
   entiers a été abandonné en découvrant du contenu spécifique au jeu caché
   dans des noms de fonction génériques). Une extraction bâclée qui « a l'air
   de marcher » est pire qu'aucune extraction.

## Une technique nommée : la généricité par comparaison

Quand deux portages existent (ici : d2vita et son projet frère carn-vita,
tous deux dérivés du même moteur), on peut *prouver* qu'un bout de code est
générique plutôt que de le supposer : si les deux portages ont le même corps
pour une même fonction, c'est une preuve forte de généricité ; s'ils ont
divergé, c'est la preuve qu'une personnalisation était nécessaire — même si
le nom de la fonction (`GetVersion`, `CreateProcessA`...) ne le laisse pas
deviner. Cette technique a permis de retrouver, remonter et croiser
plusieurs correctifs génériques entre les deux projets avant que winx86
n'existe comme dépôt séparé.

## Propositions (pas encore construites)

- **Un `CLAUDE.md` gabarit** pour tout nouveau portage basé sur winx86,
  capturant les règles qui ont rendu ce travail fiable : ne jamais affirmer
  qu'un test/build a réussi sans l'avoir réellement exécuté ; dépôt privé
  par défaut tant que la légalité d'un portage depuis des binaires
  propriétaires n'a pas été tranchée ; jamais de comportement factice marqué
  comme complet.
- **Un gabarit de CI à trois niveaux** (qemu-arm rapide → Vita3K → matériel
  réel) packagé en `.gitlab-ci.yml` réutilisable, pour qu'un nouveau portage
  n'ait pas à redécouvrir la distinction entre « ça compile », « ça boote
  sous émulation » et « c'est réellement plus rapide sur la vraie console ».
- **Un `shim_seq` généralisé au-delà des shims** — le même patron
  (séquence + empreinte + diff entre deux révisions) s'appliquerait à
  n'importe quelle table d'enregistrement « dernière inscription gagne »,
  pas seulement `register_shim`. Pas encore extrait en bibliothèque
  générique faute d'un deuxième cas d'usage concret pour le justifier.
