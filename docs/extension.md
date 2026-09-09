# Point d'extension — comment porter votre jeu

winx86 ne contient **aucun** shim Win32, **aucun** hook de jeu, **aucune**
connaissance d'un binaire particulier. Deux primitives génériques suffisent
à tout brancher.

## Les deux primitives

### `Cpu::set_alternate(from_va, to_va)`

`src/runtime/cpu.h`. Redirige l'exécution d'une adresse invité vers une
autre. C'est le mécanisme de « crochet à l'entrée d'une fonction ».

!!! warning "Toujours une adresse d'ENTRÉE de fonction"
    Jamais une cible de saut interne. Le dynarec fusionne des blocs de code
    x86 contigus en un seul bloc traduit ; un crochet posé sur une cible de
    saut interne à un bloc déjà fusionné ne sera jamais revisité — le crochet
    devient muet sans erreur visible. C'est une leçon vécue par le projet qui
    a donné naissance à winx86 : documentez cette adresse comme un vrai point
    d'entrée (généralement un prologue `push ebp` / `push ebx` reconnaissable
    au désassemblage).

### `Bridge::register_shim(dll, name, shim)` / `Bridge::shim_trap(dll, name)`

`src/runtime/bridge.h`. `register_shim` enregistre une implémentation native
sous une clé `"DLL.dll"` + `"NomDeFonction"` ; `shim_trap` obtient l'adresse
de trap correspondante, à passer à `set_alternate`.

!!! danger "`register_shim` écrase la clé"
    Enregistrer deux fois la même clé ne fait pas d'erreur : **la dernière
    inscription gagne**, silencieusement. C'est une fonctionnalité — ça
    permet à un consommateur de fournir un défaut générique puis de le
    substituer par une version spécifique au jeu sans que winx86 le sache —
    mais c'est aussi le piège classique quand on réorganise du code qui
    enregistre beaucoup de shims (typiquement en extrayant un bloc d'un
    fichier monolithique vers son propre fichier) : déplacer un bloc peut
    changer *l'ordre* des inscriptions et donc *quelle* implémentation gagne,
    sans qu'aucune compilation ne le voie. `tools/shim_seq.py`/`.sh` existent
    pour ça — voir [Outils IA / agents réutilisables](outils-ia.md).

## Exemple complet, tiré tel quel de d2vita

Ce hook (extrait de `native_hooks_codec.cpp` côté d2vita) intercepte
l'entrée d'une fonction du jeu à une adresse précise, compte des événements,
puis **rejoue fidèlement** le prologue original avant de laisser le dynarec
continuer à partir de l'instruction suivante — un repli qui ne change rien
au comportement du jeu, seulement instrumenté :

```cpp
void install_celwatch(Cpu* cpu, Bridge& br){
    if(g_celWatch && g_114 && g_d2base){
        static uint32_t s_evEntry=0, s_celObj=0;
        s_evEntry = g_d2base + 0x2092d0;   // adresse d'ENTRÉE de la fonction, dans le binaire du jeu
        s_celObj  = g_d2base + 0x48db20;

        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!celdata_evict";
        s.fn = [&br](Cpu& c) -> uint32_t {
            const uint32_t E = c.reg(R_ESP);
            if (c.reg(R_EDX) == 0) {
                if (c.reg(R_ECX) == s_celObj) ++g_celEvict;
                else ++g_celEvictOther;
            }
            // Repli fidèle : rejoue le prologue original (`push ebx`) puis
            // reprend juste après, comme si le hook n'avait jamais existé.
            c.write_u32(E - 4, c.reg(R_EBX));
            c.set_reg(R_ESP, E - 8);
            br.redirect_next(s_evEntry + 1);
            return c.reg(R_EAX);
        };

        br.register_shim("native.hook", "celdata_evict", s);
        cpu->set_alternate(s_evEntry, br.shim_trap("native.hook", "celdata_evict"));
    }
}
```

Points à retenir de cet exemple :

- La clé DLL est `"native.hook"` par convention côté d2vita pour les hooks
  qui ne correspondent à *aucune* vraie DLL Windows (contrairement aux vrais
  shims Win32 enregistrés sous `"KERNEL32.dll"`, `"USER32.dll"`, etc.) — un
  choix du consommateur, pas une exigence de winx86.
- Le hook est conditionnel (`if(g_celWatch && ...)`) : rien n'empêche un
  portage de n'armer certains hooks que sous une variable d'environnement,
  pour du diagnostic optionnel ou de l'A/B testing de performance.
- Le « repli fidèle » (rejouer le prologue original puis reprendre juste
  après) est le patron le plus sûr pour un hook qui ne fait qu'observer :
  aucun risque de divergence de comportement, seulement un coût d'un trap
  par appel.

## Le schéma général d'un portage

1. Construire son propre `Cpu` (`CpuBox86`) et `Bridge`.
2. Charger son PE32 via `PeImage`.
3. Enregistrer ses shims Win32 (`KERNEL32.dll`, `USER32.dll`, ...) via
   `register_shim` — winx86 n'en fournit aucun, voir plus bas pourquoi.
4. Enregistrer ses hooks de performance natifs sur les adresses chaudes de
   *son* binaire via `set_alternate` + `shim_trap`, comme l'exemple ci-dessus.
5. Démarrer l'ordonnanceur (coopératif ou natif).

d2vita (`tools/rt_boot.cpp`) fait exactement ça, à l'échelle de ~600 shims
Win32 et d'une vingtaine de hooks de performance sur des adresses précises
du binaire de Diablo II. Sa documentation détaille ce qui reste
volontairement hors de winx86 et pourquoi.

## Pourquoi winx86 ne fournit aucun shim, même « générique »

Un audit du 2026-09-09, mené en tentant justement d'extraire les shims Win32
de d2vita vers winx86, a trouvé du contenu spécifique au jeu caché dans des
shims au nom parfaitement générique : un chemin Windows codé en dur
(`SHGetFolderPathA` renvoyant `"C:\Diablo II"`), une émulation
d'authentification propre au protocole de connexion du jeu dans un shim
`CryptoAPI`. Rien dans la signature de ces fonctions ne le laissait deviner.
La conclusion : **il n'existe pas de shim Win32 « générique par nature » —
seulement des shims qui n'ont, pour l'instant, pas encore eu besoin de
diverger.** Mieux vaut qu'un portage écrive les siens dès le départ via
cette API que d'hériter silencieusement d'un comportement pensé pour un
autre jeu.
