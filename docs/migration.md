# Migrer un portage existant vers le moteur

[Point d'extension](extension.md) explique comment démarrer un portage **neuf**
sur winx86. Ce document traite le cas inverse, et plus fréquent : un portage qui
existe déjà, qui a son propre runtime Win32 complet, et qui doit **cesser de
dupliquer** ce que le moteur possède désormais.

C'est le cas de tout portage né par copie d'un autre — la façon la plus rapide
d'en démarrer un, et celle qui produit deux bases qui divergent ensuite.

!!! abstract "Le principe directeur"
    **Chaque endroit où un second portage bascule sur le code du moteur est une
    preuve de généricité. Chaque endroit où il ne peut pas est un défaut de
    conception trouvé gratuitement.**

    Un moteur n'est pas prouvé générique parce qu'on l'a conçu générique. Il
    l'est quand un deuxième consommateur s'en sert réellement. Tant que ce n'est
    pas fait, le moteur contient une *copie* du code qui vit aussi dans les deux
    portages : trois exemplaires au lieu de deux, ce qui est pire que le point
    de départ.

## État des lieux, chiffré

Tous les chiffres ci-dessous sont relevés sur l'arbre, pas estimés.

### Ce que le moteur possède

| | |
|---|---:|
| Sources du moteur (`src/`) | 13 764 lignes |
| Inscriptions de shims Win32 | 297 |
| DLL couvertes (au moins partiellement) | 14 |
| Unités d'installation (`win32_shims_*_install`) | 11 |

Au-delà des shims, le moteur possède les briques transverses qu'un portage
écrivait autrefois lui-même :

| brique | fichier | ce qu'elle remplace chez le portage |
|---|---|---|
| allocateur de brouillon invité | `runtime/guest_scratch` | un `misc()` / `put_cstr` local |
| allocateur de régions | `runtime/guest_region` | un `RegionAlloc` local |
| contexte de thread courant | `runtime/guest_thread_ctx` | l'accès au TIB et à la dernière erreur |
| atomiques invitées | `runtime/guest_atomics` | les `Interlocked*` et leur contrat SMC |
| objets noyau + table de handles | `runtime/guest_sync` | `KEvent`/`KSemaphore`/`KThread`/`KCrit`, `g_handles` |
| couche socket + observateur + route | `runtime/win32_shims_wsock32` | la pile réseau et son instrumentation |
| couture graphique | `render/render.h`, `render/render_null.cpp` | le dos-d'âne vers le GPU et le backend de comptage |
| mise à l'échelle de présentation | `platform/present_scale` | le `scale+flip` et le cadrage proportionnel |
| puits audio | `runtime/audio_sink`, `platform/vita_audio` | le trio null / WAV / console |

### Ce qu'un portage garde aujourd'hui

Relevé sur le premier portage, dont la liste de shims est
[générée depuis les sources](shims.md) des deux côtés :

| | Clés | Part |
|---|---:|---:|
| servies par le moteur | 297 | 46 % |
| servies par le portage | 346 | 54 % |

La répartition par DLL dit où se trouve le travail restant :

| DLL | total | moteur | portage |
|---|---:|---:|---:|
| `KERNEL32.dll` | 252 | 76 | 176 |
| `USER32.dll` | 92 | 56 | 36 |
| `ADVAPI32.dll` | 67 | 37 | 30 |
| `GDI32.dll` | 41 | 36 | 5 |
| `WSOCK32.dll` / `WS2_32.dll` | 58 | 54 | 4 |

Les DLL absentes de cette table (`glide3x`, `native.hook`, `CRYPT32`, et les
DLL propres au jeu) sont **entièrement** côté portage, et c'est normal : elles
portent l'API du jeu, pas celle de Windows.

## Le cas concret : le second portage

Le second portage de cette base — un jeu 3D, là où le premier rend en 2D
palettisée — descend du même socle : son historique montre des commits qui
*retirent* le premier jeu plutôt que d'écrire un runtime neuf. C'est ce qui rend
la comparaison si informative : **quelqu'un a déjà fait le tri « qu'est-ce qui
est propre au premier jeu ? » et l'a enlevé. Ce qui a survécu des deux côtés
n'est pas du jeu.**

### Ce qu'il duplique

| fichier | lignes | inscriptions |
|---|---:|---:|
| `src/win32/shims_misc.cpp` | 1 471 | 252 |
| `src/win32/shims_kernel32.cpp` | 1 549 | 176 |
| `src/win32/shims_user32.cpp` | 730 | 94 |
| `src/win32/shims_gdi32.cpp` | 468 | 37 |
| `src/win32/shims_audio.cpp` | 384 | 10 |
| `src/win32/shims_carnivores.cpp` | 146 | 7 |

Soit environ **576 inscriptions**, face aux 297 que le moteur fournit.

Plus, hors shims : son propre `RegionAlloc` (`src/win32/rt_internals.h`), son
propre `misc()` et `put_cstr` (`tools/rt_boot.cpp`), ses propres objets noyau et
sa propre table de handles, son propre `do_scale_and_flip`, son propre backend
de comptage, sa propre couche de puits audio.

### À quel point c'est la même chose

Mesuré, en normalisant espaces et commentaires :

| objet | verdict |
|---|---|
| `KEvent` ↔ `WxEvent` | **identique** (245 caractères de part et d'autre) |
| `KSemaphore` ↔ `WxSemaphore` | **identique** (219 caractères) |
| `KThread` ↔ `WxThread` | **identique** (259 caractères) |
| `KCrit` ↔ `WxCrit` | identique **au style de déclaration près** (`uint32_t va=0; uint32_t owner=0;` contre `uint32_t va=0, owner=0;`) |
| `KMultiWait` ↔ `WxMultiWait` | **identique à la ligne près** (28 lignes de part et d'autre) |
| `KIocp` ↔ `WxIocp` | **identique à la ligne près** (16 lignes) |

Six objets noyau, dont cinq rigoureusement identiques et le sixième séparé par
un point-virgule. Ce n'est pas une convergence heureuse : c'est le même code,
recopié puis laissé diverger cosmétiquement.

!!! danger "Citer une preuve ne remplace pas livrer l'objet"
    Le moteur n'a longtemps fourni que **4** de ces 6 types — alors que
    l'en-tête de `guest_sync.h` citait déjà l'invariant d'atomicité de
    `WxMultiWait` comme *preuve* que ces structures sont génériques. La preuve
    était là, l'objet non, et les deux portages continuaient de l'écrire.

    Défaut trouvé en migrant le second portage, corrigé depuis. C'est
    exactement ce que le principe directeur annonce : **ce qu'un second
    consommateur ne peut pas reprendre désigne un défaut du moteur.**

### Ce qui est déjà fait

La première vague a eu lieu, et elle valide la méthode.

Le moteur avait dédoublonné KERNEL32 **contre** le second portage, sur un
critère à deux conditions : corps identique hors commentaires et espaces, **et**
aucune dépendance extérieure (pas d'état partagé, pas d'ordonnanceur, pas
d'horloge). 46 shims satisfaisaient les deux. Le second portage a ensuite
re-vérifié ces 46 un par un chez lui, puis les a retirés au profit de
`win32_shims_kernel32_install(br)`.

Le détail d'ordonnancement mérite d'être copié :

```cpp
win32_shims_kernel32_install(br);   // d'abord le moteur
register_kernel32(br);              // puis les siens, qui gagnent
```

Les ~15 shims KERNEL32 que le moteur fournit **en plus** (les `Interlocked*`,
`GetLastError`/`SetLastError`) dépendent d'un point d'extension — TIB courant et
ordonnanceur — que ce portage ne branche pas encore. Ils restent donc chez lui,
et comme sa propre inscription passe **après**, elle les écrase sans effet de
bord. La règle « la dernière inscription gagne » n'est pas seulement un piège :
utilisée délibérément, c'est le mécanisme qui rend une migration progressive
sûre.

## La règle de découpe : trois catégories, pas deux

C'est le point où presque tout le monde se trompe, et l'erreur coûte cher parce
qu'elle laisse du code générique du mauvais côté pour toujours.

| catégorie | critère | destination |
|---|---|---|
| **générique** | aucun littéral, branche ou dépendance d'état propre à un jeu | le **moteur** |
| **propre au jeu** | un littéral en dur, une dépendance à l'état du jeu, un contournement, une adresse de son binaire | le **portage** |
| **propre à la console** | dépend du matériel cible, pas du jeu | le **moteur** |

!!! warning "La troisième catégorie est celle qu'on classe de travers"
    Le moteur cible la PS Vita. Donc le GPU de la console, son audio, ses cœurs,
    son horloge monotone, son journal de démarrage, la lecture de son pad :
    **tout cela appartient au moteur**, même si ça ne ressemble pas à du Win32.

    Rangé dans « propre au jeu » parce que ça ne ressemble pas à de l'émulation,
    ce code reste côté portage et chaque nouveau portage le réécrit.

Le contre-exemple qui fixe la frontière, tiré des entrées :

- la table qui associe *ce bouton* à *cette action du jeu* (une touche, un clic,
  un raccourci d'inventaire) est **propre au jeu** — ces codes ne veulent rien
  dire pour un autre titre, et les deux portages ont bien deux tables
  différentes ;
- la **lecture** du pad, le curseur virtuel, l'orbite, la sensibilité, la zone
  morte, les seuils d'appui bref sont **propres à la console** : les deux
  portages en ont besoin à l'identique.

La bonne découpe est donc : le moteur lit le pad et gère le curseur, le portage
fournit la table de correspondance.

## L'ordre de migration, par vagues

Du plus sûr au plus risqué. Chaque vague dit ce qu'elle remplace, ce qu'elle
**prouve**, et comment on la valide.

### Vague 1 — les shims sans dépendance *(faite sur le second portage)*

**Remplace** : les shims dont le corps est identique et qui ne dépendent
d'aucun symbole extérieur.
**Prouve** : que le critère à deux conditions est opérationnel, et que
l'ordonnancement « moteur d'abord, portage ensuite » est sûr.
**Valide** : recompte des inscriptions, build des deux cibles, un run réel du
jeu sous émulation ARM.

Commencer ici n'est pas une précaution excessive : c'est la vague qui installe
le mécanisme. Tout le reste en dépend.

### Vague 2 — les allocateurs

**Remplace** : `misc()` / `put_cstr` par `runtime/guest_scratch`, le
`RegionAlloc` local par `runtime/guest_region`.
**Prouve** : que le moteur sait posséder de l'**état** du portage, pas seulement
des fonctions pures. C'est le premier vrai transfert de propriété.
**Valide** : aucun jumeau ne doit rester (voir le piège n°1) ; l'épuisement doit
être rapporté par le portage via le rappel que le moteur expose.

Le moteur possède l'allocateur ; le portage lui concède une plage, parce que le
plan mémoire, lui, n'a rien d'universel. Noter que le moteur a **généralisé**
plutôt que recopié : la classe `GuestRegion` est l'algorithme du portage avec le
rapport d'échec transformé en rappel. Ce n'est donc pas un `#include` à la place
d'une définition, c'est une petite adaptation d'appelants.

### Vague 3 — les objets noyau et la table de handles

**Remplace** : `KEvent`/`KSemaphore`/`KThread`/`KCrit`, `g_handles`, le compteur
d'identifiants, la carte des sections critiques, par `runtime/guest_sync`.
**Prouve** : que la sémantique Win32 la plus subtile — celle qui a coûté un
interblocage réel à mettre au point — est réellement partagée.
**Valide** : **obligatoirement sous charge réelle**, pas au démarrage. Une
régression de synchronisation est une famine ou un interblocage, et un
démarrage vert ne prouve rien.

Un bon témoin *positif* : en fin de passe, des fils doivent être **bloqués sur
des objets noyau**. Un fil ne peut se bloquer que si la recherche de son handle
a réussi — quand un handle est inconnu, le code rend la main immédiatement. Des
fils bloqués prouvent donc que la table résout correctement, là où « aucune
erreur » ne prouve rien.

Le portage garde son instrumentation : il l'enregistre sur l'observateur
(`wx86_sync_set_observer`), un seul point, et dispatche en interne. Le moteur
raconte, il ne demande jamais d'avis.

!!! warning "Vérifier d'abord que le jeu utilise réellement ces objets"
    Sur le second portage, cette vague a été faite et **l'oracle du dépôt ne
    la voit pas** — non par défaut de l'oracle, mais parce que le jeu ne prend
    jamais ce chemin : son renderer n'importe **aucune** fonction de
    synchronisation, et son exécutable principal une seule
    (`WaitForSingleObject`).

    Prouvé en trois temps, pas supposé : une faute injectée dans
    `wx86_handle_find` ne change rien au verdict ; des sondes sur
    `handle_add`/`handle_find`/`crit_for` ne tirent aucune fois ; et leur
    présence dans le binaire est vérifiée par `strings`, donc ce silence n'est
    pas celui d'une sonde absente.

    Conséquence pratique : pour un tel portage, ces structures sont du **poids
    mort hérité** et non un besoin vivant. La migration reste bonne (elle
    supprime une duplication et aligne sur du code validé ailleurs), mais elle
    doit être annoncée comme *non validée par ce portage* — et la charge
    réelle demandée ci-dessus est à obtenir auprès d'un portage qui exerce
    vraiment la synchronisation. Ne pas maquiller un chemin non exercé en
    validation verte.

### Vague 4 — la présentation et la couture graphique

**Remplace** : le `scale+flip` local par `platform/present_scale`
(`scale_blit` + `fit_rect`), le backend de comptage local par
`render/render_null.cpp`, les types de sommet et de lot par ceux de
`render/render.h`.
**Prouve** : que la couche la plus sensible aux performances tient sans coût.
**Valide** : identité d'image **sur matériel**, et coût mesuré.

!!! danger "Le piège de cette vague"
    Le fichier de présentation n'est souvent compilé **que sur la console** — ni
    sur bureau, ni sous émulation. L'oracle d'identité d'image habituel ne peut
    donc pas l'exercer, et c'est exactement pour ça que ce code n'est jamais
    touché.

    La parade employée : un auto-contrôle qui embarque une transcription fidèle
    de la boucle d'origine et compare les sorties **octet pour octet**,
    exécutable sur bureau (`tools/present_scale_selftest.cpp`). Puis on prouve
    que l'oracle **coupe**, en injectant une faute d'un seul bit.

    Mesure de référence sur console : image identique sur 400 images, coût
    **+0,12 %** — dans le bruit.

Le second portage n'a **pas** un remplacement direct ici : son backend de
comptage est un bouchon vide (35 lignes) dans son propre espace de noms, avec
ses propres types, là où celui du moteur compte réellement (82 lignes). La
migration est une adaptation de types, pas une suppression. Son redimensionneur,
en revanche, est le **sur-ensemble** dont le moteur a tiré `fit_rect` : son
cadrage proportionnel est déjà ce que la fonction générique implémente.

### Vague 5 — les points d'extension à concevoir

Tout le reste. Et il faut le dire nettement : **le filon mécanique s'épuise.**

Sur le premier portage, après les vagues précédentes, en appliquant le critère à
deux conditions il ne restait plus que **8 fonctions déplaçables en l'état** —
et l'hypothèse prometteuse (« si on déplaçait aussi les deux aides évidentes,
beaucoup se débloqueraient ») a été testée et **réfutée** : zéro de plus.

Les fonctions restantes ne sont pas des déplacements en attente. Ce sont :

- des **transferts de propriété d'états entiers** (table des fichiers, plan
  mémoire, profilage d'attente), chacun étant une vague comme la vague 2 ;
- des fonctions qui **divergent réellement** entre les deux portages, donc
  chacune un point d'extension à concevoir — donnée ou rappel fourni par le
  consommateur, jamais deux implémentations dans le moteur.

C'est une phase de conception, pas de déménagement, et elle se mène une
fonction à la fois.

## Les pièges

Chacun vient d'un incident réel, et c'est pour ça qu'ils valent plus qu'une
recommandation abstraite.

### 1. Déplacer la propriété, jamais le type seul

**Trois accidents en deux jours, tous le même.** Le *type* part au moteur, la
*table* reste au portage, et le moteur en reçoit une seconde, vide : carte des
sections critiques, table de handles, compteur d'identifiants. Le shim opère
alors sur une table, le cœur partagé sur l'autre. **Invisible à la compilation,
payable en interblocage.**

Un cas était encore plus discret : le compteur d'identifiants n'est pas utilisé
que par les objets attendables — un instantané de processus n'est pas attendable
mais consomme quand même un numéro. Sans compteur commun, les deux séries se
recouvrent et deux objets distincts portent le même numéro.

**Le contrôle** : après chaque vague, vérifier explicitement qu'aucun jumeau ne
subsiste côté portage — zéro définition locale du type, zéro table locale, zéro
compteur local. C'est un `grep` qui se fait et qui s'écrit dans le rapport, pas
une intention.

### 2. Une clé, une inscription

`register_shim` écrase en silence : la dernière gagne. C'est la fonctionnalité
qui rend la migration progressive possible (§ vague 1), et c'est le piège
classique quand on déplace des blocs. Un doublon de ce genre a déjà cassé la
connexion réseau d'un portage sans que rien ne le signale.

**Et quand les deux corps d'un doublon diffèrent** : garder le **gagnant**,
c'est-à-dire celui qui était déjà effectif — même s'il semble moins bon. C'est
le seul que l'exécution et les essais aient jamais validé ; l'autre n'a jamais
tourné. Sur un vrai retrait de doublons, deux clés sur cinq avaient des corps
différents et le corps **mort** paraissait le meilleur (il portait une garde de
pointeur nul absente du vivant). Substituer au passage transformerait un retrait
à comportement nul en changement de comportement non validé, déguisé en
nettoyage. Si le corps mort est réellement meilleur : noter le défaut sur place,
et traiter l'amélioration dans un commit séparé qui s'annonce comme tel.

`tools/shim_seq.py` / `.sh` existent pour ça — et ils ont eux-mêmes un angle
mort : une **unité non déclarée** disparaît du relevé en silence. Un décompte
qui baisse exactement du volume de votre vague est plus probablement l'outil que
votre code.

### 3. « Le jeu n'appelle jamais » n'est pas une preuve d'inutilité

Ce motif est tombé **deux fois**. Des fonctions réseau laissées en bouchon au
motif que le client n'écoute jamais se sont révélées parfaitement
implémentables, et le sont désormais pour de vrai. Un bouchon justifié par les
besoins du *premier* jeu devient un trou pour le second.

Avant de recopier un bouchon dans le moteur, se demander si une vraie
implémentation générique coûte réellement plus cher.

### 4. Un oracle doit observer la SORTIE du code testé

L'oracle d'identité d'image d'un projet peut très bien empreindre l'**entrée** de
la fonction qu'on réécrit — auquel cas il signera n'importe quel bug, écran noir
compris. C'était le réflexe évident lors de la vague 4, et il a failli être
suivi.

Trois contrôles avant de croire un verdict vert : situer le point
d'échantillonnage par rapport au code testé ; **prouver que l'oracle coupe avec
le même binaire** (injecter une faute, vérifier qu'il la voit, la retirer) ;
publier le nombre de comparaisons effectuées.

### 5. Un silence ne prouve rien

**Trois conclusions négatives tirées d'un silence se sont révélées fausses en
deux jours.** Un message absent d'un journal peut signifier « le code ne s'arme
pas »… ou « le journal commence trop tard », ou « le banc redirige la sortie
dans un autre fichier ».

Avant de conclure qu'une chose ne se produit pas : produire un **témoin
positif** dans le fichier qu'on interroge. Si le témoin n'apparaît pas non plus,
c'est l'observation qui est en défaut, pas le code.

### 6. Assez de passes de mesure

Sur la vague 4, à **4 passes par jambe** la mesure donnait **−1,42 %** avec un
écart apparemment significatif, et une régression allait être rapportée. Les
deux passes suivantes ont **inversé le signe** : le résultat final est
**+0,12 %**.

S'arrêter à 4 aurait publié une régression inexistante **et** bloqué un
changement gratuit. Grouper les passes, ou prendre une pente — jamais conclure
sur un écart plus petit que sa propre dispersion.

### 7. Prouver la référence AVANT de s'en servir

Une régression a été diagnostiquée puis bissectée sur quatre commits avant
qu'un contrôle de reproductibilité ne montre que **la référence elle-même
n'était pas reproductible** : le dossier d'écriture réutilisé contenait un
fichier de profil de cinq jours plus tôt, absent des runs suivants.

Il n'y avait aucune régression. **Rejouer la référence deux fois et comparer
les empreintes avant de comparer quoi que ce soit d'autre** — c'est le contrôle
le moins cher du lot, et celui dont l'absence coûte le plus.

### 8. Un contrôle négatif peut porter sur une faute que le jeu ne voit pas

Pour prouver qu'un oracle coupe, une faute a été injectée dans un allocateur :
décaler chaque bloc de 16 octets. **Verdict et compteurs inchangés.** L'oracle
n'était pas aveugle — la faute était réellement bénigne à cette échelle (les
libérations échouaient en silence, ce qui fuit sans rien casser).

Un contrôle négatif posé là aurait « prouvé » que l'oracle marche alors qu'il
ne démontrait rien. Si ta faute ne bouge rien, **cherche-en une autre** avant
de conclure quoi que ce soit — dans un sens comme dans l'autre.

## Ce qui n'est pas encore dans le moteur

Un guide honnête sur ses manques est utilisable ; un guide qui promet trop fait
perdre une journée à celui qui le suit.

| manquant | où c'est aujourd'hui | pourquoi |
|---|---|---|
| ~~l'émulation DirectSound~~ | **au moteur** (`runtime/ds_emul`) | livrée depuis. Un portage dont le jeu passe par une autre bibliothèque audio ne la consommera pas, mais le **puits** (null / WAV / console) et l'horloge hôte, eux, sont communs. |
| le backend GPU de la console | côté portage | volontairement borné aux états que le premier jeu émet. La couture générique existe (`render/render.h`), le backend qui la réalise, non. |
| la lecture du pad et le curseur | côté portage | catégorie « propre à la console » identifiée mais pas encore déplacée. Les valeurs par défaut (orbite, sensibilité, zone morte) sont **identiques dans deux jeux sans rapport** : elles tiennent à l'ergonomie du stick, pas au jeu. Travail en cours. |
| ~176 fonctions `KERNEL32` | côté portage | fichiers et chemins, plan mémoire, chronométrage d'attente, horloge — voir vague 5. |
| l'ordonnancement | les **deux** existent | le moteur fournit coopératif **et** natif ; un portage choisit. Le premier portage ne cible plus que le natif, le second démarre encore en coopératif. Ce n'est pas une dette, c'est un choix par consommateur. |

### Une asymétrie à connaître avant de se lancer

Le moteur fournit **2** inscriptions pour `DDRAW.dll` — de quoi répondre « pas
de DirectDraw » proprement. Le second portage en inscrit **128** : une émulation
COM complète, avec les vtables de `IDirectDraw` et de ses interfaces.

Ce n'est pas un simple manque de couverture. C'est le moteur **façonné par les
besoins du premier consommateur** : le premier jeu ne dessine pas via
DirectDraw, donc deux fonctions suffisaient, donc la question ne s'est jamais
posée. Personne ne l'avait vu, parce que la mesure de la frontière compte ce que
le moteur *possède*, jamais ce qu'un second consommateur *aurait besoin* qu'il
possède.

C'est précisément le défaut qu'un moteur générique doit éviter, et c'est
exactement le genre de chose que seule une migration réelle révèle. À prendre
comme une bonne nouvelle : trouvé maintenant, il coûte une vague ; trouvé au
troisième portage, il coûte une refonte.

De même, la couche audio du second portage est écrite en **C** là où celle du
moteur est en C++, avec la même forme pourtant (le trio null / WAV / console,
les mêmes `write` et `close`). Reprendre l'une pour l'autre est une adaptation,
pas une inclusion.

## Résumé opérationnel

1. Comparer les deux bases fonction par fonction, sur le **corps** normalisé —
   pas sur le nom.
2. Ne déplacer d'abord que ce qui satisfait **les deux** conditions : corps
   identique **et** aucune dépendance extérieure.
3. Enregistrer le moteur **avant** ses propres shims, pour que les siens
   gagnent, et migrer par domaines.
4. Pour tout ce qui possède de l'état : transférer la **propriété**, vérifier
   qu'aucun jumeau ne reste.
5. Valider chaque vague à la hauteur de sa classe de risque — et pour la
   synchronisation, **sous charge réelle**, jamais au démarrage.
6. Traiter ce qui diverge comme un **point d'extension à concevoir**, pas comme
   un déplacement en retard.
