# Limites connues / dette technique

Cette page liste honnêtement ce qui n'est pas encore fait, plutôt que de le
laisser découvrir en silence.

## Namespace des variables d'environnement `D2_*`

Plusieurs variables de réglage entièrement génériques portent encore un
préfixe `D2_`, héritage du nom du projet d'origine (d2vita) :
`D2_WAKEPROF` (`sched_native.cpp`), `D2_JITPROFILE`, `D2_CALLRET`,
`D2_FORWARD`, `D2_BUDGETTAIL`, `D2_NOPEND`, `D2_SIGNTAG`, `D2_MMUFOLD`,
`D2_MMUSTACK`, `D2_NOLINK` (`src/dynarec86/`). Leur comportement est
entièrement générique — c'est un problème de nom, pas de couplage réel — mais
ça peut induire en erreur un nouveau consommateur qui chercherait un lien
avec Diablo II là où il n'y en a pas. Nettoyage prévu, probablement vers un
préfixe neutre (`WX86_*`) avec des alias `D2_*` conservés côté d2vita pour
compatibilité. Pas encore fait.

## `src/runtime/ds_emul.{h,cpp}` — reste côté d2vita

Émulation DirectSound par fabrication d'une vtable COM à partir d'une table
de shims. Le *mécanisme* (fabriquer une vtable COM générique depuis une
table de pointeurs de fonctions) est réutilisable tel quel par n'importe
quel jeu utilisant DirectSound ; le fichier actuel mélange ce mécanisme avec
des choix de mixage audio spécifiques au jeu d'origine. Une scission propre
(mécanisme générique dans winx86, choix de mixage côté portage) est
identifiée mais pas encore faite.

## Présentation écran et réseau — restent côté d2vita

`vita_present.cpp` (présentation à l'écran) n'a pas encore été auditée en
détail pour en extraire une éventuelle partie générique. `vita_net.{h,cpp}`
(couche réseau) a un cas plus net : son auto-test embarque des vérifications
du protocole réseau spécifique au jeu d'origine (poignée de main, serveur
par défaut) — mélangé au reste du fichier plutôt que proprement séparé.

## Les ~600 shims Win32 de d2vita ne sont PAS dans winx86, et ne le seront
## peut-être jamais tels quels

Voir [Point d'extension](extension.md#pourquoi-winx86-ne-fournit-aucun-shim-meme-generique)
pour le pourquoi. Ce n'est pas un oubli — c'est une décision de conception :
même un shim au nom d'API Windows standard peut porter un comportement
spécifique à un jeu, et il n'existe pas de façon fiable de le détecter en
bloc sans auditer chaque shim individuellement. Une bibliothèque de shims
« de base » réellement vérifiés génériques (par exemple via la technique de
comparaison à deux portages, voir
[Outils IA](outils-ia.md#une-technique-nommee-la-genericite-par-comparaison))
reste une direction possible, pas encore entreprise faute d'un audit complet.
