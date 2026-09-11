---
name: shim-split
description: Use when moving Win32 shims (or any registered hook) between a port and the engine — auditing a DLL group for what is genuinely generic, extracting it into winx86, and leaving the rest behind with a written reason. Enforces the shim_seq safety net and the one-registration-per-key rule.
---

# Découper un groupe de shims entre le moteur et le portage

Cette procédure a servi à déplacer plus de deux cents inscriptions depuis un
fichier de boot monolithique vers le moteur, groupe de DLL par groupe de DLL,
sans casser une seule fois la table effective. Elle est écrite ici parce que
l'improviser coûte une régression silencieuse.

## La règle qui décide : le CORPS, jamais le nom

**Un nom d'API Win32 ne dit rien sur la généricité du code qui l'implémente.**
Un audit a trouvé un chemin d'installation codé en dur dans un
`SHGetFolderPathA`, et une émulation d'authentification propre à un protocole de
jeu dans un shim `CryptoAPI`. Rien dans la signature ne le laissait deviner.

Classer chaque fonction en lisant son corps :

| classe | critère | destination |
|---|---|---|
| **générique** | aucun littéral, branche ou dépendance d'état propre à un jeu ; aucune dépendance à un helper hébergé par le portage | winx86 |
| **spécifique** | un littéral en dur, une dépendance à l'état du jeu, un contournement pour ce jeu-là | reste au portage |
| **à risque** | dépendance non résolue, ou risque d'inscription en double | reste en place, **avec la raison écrite dans le code** |

La troisième colonne est la plus importante. Un shim laissé en place sans raison
écrite sera re-audité de zéro dans six mois.

## Le seuil pour dire « trop enchevêtré »

« Trop enchevêtré » n'est une conclusion acceptable que sur **preuve concrète
d'un danger** : une dépendance qu'on a cherché à casser et qui résiste, un risque
d'inscription double identifié, un comportement que seul un test qu'on ne peut
pas faire tourner validerait.

**La difficulté seule n'est jamais une raison.** Deux fois sur ce projet, une
conclusion « pas rentable à extraire » a été renversée en refaisant le travail de
conception pour de bon — et les deux fois, le découplage a marché. Si tu conclus
« ça reste », le rapport doit dire *quelle preuve* t'y a conduit.

## La règle dure : UNE seule inscription par clé

`Bridge::register_shim` **écrase** silencieusement : inscrire deux fois la même
clé ne produit aucune erreur, et c'est la **dernière** inscription qui gagne.

C'est une fonctionnalité — un portage peut poser un défaut puis le substituer —
et c'est le piège classique de tout déplacement de bloc. Un cas réel a cassé la
connexion réseau d'un jeu parce qu'une extraction avait laissé une inscription
morte *après* la bonne.

**Donc : chaque clé doit avoir exactement UN site d'inscription total**, moteur
et portage confondus. Quand un doublon hérité doit être conservé (ça arrive),
garde l'ordre relatif d'origine pour que le corps gagnant ne change pas, et
documente-le en tête de fichier.

## Le filet : `shim_seq`, avant ET après

`tools/shim_seq.py` / `.sh` reconstruit la séquence ordonnée des inscriptions
avec l'empreinte du corps de chacune, et compare deux états. Trois verdicts :

- **ENSEMBLE** — une clé apparaît ou disparaît, ou change de multiplicité.
  Toujours fatal.
- **EFFECTIF** — même jeu de clés, mais un autre corps gagne pour au moins une
  clé. Toujours fatal : c'est exactement ce qu'un déplacement raté produit.
- **ORDRE** — même table effective, séquence différente. Fatal par défaut ;
  acceptable pour un refactor qui déplace des blocs entiers sans changer la
  table effective.

`tools/shim_seq.allow` documente les transitions de corps **volontaires** (clé,
empreinte avant, empreinte après, raison). Jamais un blanc-seing : une dérive
ultérieure de la même clé redevient fatale.

!!! danger "L'outil a eu de vrais bugs — vérifie qu'il voit vraiment"
    Deux ont été trouvés en cours de route : des clés situées après un point de
    découpe disparaissaient silencieusement du relevé, et une différence de
    locale entre `sort` et `comm` produisait quatre fausses alertes. Un filet
    qui ne prouve pas qu'il attrape est pire qu'un filet absent. Si le verdict
    te surprend, soupçonne l'outil **avant** de soupçonner ton déplacement.

## Le patron d'extraction

Un fichier dédié par groupe de DLL, avec une fonction d'installation que le
portage appelle explicitement depuis son propre binaire de boot :

- moteur : `src/runtime/win32_shims_<groupe>.cpp` → `win32_shims_<groupe>_install(Bridge&)`
- portage : `win32_shims_<groupe>_d2.cpp` (ou l'équivalent) pour le résidu

Le moteur n'appelle **jamais** ces fonctions tout seul : c'est le portage qui
décide quels shims il veut. Un moteur qui inscrit des shims d'autorité décide à
la place de son consommateur.

### Une contrainte du parseur à ne pas violer

`shim_seq.py` ne résout **qu'un seul niveau** de lambda locale : il cherche un
appel direct à `br.register_shim(_ordinal)` dans le corps du helper. Un helper
qui appelle un autre helper résout à zéro cible, et **toutes les clés qu'il
inscrit disparaissent silencieusement du filet**. Ce bug s'est produit une fois,
et n'a été vu que parce que le contrôle ENSEMBLE tournait avant le commit.

N'écris pas de helper de second niveau. Répète l'appel.

## Déroulé

1. **Cartographier en lecture seule.** Limites exactes (souvent non contiguës),
   inventaire des symboles qui traversent la frontière. Ne toucher à aucun
   fichier à cette étape.
2. **Relever la séquence de référence** : `tools/shim_seq.sh` avant tout
   changement.
3. **Classer fonction par fonction** selon le tableau ci-dessus, en lisant les
   corps. Pour les cas douteux, va voir au désassemblage si le jeu inspecte
   vraiment le **contenu** de la valeur rendue, ou seulement son succès — c'est
   la différence entre un shim spécifique et un shim générique, et elle ne se
   devine pas depuis la signature.
4. **Déplacer mécaniquement**, sans changer la logique. Chaque symbole qui
   traverse la frontière devient un `extern` ou un point d'extension explicite.
   Tout changement de comportement — même le retrait d'une trace — se **déclare**
   en tête de fichier.
5. **Revérifier les limites au moment du déplacement**, pas seulement à la
   cartographie : le code bouge, et un helper supprimé « parce qu'inutilisé »
   peut être réutilisé cent lignes plus loin.
6. **Valider** : `shim_seq` (ENSEMBLE/EFFECTIF/ORDRE), build des deux dépôts
   (bureau **et** ARM), et un oracle déterministe rejoué **deux fois**. Ne
   jamais commiter sur la seule preuve de compilation.
7. **Si ce n'est pas un mouvement propre, s'arrêter et le dire.** Une extraction
   bâclée qui « a l'air de marcher » est pire qu'aucune extraction.

## Cas particulier : le code qui touche au réseau actif

Un oracle qui démarre jusqu'au menu ne prouve **rien** sur `connect`, `recv`,
`send` ou `select`. Pour ces chemins, la validation exige un vrai échange contre
un serveur local, avec comparaison du trafic avant/après. Voir l'agent
correspondant côté portage, qui détient le protocole et ses pièges.

## Rapport attendu

- ce qui est devenu générique, et ce qui a été supprimé comme mort (avec la
  preuve)
- ce qui reste au portage, **avec la raison technique**, pas « par prudence »
- ce qui a été réellement testé, et par quel moyen — en distinguant testé en
  conditions réelles de testé en isolation
- tout chiffre annoncé doit être re-vérifié avant d'être écrit dans un message de
  commit
