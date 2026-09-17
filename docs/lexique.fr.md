# Lexique

Termes du moteur générique. Pour les termes spécifiques à un portage
particulier (Diablo II, MPQ, Battle.net...), voir le lexique du portage
concerné — celui de
[d2vita](https://franckrst.github.io/D2Vita/lexique/) par exemple.

**PE32** — format binaire des exécutables et DLL Windows 32 bits
(*Portable Executable*). Ce que `PeImage` charge.

**RVA** — *Relative Virtual Address*, adresse relative à la base de
chargement d'un module PE. La plupart des adresses citées dans un hook natif
sont des RVA (`base_du_module + 0x...`), pas des adresses absolues.

**Dynarec** — *dynamic recompiler/translator* : traduit du code machine x86
vers du code ARM au moment de l'exécution, par blocs, avec mise en cache des
blocs déjà traduits (contrairement à un interprète qui réinterprète chaque
instruction à chaque passage, ou à une compilation statique complète en
amont).

**Shim** — implémentation native fournie en remplacement d'une fonction
Windows importée (ex: `KERNEL32.dll!GetVersion`), enregistrée via
`Bridge::register_shim`.

**Trap** — mécanisme par lequel le dynarec s'arrête et rend la main au code
natif hôte à une adresse donnée, plutôt que de continuer à traduire/exécuter
du code invité. `Bridge::shim_trap` obtient l'adresse de trap associée à un
shim enregistré.

**`set_alternate`** — voir [Point d'extension](extension.md) :
redirige l'exécution d'une adresse invité (toujours une entrée de fonction)
vers une autre.

**Hook natif de point chaud** — un shim posé via `set_alternate` sur une
fonction du jeu (pas une vraie API Windows) dans un but de performance ou de
diagnostic, généralement avec un repli fidèle vers le code original.

**Repli fidèle** — dans un hook, rejouer manuellement l'effet du prologue
original de la fonction interceptée (ex: `push ebp`) avant de reprendre
l'exécution, pour que le hook soit transparent quand il n'a rien à changer
au comportement.

**GIL** — *Global Interpreter Lock*, verrou global sérialisant l'accès au
dynarec entre plusieurs fils natifs (à la manière du GIL de CPython) —
nécessaire car le dynarec n'est pas thread-safe par construction.

**Code auto-modifiant (SMC)** — code qui écrit dans sa propre mémoire de
code pendant l'exécution. Nécessite d'invalider ou de détruire les blocs
déjà traduits pour cette plage d'adresses (`Cpu::invalidate_code`/
`discard_code`).

**COM (vtable)** — modèle de composants Windows où un objet est exposé par
une table de pointeurs de fonctions (*vtable*). `src/runtime/ds_emul.cpp`
(au moteur) fabrique une vtable COM DirectSound entièrement à partir d'une
table de shims, sans vraie classe C++ sous-jacente.
