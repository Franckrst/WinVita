# Outils IA / agents réutilisables

Cette page recense l'outillage de développement pensé pour être réutilisable par
n'importe quel portage basé sur winx86 — pas seulement d2vita — et propose
quelques pistes pour la suite.

## `.claude/skills/` et `.claude/agents/`

Le savoir-faire du moteur est versionné avec lui, et chargé automatiquement par
Claude Code quand on travaille dans ce dépôt.

Les **skills** décrivent un domaine : les internes du dynarec et de
l'ordonnanceur, les crochets sur code invité, la méthode d'élimination face à une
corruption intermittente, la vérification d'un instrument avant d'y croire, le
protocole de mesure sur vrai matériel, et les bases de la plateforme Vita.

Les **agents** décrivent une procédure complète et rejouable :

- `shim-split` — déplacer des shims entre le moteur et un portage : classer par
  le **corps** et non par le nom d'API, le filet `shim_seq`, la règle « une seule
  inscription par clé », et le seuil à partir duquel « trop enchevêtré » est une
  conclusion acceptable.
- `doc-update` — remettre la doc en accord avec le code, en vérifiant dans le
  code plutôt que dans un résumé.

La frontière est la même que pour le code : ce qui ne sert qu'à un seul
consommateur (lire *ses* archives, piloter *sa* boucle de jeu) reste chez lui.

## `tools/gen_shim_list.py` — la page des shims est générée

[La liste des shims fournis par le moteur](shims.md) n'est pas écrite à la main :
elle est produite depuis `src/runtime/win32_shims_*.cpp`, en réutilisant le
parseur déjà éprouvé de `shim_seq.py` plutôt qu'un second analyseur à maintenir
en parallèle. La CI la régénère et **échoue si elle a divergé**
(`gen_shim_list.py --check`).

Une liste écrite à la main serait fausse au deuxième commit, et une liste fausse
est pire qu'une absence de liste : on croit alors savoir ce que le moteur couvre.

Deux choix de fond :

- les **doublons d'inscription sont signalés**, pas dédoublonnés en silence — une
  page qui les masquerait cacherait précisément ce qu'on veut voir ;
- les ordinaux Winsock sont **nommés d'après la source** (la lambda `connect_fn`
  donne `connect`), jamais d'après une table recopiée à la main. C'est ce qui
  rend visible la divergence réelle entre `WSOCK32.dll` et `WS2_32.dll` sur les
  ordinaux 10/11/12.

## `tools/shim_seq.py` / `.sh`

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

Un fichier passé via `ALLOW_BODY=<fichier>` documente les transitions de
corps *explicitement* attendues (clé, empreinte avant, empreinte après)
quand un changement de comportement est volontaire — jamais un
blanc-seing, une dérive ultérieure de la même clé redevient fatale. Rien
n'impose son nom ni son emplacement : ce n'est pas un fichier `.allow` fixe
versionné dans le dépôt.

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
  La CI actuelle ne fait que publier le site et vérifier la fraîcheur de la
  page générée.
- **Un `shim_seq` généralisé au-delà des shims** — le même patron
  (séquence + empreinte + diff entre deux révisions) s'appliquerait à
  n'importe quelle table d'enregistrement « dernière inscription gagne »,
  pas seulement `register_shim`. Pas encore extrait en bibliothèque
  générique faute d'un deuxième cas d'usage concret pour le justifier.
