# Architecture

## Vue d'ensemble

```
Exécutable Windows (.exe/.dll)
        │
   Chargeur PE32 (src/runtime/pe_image.h, .cpp)
        │
   Interface Cpu (src/runtime/cpu.h)
        │
        ├── CpuBox86 (src/runtime/cpu_box86.cpp) — backend ARMv7, dynarec
        │
   Bridge (src/runtime/bridge.h, .cpp) — point d'extension
        │
   Ordonnanceur de fils invités
        ├── sched_cooperative.* — coopératif (un fil actif à la fois)
        └── sched_native.* + gil.* — fils natifs + verrou global (GIL)
```

## Le chargeur PE32

`PeImage` (`src/runtime/pe_image.*`) mappe un exécutable ou une DLL Windows
x86 32 bits en mémoire : parse les en-têtes PE/COFF, résout la table
d'imports, applique les relocations si le binaire n'est pas chargé à son
adresse préférée. Générique à tout PE32 valide — aucune connaissance du
contenu du binaire.

## L'interface Cpu

`Cpu` (`src/runtime/cpu.h`) est l'abstraction centrale : un jeu de registres
x86, une mémoire adressable, et les primitives d'exécution/traduction. Le
seul backend fourni aujourd'hui est `CpuBox86` (ARMv7 via le dynarec), mais
l'interface elle-même ne présuppose pas de backend particulier.

Deux primitives de l'interface `Cpu` sont **le** point d'extension du moteur
— détaillées dans [Point d'extension](extension.md) :

- `Cpu::set_alternate(from_va, to_va)` — redirige l'exécution d'une adresse
  invité vers une autre.
- `Cpu::discard_code(va, size)` / `invalidate_code(va, size)` — contrat SMC
  (code auto-modifiant) : la distinction entre « marquer sale pour
  re-vérification » et « détruire sans condition les blocs traduits » à cette
  adresse, nécessaire à tout portage qui patche du code invité en mémoire.

## Le dynarec (Box86)

`third_party/box86-dynarec/` est une extraction **verbatim** du cœur
ARMv7 de [Box86](https://github.com/ptitSeb/box86) (ptitSeb, MIT), plus un
jeu de patches locaux appliqué par-dessus et régénéré mécaniquement — jamais
édité à la main :

```bash
tools/regen_box86_patch.sh   # capture le delta local actuel -> tools/box86_local_patches.diff
tools/extract_box86.sh       # ré-extrait un Box86 propre + réapplique le patch, refuse si le résultat diverge de l'arbre existant
```

Cette mécanique existe pour une raison précise : pouvoir re-synchroniser
contre une version plus récente de Box86 en amont sans perdre silencieusement
les patches locaux (fastmmu, memintrin, emitprof, signtag, le point
d'accroche de préemption de l'ordonnanceur, l'interrupteur de protection en
écriture SMC). Les patches sont conçus pour être proposables en amont à
Box86 lui-même.

`src/dynarec86/` est la couche d'intégration entre Box86 et le reste de
winx86 (interface `Cpu`, reconnaissance d'intrinsèques natifs à la
traduction — voir plus bas).

## Reconnaître un intrinsèque natif à la traduction

`src/dynarec86/dyn86_intrin.h` fournit un mécanisme générique pour
reconnaître, **au moment de la traduction** (pas via un trap à l'exécution),
qu'une séquence x86 correspond à une fonction connue et la remplacer par un
appel natif direct. C'est plus rapide qu'un hook classique (`set_alternate`
+ trap) mais demande que le remplacement soit un vrai code natif équivalent,
pas juste un raccourci. Un portage l'utilise pour ses fonctions les plus
chaudes (voir comment d2vita l'utilise pour son décodeur de sprites DCC,
côté d2vita).

## La piscine JIT sur PS Vita — segments de 16 Mio

Le code ARM traduit par le dynarec est stocké dans de la mémoire exécutable
(`sceKernelAllocMemBlockForVM`, domaine noyau VM — distinct des blocs RW
classiques `sceKernelAllocMemBlock`). `src/dynarec86/shim/vita/mman_vita.c`
implémente cette « piscine JIT » côté portage Vita.

Découverte le 2026-09-13 (jamais testée avant sur ce projet) : le noyau Vita
plafonne **chaque bloc VM à 16 Mio (0x1000000)**, refusé au-delà avec
`SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW` (sce=0x80024B0B) — vérifié en demandant
17 Mio puis 29 Mio d'un seul tenant, refusés tous les deux. C'est une limite
noyau, pas un choix de dimensionnement du projet.

La piscine grandit donc par **segments** de 16 Mio ouverts paresseusement
(`jitpool_grow()`), et non comme un unique bloc agrandi : `WX86_JITPOOL_SEGS`
(alias `D2_JITPOOL_SEGS`) fixe le nombre de segments visés (2 par défaut →
32 Mio), `WX86_JITPOOL_MB`/`D2_JITPOOL_MB` la taille de chacun (plafonnée à
16 Mio par segment). Un échec d'ouverture arme un drapeau global unique
(`g_jitseg_refused`, posé seulement après un refus RÉEL du noyau, jamais par
anticipation) qui arrête toute nouvelle tentative pour le reste de la
session — pas de martèlement du noyau à chaque bloc `PROT_EXEC` suivant. Ce
drapeau ne bloque que la **croissance** : les segments déjà ouverts avant
l'échec restent pleinement utilisables, chacun étant une entrée séparée du
tableau de segments.

Le découpage en segments ne coûte rien en performance : la liaison entre
blocs traduits est **toujours** un branchement indirect à adresse absolue
(`CreateJmpNext` dans `box86-dynarec` : `LDR_literal`+`BX` ; retour d'appel
via `BXcond`/`BX` après vérification de pile ou table de sauts), jamais un
`B`/`BL` ARM direct à portée limitée — l'éloignement entre segments n'a donc
aucun impact d'adressage.

## Ordonnanceurs

Deux stratégies pour les fils invités (`CreateThread`, etc.) :

- **Coopératif** (`sched_cooperative.*`) — un seul fil du jeu s'exécute à la
  fois, commutation explicite. Plus simple, plus déterministe, plus lent
  sous forte contention.
- **Natif** (`sched_native.*` + `gil.*`) — vrais fils du système
  d'exploitation, sérialisés par un verrou global (GIL, à la Python) pour
  protéger le dynarec, qui n'est pas thread-safe par construction. Permet un
  vrai parallélisme sur les portions de code natif qui libèrent
  volontairement le GIL.

Le choix se fait au niveau du portage, pas de winx86.

## Le Bridge — pont d'imports Win32

`Bridge` (`src/runtime/bridge.h`, `.cpp`) est le mécanisme générique
d'enregistrement de shims Win32 : `register_shim(dll, name, shim)` associe
un nom de fonction DLL+symbole à une implémentation native, `shim_trap(dll,
name)` obtient l'adresse de trap à donner à `set_alternate`.

Par-dessus ce mécanisme, `src/runtime/win32_shims_*.cpp` fournit une
bibliothèque de shims dont il a été **prouvé, corps par corps**, qu'ils ne
portent aucun comportement propre à un jeu — la liste exacte est
[générée depuis les sources](shims.md). Chacun s'installe par un appel
explicite du portage (`win32_shims_<groupe>_install(br)`) : le moteur n'en
inscrit aucun de sa propre initiative, et comme la dernière inscription gagne,
un portage peut toujours substituer la sienne.

## Services génériques exposés au portage

Quand une fonctionnalité est générique mais que la politique ne l'est pas, le
moteur expose un point d'extension neutre plutôt que de deviner :

- **`guest_scratch.{h,cpp}`** — l'allocateur de brouillon en mémoire invitée,
  nécessaire à toute API Win32 qui rend un *pointeur* que l'appelant lira. Le
  moteur alloue, le portage lui concède une plage (le plan mémoire, lui, n'a
  rien d'universel).
- **`win32_shims_wsock32.h`** — la table de poignées de sockets, l'unique
  magasin de dernière erreur Winsock, un observateur passif de la couche socket
  (le moteur raconte, il ne demande jamais d'avis) et une route de connexion
  générique.
- **`net_nonblock.{h,cpp}`**, **`poll_gil.{h,cpp}`** — résolution de noms et
  attente compatible avec le verrou global.

Voir [Point d'extension](extension.md).
