# Démarrage rapide

## Cloner

```bash
git clone git@gitlab.com:claude5564407/winx86.git
cd winx86
```

## Construire pour qemu-arm / Linux (dev, tests)

C'est la cible par défaut — elle produit un binaire ARMv7 qui tourne sous
`qemu-arm` sur une machine de développement Linux, sans matériel Vita. C'est
le premier niveau de la méthodologie de validation à trois niveaux (voir la
documentation de d2vita) : rapide, permet de vérifier la correction, mais
**ses chiffres de performance ne sont jamais comparables à la console**.

```bash
./build.sh
# -> build-arm/libwinx86.a
```

## Construire pour la vraie PS Vita

```bash
TARGET=vita ./build.sh
# -> build-vita/libwinx86_vita.a
```

Nécessite le [VitaSDK](https://vitasdk.org/) installé et sur le `PATH`.

## Contrôle de compilation desktop (portable, sans dynarec ARM)

Pour vérifier rapidement que la tranche portable (chargeur PE32 + pont
d'imports) compile sur votre machine de dev, sans avoir besoin d'un
compilateur croisé ARM :

```bash
cmake -B build
cmake --build build
```

Ça construit `libwinx86_core.a` et `pe_analyze` (l'outil d'inspection de
table d'imports PE32).

## Ce que vous obtenez, ce que vous devez encore écrire

winx86 seul ne fait rien tourner : il n'y a ni `main()`, ni shim Win32, ni
hook de jeu. Après avoir compilé `libwinx86.a`, l'étape suivante est de lier
votre propre exécutable de boot contre cette bibliothèque et d'y écrire les
quelques centaines de lignes qui chargent votre PE32, enregistrent vos shims
et démarrent l'ordonnanceur — voir [Point d'extension](extension.md).
