# Point d'extension — comment porter votre jeu

winx86 ne contient **aucun hook de jeu** et **aucune connaissance d'un binaire
particulier**. Il fournit deux primitives d'accrochage, une bibliothèque de
shims Win32 dont il a été **prouvé** qu'ils ne portent aucun comportement propre
à un jeu, et quelques points d'extension neutres par lesquels un portage injecte
sa politique à lui.

## Les deux primitives

### `Cpu::set_alternate(from_va, to_va)`

`src/runtime/cpu.h`. Redirige l'exécution d'une adresse invité vers une autre.
C'est le mécanisme de « crochet à l'entrée d'une fonction ».

!!! warning "Toujours une adresse d'ENTRÉE de fonction"
    Jamais une cible de saut interne. Le dynarec fusionne des blocs de code x86
    contigus en un seul bloc traduit ; un crochet posé sur une cible de saut
    interne à un bloc déjà fusionné ne sera jamais revisité — le crochet devient
    **muet sans erreur visible**. Documentez cette adresse comme un vrai point
    d'entrée (généralement un prologue `push ebp` / `push ebx` reconnaissable au
    désassemblage).

### `Bridge::register_shim(dll, name, shim)` / `Bridge::shim_trap(dll, name)`

`src/runtime/bridge.h`. `register_shim` enregistre une implémentation native
sous une clé `"DLL.dll"` + `"NomDeFonction"` ; `shim_trap` obtient l'adresse de
trap correspondante, à passer à `set_alternate`.

!!! danger "`register_shim` écrase la clé"
    Enregistrer deux fois la même clé ne produit aucune erreur : **la dernière
    inscription gagne**, silencieusement. C'est une fonctionnalité — elle permet
    à un portage de substituer sa version à un défaut du moteur sans que winx86
    le sache — mais c'est aussi le piège classique quand on réorganise du code
    qui enregistre beaucoup de shims. Un doublon de ce genre a déjà cassé la
    connexion réseau d'un portage, et rien ne l'avait signalé.

    `tools/shim_seq.py` / `.sh` existent exactement pour ça — voir
    [Outils IA](outils-ia.md).

## Les shims que le moteur fournit

La liste exacte est [générée depuis les sources](shims.md) et vérifiée par la
CI. En résumé : registre et sécurité (ADVAPI32), objets GDI factices, géométrie
et sémantique de fenêtre, énumération de modules, ressources de version, lecteurs
vidéo et IME rapportés absents, et une couche socket complète.

**Aucune de ces fonctions d'installation n'est appelée automatiquement.** Le
portage appelle celles qu'il veut, depuis son propre binaire de boot :

```cpp
#include "runtime/win32_shims_advapi32.h"
#include "runtime/win32_shims_wsock32.h"

win32_shims_advapi32_install(br);
win32_shims_wsock32_install(br);
// ... puis vos propres shims, qui peuvent écraser n'importe laquelle des clés
//     ci-dessus si votre jeu a besoin d'un comportement différent.
```

Un moteur qui inscrirait des shims d'autorité déciderait à la place de son
consommateur. L'ordre est le vôtre, et la dernière inscription gagne.

## Les points d'extension neutres

Quand une fonctionnalité est générique mais que la **politique** ne l'est pas, le
moteur expose un point d'extension plutôt que de deviner. Trois exemples, tous
issus de cas réels :

### L'allocateur de brouillon invité

`src/runtime/guest_scratch.h`. Beaucoup d'API Win32 ne rendent pas une valeur
mais un **pointeur** vers de la mémoire que l'appelant lira — `gethostbyname`
rend un `hostent*`, `inet_ntoa` un `char*`, `GetCommandLineA` une chaîne. Ce
pointeur doit être une adresse **invitée**, donc il faut écrire le résultat dans
l'espace mémoire de l'invité.

Le moteur possède l'allocateur ; le portage lui concède une plage, parce que le
plan mémoire, lui, n'a rien d'universel :

```cpp
wx86_scratch_init(base, size);        // le portage déclare la plage
wx86_scratch_set_oom_handler(&mon_journal);
```

C'est un allocateur à pointeur croissant qui **ne libère jamais** : il est réservé
aux valeurs de retour à durée de vie processus. Un site qui y alloue à **chaque
appel** sur un chemin répété finira par l'épuiser — c'est arrivé, et le correctif
est de mettre le résultat en cache, pas d'agrandir la plage.

### L'observateur réseau

`src/runtime/win32_shims_wsock32.h`. **Un seul** point d'observation passif sur
toute la couche socket : `connect`, `connect-done`, `send`, `recv`, `close`,
`resolve`. Le moteur **raconte** ce qui se passe ; il ne demande jamais d'avis et
ne change jamais de comportement selon la réponse.

Tout ce qui est protocole, cadrage de paquets, journalisation ou instrumentation
appartient au portage, qui enregistre **un** observateur et dispatche en interne.
Rien dans cette interface ne nomme un protocole, un port ou un produit.

```cpp
wx86_net_set_observer(&mon_observateur);   // exactement un, le second remplace
```

### La route de connexion

`wx86_net_set_redirect(ip)` réécrit toute connexion non-locale vers une adresse
donnée, en gardant le port — l'équivalent moral d'une entrée de fichier hosts.

!!! note "C'est un filet, pas la méthode normale"
    Pour pointer un client vers un serveur privé, la façon fidèle est celle d'un
    vrai PC : configurer la **liste de serveurs du client lui-même** (ses clés de
    registre, son `.ini`) pour qu'il demande votre hôte dès le départ. Mesuré sur
    un portage : une fois la liste réduite à l'adresse locale, la redirection
    devenait entièrement inutile pour tout le trajet applicatif. Elle ne reste
    utile que pour une sonde partant sur une adresse littérale, sans passer par
    un nom.

## Le schéma général d'un portage

1. Construire son `Cpu` (`CpuBox86`) et son `Bridge`.
2. Charger son PE32 via `PeImage`.
3. Déclarer la plage de brouillon invité (`wx86_scratch_init`).
4. Appeler les `win32_shims_*_install()` voulues, **puis** enregistrer ses
   propres shims — les siens gagnent, puisque la dernière inscription gagne.
5. Brancher ses politiques sur les points d'extension neutres (observateur
   réseau, route, etc.).
6. Enregistrer ses hooks de performance natifs sur les adresses chaudes de *son*
   binaire via `set_alternate` + `shim_trap`.
7. Démarrer l'ordonnanceur (coopératif ou natif).

## Exemple complet : un crochet d'observation à repli fidèle

Ce hook intercepte l'entrée d'une fonction du jeu à une adresse précise, compte
des événements, puis **rejoue fidèlement** le prologue original avant de laisser
le dynarec continuer — un repli qui ne change rien au comportement, seulement
instrumenté :

```cpp
void install_watch(Cpu* cpu, Bridge& br){
    static uint32_t s_entry = MOD_BASE + 0x2092d0;   // adresse d'ENTRÉE

    Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!watch";
    s.fn = [&br](Cpu& c) -> uint32_t {
        const uint32_t E = c.reg(R_ESP);
        ++g_count;
        // Repli fidèle : rejoue le prologue original (`push ebx`) puis
        // reprend juste après, comme si le hook n'avait jamais existé.
        c.write_u32(E - 4, c.reg(R_EBX));
        c.set_reg(R_ESP, E - 8);
        br.redirect_next(s_entry + 1);
        return c.reg(R_EAX);
    };

    br.register_shim("native.hook", "watch", s);
    cpu->set_alternate(s_entry, br.shim_trap("native.hook", "watch"));
}
```

À retenir :

- La clé DLL `"native.hook"` est une **convention du portage** pour les crochets
  qui ne correspondent à aucune vraie DLL Windows, pas une exigence de winx86.
- Un hook peut être conditionnel (armé par une variable d'environnement) pour du
  diagnostic optionnel ou de l'A/B de performance.
- Le « repli fidèle » est le patron le plus sûr pour un hook qui ne fait
  qu'observer : aucun risque de divergence, seulement un trap par appel.

## Pourquoi certains shims restent chez le portage {#frontiere}

Un audit mené en tentant d'extraire les shims Win32 d'un portage a trouvé du
contenu spécifique au jeu caché derrière des noms parfaitement génériques : un
chemin d'installation codé en dur dans un `SHGetFolderPathA`, une émulation
d'authentification propre au protocole du jeu dans un shim `CryptoAPI`. Rien
dans la signature ne le laissait deviner.

La conclusion d'alors — « aucun shim Win32 n'est générique par nature » — était
juste sur le constat et **trop pessimiste sur la suite**. Ce qui a marché
ensuite, et qui a produit la bibliothèque listée dans [Shims fournis](shims.md) :

**classer chaque fonction par son CORPS, jamais par son nom d'API.**

| classe | critère | destination |
|---|---|---|
| générique | aucun littéral, branche ou dépendance d'état propre à un jeu ; aucune dépendance à un helper du portage | le moteur |
| spécifique | un littéral en dur, une dépendance à l'état du jeu, un contournement | le portage |
| à risque | dépendance non résolue, risque d'inscription en double | reste en place, **avec la raison écrite dans le code** |

Deux critères de tranchage, appris à la dure :

- **Le jeu inspecte-t-il le *contenu* de la valeur rendue, ou seulement son
  succès ?** Un shim dont seul le succès compte est générique ; un shim dont le
  contenu est examiné porte une connaissance du jeu. Ça se tranche au
  désassemblage, pas à la signature.
- **« Trop enchevêtré » n'est une conclusion acceptable que sur preuve concrète
  d'un danger** — pas sur la difficulté. Deux chantiers conclus « pas rentable »
  ont été renversés en refaisant le travail de conception pour de bon, et les
  deux fois le découplage a tenu.

La procédure complète est dans `.claude/agents/shim-split.md`.
