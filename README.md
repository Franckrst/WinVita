# winx86

Moteur générique d'exécution x86 → ARMv7 pour des jeux Windows (PE32) sur PS
Vita. Extrait du projet [d2vita](https://gitlab.com/claude5564407/d2-vita)
(un portage de Diablo II: Lord of Destruction) le 2026-09-09, dans le but
d'être réutilisable pour porter n'importe quel autre jeu Windows x86 sur PS
Vita sans rien connaître de ce jeu en particulier.

Dépôt privé pour l'instant.

## Ce que c'est

- Un chargeur PE32/PE (`src/runtime/pe_image.*`) : mappe un exécutable ou une
  DLL Windows x86 en mémoire, résout les imports, applique les relocations.
- Un dynarec x86 → ARMv7 dérivé de [Box86](https://github.com/ptitSeb/box86)
  (ptitSeb, MIT) — `third_party/box86-dynarec/` (extraction verbatim +
  patches locaux mécaniquement régénérés, voir `tools/extract_box86.sh` et
  `tools/regen_box86_patch.sh`) et sa couche d'intégration
  (`src/dynarec86/`).
- Une interface CPU indépendante du moteur (`src/runtime/cpu.h`), avec le
  backend ARM/Box86 (`cpu_box86.cpp`).
- Un ordonnanceur de fils invités, coopératif ou natif
  (`src/runtime/sched_cooperative.*`, `sched_native.*`, `gil.*`).
- Le pont d'imports Win32 générique (`src/runtime/bridge.*`) : **c'est le
  point d'extension**, voir plus bas.
- Quelques utilitaires plateforme Vita génériques (`src/platform/`) : sortie
  audio (`vita_audio.cpp`), clavier virtuel à l'écran (`vita_kb.h`).
- L'outillage de développement : `tools/shim_seq.py`/`.sh` (détecte tout
  déplacement/écrasement d'un shim enregistré — filet de sécurité avant un
  refactor), `tools/extract_box86.sh`/`regen_box86_patch.sh` (extraction
  reproductible + validation mécanique des patches Box86 locaux),
  `tools/pe_analyze.cpp` (inspection de table d'imports PE32).

## Ce que ce n'est PAS

winx86 ne contient **aucun** shim Win32 pré-écrit (`GetVersion`,
`HeapCreate`, etc.), aucun hook natif de point chaud, aucune connaissance
d'un jeu particulier. Un audit dédié (2026-09-09) a montré que même des
shims au nom générique peuvent porter des valeurs spécifiques à un jeu (ex:
chemins codés en dur, émulation d'authentification propre à un titre) —
mieux vaut que chaque portage écrive ses propres shims via l'API
d'extension ci-dessous plutôt que d'hériter silencieusement de comportement
d'un autre jeu.

## API d'extension : comment un portage branche son propre jeu

Deux primitives, déjà génériques, suffisent :

- `Cpu::set_alternate(from_va, to_va)` (`src/runtime/cpu.h`) : redirige
  l'exécution d'une adresse invité vers une autre. C'est le mécanisme de
  "crochet à l'entrée d'une fonction" — toujours sur une adresse d'ENTRÉE de
  fonction, jamais une cible de saut (la fusion de blocs du dynarec rendrait
  le crochet muet).
- `Bridge::register_shim(dll, name, shim)` / `Bridge::shim_trap(dll, name)`
  (`src/runtime/bridge.h`) : enregistre un shim natif par nom de DLL+fonction
  et obtient son adresse de trap. `register_shim` ÉCRASE la clé : la
  dernière inscription gagne — c'est ce qui permet à un consommateur de
  d2vita/carn-vita de fournir un défaut générique et de le laisser
  ré-enregistrer par-dessus une version spécifique au jeu, sans que winx86
  ait besoin de savoir que cette substitution existe.

Un portage type : construit son propre `Cpu`/`Bridge`, charge son PE32 via
`PeImage`, enregistre ses shims Win32 et ses hooks natifs de point chaud via
ces deux primitives, lance l'ordonnanceur. Voir `d2vita` (consommateur
existant, via sous-module git) pour un exemple complet — `tools/rt_boot.cpp`
y fait exactement ça pour Diablo II.

## Construire

```
./build.sh              # cible qemu-arm/Linux (tests, dev) -> build-arm/libwinx86.a
TARGET=vita ./build.sh  # vraie cible PS Vita               -> build-vita/libwinx86_vita.a
```

Pour un simple contrôle de compilation desktop de la tranche portable
(chargeur PE32 + pont d'imports, sans le dynarec ARM) :

```
cmake -B build && cmake --build build
```

## Défauts connus, dette assumée

- Plusieurs variables d'environnement de réglage (`D2_WAKEPROF` dans
  `sched_native.cpp` ; `D2_JITPROFILE`, `D2_CALLRET`, `D2_FORWARD`,
  `D2_BUDGETTAIL`, `D2_NOPEND`, `D2_SIGNTAG`, `D2_MMUFOLD`, `D2_MMUSTACK`,
  `D2_NOLINK` dans `src/dynarec86/`) sont préfixées `D2_` bien que leur
  comportement soit entièrement générique — c'est un héritage du nom du
  projet d'origine, pas un couplage réel. Nettoyage prévu (probablement
  `WX86_*` avec alias `D2_*` conservés côté d2vita), pas encore fait.
- `src/runtime/ds_emul.{h,cpp}` (émulation DirectSound par table de vtable
  COM) mélange un mécanisme générique (fabriquer une vtable COM à partir
  d'une table de shims) avec des choix de mixage spécifiques à un jeu — reste
  côté d2vita pour l'instant, scission à faire.
- `src/platform/vita_present.cpp` (présentation à l'écran) et
  `src/platform/vita_net.{h,cpp}` (couche réseau — son auto-test intègre des
  vérifications spécifiques au protocole Battle.net) restent côté d2vita :
  mélange générique/spécifique pas encore démêlé pour le second, et non
  encore audité en détail pour le premier.
